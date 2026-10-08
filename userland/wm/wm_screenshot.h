#ifndef WM_SCREENSHOT_H
#define WM_SCREENSHOT_H

#include <stdint.h>
#include "win_proto.h"

// The compositor's half of WIN_REQ_SCREENSHOT (abi/win_proto.h): copy a
// rectangle of the composited frame into a shared-memory object the
// asking client created and granted.
//
// It copies PIXELS and nothing else -- no file, no format, no path. The
// client encodes, which is why this file knows nothing about QOI or PNG
// and why /bin/screenshot rather than the compositor picks where a
// capture lands.

// `from` is the requesting pid, whose object is named WIN_SHOT_NAME_FMT.
// `rect` carries the requested rectangle going in and the one actually
// captured, in SCREEN coordinates, coming back.
//
// Returns 0, or a negative errno: -EBUSY while a scanout lease stands
// (the back buffer is not what is on screen then), -ENOENT for no such
// shm object, -ENOSPC when the capture does not fit the capacity the
// client declared, -EINVAL for an empty or unresolvable rectangle.
int wm_screenshot_capture(int from, int mode, unsigned flags,
                          int capacity_px, struct win_shot *rect);

// WIN_SHOT_DAMAGE (abi/win_proto.h): the caster's damage since its last
// capture, copied into its screen-sized mirror. `out` lists the rects.
// Where the pointer's hotspot is now, every frame: a caster that asked
// (WIN_SHOT_POINTER_POS) is told when it moved.
void wm_screenshot_pointer_at(int x, int y);
void wm_screenshot_pointer(int32_t *x, int32_t *y);   // ...and where that is

int wm_screenshot_damage(int from, unsigned flags, int capacity_px, struct win_shot *size,
                         struct win_damage *out);

// The render paths' hooks: what a frame repainted, where a moving
// pointer was and is (for a caster that asked for the pointer drawn
// in), and that the pointer's shape changed.
void wm_screenshot_frame_damage(int x, int y, int w, int h);
void wm_screenshot_pointer_damage(int x, int y, int w, int h);
void wm_screenshot_cursor_changed(void);
// A client went away: its caster slot is freed (wm_client.c's scan).
void wm_screenshot_client_gone(int pid);
// Is any caster sharing the screen with the pointer drawn in? The render
// paths ask before working out where the pointer's box is.
int wm_screenshot_casting_pointer(void);

#endif // WM_SCREENSHOT_H
