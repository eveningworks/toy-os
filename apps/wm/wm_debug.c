// See wm_debug.h for what this is for and why it lives here.
#include "wm_internal.h"
#include "wm_debug.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "gui_apps.h"
#include "kapi.h"

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
// Argument parsing
// ---------------------------------------------------------------------

// Pulls the next whitespace-separated token off `*p`, NUL-terminating
// it in place and advancing `*p` past it. Returns NULL at end of line.
static char *next_tok(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
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
static void col_str(const char *s, int width) {
    int n = 0;
    if (s) { klog_write(s); n = (int)k_strlen(s); }
    for (int i = n; i < width; i++) klog_write(" ");
}

static void col_int(int v, int width) {
    char buf[16];
    k_snprintf(buf, sizeof buf, "%d", v);
    col_str(buf, width);
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

// NOTE for anything added below: klog_printf() formats into a
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
static void cmd_windows(int json) {
    if (json) {
        klog_write("{\"count\":");
        klog_printf("%d,\"focused\":%d,\"windows\":[", window_count,
                     window_count > 0 ? window_count - 1 : -1);
        for (int i = 0; i < window_count; i++) {
            const struct window *w = &windows[i];
            klog_printf("%s{\"z\":%d,\"title\":\"%s\",\"app\":\"%s\",",
                         i ? "," : "", i, w->title,
                         w->app ? w->app->name : "");
            klog_printf("\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,",
                         w->x, w->y, w->w, w->h);
            klog_printf("\"content\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},",
                         window_content_x(w), window_content_y(w),
                         window_content_w(w), window_content_h(w));
            // client_pid is 0 for a kernel-space window and the owning
            // process for a ring-3 one -- which ring a window belongs to
            // is otherwise only visible in the Task Manager's own text,
            // so a test asserting "this really is a ring-3 client" would
            // have nothing to read.
            klog_printf("\"client_pid\":%d,", w->client_pid);
            klog_printf("\"state\":\"%s\",\"focused\":%s,\"resizable\":%s}",
                         state_name(w->state),
                         (i == window_count - 1) ? "true" : "false",
                         w->resizable ? "true" : "false");
        }
        klog_write("]}\r\n");
        return;
    }

    klog_printf("%d window(s), z-order bottom to top; the top one has focus\r\n",
                 window_count);
    if (window_count == 0) return;
    klog_write("  z  title           x    y    w    h  | content x/y/w/h    state\r\n");
    for (int i = 0; i < window_count; i++) {
        const struct window *w = &windows[i];
        klog_write("  ");
        col_int(i, 3);
        col_str(w->title, 16);
        col_int(w->x, 5); col_int(w->y, 5); col_int(w->w, 5); col_int(w->h, 5);
        klog_write("| ");
        col_int(window_content_x(w), 5); col_int(window_content_y(w), 5);
        col_int(window_content_w(w), 5); col_int(window_content_h(w), 5);
        klog_write(" ");
        klog_write(state_name(w->state));
        klog_write((i == window_count - 1) ? " (focused)\r\n" : "\r\n");
    }
}

// What is at this point? Answers the question a failed click actually
// raises -- "did my coordinate land where I thought" -- without a
// screenshot. Regions are named the same way wm_input.c's hit-testing
// thinks about them, so the answer maps onto the code that would run.
static void cmd_probe(int px, int py, int json) {
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
        klog_printf("{\"x\":%d,\"y\":%d,\"window\":%d,\"title\":\"%s\","
                     "\"region\":\"%s\",\"overlay\":\"%s\"", px, py, hit,
                     hit >= 0 ? windows[hit].title : "", region, overlay);
        if (hit >= 0) {
            const struct window *w = &windows[hit];
            klog_printf(",\"content_rel\":{\"x\":%d,\"y\":%d}",
                         px - window_content_x(w), py - window_content_y(w));
        }
        klog_write("}\r\n");
        return;
    }

    klog_printf("(%d,%d): %s", px, py, region);
    if (hit >= 0) {
        const struct window *w = &windows[hit];
        klog_printf(" of window %d \"%s\"; content-relative (%d,%d)",
                     hit, w->title,
                     px - window_content_x(w), py - window_content_y(w));
    }
    if (k_strcmp(overlay, "none") != 0) {
        klog_printf("  [%s is above everything and would take this click]", overlay);
    }
    klog_write("\r\n");
}

// Start menu rows, as the kernel computes them -- the numbers
// tools/gui_flow.py used to hardcode.
static void cmd_menu(int json) {
    int mx, my, mw, item_h, total;
    start_menu_geometry(&mx, &my, &mw, &item_h, &total);

    if (json) {
        klog_printf("{\"open\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"item_h\":%d,\"rows\":[",
                     start_menu_open ? "true" : "false", mx, my, mw, item_h);
        for (int i = 0; i < gui_app_registry_count; i++) {
            klog_printf("%s{\"label\":\"%s\",\"kind\":\"app\",\"y\":%d,\"cy\":%d}",
                         i ? "," : "", gui_app_registry[i].name,
                         my + i * item_h, my + i * item_h + item_h / 2);
        }
        for (int i = 0; i < wm_system_action_count; i++) {
            int row = gui_app_registry_count + i;
            klog_printf(",{\"label\":\"%s\",\"kind\":\"action\",\"y\":%d,\"cy\":%d}",
                         wm_system_actions[i].label,
                         my + row * item_h, my + row * item_h + item_h / 2);
        }
        klog_write("]}\r\n");
        return;
    }

    klog_printf("start menu: %s, x=%d y=%d w=%d item_h=%d rows=%d\r\n",
                 start_menu_open ? "open" : "closed", mx, my, mw, item_h, total);
    for (int i = 0; i < gui_app_registry_count; i++) {
        klog_write("  row "); col_int(i, 3);
        klog_write("y="); col_int(my + i * item_h, 6);
        klog_write("centre="); col_int(my + i * item_h + item_h / 2, 6);
        klog_write("app     "); klog_write(gui_app_registry[i].name);
        klog_write("\r\n");
    }
    for (int i = 0; i < wm_system_action_count; i++) {
        int row = gui_app_registry_count + i;
        klog_write("  row "); col_int(row, 3);
        klog_write("y="); col_int(my + row * item_h, 6);
        klog_write("centre="); col_int(my + row * item_h + item_h / 2, 6);
        klog_write("action  "); klog_write(wm_system_actions[i].label);
        klog_write("\r\n");
    }
}

// Taskbar: the Start button and one button per open window, with the
// rects wm_render.c actually draws and wm_input.c actually hit-tests.
static void cmd_taskbar(int json) {
    int bar_y = screen_h - taskbar_h;
    int sw = start_btn_w();
    int bw = win_btn_w();

    if (json) {
        klog_printf("{\"y\":%d,\"h\":%d,\"start\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                     "\"cx\":%d,\"cy\":%d},\"buttons\":[",
                     bar_y, taskbar_h, 0, bar_y, sw, taskbar_h,
                     sw / 2, bar_y + taskbar_h / 2);
        for (int i = 0; i < window_count; i++) {
            int bx = sw + 4 + i * (bw + 4);
            klog_printf("%s{\"index\":%d,\"title\":\"%s\",\"x\":%d,\"w\":%d,"
                         "\"cx\":%d,\"cy\":%d}",
                         i ? "," : "", i, windows[i].title, bx, bw,
                         bx + bw / 2, bar_y + taskbar_h / 2);
        }
        klog_write("]}\r\n");
        return;
    }

    klog_printf("taskbar: y=%d h=%d\r\n", bar_y, taskbar_h);
    klog_write("  start   x="); col_int(0, 6);
    klog_write("w="); col_int(sw, 6);
    klog_printf("centre=(%d,%d)\r\n", sw / 2, bar_y + taskbar_h / 2);
    for (int i = 0; i < window_count; i++) {
        int bx = sw + 4 + i * (bw + 4);
        klog_write("  win "); col_int(i, 4);
        klog_write("x="); col_int(bx, 6);
        klog_write("w="); col_int(bw, 6);
        klog_printf("centre=(%d,%d)  %s\r\n",
                     bx + bw / 2, bar_y + taskbar_h / 2, windows[i].title);
    }
}

// Everything else the WM is holding: which overlays are up, where the
// cursor is, what's armed, and this frame's damage rect. The last one
// is the thing you want when a repaint looks wrong -- it's otherwise
// completely invisible.
static void cmd_state(int json) {
    int cx, cy;
    uint8_t buttons;
    mouse_get_state(&cx, &cy, &buttons);
    int dx, dy, dw, dh;
    wm_debug_damage(&dx, &dy, &dw, &dh);

    if (json) {
        klog_printf("{\"screen\":{\"w\":%d,\"h\":%d},\"taskbar_h\":%d,",
                     screen_w, screen_h, taskbar_h);
        klog_printf("\"cursor\":{\"x\":%d,\"y\":%d,\"buttons\":%u},",
                     cx, cy, (unsigned)buttons);
        klog_printf("\"overlays\":{\"start_menu\":%s,\"context_menu\":%s,",
                     start_menu_open ? "true" : "false",
                     context_menu_open ? "true" : "false");
        klog_printf("\"file_picker\":%s,\"confirm_dialog\":%s},",
                     file_picker_open ? "true" : "false",
                     confirm_dialog_open ? "true" : "false");
        klog_printf("\"dragging\":%d,\"resizing\":%d,\"content_pressed\":%d,",
                     dragging, resizing, content_pressed);
        klog_printf("\"redraw_pending\":%s,\"pending\":%d,",
                     redraw_pending ? "true" : "false", wm_debug_input_pending());
        // The pid of the ring-3 process this desktop is currently
        // running (0 = none) -- wm.c's pending_proc. Exposed because it
        // is the only host-observable way to tell that a client process
        // and the WM are alive AT THE SAME TIME: a test that wants to
        // prove the desktop stays responsive while a process runs has
        // to know the process is still running, and asking the WM is
        // the same "ask, don't measure a screenshot" principle the rest
        // of this file is built on. See tools/sched_gui_test.py.
        klog_printf("\"proc_pid\":%d,", pending_proc);
        klog_printf("\"damage\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}}\r\n",
                     dx, dy, dw, dh);
        return;
    }

    klog_printf("screen %dx%d, taskbar %dpx\r\n", screen_w, screen_h, taskbar_h);
    klog_printf("cursor (%d,%d) buttons=0x%x\r\n", cx, cy, (unsigned)buttons);
    klog_printf("overlays: start_menu=%d context_menu=%d file_picker=%d confirm=%d\r\n",
                 start_menu_open, context_menu_open, file_picker_open, confirm_dialog_open);
    klog_printf("dragging=%d resizing=%d content_pressed=%d redraw_pending=%d\r\n",
                 dragging, resizing, content_pressed, redraw_pending);
    klog_printf("injected events pending: %d\r\n", wm_debug_input_pending());
    if (pending_proc) klog_printf("ring-3 process: pid %d\r\n", pending_proc);
    else              klog_write("ring-3 process: none\r\n");
    klog_printf("damage rect: x=%d y=%d w=%d h=%d%s\r\n", dx, dy, dw, dh,
                 dw <= 0 ? "  (none this frame -- full-screen repaint)" : "");
}

// ---------------------------------------------------------------------
// Driving
// ---------------------------------------------------------------------

static void cmd_open(const char *name) {
    for (int i = 0; i < gui_app_registry_count; i++) {
        if (k_strcmp(gui_app_registry[i].name, name) == 0) {
            open_app(&gui_app_registry[i]);
            klog_printf("gui: opened \"%s\"\r\n", name);
            return;
        }
    }
    klog_printf("gui: no app named \"%s\". Known apps:\r\n", name);
    for (int i = 0; i < gui_app_registry_count; i++) {
        klog_printf("  %s\r\n", gui_app_registry[i].name);
    }
}

static void cmd_close(int index) {
    if (index < 0 || index >= window_count) {
        klog_printf("gui: no window %d (see `gui windows`)\r\n", index);
        return;
    }
    klog_printf("gui: closing window %d \"%s\"\r\n", index, windows[index].title);
    close_window(index);
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
#define DRAG_STEPS 8
static int cmd_drag(int x0, int y0, int x1, int y1) {
    if (!inject_push(x0, y0, 0)) return 0;
    if (!inject_push(x0, y0, 1)) return 0;
    for (int i = 1; i <= DRAG_STEPS; i++) {
        int x = x0 + (x1 - x0) * i / DRAG_STEPS;
        int y = y0 + (y1 - y0) * i / DRAG_STEPS;
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

static void usage(void) {
    klog_write("gui subcommands (all of these work while the desktop is up):\r\n");
    klog_write("  windows [--json]      open windows: rects, content rects, z-order, focus\r\n");
    klog_write("  probe X Y [--json]    what is at this point, and what would take the click\r\n");
    klog_write("  menu [--json]         start menu row geometry, as the kernel computes it\r\n");
    klog_write("  taskbar [--json]      start button + per-window button rects\r\n");
    klog_write("  state [--json]        overlays, cursor, armed state, damage rect\r\n");
    klog_write("  damage [verify on|off]  the damage rect; verify renders every frame\r\n");
    klog_write("                        twice and reports pixels the damage rect missed\r\n");
    klog_write("  apps                  the gui_app registry\r\n");
    klog_write("  open <AppName>        open a window directly (no menu clicking)\r\n");
    klog_write("  close <index>         close window <index> from `gui windows`\r\n");
    klog_write("  move X Y              move the cursor, nothing held (for hover)\r\n");
    klog_write("  click X Y             synthetic press+release at a point\r\n");
    klog_write("  drag X1 Y1 X2 Y2      synthetic press, interpolated move, release\r\n");
    klog_write("  key <c|0xNN>          synthetic keypress to the focused window\r\n");
    klog_write("  wheel <n>             synthetic wheel notches (+up / -down)\r\n");
    klog_write("Injected input enters at the WM loop, below the PS/2 driver -- it tests\r\n");
    klog_write("WM/app logic, not the mouse driver. It is also asynchronous: the events\r\n");
    klog_write("drain one per frame, so allow ~100ms before reading the result back.\r\n");
}

int wm_debug_dispatch(char *line) {
    char *p = line;
    char *sub = next_tok(&p);
    if (!sub) { usage(); return 1; }

    // `rest` is scanned for --json by wants_json(), which consumes it --
    // so grab positional arguments BEFORE asking about the flag.
    if (k_strcmp(sub, "windows") == 0)      { cmd_windows(wants_json(p)); return 1; }
    if (k_strcmp(sub, "menu") == 0)         { cmd_menu(wants_json(p)); return 1; }
    if (k_strcmp(sub, "taskbar") == 0)      { cmd_taskbar(wants_json(p)); return 1; }
    if (k_strcmp(sub, "state") == 0)        { cmd_state(wants_json(p)); return 1; }

    if (k_strcmp(sub, "damage") == 0) {
        char *arg = next_tok(&p);
        if (arg && k_strcmp(arg, "verify") == 0) {
            char *onoff = next_tok(&p);
            if (!onoff) {
                klog_printf("damage verification is %s\r\n",
                             wm_damage_verify_enabled() ? "on" : "off");
                return 1;
            }
            wm_damage_verify_set(k_strcmp(onoff, "on") == 0);
            return 1;
        }
        int dx, dy, dw, dh;
        wm_debug_damage(&dx, &dy, &dw, &dh);
        klog_printf("damage: x=%d y=%d w=%d h=%d  verify=%s\r\n", dx, dy, dw, dh,
                     wm_damage_verify_enabled() ? "on" : "off");
        return 1;
    }
    if (k_strcmp(sub, "help") == 0)         { usage(); return 1; }

    if (k_strcmp(sub, "apps") == 0) {
        for (int i = 0; i < gui_app_registry_count; i++) {
            klog_write("  ");
            col_str(gui_app_registry[i].name, 16);
            klog_printf("resizable=%d multi_instance=%d\r\n",
                         gui_app_registry[i].resizable,
                         gui_app_registry[i].multi_instance);
        }
        return 1;
    }

    if (k_strcmp(sub, "probe") == 0) {
        char *ax = next_tok(&p), *ay = next_tok(&p);
        int x, y;
        if (!parse_int(ax, &x) || !parse_int(ay, &y)) {
            klog_write("usage: gui probe X Y [--json]\r\n");
            return 1;
        }
        cmd_probe(x, y, wants_json(p));
        return 1;
    }

    if (k_strcmp(sub, "open") == 0) {
        char *name = next_tok(&p);
        if (!name) { klog_write("usage: gui open <AppName>  (see `gui apps`)\r\n"); return 1; }
        // Re-join a two-word name ("Task Manager", "Control Panel") --
        // next_tok() split it, and quoting over a serial line is worse
        // than just gluing the remainder back on.
        if (*p) {
            char *end = name + k_strlen(name);
            *end = ' ';
        }
        cmd_open(name);
        return 1;
    }

    if (k_strcmp(sub, "close") == 0) {
        int idx;
        if (!parse_int(next_tok(&p), &idx)) {
            klog_write("usage: gui close <index>  (see `gui windows`)\r\n");
            return 1;
        }
        cmd_close(idx);
        return 1;
    }

    if (k_strcmp(sub, "click") == 0) {
        int x, y;
        if (!parse_int(next_tok(&p), &x) || !parse_int(next_tok(&p), &y)) {
            klog_write("usage: gui click X Y\r\n");
            return 1;
        }
        klog_printf(cmd_click(x, y) ? "gui: queued click at (%d,%d)\r\n"
                                     : "gui: input queue full, click at (%d,%d) DROPPED\r\n",
                     x, y);
        return 1;
    }

    if (k_strcmp(sub, "drag") == 0) {
        int x0, y0, x1, y1;
        if (!parse_int(next_tok(&p), &x0) || !parse_int(next_tok(&p), &y0) ||
            !parse_int(next_tok(&p), &x1) || !parse_int(next_tok(&p), &y1)) {
            klog_write("usage: gui drag X1 Y1 X2 Y2\r\n");
            return 1;
        }
        klog_printf(cmd_drag(x0, y0, x1, y1)
                     ? "gui: queued drag (%d,%d) -> (%d,%d)\r\n"
                     : "gui: input queue full, drag (%d,%d) -> (%d,%d) DROPPED\r\n",
                     x0, y0, x1, y1);
        return 1;
    }

    if (k_strcmp(sub, "wheel") == 0) {
        klog_write(cmd_wheel(next_tok(&p)) ? "gui: queued wheel\r\n"
                                            : "gui: bad or dropped wheel delta\r\n");
        return 1;
    }

    if (k_strcmp(sub, "move") == 0) {
        char *xs = next_tok(&p), *ys = next_tok(&p);
        int x, y;
        if (!xs || !ys || !parse_int(xs, &x) || !parse_int(ys, &y)) {
            klog_write("usage: gui move X Y\r\n");
            return 1;
        }
        klog_printf(cmd_move(x, y) ? "gui: queued move to (%d,%d)\r\n"
                                    : "gui: move queue full\r\n", x, y);
        return 1;
    }

    if (k_strcmp(sub, "key") == 0) {
        char *k = next_tok(&p);
        klog_write(cmd_key(k, p) ? "gui: queued key\r\n" : "gui: bad or dropped key\r\n");
        return 1;
    }

    return 0;
}
