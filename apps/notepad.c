// A minimal text editor: type, backspace, enter, arrow-key/Home/End
// cursor movement, Delete, click-to-position and click-drag/shift+arrow
// text selection, and a toolbar with Open.../Save As... buttons that
// pop apps/wm/file_picker.h's reusable browser dialog instead of an
// always-visible inline filename field -- see docs/decisions.md. Also
// doubles as the WM's keyboard-focus test: open it alongside About and
// confirm keystrokes always land in whichever window is on top.
//
// Open.../Save As... replaced the old always-visible ui_textbox
// filename field + Save/Load pair (build 490-era) -- file_picker.h's
// first real caller. There's no "current file" tracked between saves
// the way a real editor's plain "Save" (no dialog) would need: every
// Save is a Save As, same as every Open goes through the picker, per
// the user's own choice when this was built (see docs/decisions.md).
// notepad_picker_save_choice()/notepad_picker_open_choice() below are
// the callbacks file_picker_open_with() invokes once a path is chosen.
// Neither Save nor Load calls fs_write()/fs_read() directly anymore --
// notepad_picker_saved()/notepad_picker_opened() write/read steppably
// via wm.h's window_start_write()/window_start_read(), polled once per
// frame by wm_run() instead of blocking the whole desktop (Milestone 1
// phases 3-4, docs/roadmap.md); see notepad_picker_saved()/
// notepad_write_complete() and notepad_picker_opened()/
// notepad_read_complete() below.
//
// Save/Load moved from bare widget_button()/widget_hit() calls to a
// real struct ui_button_group (apps/ui/ui_button_group.h) -- the same
// migration ui_button_group.h's own top comment said was deliberately
// left for later when Calculator got it first; see docs/decisions.md.
//
// Cursor movement (build 377) rides on widgets.h's text_scrollback
// gaining a real cursor -- see its own top comment for why: this file
// was exactly the "second real caller" that justified adding it, next
// to the CLI/GUI-Terminal text editor (apps/editor.c) that motivated
// the work in the first place. One known gap: moving the cursor
// off-screen (e.g. pressing Up repeatedly while scrolled) doesn't
// auto-scroll the view to follow it -- the cursor just becomes
// invisible until scrolled back into view by hand. Not implemented in
// this pass; see CHANGELOG.md's build 377 entry.
//
// Click-to-position and text selection (see this file's docs/decisions.md
// entry) build directly on ui_scrollback.h's click/selection API, added
// alongside this: notepad_drag_start() claims every mouse-down in the
// text body (not just scrollbar-thumb drags) so a plain click and a
// drag-select both fall out of the same code path (a drag that never
// moves IS a click-to-position). Shift+arrow/Home/End extends the
// selection instead of moving a fresh one every keystroke -- see
// notepad_extend_selection().
//
// Phase 4/4 of the scrollbar plan (see CHANGELOG.md builds 263, 273,
// 283 for the first three): converted from a flat char[] + manual
// col/row draw loop to the same struct text_scrollback (widgets.h)
// apps/terminal.c uses. That's the whole point of sharing the widget --
// Page Up/Page Down, the visual draggable scrollbar, the mouse wheel,
// and now click-to-position/selection too all come for free from code
// already written and tested for Terminal (or, for selection, available
// to it later), instead of a second, Notepad-specific implementation.
// Save/Load now serialize the scrollback's ring buffer to/from a flat
// byte stream at the filesystem boundary (see notepad_serialize()/
// notepad_load_text() below) since fs_write()/fs_read() (fs.h) only
// know about flat buffers, not this widget.
#include "notepad.h"
#include "wm/wm.h"
#include "wm/file_picker.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

#define NOTEPAD_DEFAULT_NAME "notepad.txt" // Save As...'s starting suggestion until something's actually been saved/loaded, see this file's top comment
// Macros, not cached constants, so both track gfx_char_w()/gfx_char_h()
// live if the font size changes at runtime (see gfx_set_font_size()) --
// same reasoning as WM_TITLEBAR_H in wm.h.
// ROW_VPAD leaves a couple of real pixels beyond the glyph height so
// button/field borders never paint flush against the toolbar's own
// top/bottom edge. TOOLBAR_H used to be exactly `gfx_char_h() +
// 2*BTN_MARGIN` once BTN_MARGIN's two margins were subtracted back out
// in `bh` (see toolbar_geometry()'s callers below) -- this adds the
// extra gap. See docs/decisions.md.
#define ROW_VPAD 3
#define TOOLBAR_H (gfx_char_h() + 2 * ROW_VPAD + 2 * BTN_MARGIN)
#define BTN_W (10 * gfx_char_w() + 16) // fits "Save As..." (10 chars, the longer of the two labels) at any font size
#define BTN_GAP 8
#define BTN_MARGIN 4
// ui_button `code`s for Open.../Save As... -- app-defined, delivered
// back from ui_button_group_click() (see notepad_click() below), same
// "arbitrary int the caller assigns meaning to" convention
// calculator.c's button codes use.
#define BTN_OPEN_CODE 'O'
#define BTN_SAVEAS_CODE 'S'
#define NOTEPAD_BTN_COUNT 2
#define NOTEPAD_NAME_MAX 64 // FS_PATH_MAX (fs.h) -- the longest path fs_write()/fs_read() will ever hand back
// How many text rows/cols the initial window should comfortably fit --
// text itself always reflows to whatever size the window actually is
// (see widget_scrollback_draw()'s own reflow), this just picks a
// sensible starting size for the current font.
#define NOTEPAD_COLS 44
#define NOTEPAD_ROWS 12
#define STATUS_COLS 14 // room for the longest status text, "No file yet."
// Same pattern as TERM_SCROLLBAR_W/TERM_MIN_W_FOR_SCROLLBAR in
// terminal.c: width of the scrollbar strip reserved along the text
// area's right edge, skipped entirely below a minimum width.
#define NOTEPAD_SCROLLBAR_W (gfx_char_w() + 4)
#define NOTEPAD_MIN_W_FOR_SCROLLBAR (NOTEPAD_SCROLLBAR_W * 3)

// Single static instance -- the window manager only allows one open
// Notepad window at a time (see wm.c's open_app), so this doesn't need
// to be a pool.
struct notepad_state {
    struct text_scrollback tb;
    struct ui_button buttons[NOTEPAD_BTN_COUNT]; // Open..., Save As... -- see this file's top comment on the ui_button_group migration
    struct ui_button_group group;
    char last_name[NOTEPAD_NAME_MAX]; // the last path actually saved/loaded -- Save As...'s starting suggestion (see notepad_click() below); NOTEPAD_DEFAULT_NAME until the first successful Save/Load
    char status[32]; // brief feedback after Save/Load, shown in the toolbar
    int scrollbar_grab_offset; // set by notepad_drag_start(), read by notepad_drag() -- see widgets.h's widget_scrollbar_thumb_rect()
    // Which of the two things a held-down drag is currently doing --
    // notepad_drag_start() claims a mouse-down in either the text body
    // (click-to-position/drag-select) or the scrollbar thumb, and
    // notepad_drag() needs to know which so it updates the right state
    // on every subsequent tick while the button stays held. 1 = dragging
    // a text selection, 0 = dragging the scrollbar thumb.
    int dragging_selection;
    int saving; // 1 while a steppable write started via wm.h's window_start_write() is in flight -- see notepad_picker_saved()/notepad_write_complete() (Milestone 1 phase 3, docs/roadmap.md)
    char pending_save_path[NOTEPAD_NAME_MAX]; // path passed to fs_write_range_begin() -- stashed here because file_picker's `path` argument isn't guaranteed to outlive notepad_picker_saved()'s single call, but notepad_write_complete() (running frames later) still needs it for last_name
    int loading; // 1 while a steppable read started via wm.h's window_start_read() is in flight -- see notepad_picker_opened()/notepad_read_complete() (Milestone 1 phase 4, docs/roadmap.md)
    char pending_load_path[NOTEPAD_NAME_MAX]; // same stashing reason as pending_save_path above, for Open... instead of Save As...
};
static struct notepad_state g_notepad;

// The window to register the pending write against -- captured FRESH in
// notepad_click() when Save As... is pressed (see notepad_click() below),
// NOT once in notepad_open() and reused: `struct window *` isn't a
// stable per-window identity in this WM -- bring_to_front() (wm.c)
// reorders by copying window CONTENTS between fixed array slots, not by
// moving pointers, so a pointer cached once at open time can silently
// end up pointing at a DIFFERENT window after any later reorder (About
// getting clicked in front of Notepad, say). Safe to capture in
// notepad_click() and still be accurate by the time
// notepad_picker_saved() runs frames later: the file picker it opens is
// modal (file_picker_handle_click() is checked before any taskbar/
// window click in wm_input.c's wm_handle_left_click()), so no other
// window can be reordered while it's open. Needed at all because
// notepad_picker_saved() (file_picker.h's on_choose callback) takes no
// window/ctx parameter of its own (see this file's top comment on why:
// only one Notepad window can ever exist) but wm.h's window_start_write()
// needs a `struct window *` to register the pending write against.
static struct window *g_notepad_save_win;

// Same reasoning as g_notepad_save_win above, for Open... instead of
// Save As... -- captured fresh in notepad_click() when Open... is
// pressed, needed by notepad_picker_opened() to call wm.h's
// window_start_read().
static struct window *g_notepad_open_win;

// Scratch buffer for notepad_serialize() (see notepad_click()'s Save
// handler) -- static, not a stack-local, deliberately: the WM runs on
// the kernel's own boot stack (16KB total, see boot.asm), not a process
// kstack, and putting a SCROLLBACK_CAP-sized (8KB) buffer on it would
// eat half of that in one local array on top of whatever call depth
// already got here. A second static instance is fine for the same
// reason g_notepad itself is: only one Notepad window can ever be open
// (see wm.c's open_app), so there's nothing to make reentrant. Must stay
// unchanged for the whole duration of a save now that Save As... writes
// steppably (fs_write_range_begin()'s own contract) -- safe because
// nothing re-serializes into it until the previous write reaches a
// terminal result (the Save As... button is disabled meanwhile, see
// notepad_picker_saved()).
static char g_save_buf[SCROLLBACK_CAP];

// Scratch buffer for notepad_picker_opened()/notepad_read_complete()'s
// steppable Open... (Milestone 1 phase 4, docs/roadmap.md) -- same
// static-not-stack reasoning as g_save_buf above. Must stay unchanged
// for the whole duration of a load, same as g_save_buf must during a
// save: safe because nothing re-reads into it until the previous read
// reaches a terminal result (the Open... button is disabled meanwhile,
// see notepad_picker_opened()).
static char g_load_buf[SCROLLBACK_CAP];

// Content-area size for the current font -- see gui_apps.h's
// default_size. Width is whichever of "toolbar + status text" or
// "NOTEPAD_COLS of text" is wider, so the toolbar never feels cramped
// even though the text area itself is fully dynamic.
void notepad_default_size(int *w, int *h) {
    int toolbar_w = 2 * BTN_MARGIN + 2 * BTN_W + BTN_GAP + STATUS_COLS * gfx_char_w();
    int text_w = NOTEPAD_COLS * gfx_char_w();
    *w = toolbar_w > text_w ? toolbar_w : text_w;
    *h = TOOLBAR_H + NOTEPAD_ROWS * gfx_char_h();
}

void notepad_open(struct window *win) {
    widget_scrollback_init(&g_notepad.tb);
    widget_scrollback_set_color(&g_notepad.tb, VGA_BLACK); // near-black-on-white, not the terminal's light-grey-on-black

    uint32_t btn_bg = gfx_rgb(200, 200, 212);
    ui_button_init(&g_notepad.buttons[0], 0, 0, 0, 0, "Open...", btn_bg, THEME_TEXT, BTN_OPEN_CODE);
    ui_button_init(&g_notepad.buttons[1], 0, 0, 0, 0, "Save As...", btn_bg, THEME_TEXT, BTN_SAVEAS_CODE);
    ui_button_group_init(&g_notepad.group, g_notepad.buttons, NOTEPAD_BTN_COUNT);

    k_strcpy(g_notepad.last_name, NOTEPAD_DEFAULT_NAME);
    g_notepad.status[0] = '\0';
    g_notepad.dragging_selection = 0;
    g_notepad.saving = 0;
    g_notepad.pending_save_path[0] = '\0';
    g_notepad.loading = 0;
    g_notepad.pending_load_path[0] = '\0';
    window_set_state(win, &g_notepad);
}

// Splits the text area (below the toolbar) into the text region and (if
// there's room) the scrollbar strip -- shared by notepad_draw(),
// notepad_key()'s Page Up/Down handling, and the click/drag handlers
// below, so all four agree on exactly the same geometry
// widget_scrollback_draw() actually used to render. Mirrors terminal.c's
// term_layout(); out_text_h excludes the toolbar row the same way
// out_ch there is the whole content area (Terminal has no toolbar).
static void notepad_layout(struct window *win, int *out_text_w, int *out_text_h, int *out_show_scrollbar) {
    int cw = window_content_w(win);
    *out_text_h = window_content_h(win) - TOOLBAR_H;
    if (cw > NOTEPAD_MIN_W_FOR_SCROLLBAR) {
        *out_show_scrollbar = 1;
        *out_text_w = cw - NOTEPAD_SCROLLBAR_W;
    } else {
        *out_show_scrollbar = 0;
        *out_text_w = cw;
    }
}

// Shared by draw_toolbar() and every toolbar click handler below, so
// they all agree on exactly the same x positions -- same pattern
// notepad_layout() already uses for the text area's geometry.
static void toolbar_geometry(int *out_open_x, int *out_saveas_x) {
    *out_open_x = BTN_MARGIN;
    *out_saveas_x = *out_open_x + BTN_W + BTN_GAP;
}

// Refreshes Open.../Save As...'s geometry from toolbar_geometry() --
// font-size-dependent (BTN_W reads gfx_char_w() live), so this needs to
// re-run before every draw/press/click, not just once at open. Same
// reasoning as calculator.c's calculator_layout().
static void notepad_layout_buttons(struct notepad_state *st) {
    int open_x0, saveas_x0;
    toolbar_geometry(&open_x0, &saveas_x0);
    int bh = TOOLBAR_H - 2 * BTN_MARGIN;
    ui_button_set_geometry(&st->buttons[0], open_x0, BTN_MARGIN, BTN_W, bh);
    ui_button_set_geometry(&st->buttons[1], saveas_x0, BTN_MARGIN, BTN_W, bh);
}

static void draw_toolbar(struct window *win, struct notepad_state *st,
                          int cx, int cy, int cw, uint32_t fg) {
    uint32_t toolbar_bg = THEME_BUTTON_BG;
    gfx_fill_rect(cx, cy, cw, TOOLBAR_H, toolbar_bg);

    int by = cy + BTN_MARGIN;

    int open_x0, saveas_x0;
    toolbar_geometry(&open_x0, &saveas_x0);

    notepad_layout_buttons(st);
    ui_button_group_draw(&st->group, cx, cy);

    if (st->status[0]) {
        gfx_draw_string(cx + saveas_x0 + BTN_W + 12, by, st->status, fg, toolbar_bg);
    }
    (void)win;
}

void notepad_draw(struct window *win) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);

    uint32_t bg = THEME_WHITE;
    uint32_t fg = THEME_TEXT;

    draw_toolbar(win, st, cx, cy, cw, fg);

    int text_y = cy + TOOLBAR_H;
    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);

    widget_scrollback_draw(&st->tb, cx, text_y, text_w, text_h, bg, THEME_SELECTION_BG, 1);

    if (show_scrollbar) {
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
        widget_scrollbar_draw(cx + text_w, text_y, NOTEPAD_SCROLLBAR_W, text_h, total_lines, visible_rows,
                               st->tb.scroll_offset, gfx_rgb(225, 225, 230), gfx_rgb(150, 150, 160));
    }
}

// Called at the start of a shift+arrow/Home/End key so the selection
// keeps growing from wherever it started, instead of re-anchoring at
// the cursor's CURRENT position on every single shift+key press (which
// would make the selection always exactly one character/line, never
// accumulate). A plain (non-shift) cursor move should call
// widget_scrollback_selection_clear() instead -- see notepad_key() below.
static void notepad_extend_selection(struct notepad_state *st) {
    if (!st->tb.sel_active) widget_scrollback_selection_start(&st->tb);
}

void notepad_key(struct window *win, int key) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);

    // No filename field to route keys to anymore -- Open.../Save As...
    // pop file_picker.h's own dialog instead, which captures keyboard
    // input itself while open (wm.c's main loop routes to it before
    // this app's on_key ever runs, see file_picker.h). Every key here
    // is unconditionally text-body input now.
    if (key == '\b') {
        if (widget_scrollback_selection_present(&st->tb)) widget_scrollback_delete_selection(&st->tb);
        else widget_scrollback_backspace_at_cursor(&st->tb);
    } else if (key == KEY_DELETE) {
        if (widget_scrollback_selection_present(&st->tb)) widget_scrollback_delete_selection(&st->tb);
        else widget_scrollback_delete_at_cursor(&st->tb);
    } else if (key == '\r' || key == '\n') {
        if (widget_scrollback_selection_present(&st->tb)) widget_scrollback_delete_selection(&st->tb);
        widget_scrollback_insert_at_cursor(&st->tb, '\n');
    } else if (key == KEY_ARROW_LEFT) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_left(&st->tb);
    } else if (key == KEY_ARROW_RIGHT) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_right(&st->tb);
    } else if (key == KEY_ARROW_UP) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_up(&st->tb);
    } else if (key == KEY_ARROW_DOWN) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_down(&st->tb);
    } else if (key == KEY_HOME) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_home(&st->tb);
    } else if (key == KEY_END) {
        widget_scrollback_selection_clear(&st->tb);
        widget_scrollback_cursor_end(&st->tb);
    } else if (key == KEY_SHIFT_ARROW_LEFT) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_left(&st->tb);
    } else if (key == KEY_SHIFT_ARROW_RIGHT) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_right(&st->tb);
    } else if (key == KEY_SHIFT_ARROW_UP) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_up(&st->tb);
    } else if (key == KEY_SHIFT_ARROW_DOWN) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_down(&st->tb);
    } else if (key == KEY_SHIFT_HOME) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_home(&st->tb);
    } else if (key == KEY_SHIFT_END) {
        notepad_extend_selection(st);
        widget_scrollback_cursor_end(&st->tb);
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // Same "one screenful minus a line of overlap" convention as
        // terminal.c's KEY_PAGE_UP/KEY_PAGE_DOWN handling.
        int text_w, text_h, show_scrollbar;
        notepad_layout(win, &text_w, &text_h, &show_scrollbar);
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
        int page = visible_rows > 1 ? visible_rows - 1 : 1;
        widget_scrollback_scroll(&st->tb, key == KEY_PAGE_UP ? page : -page);
        window_invalidate(win);
        return; // paging doesn't touch st->status/the text itself
    } else if (IS_PRINTABLE_KEY(key)) {
        if (widget_scrollback_selection_present(&st->tb)) widget_scrollback_delete_selection(&st->tb);
        widget_scrollback_insert_at_cursor(&st->tb, (char)key);
    } else {
        return; // ignore other control codes for this simple version
    }

    // Typing invalidates any stale "Saved."/"Loaded." -- but not
    // "Saving..."/"Loading..." itself: an operation in progress keeps
    // showing that (it's still true) rather than going blank until it
    // completes.
    if (!st->saving && !st->loading) st->status[0] = '\0';
    window_invalidate(win);
}

// Flattens the scrollback's ring buffer into a plain null-terminated
// byte string -- fs_write() (fs.h) only knows how to persist flat
// buffers, not this widget, and reaches into tb's buf/start/count
// fields directly (same as terminal.c's scrollbar code already reads
// tb.scroll_offset directly -- struct text_scrollback is a plain public
// struct, not an opaque handle). `max` should leave room for the
// trailing '\0'; returns the number of characters written (excluding
// it).
static int notepad_serialize(struct text_scrollback *tb, char *out, int max) {
    int n = 0;
    for (int i = 0; i < tb->count && n < max - 1; i++) {
        out[n++] = tb->buf[(tb->start + i) % SCROLLBACK_CAP].ch;
    }
    out[n] = '\0';
    return n;
}

// Inverse of notepad_serialize(): clears the scrollback and re-appends
// `data` one character at a time via the normal putc path, so loaded
// text goes through exactly the same code a typed character would
// (same ring-buffer-full behavior if a saved file somehow exceeds
// SCROLLBACK_CAP, same coloring).
static void notepad_load_text(struct text_scrollback *tb, const char *data, uint32_t n) {
    widget_scrollback_clear(tb);
    for (uint32_t i = 0; i < n && data[i] != '\0'; i++) {
        widget_scrollback_putc(tb, data[i]);
    }
}

// file_picker.h's on_choose/on_cancel callbacks for Open.../Save
// As... -- see this file's top comment. No ctx parameter needed
// (file_picker.h's callbacks are plain function pointers) for the same
// reason g_notepad itself is a bare static struct: only one Notepad
// window can ever be open (wm.c's open_app), so there's nothing to
// disambiguate between instances. redraw_pending is already set by the
// time either of these runs (file_picker.c sets it when the dialog
// closes, before invoking the callback), so there's no window handle
// to invalidate here -- the next frame redraws Notepad's toolbar/status
// along with everything else.
static void notepad_picker_cancelled(void) {
    // Nothing to undo -- the dialog already closed itself.
}

// Save As... -- Milestone 1 phase 3 (docs/roadmap.md): writes steppably
// via wm.h's window_start_write() instead of blocking on fs_write(), so
// wm_run() keeps the desktop responsive (redrawing, routing input to
// OTHER windows) while a large save is still in progress. The Save
// As... button is disabled the whole time (see notepad_write_complete()
// re-enabling it) so this can't be re-entered -- guarded here too
// (window_write_pending()) as cheap insurance against that invariant
// ever slipping, not because it's expected to trigger.
static void notepad_picker_saved(const char *path) {
    if (window_write_pending()) {
        k_strcpy(g_notepad.status, "Save failed.");
        return;
    }

    int n = notepad_serialize(&g_notepad.tb, g_save_buf, sizeof(g_save_buf));

    // fs_write_range_begin()/step() extend a file but never shrink it
    // (unlike fs_write()'s own free_all_blocks()-then-write truncation --
    // see tfs.c's tfs_write()), so overwriting an existing longer file
    // with shorter text would otherwise leave its old trailing bytes on
    // disk. Delete first to reclaim it -- fails harmlessly (return value
    // ignored) the first time `path` is saved, when it doesn't exist yet.
    fs_delete(path);

    void *h = fs_write_range_begin(path, 0, g_save_buf, (uint32_t)n);
    if (!h || !window_start_write(g_notepad_save_win, h)) {
        k_strcpy(g_notepad.status, "Save failed.");
        return;
    }

    k_strcpy(g_notepad.pending_save_path, path);
    g_notepad.saving = 1;
    ui_button_set_disabled(&g_notepad.buttons[1], 1);
    k_strcpy(g_notepad.status, "Saving...");
}

// wm.h's window_start_write() callback (via gui_apps.h's
// on_write_complete) -- fires once wm_run()'s per-frame poll of the
// write started above reaches FS_STEP_DONE or FS_STEP_FAILED.
void notepad_write_complete(struct window *win, int success) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    st->saving = 0;
    ui_button_set_disabled(&st->buttons[1], 0);
    if (success) {
        k_strcpy(st->status, "Saved.");
        k_strcpy(st->last_name, st->pending_save_path);
    } else {
        k_strcpy(st->status, "Save failed.");
    }
    window_invalidate(win);
}

// Open... -- Milestone 1 phase 4 (docs/roadmap.md): reads steppably via
// wm.h's window_start_read() instead of blocking on fs_read(), same
// reasoning as notepad_picker_saved()'s Save As... below. The Open...
// button is disabled the whole time (see notepad_read_complete()
// re-enabling it) so this can't be re-entered -- guarded here too
// (window_read_pending()) as cheap insurance, same spirit as
// notepad_picker_saved()'s own guard.
static void notepad_picker_opened(const char *path) {
    if (window_read_pending()) {
        k_strcpy(g_notepad.status, "Load failed.");
        return;
    }
    if (!fs_exists(path) || fs_is_dir(path)) {
        k_strcpy(g_notepad.status, "Load failed.");
        return;
    }

    uint64_t size64 = fs_size(path);
    uint32_t size = (size64 > sizeof(g_load_buf)) ? (uint32_t)sizeof(g_load_buf) : (uint32_t)size64;

    if (size == 0) {
        // Nothing to step -- an empty file is a valid, instant load,
        // same as fs_read() handing back a 0-byte buffer used to be.
        notepad_load_text(&g_notepad.tb, g_load_buf, 0);
        k_strcpy(g_notepad.status, "Loaded.");
        k_strcpy(g_notepad.last_name, path);
        return;
    }

    void *h = fs_read_range_begin(path, 0, g_load_buf, size);
    if (!h || !window_start_read(g_notepad_open_win, h)) {
        k_strcpy(g_notepad.status, "Load failed.");
        return;
    }

    k_strcpy(g_notepad.pending_load_path, path);
    g_notepad.loading = 1;
    ui_button_set_disabled(&g_notepad.buttons[0], 1);
    k_strcpy(g_notepad.status, "Loading...");
}

// wm.h's window_start_read() callback (via gui_apps.h's
// on_read_complete) -- fires once wm_run()'s per-frame poll of the
// read started above reaches FS_STEP_DONE or FS_STEP_FAILED. `total`
// is how many bytes actually landed in g_load_buf -- see
// fs.h's fs_read_range_step() for why this can't just be `size` from
// notepad_picker_opened() above (an implicit contract here: nothing
// changes the target file's size between begin() and this callback, so
// in practice `total` always equals that `size`, but reading it back
// from the callback rather than re-deriving it is the honest contract
// fs_read_range_step() actually offers).
void notepad_read_complete(struct window *win, int success, uint32_t total) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    st->loading = 0;
    ui_button_set_disabled(&st->buttons[0], 0);
    if (success) {
        notepad_load_text(&st->tb, g_load_buf, total);
        k_strcpy(st->status, "Loaded.");
        k_strcpy(st->last_name, st->pending_load_path);
    } else {
        k_strcpy(st->status, "Load failed.");
    }
    window_invalidate(win);
}

// Toolbar (Open.../Save As...) clicks, and scrollbar track clicks (page
// up/down) that aren't on the thumb -- thumb clicks never reach here,
// they're claimed by notepad_drag_start() instead (see gui_apps.h's
// on_click/on_drag_start contract). Mirrors terminal.c's terminal_click.
void notepad_click(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);

    if (cy < TOOLBAR_H) {
        notepad_layout_buttons(st);
        int code = ui_button_group_click(&st->group, cx, cy);
        if (code == BTN_OPEN_CODE) {
            g_notepad_open_win = win; // fresh, live pointer -- see its own comment on why this can't be cached earlier
            file_picker_open_with(FILE_PICKER_OPEN, "Open", "/", "",
                                   notepad_picker_opened, notepad_picker_cancelled);
        } else if (code == BTN_SAVEAS_CODE) {
            g_notepad_save_win = win; // fresh, live pointer -- see its own comment on why this can't be cached earlier
            file_picker_open_with(FILE_PICKER_SAVE, "Save As", "/", st->last_name,
                                   notepad_picker_saved, notepad_picker_cancelled);
        }
        window_invalidate(win);
        return;
    }

    // A click below the toolbar that lands on the scrollbar's track
    // (paging) reaches here -- a click in the text body itself, or on
    // the scrollbar's thumb, is claimed by notepad_drag_start() before
    // on_click ever runs (see gui_apps.h's on_click/on_drag_start
    // contract), so this is only ever SCROLLBAR_ZONE_ABOVE/BELOW in
    // practice.
    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    if (!show_scrollbar) return;

    int local_cy = cy - TOOLBAR_H;
    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, NOTEPAD_SCROLLBAR_W, text_h,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, local_cy);
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

// gui_apps.h's on_press: called every tick a button is held, starting
// with the initial button-down -- see calculator_press()'s own comment
// for the full contract (this is the identical wrapper shape, just over
// Notepad's 2-button group instead of Calculator's grid). Toolbar
// clicks never reach notepad_drag_start() (it bails out at `cy <
// TOOLBAR_H` before any of this), so on_press/on_click still see every
// Save/Load interaction exactly as before.
int notepad_press(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    if (cy >= TOOLBAR_H) return 0; // only the toolbar's buttons care about press-feedback
    notepad_layout_buttons(st);
    return ui_button_group_press(&st->group, cx, cy);
}

void notepad_release(struct window *win) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    ui_button_group_release(&st->group);
    window_invalidate(win);
}

int notepad_drag_start(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    if (cy < TOOLBAR_H) return 0; // toolbar clicks are ordinary clicks, never a drag

    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    int local_cy = cy - TOOLBAR_H;

    if (show_scrollbar && cx >= text_w) {
        // The scrollbar strip -- unchanged from before click-to-position/
        // selection existed: only a thumb hit claims the drag (track
        // clicks page instantly via notepad_click() instead, no drag
        // needed for those).
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
        enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, NOTEPAD_SCROLLBAR_W, text_h,
                                                          total_lines, visible_rows, st->tb.scroll_offset, cx, local_cy);
        if (zone != SCROLLBAR_ZONE_THUMB) return 0;

        int thumb_y, thumb_h;
        widget_scrollbar_thumb_rect(0, text_h, total_lines, visible_rows, st->tb.scroll_offset, &thumb_y, &thumb_h);
        st->scrollbar_grab_offset = local_cy - thumb_y;
        st->dragging_selection = 0;
        return 1;
    }

    // The text body itself -- claim every mouse-down here as a drag,
    // not just the ones that turn out to move: this is what gives both
    // "click to position the cursor" (a drag that never moves is
    // indistinguishable from a plain click -- the selection anchor and
    // cursor end up equal, which widget_scrollback_selection_present()
    // treats as no selection at all) and "drag to select" for free from
    // the same code path, rather than needing separate on_click handling
    // for the no-movement case.
    widget_scrollback_selection_clear(&st->tb);
    st->tb.cursor = widget_scrollback_index_at_point(&st->tb, 0, 0, text_w, text_h, cx, local_cy);
    widget_scrollback_selection_start(&st->tb);
    st->dragging_selection = 1;
    window_invalidate(win);
    return 1;
}

void notepad_drag(struct window *win, int cx, int cy) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    int text_w, text_h, show_scrollbar;
    notepad_layout(win, &text_w, &text_h, &show_scrollbar);
    int local_cy = cy - TOOLBAR_H;

    if (st->dragging_selection) {
        st->tb.cursor = widget_scrollback_index_at_point(&st->tb, 0, 0, text_w, text_h, cx, local_cy);
        window_invalidate(win);
        return;
    }

    (void)show_scrollbar; // a scrollbar drag only ever starts while true; harmless either way if the window shrank mid-drag
    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, text_h, &total_lines, &visible_rows);
    st->tb.scroll_offset = widget_scrollbar_offset_for_drag(0, text_h, total_lines, visible_rows,
                                                              local_cy, st->scrollbar_grab_offset);
    window_invalidate(win);
}

// 3 lines per notch -- same convention as terminal.c's terminal_wheel.
#define NOTEPAD_WHEEL_LINES 3

void notepad_wheel(struct window *win, int delta) {
    struct notepad_state *st = (struct notepad_state *)window_get_state(win);
    widget_scrollback_scroll(&st->tb, delta * NOTEPAD_WHEEL_LINES);
    window_invalidate(win);
}
