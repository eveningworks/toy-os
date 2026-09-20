#ifndef WM_ANIM_H
#define WM_ANIM_H

// WINDOW ANIMATIONS: a window opening, closing, minimizing to its
// taskbar button or coming back from it is drawn as a GHOST -- a
// snapshot of the window (chrome and content) rendered once into its
// own buffer, then blitted scaled and faded along a 150 ms ease-out
// tween (lib/utween.h) while the live window is hidden or already
// gone. DWM and KWin do the same with the window's texture; here the
// snapshot is the texture.
//
// A ghost is independent of the window that spawned it: it owns its
// pixels, so a window whose client has exited still shrinks away, and
// windows[] can compact under it. While an OPEN or RESTORE ghost runs,
// render_scene() skips the live window (wm_anim_hides()), so the two
// are never both on screen.
//
// Frames come from the WM loop: while any ghost is active the loop
// waits one frame instead of its long park and wm_anim_step() sets
// redraw_pending, damaging the ghost's old and new rects.
//
// `desktop.animations` turns all of it off: every state change then
// lands in one frame, exactly as before.

#include "wm.h"

void wm_anim_poll_config(void);
int  wm_anim_enabled(void);

int  wm_anim_active(void);   // how many ghosts are in flight
// Ghost `i` (0..WM_ANIM_MAX-1) as drawn THIS frame: its rect and alpha,
// or 0 when that slot is idle. `gui state --json` reports them so a
// test can ask where a ghost is instead of racing a screenshot.
#define WM_ANIM_MAX 8
int  wm_anim_rect(int i, int *x, int *y, int *w, int *h, int *alpha);

// How many PIECES ghost `i` is drawn as -- 1 for every effect except
// `shatter`, which breaks the window into tiles. Reported through
// `gui state --json`, because a shatter and a scale move the same
// bounding box toward the same button: without this a test cannot tell
// one from the other, and an effect that quietly fell back to scale
// would pass every assertion about where the ghost is.
int  wm_anim_pieces(int i);

// Starters. Each takes the window's index NOW, snapshots it, and the
// caller then changes the state (or closes it) as it always did.
void wm_anim_open(int idx);      // first present of a new toplevel: scale in from 92%
void wm_anim_close(int idx);     // before its buffers go: scale out and fade
void wm_anim_minimize(int idx);  // before state = WIN_MINIMIZED: shrink to its taskbar button
void wm_anim_restore(int idx);   // before state = WIN_NORMAL: grow back from it

// Loop and scene hooks.
void wm_anim_step(void);         // advance every ghost; damages and asks for a frame
void wm_anim_draw(void);         // blit the ghosts (after the taskbar, before overlays)
int  wm_anim_hides(const struct window *w); // the live window is stood in for by a ghost

#define WM_ANIM_FRAME_MS 16

#endif // WM_ANIM_H
