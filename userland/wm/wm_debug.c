// See wm_debug.h for what this is for and why it lives here.
#include "wm_internal.h"
#include "wm_taskbar.h"
#include "lib/icon_cache.h"
#include "wm_tray.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "wm_overlay.h"
#include "osk.h"
#include "wm_debug.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "gui_apps.h"
#include "kapi.h"
#include "win_server.h"
#include "win_events.h"
#include <stdarg.h> // dbg_out_printf()'s varargs
#include "rt/sys.h"
#include "wm/wm_rawin.h"

// ---------------------------------------------------------------------
// Synthetic input queue
// ---------------------------------------------------------------------

// Deep enough for the longest sequence any single command enqueues -- a
// drag is move, press, N interpolated held moves, release. Overflow
// drops the tail and says so rather than wrapping silently, because a
// half-delivered drag looks like a WM bug rather than a full queue.
#define INJECT_MAX 64

struct inject_event { int x, y; uint8_t buttons; };
static struct inject_event g_inject[INJECT_MAX];
static int g_inject_head = 0, g_inject_tail = 0;

#define KEY_INJECT_MAX 32
static int g_keys[KEY_INJECT_MAX];
static uint8_t g_key_mods[KEY_INJECT_MAX]; // KEY_MOD_* bits, parallel to g_keys
static int g_keys_head = 0, g_keys_tail = 0;

static int g_wheel[KEY_INJECT_MAX];
static int g_wheel_head = 0, g_wheel_tail = 0;

// Anything queued from the debug console that the frame loop consumes
// ONE OF PER ITERATION. The loop blocks between frames now, and an
// injected press whose release is still in the queue would wait for a
// wake that is never coming -- the queue is ring-3 memory the kernel
// knows nothing about, so pushing to it wakes nobody.
int wm_debug_work_pending(void) {
    return g_inject_head != g_inject_tail ||
           g_keys_head   != g_keys_tail   ||
           g_wheel_head  != g_wheel_tail;
}

static int inject_push(int x, int y, uint8_t buttons) {
    int next = (g_inject_tail + 1) % INJECT_MAX;
    if (next == g_inject_head) return 0; // full
    g_inject[g_inject_tail].x = x;
    g_inject[g_inject_tail].y = y;
    g_inject[g_inject_tail].buttons = buttons;
    g_inject_tail = next;
    return 1;
}

int wm_debug_next_input(int *out_x, int *out_y, uint8_t *out_buttons) {
    if (g_inject_head == g_inject_tail) return 0;
    *out_x = g_inject[g_inject_head].x;
    *out_y = g_inject[g_inject_head].y;
    *out_buttons = g_inject[g_inject_head].buttons;
    g_inject_head = (g_inject_head + 1) % INJECT_MAX;
    return 1;
}

// How many injected events (of any kind) are still undelivered.
//
// Exists so a test can WAIT for its input to drain instead of sleeping a
// guessed interval. The guess was wrong: gui_debug.py's settle() assumed
// one event per WM frame at 100Hz, i.e. ~110ms for a drag's eleven, and
// a drag measured at ~800ms -- because `gui damage verify on` renders
// every frame twice and diffs the whole screen, so the loop runs nowhere
// near 100Hz while it's on. A fixed sleep therefore raced: windows moved
// between a test's `gui windows` and its next command, and the damage
// exerciser reported a different bug on each run of the same script.
// Anything derived from frame rate is a guess; the queue depth is a fact.
int wm_debug_input_pending(void) {
    int mouse = (g_inject_tail - g_inject_head + INJECT_MAX) % INJECT_MAX;
    int keys = (g_keys_tail - g_keys_head + KEY_INJECT_MAX) % KEY_INJECT_MAX;
    int wheel = (g_wheel_tail - g_wheel_head + KEY_INJECT_MAX) % KEY_INJECT_MAX;
    return mouse + keys + wheel;
}

int wm_debug_next_wheel(void) {
    if (g_wheel_head == g_wheel_tail) return 0;
    int d = g_wheel[g_wheel_head];
    g_wheel_head = (g_wheel_head + 1) % KEY_INJECT_MAX;
    return d;
}

int wm_debug_next_key(void) { return wm_debug_next_key_mods(0); }

int wm_debug_next_key_mods(uint8_t *out_mods) {
    if (g_keys_head == g_keys_tail) return 0;
    int k = g_keys[g_keys_head];
    if (out_mods) *out_mods = g_key_mods[g_keys_head];
    g_keys_head = (g_keys_head + 1) % KEY_INJECT_MAX;
    return k;
}

// ---------------------------------------------------------------------
// The output sink
// ---------------------------------------------------------------------
//
// See wm_debug.h for why a caller-supplied sink rather than a klog
// redirect. Everything below writes through these two and nothing in
// this file calls sys_eprint() any more -- which is the property that
// keeps a `gui spawn`'s ELF-loader log lines on the serial port where
// tests read them, instead of inside the reply.

void dbg_out_write(struct dbg_out *o, const char *s) {
    if (!o || !o->buf || !s) return;
    int limit = o->cap - o->reserve;
    while (*s) {
        if (o->len + 1 >= limit) { o->overflow = 1; break; }
        o->buf[o->len++] = *s++;
    }
    o->buf[o->len] = '\0';
}

int dbg_out_mark(const struct dbg_out *o) { return o ? o->len : 0; }

void dbg_out_rollback(struct dbg_out *o, int mark) {
    if (!o || !o->buf || mark < 0 || mark > o->len) return;
    o->len = mark;
    o->buf[o->len] = '\0';
}

void dbg_out_reserve(struct dbg_out *o, int bytes) {
    if (!o) return;
    if (bytes < 0) bytes = 0;
    if (bytes > o->cap - 1) bytes = o->cap - 1;
    o->reserve = bytes;
}

void dbg_out_printf(struct dbg_out *o, const char *fmt, ...) {
    // Formats into a line buffer first, then appends -- k_vsnprintf()
    // needs somewhere contiguous, and the sink's remaining space is not
    // guaranteed to be big enough to format into directly.
    char line[KFMT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg_out_write(o, line);
}

// ---------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------

// Pulls the next whitespace-separated token off `*p`, NUL-terminating
// it in place and advancing `*p` past it. Returns NULL at end of line.
static char *next_tok(char **p) {
    char *s = *p;
    while (k_isblank(*s)) s++;
    if (!*s) { *p = s; return 0; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = '\0'; s++; }
    *p = s;
    return start;
}

// A signed integer argument. Rejects rather than guesses, matching
// kernel/lib's parser convention (see CLAUDE.md) -- a bad coordinate
// silently becoming 0 would put a click in the top-left corner and look
// like a hit-testing bug.
// Decimal, optionally signed -- or hex with an explicit "0x"/"0X"
// prefix.
//
// The hex half is not decoration: cmd_key() below has always documented
// `gui key 0x1b` as the way to send an unprintable key, and every KEY_*
// code in api/keyboard.h is written in hex, but k_parse_u32() takes
// plain decimal only ("no prefix", per its own contract) -- so every
// such command was answered with "bad or dropped key" while the help
// text advertised it. Found by a test that typed the arrow-key codes
// exactly as this file's own comment said to.
static int parse_int(const char *s, int *out) {
    if (!s || !*s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; if (!*s) return 0; }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint64_t hv = 0;
        if (!k_parse_hex(s, &hv)) return 0;
        *out = sign * (int)hv;
        return 1;
    }

    uint32_t v = 0;
    if (!k_parse_u32(s, &v)) return 0;
    *out = sign * (int)v;
    return 1;
}

// `--json` may appear anywhere in the remaining arguments.
static int wants_json(char *rest) {
    char *p = rest, *t;
    while ((t = next_tok(&p)) != 0) {
        if (k_strcmp(t, "--json") == 0) return 1;
    }
    return 0;
}

// kfmt's printf supports a zero-pad width (%04x) but NOT left-justify
// (%-4d) or a string width (%-14s) -- see api/kfmt.h. Passing one
// prints the specifier literally AND desyncs every argument after it,
// which is how the first version of this file emitted rows reading
// "y=%-4d centre=%-4d  app  <garbage>". These two do the padding
// explicitly instead of pretending the formatter can.
static void col_str(struct dbg_out *o, const char *s, int width) {
    int n = 0;
    if (s) { dbg_out_write(o, s); n = (int)k_strlen(s); }
    for (int i = n; i < width; i++) dbg_out_write(o, " ");
}

static void col_int(struct dbg_out *o, int v, int width) {
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", v);
    col_str(o, buf, width);
}

static const char *state_name(enum window_state s) {
    switch (s) {
        case WIN_MINIMIZED: return "minimized";
        case WIN_MAXIMIZED: return "maximized";
        default:            return "normal";
    }
}

// ---------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------

// NOTE for anything added below: dbg_out_printf() formats into a
// KFMT_LINE_MAX (256) byte buffer and whatever doesn't fit is simply
// lost. A single call emitting a whole JSON object overflows that
// quietly -- `gui state --json` did, and the reply came back as valid
// JSON right up to a severed string. Emit long output as several
// calls, each comfortably under the limit, rather than one big one.
//
// windows[] is stored back-to-front: index 0 is the bottom of the
// z-order, window_count-1 the top (and therefore the focused one -- see
// wm.c's bring_to_front()). Reported explicitly rather than left for
// the reader to know.
static void cmd_windows(struct dbg_out *o, int json) {
    if (json) {
        dbg_out_write(o, "{\"count\":");
        dbg_out_printf(o, "%d,\"focused\":%d,\"windows\":[", window_count,
                     window_count > 0 ? window_count - 1 : -1);
        // Enough for `],"listed":NNN,"truncated":true}` whatever happens
        // in the loop -- see dbg_out_reserve(). `count` above is the real
        // total, so a caller can always tell how much it is missing.
        dbg_out_reserve(o, 48);
        int listed = 0;
        for (int i = 0; i < window_count; i++) {
            if (o->overflow) break;
            int mark = dbg_out_mark(o);
            const struct window *w = &windows[i];
            dbg_out_printf(o, "%s{\"z\":%d,\"title\":\"%s\",\"app\":\"%s\",",
                         i ? "," : "", i, w->title,
                         w->app ? w->app->name : "");
            dbg_out_printf(o, "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,",
                         w->x, w->y, w->w, w->h);
            dbg_out_printf(o, "\"content\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},",
                         window_content_x(w), window_content_y(w),
                         window_content_w(w), window_content_h(w));
            // client_pid is 0 for a kernel-space window and the owning
            // process for a ring-3 one -- which ring a window belongs to
            // is otherwise only visible in the Task Manager's own text,
            // so a test asserting "this really is a ring-3 client" would
            // have nothing to read.
            dbg_out_printf(o, "\"client_pid\":%d,", w->client_pid);
            // The TITLE-BAR ICON'S OWN RECT, not a formula a tool can
            // re-derive: which square is drawn AND which square is
            // clickable is one answer (title_icon(), see
            // wm_internal.h), so a test reading it cannot drift from
            // what the compositor actually did. null when this window
            // shows no icon -- no app_id, no matching .desktop entry,
            // or no artwork on disk.
            {
                int ix, iy, isz;
                if (title_icon(i, &ix, &iy, &isz)) {
                    dbg_out_printf(o, "\"icon\":{\"name\":\"%s\",\"x\":%d,\"y\":%d,\"size\":%d},",
                                 wm_window_icon_name(i), ix, iy, isz);
                } else {
                    dbg_out_write(o, "\"icon\":null,");
                }
            }
            // The FLAG, not the decorated title: wm_render.c appends
            // "(Not Responding)" at draw time, so the title here is the
            // client's own and a test looking for the suffix in it finds
            // nothing. Report the fact and let the test assert on that.
            dbg_out_printf(o, "\"not_responding\":%s,", w->not_responding ? "true" : "false");
            dbg_out_printf(o, "\"state\":\"%s\",\"focused\":%s,\"resizable\":%s}",
                         state_name(w->state),
                         (i == window_count - 1) ? "true" : "false",
                         w->resizable ? "true" : "false");
            // ALL OR NOTHING: a half-written element in front of the
            // closing bracket is as unparseable as no bracket at all.
            if (o->overflow) { dbg_out_rollback(o, mark); break; }
            listed++;
        }
        int cut = o->overflow;
        o->overflow = 0;          // the ending is not a failure to write
        dbg_out_reserve(o, 0);
        dbg_out_printf(o, "],\"listed\":%d,\"truncated\":%s}\r\n",
                     listed, cut ? "true" : "false");
        return;
    }

    dbg_out_printf(o, "%d window(s), z-order bottom to top; the top one has focus\r\n",
                 window_count);
    if (window_count == 0) return;
    dbg_out_write(o, "  z  title           x    y    w    h  | content x/y/w/h    state\r\n");
    for (int i = 0; i < window_count; i++) {
        const struct window *w = &windows[i];
        dbg_out_write(o, "  ");
        col_int(o, i, 3);
        col_str(o, w->title, 16);
        col_int(o, w->x, 5); col_int(o, w->y, 5); col_int(o, w->w, 5); col_int(o, w->h, 5);
        dbg_out_write(o, "| ");
        col_int(o, window_content_x(w), 5); col_int(o, window_content_y(w), 5);
        col_int(o, window_content_w(w), 5); col_int(o, window_content_h(w), 5);
        dbg_out_write(o, " ");
        dbg_out_write(o, state_name(w->state));
        dbg_out_write(o, (i == window_count - 1) ? " (focused)\r\n" : "\r\n");
    }
}

// What is at this point? Answers the question a failed click actually
// raises -- "did my coordinate land where I thought" -- without a
// screenshot. Regions are named the same way wm_input.c's hit-testing
// thinks about them, so the answer maps onto the code that would run.
static void cmd_probe(struct dbg_out *o, int px, int py, int json) {
    const char *region = "desktop";
    int hit = -1;

    // Topmost first, matching wm_handle_left_click()'s own order.
    for (int i = window_count - 1; i >= 0; i--) {
        const struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (px < w->x || px >= w->x + w->w || py < w->y || py >= w->y + w->h) continue;

        hit = i;
        struct btn_rects r = title_buttons(w);
        if (py < w->y + WM_TITLEBAR_H) {
            if (px >= r.close_x && px < r.close_x + r.size) region = "close-button";
            else if (px >= r.max_x && px < r.max_x + r.size) region = "maximize-button";
            else if (px >= r.min_x && px < r.min_x + r.size) region = "minimize-button";
            else region = "title-bar";
        } else if (px >= w->x + w->w - RESIZE_MARGIN || py >= w->y + w->h - RESIZE_MARGIN) {
            region = "resize-edge";
        } else {
            region = "content";
        }
        break;
    }

    // Overlays are checked after, and reported separately, because they
    // sit ABOVE every window and would swallow the click regardless of
    // what the window hit-test above says.
    const char *overlay = "none";
    if (confirm_dialog_open)   overlay = "confirm-dialog";
    else if (file_picker_open) overlay = "file-picker";
    else if (context_menu_open) overlay = "context-menu";
    else if (start_menu_open)  overlay = "start-menu";
    else if (py >= screen_h - taskbar_h) overlay = "taskbar";

    if (json) {
        dbg_out_printf(o, "{\"x\":%d,\"y\":%d,\"window\":%d,\"title\":\"%s\","
                     "\"region\":\"%s\",\"overlay\":\"%s\"", px, py, hit,
                     hit >= 0 ? windows[hit].title : "", region, overlay);
        if (hit >= 0) {
            const struct window *w = &windows[hit];
            dbg_out_printf(o, ",\"content_rel\":{\"x\":%d,\"y\":%d}",
                         px - window_content_x(w), py - window_content_y(w));
        }
        dbg_out_write(o, "}\r\n");
        return;
    }

    dbg_out_printf(o, "(%d,%d): %s", px, py, region);
    if (hit >= 0) {
        const struct window *w = &windows[hit];
        dbg_out_printf(o, " of window %d \"%s\"; content-relative (%d,%d)",
                     hit, w->title,
                     px - window_content_x(w), py - window_content_y(w));
    }
    if (k_strcmp(overlay, "none") != 0) {
        dbg_out_printf(o, "  [%s is above everything and would take this click]", overlay);
    }
    dbg_out_write(o, "\r\n");
}

// Start menu rows, as the kernel computes them -- the numbers
// tools/gui_flow.py used to hardcode.
// The open right-click menu's rows, in the same shape cmd_menu(o, ) reports
// the Start menu's. Without this a test cannot reach a context-menu row
// at all except by re-deriving its position, which this project's own
// rules forbid -- and the one thing that most needed testing there was
// the Close row, which used to skip a client's close handshake.
static void cmd_ctxmenu(struct dbg_out *o, int json) {
    int x = 0, y = 0, w = 0, ih = 0;
    int rows = context_menu_geometry(&x, &y, &w, &ih);

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"item_h\":%d,\"rows\":[",
                     rows ? "true" : "false", x, y, w, ih);
        for (int i = 0; i < rows; i++) {
            dbg_out_printf(o, "%s{\"label\":\"%s\",\"y\":%d,\"cy\":%d}",
                         i ? "," : "", context_menu_row_label(i),
                         y + i * ih, y + i * ih + ih / 2);
        }
        dbg_out_write(o, "]}\r\n");
        return;
    }

    if (!rows) { dbg_out_write(o, "context menu: closed\r\n"); return; }
    dbg_out_printf(o, "context menu: open, x=%d y=%d w=%d item_h=%d rows=%d\r\n",
                 x, y, w, ih, rows);
    for (int i = 0; i < rows; i++) {
        dbg_out_write(o, "  row "); col_int(o, i, 3);
        dbg_out_write(o, "centre="); col_int(o, y + i * ih + ih / 2, 6);
        dbg_out_write(o, context_menu_row_label(i));
        dbg_out_write(o, "\r\n");
    }
}

// The open confirm dialog's message and buttons.
static void cmd_dialog(struct dbg_out *o, int json) {
    const char *msg = confirm_dialog_message();
    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"message\":\"%s\",\"buttons\":[",
                     msg ? "true" : "false", msg ? msg : "");
        for (int i = 0; i < 2; i++) {
            int x, y, w, h; const char *label = 0;
            if (!confirm_dialog_button_rect(i, &x, &y, &w, &h, &label)) break;
            dbg_out_printf(o, "%s{\"label\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                         "\"cx\":%d,\"cy\":%d}",
                         i ? "," : "", label ? label : "", x, y, w, h,
                         x + w / 2, y + h / 2);
        }
        dbg_out_write(o, "]}\r\n");
        return;
    }
    if (!msg) { dbg_out_write(o, "dialog: closed\r\n"); return; }
    dbg_out_printf(o, "dialog: open -- \"%s\"\r\n", msg);
    for (int i = 0; i < 2; i++) {
        int x, y, w, h; const char *label = 0;
        if (!confirm_dialog_button_rect(i, &x, &y, &w, &h, &label)) break;
        dbg_out_printf(o, "  button %d centre=%d,%d  %s\r\n", i, x + w / 2, y + h / 2,
                     label ? label : "");
    }
}

static void cmd_menu(struct dbg_out *o, int json) {
    int mx, my, mw, item_h, total;
    start_menu_geometry(&mx, &my, &mw, &item_h, &total);

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"item_h\":%d,\"rows\":[",
                     start_menu_open ? "true" : "false", mx, my, mw, item_h);
        int app_rows = gui_app_visible_count(GUI_SHOW_STARTMENU);
        for (int i = 0; i < app_rows; i++) {
            dbg_out_printf(o, "%s{\"label\":\"%s\",\"kind\":\"app\",\"y\":%d,\"cy\":%d}",
                         i ? "," : "",
                         gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name,
                         my + i * item_h, my + i * item_h + item_h / 2);
        }
        for (int i = 0; i < wm_system_action_count; i++) {
            int row = app_rows + i;
            dbg_out_printf(o, ",{\"label\":\"%s\",\"kind\":\"action\",\"y\":%d,\"cy\":%d}",
                         wm_system_actions[i].label,
                         my + row * item_h, my + row * item_h + item_h / 2);
        }
        dbg_out_write(o, "]}\r\n");
        return;
    }

    dbg_out_printf(o, "start menu: %s, x=%d y=%d w=%d item_h=%d rows=%d\r\n",
                 start_menu_open ? "open" : "closed", mx, my, mw, item_h, total);
    int app_rows = gui_app_visible_count(GUI_SHOW_STARTMENU);
    for (int i = 0; i < app_rows; i++) {
        dbg_out_write(o, "  row "); col_int(o, i, 3);
        dbg_out_write(o, "y="); col_int(o, my + i * item_h, 6);
        dbg_out_write(o, "centre="); col_int(o, my + i * item_h + item_h / 2, 6);
        dbg_out_write(o, "app     ");
        dbg_out_write(o, gui_app_visible_at(GUI_SHOW_STARTMENU, i)->name);
        dbg_out_write(o, "\r\n");
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        int row = app_rows + i;
        dbg_out_write(o, "  row "); col_int(o, row, 3);
        dbg_out_write(o, "y="); col_int(o, my + row * item_h, 6);
        dbg_out_write(o, "centre="); col_int(o, my + row * item_h + item_h / 2, 6);
        dbg_out_write(o, "action  "); dbg_out_write(o, wm_system_actions[i].label);
        dbg_out_write(o, "\r\n");
    }
}

// Taskbar: the Start button and one button per open window, with the
// rects wm_render.c actually draws and wm_input.c actually hit-tests.
// The strip's live layout, from the SAME taskbar_layout() that draws it
// and hit-tests it (wm_taskbar.h).
//
// This used to be a fourth, independent walk of the windows -- and it
// was already wrong: it placed button 0 at `sw + 4` where the real one
// sat at `4 + sw + 8`, so every centre this reported, and every test
// click aimed at one, was 8 pixels left of the button. A report a test
// trusts has to come from the code under test, not from a copy of it.
//
// `count` is how many windows a button stands for (>1 = a collapsed
// group) and `hidden` is how many windows did not fit on the strip at
// all -- which is the number a test asserts is zero.
static void cmd_taskbar(struct dbg_out *o, int json) {
    int bar_y = screen_h - taskbar_h;
    int sw = start_btn_w();
    char start_mark_buf[48];
    static struct taskbar_button btns[64];
    int nb = taskbar_layout(btns, 64);

    if (json) {
        // The MARK's own rect goes in beside the button's, from the same
        // start_icon() draw_taskbar() blits with -- null in `text` mode
        // and when the artwork is missing, which is exactly when the
        // button falls back to the word. A test reads this rather than
        // re-deriving a centred position and drifting from it.
        {
            int mx, my, msz;
            if (start_icon(&mx, &my, &msz)) {
                k_snprintf(start_mark_buf, sizeof start_mark_buf,
                           "{\"x\":%d,\"y\":%d,\"size\":%d}", mx, my, msz);
            } else {
                k_strlcpy(start_mark_buf, "null", sizeof start_mark_buf);
            }
        }
        dbg_out_printf(o, "{\"y\":%d,\"h\":%d,\"start\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                     "\"cx\":%d,\"cy\":%d,\"mark\":%s},\"tray_x\":%d,\"hidden\":%d,\"buttons\":[",
                     bar_y, taskbar_h, 0, bar_y, sw, taskbar_h,
                     sw / 2, bar_y + taskbar_h / 2, start_mark_buf,
                     tray_left(), taskbar_hidden());
        dbg_out_reserve(o, 48); // room for the ending -- see cmd_windows()
        int listed = 0;
        for (int i = 0; i < nb; i++) {
            if (o->overflow) break;
            int mark = dbg_out_mark(o);
            dbg_out_printf(o, "%s{\"index\":%d,\"title\":\"%s\",\"app_id\":\"%s\","
                         "\"label\":\"%s\",\"x\":%d,\"w\":%d,"
                         "\"count\":%d,\"cx\":%d,\"cy\":%d}",
                         i ? "," : "", btns[i].first, windows[btns[i].first].title,
                         windows[btns[i].first].app_id, btns[i].label,
                         btns[i].x, btns[i].w, btns[i].count,
                         btns[i].x + btns[i].w / 2, bar_y + taskbar_h / 2);
            if (o->overflow) { dbg_out_rollback(o, mark); break; }
            listed++;
        }
        int cut = o->overflow;
        o->overflow = 0;
        dbg_out_reserve(o, 0);
        dbg_out_printf(o, "],\"listed\":%d,\"truncated\":%s}\r\n",
                     listed, cut ? "true" : "false");
        return;
    }

    dbg_out_printf(o, "taskbar: y=%d h=%d tray_x=%d hidden=%d\r\n",
                 bar_y, taskbar_h, tray_left(), taskbar_hidden());
    dbg_out_write(o, "  start   x="); col_int(o, 0, 6);
    dbg_out_write(o, "w="); col_int(o, sw, 6);
    dbg_out_printf(o, "centre=(%d,%d)\r\n", sw / 2, bar_y + taskbar_h / 2);
    for (int i = 0; i < nb; i++) {
        dbg_out_write(o, "  win "); col_int(o, btns[i].first, 4);
        dbg_out_write(o, "x="); col_int(o, btns[i].x, 6);
        dbg_out_write(o, "w="); col_int(o, btns[i].w, 6);
        dbg_out_write(o, "n="); col_int(o, btns[i].count, 4);
        dbg_out_printf(o, "centre=(%d,%d)  %s\r\n",
                     btns[i].x + btns[i].w / 2, bar_y + taskbar_h / 2, btns[i].label);
    }
}

// The tray's volume flyout: the panel, its controls' centres and the
// device rows -- from the SAME volume_geometry() the drawing and the
// hit-testing use, so a test clicking a reported centre clicks what was
// drawn there.
//
// The device LABELS are reported because they are what a person reads
// and what the kernel's `audio_device` choice list computed; a test
// asserting on them is asserting the whole path from the registered
// driver to the row.
// The on-screen keyboard: the panel, the tray item that toggles it, the
// armed modifiers, and a keycap's box BY LABEL. `gui osk key <cap>` is
// the one a test uses -- deriving a cap's centre from the panel rect
// would be a second copy of osk.c's span walk.
static void cmd_osk(struct dbg_out *o, const char *arg, int json) {
    struct osk_report r;
    osk_report(&r);

    int kx, ky, kw, kh;
    if (arg && *arg && osk_key_box(arg, &kx, &ky, &kw, &kh)) {
        if (json)
            dbg_out_printf(o, "{\"cap\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                         "\"cx\":%d,\"cy\":%d}\r\n", arg, kx, ky, kw, kh,
                         kx + kw / 2, ky + kh / 2);
        else
            dbg_out_printf(o, "osk key \"%s\": x=%d y=%d w=%d h=%d centre=(%d,%d)\r\n",
                         arg, kx, ky, kw, kh, kx + kw / 2, ky + kh / 2);
        return;
    }
    if (arg && *arg) {
        dbg_out_printf(o, "osk: no key labelled \"%s\"\r\n", arg);
        return;
    }

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"mods\":%u,", osk_open ? "true" : "false", r.mods);
        dbg_out_printf(o, "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,", r.x, r.y, r.w, r.h);
        dbg_out_printf(o, "\"tray\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d}}\r\n",
                     r.tray_x, r.tray_y, r.tray_w, r.tray_h,
                     r.tray_x + r.tray_w / 2, r.tray_y + r.tray_h / 2);
        return;
    }
    dbg_out_printf(o, "osk: %s  mods=0x%x  x=%d y=%d w=%d h=%d\r\n",
                 osk_open ? "open" : "closed", r.mods, r.x, r.y, r.w, r.h);
    dbg_out_printf(o, "  tray x=%d y=%d w=%d h=%d centre=(%d,%d)\r\n",
                 r.tray_x, r.tray_y, r.tray_w, r.tray_h,
                 r.tray_x + r.tray_w / 2, r.tray_y + r.tray_h / 2);
}

static void cmd_volume(struct dbg_out *o, int json) {
    struct volume_geom g;
    volume_geometry(&g);

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"level\":%d,\"muted\":%s,",
                     volume_open ? "true" : "false", g.level,
                     g.muted ? "true" : "false");
        dbg_out_printf(o, "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,", g.x, g.y, g.w, g.h);
        dbg_out_printf(o, "\"tray\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d},",
                     g.tray_x, g.tray_y, g.tray_w, g.tray_h,
                     g.tray_x + g.tray_w / 2, g.tray_y + g.tray_h / 2);
        dbg_out_printf(o, "\"mute\":{\"cx\":%d,\"cy\":%d},",
                     g.mute_x + g.mute_w / 2, g.mute_y + g.mute_h / 2);
        dbg_out_printf(o, "\"slider\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cy\":%d},",
                     g.slider_x, g.slider_y, g.slider_w, g.slider_h,
                     g.slider_y + g.slider_h / 2);
        dbg_out_printf(o, "\"selected\":%d,\"devices\":[", g.selected_row);
        for (int i = 0; i < g.rows; i++) {
            char value[32], label[48];
            if (!volume_row(i, value, sizeof value, label, sizeof label)) break;
            dbg_out_printf(o, "%s{\"value\":\"%s\",\"label\":\"%s\","
                         "\"cx\":%d,\"cy\":%d}", i ? "," : "", value, label,
                         g.x + g.w / 2, g.list_y + i * g.row_h + g.row_h / 2);
        }
        dbg_out_write(o, "]}\r\n");
        return;
    }

    dbg_out_printf(o, "volume: %s  level=%d%s  x=%d y=%d w=%d h=%d\r\n",
                 volume_open ? "open" : "closed", g.level,
                 g.muted ? " muted" : "", g.x, g.y, g.w, g.h);
    dbg_out_printf(o, "  tray x=%d y=%d w=%d h=%d centre=(%d,%d)\r\n",
                 g.tray_x, g.tray_y, g.tray_w, g.tray_h,
                 g.tray_x + g.tray_w / 2, g.tray_y + g.tray_h / 2);
    dbg_out_printf(o, "  mute=(%d,%d) slider x=%d y=%d w=%d\r\n",
                 g.mute_x + g.mute_w / 2, g.mute_y + g.mute_h / 2,
                 g.slider_x, g.slider_y, g.slider_w);
    for (int i = 0; i < g.rows; i++) {
        char value[32], label[48];
        if (!volume_row(i, value, sizeof value, label, sizeof label)) break;
        dbg_out_printf(o, "  %s %-12s \"%s\" centre=(%d,%d)\r\n",
                     i == g.selected_row ? "*" : " ", value, label,
                     g.x + g.w / 2, g.list_y + i * g.row_h + g.row_h / 2);
    }
}

// The framebuffer grant as the compositor sees it: how many scanouts,
// which one it draws into next, and how many presents FLIPPED. The
// last number is what a test asserts -- a display that reports three
// buffers and never changes the index is a flip that is not happening.
static void cmd_fb(struct dbg_out *o, int json) {
    const struct ugfx_screen *s = &g_wm_screen;
    if (json) {
        dbg_out_printf(o, "{\"buffers\":%d,\"back\":%d,\"presents\":%u,\"flips\":%u}\r\n",
                     s->buffers, s->back_index, s->presents, s->flips);
        return;
    }
    dbg_out_printf(o, "fb: %d scanout%s, back=%d, presents=%u flips=%u\r\n",
                 s->buffers, s->buffers == 1 ? "" : "s", s->back_index,
                 s->presents, s->flips);
}

// The brightness flyout: the same shape as cmd_volume(), plus whether
// the setting is available and the sentence shown when it is not --
// which is the whole panel on a machine with no backlight (QEMU).
static void cmd_brightness(struct dbg_out *o, int json) {
    struct brightness_geom g;
    brightness_geometry(&g);
    const char *why = brightness_unavailable_text();

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"level\":%d,\"available\":%s,\"unavailable\":\"%s\",",
                     brightness_open ? "true" : "false", g.level,
                     g.available ? "true" : "false", why);
        dbg_out_printf(o, "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,", g.x, g.y, g.w, g.h);
        dbg_out_printf(o, "\"tray\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d},",
                     g.tray_x, g.tray_y, g.tray_w, g.tray_h,
                     g.tray_x + g.tray_w / 2, g.tray_y + g.tray_h / 2);
        dbg_out_printf(o, "\"slider\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cy\":%d}}\r\n",
                     g.slider_x, g.slider_y, g.slider_w, g.slider_h,
                     g.slider_y + g.slider_h / 2);
        return;
    }
    dbg_out_printf(o, "brightness: %s  level=%d  %s  x=%d y=%d w=%d h=%d\r\n",
                 brightness_open ? "open" : "closed", g.level,
                 g.available ? "available" : why, g.x, g.y, g.w, g.h);
    dbg_out_printf(o, "  tray x=%d y=%d w=%d h=%d centre=(%d,%d)  slider x=%d y=%d w=%d\r\n",
                 g.tray_x, g.tray_y, g.tray_w, g.tray_h,
                 g.tray_x + g.tray_w / 2, g.tray_y + g.tray_h / 2,
                 g.slider_x, g.slider_y, g.slider_w);
}

// The tray clock's calendar popup: the panel's rect, its `<`/`>`
// centres, and the grid -- from the SAME calendar_geometry() the
// drawing and the hit-testing use, so a test clicking a reported centre
// clicks what was drawn there (the rule cmd_taskbar() above was
// rewritten for).
//
// `today` is reported beside the viewed month because the highlighted
// cell is a function of both, and a test that carried its own clock
// would be asserting against the HOST's date rather than the guest's --
// which differ whenever the guest's RTC or `timezone` says so.
static void cmd_calendar(struct dbg_out *o, int json) {
    struct calendar_geom g;
    calendar_geometry(&g);
    int ty, tm, td;
    calendar_today(&ty, &tm, &td);
    int cx = 0, cy = 0, cw = 0, ch = 0;
    int have_clock = tray_clock_rect(&cx, &cy, &cw, &ch);

    if (json) {
        dbg_out_printf(o, "{\"open\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,",
                     calendar_open ? "true" : "false", g.x, g.y, g.w, g.h);
        dbg_out_printf(o, "\"header_h\":%d,\"cell_w\":%d,\"cell_h\":%d,"
                     "\"grid_x\":%d,\"grid_y\":%d,",
                     g.header_h, g.cell_w, g.cell_h, g.grid_x, g.grid_y);
        dbg_out_printf(o, "\"prev\":{\"cx\":%d,\"cy\":%d},\"next\":{\"cx\":%d,\"cy\":%d},"
                     "\"title\":{\"cx\":%d,\"cy\":%d,\"text\":\"%s %d\"},",
                     g.prev_x + g.prev_w / 2, g.prev_y + g.prev_h / 2,
                     g.next_x + g.next_w / 2, g.next_y + g.next_h / 2,
                     g.title_x + g.title_w / 2, g.title_y + g.title_h / 2,
                     calendar_month_name(g.view_month), g.view_year);
        dbg_out_printf(o, "\"view\":{\"year\":%d,\"month\":%d,\"days\":%d,\"first_col\":%d},",
                     g.view_year, g.view_month, g.days, g.first_col);
        dbg_out_printf(o, "\"today\":{\"year\":%d,\"month\":%d,\"day\":%d,"
                     "\"col\":%d,\"row\":%d},",
                     ty, tm, td, g.today_col, g.today_row);
        dbg_out_printf(o, "\"week_start\":\"%s\",\"clock\":",
                     g.week_start_monday ? "monday" : "sunday");
        if (have_clock)
            dbg_out_printf(o, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d}}\r\n",
                         cx, cy, cw, ch, cx + cw / 2, cy + ch / 2);
        else
            dbg_out_write(o, "null}\r\n");
        return;
    }

    dbg_out_printf(o, "calendar: %s  %s %d  x=%d y=%d w=%d h=%d\r\n",
                 calendar_open ? "open" : "closed",
                 calendar_month_name(g.view_month), g.view_year,
                 g.x, g.y, g.w, g.h);
    dbg_out_printf(o, "  grid  x=%d y=%d cell=%dx%d first_col=%d days=%d week_start=%s\r\n",
                 g.grid_x, g.grid_y, g.cell_w, g.cell_h, g.first_col, g.days,
                 g.week_start_monday ? "monday" : "sunday");
    dbg_out_printf(o, "  today %d-%d-%d  cell col=%d row=%d\r\n",
                 ty, tm, td, g.today_col, g.today_row);
    dbg_out_printf(o, "  prev=(%d,%d) next=(%d,%d) title=(%d,%d)\r\n",
                 g.prev_x + g.prev_w / 2, g.prev_y + g.prev_h / 2,
                 g.next_x + g.next_w / 2, g.next_y + g.next_h / 2,
                 g.title_x + g.title_w / 2, g.title_y + g.title_h / 2);
    if (have_clock)
        dbg_out_printf(o, "  clock x=%d y=%d w=%d h=%d centre=(%d,%d)\r\n",
                     cx, cy, cw, ch, cx + cw / 2, cy + ch / 2);
}

// Everything else the WM is holding: which overlays are up, where the
// cursor is, what's armed, and this frame's damage rect. The last one
// is the thing you want when a repaint looks wrong -- it's otherwise
// completely invisible.
static void cmd_state(struct dbg_out *o, int json) {
    int cx, cy;
    uint8_t buttons;
    wm_rawin_mouse(&cx, &cy, &buttons);
    int dx, dy, dw, dh;
    wm_debug_damage(&dx, &dy, &dw, &dh);

    if (json) {
        dbg_out_printf(o, "{\"screen\":{\"w\":%d,\"h\":%d},\"taskbar_h\":%d,",
                     screen_w, screen_h, taskbar_h);
        // The frontmost window, named by its client's pid, so "which
        // window is on top" can be asked WITHOUT pulling the whole
        // window list -- which is capped at WIN_DEBUG_REPLY_MAX and
        // truncates past about twenty-five windows (docs/bugs.md).
        // -1 when there are no windows at all.
        dbg_out_printf(o, "\"windows\":%d,\"front_pid\":%d,",
                     window_count,
                     window_count > 0 ? windows[window_count - 1].client_pid : -1);
        // `shape` is the resolved WM_CURSOR_* (wm_internal.h), which is
        // how a test checks what a client asked for without having to
        // recognise a sprite in a screenshot.
        dbg_out_printf(o, "\"cursor\":{\"x\":%d,\"y\":%d,\"buttons\":%u,\"shape\":%d},",
                     cx, cy, (unsigned)buttons, (int)wm_cursor_kind_at(cx, cy));
        dbg_out_printf(o, "\"overlays\":{\"start_menu\":%s,\"context_menu\":%s,",
                     start_menu_open ? "true" : "false",
                     context_menu_open ? "true" : "false");
        dbg_out_printf(o, "\"file_picker\":%s,\"confirm_dialog\":%s,\"calendar\":%s,\"volume\":%s,\"brightness\":%s},",
                     file_picker_open ? "true" : "false",
                     confirm_dialog_open ? "true" : "false",
                     calendar_open ? "true" : "false",
                     volume_open ? "true" : "false",
                     brightness_open ? "true" : "false");
        dbg_out_printf(o, "\"dragging\":%d,\"resizing\":%d,\"content_pressed\":%d,",
                     dragging, resizing, content_pressed);
        // Resize proposals sent since boot -- see wm.c. One per drag
        // would mean the window only resizes on release.
        dbg_out_printf(o, "\"resizes_asked\":%u,", resize_asks);
        // WHAT THE LAST DRAG ACTUALLY SHOWED, not what the setting
        // says: `auto` resolves to one or the other while the drag runs
        // (wm_input.c), and nothing else can report which way it went.
        dbg_out_printf(o, "\"resize_paint\":\"%s\",\"move_paint\":\"%s\",",
                     resize_outline_mode ? "outline" : "live",
                     move_outline_mode ? "outline" : "live");
        dbg_out_printf(o, "\"resize_lag_ms\":%u,", resize_lag_ms);
        dbg_out_printf(o, "\"redraw_pending\":%s,\"pending\":%d,",
                     redraw_pending ? "true" : "false", wm_debug_input_pending());
        dbg_out_printf(o, "\"hwcursor\":%s,",
                     wm_hwcursor_active() ? "true" : "false");
        // Always 0, and kept in the grammar on purpose. It reported
        // wm.c's pending_proc -- the process a WINDOW was waiting on --
        // which only window_start_process() ever set, and nothing has
        // called that since M41 stage 0 retired the kernel-space
        // Terminal. Stage 4c DELETED that machinery outright (R9): a
        // ring-3 WM is a process and can simply block, so the whole
        // one-step-per-frame dance went with it.
        //
        // The field stays because the test tools parse this object and a
        // key vanishing is a harness failure that looks like a WM bug.
        // Use "launched" below for the real question -- whether a process
        // the desktop started is alive.
        dbg_out_printf(o, "\"proc_pid\":%d,", 0);
        // Every pid the desktop launched that is still running -- the
        // WM's own reaped launch table (wm.c's g_launched). This is the
        // host-observable way to tell that a client process and the WM
        // are alive AT THE SAME TIME: proving the desktop stays
        // responsive while a process runs needs the process's liveness
        // from the same answer, and asking the WM is the "ask, don't
        // measure a screenshot" principle the rest of this file is built
        // on. See tools/sched_gui_test.py.
        dbg_out_write(o, "\"launched\":[");
        int first = 1;
        for (int i = 0; i < wm_launched_max(); i++) {
            int pid = wm_launched_pid(i);
            if (!pid) continue;
            dbg_out_printf(o, "%s%d", first ? "" : ",", pid);
            first = 0;
        }
        dbg_out_write(o, "],");
        dbg_out_printf(o, "\"damage\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}}\r\n",
                     dx, dy, dw, dh);
        return;
    }

    dbg_out_printf(o, "screen %dx%d, taskbar %dpx\r\n", screen_w, screen_h, taskbar_h);
    dbg_out_printf(o, "cursor (%d,%d) buttons=0x%x shape=%d\r\n", cx, cy,
                 (unsigned)buttons, (int)wm_cursor_kind_at(cx, cy));
    dbg_out_printf(o, "overlays: topmost=%s\r\n",
                 wm_overlay_topmost() ? wm_overlay_topmost() : "none");
    dbg_out_printf(o, "overlays: start_menu=%d context_menu=%d file_picker=%d confirm=%d calendar=%d volume=%d brightness=%d osk=%d\r\n",
                 start_menu_open, context_menu_open, file_picker_open, confirm_dialog_open,
                 calendar_open, volume_open, brightness_open, osk_open);
    dbg_out_printf(o, "dragging=%d resizing=%d content_pressed=%d redraw_pending=%d\r\n",
                 dragging, resizing, content_pressed, redraw_pending);
    dbg_out_printf(o, "resize proposals sent: %u\r\n", resize_asks);
    dbg_out_printf(o, "last resize lag: %ums\r\n", resize_lag_ms);
    dbg_out_printf(o, "last drag showed: resize=%s move=%s\r\n",
                 resize_outline_mode ? "outline" : "live",
                 move_outline_mode ? "outline" : "live");
    dbg_out_printf(o, "injected events pending: %d\r\n", wm_debug_input_pending());
    dbg_out_printf(o, "scene repaints: %u\r\n", wm_scene_frames());
    dbg_out_write(o, "launched (still running):");
    int any = 0;
    for (int i = 0; i < wm_launched_max(); i++) {
        int pid = wm_launched_pid(i);
        if (!pid) continue;
        dbg_out_printf(o, " %d", pid);
        any = 1;
    }
    dbg_out_write(o, any ? "\r\n" : " none\r\n");
    dbg_out_printf(o, "damage rect: x=%d y=%d w=%d h=%d%s\r\n", dx, dy, dw, dh,
                 dw <= 0 ? "  (none this frame -- full-screen repaint)" : "");
}

// ---------------------------------------------------------------------
// Driving
// ---------------------------------------------------------------------

static void cmd_open(struct dbg_out *o, const char *name) {
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (k_strcmp(gui_app_registry[i].name, name) == 0) {
            open_app(&gui_app_registry[i]);
            dbg_out_printf(o, "gui: opened \"%s\"\r\n", name);
            return;
        }
    }
    dbg_out_printf(o, "gui: no app named \"%s\". Known apps:\r\n", name);
    for (int i = 0; i < gui_app_registry_count; i++) {
        dbg_out_printf(o, "  %s\r\n", gui_app_registry[i].name);
    }
}

// Spawn a ring-3 binary directly, with no Terminal in the loop.
//
// `gui open` can only launch what is in the app registry, so every test
// that wanted a ring-3 client had to open a Terminal and type at it --
// which drags the Terminal's own allowlist, its pending-process slot and
// its shell into a test that is about something else entirely. It also
// makes a client that never exits untestable: the Terminal that spawned
// it cannot then be closed.
//
// Tracked for reaping exactly as a Start-menu launch is, so this does
// not quietly reintroduce the leak wm_track_launched() exists to fix.
static void cmd_spawn(struct dbg_out *o, const char *path, const char *args) {
    int pid = sys_spawn(path, args, -1);
    if (pid > 0) {
        wm_track_launched(pid);
        dbg_out_printf(o, "gui: spawned \"%s\" as pid %d\r\n", path, pid);
    } else {
        dbg_out_printf(o, "gui: spawn of \"%s\" FAILED (no slot, or no such binary)\r\n", path);
    }
}

// The other half of `gui spawn`. There was no way for a test to end a
// process it had started: the only killers were Task Manager's button
// and the force-quit dialog, which drag an app and a modal into a test
// about neither. M41's R7 needs a compositor killed OUTRIGHT -- the
// least graceful of the three teardown paths, and the one a crashing WM
// actually takes.
//
// scheduler_kill() is the same call SYS_KILL makes, so this exercises
// the real path rather than a test-only shortcut.
static void cmd_kill(struct dbg_out *o, int pid) {
    if (pid <= 0) {
        dbg_out_write(o, "usage: gui kill PID\r\n");
        return;
    }
    if (sys_kill(pid, SIGKILL) == 0) {
        dbg_out_printf(o, "gui: killed pid %d\r\n", pid);
    } else {
        dbg_out_printf(o, "gui: no such process %d\r\n", pid);
    }
}

static void cmd_close(struct dbg_out *o, int index) {
    if (index < 0 || index >= window_count) {
        dbg_out_printf(o, "gui: no window %d (see `gui windows`)\r\n", index);
        return;
    }
    dbg_out_printf(o, "gui: closing window %d \"%s\"\r\n", index, windows[index].title);
    // wm_request_close(), NOT close_window() -- the same path the X
    // button, the context menu and Alt+F4 take. This was the FOURTH
    // close in the WM and the one that still seized a window instead of
    // asking for it: close_window() drops a client's window without
    // telling the client, so the process carries on running with no
    // window, holding a slot out of MAX_PROCS (64) until reboot. Opening
    // four apps from a test then stopped the desktop launching anything
    // at all, which reads as a spawn bug rather than as a close bug.
    //
    // See wm_request_close()'s own comment: repeating the client check
    // is exactly how the context menu drifted, and this is the same
    // mistake in the debug console.
    wm_request_close(index);
}

// A bare cursor MOVE, no buttons. Exists because hover was otherwise
// untestable: `gui click` moves the cursor too, but it also presses and
// releases, so by the time a test can screenshot the result the click has
// already been acted on -- and docs/gui-guidelines.md requires hover
// states to be verified by pixel value, which needs the cursor parked
// somewhere with nothing held.
static int cmd_move(int x, int y) {
    return inject_push(x, y, 0);
}

// A click is four queued events, not one: move, press, a held tick, and
// release. Each is consumed on its own wm_run() iteration, which is
// what makes press and release land on different frames -- a control
// that arms on press and commits on release (every control in this GUI,
// see docs/gui-guidelines.md) needs exactly that to behave normally.
// The RIGHT button, which opens a context menu. Bit 0x2, matching
// wm.c's own right_edge_down test -- the same press/press/release shape
// cmd_click() uses for the left one, so the WM sees a real edge.
//
// Added because no test could open a context menu at all, which is how
// its Close row went on tearing ring-3 windows down without their
// handshake while the X button beside it asked politely.
static int cmd_rclick(int x, int y) {
    return inject_push(x, y, 0) &&
           inject_push(x, y, 2) &&
           inject_push(x, y, 2) &&
           inject_push(x, y, 0);
}

static int cmd_click(int x, int y) {
    return inject_push(x, y, 0) &&
           inject_push(x, y, 1) &&
           inject_push(x, y, 1) &&
           inject_push(x, y, 0);
}

// Interpolated so the WM sees real intermediate positions: a drag that
// jumped straight to its destination would never exercise the
// per-tick "is the cursor still over the armed control" tracking, which
// is most of what a drag test is for.
//
// **MORE STEPS IS FINER MOTION, NOT A LONGER DRAG.** One queued position
// is consumed per wm_run() iteration and the loop iterates as fast as it
// can while input is pending, so 48 steps take about as long as 8. What
// they buy is intermediate positions a hit region cannot be stepped
// over -- see docs/roadmap-details.md's "Finer `gui drag`
// interpolation".
#define DRAG_STEPS 8
#define DRAG_STEPS_MAX (INJECT_MAX - 4)  // room for the press and release
static int cmd_drag(int x0, int y0, int x1, int y1, int steps) {
    if (steps < 1) steps = DRAG_STEPS;
    if (steps > DRAG_STEPS_MAX) steps = DRAG_STEPS_MAX;
    if (!inject_push(x0, y0, 0)) return 0;
    if (!inject_push(x0, y0, 1)) return 0;
    for (int i = 1; i <= steps; i++) {
        int x = x0 + (x1 - x0) * i / steps;
        int y = y0 + (y1 - y0) * i / steps;
        if (!inject_push(x, y, 1)) return 0;
    }
    return inject_push(x1, y1, 0);
}

// `gui key <c> [shift|ctrl|alt|altgr]...`
//
// The modifier words set the KEY_MOD_* bits the WM delivers alongside
// the key -- they do NOT re-encode it. So `gui key 0x09 shift` is
// Shift-Tab (key 0x09, KEY_MOD_SHIFT), which is the only way to express
// it: Tab has no shifted character, so shift alone changes nothing about
// the key itself. For Ctrl-A, send the control code (`gui key 0x01`)
// rather than `gui key a ctrl` -- that is what a real keyboard produces,
// and what every consumer matches on (see keyboard.h).
static int cmd_key(const char *s, char *rest) {
    if (!s || !*s) return 0;
    int code;
    // A bare character, or "0x1b"-style for anything unprintable.
    if (s[1] == '\0') code = (unsigned char)s[0];
    else if (!parse_int(s, &code)) return 0;

    uint8_t mods = 0;
    for (char *p = rest, *t; (t = next_tok(&p)) != 0; ) {
        if (k_strcmp(t, "shift") == 0) mods |= KEY_MOD_SHIFT;
        else if (k_strcmp(t, "ctrl") == 0) mods |= KEY_MOD_CTRL;
        else if (k_strcmp(t, "alt") == 0) mods |= KEY_MOD_ALT;
        else if (k_strcmp(t, "altgr") == 0) mods |= KEY_MOD_ALTGR;
        else return 0; // an unrecognised word is a typo, not a modifier
    }

    int next = (g_keys_tail + 1) % KEY_INJECT_MAX;
    if (next == g_keys_head) return 0;
    g_keys[g_keys_tail] = code;
    g_key_mods[g_keys_tail] = mods;
    g_keys_tail = next;
    return 1;
}

static int cmd_wheel(const char *s) {
    int delta;
    if (!parse_int(s, &delta) || delta == 0) return 0;
    int next = (g_wheel_tail + 1) % KEY_INJECT_MAX;
    if (next == g_wheel_head) return 0;
    g_wheel[g_wheel_tail] = delta;
    g_wheel_tail = next;
    return 1;
}

// Is there a second consumer of the input stream, and is it keeping up?
//
// No other `gui` subcommand can see one: everything else reports the
// WM's own state, and a registered compositor is by definition another
// process. Without this, "the compositor received the click" could only
// be asserted from the client's own log -- which cannot distinguish
// "delivered and handled" from "never delivered" when the client is the
// thing under test.
//
// `dropped` is the load-bearing number. The queue is 32 deep and drops
// the OLDEST, so a compositor falling behind loses input silently; that
// is exactly the failure this has to be able to name.
static void cmd_compositor(struct dbg_out *o, int json) {
    // WIN_REQ_EVENT_STATS rather than the kernel's own counters: this
    // process IS the compositor, so it asks about itself, and the reply
    // carries the registered pid so the answer cannot disagree with who
    // the kernel thinks is composing.
    struct win_request_msg q;
    k_memset(&q, 0, sizeof q);
    q.type = WIN_REQ_EVENT_STATS;
    q.a = 0; // 0 = "me" -- see WIN_REQ_EVENT_STATS
    int pid = 0, pending = 0, dropped = 0;
    if (sys_win_request(&q) == 0) {
        pending = q.a;
        dropped = q.b;
        pid = q.c;
    }

    if (json) {
        dbg_out_printf(o, "{\"pid\":%d,\"pending\":%d,\"dropped\":%d}\r\n",
                    pid, pending, dropped);
    } else if (!pid) {
        dbg_out_write(o, "compositor: none registered\r\n");
    } else {
        dbg_out_printf(o, "compositor: pid %d  pending %d  dropped %d\r\n",
                    pid, pending, dropped);
    }
}

static void usage(struct dbg_out *o) {
    dbg_out_write(o, "gui subcommands (all of these work while the desktop is up):\r\n");
    dbg_out_write(o, "  windows [--json]      open windows: rects, content rects, z-order, focus\r\n");
    dbg_out_write(o, "  probe X Y [--json]    what is at this point, and what would take the click\r\n");
    dbg_out_write(o, "  menu [--json]         start menu row geometry, as the kernel computes it\r\n");
    dbg_out_write(o, "  ctxmenu [--json]      the open right-click menu's rows, same shape as `menu`\r\n");
    dbg_out_write(o, "  dialog [--json]       the open confirm dialog's message and button centres\r\n");
    dbg_out_write(o, "  rclick X Y            right-click, which is what opens a context menu\r\n");
    dbg_out_write(o, "  spawn PATH            run a ring-3 binary directly -- no Terminal needed\r\n");
    dbg_out_write(o, "  taskbar [--json]      start button + per-window button rects\r\n");
    dbg_out_write(o, "  calendar [--json]     the clock's calendar popup: panel, grid, today\r\n");
    dbg_out_write(o, "  volume [--json]       the tray volume flyout: level, slider, devices\r\n");
    dbg_out_write(o, "  brightness [--json]   the tray brightness flyout: level, slider, availability\r\n");
    dbg_out_write(o, "  state [--json]        overlays, cursor, armed state, damage rect\r\n");
    dbg_out_write(o, "  compositor [--json]   the registered compositor pid, its queue depth\r\n");
    dbg_out_write(o, "  fb [--json]           the framebuffer grant: scanouts, back index, flips\r\n");
    dbg_out_write(o, "                        and how much input it has dropped\r\n");
    dbg_out_write(o, "  damage [verify on|off]  the damage rect; verify renders every frame\r\n");
    dbg_out_write(o, "                        twice and reports pixels the damage rect missed\r\n");
    dbg_out_write(o, "  apps                  the gui_app registry\r\n");
    dbg_out_write(o, "  open <AppName>        open a window directly (no menu clicking)\r\n");
    dbg_out_write(o, "  close <index>         close window <index> from `gui windows`\r\n");
    dbg_out_write(o, "  move X Y              move the cursor, nothing held (for hover)\r\n");
    dbg_out_write(o, "  click X Y             synthetic press+release at a point\r\n");
    dbg_out_write(o, "  drag X1 Y1 X2 Y2 [N]  synthetic press, N interpolated moves, release\r\n");
    dbg_out_write(o, "  key <c|0xNN>          synthetic keypress to the focused window\r\n");
    dbg_out_write(o, "  wheel <n>             synthetic wheel notches (+up / -down)\r\n");
    dbg_out_write(o, "  icons                 how many app icons are decoded and cached\r\n");
    dbg_out_write(o, "  watchdog [<ms>|off]   slow-frame threshold, and how often it fired\r\n");
    dbg_out_write(o, "  pingtimeout [<ticks>] not-responding timeout (a TEST lever)\r\n");
    dbg_out_write(o, "  pinginterval [<ticks>] how often every client is asked (a TEST lever)\r\n");
    dbg_out_write(o, "Injected input enters at the WM loop, below the PS/2 driver -- it tests\r\n");
    dbg_out_write(o, "WM/app logic, not the mouse driver. It is also asynchronous: the events\r\n");
    dbg_out_write(o, "drain one per frame, so allow ~100ms before reading the result back.\r\n");
}

// The icon cache: what has been decoded, and at which size.
//
// It exists so a test can assert the cache IS a cache. "The icon is
// drawn" says nothing about whether the file was decoded once or on
// every frame, and the difference between those is milliseconds per
// repaint -- exactly the kind of thing that is invisible until the
// machine feels slow and nobody knows why.
static void cmd_icons(struct dbg_out *o, int json) {
    int n = icon_cache_count();
    if (json) {
        dbg_out_printf(o, "{\"cached\":%d,\"evictions\":%d}\r\n",
                       n, icon_cache_evictions());
        return;
    }
    dbg_out_printf(o, "icons: %d cached (name,size pairs decoded and scaled), "
                   "%d evicted\r\n", n, icon_cache_evictions());
}

int wm_debug_dispatch_out(char *line, struct dbg_out *o) {
    char *p = line;
    char *sub = next_tok(&p);
    if (!sub) { usage(o); return 1; }

    // `rest` is scanned for --json by wants_json(), which consumes it --
    // so grab positional arguments BEFORE asking about the flag.
    // `gui resize <w> <h>` -- the FOCUSED window's CONTENT size, in
    // pixels. The one way a test can resize a window: the grip needs a
    // real pointer tracked across frames, which injected input cannot
    // be (see wm_resize_client).
    if (k_strcmp(sub, "resize") == 0) {
        int w, h;
        if (!parse_int(next_tok(&p), &w) || !parse_int(next_tok(&p), &h) ||
            w < 1 || h < 1) {
            dbg_out_write(o, "usage: gui resize W H\r\n");
            return 1;
        }
        int idx = window_count - 1;   // topmost is the focused one
        if (idx < 0) { dbg_out_write(o, "gui: no window\r\n"); return 1; }
        wm_resize_client(idx, w, h);
        dbg_out_printf(o, "gui: asked \"%s\" for %dx%d\r\n",
                       windows[idx].title, w, h);
        return 1;
    }

    if (k_strcmp(sub, "windows") == 0)      { cmd_windows(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "menu") == 0)         { cmd_menu(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "ctxmenu") == 0)      { cmd_ctxmenu(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "dialog") == 0)       { cmd_dialog(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "taskbar") == 0)      { cmd_taskbar(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "calendar") == 0)     { cmd_calendar(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "volume") == 0)       { cmd_volume(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "osk") == 0) {
        // `gui osk key <cap>`. The positional word is taken BEFORE
        // wants_json() consumes the rest, per the note above.
        char *arg = next_tok(&p);
        if (arg && k_strcmp(arg, "key") == 0) arg = next_tok(&p);
        else if (arg && arg[0] == '-') arg = 0;   // a flag, not a cap
        cmd_osk(o, arg, wants_json(p));
        return 1;
    }
    if (k_strcmp(sub, "brightness") == 0)   { cmd_brightness(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "state") == 0)        { cmd_state(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "compositor") == 0)   { cmd_compositor(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "fb") == 0)           { cmd_fb(o, wants_json(p)); return 1; }
    if (k_strcmp(sub, "icons") == 0)        { cmd_icons(o, wants_json(p)); return 1; }

    if (k_strcmp(sub, "damage") == 0) {
        char *arg = next_tok(&p);
        if (arg && k_strcmp(arg, "verify") == 0) {
            char *onoff = next_tok(&p);
            if (!onoff) {
                dbg_out_printf(o, "damage verification is %s\r\n",
                             wm_damage_verify_enabled() ? "on" : "off");
                return 1;
            }
            wm_damage_verify_set(k_strcmp(onoff, "on") == 0);
            return 1;
        }
        int dx, dy, dw, dh;
        wm_debug_damage(&dx, &dy, &dw, &dh);
        dbg_out_printf(o, "damage: x=%d y=%d w=%d h=%d  verify=%s\r\n", dx, dy, dw, dh,
                     wm_damage_verify_enabled() ? "on" : "off");
        return 1;
    }
    // The not-responding ping timeout. A TEST lever, not a setting --
    // see wm_internal.h. Reports the value either way, so a tool can
    // assert it actually took rather than assuming.
    if (k_strcmp(sub, "pingtimeout") == 0) {
        char *arg = next_tok(&p);
        if (arg) {
            int ticks;
            if (parse_int(arg, &ticks) && ticks > 0) {
                wm_ping_timeout_ticks = ticks;
            } else {
                dbg_out_write(o, "usage: gui pingtimeout [<ticks>]\r\n");
                return 1;
            }
        }
        dbg_out_printf(o, "pingtimeout: %d ticks (%d ms at 100Hz)\r\n",
                       wm_ping_timeout_ticks, wm_ping_timeout_ticks * 10);
        return 1;
    }

    // How often every client is ASKED, the other half of the lever
    // above. A test wanting a hang detected quickly has to shorten both:
    // detection is interval + timeout.
    if (k_strcmp(sub, "pinginterval") == 0) {
        char *arg = next_tok(&p);
        if (arg) {
            int ticks;
            if (parse_int(arg, &ticks) && ticks > 0) {
                wm_ping_interval_ticks = ticks;
            } else {
                dbg_out_write(o, "usage: gui pinginterval [<ticks>]\r\n");
                return 1;
            }
        }
        dbg_out_printf(o, "pinginterval: %d ticks (%d ms at 100Hz)\r\n",
                       wm_ping_interval_ticks, wm_ping_interval_ticks * 10);
        return 1;
    }

    // The slow-frame watchdog (wm_watchdog.c). Reporting the counters
    // matters as much as the threshold: "no SLOW FRAME lines in the log"
    // is only evidence the WM was fast if the watchdog was actually
    // armed, and those two states otherwise look identical from outside.
    if (k_strcmp(sub, "watchdog") == 0) {
        char *arg = next_tok(&p);
        if (arg) {
            int ms;
            if (k_strcmp(arg, "off") == 0) {
                wmwd_set_threshold_ms(0);
            } else if (parse_int(arg, &ms) && ms >= 0) {
                wmwd_set_threshold_ms((uint32_t)ms);
            } else {
                dbg_out_write(o, "usage: gui watchdog [<ms>|off]\r\n");
                return 1;
            }
        }
        uint32_t th = wmwd_threshold_ms();
        if (th) dbg_out_printf(o, "watchdog: on, threshold %u ms\r\n", th);
        else    dbg_out_write(o, "watchdog: off\r\n");
        dbg_out_printf(o, "  %u slow frame(s) so far; slowest frame %u ms\r\n",
                       wmwd_slow_frames(), wmwd_peak_ms());
        return 1;
    }

    if (k_strcmp(sub, "help") == 0)         { usage(o); return 1; }

    if (k_strcmp(sub, "apps") == 0) {
        for (int i = 0; i < gui_app_registry_count; i++) {
            dbg_out_write(o, "  ");
            col_str(o, gui_app_registry[i].name, 16);
            // `show_in` is what makes "loaded" and "visible here" two
            // separate observations. Without it a test cannot tell an
            // entry FILTERED off a surface from one that never loaded,
            // and an assertion that cannot tell those apart passes just
            // as happily against a broken parser as against a working
            // filter.
            unsigned s = gui_app_registry[i].show_in;
            dbg_out_printf(o, "resizable=%d multi_instance=%d show_in=%s%s\r\n",
                         gui_app_registry[i].resizable,
                         gui_app_registry[i].multi_instance,
                         (s & GUI_SHOW_DESKTOP) ? "desktop" : "",
                         (s & GUI_SHOW_STARTMENU) ? "+startmenu" : "");
        }
        return 1;
    }

    if (k_strcmp(sub, "probe") == 0) {
        char *ax = next_tok(&p), *ay = next_tok(&p);
        int x, y;
        if (!parse_int(ax, &x) || !parse_int(ay, &y)) {
            dbg_out_write(o, "usage: gui probe X Y [--json]\r\n");
            return 1;
        }
        cmd_probe(o, x, y, wants_json(p));
        return 1;
    }

    if (k_strcmp(sub, "open") == 0) {
        char *name = next_tok(&p);
        if (!name) { dbg_out_write(o, "usage: gui open <AppName>  (see `gui apps`)\r\n"); return 1; }
        // Re-join a two-word name ("Task Manager", "Control Panel") --
        // next_tok() split it, and quoting over a serial line is worse
        // than just gluing the remainder back on.
        if (*p) {
            char *end = name + k_strlen(name);
            *end = ' ';
        }
        cmd_open(o, name);
        return 1;
    }

    if (k_strcmp(sub, "close") == 0) {
        int idx;
        if (!parse_int(next_tok(&p), &idx)) {
            dbg_out_write(o, "usage: gui close <index>  (see `gui windows`)\r\n");
            return 1;
        }
        cmd_close(o, idx);
        return 1;
    }

    if (k_strcmp(sub, "spawn") == 0) {
        char *path = next_tok(&p);
        if (!path) { dbg_out_write(o, "usage: gui spawn /path/to/binary [args]\r\n"); return 1; }
        // Everything after the path is the argument string, passed
        // through verbatim (scheduler_spawn splits it the same way the
        // shell's `run` does). Needed because a test's binary can take
        // one -- spin_test's round count is the reason this exists, and
        // without it that test had to go through a Terminal to say it.
        while (k_isblank(*p)) p++;
        cmd_spawn(o, path, *p ? p : 0);
        return 1;
    }

    if (k_strcmp(sub, "kill") == 0) {
        int pid;
        if (!parse_int(next_tok(&p), &pid)) {
            dbg_out_write(o, "usage: gui kill PID\r\n");
            return 1;
        }
        cmd_kill(o, pid);
        return 1;
    }

    if (k_strcmp(sub, "rclick") == 0) {
        int x, y;
        if (!parse_int(next_tok(&p), &x) || !parse_int(next_tok(&p), &y)) {
            dbg_out_write(o, "usage: gui rclick X Y\r\n");
            return 1;
        }
        dbg_out_write(o, cmd_rclick(x, y) ? "gui: queued rclick\r\n" : "gui: input queue full\r\n");
        return 1;
    }

    if (k_strcmp(sub, "click") == 0) {
        int x, y;
        if (!parse_int(next_tok(&p), &x) || !parse_int(next_tok(&p), &y)) {
            dbg_out_write(o, "usage: gui click X Y\r\n");
            return 1;
        }
        dbg_out_printf(o, cmd_click(x, y) ? "gui: queued click at (%d,%d)\r\n"
                                     : "gui: input queue full, click at (%d,%d) DROPPED\r\n",
                     x, y);
        return 1;
    }

    if (k_strcmp(sub, "drag") == 0) {
        int x0, y0, x1, y1, steps = 0;
        if (!parse_int(next_tok(&p), &x0) || !parse_int(next_tok(&p), &y0) ||
            !parse_int(next_tok(&p), &x1) || !parse_int(next_tok(&p), &y1)) {
            dbg_out_write(o, "usage: gui drag X1 Y1 X2 Y2 [STEPS]\r\n");
            return 1;
        }
        parse_int(next_tok(&p), &steps);   // optional; 0 keeps the default
        dbg_out_printf(o, cmd_drag(x0, y0, x1, y1, steps)
                     ? "gui: queued drag (%d,%d) -> (%d,%d)\r\n"
                     : "gui: input queue full, drag (%d,%d) -> (%d,%d) DROPPED\r\n",
                     x0, y0, x1, y1);
        return 1;
    }

    if (k_strcmp(sub, "wheel") == 0) {
        dbg_out_write(o, cmd_wheel(next_tok(&p)) ? "gui: queued wheel\r\n"
                                            : "gui: bad or dropped wheel delta\r\n");
        return 1;
    }

    if (k_strcmp(sub, "move") == 0) {
        char *xs = next_tok(&p), *ys = next_tok(&p);
        int x, y;
        if (!xs || !ys || !parse_int(xs, &x) || !parse_int(ys, &y)) {
            dbg_out_write(o, "usage: gui move X Y\r\n");
            return 1;
        }
        dbg_out_printf(o, cmd_move(x, y) ? "gui: queued move to (%d,%d)\r\n"
                                    : "gui: move queue full\r\n", x, y);
        return 1;
    }

    if (k_strcmp(sub, "key") == 0) {
        char *k = next_tok(&p);
        dbg_out_write(o, cmd_key(k, p) ? "gui: queued key\r\n" : "gui: bad or dropped key\r\n");
        return 1;
    }

    return 0;
}
