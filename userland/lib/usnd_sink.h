#ifndef USND_SINK_H
#define USND_SINK_H

#include <stdint.h>

// Where mixed samples go. INTERNAL to usnd -- an app includes usnd.h
// and never learns which of these it got.
//
// One row today: the kernel's exclusive stream. The row that does not
// exist is a sound DAEMON, and this interface is the whole reason it
// can arrive without touching a single app -- the same move libasound
// made when PulseAudio appeared, where an app kept calling
// snd_pcm_writei() and a plugin redirected it.
//
// BOTH ROWS EXIST NOW. The daemon's is a shared-memory ring per client
// (SYS_SHM_OPEN, the primitive PipeWire uses memfds for), and it is
// tried FIRST -- the device row is what a machine with no daemon, or a
// daemon that has died, falls back to.

struct usnd_sink {
    const char *name;

    // Acquire the output. 0, or a negative errno -- -ENODEV (no
    // hardware) and -EBUSY (someone else holds it) are both ordinary
    // and must be distinguishable, because only one of them is worth
    // retrying.
    int (*open)(void);

    // THE RATE TO WRITE AT. `want` is the content's own rate (0: no
    // preference) -- a card set to follow what plays may switch to it.
    // The answer is the rate the sink plays; 0 means it is not decided
    // yet (a daemon answering, or the card draining before a switch),
    // and the caller writes NOTHING until it is not 0 -- what is queued
    // then plays out at the old rate, which is what makes a switch clean.
    uint32_t (*rate)(uint32_t want);

    // Hand over up to `frames` frames of interleaved stereo s32 at the
    // rate above -- the ring's own format (abi/sound_abi.h). Returns the number ACCEPTED, which may be 0 when the
    // sink is full -- that is not an error, it is back-pressure, and it
    // is what paces the worker thread. Never blocks.
    long (*write)(const int32_t *pcm, long frames);

    // How many frames the sink will take right now. The worker asks
    // before it decodes, so a full sink costs no decoding at all.
    long (*space)(void);

    // Frames handed over that have NOT been played yet. This is what
    // turns "frames decoded" into "frames the listener has heard" -- a
    // position readout without it runs a third of a second fast, which
    // is the ring's whole depth.
    long (*pending)(void);

    // Throw away everything not yet played. A seek needs it: without
    // one, the old position keeps playing for the length of whatever is
    // still queued.
    void (*flush)(void);

    void (*close)(void);
};

// The kernel's exclusive PCM stream (SYS_SND_OPEN + the mapped ring).
extern const struct usnd_sink usnd_sink_device;

// /bin/soundd, through a shared-memory ring this process owns. `open`
// returns -ENODEV when no daemon is running, which is the ordinary
// case on a machine that never started one.
extern const struct usnd_sink usnd_sink_daemon;

#endif
