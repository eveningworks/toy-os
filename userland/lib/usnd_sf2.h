#ifndef USND_SF2_H
#define USND_SF2_H

#include <stdint.h>
#include <stddef.h>

// A SoundFont 2 bank, parsed and FLATTENED. Shared between usnd_synth.c,
// usnd_mid.c and their tests; not for apps.
//
// **THE WHOLE SAMPLE CHUNK IS READ INTO MEMORY AT LOAD**, never mapped
// and paged in: a page fault in the mixer thread is a dropout, which is
// why FluidSynth loads and mlock()s its banks by default. The bank is
// cached per process (sf2_get/sf2_put) because a player opens every file
// twice -- once for its info, once to play it.
//
// A preset's zones and its instruments' zones are multiplied out into
// REGIONS at load, each with its generators summed and its modulators
// merged, so a note-on is a scan of one preset's regions and nothing
// else.

// SoundFont 2.04 generator numbers, the ones anything here reads.
enum {
    SF2_START_OFS = 0, SF2_END_OFS = 1, SF2_LSTART_OFS = 2, SF2_LEND_OFS = 3,
    SF2_START_COARSE = 4, SF2_MODLFO_PITCH = 5, SF2_VIBLFO_PITCH = 6,
    SF2_MODENV_PITCH = 7, SF2_FILTER_FC = 8, SF2_FILTER_Q = 9,
    SF2_MODLFO_FC = 10, SF2_MODENV_FC = 11, SF2_END_COARSE = 12,
    SF2_MODLFO_VOL = 13, SF2_PAN = 17,
    SF2_MODLFO_DELAY = 21, SF2_MODLFO_FREQ = 22,
    SF2_VIBLFO_DELAY = 23, SF2_VIBLFO_FREQ = 24,
    SF2_MODENV_DELAY = 25, SF2_MODENV_ATTACK = 26, SF2_MODENV_HOLD = 27,
    SF2_MODENV_DECAY = 28, SF2_MODENV_SUSTAIN = 29, SF2_MODENV_RELEASE = 30,
    SF2_KEY_MODENV_HOLD = 31, SF2_KEY_MODENV_DECAY = 32,
    SF2_VOLENV_DELAY = 33, SF2_VOLENV_ATTACK = 34, SF2_VOLENV_HOLD = 35,
    SF2_VOLENV_DECAY = 36, SF2_VOLENV_SUSTAIN = 37, SF2_VOLENV_RELEASE = 38,
    SF2_KEY_VOLENV_HOLD = 39, SF2_KEY_VOLENV_DECAY = 40,
    SF2_INSTRUMENT = 41, SF2_KEY_RANGE = 43, SF2_VEL_RANGE = 44,
    SF2_LSTART_COARSE = 45, SF2_KEYNUM = 46, SF2_VELOCITY = 47,
    SF2_ATTENUATION = 48, SF2_LEND_COARSE = 50,
    SF2_COARSE_TUNE = 51, SF2_FINE_TUNE = 52, SF2_SAMPLE_ID = 53,
    SF2_SAMPLE_MODES = 54, SF2_SCALE_TUNING = 56, SF2_EXCLUSIVE = 57,
    SF2_ROOT_KEY = 58,
    SF2_GEN_COUNT = 61
};

// A modulator (SF2 2.04 section 8.2): `amount` scaled by two mapped
// sources, added to generator `dest`. A source packs the controller
// index, the CC flag (0x80), direction (0x100), polarity (0x200) and
// curve (bits 10-15: linear, concave, convex, switch).
struct sf2_mod {
    uint16_t src, dest, amt_src, transform;
    int16_t amount;
};

#define SF2_MAX_MODS 32             // per region; more is dropped

struct sf2_region {
    uint8_t key_lo, key_hi, vel_lo, vel_hi;
    int16_t gen[SF2_GEN_COUNT];     // instrument + preset, summed
    // The FINAL modulator list, in sf2_bank.mods: the defaults, replaced
    // by identical instrument modulators, with the preset's ADDED.
    uint32_t mod_first;
    uint16_t mod_count;

    // The sample, with the address offsets APPLIED and CHECKED against
    // the sample chunk -- so the synth indexes `data` without a bound.
    uint32_t start, end;            // [start, end)
    uint32_t loop_start, loop_end;  // [loop_start, loop_end); equal = none
    uint32_t rate;
    uint8_t  root;                  // overridingRootKey or the sample's own
    int8_t   correction;            // cents
};

struct sf2_preset {
    uint16_t bank, program;
    char name[21];
    uint32_t first, count;          // into sf2_bank.regions
};

struct sf2_bank {
    int16_t *data;                  // the sample chunk, plus a zeroed tail
    uint32_t nsamples;
    struct sf2_preset *presets;
    uint32_t npresets;
    struct sf2_region *regions;
    uint32_t nregions;
    struct sf2_mod *mods;
    uint32_t nmods;
    char name[64];                 // INFO/INAM, or empty
    char path[128];
    int refs;
};

// Parse a bank. 0, -ENOMEM, or -EINVAL with usnd_fail() set; a bank
// whose structure is broken is REFUSED rather than partly loaded.
int  sf2_load(const char *path, struct sf2_bank **out);
void sf2_free(struct sf2_bank *b);

// The same, over bytes already in memory (the tests build banks this
// way). The bytes are not kept.
int  sf2_parse(const uint8_t *d, size_t n, struct sf2_bank **out);

// The process's cached bank for `path`, loaded on first use. A bank a
// stream still holds is never freed from under it.
int  sf2_get(const char *path, struct sf2_bank **out);
void sf2_put(struct sf2_bank *b);

// (bank, program), or NULL. No fallback here -- that is GM policy and
// belongs to the synth.
const struct sf2_preset *sf2_find(const struct sf2_bank *b, int bank, int program);

#endif
