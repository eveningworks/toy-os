// Notepad, as a RING-3 PROCESS.
//
// The kernel-space original is apps/notepad.c. This is the same editor
// -- wrapped scrollable text, a cursor, click-to-position and
// drag-select, a toolbar, open and save -- running as an ordinary
// ring-3 program over the windowing protocol.
//
// WHAT THE MIGRATION MADE SIMPLER, WHICH IS THE INTERESTING PART
// ---------------------------------------------------------------
// The kernel version cannot block. A GUI app there runs inside
// wm_run()'s loop, so a blocking disk read would freeze the whole
// desktop -- which is why apps/notepad.c carries `window_start_read()`/
// `window_start_write()`, why the WM has pending_read/pending_write
// slots polled once per frame, why loading a file is a state machine
// with an on_read_complete callback, and why close_window() has to
// refuse while I/O is in flight.
//
// None of that is needed here. This is a separate process: it calls
// sys_read() and blocks, and the desktop keeps running because the
// kernel context is a scheduler participant now (Milestone 41 stage 1).
// So the whole stepped-I/O apparatus collapses into an ordinary
// read-the-file-into-a-buffer loop. That is a real architectural
// dividend of moving apps out of the kernel, not a shortcut.
//
// THE FILE DIALOG IS DRAWN BY THIS PROCESS, not the window server.
// apps/wm/file_picker.c is a WM modal built on wm_internal.h and could
// not be ported; more to the point, it SHOULDN'T be. A display server
// has no business owning file dialogs -- GTK and Qt each draw their
// own, and a portal is a later refinement rather than the starting
// point. So the picker below is part of the application, using
// sys_listdir() for the listing.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "keyboard.h" // KEY_* codes, the same ones the WM delivers

#define WIN_W 560
#define WIN_H 380

#define MARGIN 8
#define TOOLBAR_GAP 6

// Toolbar button codes.
#define BTN_NEW  1
#define BTN_OPEN 2
#define BTN_SAVE 3

#define PATH_MAX_LEN 64 // FS_PATH_MAX

static struct utext g_text;
static struct uui_button g_buttons[3];
static struct uui_button_group g_toolbar;

static char g_path[PATH_MAX_LEN];   // "" until saved/opened
static char g_status[96];
static int g_dirty;

// --- the open dialog --------------------------------------------------
//
// A modal panel drawn over the editor, inside this window. Entirely the
// application's own -- see the file header.
#define DIALOG_MAX_FILES 32
static int g_dialog_open;
static struct dirent g_entries[DIALOG_MAX_FILES];
static int g_entry_count;
static int g_sel;
static int g_dialog_saving; // 1 = "Save As" flavour: type a name

static char g_name_field[PATH_MAX_LEN];
static int g_name_len;

// The directory the open dialog is showing. A file dialog that cannot
// leave one directory is not much of a dialog -- selecting a directory
// descends into it, and ".." (synthesised as the first row whenever
// we are below the root) goes back up.
static char g_dialog_dir[PATH_MAX_LEN] = "/";

// --- small helpers (no libc) -----------------------------------------

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static void scopy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void set_status(const char *s) { scopy(g_status, s, (int)sizeof g_status); }

// The title carries the filename and a dirty marker, so it changes as
// the document does. Sent only when it ACTUALLY changed: this used to
// be called before every repaint, which meant a TWP message per
// keystroke to say the same thing.
static char g_shown_title[WIN_TITLE_LEN];

static void set_title(struct uapp *a) {
    char t[WIN_TITLE_LEN];
    const char *name = g_path[0] ? g_path : "untitled";
    int i = 0;
    if (g_dirty && i < WIN_TITLE_LEN - 2) t[i++] = '*';
    for (int j = 0; name[j] && i < WIN_TITLE_LEN - 1; j++) t[i++] = name[j];
    t[i] = '\0';

    for (int j = 0;; j++) {
        if (t[j] != g_shown_title[j]) break;
        if (!t[j]) return; // identical -- nothing to send
    }
    scopy(g_shown_title, t, WIN_TITLE_LEN);
    uapp_set_title(a, t);
}

// --- file I/O ---------------------------------------------------------
//
// Plain blocking calls. See the file header for why that is allowed
// here and is not in the kernel-space version.

static int load_file(const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) { set_status("open failed"); return 0; }

    utext_clear(&g_text);
    char chunk[512];
    for (;;) {
        int64_t n = sys_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) utext_putc(&g_text, chunk[i]);
    }
    sys_close(fd);

    g_text.cursor = 0;
    g_text.scroll_offset = 0;
    utext_sel_clear(&g_text);
    scopy(g_path, path, PATH_MAX_LEN);
    g_dirty = 0;
    set_status("opened");
    return 1;
}

static int save_file(const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { set_status("save failed"); return 0; }

    // Write in chunks rather than a character at a time: one syscall
    // per character would be thousands of kernel entries for a modest
    // file, and SYS_WRITE is capped per call anyway (SYS_WRITE_MAX).
    char chunk[512];
    int n = 0;
    for (int i = 0; i < g_text.count; i++) {
        chunk[n++] = utext_at(&g_text, i);
        if (n == (int)sizeof chunk) {
            if (sys_write(fd, chunk, (size_t)n) < 0) { sys_close(fd); set_status("write failed"); return 0; }
            n = 0;
        }
    }
    if (n > 0 && sys_write(fd, chunk, (size_t)n) < 0) {
        sys_close(fd);
        set_status("write failed");
        return 0;
    }
    sys_close(fd);

    scopy(g_path, path, PATH_MAX_LEN);
    g_dirty = 0;
    set_status("saved");
    return 1;
}

// --- layout -----------------------------------------------------------

static int toolbar_h(void) { return ugfx_char_h() + 16; }

// The scrollbar's width comes from the WIDGET, not from a number here.
// It used to be `#define SCROLLBAR_W 8` with a matching hardcoded "10px
// gutter" in the text rect below -- two constants that had to agree,
// both narrower than the toolkit's own bars, and the reason this one was
// hard to grab with a mouse. See ui/uui_scrollbar.h.
static int scrollbar_w(void) {
    int w;
    uui_scrollbar_natural_size(&w, 0);
    return w;
}

// Width plus the 2px breathing space between the text and the strip, so
// the text rect and the bar's own x can never disagree about the gap.
static int scrollbar_gutter(void) { return scrollbar_w() + 2; }

// Derived from the content SIZE rather than from a surface, because the
// event callbacks need it too and they never hold one. That it derives
// at all is what makes this app resizable with no resize code: a bigger
// window is simply a bigger page.
static void text_rect_for(int cw, int ch, int *x, int *y, int *w, int *h) {
    *x = MARGIN;
    *y = MARGIN + toolbar_h() + TOOLBAR_GAP;
    *w = cw - 2 * MARGIN - scrollbar_gutter(); // room for the scrollbar
    *h = ch - *y - MARGIN - ugfx_char_h() - 4; // status line at the bottom
}

static void text_rect(struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    text_rect_for(s->w, s->h, x, y, w, h);
}

static void layout_toolbar(void) {
    int bw = 8 * ugfx_char_w();
    int bh = toolbar_h();
    for (int i = 0; i < 3; i++) {
        uui_button_set_geometry(&g_buttons[i], MARGIN + i * (bw + TOOLBAR_GAP), MARGIN, bw, bh);
    }
}

// --- drawing ----------------------------------------------------------

// Where the scrollbar strip is, given the text rect. One derivation,
// used by the draw and by the hit test below -- two copies of this is
// the classic way a scrollbar ends up drawing in one place and
// responding in another.
static void scrollbar_rect(int tx, int ty, int tw, int th,
                            int *bx, int *by, int *bw, int *bh) {
    *bx = tx + tw + 2;
    *by = ty;
    *bw = scrollbar_w();
    *bh = th;
}

// Notepad used to hand-roll its scrollbar here -- trough, thumb
// position, thumb height, all recomputed locally -- while
// ui/uui_scrollbar.h had exactly that widget. The copy is why it drew
// something it could never move: none of the widget's hit-testing came
// with it.
//
// Arrows are ON here (UUI_SCROLLBAR_ARROWS): an editor is where a
// stepper is actually wanted, and this is the flag's first caller.
#define NP_SCROLLBAR_FLAGS UUI_SCROLLBAR_ARROWS

static void draw_scrollbar(struct ugfx_surface *s, int tx, int ty, int tw, int th) {
    int total, visible;
    utext_metrics(&g_text, tw, th, &total, &visible);

    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
    uui_scrollbar_draw(s, bx, by, bw, bh, total, visible, g_text.scroll_offset,
                        ugfx_rgb(225, 225, 230), ugfx_rgb(150, 155, 165),
                        NP_SCROLLBAR_FLAGS);
}

static void draw_dialog(struct ugfx_surface *s) {
    int w = s->w - 80, h = s->h - 80;
    int x = 40, y = 40;

    ugfx_fill_rect(s, x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, x, y, w, h, ugfx_rgb(150, 155, 165));

    if (g_dialog_saving) {
        ugfx_draw_string(s, x + 10, y + 8, "Save as -- type a name, Enter to save",
                          UTHEME_TEXT, UTHEME_PANEL_BG);
    } else {
        // Showing the path matters once directories can be entered --
        // otherwise a listing three levels down is indistinguishable
        // from the root's.
        char hdr[PATH_MAX_LEN + 8];
        scopy(hdr, "Open: ", (int)sizeof hdr);
        int hl = slen(hdr);
        for (int i = 0; g_dialog_dir[i] && hl < (int)sizeof hdr - 1; i++) hdr[hl++] = g_dialog_dir[i];
        hdr[hl] = '\0';
        ugfx_draw_string_clipped(s, x + 10, y + 8, w - 20, hdr, UTHEME_TEXT, UTHEME_PANEL_BG);
    }

    int row_h = ugfx_char_h() + 4;
    int list_y = y + 10 + ugfx_char_h() + 8;

    if (g_dialog_saving) {
        ugfx_fill_rect(s, x + 10, list_y, w - 20, row_h, UTHEME_WHITE);
        ugfx_draw_string(s, x + 14, list_y + 2, g_name_field, UTHEME_TEXT, UTHEME_WHITE);
        // Caret, so it reads as an editable field rather than a label.
        ugfx_fill_rect(s, x + 14 + ugfx_text_width(g_name_field), list_y + 2,
                        2, ugfx_char_h(), UTHEME_TEXT);
        return;
    }

    for (int i = 0; i < g_entry_count; i++) {
        int ry = list_y + i * row_h;
        if (ry + row_h > y + h - 10) break;
        uint32_t bg = (i == g_sel) ? ugfx_rgb(205, 220, 240) : UTHEME_PANEL_BG;
        ugfx_fill_rect(s, x + 10, ry, w - 20, row_h, bg);
        ugfx_draw_string_clipped(s, x + 14, ry + 2, w - 28, g_entries[i].name, UTHEME_TEXT, bg);
    }
}

static void draw(struct ugfx_surface *s, int focused) {
    ugfx_fill(s, UTHEME_PANEL_BG);

    layout_toolbar();
    uui_button_group_draw(&g_toolbar, s);

    int tx, ty, tw, th;
    text_rect(s, &tx, &ty, &tw, &th);
    ugfx_draw_rect(s, tx - 1, ty - 1, tw + 2, th + 2, ugfx_rgb(200, 205, 215));
    utext_draw(&g_text, s, tx, ty, tw, th,
                // The caret shows only when this window has keyboard
                // focus AND no modal dialog is over the text. An
                // unfocused window drawing one claims to be taking
                // input that is going somewhere else -- see TWP's
                // WIN_EV_FOCUS.
                UTHEME_TEXT, UTHEME_WHITE, ugfx_rgb(205, 220, 240),
                focused && !g_dialog_open);
    draw_scrollbar(s, tx, ty, tw, th);

    ugfx_draw_string_clipped(s, MARGIN, s->h - ugfx_char_h() - 2, s->w - 2 * MARGIN,
                              g_status, ugfx_rgb(90, 100, 115), UTHEME_PANEL_BG);

    if (g_dialog_open) draw_dialog(s);
}

// --- the dialog's behaviour -------------------------------------------

// Rereads g_dialog_dir into g_entries[], with a ".." row first when
// there is somewhere to go up to.
static void refresh_listing(void) {
    g_entry_count = 0;
    g_sel = 0;

    int at_root = (g_dialog_dir[0] == '/' && g_dialog_dir[1] == '\0');
    if (!at_root) {
        scopy(g_entries[0].name, "..", (int)sizeof g_entries[0].name);
        g_entries[0].is_dir = 1;
        g_entry_count = 1;
    }

    int n = sys_listdir(g_dialog_dir, g_entries + g_entry_count,
                         DIALOG_MAX_FILES - g_entry_count);
    if (n > 0) g_entry_count += n;
    if (g_entry_count == 0) set_status("empty directory");
}

// Joins g_dialog_dir and `name` into `out`, keeping exactly one '/'.
static void join_path(char *out, const char *name) {
    int i = 0;
    for (; g_dialog_dir[i] && i < PATH_MAX_LEN - 2; i++) out[i] = g_dialog_dir[i];
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    for (int j = 0; name[j] && i < PATH_MAX_LEN - 1; j++) out[i++] = name[j];
    out[i] = '\0';
}

// Drops the last component of g_dialog_dir, never past the root.
static void dir_up(void) {
    int n = slen(g_dialog_dir);
    while (n > 1 && g_dialog_dir[n - 1] == '/') n--;
    while (n > 1 && g_dialog_dir[n - 1] != '/') n--;
    if (n < 1) n = 1;
    g_dialog_dir[n] = '\0';
    if (n > 1 && g_dialog_dir[n - 1] == '/') g_dialog_dir[n - 1] = '\0';
    if (g_dialog_dir[0] == '\0') scopy(g_dialog_dir, "/", PATH_MAX_LEN);
}

// Acts on whatever row is selected: descend into a directory, or open a
// file. Returns 1 if the dialog should close.
static int dialog_activate(void) {
    if (g_sel < 0 || g_sel >= g_entry_count) return 1;
    struct dirent *e = &g_entries[g_sel];

    if (e->is_dir) {
        if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') dir_up();
        else {
            char next[PATH_MAX_LEN];
            join_path(next, e->name);
            scopy(g_dialog_dir, next, PATH_MAX_LEN);
        }
        refresh_listing();
        return 0; // stay open, now showing the new directory
    }

    char p[PATH_MAX_LEN];
    join_path(p, e->name);
    load_file(p);
    return 1;
}

static void open_dialog(int saving) {
    g_dialog_open = 1;
    g_dialog_saving = saving;
    g_sel = 0;
    if (saving) {
        scopy(g_name_field, g_path[0] ? g_path : "", PATH_MAX_LEN);
        g_name_len = slen(g_name_field);
    } else {
        scopy(g_dialog_dir, "/", PATH_MAX_LEN);
        refresh_listing();
    }
}

// Returns 1 if the dialog consumed the key.
static int dialog_key(int key) {
    if (key == 0x1B) { g_dialog_open = 0; set_status("cancelled"); return 1; }

    if (g_dialog_saving) {
        if (key == '\n' || key == '\r') {
            g_dialog_open = 0;
            if (g_name_len > 0) {
                // Normalise to an absolute path. A bare name typed here
                // used to be stored as-is, so the title read
                // "notes.txt" after a save and "/notes.txt" after an
                // open -- the same file described two ways, and a later
                // Save then wrote through a relative path whose meaning
                // depended on the kernel's cwd rather than on where the
                // dialog actually was.
                char full[PATH_MAX_LEN];
                if (g_name_field[0] == '/') scopy(full, g_name_field, PATH_MAX_LEN);
                else join_path(full, g_name_field);
                save_file(full);
            }
            return 1;
        }
        if (key == '\b') {
            if (g_name_len > 0) g_name_field[--g_name_len] = '\0';
            return 1;
        }
        if (key >= 32 && key < 127 && g_name_len < PATH_MAX_LEN - 1) {
            g_name_field[g_name_len++] = (char)key;
            g_name_field[g_name_len] = '\0';
        }
        return 1;
    }

    if (key == KEY_ARROW_UP)   { if (g_sel > 0) g_sel--; return 1; }
    if (key == KEY_ARROW_DOWN) { if (g_sel < g_entry_count - 1) g_sel++; return 1; }
    if (key == '\n' || key == '\r') {
        if (dialog_activate()) g_dialog_open = 0;
        return 1;
    }
    return 1; // modal: swallow everything else
}

// --- editor keys -------------------------------------------------------

static void editor_key(int key) {
    switch (key) {
    case KEY_ARROW_LEFT:  utext_sel_clear(&g_text); utext_cursor_left(&g_text); return;
    case KEY_ARROW_RIGHT: utext_sel_clear(&g_text); utext_cursor_right(&g_text); return;
    case KEY_ARROW_UP:    utext_sel_clear(&g_text); utext_cursor_up(&g_text); return;
    case KEY_ARROW_DOWN:  utext_sel_clear(&g_text); utext_cursor_down(&g_text); return;
    case KEY_HOME:        utext_sel_clear(&g_text); utext_cursor_home(&g_text); return;
    case KEY_END:         utext_sel_clear(&g_text); utext_cursor_end(&g_text); return;
    case KEY_PAGE_UP:     utext_scroll(&g_text, 5); return;
    case KEY_PAGE_DOWN:   utext_scroll(&g_text, -5); return;

    // Shift+arrow extends a selection: anchor once, then move.
    case KEY_SHIFT_ARROW_LEFT:
        if (!g_text.sel_active) utext_sel_start(&g_text);
        utext_cursor_left(&g_text); return;
    case KEY_SHIFT_ARROW_RIGHT:
        if (!g_text.sel_active) utext_sel_start(&g_text);
        utext_cursor_right(&g_text); return;
    case KEY_SHIFT_ARROW_UP:
        if (!g_text.sel_active) utext_sel_start(&g_text);
        utext_cursor_up(&g_text); return;
    case KEY_SHIFT_ARROW_DOWN:
        if (!g_text.sel_active) utext_sel_start(&g_text);
        utext_cursor_down(&g_text); return;

    case KEY_DELETE:
        if (utext_sel_present(&g_text)) utext_sel_delete(&g_text);
        else utext_delete(&g_text);
        g_dirty = 1; return;
    case '\b':
        if (utext_sel_present(&g_text)) utext_sel_delete(&g_text);
        else utext_backspace(&g_text);
        g_dirty = 1; return;

    case 0x0F: // Ctrl-O
        open_dialog(0); return;
    case 0x13: // Ctrl-S
        if (g_path[0]) save_file(g_path);
        else open_dialog(1);
        return;
    default: break;
    }

    if (key == '\n' || key == '\r') {
        if (utext_sel_present(&g_text)) utext_sel_delete(&g_text);
        utext_insert(&g_text, '\n');
        g_dirty = 1;
        return;
    }
    if (key >= 32 && key < 127) {
        if (utext_sel_present(&g_text)) utext_sel_delete(&g_text);
        utext_insert(&g_text, (char)key);
        g_dirty = 1;
    }
}

// --- Toykit callbacks -------------------------------------------------
//
// Notepad does NOT use uapp's `buttons` routing, on purpose: its file
// dialog is modal and has to swallow input the toolbar would otherwise
// see. `desc.buttons` is optional for exactly this case -- an app with
// its own precedence rules keeps them, and still gets the loop, the
// handshake and the resize handling.

static int g_dragging;
static int g_scrollbar_drag;
static int g_scrollbar_grab; // how far down the thumb the drag started

// Reports the scrollbar's rect, content-relative, so a test asks where
// it is instead of re-deriving it -- the rule docs/gui-guidelines.md
// states after three tools each learned it the hard way. Written once
// per draw; the log is idempotent enough that a reader only ever needs
// the last line.
static void log_layout(struct uapp *a) {
    int tx, ty, tw, th, bx, by, bw, bh;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);

    char b[80];
    int n = 0;
    const char *pre = "notepad: layout scrollbar ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    int v[4] = { bx, by, bw, bh };
    for (int i = 0; i < 4; i++) {
        if (i) b[n++] = ' ';
        int x = v[i];
        char d2[12];
        int c = 0;
        if (x <= 0) d2[c++] = '0';
        while (x > 0) { d2[c++] = (char)('0' + x % 10); x /= 10; }
        while (c > 0) b[n++] = d2[--c];
    }
    b[n++] = '\n';
    b[n] = '\0';
    sys_eprint(b);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    set_title(a);
    draw(uapp_surface(d), uapp_focused(a));
    log_layout(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (g_dialog_open) {
        dialog_key(key);
    } else if (key == 0x1B) {
        uapp_quit(a, 0);
        return;
    } else {
        editor_key(key);
    }
    uapp_redraw(a);
}


// A click in the trough jumps there; a click on the thumb starts a
// drag. Returns 1 if the scrollbar took the click.
static int scrollbar_press(int px, int py, int tx, int ty, int tw, int th) {
    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);

    int total, visible;
    utext_metrics(&g_text, tw, th, &total, &visible);

    // The widget classifies the click, so the arrows, the thumb and the
    // trough cannot disagree with what was drawn.
    enum uui_scrollbar_zone z =
        uui_scrollbar_hit(bx, by, bw, bh, total, visible, g_text.scroll_offset,
                           px, py, NP_SCROLLBAR_FLAGS);
    switch (z) {
    case UUI_SB_NONE:
        return 0;
    case UUI_SB_UP:
        utext_scroll(&g_text, 1);   // one line back
        return 1;
    case UUI_SB_DOWN:
        utext_scroll(&g_text, -1);  // one line forward
        return 1;
    case UUI_SB_ABOVE:
        utext_scroll(&g_text, visible);  // page
        return 1;
    case UUI_SB_BELOW:
        utext_scroll(&g_text, -visible);
        return 1;
    case UUI_SB_THUMB: {
        // WHERE on the thumb the grab happened. Without this the drag
        // maths below is told the cursor is at the thumb's TOP, so the
        // thumb leaps up by however far down it was actually grabbed --
        // which made the bar usable only by catching its top edge
        // exactly. ui_listbox.c and ui_textview.c have always captured
        // this; ui/uui_scrollbar.h's `grab_offset_in_thumb` says to.
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(by, bh, total, visible, g_text.scroll_offset,
                                  &thumb_y, &thumb_h, bw, NP_SCROLLBAR_FLAGS);
        g_scrollbar_grab = py - thumb_y;
        g_scrollbar_drag = 1;
        return 1;
    }
    }
    return 1;
}

static void on_wheel(struct uapp *a, int notches) {
    // Three lines a notch, the same step the kernel-space scrollback
    // uses. utext_scroll clamps for us.
    utext_scroll(&g_text, notches * 3);
    uapp_redraw(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);

    if (g_dialog_open) {
        if (!g_dialog_saving) {
            int row_h = ugfx_char_h() + 4;
            int list_y = 40 + 10 + ugfx_char_h() + 8;
            int idx = (y - list_y) / row_h;
            if (idx >= 0 && idx < g_entry_count) {
                // Clicking the already-selected row activates it. There
                // is no double-click concept in TWP, and requiring a
                // trip to the keyboard to enter a directory would be
                // worse.
                if (idx == g_sel) {
                    if (dialog_activate()) g_dialog_open = 0;
                } else {
                    g_sel = idx;
                }
            }
        }
    } else if (uui_button_group_press(&g_toolbar, x, y)) {
        // a toolbar button armed
    } else if (scrollbar_press(x, y, tx, ty, tw, th)) {
        // handled: jumped to the clicked position
    } else if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
        g_text.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
        utext_sel_start(&g_text);
        g_dragging = 1;
    }
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);

    if (buttons) {
        if (g_scrollbar_drag) {
            int total, visible;
            utext_metrics(&g_text, tw, th, &total, &visible);
            if (total > visible) {
                int bx, by, bw, bh;
                scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
                // The widget's own drag mapping -- so the thumb tracks
                // the cursor the same way it is drawn, arrows included.
                g_text.scroll_offset =
                    uui_scrollbar_offset_for_drag(by, bh, total, visible, y,
                                                   g_scrollbar_grab,
                                                   bw, NP_SCROLLBAR_FLAGS);
                uapp_redraw(a);
            }
        } else if (g_dragging) {
            g_text.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
            uapp_redraw(a);
        } else if (uui_button_group_press(&g_toolbar, x, y)) {
            uapp_redraw(a);
        }
    } else if (uui_button_group_hover(&g_toolbar, x, y)) {
        uapp_redraw(a);
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    g_dragging = 0;
    g_scrollbar_drag = 0;
    int code = uui_button_group_release(&g_toolbar);
    if (code == BTN_NEW) {
        utext_clear(&g_text);
        g_path[0] = '\0';
        g_dirty = 0;
        set_status("new file");
    } else if (code == BTN_OPEN) {
        open_dialog(0);
    } else if (code == BTN_SAVE) {
        if (g_path[0]) save_file(g_path);
        else open_dialog(1);
    }
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    (void)a;
    utext_init(&g_text);
    g_path[0] = '\0';
    set_status("Ctrl-O open, Ctrl-S save, Esc quit");

    uint32_t fg = UTHEME_TEXT, bg = UTHEME_BUTTON_BG;
    uui_button_init(&g_buttons[0], 0, 0, 0, 0, "New",  bg, fg, BTN_NEW);
    uui_button_init(&g_buttons[1], 0, 0, 0, 0, "Open", bg, fg, BTN_OPEN);
    uui_button_init(&g_buttons[2], 0, 0, 0, 0, "Save", bg, fg, BTN_SAVE);
    uui_button_group_init(&g_toolbar, g_buttons, 3);
}

int main(void) {
    struct uapp_desc desc = {
        .title      = "untitled",
        .w          = WIN_W,
        .h          = WIN_H,
        .x          = 180,
        .y          = 90,
        // Resizable: an editor is the app that most wants it, and it
        // needs no resize code -- text_rect_for() already derives the
        // text area from the content size, so a bigger window is a
        // bigger page. The minimum keeps the toolbar and one text row
        // visible.
        .flags      = UAPP_RESIZABLE,
        .min_w      = 240,
        .min_h      = 120,
        .on_open    = on_open_cb,
        .on_draw    = on_draw,
        .on_key     = on_key,
        .on_press   = on_press,
        .on_motion  = on_motion,
        .on_release = on_release,
        .on_wheel   = on_wheel,
    };
    return uapp_run(&desc);
}
