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
#define AUDIO_PWM_TOP_DEFAULT   1023u  // default; runtime may pick a better TOP
#define AUDIO_BATCH_FRAMES      64u    // ~1.45ms @44.1kHz: smaller handoff window
#define AUDIO_NUM_BUFFERS       2u

// ===== Internal state =====
typedef struct {
    int            pin_l, pin_r;
    int            sample_rate;
    int            channels;
    uint           slice;
    uint           chan_l, chan_r;
    uint16_t       top;            // PWM wrap value chosen to minimize rate error
    uint32_t       actual_rate;    // actual achieved sample rate

    // DMA channels (one per PWM channel)
    int            dma_l;
    int            dma_r;

    // Two ping-pong buffers, per channel, 16-bit PWM levels
    uint16_t       buf_l[AUDIO_NUM_BUFFERS][AUDIO_BATCH_FRAMES];
    uint16_t       buf_r[AUDIO_NUM_BUFFERS][AUDIO_BATCH_FRAMES];

    volatile uint  play_idx;
    volatile uint  fill_idx;
    volatile bool  dma_batch_done;
    bool           started;

    // Provider callback (pull model)
    audio_out_provider_t provider;
    void*         provider_user;

    // Simple underrun counter for diagnostics
    volatile uint32_t underruns;
} audio_state_t;

static audio_state_t s = {0};

// ===== Small helpers =====
static inline uint16_t pcm16_to_level(int16_t sample) {
    // Map signed 16-bit PCM to 0..TOP inclusive using fixed-point scale
    // level ≈ (u * (TOP+1)) >> 16, u in 0..65535
    uint32_t u = (uint32_t)(sample + 32768); // 0..65535
    return (uint16_t)((u * (uint32_t)(s.top + 1)) >> 16);
}

// Find PWM TOP and 8.4 divider that best approximate the desired sample rate
static void choose_pwm_params(int target_hz, uint32_t clk_sys_hz, uint16_t* out_top, uint8_t* out_divi, uint8_t* out_divf) {
    // Limits from RP2040: clkdiv in [1.0 .. 255 + 15/16], TOP up to 65535 (we'll keep modest)
    double best_err = 1e30;
    uint16_t best_top = AUDIO_PWM_TOP_DEFAULT;
    uint8_t best_divi = 0;
    uint8_t best_divf = 0;
    // Search a reasonable TOP range around default for good fits
    // Wider search to find very low error fits
    for (uint16_t top = 256; top <= 8192; top += 1) {
        double ideal_div = (double)clk_sys_hz / ((double)target_hz * (double)(top + 1));
        if (ideal_div < 1.0 || ideal_div > (255.0 + 15.0/16.0)) continue;
        int divi = (int)ideal_div;
        double frac = ideal_div - (double)divi;
        int divf = (int)(frac * 16.0 + 0.5); // round to nearest 1/16
        if (divf == 16) { divi += 1; divf = 0; }
        if (divi < 1 || divi > 255) continue;
        double actual = (double)clk_sys_hz / (((double)divi + (double)divf/16.0) * (double)(top + 1));
        double err = actual > target_hz ? (actual - target_hz) : (target_hz - actual);
        if (err < best_err) {
            best_err = err;
            best_top = top;
            best_divi = (uint8_t)divi;
            best_divf = (uint8_t)divf;
            if (best_err <= 0.1) break; // good enough
        }
    }
    *out_top = best_top;
    *out_divi = best_divi;
    *out_divf = best_divf;
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

    // Choose TOP and 8.4 clkdiv that minimize rate error
    uint32_t clk = clock_get_hz(clk_sys);
    uint16_t top; uint8_t divi; uint8_t divf;
    choose_pwm_params(sample_rate, clk, &top, &divi, &divf);
    s.top = top;
    pwm_set_wrap(s.slice, s.top);
    pwm_set_clkdiv_int_frac(s.slice, divi, divf);
    // Compute actual rate for diagnostics
    s.actual_rate = (uint32_t)((double)clk / (((double)divi + (double)divf/16.0) * (double)(s.top + 1)) + 0.5);

    // Start disabled; audio_out_start() will enable
    pwm_set_enabled(s.slice, false);

    // Set mid levels (silence)
    pwm_set_chan_level(s.slice, s.chan_l, s.top/2);
    pwm_set_chan_level(s.slice, s.chan_r, s.top/2);
}

static inline volatile uint16_t* pwm_cc_addr_low(uint slice) {
    return (volatile uint16_t*)&pwm_hw->slice[slice].cc;         // low half = A
}
static inline volatile uint16_t* pwm_cc_addr_high(uint slice) {
    return ((volatile uint16_t*)&pwm_hw->slice[slice].cc) + 1;   // high half = B
}

// Forward declaration (defined later)
static void audio_kick_dma_pair(uint buf_index);

// DMA IRQ: fires when LEFT channel finishes moving one batch.
// We keep RIGHT in lockstep; so one IRQ is enough.
static void audio_fill_buffer(uint buf_index) {
    if (!s.provider) {
        // fill silence
    for (size_t i = 0; i < AUDIO_BATCH_FRAMES; i++){
            s.buf_l[buf_index][i] = s.top/2;
            s.buf_r[buf_index][i] = s.top/2;
        }
        return;
    }
    // Temporary small stack buffer to ask provider for frames then convert
    int16_t tmp[2 * AUDIO_BATCH_FRAMES];
    size_t got = s.provider(tmp, AUDIO_BATCH_FRAMES, s.provider_user);
    if (got < AUDIO_BATCH_FRAMES) {
        s.underruns++;
    }
    // Convert frames we have; if underrun, repeat last sample for remainder
    int16_t last_l = 0, last_r = 0;
    if (got) { last_l = tmp[0]; last_r = tmp[1]; }
    for (size_t i = 0; i < AUDIO_BATCH_FRAMES; ++i) {
        int16_t l, r;
        if (i < got) {
            if (s.channels == 2) { l = tmp[2*i]; r = tmp[2*i+1]; }
            else { l = r = tmp[i]; }
            last_l = l; last_r = r;
        } else {
            l = last_l; r = last_r; // stretch last sample
        }
        s.buf_l[buf_index][i] = pcm16_to_level(l);
        s.buf_r[buf_index][i] = pcm16_to_level(r);
    }
}

static void __isr audio_dma_irq(void) {
    if (dma_hw->ints1 & (1u << s.dma_l)) {
        dma_hw->ints1 = (1u << s.dma_l);
        // Buffer just consumed:
        uint finished = s.play_idx;
        // Next buffer already prepared:
        uint next = finished ^ 1u;
        // Start DMA on next buffer first (minimize gap)
        audio_kick_dma_pair(next);
        s.play_idx = next;
        // Refill the freed buffer for future cycle
        audio_fill_buffer(finished);
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
    // Make DMA IRQ high priority so it preempts other work (like decode timer)
    irq_set_priority(DMA_IRQ_1, 0x40);
    irq_set_enabled(DMA_IRQ_1, true);
    dma_channel_set_irq1_enabled(s.dma_l, true);

    for (uint b = 0; b < AUDIO_NUM_BUFFERS; ++b) {
        for (uint i = 0; i < AUDIO_BATCH_FRAMES; ++i) {
            s.buf_l[b][i] = s.top/2;
            s.buf_r[b][i] = s.top/2;
        }
    }
    s.play_idx = 0;
    s.fill_idx = 1;
    s.dma_batch_done = false;
    s.started = false;
    s.provider = NULL;
    s.provider_user = NULL;
    s.underruns = 0;

    return true;
}

void audio_out_set_provider(audio_out_provider_t cb, void* user) {
    s.provider = cb;
    s.provider_user = user;
}

void audio_out_start(void) {
    if (s.started) return;
    // Prefill both buffers before start
    audio_fill_buffer(0);
    audio_fill_buffer(1);
    s.play_idx = 0;
    audio_kick_dma_pair(s.play_idx);
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
    pwm_set_chan_level(s.slice, s.chan_l, s.top/2);
    pwm_set_chan_level(s.slice, s.chan_r, s.top/2);
    pwm_set_enabled(s.slice, false);

    s.started = false;

    // Release DMA channels to avoid leaking across sessions
    if (s.dma_l >= 0) { dma_channel_unclaim(s.dma_l); s.dma_l = -1; }
    if (s.dma_r >= 0) { dma_channel_unclaim(s.dma_r); s.dma_r = -1; }
}

uint32_t audio_out_underruns(void){ return s.underruns; }

uint32_t audio_out_actual_rate(void){ return s.actual_rate; }

// Feed decoded interleaved frames into the "fill" buffer.
// Returns frames accepted (may be < frames if buffer is full).
// Push/free API removed in pull model.