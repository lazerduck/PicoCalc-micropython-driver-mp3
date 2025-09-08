// Minimal decoder interface: swap implementations without touching mp3player.c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int sample_rate;   // e.g., 44100
    int channels;      // 1 or 2
} mp3_stream_info_t;

typedef struct mp3_decoder_s mp3_decoder_t;

mp3_decoder_t* mp3_decoder_create(void);
void mp3_decoder_destroy(mp3_decoder_t* dec);

// Open from VFS path (for fake decoder this is ignored or used to seed)
bool mp3_decoder_open(mp3_decoder_t* dec, const char* path, mp3_stream_info_t* out_info);

// Decode up to `max_frames` PCM frames into interleaved int16 (LRLR...)
// Returns number of frames produced; 0 on EOF; negative on error
int mp3_decoder_decode(mp3_decoder_t* dec, int16_t* out_interleaved, size_t max_frames);

// Optional: reset/seek-to-beginning
bool mp3_decoder_rewind(mp3_decoder_t* dec);
