#ifndef WM_GEOMETRY_H
#define WM_GEOMETRY_H

#include "wm.h"

// Per-application window geometry, remembered across launches.
//
// KEYED BY `app_id`, NOT BY `app_identity`. The identity int is interned
// from a spawn path per boot (kernel/proc/win_server.c), so it is an
// index into a table built in launch order -- "identity 3" today is a
// different app tomorrow. `app_id` is a string the app declares and the
// registry defaults to its Exec basename, which is what makes it stable
// across reboots. Desktop icon positions already key on a name for the
// same reason (userland/wm/desktop.c).
//
// SAVED ON CLOSE, not on every move. Geometry changes from five call
// sites -- two drags, maximize, a client's resize ack, and the
// pull-back-on-screen -- and a drag writes new coordinates every frame.
// Saving there would be hundreds of whole-file rewrites per drag;
// close_window() is one chokepoint holding the final answer.

// Applies this window's remembered geometry, if it has any and the app
// allows it. Call once, right after a window is created and its app_id
// is set. Silently does nothing when there is nothing saved -- the
// caller's own default placement then stands.
void wm_geometry_restore(int idx);

// Records this window's geometry. Call from close_window() BEFORE the
// slot is shifted out of the array.
void wm_geometry_save(const struct window *win);

#endif
