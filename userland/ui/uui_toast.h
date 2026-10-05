#ifndef UUI_TOAST_H
#define UUI_TOAST_H

// A TOAST: one short line in a dark pill that rises into view at the
// bottom of a box the app names, over its content -- Android's Toast,
// the "Solved in 0:37" note a game shows. Not a widget and takes no
// input: the app paints it last in its own draw, and hides it when
// whatever it describes is over. It does NOT time itself out, because
// a timer would keep a still window waking (docs/gui-guidelines.md, "An
// idle screen must SIT STILL").

#include "ui/ugfx.h"

struct uui_toast {
    char text[96];
    unsigned in_ms;              // the rise; 0 = it simply appears
    unsigned long long t0_ns;    // when the rise starts, may be in the future
    int shown;
};

// Shows `text` (truncated to fit `text`), rising over `in_ms` from
// `t0_ns`. Pass uui_anim_ms() for `in_ms`.
void uui_toast_show(struct uui_toast *t, const char *text, unsigned in_ms,
                    unsigned long long t0_ns);
void uui_toast_hide(struct uui_toast *t);

// Paints it centred along the bottom of (x, y, w, h), clipped to that
// box so it rises out of the box's bottom edge. Returns 1 while it is
// still moving (and has asked for a frame).
int uui_toast_draw(struct ugfx_surface *s, struct uui_toast *t,
                   int x, int y, int w, int h, unsigned long long now_ns);

// Where it sits once risen, for a test to ask rather than guess.
// 0 when hidden.
int uui_toast_rect(const struct uui_toast *t, int x, int y, int w, int h,
                   int *rx, int *ry, int *rw, int *rh);

#endif // UUI_TOAST_H
