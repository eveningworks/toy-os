// DG_sound_module -- Doom's sound effects, over lib/usnd.h.
//
// doomgeneric ships no sound backend: `FEATURE_SOUND` is left
// undefined, so `sound_modules[]` is empty and `I_InitSound` finds
// nothing. This is the module that fills it, and it lives HERE rather
// than in userland/ports/doom/ for the same reason dg_toyos.c does --
// the vendored tree stays byte for byte upstream.
//
// **DOOM ALREADY DECIDED EVERYTHING THIS FILE COULD HAVE DECIDED.**
// s_sound.c does distance attenuation, channel allocation and stealing,
// and hands `I_StartSound` a channel, a volume (0-127) and a separation
// (0-255). So the backend is a mapping and nothing more: one usnd voice
// per Doom channel, with the vol/sep pair turned into a stereo gain
// pair. That is what Chocolate Doom does too -- SDL_mixer channels plus
// Mix_SetPanning -- and it is why there is no mixer in this file. There
// is already one, in usnd.
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "lib/usnd.h"
#include "ui/ulog.h"

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "deh_str.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

// Doom asks for at most 16 (snd_channels); usnd has USND_VOICES.
#define MAX_CHANNELS 16

// A DMX lump: 8-byte header, then 8-bit UNSIGNED mono samples.
#define DMX_HEADER 8
// The DMX library skips 16 sample bytes at each end. Reason unknown
// upstream and copied deliberately: a sound trimmed differently from
// every other port is a difference nobody would think to look for.
#define DMX_PAD 16
// Below this DMX itself refuses to play a lump, so we do too.
#define DMX_MIN_SAMPLES 48

static int g_have_sound;
static boolean g_use_sfx_prefix;

// THE SDL PORT'S CONFIG VARIABLES, defined here because Doom binds them
// whether or not anything reads them: `i_sound.c` and `m_config.c` both
// reference them under FEATURE_SOUND, and they are normally provided by
// `i_sdlsound.c`, which doomgeneric does not ship. usnd does its own
// rate conversion, so nothing here consults either value -- they exist
// so the config table resolves.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// One usnd voice per Doom channel. USND_VOICE_NONE means "nothing here",
// and a handle whose sound has finished reports itself inactive rather
// than needing to be cleaned up on a cadence.
static usnd_voice_t g_channel[MAX_CHANNELS];

// The decoded clip per sound effect, cached in the vendored struct's own
// `driver_data` field -- which exists for exactly this and means the
// cache cannot fall out of step with S_sfx[].
static struct usnd_clip *clip_of(sfxinfo_t *sfx) { return sfx->driver_data; }

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t len) {
    if (sfx->link != NULL) sfx = sfx->link;
    if (g_use_sfx_prefix) M_snprintf(buf, len, "ds%s", DEH_String(sfx->name));
    else                  M_StringCopy(buf, DEH_String(sfx->name), len);
}

// Decodes one DMX lump into a clip. The conversion to 48 kHz stereo is
// usnd's -- a DMX lump is 8-bit mono at about 11 kHz, and every one of
// those three differences from the device is the library's to close.
static boolean CacheSFX(sfxinfo_t *sfx) {
    if (clip_of(sfx)) return true;
    if (sfx->lumpnum < 0) return false;

    byte *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
    unsigned lumplen = W_LumpLength(sfx->lumpnum);
    boolean ok = false;

    if (lumplen >= DMX_HEADER && data[0] == 0x03 && data[1] == 0x00) {
        unsigned rate = (unsigned)((data[3] << 8) | data[2]);
        unsigned len  = (unsigned)((data[7] << 24) | (data[6] << 16) |
                                   (data[5] << 8)  |  data[4]);
        // A header claiming more than the lump holds is a broken sound,
        // not a sound to read past the end of.
        if (len <= lumplen - DMX_HEADER && len > DMX_MIN_SAMPLES &&
            len > 2 * DMX_PAD && rate >= 4000) {
            const byte *pcm8 = data + DMX_HEADER + DMX_PAD;
            unsigned frames = len - 2 * DMX_PAD;

            // 8-bit UNSIGNED, centred on 128. usnd takes s16, so the
            // widening happens here; getting the bias wrong would make
            // silence full-scale, which is the loudest way to be wrong.
            int16_t *s16 = malloc((size_t)frames * sizeof(int16_t));
            if (s16) {
                for (unsigned i = 0; i < frames; i++)
                    s16[i] = (int16_t)(((int)pcm8[i] - 128) << 8);

                struct usnd_clip *c = malloc(sizeof *c);
                if (c && usnd_clip_from_pcm(s16, frames, rate, 1, c) == 0) {
                    sfx->driver_data = c;
                    ok = true;
                } else {
                    free(c);
                }
                free(s16);   // usnd copied what it needed
            }
        }
    }
    W_ReleaseLumpNum(sfx->lumpnum);
    return ok;
}

// vol is 0-127 and sep is 0-255 (128 = centre), both Doom's own scales.
// The gains are its: `sep` maps to a pair of linear weights, which is
// what Chocolate Doom hands Mix_SetPanning and near enough what the DOS
// original did.
static void gains_for(int vol, int sep, int *out_l, int *out_r) {
    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 255) sep = 255;
    int left  = 254 - sep;      // Chocolate Doom's own expressions
    int right = sep;
    *out_l = (vol * left)  / 127;   // 0..254 at full volume
    *out_r = (vol * right) / 127;
}

// --- the module -------------------------------------------------------

static boolean I_TOY_InitSound(boolean use_sfx_prefix) {
    g_use_sfx_prefix = use_sfx_prefix;
    for (int i = 0; i < MAX_CHANNELS; i++) g_channel[i] = USND_VOICE_NONE;

    // NO HARDWARE IS NOT AN ERROR anywhere else in this system, but it
    // IS here: returning false is how i_sound.c is told to fall through
    // to the next module and leave the game silent, which is exactly
    // right on the default boot (no AC97) and while another program
    // holds the exclusive stream.
    if (usnd_init() != 0) {
        ulogf("doom: no sound -- %s\n", usnd_last_error());
        return false;
    }
    g_have_sound = 1;
    ulogf("doom: sound on %s\n", usnd_sink_name());
    return true;
}

static void I_TOY_ShutdownSound(void) {
    if (!g_have_sound) return;
    for (int i = 0; i < MAX_CHANNELS; i++) usnd_voice_stop(g_channel[i]);
    // The clips are NOT freed: Z_Zone owns S_sfx and the process is
    // exiting. Freeing them would mean walking a table this module does
    // not own to find pointers it wrote into somebody else's struct.
    usnd_shutdown();
    g_have_sound = 0;
}

static int I_TOY_GetSfxLumpNum(sfxinfo_t *sfx) {
    char name[9];
    GetSfxLumpName(sfx, name, sizeof name);
    return W_GetNumForName(name);
}

// Nothing to pump: usnd's own worker thread keeps the device fed, which
// is the whole reason a slow frame cannot cause a dropout here.
static void I_TOY_UpdateSound(void) { }

static void I_TOY_UpdateSoundParams(int channel, int vol, int sep) {
    if (!g_have_sound || channel < 0 || channel >= MAX_CHANNELS) return;
    int l, r;
    gains_for(vol, sep, &l, &r);
    usnd_voice_set_gain(g_channel[channel], l, r);
}

static int I_TOY_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if (!g_have_sound || channel < 0 || channel >= MAX_CHANNELS) return -1;
    if (!CacheSFX(sfx)) return -1;

    // Doom has already decided this channel is free; whatever was on it
    // stops now, which is what makes a re-triggered sound restart rather
    // than layer on itself.
    usnd_voice_stop(g_channel[channel]);

    int l, r;
    gains_for(vol, sep, &l, &r);
    g_channel[channel] = usnd_voice_play(clip_of(sfx), l, r);
    return g_channel[channel] == USND_VOICE_NONE ? -1 : channel;
}

static void I_TOY_StopSound(int channel) {
    if (!g_have_sound || channel < 0 || channel >= MAX_CHANNELS) return;
    usnd_voice_stop(g_channel[channel]);
    g_channel[channel] = USND_VOICE_NONE;
}

static boolean I_TOY_SoundIsPlaying(int channel) {
    if (!g_have_sound || channel < 0 || channel >= MAX_CHANNELS) return false;
    return usnd_voice_active(g_channel[channel]) ? true : false;
}

// PRECACHED AT STARTUP: every effect is decoded before play begins, so
// no sound ever costs a conversion on the frame it is first heard.
// About 11 MB for Doom's full set, flat and predictable, against a
// lazy cache that is smaller but hitches once per new sound.
static void I_TOY_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    if (!g_have_sound) return;
    int ok = 0, found = 0;
    for (int i = 0; i < num_sounds; i++) {
        char name[9];
        GetSfxLumpName(&sounds[i], name, sizeof name);

        // W_CheckNumForName, NEVER W_GetNumForName. S_Init precaches
        // over the WHOLE of S_sfx[], and entry 0 is a dummy with an
        // empty name -- W_GetNumForName calls I_Error on a miss, so
        // asking it here kills the game before its window ever opens.
        int lump = W_CheckNumForName(name);
        if (lump < 0) continue;
        sounds[i].lumpnum = lump;
        found++;
        if (CacheSFX(&sounds[i])) ok++;
    }
    ulogf("doom: cached %d/%d sound effects (%d named)\n", ok, num_sounds, found);
}

static snddevice_t sound_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    sound_devices,
    (int)(sizeof sound_devices / sizeof sound_devices[0]),
    I_TOY_InitSound,
    I_TOY_ShutdownSound,
    I_TOY_GetSfxLumpNum,
    I_TOY_UpdateSound,
    I_TOY_UpdateSoundParams,
    I_TOY_StartSound,
    I_TOY_StopSound,
    I_TOY_SoundIsPlaying,
    I_TOY_CacheSounds,
};
