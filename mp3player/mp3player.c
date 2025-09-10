#include "py/obj.h"
#include "py/runtime.h"
#include <string.h>
#include <stdlib.h>
#include "py/misc.h"

#include "mp3_decode.h"
#include "ring_buffer.h"
#include "audio_out_pwm.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"


typedef enum { S_IDLE=0, S_LOADED, S_PLAYING, S_EOF } mp3_state_t;

// GLOBAL STATE (replace your existing `static struct { ... } g = {0};` with this)
static struct {
    mp3_state_t     state;
    audio_out_cfg_t outcfg;
    ring_buffer_t   rb;            // PCM bytes
    size_t          frame_bytes;   // channels * 2
    size_t          target_bytes;  // ring capacity target (e.g., ~150ms)

    mp3_decoder_t*  dec;

    char*           path;
    size_t          path_len;

    // Reusable decode buffer to avoid malloc/free in play()/poll()
    int16_t*        scratch;        // one MP3 frame worth (max 1152 samples * channels)
    size_t          scratch_frames;
    size_t          scratch_elems;
    bool            eof;
    repeating_timer_t decode_timer;
    // Test tone
    bool            tone_mode;
    uint32_t        tone_phase;
    uint32_t        tone_step;
    volatile bool   service_pending;
    // Output sample-rate scaling in Q16 (65536 = 100%). Set via mp3.set_rate(percent).
    uint32_t        rate_scale_q16;
} g = {0};

static mp_obj_t mp3_init(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args){
    enum { ARG_pin_l, ARG_pin_r, ARG_buffer_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_pin_l,     MP_ARG_INT, {.u_int = 26} },
        { MP_QSTR_pin_r,     MP_ARG_INT, {.u_int = 27} },
        { MP_QSTR_buffer_ms, MP_ARG_INT, {.u_int = 150} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, a);
    memset(&g, 0, sizeof(g));
    g.outcfg.pin_l = a[ARG_pin_l].u_int;
    g.outcfg.pin_r = a[ARG_pin_r].u_int;
    // ring buffer created after we know sample_rate/channels at load()
    g.state = S_IDLE;
    g.tone_mode = false;
    g.rate_scale_q16 = 65536; // 100%
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mp3_init_obj, 0, mp3_init);


static mp_obj_t mp3_load(mp_obj_t path_in){
    if (g.state != S_IDLE && g.state != S_EOF) {
        mp_raise_ValueError(MP_ERROR_TEXT("stop first"));
    }

    const char* path = mp_obj_str_get_str(path_in);
    

    if (g.path) { m_del(char, g.path, g.path_len + 1); g.path = NULL; g.path_len = 0; }
    size_t len = strlen(path);
    g.path = m_new(char, len + 1);
    memcpy(g.path, path, len + 1);  // includes the null terminator
    g.path_len = len;

    if (!g.dec) g.dec = mp3_decoder_create();

    mp3_stream_info_t info;
    if (!mp3_decoder_open(g.dec, g.path, &info)) {
        mp_raise_ValueError(MP_ERROR_TEXT("open/decode failed"));
    }

    g.outcfg.sample_rate = info.sample_rate;
    g.outcfg.channels    = (info.channels < 1) ? 1 : ((info.channels > 2) ? 2 : info.channels);
    g.frame_bytes        = (size_t)g.outcfg.channels * 2;

    // Ring buffer ~48KB (good headroom; ~225ms @ 44.1k stereo)
    if (g.rb.data) rb_free(&g.rb);
    const size_t bytes = 48 * 1024;
    if (!rb_init(&g.rb, bytes)) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("rb init"));
    }
    g.target_bytes = bytes * 9 / 10; // high-water 90%

    g.state = S_LOADED;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3_load_obj, mp3_load);


// Provider callback for audio_out (pull model)
static size_t provider_cb(int16_t* dst, size_t max_frames, void* user) {
    (void)user;
    if (g.tone_mode) {
        for (size_t i=0;i<max_frames;i++) {
            uint32_t ph = g.tone_phase;
            g.tone_phase += g.tone_step;
            int32_t saw = (int32_t)(ph >> 16) - 32768; // -32768..32767
            // crude shaping to soften saw (optional)
            int32_t shaped = saw - (saw*saw/32768)/4;
            int16_t sample = (int16_t)shaped;
            if (g.outcfg.channels == 2) {
                dst[2*i] = sample;
                dst[2*i+1] = sample;
            } else {
                dst[i] = sample;
            }
        }
        return max_frames;
    }
    // Need interleaved frames from ring buffer; ring stores raw PCM int16 interleaved.
    size_t want_bytes = max_frames * g.frame_bytes;
    size_t have_bytes = rb_used_space(&g.rb);
    if (have_bytes == 0) return 0;
    if (want_bytes > have_bytes) want_bytes = have_bytes;
    size_t frames = want_bytes / g.frame_bytes;
    rb_read(&g.rb, dst, frames * g.frame_bytes);
    return frames;
}

// Forward declaration of scheduled service (defined later)
extern const mp_obj_fun_builtin_fixed_t mp3_service_obj;

// Decode timer callback: schedule VM-context refill when below target
static bool decode_timer_cb(repeating_timer_t* rt) {
    (void)rt;
    if (g.state != S_PLAYING || g.eof) return true;
    size_t used = rb_used_space(&g.rb);
    if (used < g.target_bytes && !g.service_pending) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&mp3_service_obj), mp_const_none);
        g.service_pending = true;
    }
    return true;
}

static mp_obj_t mp3_play(void){
    mp_printf(&mp_plat_print, "play:A enter\n");
    if (g.state != S_LOADED && g.state != S_EOF) {
        mp_raise_ValueError(MP_ERROR_TEXT("load first"));
    }
    mp_printf(&mp_plat_print, "play:B audio_out_init\n");
    // Apply optional user rate scaling before audio init
    if (g.rate_scale_q16 != 65536) {
        int64_t sr = (int64_t)g.outcfg.sample_rate * (int64_t)g.rate_scale_q16;
        sr = (sr + 32768) >> 16;
        if (sr < 8000) sr = 8000;
        if (sr > 192000) sr = 192000;
        g.outcfg.sample_rate = (int)sr;
    }
    if (!audio_out_init(&g.outcfg)) {
        mp_raise_ValueError(MP_ERROR_TEXT("audio init failed"));
    }
    rb_clear(&g.rb);

    // Allocate reusable scratch buffer once
    if (g.scratch) { m_del(int16_t, g.scratch, g.scratch_elems); g.scratch = NULL; }
    // Scratch sized to one MP3 frame (1152) to reduce RAM; we'll loop decode as needed
    g.scratch_frames = 1152;
    size_t chans = (g.outcfg.channels < 1) ? 1 : ((g.outcfg.channels > 2) ? 2 : g.outcfg.channels);
    g.scratch_elems  = g.scratch_frames * chans;
    mp_printf(&mp_plat_print, "play:C alloc scratch %u elems\n", (unsigned)g.scratch_elems);
    g.scratch        = m_new(int16_t, g.scratch_elems);
    g.eof = false;

    // Initial predecode: fill ring up to target_bytes/2 or until timeout
    int zero_runs = 0;
    const int ZERO_SCAN_LIMIT = 1024; // keep pre-start scanning short; proceed and let timer advance
    absolute_time_t start = get_absolute_time();
    while (rb_used_space(&g.rb) < g.target_bytes / 2) {
        int got = mp3_decoder_decode(g.dec, g.scratch, g.scratch_frames);
        if (got < 0) { g.eof = true; break; }
        if (got == 0) {
            if (mp3_decoder_is_eof(g.dec)) { g.eof = true; break; }
            if (++zero_runs * 1 > ZERO_SCAN_LIMIT) { // each zero typically skips ~1 byte
                break; // let timer continue scanning incrementally
            }
            if (absolute_time_diff_us(start, get_absolute_time()) > 150000) { // 150ms patience
                break; // start playback anyway
            }
            continue; // keep scanning for sync
        }
        zero_runs = 0;
        size_t bytes = (size_t)got * g.frame_bytes;
        if (rb_free_space(&g.rb) < bytes) break;
        rb_write(&g.rb, g.scratch, bytes);
    }

    audio_out_set_provider(provider_cb, NULL);
    // Delay actual start until we have at least a small cushion, else start immediately if EOF or timeout
    if (rb_used_space(&g.rb) < g.frame_bytes * 256 && !g.eof) {
        // Fill a minimal cushion synchronously (burst attempts)
        int more_attempts = 0;
        while (rb_used_space(&g.rb) < g.frame_bytes * 256 && more_attempts < 256 && !g.eof) {
            int got = mp3_decoder_decode(g.dec, g.scratch, g.scratch_frames);
            if (got <= 0) {
                if (mp3_decoder_is_eof(g.dec)) { g.eof = true; break; }
                more_attempts++;
                continue;
            }
            size_t bytes = (size_t)got * g.frame_bytes;
            if (rb_free_space(&g.rb) < bytes) break;
            rb_write(&g.rb, g.scratch, bytes);
            // don't count successful attempt toward more_attempts to allow burst fill
        }
    }
    audio_out_start();

    // Prioritize DMA IRQ so audio buffer swaps are never delayed by decoding work
    irq_set_priority(DMA_IRQ_1, 0x80);

    // Start decode maintenance timer every 3ms to further avoid periodic alignment
    add_repeating_timer_ms(-3, decode_timer_cb, NULL, &g.decode_timer);
    mp_printf(&mp_plat_print, "play:D started pull-model\n");

    g.state = S_PLAYING;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_play_obj, mp3_play);

// Poll becomes no-op (kept for compatibility)
static mp_obj_t mp3_poll(void){ return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_poll_obj, mp3_poll);


static mp_obj_t mp3_stop(void){
    audio_out_stop();
    cancel_repeating_timer(&g.decode_timer);
    if (g.scratch) { m_del(int16_t, g.scratch, g.scratch_elems); g.scratch = NULL; }
    if (g.rb.data) rb_free(&g.rb);
    g.service_pending = false;

    // Close decoder file handle to free VFS resources
    if (g.dec) {
        mp3_decoder_destroy(g.dec);
        g.dec = NULL; // will be re-created on next load
    }

    // Keep decoder so you can play again without re-create.
    // If you want to fully reset path too, uncomment:
    // if (g.path) { m_del(char, g.path, g.path_len + 1); g.path = NULL; g.path_len = 0; }

    g.state = S_IDLE;
    g.tone_mode = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_stop_obj, mp3_stop);

static mp_obj_t mp3_state(void){
    const char* s = "idle";
    switch (g.state){
        case S_IDLE:   s="idle"; break;
        case S_LOADED: s="loaded"; break;
    case S_PLAYING:s = g.eof && rb_used_space(&g.rb)==0 ? "eof" : "playing"; break;
    case S_EOF:    s="eof"; break;
    }
    return mp_obj_new_str(s, strlen(s));
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_state_obj, mp3_state);

static mp_obj_t mp3_stats(void){
    mp_obj_t tuple[8];
    tuple[0] = mp_obj_new_int(g.outcfg.sample_rate);
    tuple[1] = mp_obj_new_int(g.outcfg.channels);
    tuple[2] = mp_obj_new_int(rb_used_space(&g.rb));
    tuple[3] = mp_obj_new_int(rb_free_space(&g.rb));
    tuple[4] = mp_obj_new_int(g.target_bytes);
    tuple[5] = mp_obj_new_int(g.eof);
    tuple[6] = mp_obj_new_int(g.state);
    tuple[7] = mp_obj_new_int_from_uint(audio_out_underruns());
    return mp_obj_new_tuple(8, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_stats_obj, mp3_stats);

static mp_obj_t mp3_diag(void){
    uint32_t f=0,z=0; if (g.dec) mp3_decoder_get_diag(g.dec, &f, &z);
    mp_obj_t tuple[2];
    tuple[0] = mp_obj_new_int_from_uint(f);
    tuple[1] = mp_obj_new_int_from_uint(z);
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_diag_obj, mp3_diag);

static mp_obj_t mp3_test_tone(mp_obj_t freq_in){
    mp_int_t freq = mp_obj_get_int(freq_in);
    if (freq < 20) freq = 20;
    if (freq > 12000) freq = 12000;
    if (g.state == S_IDLE) {
        g.outcfg.sample_rate = 44100;
        if (g.rate_scale_q16 != 65536) {
            int64_t sr = (int64_t)g.outcfg.sample_rate * (int64_t)g.rate_scale_q16;
            sr = (sr + 32768) >> 16;
            if (sr < 8000) sr = 8000;
            if (sr > 192000) sr = 192000;
            g.outcfg.sample_rate = (int)sr;
        }
        g.outcfg.channels = 2;
        if (!audio_out_init(&g.outcfg)) mp_raise_ValueError(MP_ERROR_TEXT("audio init failed"));
        audio_out_set_provider(provider_cb, NULL);
        g.state = S_PLAYING;
    }
    g.tone_mode = true;
    g.tone_phase = 0;
    uint64_t step = ((uint64_t)freq << 32) / (uint32_t)g.outcfg.sample_rate;
    g.tone_step = (uint32_t)step;
    audio_out_start();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3_test_tone_obj, mp3_test_tone);

// Set playback speed as a percent (float allowed). 100.0 = normal, 90.0 = 10% slower, 110.0 = 10% faster.
static mp_obj_t mp3_set_rate(mp_obj_t percent_in){
    mp_float_t pct = mp_obj_get_float(percent_in);
    if (pct < 50.0f) pct = 50.0f;        // clamp to sane range
    if (pct > 200.0f) pct = 200.0f;
    // Convert percent to Q16 scale factor
    mp_float_t q = pct * (65536.0f / 100.0f);
    uint32_t q16 = (uint32_t)(q + 0.5f);
    if (q16 == 0) q16 = 1;
    g.rate_scale_q16 = q16;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp3_set_rate_obj, mp3_set_rate);

// Return the actual output sample rate chosen by the PWM fitter
static mp_obj_t mp3_out_rate(void){
    return mp_obj_new_int_from_uint(audio_out_actual_rate());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mp3_out_rate_obj, mp3_out_rate);

// Scheduled service: runs in VM context. Refill ring up to target.
static mp_obj_t mp3_service(mp_obj_t _arg){
    if (g.state != S_PLAYING || g.eof) { g.service_pending = false; return mp_const_none; }
    size_t need_level = g.target_bytes;
    size_t low_level  = g.frame_bytes * 256; // keep a healthy cushion
    int attempts = 0;
    int attempt_cap = (rb_used_space(&g.rb) < low_level) ? 96 : 32;
    while (attempts < attempt_cap && rb_used_space(&g.rb) < need_level) {
        int got = mp3_decoder_decode(g.dec, g.scratch, g.scratch_frames);
        if (got < 0) { g.eof = true; break; }
        if (got == 0) {
            if (mp3_decoder_is_eof(g.dec)) { g.eof = true; break; }
            attempts++;
            continue;
        }
        size_t to_write_bytes = (size_t)got * g.frame_bytes;
        if (rb_free_space(&g.rb) < to_write_bytes) break;
        rb_write(&g.rb, g.scratch, to_write_bytes);
        attempts++;
    }
    g.service_pending = false;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(mp3_service_obj, mp3_service);

// Module table
static const mp_rom_map_elem_t mp3_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mp3) },
    { MP_ROM_QSTR(MP_QSTR_init),     MP_ROM_PTR(&mp3_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_load),     MP_ROM_PTR(&mp3_load_obj) },
    { MP_ROM_QSTR(MP_QSTR_play),     MP_ROM_PTR(&mp3_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll),     MP_ROM_PTR(&mp3_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),     MP_ROM_PTR(&mp3_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_state),    MP_ROM_PTR(&mp3_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats),    MP_ROM_PTR(&mp3_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_diag),     MP_ROM_PTR(&mp3_diag_obj) },
    { MP_ROM_QSTR(MP_QSTR_test_tone), MP_ROM_PTR(&mp3_test_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_rate), MP_ROM_PTR(&mp3_set_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_out_rate), MP_ROM_PTR(&mp3_out_rate_obj) },
};
static MP_DEFINE_CONST_DICT(mp3_module_globals, mp3_module_globals_table);

const mp_obj_module_t mp3_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp3_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mp3, mp3_user_cmodule);
