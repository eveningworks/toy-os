#ifndef ULIB_UVID_PLAY_H
#define ULIB_UVID_PLAY_H

// uvid_play -- PLAYING a video: a thread decoding a few frames ahead, the
// sound through lib/usnd.h, and the frame due now for whoever draws.
// The Video Player, the video wallpaper and ui/uui_video.h's widget all
// play through it; lib/uvid.h is the decoding underneath.
//
// **THE SOUND IS THE CLOCK** (lib/uvid.h says why): with a sound track
// and a sound device, "now" is usnd_position() -- what has been HEARD --
// and the pictures follow it; a picture that is late is DROPPED rather
// than shown late, as every player does. With no sound, muted, or at a
// speed other than 1x, "now" is the monotonic clock.
//
// **THE MAIN THREAD ONLY EVER LOOKS.** uvid_play_tick() picks the frame
// due now out of what the decoder already finished and never decodes;
// the decoder copies each frame into a slot of its own, so the one being
// drawn is never written. The decoder PARKS BY SLEEPING: a condition
// variable spins here (lib/uthumb.c's measurement).
//
// usnd has ONE streaming voice per process, so one player with sound at
// a time; UVID_PLAY_MUTE players are free.
#include <stdint.h>
#include "lib/uvid.h"

struct uvid_play;

#define UVID_PLAY_MUTE 0x01     // never touches usnd: a wallpaper, a preview
#define UVID_PLAY_LOOP 0x02     // the start again at the end

int  uvid_play_open(struct uvid_play **out, const char *path, unsigned flags);
void uvid_play_close(struct uvid_play *p);
const struct uvid_info *uvid_play_info(const struct uvid_play *p);

void uvid_play_set_paused(struct uvid_play *p, int paused);
int  uvid_play_paused(const struct uvid_play *p);
// Exact: the next frame shown is the one showing at `ms`.
void uvid_play_seek(struct uvid_play *p, uint32_t ms);
// Percent of normal speed, 25..400. At anything but 100 the sound is
// silenced and the pictures run on the clock: usnd has no time stretch,
// and sound at the wrong pitch is worse than none.
void uvid_play_set_speed(struct uvid_play *p, int percent);
int  uvid_play_speed(const struct uvid_play *p);

// The clock, in ms from the start of the file.
uint32_t uvid_play_position(struct uvid_play *p);
// Past the end, with nothing left to show (never, with UVID_PLAY_LOOP).
int  uvid_play_ended(struct uvid_play *p);

// Moves to the frame due now. 1 when it changed: the caller repaints.
int  uvid_play_tick(struct uvid_play *p);
// The frame to draw, valid until the next tick; NULL before the first.
const struct uvid_frame *uvid_play_frame(const struct uvid_play *p);
// Changes whenever the frame does: what a drawer caching its scaled
// picture keys on, since a slot's address repeats. 0 for a NULL player.
uint32_t uvid_play_serial(const struct uvid_play *p);

struct uvid_play_stats {
    uint32_t shown, dropped;    // since open
    int sound;                  // the sound is playing and is the clock
};
void uvid_play_stats(const struct uvid_play *p, struct uvid_play_stats *out);

#endif
