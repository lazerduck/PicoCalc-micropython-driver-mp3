// audio_out_pwm.c
#include "audio_out_pwm.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include <string.h>

#ifndef count_of
#define count_of(a) (sizeof(a)/sizeof((a)[0]))
#endif

// ===== Config =====
#define AUDIO_PWM_TOP           1023u  // 8-bit PWM
#define AUDIO_BATCH_FRAMES      256u   // DMA burst size (per buffer)
#define AUDIO_NUM_BUFFERS       2u     // ping-pong

// ===== Internal state =====
typedef struct {
    int            pin_l, pin_r;
    int            sample_rate;
    int            channels;
    uint           slice;
    uint           chan_l, chan_r;

    // DMA channels (one per PWM channel)
    int            dma_l;
    int            dma_r;

    // Two ping-pong buffers, per channel, 16-bit PWM levels
    uint16_t       buf_l[AUDIO_NUM_BUFFERS][AUDIO_BATCH_FRAMES];
    uint16_t       buf_r[AUDIO_NUM_BUFFERS][AUDIO_BATCH_FRAMES];

    // Which buffer is currently being played by DMA, and which is being filled
    volatile uint  play_idx;    // 0 or 1
    volatile uint  fill_idx;    // 0 or 1

    // How many frames have been written into the fill buffer so far
    volatile uint  fill_count;

    // Flag set in DMA IRQ when a buffer finishes -> swap needed
    volatile bool  dma_batch_done;

    // Running flag
    bool           started;
} audio_state_t;

static audio_state_t s = {0};

// ===== Small helpers =====
static inline uint16_t pcm16_to_level(int16_t sample) {
    // 16-bit signed -> 0..AUDIO_PWM_TOP
    // Map [-32768..32767] -> [0..255]
    uint32_t u = (uint32_t)(sample + 32768);           // 0..65535
    return (uint16_t)((u * AUDIO_PWM_TOP) / 65535u);
}

static void audio_configure_pwm(int pin_l, int pin_r, int sample_rate) {
    gpio_set_function(pin_l, GPIO_FUNC_PWM);
    gpio_set_function(pin_r, GPIO_FUNC_PWM);

    uint slice_l = pwm_gpio_to_slice_num(pin_l);
    // uint slice_r = pwm_gpio_to_slice_num(pin_r);

    // For GP26/27 this should be the same slice; assert softly
    s.slice = slice_l;

    s.chan_l = pwm_gpio_to_channel(pin_l);
    s.chan_r = pwm_gpio_to_channel(pin_r);

    // Compute clkdiv to make PWM wrap at sample_rate
    // f = clk_sys / (clkdiv * (TOP+1))  => clkdiv = clk_sys / (f*(TOP+1))
    uint32_t clk = clock_get_hz(clk_sys);
    float clkdiv = (float)clk / ((float)sample_rate * (float)(AUDIO_PWM_TOP + 1));
    if (clkdiv < 1.0f) clkdiv = 1.0f;
    if (clkdiv > 255.0f) clkdiv = 255.0f;

    pwm_set_wrap(s.slice, AUDIO_PWM_TOP);
    pwm_set_clkdiv(s.slice, clkdiv);

    // Start disabled; audio_out_start() will enable
    pwm_set_enabled(s.slice, false);

    // Set mid levels (silence)
    pwm_set_chan_level(s.slice, s.chan_l, AUDIO_PWM_TOP/2);
    pwm_set_chan_level(s.slice, s.chan_r, AUDIO_PWM_TOP/2);
}

static inline volatile uint16_t* pwm_cc_addr_low(uint slice) {
    return (volatile uint16_t*)&pwm_hw->slice[slice].cc;         // low half = A
}
static inline volatile uint16_t* pwm_cc_addr_high(uint slice) {
    return ((volatile uint16_t*)&pwm_hw->slice[slice].cc) + 1;   // high half = B
}

// DMA IRQ: fires when LEFT channel finishes moving one batch.
// We keep RIGHT in lockstep; so one IRQ is enough.
static void __isr audio_dma_irq(void) {
    // Clear IRQ for our left channel
    if (dma_hw->ints1 & (1u << s.dma_l)) {
        dma_hw->ints1 = (1u << s.dma_l);
        s.dma_batch_done = true;
    }
}

// Program one DMA channel to stream N halfwords from buf -> PWM level reg (fixed)
static void audio_start_dma_one(int dma_chan, const uint16_t* buf, volatile uint16_t* pwm_cc_half, uint dreq, uint count) {
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dreq);

    dma_channel_configure(
        dma_chan, &c,
        (void*)pwm_cc_half,   // dst
        (const void*)buf,     // src
        count,                // transfers
        false                 // don't start yet
    );
}

// Call with interrupts disabled or from main + our simple flags
static void audio_kick_dma_pair(uint buf_index) {
    // Both channels use the same DREQ: PWM wrap for this slice
    uint dreq = DREQ_PWM_WRAP0 + s.slice; // works on RP2040/RP2350 SDKs

    audio_start_dma_one(s.dma_l, s.buf_l[buf_index], pwm_cc_addr_low(s.slice),  dreq, AUDIO_BATCH_FRAMES);
    audio_start_dma_one(s.dma_r, s.buf_r[buf_index], pwm_cc_addr_high(s.slice), dreq, AUDIO_BATCH_FRAMES);

    // Start both back-to-back
    dma_channel_start(s.dma_l);
    dma_channel_start(s.dma_r);
}

bool audio_out_init(const audio_out_cfg_t* cfg) {
    memset(&s, 0, sizeof(s));
    s.pin_l       = cfg->pin_l;
    s.pin_r       = cfg->pin_r;
    s.sample_rate = cfg->sample_rate;
    s.channels    = cfg->channels <= 1 ? 1 : 2;

    audio_configure_pwm(s.pin_l, s.pin_r, s.sample_rate);

    // Claim two DMA channels
    s.dma_l = dma_claim_unused_channel(true);
    s.dma_r = dma_claim_unused_channel(true);

    // IRQ for left channel completion
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_set_irq1_enabled(s.dma_l, true);

    // Pre-fill both ping-pong buffers with silence
    for (uint b = 0; b < AUDIO_NUM_BUFFERS; ++b) {
        for (uint i = 0; i < AUDIO_BATCH_FRAMES; ++i) {
            s.buf_l[b][i] = AUDIO_PWM_TOP / 2;
            s.buf_r[b][i] = AUDIO_PWM_TOP / 2;
        }
    }
    s.play_idx   = 0;
    s.fill_idx   = 1;
    s.fill_count = 0;
    s.dma_batch_done = false;
    s.started = false;

    return true;
}

void audio_out_start(void) {
    if (s.started) return;

    // Start with buffer 0 playing (silence) while buffer 1 gets filled
    audio_kick_dma_pair(0);

    // Enable PWM slice (starts generating DREQs)
    pwm_set_enabled(s.slice, true);

    s.started = true;
}

void audio_out_stop(void) {
    if (!s.started) return;
    // Abort DMA
    dma_channel_abort(s.dma_l);
    dma_channel_abort(s.dma_r);
    dma_channel_set_irq1_enabled(s.dma_l, false);
    irq_set_enabled(DMA_IRQ_1, false);

    // Mute + disable PWM
    pwm_set_chan_level(s.slice, s.chan_l, AUDIO_PWM_TOP/2);
    pwm_set_chan_level(s.slice, s.chan_r, AUDIO_PWM_TOP/2);
    pwm_set_enabled(s.slice, false);

    s.started = false;
}

// Feed decoded interleaved frames into the "fill" buffer.
// Returns frames accepted (may be < frames if buffer is full).
size_t audio_out_push_interleaved(const int16_t* lr, size_t frames) {
    if (!s.started) return 0; // we expect start() to be called first

    // If the DMA finished a batch, swap play/fill roles and restart DMA.
    if (s.dma_batch_done) {
        s.dma_batch_done = false;

        // The buffer that just finished is now our new fill buffer.
        s.fill_idx = s.play_idx;
        s.fill_count = 0;

        // Flip play buffer and kick DMA on the other one.
        s.play_idx ^= 1u;
        audio_kick_dma_pair(s.play_idx);
    }

    // Fill as much as we can into the current fill buffer.
    size_t can_accept = (AUDIO_BATCH_FRAMES - s.fill_count);
    size_t to_take = frames < can_accept ? frames : can_accept;

    if (to_take) {
        uint16_t* L = s.buf_l[s.fill_idx] + s.fill_count;
        uint16_t* R = s.buf_r[s.fill_idx] + s.fill_count;

        if (s.channels == 2) {
            for (size_t i = 0; i < to_take; ++i) {
                int16_t l = lr[2*i + 0];
                int16_t r = lr[2*i + 1];
                L[i] = pcm16_to_level(l);
                R[i] = pcm16_to_level(r);
            }
        } else {
            for (size_t i = 0; i < to_take; ++i) {
                int16_t m = lr[i];
                uint16_t lv = pcm16_to_level(m);
                L[i] = lv; R[i] = lv;
            }
        }
        s.fill_count += (uint)to_take;
    }

    return to_take;
}
