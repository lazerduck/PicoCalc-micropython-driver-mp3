// mp3_decode_minimp3.c
// Wrapper implementing mp3_decode.h using (stubbed) minimp3 interface.
// NOTE: Current embedded minimp3.h is a stub producing silence.
// Replace stub with full decoder to get real audio.

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "mp3_decode.h"
#include "minimp3.h"
#include <string.h>
#include <stdlib.h>
#include "vfs_bridge.h"

typedef struct mp3_decoder_s {
    mp3dec_t      core;
    mp_obj_t      file_obj;      // MicroPython file object
    char          path_copy[128];
    uint8_t       inbuf[16384];
    int           inbuf_len;
    int           inbuf_pos;
    int           sample_rate;
    int           channels;
    int           fixed_channels; // desired output channel count (1 or 2)
    // Stash for leftover decoded samples
    int16_t       stash[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int           stash_frames;
    int           stash_pos;
    int           eof;
    uint32_t      frames_decoded;
    uint32_t      zero_returns;
} mp3_decoder_impl_t;

static mp3_decoder_impl_t g_impl; // single instance (adjust to malloc if multiple needed)

mp3_decoder_t* mp3_decoder_create(void){
    memset(&g_impl, 0, sizeof(g_impl));
    mp3dec_init(&g_impl.core);
    g_impl.sample_rate = 44100; g_impl.channels = 2; // defaults
    g_impl.fixed_channels = 2;
    g_impl.frames_decoded = 0;
    g_impl.zero_returns = 0;
    g_impl.file_obj = MP_OBJ_NULL;
    g_impl.path_copy[0] = '\0';
    return (mp3_decoder_t*)&g_impl;
}

void mp3_decoder_destroy(mp3_decoder_t* dec){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec; if (d->file_obj != MP_OBJ_NULL) { vfs_close(&d->file_obj); }
}

// Refill while preserving leftover tail bytes (frame header may straddle boundary)
static int refill(mp3_decoder_impl_t* d){
    if (d->file_obj == MP_OBJ_NULL) return 0;
    int remain = 0;
    if (d->inbuf_pos < d->inbuf_len) {
        remain = d->inbuf_len - d->inbuf_pos;
        if (remain > 0 && remain < (int)sizeof(d->inbuf)) {
            memmove(d->inbuf, d->inbuf + d->inbuf_pos, remain);
        } else {
            remain = 0;
        }
    }
    int space = (int)sizeof(d->inbuf) - remain;
    if (space < 0) space = 0;
    int n = vfs_read(d->file_obj, d->inbuf + remain, space);
    if (n <= 0) {
        d->inbuf_len = remain;
        d->inbuf_pos = 0;
        d->eof = 1;
        return d->inbuf_len;
    }
    d->inbuf_len = remain + n;
    d->inbuf_pos = 0;
    return d->inbuf_len;
}

bool mp3_decoder_open(mp3_decoder_t* dec, const char* path, mp3_stream_info_t* out_info){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec;
    if (d->file_obj != MP_OBJ_NULL) { vfs_close(&d->file_obj); }
    d->inbuf_len = d->inbuf_pos = 0; d->eof = 0; d->stash_pos = d->stash_frames = 0; d->frames_decoded = 0; d->zero_returns = 0;
    size_t plen = strlen(path);
    if (plen >= sizeof(d->path_copy)) plen = sizeof(d->path_copy)-1;
    memcpy(d->path_copy, path, plen); d->path_copy[plen] = '\0';
    if (!vfs_open_rb(d->path_copy, &d->file_obj)) return false;
    // Prime header to get sample rate / channels. Also skip ID3v2 tag if present.
    refill(d);
    // ID3v2 header is 10 bytes: 'ID3' + ver + flags + 4-byte synchsafe size
    if (d->inbuf_len >= 10 && d->inbuf[0]=='I' && d->inbuf[1]=='D' && d->inbuf[2]=='3') {
        int tag_size = ( (d->inbuf[6] & 0x7f) << 21 ) |
                       ( (d->inbuf[7] & 0x7f) << 14 ) |
                       ( (d->inbuf[8] & 0x7f) << 7  ) |
                       ( (d->inbuf[9] & 0x7f) );
        tag_size += 10; // include header
        // Skip tag_size bytes by draining current buffer then skipping remainder in chunks
        int to_skip = tag_size;
        while (to_skip > 0) {
            if (d->inbuf_pos >= d->inbuf_len) {
                if (refill(d)==0) break; // EOF prematurely
            }
            int avail = d->inbuf_len - d->inbuf_pos;
            if (avail > to_skip) avail = to_skip;
            d->inbuf_pos += avail;
            to_skip -= avail;
        }
        // If we stopped mid-buffer, compact remaining bytes to front to decode seamlessly
        if (d->inbuf_pos < d->inbuf_len) {
            int remain = d->inbuf_len - d->inbuf_pos;
            memmove(d->inbuf, d->inbuf + d->inbuf_pos, remain);
            d->inbuf_len = remain;
            d->inbuf_pos = 0;
        } else {
            d->inbuf_len = d->inbuf_pos = 0;
        }
        if (d->inbuf_len == 0) refill(d);
    }
    mp3dec_frame_info_t fi; int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples = mp3dec_decode_frame(&d->core, d->inbuf + d->inbuf_pos, d->inbuf_len - d->inbuf_pos, pcm, &fi); // samples per channel
    if (fi.hz) {
        d->sample_rate = fi.hz;
    }
    if (fi.channels) {
        d->channels = fi.channels;
    }
    (void)samples; // initial header probe only
    // Decide fixed output channels (expand mono to stereo to keep mixer simple)
    d->fixed_channels = (d->channels == 1) ? 2 : d->channels;
    if (fi.frame_bytes > 0) {
        d->inbuf_pos += fi.frame_bytes; // consume the probed frame so we don't replay it
    }
    if (out_info){ out_info->sample_rate = d->sample_rate; out_info->channels = d->fixed_channels; }
    return true;
}

int mp3_decoder_decode(mp3_decoder_t* dec, int16_t* out, size_t max_frames){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec;
    if (d->stash_pos < d->stash_frames){
        int avail = d->stash_frames - d->stash_pos;
        if ((size_t)avail > max_frames) avail = (int)max_frames;
        memcpy(out, d->stash + d->stash_pos * d->channels, (size_t)avail * d->channels * sizeof(int16_t));
        d->stash_pos += avail;
        return avail;
    }
    d->stash_pos = d->stash_frames = 0;
    if (d->eof) return 0;
    // Single-attempt decode per call (keep ISR work bounded). On failure, skip by reported frame_bytes (offset) if any.
    mp3dec_frame_info_t fi; int samples_per_ch = 0;
    if (d->inbuf_pos >= d->inbuf_len - 4) {
        if (refill(d) <= 0) {
            // nothing more to read
            return 0;
        }
    }
    samples_per_ch = mp3dec_decode_frame(&d->core, d->inbuf + d->inbuf_pos, d->inbuf_len - d->inbuf_pos, d->stash, &fi);
    if (samples_per_ch <= 0) {
        // If decoder suggests an offset to next header (fi.frame_bytes holds skip on failure), advance by that many bytes.
        if (fi.frame_bytes > 0) {
            int skip = fi.frame_bytes;
            if (skip > (d->inbuf_len - d->inbuf_pos)) skip = d->inbuf_len - d->inbuf_pos;
            d->inbuf_pos += skip;
        } else {
            // Make forward progress to avoid re-scanning same byte forever
            if (d->inbuf_pos < d->inbuf_len) d->inbuf_pos++;
        }
        // Refill if we consumed most of the buffer
        if (d->inbuf_pos >= d->inbuf_len - 4) {
            refill(d);
        }
        d->zero_returns++;
        return 0;
    }
    d->inbuf_pos += fi.frame_bytes;
    if (fi.hz) {
        d->sample_rate = fi.hz;
    }
    if (fi.channels) {
        d->channels = fi.channels;
    }
    // Expand mono to stereo if needed
    if (d->channels == 1 && d->fixed_channels == 2) {
        for (int i = samples_per_ch - 1; i >= 0; --i) {
            int16_t s = d->stash[i];
            d->stash[2*i] = s;
            d->stash[2*i + 1] = s;
        }
        d->channels = 2;
    }
    d->stash_frames = samples_per_ch; // frames
    d->stash_pos = 0;
    if (d->stash_frames == 0) return 0;
    // Serve immediately
    int serve = d->stash_frames;
    if ((size_t)serve > max_frames) serve = (int)max_frames;
    memcpy(out, d->stash, (size_t)serve * d->channels * sizeof(int16_t));
    d->stash_pos = serve;
    d->frames_decoded++;
    return serve;
}

bool mp3_decoder_rewind(mp3_decoder_t* dec){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec;
    if (d->file_obj != MP_OBJ_NULL) { vfs_close(&d->file_obj); }
    if (!vfs_open_rb(d->path_copy, &d->file_obj)) return false;
    d->inbuf_len = d->inbuf_pos = 0; d->eof = 0; d->stash_pos = d->stash_frames = 0;
    return true;
}

bool mp3_decoder_is_eof(mp3_decoder_t* dec){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec;
    return d->eof && d->stash_pos >= d->stash_frames && d->inbuf_pos >= d->inbuf_len;
}

void mp3_decoder_get_diag(mp3_decoder_t* dec, uint32_t* frames, uint32_t* zero_returns){
    mp3_decoder_impl_t* d = (mp3_decoder_impl_t*)dec;
    if (frames) *frames = d->frames_decoded;
    if (zero_returns) *zero_returns = d->zero_returns;
}
