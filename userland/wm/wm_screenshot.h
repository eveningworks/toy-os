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

#endif // WM_SCREENSHOT_H
