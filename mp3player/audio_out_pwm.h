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

bool   audio_out_init(const audio_out_cfg_t* cfg);
void   audio_out_start(void);
void   audio_out_stop(void);

// Feed decoded PCM (interleaved int16 L,R,L,R...) into the output buffer.
// Returns number of FRAMES accepted.
size_t audio_out_push_interleaved(const int16_t* lr, size_t frames);
