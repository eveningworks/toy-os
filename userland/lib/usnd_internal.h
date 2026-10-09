#ifndef USND_INTERNAL_H
#define USND_INTERNAL_H

#include "lib/usnd.h"

// Shared between usnd.c and its codecs. Not for apps.

// Records the sentence usnd_last_error() returns. A codec calls this on
// every refusal, because the errno alone cannot tell "not a WAV" from
// "a WAV this build will not play".
void usnd_fail(const char *msg);

// Decode a whole (already opened) stream into a clip, in the device
// format. Shared so usnd_clip_load() and usnd_clip_from_pcm() cannot
// drift: they differ only in where the samples come from.
int usnd_clip_drain(struct usnd_stream *s, struct usnd_clip *c);

// THE MPEG AUDIO SYNTHESIS FILTERBANK (ISO/IEC 11172-3, 2.4.3.2.2): 32
// subband samples in, 32 PCM samples out, per time slot -- the last stage
// of every MPEG-1 audio layer, so Layer II and Layer III share one.
// `sb` is `slots` rows of 32 subband samples, nominal full scale +-1.0;
// out is s32, every `stride`th sample. The state is the 1024-sample
// history a seek must clear.
struct usnd_mpsynth {
    float v[1024];
    int pos;
    // The window's 512 taps, gathered per slot. In the state rather than
    // on the stack: 2 KiB is the whole ring-3 frame budget.
    float u[512];
};
void usnd_mpsynth_reset(struct usnd_mpsynth *s);
void usnd_mpsynth_run(struct usnd_mpsynth *s, const float *sb, int slots, int32_t *out, int stride);

// An MPEG-1 audio frame header, any layer: 1 when `h` (4 bytes) is one.
// The MPEG-2 half-rate extension is not recognised.
struct usnd_mpa_hdr {
    int layer;          // 1, 2 or 3
    int rate, channels, bitrate;    // Hz, 1 or 2, kbit/s
    int frame_bytes, padding, protect, mode, mode_ext;
};
int usnd_mpa_header(const uint8_t *h, struct usnd_mpa_hdr *out);

// A Layer II frame decoder over bytes the caller found: usnd_codec_mp2
// for a .mp2 file, usnd_vid.c for the sound in a .mpg. decode() takes one
// whole frame and writes 1152 sample frames, interleaved s32, returning
// that count or a negative errno; reset() is for after a seek.
struct usnd_mp2;
struct usnd_mp2 *usnd_mp2_new(void);
void usnd_mp2_free(struct usnd_mp2 *d);
void usnd_mp2_reset(struct usnd_mp2 *d);
int  usnd_mp2_decode(struct usnd_mp2 *d, const uint8_t *frame, int len, int32_t *out);

// Structural check on the MP3 Huffman tables: every one must be a
// complete prefix code. Needs no audio, so /tests/usnd_test and
// tools/usnd_hostcheck.py both run it before anything else. 0 on success.
int usnd_mp3_selftest(void);

// The SoundFont the MIDI codec plays with, in place of the one it would
// find in /usr/share/soundfonts; NULL restores the search. For tests,
// which need a bank whose every sample they know.
void usnd_mid_set_soundfont(const char *path);

#endif
