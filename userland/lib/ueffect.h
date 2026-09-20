#ifndef ULIB_UEFFECT_H
#define ULIB_UEFFECT_H

#include "lib/usaver.h" // struct usaver -- the declared-option set

// A WINDOW EFFECT'S OPTIONS -- what it declares, and what the user
// chose. The screensavers' arrangement, applied to the compositor's
// minimize effects:
//
//   /usr/wm/effects/<name>.effect   what options exist -- shipped, read-only
//   /etc/effects/<name>.conf        what they are set to -- written by the user
//
// **WHY NOT TWO FLAT SETTINGS IN /etc/settings.d.** Because an option
// belongs to ONE effect, and a flat setting cannot say so: `shatter`'s
// piece count would sit in Appearance -> Effects being asked about
// while Scale is selected, and every effect added later would leave
// another orphan row behind it. XScreenSaver solved this for savers and
// this system already copies that; KDE's per-effect config button is
// the same idea from the other direction.
//
// **AND THE FORMAT IS ALREADY GENERIC.** `struct usaver_opt` is "a
// bounded int or a named choice, with a label, a default and a
// resolved value" -- nothing in it is about screensavers. So this is
// the same loader over a different pair of paths
// (`usaver_load_files()`), not a second parser.
//
// AN EFFECT WITH NO DESCRIPTOR HAS NO OPTIONS, which is a state and not
// an error: `scale`, `genie`, `squash` and `glide` ship without one
// and take no configuring. `shatter` is the first that does.

#define UEFFECT_DESC_DIR "/usr/wm/effects"
#define UEFFECT_CONF_DIR "/etc/effects"

// Loads `effect`'s descriptor and its saved values. Returns 1 when a
// descriptor was read, 0 when there is none -- and `out` is usable
// either way, with opt_count 0, so no caller needs the distinction.
int ueffect_load(const char *effect, struct usaver *out);

// Where `effect`'s values are written. One place, because System
// Settings writes this path and the compositor reads it.
void ueffect_conf_path(const char *effect, char *out, size_t cap);

#endif // ULIB_UEFFECT_H
