#ifndef UUI_PROGRESS_H
#define UUI_PROGRESS_H

#include <stdint.h>
#include "ui/ugfx.h"

// A PROGRESS BAR: a track and a fill, determinate or busy.
//
// Not uui_meter, whose bar is one row of a READING (caption, value,
// unit): a progress bar is a control-height strip that stretches to its
// row and says how much of a job is done. QProgressBar and Windows'
// PROGRESS_CLASS are the shape -- a value in a fixed range, and a
// "marquee" mode for work whose length is not known yet.
//
// Takes no input (no `hit`): a click passes through, as on uui_meter.

struct uui_progress {
    int x, y, w, h;
    // PER MILLE, 0..1000, clamped -- no floating point in ring 3. A
    // NEGATIVE value is BUSY: a block that slides, for work whose total
    // is unknown (Windows' PBS_MARQUEE). Busy is not zero: an empty bar
    // says "nothing done yet", busy says "cannot tell".
    int value;
    int phase;      // OWNED: the busy block's position, advanced by _tick()
    uint32_t track, fill, border;   // from the theme at _init()
};

void uui_progress_init(struct uui_progress *p);
void uui_progress_set(struct uui_progress *p, int per_mille);
void uui_progress_set_busy(struct uui_progress *p);

// Advance the busy animation. Call from on_tick; returns 1 when the bar
// needs repainting (it is busy), 0 otherwise, so it composes with the
// app's own repaint decision.
int uui_progress_tick(struct uui_progress *p);

void uui_progress_natural_size(const struct uui_progress *p, int *out_w, int *out_h);
void uui_progress_draw(struct ugfx_surface *s, const struct uui_progress *p);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_progress_ops;

#endif
