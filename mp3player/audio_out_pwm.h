// audio_out_pwm.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int pin_l;
    int pin_r;
    int sample_rate; // e.g., 44100
    int channels;    // 1 or 2
} audio_out_cfg_t;

// Provider callback: fill up to max_frames of interleaved PCM (int16 LRLR...).
// Return number of frames actually provided (may be < max_frames on underrun).
typedef size_t (*audio_out_provider_t)(int16_t* dst, size_t max_frames, void* user);

bool   audio_out_init(const audio_out_cfg_t* cfg);
void   audio_out_set_provider(audio_out_provider_t cb, void* user);
void   audio_out_start(void);   // requires provider set
void   audio_out_stop(void);

// (Legacy push/free API removed in pull-model; keep stubs if needed.)
uint32_t audio_out_underruns(void);