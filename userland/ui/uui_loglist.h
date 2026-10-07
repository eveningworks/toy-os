#ifndef UUI_LOGLIST_H
#define UUI_LOGLIST_H
// uui_loglist -- a log's lines (lib/ulogset.h) as a list: Time, Level,
// Source and the message with its subsystem in bold, a severity drawn as
// a shape AND a theme colour, errors marked on the scrollbar, and an
// optional TIMELINE strip above it -- lines per bucket over the span,
// errors as ticks, and a range dragged out on it that the app filters to.
// macOS Console's list with a profiler's timeline; the Log Viewer is the
// first caller and Device Manager's Events the next.
//
// THE SET IS THE CALLER'S: this draws `set->view` and never filters. An
// app rebuilds the view, then calls uui_loglist_refresh(), which keeps
// the selection on the same LINE and, when following, shows the newest.
#include "ui/uui_widget.h"
#include "lib/ulogset.h"

// What changed since uui_loglist_take_changes() was last asked.
#define UUI_LOGLIST_SEL    0x01   // the selected line
#define UUI_LOGLIST_FOLLOW 0x02   // following stopped -- the reader scrolled up
#define UUI_LOGLIST_RANGE  0x04   // a range was dragged on the timeline, or cleared

struct uui_loglist {
    int x, y, w, h;
    const struct ulogset *set;
    int selected;       // a VIEW row, or -1
    int top;            // first visible view row
    int hovered;        // OWNED
    int focused;        // OWNED
    int follow;         // keep the newest row in view
    int timeline;       // draw the strip
    unsigned range_lo, range_hi;   // cs; both 0 = none
    int changes;        // UUI_LOGLIST_*
    // OWNED drag state: the thumb (grab offset, -1 none) or the timeline
    // brush (its anchor cs, -1 none).
    int thumb_grab;
    long brush_anchor;
    int bar_hover;
};

void uui_loglist_init(struct uui_loglist *l, const struct ulogset *set);
// After the set's view was rebuilt: the selection stays on the same line
// (or the nearest row), and a following list shows its end.
void uui_loglist_refresh(struct uui_loglist *l);
// The selected LINE (an index into set->lines), or -1.
int  uui_loglist_selected_line(const struct uui_loglist *l);
// Selects the view row showing `line` -- scrolled into view when `reveal`
// -- or clears the selection for -1. 0 if no row shows it. A caller that
// re-read its set finds the line again (the indices moved) and calls this.
int  uui_loglist_select_line(struct uui_loglist *l, int line, int reveal);
void uui_loglist_set_follow(struct uui_loglist *l, int on);
int  uui_loglist_take_changes(struct uui_loglist *l);
// The timeline strip's height, 0 when it is off.
int  uui_loglist_timeline_h(const struct uui_loglist *l);

void uui_loglist_natural_size(const void *w, int *out_w, int *out_h);
void uui_loglist_set_geometry(void *w, int x, int y, int width, int height);
void uui_loglist_draw(struct ugfx_surface *s, const void *w);

extern const struct uui_widget_ops uui_loglist_ops;

#endif
