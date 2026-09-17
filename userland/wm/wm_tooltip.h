#ifndef WM_TOOLTIP_H
#define WM_TOOLTIP_H

#include <stdint.h>

// THE PANEL'S TOOLTIP: what the thing under the pointer is, in words,
// after the pointer has rested on it.
//
// WHY THE PANEL NEEDS ITS OWN. `uui_toolbar` already has one, and it is
// the right one for a ring-3 app: the widget rides the app's tick and
// opens a popup SURFACE the compositor places. The panel draws its
// overlays straight into the compositor's own buffer and has no widget
// tree and no surfaces of its own, so it cannot use that -- what it can
// use is its RULES, and those are copied here deliberately:
//
//   * IT TAKES NO INPUT. No grab, no hit region, no click. A tooltip
//     that took the pointer would eat the click the person was about to
//     make, which is the first thing every toolkit gets wrong.
//   * IT APPEARS ON A DELAY, not on arrival. Sliding a pointer across a
//     list must not strobe; the delay is what makes it a hint rather
//     than a flicker. `UUI_TOOLTIP_DELAY_TICKS` is the toolkit's, and
//     the panel uses the same number so the desktop does not feel like
//     two systems.
//   * A MOVE CANCELS IT. Moving to another row re-arms from zero rather
//     than swapping the text under a tooltip that is already up.
//
// WHAT REAL SYSTEMS DO. Windows shows a tooltip below-right of the
// pointer after ~500ms and hides it on any movement or click; KWin and
// GTK do the same with their own delays. Explorer shows one only when
// the text is TRUNCATED -- worth knowing, and deliberately not what
// this does: here the description is worth reading in full even when it
// fits, because it is the only place an app says what it is.

// Ask for a tooltip on a rect, with this text. Called every frame by
// whatever is hovering -- the same text and rect re-arms nothing, a
// DIFFERENT one starts the delay again, and `text == NULL` (or an empty
// one) cancels. That shape is what lets a caller simply say what the
// pointer is over each frame and never track state of its own.
void wm_tooltip_track(const char *text, int x, int y, int w, int h);

// Advance the delay and show or hide. Call once per wm_run() tick, like
// start_menu_update(); a no-op when nothing is being tracked.
void wm_tooltip_update(void);

// Hide it now, and forget what was being tracked -- for a caller whose
// surface is going away (the Start menu closing) rather than for the
// pointer merely moving off.
void wm_tooltip_cancel(void);

// The overlay registry's ops (wm_overlay.h). No click op and no hover
// op: it is not a control and it never lights up.
extern int wm_tooltip_open;
void wm_tooltip_draw(int mx, int my);
void wm_tooltip_damage(void);

// What it currently says, and where -- for `gui tooltip`, so a test can
// assert on the text rather than on pixels it would have to read twice.
// Returns 0 when nothing is showing.
int wm_tooltip_state(const char **text, int *x, int *y, int *w, int *h);

#endif
