#include "mp3_decode.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct mp3_decoder_s {
    int   sr, ch;
    float phase;
    bool  eof;
};

static struct mp3_decoder_s g_dec;

mp3_decoder_t* mp3_decoder_create(void) {
    memset(&g_dec, 0, sizeof(g_dec));
    g_dec.sr = 44100; g_dec.ch = 2; g_dec.phase = 0.f; g_dec.eof = false;
    return &g_dec;
}
void mp3_decoder_destroy(mp3_decoder_t* dec) { (void)dec; }
bool mp3_decoder_open(mp3_decoder_t* dec, const char* path, mp3_stream_info_t* out_info) {
    (void)path;
    if (out_info) { out_info->sample_rate = dec->sr; out_info->channels = dec->ch; }
    return true;
}
int mp3_decoder_decode(mp3_decoder_t* dec, int16_t* out, size_t max_frames) {
    const float freq = 440.0f;
    const float inc  = 2.f * (float)M_PI * freq / (float)dec->sr;
    for (size_t i = 0; i < max_frames; ++i) {
        float s = sinf(dec->phase);
        dec->phase += inc;
        if (dec->phase > 2.f * (float)M_PI) dec->phase -= 2.f * (float)M_PI;
        int16_t sample = (int16_t)(s * 20000);
        out[2*i + 0] = sample;
        out[2*i + 1] = sample;
    }
    return (int)max_frames;
}
bool mp3_decoder_rewind(mp3_decoder_t* dec) { dec->phase = 0.f; return true; }
