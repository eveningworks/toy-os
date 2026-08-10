// A GUI terminal-emulator app -- phase 4/4 of the plan started at
// CHANGELOG.md build 183 (see that entry, and builds 193/203, for the
// three prerequisite phases this is built on): a vga_sink (vga.h)
// redirecting console output into a text_scrollback widget (widgets.h),
// driving the real shell_dispatch() (shell.h) from keystrokes instead
// of duplicating shell.c's ~40 command handlers.
//
// NOT supported, on purpose (see BLOCKED_CMDS below): commands that
// never return (`ring3test`, `elftest`), that take over the whole
// physical screen by drawing straight to the framebuffer instead of
// going through the console/sink (`gui`, `guitest`, `wintest`), that
// block the calling context for their whole run without yielding back
// to the window manager (`schedtest`, `echotest`), or that would
// recursively re-enter a blocking loop from inside this window's
// on_key callback (`run`, which could launch `shell` or `gui` again).
// Typing one of these prints an explanation instead of running it --
// see build 193's CHANGELOG entry for the full reasoning. Everything
// else (including `crashtest`, `filetest`, `newsyscalltest`,
// `syscalltest`, `writetest`, `ptrtest`, `reboot`, and ordinary
// commands like `ls`/`cat`/`cd`) runs for real through shell_dispatch(),
// because SYS_WRITE (the only way those ring-3 processes print) already
// goes through vga_putc() -- and vga_putc() already respects whatever
// sink is active (see vga.h's struct vga_sink comment) -- with zero
// terminal-specific code needed for any of them.
#include "terminal.h"
#include "wm/wm.h"
#include "widgets.h"
#include "shell.h"
#include "kapi.h"

#define TERM_LINE_MAX 128
#define TERM_HISTORY_MAX 8
// How many text rows/cols the initial window should comfortably fit --
// same idea as notepad.c's NOTEPAD_COLS/ROWS: just a starting size for
// the current font, not a hard limit (the scrollback widget reflows to
// whatever size the window actually is on every draw -- see
// widgets.h's comment on why that's cheap enough to just always do).
#define TERM_COLS 70
#define TERM_ROWS 20
// Width of the scrollbar strip reserved along the content area's right
// edge -- scales with font size like everything else here (see
// TOOLBAR_H in notepad.c for the same pattern). Below TERM_MIN_W_FOR_SCROLLBAR
// (an arbitrarily-picked "would leave basically no room for text" content
// width), the scrollbar is skipped entirely and text uses the full width --
// see term_layout().
#define TERM_SCROLLBAR_W (gfx_char_w() + 4)
#define TERM_MIN_W_FOR_SCROLLBAR (TERM_SCROLLBAR_W * 3)

// Single static instance -- like every other GUI app here, only one
// window of it can be open at a time (see wm.c's open_app).
struct terminal_state {
    struct text_scrollback tb;
    char line[TERM_LINE_MAX];
    int line_len;
    char history[TERM_HISTORY_MAX][TERM_LINE_MAX];
    int history_count;
    int hist_index;       // like shell.c's shell_read_line(): one past the
                           // newest = "current blank/in-progress line"
    char saved_current[TERM_LINE_MAX];
    int scrollbar_grab_offset; // set by terminal_drag_start(), read by terminal_drag() -- see widgets.h's widget_scrollbar_thumb_rect()
};
static struct terminal_state g_terminal;

static const char *const BLOCKED_CMDS[] = {
    "gui", "run", "ring3test", "elftest", "guitest", "wintest", "schedtest", "echotest",
};
#define BLOCKED_CMD_COUNT (sizeof(BLOCKED_CMDS) / sizeof(BLOCKED_CMDS[0]))

static int is_blocked_command(const char *cmd) {
    for (unsigned i = 0; i < BLOCKED_CMD_COUNT; i++) {
        if (k_strcmp(cmd, BLOCKED_CMDS[i]) == 0) return 1;
    }
    return 0;
}

static void term_write(struct terminal_state *st, const char *s, enum vga_color fg) {
    widget_scrollback_set_color(&st->tb, fg);
    for (const char *p = s; *p; p++) widget_scrollback_putc(&st->tb, *p);
}

static void term_print_prompt(struct terminal_state *st) {
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREEN);
    for (const char *p = shell_cwd(); *p; p++) widget_scrollback_putc(&st->tb, *p);
    widget_scrollback_putc(&st->tb, '>');
    widget_scrollback_putc(&st->tb, ' ');
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
}

// ---- vga_sink glue: lets shell_dispatch()'s vga_write()/vga_putc()/
// etc calls land in this window's scrollback widget instead of the
// physical console (see vga.h's struct vga_sink comment for the
// mechanism). `ctx` is always &g_terminal.tb.
static void sink_putc(void *ctx, char c) { widget_scrollback_putc((struct text_scrollback *)ctx, c); }
static void sink_backspace(void *ctx) { widget_scrollback_backspace((struct text_scrollback *)ctx); }
static void sink_clear(void *ctx) { widget_scrollback_clear((struct text_scrollback *)ctx); }
static void sink_set_color(void *ctx, enum vga_color fg, enum vga_color bg) {
    (void)bg; // the scrollback widget tracks one color per cell, no per-cell background yet
    widget_scrollback_set_color((struct text_scrollback *)ctx, fg);
}
static uint32_t sink_rows(void *ctx) {
    // Only consulted by console_page() (shell.c's `help` pager) when no
    // sink is active being false -- but console_page() checks
    // vga_sink_active() FIRST and skips straight to unpaginated output
    // whenever a sink (like this one) is installed (see build 193's
    // CHANGELOG entry), so this never actually gets called in practice.
    // Present anyway so the struct vga_sink literal below doesn't need
    // a special-case comment of its own; returns a plausible constant.
    (void)ctx;
    return 24;
}

static void term_history_add(struct terminal_state *st, const char *line) {
    if (k_strlen(line) == 0) return;
    if (st->history_count < TERM_HISTORY_MAX) {
        k_strcpy(st->history[st->history_count], line);
        st->history_count++;
    } else {
        for (int i = 1; i < TERM_HISTORY_MAX; i++) k_strcpy(st->history[i - 1], st->history[i]);
        k_strcpy(st->history[TERM_HISTORY_MAX - 1], line);
    }
}

// Erases the in-progress input line from the scrollback (one
// widget_scrollback_backspace() per character -- same idea as shell.c's
// redraw_line(), just via the widget instead of vga_backspace()) and
// replaces it with `new_line`, used by the up/down history browsing
// below.
static void term_set_line(struct terminal_state *st, const char *new_line) {
    for (int i = 0; i < st->line_len; i++) widget_scrollback_backspace(&st->tb);
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
    int len = 0;
    while (new_line[len] && len < TERM_LINE_MAX - 1) {
        widget_scrollback_putc(&st->tb, new_line[len]);
        st->line[len] = new_line[len];
        len++;
    }
    st->line_len = len;
}

static void term_run_line(struct terminal_state *st, char *line) {
    char cmd[TERM_LINE_MAX];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < (int)sizeof(cmd) - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';

    if (cmd[0] && is_blocked_command(cmd)) {
        term_write(st,
            "Not available in the terminal app -- it either doesn't\n"
            "return, draws straight to the physical screen (bypassing\n"
            "this window), or would recursively re-enter a blocking\n"
            "loop from here. Esc out of the GUI and run it from the\n"
            "physical shell instead.\n",
            VGA_LIGHT_RED);
        return;
    }

    static const struct vga_sink term_sink = {
        .ctx = &g_terminal.tb, .putc = sink_putc, .backspace = sink_backspace,
        .clear = sink_clear, .set_color = sink_set_color, .rows = sink_rows,
    };
    shell_dispatch(line, &term_sink);
}

void terminal_default_size(int *w, int *h) {
    *w = TERM_COLS * gfx_char_w();
    *h = TERM_ROWS * gfx_char_h();
}

// Splits the content area into the text region and (if there's room)
// the scrollbar strip -- shared by terminal_draw(), terminal_key()'s
// Page Up/Down handling, and the drag/click handlers below, so all four
// agree on exactly the same geometry widget_scrollback_draw() actually
// used to render (a mismatch there would mean scrolling by the wrong
// page size, or hit-testing against the wrong column count).
static void term_layout(struct window *win, int *out_text_w, int *out_ch, int *out_show_scrollbar) {
    int cw = window_content_w(win);
    *out_ch = window_content_h(win);
    if (cw > TERM_MIN_W_FOR_SCROLLBAR) {
        *out_show_scrollbar = 1;
        *out_text_w = cw - TERM_SCROLLBAR_W;
    } else {
        *out_show_scrollbar = 0;
        *out_text_w = cw;
    }
}

void terminal_open(struct window *win) {
    widget_scrollback_init(&g_terminal.tb);
    g_terminal.line_len = 0;
    g_terminal.history_count = 0;
    g_terminal.hist_index = 0;
    g_terminal.saved_current[0] = '\0';
    window_set_state(win, &g_terminal);

    term_write(&g_terminal,
        "toy-os terminal -- runs the real shell; type 'help' to get started\n"
        "(a few commands that don't fit inside a window aren't available\n"
        "here -- see README.md's terminal-emulator section)\n",
        VGA_LIGHT_CYAN);
    term_print_prompt(&g_terminal);
}

void terminal_draw(struct window *win) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);

    widget_scrollback_draw(&st->tb, cx, cy, text_w, ch, gfx_rgb(0, 0, 0), 1);

    if (show_scrollbar) {
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
        widget_scrollbar_draw(cx + text_w, cy, TERM_SCROLLBAR_W, ch, total_lines, visible_rows,
                               st->tb.scroll_offset, gfx_rgb(15, 15, 15), gfx_rgb(90, 90, 90));
    }
}

// Scrollbar track clicks that aren't on the thumb (paging up/down) --
// thumb clicks never reach here, they're claimed by terminal_drag_start()
// instead (see gui_apps.h's on_click/on_drag_start contract).
void terminal_click(struct window *win, int cx, int cy) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    if (!show_scrollbar) return;

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, TERM_SCROLLBAR_W, ch,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, cy);
    int page = visible_rows > 1 ? visible_rows - 1 : 1;
    if (zone == SCROLLBAR_ZONE_ABOVE) {
        widget_scrollback_scroll(&st->tb, page);
    } else if (zone == SCROLLBAR_ZONE_BELOW) {
        widget_scrollback_scroll(&st->tb, -page);
    } else {
        return; // click landed in the text area (or the bar isn't shown) -- nothing to do
    }
    window_invalidate(win);
}

int terminal_drag_start(struct window *win, int cx, int cy) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    if (!show_scrollbar) return 0;

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, TERM_SCROLLBAR_W, ch,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, cy);
    if (zone != SCROLLBAR_ZONE_THUMB) return 0;

    int thumb_y, thumb_h;
    widget_scrollbar_thumb_rect(0, ch, total_lines, visible_rows, st->tb.scroll_offset, &thumb_y, &thumb_h);
    st->scrollbar_grab_offset = cy - thumb_y;
    return 1;
}

void terminal_drag(struct window *win, int cx, int cy) {
    (void)cx; // this is a purely vertical scrollbar -- only cy matters
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    (void)show_scrollbar; // a drag only ever starts while true; harmless either way if the window shrank mid-drag

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    st->tb.scroll_offset = widget_scrollbar_offset_for_drag(0, ch, total_lines, visible_rows,
                                                              cy, st->scrollbar_grab_offset);
    window_invalidate(win);
}

void terminal_key(struct window *win, int key) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);

    if (key == '\r' || key == '\n') {
        widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
        widget_scrollback_putc(&st->tb, '\n');
        st->line[st->line_len] = '\0';
        term_history_add(st, st->line);
        term_run_line(st, st->line);
        st->line_len = 0;
        st->hist_index = st->history_count;
        term_print_prompt(st);
    } else if (key == '\b') {
        if (st->line_len > 0) {
            st->line_len--;
            widget_scrollback_backspace(&st->tb);
        }
    } else if (key == KEY_ARROW_UP) {
        if (st->hist_index > 0) {
            if (st->hist_index == st->history_count) {
                st->line[st->line_len] = '\0';
                k_strcpy(st->saved_current, st->line);
            }
            st->hist_index--;
            term_set_line(st, st->history[st->hist_index]);
        }
    } else if (key == KEY_ARROW_DOWN) {
        if (st->hist_index < st->history_count) {
            st->hist_index++;
            const char *replacement = (st->hist_index == st->history_count)
                                           ? st->saved_current
                                           : st->history[st->hist_index];
            term_set_line(st, replacement);
        }
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // Page size is "one screenful minus a line of overlap" -- a
        // common terminal-scrolling convention, and it also means
        // paging down repeatedly can't skip past the bottom in one
        // jump (widget_scrollback_scroll()'s clamp handles the exact
        // boundary either way, this just picks a sensible step size).
        int text_w, ch, show_scrollbar;
        term_layout(win, &text_w, &ch, &show_scrollbar);
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
        int page = visible_rows > 1 ? visible_rows - 1 : 1;
        widget_scrollback_scroll(&st->tb, key == KEY_PAGE_UP ? page : -page);
    } else if (key >= 32 && key < 127 && st->line_len < TERM_LINE_MAX - 1) {
        widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
        widget_scrollback_putc(&st->tb, (char)key);
        st->line[st->line_len++] = (char)key;
    } else {
        return; // unhandled key -- nothing changed, no need to invalidate
    }

    window_invalidate(win);
}
