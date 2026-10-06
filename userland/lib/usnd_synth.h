#ifndef USND_SYNTH_H
#define USND_SYNTH_H

#include <stdint.h>
#include "lib/usnd_sf2.h"

// A General MIDI synthesiser over a SoundFont bank: sixteen channels of
// MIDI state and a pool of sample-playing voices. Fed channel messages,
// asked for PCM. It knows nothing about files or time -- usnd_mid.c
// is the sequencer that drives it, and a live MIDI port would be a
// second driver of the same calls.
//
// Not thread-safe: one caller at a time, which the mixer's lock gives.

#define USYNTH_VOICES 64

struct usynth;

struct usynth *usynth_new(const struct sf2_bank *bank, uint32_t rate);
void usynth_free(struct usynth *s);

// Every voice silenced and every controller at its GM power-on value.
void usynth_reset(struct usynth *s);

// One channel message: status 0x80..0xEF and its data bytes (d2 is
// ignored by the one-byte messages).
void usynth_message(struct usynth *s, uint8_t status, uint8_t d1, uint8_t d2);

// Interleaved s32 stereo (full scale in the top bits) at the rate given to usynth_new(). Always
// produces `frames` frames; with nothing sounding they are zeroes.
void usynth_render(struct usynth *s, int32_t *out, long frames);

// Put the block grid where it would be at `frame` of continuous
// playback -- after a seek, so what follows is sample-identical.
void usynth_align(struct usynth *s, uint64_t frame);

int usynth_active_voices(const struct usynth *s);

#endif
