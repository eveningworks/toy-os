#ifndef UUI_THUMBSTRIP_H
#define UUI_THUMBSTRIP_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"
#include "lib/uimg.h"

// uui_thumbstrip -- a row of thumbnails with one selected: a filmstrip,
// Windows Photos' and Gwenview's thumbnail bar.
//
// IT OWNS NO PICTURES. Each cell asks `thumb(ctx, index, px)` while it
// draws, and that callback must be a LOOKUP -- lib/uthumb.h's
// uthumb_get(), which queues a miss and answers NULL until the worker
// has it. A NULL draws a placeholder, so a strip fills in as the
// decodes land rather than waiting for all of them.
//
// THE SELECTION IS THE APP'S TOO: a click selects and parks the index
// for uui_thumbstrip_take(), the router naming the strip to the app on
// release; the app then shows that picture and calls _select() for
// keyboard moves. Left/Right are the app's, not the widget's -- they
// mean "next picture" whatever has the focus.
//
// FEW CELLS ARE CENTRED, MANY SCROLL: the wheel moves the row, and
// _select() scrolls the selected cell into view.
struct uui_thumbstrip {
    int x, y, w, h;
    int count;
    int selected;          // -1 for none
    int scroll;            // pixels the row is moved left, when it overflows
    int hot;               // hovered cell, or -1; OWNED
    int armed;             // pressed cell awaiting its release, or -1; OWNED
    int committed;         // clicked, for _take(); -1 when none
    const struct uimg *(*thumb)(void *ctx, int index, int px);
    void *ctx;
    // The ground, the selection ring and the hover ring. The defaults
    // suit a dark strip; an app tinting its stage sets its own.
    uint32_t bg, sel, hover, empty;
};

void uui_thumbstrip_init(struct uui_thumbstrip *t);
void uui_thumbstrip_set(struct uui_thumbstrip *t, int count,
                        const struct uimg *(*thumb)(void *ctx, int index, int px),
                        void *ctx);
// Selects `index` (or -1) and scrolls it into view.
void uui_thumbstrip_select(struct uui_thumbstrip *t, int index);
// The cell a click chose since the last call, or -1.
int uui_thumbstrip_take(struct uui_thumbstrip *t);
// A cell's rect, content-relative; 0 when it is not on screen.
int uui_thumbstrip_cell_rect(const struct uui_thumbstrip *t, int index,
                             int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_thumbstrip_ops;

#endif
