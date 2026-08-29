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
// **WHAT A DAEMON STILL NEEDS, so the next session does not discover it
// halfway in**: there are no unix sockets here, PIPE_MAX is 8 kernel-
// wide, and a pipe carries no credentials -- so the real shape is a
// shared-memory ring per client, which is what PipeWire uses memfds
// for and which needs a kernel primitive this system has not got. See
// docs/roadmap.md's Sound track.

struct usnd_sink {
    const char *name;

    // Acquire the output. 0, or a negative errno -- -ENODEV (no
    // hardware) and -EBUSY (someone else holds it) are both ordinary
    // and must be distinguishable, because only one of them is worth
    // retrying.
    int (*open)(void);

    // Hand over up to `frames` frames of interleaved stereo s16 at
    // USND_RATE. Returns the number ACCEPTED, which may be 0 when the
    // sink is full -- that is not an error, it is back-pressure, and it
    // is what paces the worker thread. Never blocks.
    long (*write)(const int16_t *pcm, long frames);

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

#endif
