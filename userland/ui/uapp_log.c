// uapp's layout log and the widget map `gui probe` reads -- see
// ui/uapp_internal.h for where the rest of uapp is.
#include "ui/uapp_internal.h"

// Fill a request's `text` from a NUL-terminated string, truncating to
// fit. Shared by the title and the app id -- both ride that one field
// (never at the same time) and both truncate rather than fail.
static void copy_text(char *dst, const char *src) {
    int i = 0;
    for (; src && src[i] && i < WIN_TITLE_LEN - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// --- the layout log ---------------------------------------------------
//
// **OFF UNLESS A TEST TURNS IT ON, AND DEDUPED WHEN IT IS.** This is the
// "log my geometry so a test can drive me by asking, not by guessing
// pixels" report, and it used to be written on EVERY FRAME,
// unconditionally, to the kernel log. Nine apps do it. The result was
// that `dmesg` on a machine with a window open was mostly one app
// repeating itself -- and `dmesg -w` was a FEEDBACK LOOP: printing a
// line moved the Terminal's caret, which redrew, which logged the new
// caret, which printed a line.
//
// `desktop.layout_log` gates it, off by default, the same call
// `kernel.kbdtap` makes: the cost of recording is trivial, and the
// default is about what the machine SHOWS.
//
// READ ONCE, at first use. A test sets the setting before launching the
// app it intends to watch, which tools/gui_debug.py's enter_gui() does
// for every tool at once. Re-reading per frame would put a syscall on
// the draw path to answer a question whose answer does not change
// during a run.
static int g_layout_log = -1;   // -1 = not yet asked

int layout_log_enabled(void) {
    if (g_layout_log < 0) {
        // The qualified name: identity is (namespace, name), and the
        // namespace is the registered name of the file it lives in.
        //
        // **usetting_get(), NOT sys_setting().** This setting is
        // DECLARED by /etc/settings.d/desktop.layout_log, so the
        // syscall -- which answers for the kernel's registry alone --
        // reads it as unset and silently turns layout logging off for
        // every GUI tool that depends on it. Fourteen of them failed at
        // once that way.
        char value[SETTING_ABI_VALUE_MAX];
        g_layout_log = (usetting_get("desktop.layout_log", value, sizeof value)
                        && value[0] == 'o' && value[1] == 'n') ? 1 : 0;
    }
    return g_layout_log;
}

// THE BLOCK A FRAME PRODUCES, held so it can be compared with the last
// one. Deduping LINE BY LINE would not work: an app logs several lines
// per frame, and each differs from the line before it, so nothing would
// ever match. What repeats is the whole block, which is exactly what an
// idle window emits over and over.
//
// **AN OVERFLOW SAYS SO.** Dropping what does not fit is silent, and
// what it drops is whatever an app logs LAST -- so a test asserting on
// those lines reads a working app as broken (it did). The marker's room
// is reserved so it cannot itself be the line that will not fit, and it
// goes INSIDE the block so the dedupe covers it: outside, it would
// print every frame.
#define LAYOUT_BLOCK_MAX 8192
#define LAYOUT_BLOCK_MARK "uapp: layout log TRUNCATED -- raise LAYOUT_BLOCK_MAX\n"
#define LAYOUT_BLOCK_USABLE (LAYOUT_BLOCK_MAX - (int)sizeof(LAYOUT_BLOCK_MARK))
static char g_block[LAYOUT_BLOCK_MAX];
static int  g_block_len;
static int  g_block_over;
// THE DEDUPE IS PER SURFACE. A dialog window draws its own frame after
// the toplevel's, so one shared "previous block" would see two different
// reports alternating and never match -- the dedupe would be off exactly
// when two windows are open. Source 0 is the toplevel, 1 any dialog.
#define LAYOUT_LOG_SOURCES 2
static char g_block_prev[LAYOUT_LOG_SOURCES][LAYOUT_BLOCK_MAX];
static int  g_block_prev_len[LAYOUT_LOG_SOURCES];

// The formatted form, which is what every app's own report uses. It
// exists so those lines go through the SAME gate and the same per-frame
// dedupe as the widget walk -- a line written straight to ulogf() is
// one this cannot suppress, and it was about twenty such lines a frame
// that made `dmesg` useless.
void uapp_logf_layout(const char *fmt, ...) {
    if (!layout_log_enabled()) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    uapp_log_layout_line(line);
}

void uapp_log_layout_line(const char *line) {
    if (!layout_log_enabled()) return;
    for (const char *p = line; *p; p++) {
        if (g_block_len >= LAYOUT_BLOCK_USABLE) { g_block_over = 1; return; }
        g_block[g_block_len++] = *p;
    }
}

// Called by the draw path once the app has finished. Emits the block
// only if it differs from the previous frame's.
void layout_log_flush(int src) {
    if (!g_layout_log || !g_block_len) { g_block_len = g_block_over = 0; return; }
    char *prev = g_block_prev[src];
    int *prev_len = &g_block_prev_len[src];
    if (g_block_over) {
        for (const char *p = LAYOUT_BLOCK_MARK; *p; p++) g_block[g_block_len++] = *p;
        g_block_over = 0;
    }
    int same = (g_block_len == *prev_len);
    for (int i = 0; same && i < g_block_len; i++)
        if (g_block[i] != prev[i]) same = 0;
    if (!same) {
        g_block[g_block_len] = '\0';
        ulog(g_block);
        for (int i = 0; i < g_block_len; i++) prev[i] = g_block[i];
        *prev_len = g_block_len;
    }
    g_block_len = 0;
}

// The layout walk (ui/uui_describe.h has the vocabulary). Every NAMED
// item with a `bounds` op gets `<prefix>: layout <name> x y w h`, then
// whatever its `describe` op adds; containers are entered through
// `children`, so a widget inside a scroll view or a splitter is reported
// like any other. Content-relative; a test adds the window's content
// origin. Unnamed and hidden items are skipped -- a name is what says
// "a test may want this".
static void describe_line(void *ctx, const char *line) {
    (void)ctx;
    uapp_log_layout_line(line);
}

// --- the widget map the compositor answers `gui probe` from ----------
//
// **THE COMPOSITOR CANNOT SEE INSIDE A WINDOW**, so a test could ask it
// what window and what chrome were under a point and never what CONTROL
// was. The client exports its own map instead -- AT-SPI's shape, which
// is also what uui_describe.h is modelled on (abi/win_proto.h's
// WIN_REQ_WIDGET has the reasoning).
//
// SENT ONLY WHEN IT CHANGES. The walk runs every frame because it is a
// few dozen pointer derefs and no formatting; what it produces is
// compared against the last set and the messages go out only on a
// difference, which in practice is a resize or a layout swap. That is
// what keeps this off the per-frame cost of an app that redraws
// constantly, and out of the LOG -- the string-building layout log is
// still opt-in behind `desktop.layout_log`, and deliberately separate.
struct wmap_entry { char name[WIN_TITLE_LEN]; int x, y, w, h; };
static struct wmap_entry g_wmap[WIN_WIDGET_MAX];
static int g_wmap_n;
static struct wmap_entry g_wmap_sent[WIN_WIDGET_MAX];
static int g_wmap_sent_n = -1;   // -1: nothing sent yet, so the first
                                 // walk always reports

static void wmap_add(const char *name, int x, int y, int w, int h) {
    if (g_wmap_n >= WIN_WIDGET_MAX) return;
    struct wmap_entry *e = &g_wmap[g_wmap_n++];
    copy_text(e->name, name);
    e->x = x; e->y = y; e->w = w; e->h = h;
}

// A widget reachable both through `.layout` and through `.widgets` (the
// usual shape: the tree draws it, the flat list routes it) is reported
// ONCE -- the second sighting is skipped, not re-logged.
#define LOG_SEEN_MAX 128
static const void *g_log_seen[LOG_SEEN_MAX];
int g_log_seen_n;

static int log_seen(const void *w) {
    for (int i = 0; i < g_log_seen_n; i++) if (g_log_seen[i] == w) return 1;
    if (g_log_seen_n < LOG_SEEN_MAX) g_log_seen[g_log_seen_n++] = w;
    return 0;
}

void log_items(const char *prefix, struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (it->hidden || !it->ops) continue;
        if (it->name && it->ops->bounds && !log_seen(it->widget)) {
            int x, y, w, h;
            it->ops->bounds(it->widget, &x, &y, &w, &h);
            char line[96];
            snprintf(line, sizeof line, "%s: layout %s %d %d %d %d\n",
                     prefix, it->name, x, y, w, h);
            uapp_log_layout_line(line);
            if (it->ops->describe) {
                struct uui_describe d = { describe_line, 0, prefix, it->name };
                it->ops->describe(it->widget, &d);
            }
        }
        if (it->ops->children) {
            int n = 0;
            struct uui_item *sub = it->ops->children(it->widget, &n);
            if (sub) log_items(prefix, sub, n);
        }
    }
}

static void wmap_items(struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (it->hidden || !it->ops) continue;
        if (it->name && it->ops->bounds && !log_seen(it->widget)) {
            int x, y, w, h;
            it->ops->bounds(it->widget, &x, &y, &w, &h);
            wmap_add(it->name, x, y, w, h);
        }
        if (it->ops->children) {
            int n = 0;
            struct uui_item *sub = it->ops->children(it->widget, &n);
            if (sub) wmap_items(sub, n);
        }
    }
}

// Walk, compare, and report only a change. Called from the draw path.
void wmap_sync(struct uapp *a) {
    const struct uapp_desc *d = a->desc;
    if (!d) return;
    g_wmap_n = 0;
    g_log_seen_n = 0;
    if (d->layout) {
        struct uui_item root = { .ops = &uui_layout_ops, .widget = (void *)d->layout };
        wmap_items(&root, 1);
    }
    if (d->widgets && d->widget_count) wmap_items(d->widgets, d->widget_count);

    int same = (g_wmap_n == g_wmap_sent_n);
    for (int i = 0; same && i < g_wmap_n; i++) {
        const struct wmap_entry *x = &g_wmap[i], *y = &g_wmap_sent[i];
        if (x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h ||
            strcmp(x->name, y->name) != 0) same = 0;
    }
    if (same) return;

    wmchan_send(WIN_REQ_WIDGET_RESET, a->window, 0, 0, 0, 0);
    for (int i = 0; i < g_wmap_n; i++) {
        const struct wmap_entry *e = &g_wmap[i];
        wmchan_send(WIN_REQ_WIDGET, a->window, e->x, e->y,
                    WIN_WIDGET_WH(e->w, e->h), e->name);
        g_wmap_sent[i] = *e;
    }
    g_wmap_sent_n = g_wmap_n;
}

void uapp_log_widget(struct uapp *a, const char *prefix, const char *name,
                     const struct uui_widget_ops *ops, const void *widget) {
    (void)a;
    if (!layout_log_enabled() || !ops || !ops->bounds) return;
    int x, y, w, h;
    ops->bounds(widget, &x, &y, &w, &h);
    char line[96];
    snprintf(line, sizeof line, "%s: layout %s %d %d %d %d\n", prefix, name, x, y, w, h);
    uapp_log_layout_line(line);
    if (ops->describe) {
        struct uui_describe d = { describe_line, 0, prefix, name };
        ops->describe(widget, &d);
    }
}

void uapp_log_layout(struct uapp *a, const char *prefix) {
    if (!layout_log_enabled()) return;
    g_log_seen_n = 0;
    if (a->desc->layout) log_items(prefix, a->desc->layout->items, a->desc->layout->count);
    if (a->top.router.count) log_items(prefix, a->top.router.items, a->top.router.count);
}

