#ifndef UUI_CONFETTI_H
#define UUI_CONFETTI_H

// A CONFETTI BURST: pieces thrown up from a point, tumbling and falling
// under gravity, fading out at the end -- what a game shows when it is
// won (Microsoft Minesweeper, Google's, a Solitaire cascade's cousin).
// Not a widget: a thing an app's own draw paints over its content, like
// uui_toast.
//
// TIME IS THE CALLER'S, as in lib/utween.h: start and draw take
// `now_ns`, and the motion is a function of elapsed time, never of
// frames, so a slow frame moves every piece further instead of slowing
// the burst. Integer only, like the rest of the desktop.
//
// While any piece is still in the air, uui_confetti_draw() asks for the
// next frame itself (ui/uui_anim.h); once the last one has faded it
// stops asking, so the window goes back to sitting still.

#include <stdint.h>
#include "ui/ugfx.h"

#define UUI_CONFETTI_MAX 72

struct uui_confetti_piece {
    int16_t vx, vy;      // px per second at launch
    uint8_t w, h;        // the piece, face on
    uint8_t spin;        // tumble, in tenths of a turn per second
    uint8_t colour;      // index into the theme palette the draw uses
    uint16_t delay_ms;   // launched this long after the burst starts
};

struct uui_confetti {
    struct uui_confetti_piece p[UUI_CONFETTI_MAX];
    int count;
    int ox, oy;                  // the launch point, surface coordinates
    int g;                       // gravity, px per second squared
    unsigned dur_ms;             // one piece's life; 0 = idle
    unsigned long long t0_ns;    // the burst's start, may be in the future
};

// Throws `count` pieces (clamped to UUI_CONFETTI_MAX) from (ox, oy),
// rising about `reach_px` and spreading about as far sideways, each
// living `dur_ms`. `dur_ms` 0 starts nothing: pass uui_anim_ms() so
// "animations off" means no burst. `t0_ns` may be in the future to
// launch after something else finishes.
void uui_confetti_start(struct uui_confetti *c, int ox, int oy, int reach_px,
                        int count, unsigned dur_ms, uint32_t seed,
                        unsigned long long t0_ns);

void uui_confetti_stop(struct uui_confetti *c);

// Paints the pieces in flight at `now_ns`, inside the surface's clip.
// Returns 1 while any piece is still to be drawn (and has asked for a
// frame), 0 once the burst is over.
int uui_confetti_draw(struct ugfx_surface *s, struct uui_confetti *c,
                      unsigned long long now_ns);

static inline int uui_confetti_active(const struct uui_confetti *c) {
    return c->dur_ms != 0;
}

#endif // UUI_CONFETTI_H
