#ifndef ULIB_USHOT_H
#define ULIB_USHOT_H

#include <stdint.h>
#include "win_proto.h"
#include "lib/uchan.h"

// Taking a screenshot, from the asking side of WIN_REQ_SCREENSHOT.
//
// The split this library sits on: THE COMPOSITOR COPIES PIXELS AND THE
// CLIENT ENCODES THEM (abi/win_proto.h). So everything about image
// formats, filenames and where a capture lands is here, in the caller's
// process, and the compositor knows none of it -- which is what lets
// /bin/screenshot be an ordinary program with no window, usable over
// telnet on a machine whose screen you cannot see.
//
// **THE BUFFER IS ALLOCATED ONCE, AT SCREEN SIZE, AND KEPT.** A shot is
// then a copy into memory that is already mapped and granted, so a
// screenshot app taking one every few seconds costs no allocation and no
// grant. It also means a REGION capture cannot overflow: its rectangle
// is a sub-rectangle of the screen by construction.

struct ushot {
    // The capture. `px` is w*h pixels of 0x00RRGGBB, tightly packed --
    // exactly what struct uimg wants, so an encode needs no conversion.
    // (x, y) is where the rectangle sat on the SCREEN, which is the
    // compositor's answer rather than the caller's question: a WINDOW
    // capture does not know where it will land.
    int w, h, x, y;
    uint32_t *px;
    char app[16];   // a WINDOW capture's app id ("notepad"), else ""

    // Private. `px` points into `map`; nothing here is separately freed.
    void *map;
    uint64_t map_bytes;
    int cap_px;
    int screen_w, screen_h;
    struct uchan_client own;   // used only when this opened its own
    struct uchan_client *chan;
    int chan_owned;
};

// Connects to the compositor and allocates the shared buffer. Returns 0,
// or a negative errno: -ENOENT if no compositor is running (which is
// what a capture attempted from the text console gets), -ENOMEM.
int ushot_open(struct ushot *s);

// **ONE PER PROCESS.** The capture buffer is a shared-memory object named
// after the pid, so a second struct ushot unlinks the first's and both
// read the same pixels. Copy a frame out to keep two.

// The same, on a channel the caller already holds. A process may hold
// only ONE ring per server -- the name is derived from its pid -- so a
// windowed app MUST pass uapp_wmchan() rather than opening a second,
// and closing this never closes a borrowed channel.
int ushot_open_on(struct ushot *s, struct uchan_client *chan);

// Captures. `mode` is WIN_SHOT_SCREEN / _WINDOW / _REGION, `flags` is
// WIN_SHOT_* (WIN_SHOT_POINTER), and the rectangle is read for REGION
// only. On success `s->w/h/x/y/px` describe what was captured.
//
// Returns 0 or a negative errno, including -EBUSY while a fullscreen
// client holds the display (see abi/win_proto.h) -- a refusal there
// rather than a stale frame nothing would flag as stale.
int ushot_take(struct ushot *s, int mode, unsigned flags,
               int x, int y, int w, int h);

// The rectangle a capture WOULD take, without taking one. `mode` is
// WIN_SHOT_WINDOW or WIN_SHOT_WINDOW_AT (which reads x/y as a point);
// `out` gets the rect in screen coordinates. Nothing is copied and the
// held capture is untouched, so a picker can ask on every pointer move.
//
// `flags`: WIN_SHOT_SHADOW, to get the rect a shadowed capture would take.
//
// Returns 0, or -EINVAL when there is no window there.
int ushot_probe(struct ushot *s, int mode, unsigned flags, int x, int y, struct win_shot *out);

// Narrows the capture already held to a sub-rectangle, in the captured
// image's own coordinates. The pixels MOVE inside the buffer, so this is
// not reversible -- take again to widen.
//
// The region-select flow is why it exists: a band is dragged over a
// frozen full-screen capture, exactly as Spectacle and GNOME do it, so
// the crop happens after the capture and not as a second one.
int ushot_crop(struct ushot *s, int x, int y, int w, int h);

// Writes the capture. `format` is "qoi" or "png", or NULL to take it
// from the path's extension. Returns 0 or a negative errno.
int ushot_save(const struct ushot *s, const char *path, const char *format);

// SHARING THE SCREEN (abi/win_proto.h's WIN_SHOT_DAMAGE). `px` becomes a
// MIRROR of the screen (screen_w per row) and only what the compositor
// repainted since the last call is copied into it; `out` lists those
// rects, n = 0 when nothing changed. The first call copies everything.
// WIN_SHOT_POINTER in `flags` draws the pointer in, and then its moves
// count as changes. 0 or a negative errno (-EBUSY: a fullscreen program
// has the display, or four screens are shared already) -- or
// USHOT_RESIZED: the screen changed size, nothing was copied, and the
// mirror has been re-made at the new `screen_w` x `screen_h` (so `px`
// MOVED); the next call copies the whole screen into it.
#define USHOT_RESIZED 1
int ushot_damage(struct ushot *s, unsigned flags, struct win_damage *out);

// The pointer's shape, 0xAARRGGBB, copied into `out` (at most `cap`
// pixels, 64x64 at most), with its size and hotspot. The capture
// buffer -- a ushot_damage() mirror included -- is left as it was.
int ushot_cursor(struct ushot *s, uint32_t *out, int cap, int *w, int *h, int *hot_x, int *hot_y);

// Drains the compositor's events; returns the WIN_CAST_* bits seen (what
// changed since the last damage or cursor capture). Never blocks.
int ushot_events(struct ushot *s);

void ushot_close(struct ushot *s);

// A sentence for a return code from any of the above -- including the
// ones whose errno alone would be misleading (-EBUSY is "a fullscreen
// program has the display", not "try again shortly").
const char *ushot_strerror(int rc);

#endif // ULIB_USHOT_H
