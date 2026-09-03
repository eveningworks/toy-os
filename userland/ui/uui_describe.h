// uui_describe.h -- a widget REPORTS ITS OWN GEOMETRY, by name.
//
// The test-facing layout log (ui/uapp.h, uapp_log_layout()) used to be
// written by each app: a hand-rolled logger per app calling the
// widgets' rect accessors and printing them in its own vocabulary, ~60
// lines across seven apps, with the toolkit's own helper able to say
// only `layout <id-number> x y w h`. This is the shape Qt's QAccessible
// and GTK's AT-SPI use instead: every widget can DESCRIBE itself -- its
// bounds and the sub-rects a driver needs (a menu's titles and popup
// rows, a tab strip's slots, the picture inside an image box) -- and
// one toolkit walk emits them all under the app's names.
//
// THE VOCABULARY, one line each, content-relative:
//   <prefix>: layout <name> x y w h              the widget's bounds
//   <prefix>: layout <name>.<part> x y w h       one sub-rect
//   <prefix>: layout <name>.<part> i x y w h     the i-th of several
//   <prefix>: layout <name>.<part> i j x y w h   two indices (level, row)
//   <prefix>: layout <name>.<part> v             a scalar (a selected index)
// `name` is the app's, from uui_item.name; `part` is the widget's and
// is the same word for every app, which is what lets one test helper
// serve every tool.
//
// A widget's `describe` op emits through the callback in `d`, never by
// logging directly: the widgets are also linked into the compositor,
// which has no uapp, and the callback is what keeps this file free of
// one. It is also what puts every line through uapp's per-frame dedupe.
#ifndef UUI_DESCRIBE_H
#define UUI_DESCRIBE_H

struct uui_describe {
    void (*line)(void *ctx, const char *line);   // one line, newline included
    void *ctx;
    const char *prefix;   // the app's log prefix ("imgview")
    const char *name;     // the item's name ("menu")
};

void uui_describe_rect(const struct uui_describe *d, const char *part,
                       int x, int y, int w, int h);
void uui_describe_rect_i(const struct uui_describe *d, const char *part, int i,
                         int x, int y, int w, int h);
void uui_describe_rect_ij(const struct uui_describe *d, const char *part, int i, int j,
                          int x, int y, int w, int h);
void uui_describe_int(const struct uui_describe *d, const char *part, int v);

#endif
