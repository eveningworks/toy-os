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
// the callbacks file_picker_open_with() invokes once a path is chosen;
// the actual fs_write()/fs_read() calls are unchanged from the old
// inline-field version, just moved into those.
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
};
static struct notepad_state g_notepad;

// Scratch buffer for notepad_serialize() (see notepad_click()'s Save
// handler) -- static, not a stack-local, deliberately: the WM runs on
// the kernel's own boot stack (16KB total, see boot.asm), not a process
// kstack, and putting a SCROLLBACK_CAP-sized (8KB) buffer on it would
// eat half of that in one local array on top of whatever call depth
// already got here. A second static instance is fine for the same
// reason g_notepad itself is: only one Notepad window can ever be open
// (see wm.c's open_app), so there's nothing to make reentrant.
static char g_save_buf[SCROLLBACK_CAP];

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

    st->status[0] = '\0'; // typing invalidates any stale "Saved."/"Loaded."
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

static void notepad_picker_saved(const char *path) {
    notepad_serialize(&g_notepad.tb, g_save_buf, sizeof(g_save_buf));
    if (fs_write(path, g_save_buf, 0)) {
        k_strcpy(g_notepad.status, "Saved.");
        k_strcpy(g_notepad.last_name, path);
    } else {
        k_strcpy(g_notepad.status, "Save failed.");
    }
}

static void notepad_picker_opened(const char *path) {
    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (data) {
        notepad_load_text(&g_notepad.tb, data, size);
        k_strcpy(g_notepad.status, "Loaded.");
        k_strcpy(g_notepad.last_name, path);
    } else {
        k_strcpy(g_notepad.status, "Load failed.");
    }
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
            file_picker_open_with(FILE_PICKER_OPEN, "Open", "/", "",
                                   notepad_picker_opened, notepad_picker_cancelled);
        } else if (code == BTN_SAVEAS_CODE) {
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
