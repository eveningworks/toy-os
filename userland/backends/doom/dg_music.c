// DG_music_module -- Doom's music, over lib/usnd.h.
//
// The SYNTHESIS is not ours and deliberately so: `music_opl_module` in
// the vendored tree is Chocolate Doom's own OPL driver, reading the
// WAD's GENMIDI instrument bank and driving an emulated Yamaha chip
// (opl/dbopl.c). That code was written against the very headers
// doomgeneric already ships -- `i_sound.h` declares `music_opl_module`
// by name -- so this module is a FORWARDER plus the one thing Chocolate
// Doom got from SDL and we have to supply ourselves: somewhere for the
// samples to go.
//
// **THE PUMP IS THE WHOLE JOB.** SDL pulled from a callback on its own
// audio thread; here nothing pulls, so `Poll` -- which Doom already
// calls once a frame from S_UpdateSounds -- renders as much as usnd's
// pushed source will take. That keeps the OPL clock driven by SAMPLES
// PRODUCED rather than by wall time, which is what stops the tempo
// drifting on an emulator whose speed varies.
#include <stdint.h>
#include "doomtype.h"
#include "i_sound.h"
#include "lib/usnd.h"
#include "ui/ulog.h"

// 256 is unity. See I_TOY_InitMusic for the measurement behind this.
#define MUSIC_GAIN 768

static int g_on;

static boolean I_TOY_InitMusic(void) {
    // -ENODEV and -EBUSY are both ordinary here, exactly as for the
    // effects: no card on the default boot, and the stream is exclusive.
    if (usnd_init() != 0) {
        ulogf("doom: no music -- %s\n", usnd_last_error());
        return false;
    }
    // The OPL driver opens the pushed source and starts the thread that
    // feeds it -- it has to, because OPL_Detect() blocks on a rendered
    // callback before this call returns. See opl_toyos.c.
    if (!music_opl_module.Init()) {
        ulogf("doom: OPL init failed\n");
        return false;
    }
    // OPL MUSIC IS MUCH QUIETER THAN SAMPLED EFFECTS, and balancing
    // them is ours to do -- Doom's own music and sfx sliders set each
    // path's level, not the level BETWEEN paths. Measured on the
    // attract demo: music alone peaked at 1735 while effects alone hit
    // ~19000, about 21 dB apart, which leaves the music inaudible under
    // gunfire. This lands it around a quarter of the effects' peak.
    // Nothing can clip: the mixer saturates rather than wrapping.
    usnd_push_set_gain(MUSIC_GAIN);

    g_on = 1;
    ulogf("doom: music on OPL emulation\n");
    return true;
}

static void I_TOY_ShutdownMusic(void) {
    if (!g_on) return;
    music_opl_module.Shutdown();
    g_on = 0;
}

// Volume is the OPL driver's: it scales the instrument output levels,
// which is what the DOS game did. usnd_push_set_gain() stays at unity
// so the two do not multiply into an unexpectedly quiet mix.
static void I_TOY_SetMusicVolume(int v) { music_opl_module.SetMusicVolume(v); }
static void I_TOY_PauseSong(void)       { music_opl_module.PauseMusic(); }
static void I_TOY_ResumeSong(void)      { music_opl_module.ResumeMusic(); }
static void *I_TOY_RegisterSong(void *d, int n) { return music_opl_module.RegisterSong(d, n); }
static void I_TOY_UnRegisterSong(void *h)       { music_opl_module.UnRegisterSong(h); }
static void I_TOY_PlaySong(void *h, boolean loop) { music_opl_module.PlaySong(h, loop); }
static void I_TOY_StopSong(void)        { music_opl_module.StopSong(); }
static boolean I_TOY_MusicIsPlaying(void) { return music_opl_module.MusicIsPlaying(); }

// NOTHING TO PUMP: the OPL driver renders on its own thread, so the
// music keeps its tempo through a slow frame -- which is the whole
// reason that thread exists. Kept as a slot rather than NULL so the
// reason is written down where somebody would look for it.
static void I_TOY_PollMusic(void) { }

static snddevice_t music_devices[] = { SNDDEVICE_ADLIB, SNDDEVICE_SB };

music_module_t DG_music_module = {
    music_devices, (int)(sizeof music_devices / sizeof music_devices[0]),
    I_TOY_InitMusic, I_TOY_ShutdownMusic, I_TOY_SetMusicVolume,
    I_TOY_PauseSong, I_TOY_ResumeSong, I_TOY_RegisterSong,
    I_TOY_UnRegisterSong, I_TOY_PlaySong, I_TOY_StopSong,
    I_TOY_MusicIsPlaying, I_TOY_PollMusic,
};
