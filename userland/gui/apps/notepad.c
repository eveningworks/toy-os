// Notepad, as a RING-3 PROCESS.
//
// The kernel-space original is apps/notepad.c. This is the same editor
// -- wrapped scrollable text, a cursor, click-to-position and
// drag-select, open and save -- running as an ordinary ring-3 program
// over the windowing protocol.
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
//
// THE THREE-BUTTON TOOLBAR IS GONE, replaced by a real menu bar
// (ui/uui_menubar.h) and a real status bar (ui/uui_statusbar.h). New /
// Open / Save were the only commands this editor could offer while they
// each had to be a button wide enough to read; a menu holds Save As,
// Select All, a Recent list and the view commands without spending any
// window on them, which is exactly why every desktop editor has one.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/dirsort.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "keyboard.h" // KEY_* codes, the same ones the WM delivers

#define WIN_W 560
#define WIN_H 380

#define MARGIN 8

#define PATH_MAX_LEN 64 // FS_PATH_MAX

// --- menu command codes -----------------------------------------------
//
// One numbering for the menu and for the keyboard accelerators, so
// Ctrl-S and File > Save cannot drift apart: both call do_command().
#define CMD_NEW        1
#define CMD_OPEN       2
#define CMD_SAVE       3
#define CMD_SAVE_AS    4
#define CMD_EXIT       5
#define CMD_RECENT     6  // the submenu itself -- queried, never committed
#define CMD_RECENT_0   7
#define CMD_RECENT_1   8
#define CMD_RECENT_2   9
#define CMD_SELECT_ALL 10
#define CMD_DELETE     11
#define CMD_GOTO       12 // submenu
#define CMD_GOTO_TOP   13
#define CMD_GOTO_END   14
#define CMD_STATUSBAR  15

#define RECENT_MAX 3

static struct utext g_text;

static char g_path[PATH_MAX_LEN];   // "" until saved/opened
static char g_status[96];
static int g_dirty;
static int g_show_status = 1;

// The Recent list. The menu items below point straight at these buffers,
// so "update the menu" is "write the string" -- there is no menu state
// to keep in sync. See ui/uui_menubar.h on why the tree is const and the
// per-item state is asked for instead.
static char g_recent[RECENT_MAX][PATH_MAX_LEN];
static int g_recent_count;

static struct uui_menubar g_menu;
static struct uui_statusbar g_statusbar;
static char g_lncol[20];
static char g_modflag[8];

// --- the menu tree ----------------------------------------------------

static const struct uui_menu_item recent_items[] = {
    UUI_MENU(g_recent[0], CMD_RECENT_0, 0),
    UUI_MENU(g_recent[1], CMD_RECENT_1, 0),
    UUI_MENU(g_recent[2], CMD_RECENT_2, 0),
};

static const struct uui_menu_item file_items[] = {
    UUI_MENU("New",          CMD_NEW,     "Ctrl-N"),
    UUI_MENU("Open...",      CMD_OPEN,    "Ctrl-O"),
    UUI_SUBMENU_CODE("Recent files", recent_items, CMD_RECENT),
    UUI_MENU_SEP,
    UUI_MENU("Save",         CMD_SAVE,    "Ctrl-S"),
    UUI_MENU("Save As...",   CMD_SAVE_AS, 0),
    UUI_MENU_SEP,
    UUI_MENU("Exit",         CMD_EXIT,    "Alt+F4"),
};

static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Select All",       CMD_SELECT_ALL, "Ctrl-A"),
    UUI_MENU("Delete Selection", CMD_DELETE,     "Del"),
};

static const struct uui_menu_item goto_items[] = {
    UUI_MENU("Top of file",    CMD_GOTO_TOP, 0),
    UUI_MENU("Bottom of file", CMD_GOTO_END, 0),
};

static const struct uui_menu_item view_items[] = {
    UUI_SUBMENU("Go to", goto_items),
    UUI_MENU_SEP,
    UUI_MENU("Status bar", CMD_STATUSBAR, 0),
};

static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
};

// --- the open dialog --------------------------------------------------
//
// A modal panel drawn over the editor, inside this window. Entirely the
// application's own -- see the file header.
#define DIALOG_MAX_FILES 32
static int g_dialog_open;
static struct sys_dirent g_entries[DIALOG_MAX_FILES];
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

static int seq(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && a[i] == b[i]; i++) {}
    return a[i] == b[i];
}

static void set_status(const char *s) { scopy(g_status, s, (int)sizeof g_status); }

// Appends `v` as decimal at `n`; returns the new length.
static int put_int(char *b, int n, int v) {
    char d[12];
    int c = 0;
    if (v <= 0) d[c++] = '0';
    while (v > 0) { d[c++] = (char)('0' + v % 10); v /= 10; }
    while (c > 0) b[n++] = d[--c];
    return n;
}

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

// Most-recent-first, deduplicated, capped. Called on every successful
// open and save, which is the only place a path becomes "a file this
// session has actually touched".
static void recent_push(const char *path) {
    int at = RECENT_MAX - 1;
    for (int i = 0; i < g_recent_count; i++)
        if (seq(g_recent[i], path)) { at = i; break; }

    for (int i = at; i > 0; i--) scopy(g_recent[i], g_recent[i - 1], PATH_MAX_LEN);
    scopy(g_recent[0], path, PATH_MAX_LEN);
    if (g_recent_count < RECENT_MAX) g_recent_count++;
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

    g_text.ed.cursor = 0;
    g_text.scroll_offset = 0;
    utext_sel_clear(&g_text);
    scopy(g_path, path, PATH_MAX_LEN);
    g_dirty = 0;
    recent_push(path);
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
    recent_push(path);
    set_status("saved");
    return 1;
}

// --- layout -----------------------------------------------------------

static int menubar_h(void) {
    int h;
    uui_menubar_natural_size(&g_menu, 0, &h);
    return h;
}

static int statusbar_h(void) {
    if (!g_show_status) return 0;
    int h;
    uui_statusbar_natural_size(&g_statusbar, 0, &h);
    return h;
}

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
    *y = menubar_h() + MARGIN;
    *w = cw - 2 * MARGIN - scrollbar_gutter(); // room for the scrollbar
    *h = ch - *y - MARGIN - statusbar_h();
}

static void text_rect(struct ugfx_surface *s, int *x, int *y, int *w, int *h) {
    text_rect_for(s->w, s->h, x, y, w, h);
}

// Placed against the CONTENT rect, which is also the popup bounds handed
// to the menu bar -- see ui/uui_menubar.h on why that rectangle is the
// whole difference between this and a real desktop's popups.
static void layout_chrome(int cw, int ch) {
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, menubar_h());
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_statusbar, 0, ch - statusbar_h(), cw, statusbar_h());
}

// LOGICAL lines, not wrapped rows: "Ln 12" in every editor's status bar
// counts newlines, and a reader comparing it against a shell `sed -n
// 12p` expects the same answer.
static void update_indicators(void) {
    int line = 1, col = 1;
    for (int i = 0; i < g_text.ed.cursor && i < g_text.count; i++) {
        if (utext_at(&g_text, i) == '\n') { line++; col = 1; }
        else col++;
    }

    int n = 0;
    g_lncol[n++] = 'L'; g_lncol[n++] = 'n'; g_lncol[n++] = ' ';
    n = put_int(g_lncol, n, line);
    g_lncol[n++] = ','; g_lncol[n++] = ' ';
    g_lncol[n++] = 'C'; g_lncol[n++] = 'o'; g_lncol[n++] = 'l'; g_lncol[n++] = ' ';
    n = put_int(g_lncol, n, col);
    g_lncol[n] = '\0';

    scopy(g_modflag, g_dirty ? "MOD" : "--", (int)sizeof g_modflag);
}

// --- item state -------------------------------------------------------
//
// Asked for by the menu bar, per item, every draw and every hit test. So
// there is no "refresh the menu" step anywhere in this app: greying Save
// out when the document is clean is this function and nothing else.
static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_SAVE:
        return g_dirty ? 0 : UUI_MI_DISABLED;
    case CMD_DELETE:
        return utext_sel_present(&g_text) ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT:
        return g_recent_count > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_0: return g_recent_count > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_1: return g_recent_count > 1 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_2: return g_recent_count > 2 ? 0 : UUI_MI_DISABLED;
    case CMD_STATUSBAR:
        return g_show_status ? UUI_MI_CHECKED : 0;
    default:
        return 0;
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

    layout_chrome(s->w, s->h);
    uui_menubar_draw(s, &g_menu);

    int tx, ty, tw, th;
    text_rect(s, &tx, &ty, &tw, &th);
    ugfx_draw_rect(s, tx - 1, ty - 1, tw + 2, th + 2, ugfx_rgb(200, 205, 215));
    utext_draw(&g_text, s, tx, ty, tw, th,
                // The caret shows only when this window has keyboard
                // focus AND no modal dialog or open menu is over the
                // text. An unfocused window drawing one claims to be
                // taking input that is going somewhere else -- see TWP's
                // WIN_EV_FOCUS.
                UTHEME_TEXT, UTHEME_WHITE, ugfx_rgb(205, 220, 240),
                focused && !g_dialog_open && !uui_menubar_is_open(&g_menu));
    draw_scrollbar(s, tx, ty, tw, th);

    if (g_show_status) {
        update_indicators();
        uui_statusbar_draw(s, &g_statusbar);
    }

    if (g_dialog_open) draw_dialog(s);

    // LAST. Drawing is immediate-mode, so z-order is call order -- a
    // menu drawn in place would be painted over by the text area.
    uui_menubar_draw_popup(s, &g_menu);
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
    if (n > 0) {
        // SORTED, and by the SAME function /bin/ls uses (lib/dirsort.h).
        // SYS_LISTDIR returns whatever order the filesystem walked, so
        // this dialog used to list files in an order nothing could
        // predict -- bad on its own, and it also has to match `ls`,
        // since that is how a test derives which row to click.
        //
        // Only the entries AFTER the ".." row are sorted; that row is
        // already at index 0 and belongs there.
        dirsort(g_entries + g_entry_count, n, DIRSORT_NAME, 0);
        g_entry_count += n;
    }
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
    struct sys_dirent *e = &g_entries[g_sel];

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
    uui_menubar_close(&g_menu); // a modal owns the input; the menu steps aside
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

// --- commands ---------------------------------------------------------
//
// The ONE place a command happens. The menu bar routes here, and so do
// the Ctrl accelerators -- which is what stops "Ctrl-S" and "File >
// Save" from being two implementations of saving.

static void log_action(int code);

static void do_command(struct uapp *a, int code) {
    log_action(code);
    switch (code) {
    case CMD_NEW:
        utext_clear(&g_text);
        g_path[0] = '\0';
        g_dirty = 0;
        set_status("new file");
        break;
    case CMD_OPEN:
        open_dialog(0);
        break;
    case CMD_SAVE:
        if (g_path[0]) save_file(g_path);
        else open_dialog(1);
        break;
    case CMD_SAVE_AS:
        open_dialog(1);
        break;
    case CMD_EXIT:
        uapp_quit(a, 0);
        break;
    case CMD_RECENT_0:
    case CMD_RECENT_1:
    case CMD_RECENT_2: {
        int i = code - CMD_RECENT_0;
        if (i < g_recent_count) {
            // A copy: load_file() calls recent_push(), which rewrites the
            // very slot the path is being read out of.
            char p[PATH_MAX_LEN];
            scopy(p, g_recent[i], PATH_MAX_LEN);
            load_file(p);
        }
        break;
    }
    case CMD_SELECT_ALL:
        // Ctrl-A reaches the editor as a control code and is handled by
        // the shared keymap too; this is the MENU path to the same
        // thing, which is why it calls the same core rather than
        // re-implementing it out of cursor moves.
        utext_key(&g_text, 0x01, 0);
        set_status("selected all");
        break;
    case CMD_DELETE:
        if (utext_sel_present(&g_text)) { utext_sel_delete(&g_text); g_dirty = 1; }
        break;
    case CMD_GOTO_TOP:
        g_text.ed.cursor = 0;
        utext_sel_clear(&g_text);
        utext_scroll(&g_text, g_text.count); // clamps to the top
        break;
    case CMD_GOTO_END:
        g_text.ed.cursor = g_text.count;
        utext_sel_clear(&g_text);
        g_text.scroll_offset = 0; // 0 is pinned to the newest text
        break;
    case CMD_STATUSBAR:
        g_show_status = !g_show_status;
        break;
    default:
        break;
    }
}

// --- editor keys -------------------------------------------------------

// Returns 1 if this key was an accelerator, having run its command.
static int accelerator(struct uapp *a, int key) {
    switch (key) {
    case 0x0E: do_command(a, CMD_NEW);        return 1; // Ctrl-N
    case 0x0F: do_command(a, CMD_OPEN);       return 1; // Ctrl-O
    case 0x13: do_command(a, CMD_SAVE);       return 1; // Ctrl-S
    case 0x01: do_command(a, CMD_SELECT_ALL); return 1; // Ctrl-A
    default:   return 0;
    }
}

static void editor_key(int key) {
    // Paging is this app's, because it is about the VIEW rather than
    // the text -- the core moves a caret, and a page here scrolls
    // without moving one.
    if (key == KEY_PAGE_UP)   { utext_scroll(&g_text, 5);  return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(&g_text, -5); return; }

    // Everything else -- arrows, Shift+arrows, Home/End, Ctrl+A,
    // Backspace and Delete removing a selection, typing replacing one --
    // is the SHARED keymap (ui/uui_edit.h). This function used to be
    // sixty lines of exactly that, written here and nowhere else, so
    // the single-line text field got none of it.
    int before = g_text.count;
    if (utext_key(&g_text, key, 0)) {
        if (g_text.count != before) g_dirty = 1;
        return;
    }

    // Enter is deliberately declined by the core: a field commits, a
    // document inserts a newline. This is a document.
    if (key == '\n' || key == '\r') {
        utext_sel_delete(&g_text);
        utext_insert(&g_text, '\n');
        g_dirty = 1;
    }
}

// --- Toykit callbacks -------------------------------------------------
//
// Notepad does NOT use uapp's `buttons` routing, on purpose: its file
// dialog is modal and its menu bar owns input ahead of everything else.
// `desc.buttons` is optional for exactly this case -- an app with its
// own precedence rules keeps them, and still gets the loop, the
// handshake and the resize handling.

static int g_dragging;
static int g_scrollbar_drag;
static int g_scrollbar_grab; // how far down the thumb the drag started

// --- self-reported layout ----------------------------------------------
//
// docs/gui-guidelines.md: a GUI test asks the app where things are.
// Re-deriving a menu's rectangles in Python would be hopeless anyway --
// they depend on which submenu is open and on how the placement flipped
// -- so every rect a test could want is reported here, from the SAME
// accessors the widget draws with.

static void emit(const char *prefix, const int *v, int n) {
    char b[96];
    int i = 0;
    while (prefix[i]) { b[i] = prefix[i]; i++; }
    for (int k = 0; k < n; k++) {
        b[i++] = ' ';
        i = put_int(b, i, v[k]);
    }
    b[i++] = '\n';
    b[i] = '\0';
    sys_eprint(b);
}

static void log_action(int code) {
    int v[1] = { code };
    emit("notepad: action", v, 1);
}

static void log_layout(struct uapp *a) {
    int cw = uapp_width(a), ch = uapp_height(a);

    int tx, ty, tw, th, x, y, w, h;
    text_rect_for(cw, ch, &tx, &ty, &tw, &th);
    scrollbar_rect(tx, ty, tw, th, &x, &y, &w, &h);
    { int v[4] = { x, y, w, h }; emit("notepad: layout scrollbar", v, 4); }

    // The editable area itself. Added when the toolbar became a menu
    // bar: the text moved up by the difference in chrome height, and a
    // test sampling a hardcoded band below it went on comparing three
    // identical patches of blank background and reporting them as
    // passes. A rect the app states cannot drift that way.
    { int v[4] = { tx, ty, tw, th }; emit("notepad: layout text", v, 4); }

    { int v[4] = { g_menu.x, g_menu.y, g_menu.w, g_menu.h };
      emit("notepad: layout menubar", v, 4); }

    for (int i = 0; i < (int)(sizeof menu_bar / sizeof menu_bar[0]); i++) {
        if (!uui_menubar_title_rect(&g_menu, i, &x, &y, &w, &h)) continue;
        int v[5] = { i, x, y, w, h };
        emit("notepad: layout title", v, 5);
    }

    if (g_show_status) {
        { int v[4] = { g_statusbar.x, g_statusbar.y, g_statusbar.w, g_statusbar.h };
          emit("notepad: layout statusbar", v, 4); }
        for (int i = 0; i < g_statusbar.count; i++) {
            if (!uui_statusbar_pane_rect(&g_statusbar, i, &x, &y, &w, &h)) continue;
            int v[5] = { i, x, y, w, h };
            emit("notepad: layout pane", v, 5);
        }
    }

    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) {
        if (uui_menubar_popup_rect(&g_menu, l, &x, &y, &w, &h)) {
            int v[5] = { l, x, y, w, h };
            emit("notepad: layout popup", v, 5);
        }
        for (int i = 0; uui_menubar_item_rect(&g_menu, l, i, &x, &y, &w, &h); i++) {
            int v[6] = { l, i, x, y, w, h };
            emit("notepad: layout item", v, 6);
        }
    }
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
        uapp_redraw(a);
        return;
    }

    // The menu bar gets first refusal. When closed it takes only F10, so
    // Esc still quits and every editing key is untouched.
    int code = -1;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }

    if (accelerator(a, key)) {
        uapp_redraw(a);
        return;
    }

    // Esc is NOT a quit key. It closes a menu or cancels a dialog --
    // both handled above -- and otherwise does nothing. Closing the
    // window is Alt+F4, which never reaches here: the WM takes it and
    // asks through the same handshake the X button uses, so this app's
    // on_close (uapp's default, accept) still decides. That matters
    // most here of all the clients: Esc was one stray press away from
    // destroying unsaved text, and it became the menu-close key too.
    editor_key(key);
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
        // exactly. ui/uui_scrollbar.h's `grab_offset_in_thumb` says so.
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
    } else if (uui_menubar_press(&g_menu, x, y)) {
        // the menu opened, switched, or swallowed a dismissing click
    } else if (scrollbar_press(x, y, tx, ty, tw, th)) {
        // handled: jumped to the clicked position
    } else if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
        g_text.ed.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
        utext_sel_start(&g_text);
        g_dragging = 1;
    }
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);

    // The menu tracks the cursor whether or not a button is held -- that
    // is what makes press-on-a-title, drag-down, release-on-an-item work,
    // and it is how every real menu bar behaves. Skipped only while a
    // text or scrollbar drag owns the mouse.
    if (!g_dragging && !g_scrollbar_drag && !g_dialog_open) {
        if (uui_menubar_motion(&g_menu, x, y)) uapp_redraw(a);
        if (uui_menubar_is_open(&g_menu)) return;
    }

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
            g_text.ed.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
            uapp_redraw(a);
        }
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    g_dragging = 0;
    g_scrollbar_drag = 0;

    int code = uui_menubar_release(&g_menu, x, y);
    if (code >= 0) do_command(a, code);
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    (void)a;
    utext_init(&g_text);
    g_path[0] = '\0';
    set_status("F10 for the menu -- Ctrl-O open, Ctrl-S save, Alt+F4 quit");

    for (int i = 0; i < RECENT_MAX; i++) scopy(g_recent[i], "(empty)", PATH_MAX_LEN);

    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;

    uui_statusbar_init(&g_statusbar);
    g_statusbar.count = 3;
    g_statusbar.panes[0].text = g_status;   g_statusbar.panes[0].chars = 0;
    g_statusbar.panes[1].text = g_lncol;    g_statusbar.panes[1].chars = 14;
    g_statusbar.panes[2].text = g_modflag;  g_statusbar.panes[2].chars = 4;
    update_indicators();
}

int main(void) {
    struct uapp_desc desc = {
        .title      = "untitled",
        .app_id     = "notepad",
        .w          = WIN_W,
        .h          = WIN_H,
        .x          = 180,
        .y          = 90,
        // Resizable: an editor is the app that most wants it, and it
        // needs no resize code -- text_rect_for() already derives the
        // text area from the content size, so a bigger window is a
        // bigger page. The minimum keeps the menu bar, one text row and
        // the status bar visible.
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
