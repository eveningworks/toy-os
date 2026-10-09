#ifndef UUI_MEDIALIST_H
#define UUI_MEDIALIST_H

// uui_medialist -- a PLAYLIST: one row per item, a 16:9 picture, a title
// with a detail line under it, and a length at the right. The Video
// Player's "Up next" panel; Windows Media Player's and Celluloid's list.
//
// IT OWNS NO DATA, as uui_table owns none: `text` fills a row's three
// strings while it draws, and `thumb` is a LOOKUP (lib/uthumb.h's
// uthumb_get(), which queues a miss and answers NULL -- a NULL draws a
// placeholder, so the list fills in as the decodes land).
//
// A CLICK ON A ROW PLAYS IT: armed on press, committed on the release
// over the same row, and parked for uui_medialist_take(); Enter commits
// the selected row the same way. `current` -- what is playing -- is the
// app's to set, and drawn in the selection's fill whether or not it is
// also selected. The wheel scrolls; Up/Down move the selection; the
// overlay scrollbar is the one every list here draws.
#include <stdint.h>
#include "ui/uui_widget.h"

struct uimg;

typedef void (*uui_medialist_text_fn)(void *ctx, int row, char *title, int tcap,
                                      char *detail, int dcap, char *right, int rcap);
typedef const struct uimg *(*uui_medialist_thumb_fn)(void *ctx, int row, int px);

struct uui_medialist {
    int x, y, w, h;
    int count;
    int selected;               // the keyboard's row, -1 for none
    int current;                // the row playing, -1 for none
    int scroll;                 // pixels scrolled
    uui_medialist_text_fn text;
    uui_medialist_thumb_fn thumb;
    void *ctx;
    uint32_t bg;                // the panel's ground
    // OWNED
    int hot, armed, committed;  // rows; -1 for none
    int focused;
    int bar_hot, bar_drag, bar_grab;
};

void uui_medialist_init(struct uui_medialist *m);
void uui_medialist_set(struct uui_medialist *m, int count, uui_medialist_text_fn text,
                       uui_medialist_thumb_fn thumb, void *ctx);
// Selects `row` and scrolls it into view.
void uui_medialist_select(struct uui_medialist *m, int row);
// The row a click or Enter chose since the last call, or -1.
int  uui_medialist_take(struct uui_medialist *m);
int  uui_medialist_row_h(void);

extern const struct uui_widget_ops uui_medialist_ops;

#endif
