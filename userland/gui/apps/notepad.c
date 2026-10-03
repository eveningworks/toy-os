// Notepad -- a tabbed plain-text editor with a live Markdown preview,
// as a RING-3 PROCESS.
//
// Kate's and Windows 11 Notepad's shape (mockup N3, 2026-10-03): a menu
// bar, a command bar coloured by what each command does, a tab per open
// document, the text with a line-number gutter and the caret's line
// tinted, a preview pane on the right for Markdown, and a status bar of
// readouts. The editing itself is the toolkit's -- utext for the buffer,
// uui_edit for the keymap, uui_undo for the history -- so this file is
// documents, files and chrome.
//
// BLOCKING I/O IS ALLOWED HERE. This is a process of its own: a read
// blocks it and the desktop keeps running (the GUI-in-ring-3 milestone),
// so loading a file is an ordinary read into a buffer.
//
// THE FILE DIALOG IS THE TOOLKIT'S, IN A WINDOW OF ITS OWN
// (ui/uui_filedialog.h), shared with Image Viewer and Audio Player.
//
// WHAT IS STILL DRAWN BY HAND is the document and its two scrollbars:
// utext has no widget of its own. Everything else is a routed widget.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "kpath.h"   // k_path_join/_dirname/_basename -- the kernel's, linked into ring 3
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "lib/uopen.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_markdown.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_tabs.h"
#include "ui/uui_findbar.h"
#include "ui/uui_splitter.h"
#include "ui/uui_undo.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "lib/ufile.h"
#include "lib/uclip.h"
#include "keyboard.h" // KEY_* codes, the same ones the WM delivers

// A small inset so glyphs and the frame don't touch when there is no
// gutter -- not an outer moat. The edit surface fills the content area
// between the bars, flush, as a real editor's edit control does.
#define TEXT_PAD 3

#define PATH_MAX_LEN 64          // NOT FS_PATH_MAX -- this app's own
                                 // buffers, still the old bound.
                                 // See docs/roadmap.md.
#define MAX_DOCS 16              // tabs
#define UNDO_BYTES (256 * 1024)  // each document's edit history

// --- widget ids -------------------------------------------------------

#define ID_MENU     1
#define ID_STATUS   2
#define ID_TOOLBAR  3
#define ID_TABS     4
#define ID_FIND     5
#define ID_CTX      6
#define ID_MD       7
#define ID_ASK      8
#define ID_SPLIT    9

// --- command codes ----------------------------------------------------
//
// One numbering for the menu, the command bar and the keys, so Ctrl-S
// and File > Save cannot drift apart: all three call do_command(). The
// first twenty are what tools/menubar_test.py logs; new ones go after.
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
#define CMD_CUT        16
#define CMD_COPY       17
#define CMD_PASTE      18
#define CMD_WORDWRAP   19
#define CMD_MARKDOWN   20
#define CMD_UNDO       21
#define CMD_REDO       22
#define CMD_FIND       23
#define CMD_LINENUMS   24
#define CMD_CLOSE_TAB  25
#define CMD_NEXT_TAB   26
#define CMD_PREV_TAB   27
#define CMD_FIND_NEXT  28
#define CMD_FIND_PREV  29

#define RECENT_MAX 3

static struct uapp *g_app;   // for callbacks that carry no uapp

// --- documents --------------------------------------------------------
//
// THE STORAGE IS SIZED TO THE FILE, and it is this app's rather than
// utext's (utext.h). The ceiling is a REFUSAL, not a truncation: an edit
// costs two passes over the buffer (the move that makes room and the
// wrap index behind it), so past a few megabytes typing gets slow.
// pci.ids (1.6 MB) is the file this number was chosen against.
#define DOC_MAX_BYTES  (4 * 1024 * 1024)
#define DOC_EDIT_SLACK 8192   // room past the file's end to type into

struct doc {
    struct utext text;
    char *buf;
    int cap;
    struct uui_undo undo;
    unsigned char *undo_mem;   // NULL when it could not be had: see doc_dirty()
    unsigned saved_rev;        // the fallback's clean point
    char path[PATH_MAX_LEN];   // "" until saved or opened
    char label[40];            // the tab's text
    int preview;               // the Markdown pane is open beside it
    int crlf;                  // the file came with CRLF line endings
};

static struct doc *g_docs[MAX_DOCS];
static int g_ndocs;
static int g_cur;

static struct doc *cur(void) { return g_docs[g_cur]; }
#define T (&cur()->text)

// DIRTY IS A POSITION IN THE HISTORY (ui/uui_undo.h): undoing back to
// what was saved makes the document clean again, as in every editor.
// Without history memory, any change since the save counts.
static int doc_dirty(const struct doc *d) {
    if (d->undo_mem) return !uui_undo_is_clean(&d->undo);
    return d->text.rev != d->saved_rev;
}

static void doc_mark_clean(struct doc *d) {
    uui_undo_mark_clean(&d->undo);
    d->saved_rev = d->text.rev;
}

static int any_dirty(void) {
    for (int i = 0; i < g_ndocs; i++) if (doc_dirty(g_docs[i])) return 1;
    return 0;
}

// Grows `d`'s buffer to hold `bytes` plus room to type. 0 if it could
// not, leaving the old buffer intact and usable.
static int doc_reserve(struct doc *d, int bytes) {
    int want = bytes + DOC_EDIT_SLACK;
    if (want <= d->cap) return 1;
    char *n = (char *)realloc(d->buf, (size_t)want);
    if (!n) return 0;
    d->buf = n;
    d->cap = want;
    // The buffer MOVED; the caret, selection and scroll all survive.
    d->text.buf = d->buf;
    d->text.cap = d->cap;
    return 1;
}

static char g_status[96];
static int g_show_status = 1;
static int g_linenums = 1;

// The Recent list. The menu items point straight at these buffers, so
// "update the menu" is "write the string" (ui/uui_menubar.h).
static char g_recent[RECENT_MAX][PATH_MAX_LEN];
static int g_recent_count;

static struct uui_menubar g_menu;
static struct uui_menubar g_ctx;   // the context menu -- no bar of its own
static struct uui_toolbar g_tb;
static struct uui_tabs g_tabs;
static struct uui_tab g_tablist[MAX_DOCS];
static struct uui_findbar g_find;
static int g_find_open;
static struct uui_splitter g_split;

// THE PREVIEW is a widget (ui/uui_markdown.h) beside the text, fed the
// buffer the editor holds and scrolled to follow it -- GNOME Text
// Editor's and Kate's live preview.
static struct uui_markdown g_md;
static struct uui_statusbar g_statusbar;
static char g_lncol[24];
static char g_chars[24];
static char g_kind[16];
static char g_eol[16];

// --- the menu tree ----------------------------------------------------

static const struct uui_menu_item recent_items[] = {
    UUI_MENU(g_recent[0], CMD_RECENT_0, 0),
    UUI_MENU(g_recent[1], CMD_RECENT_1, 0),
    UUI_MENU(g_recent[2], CMD_RECENT_2, 0),
};

static const struct uui_menu_item file_items[] = {
    UUI_MENU("New tab",      CMD_NEW,       "Ctrl-N"),
    UUI_MENU("Open...",      CMD_OPEN,      "Ctrl-O"),
    UUI_SUBMENU_CODE("Recent files", recent_items, CMD_RECENT),
    UUI_MENU_SEP,
    UUI_MENU("Save",         CMD_SAVE,      "Ctrl-S"),
    UUI_MENU("Save As...",   CMD_SAVE_AS,   0),
    UUI_MENU_SEP,
    UUI_MENU("Close tab",    CMD_CLOSE_TAB, "Ctrl-W"),
    UUI_MENU("Exit",         CMD_EXIT,      "Alt+F4"),
};

static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Undo",             CMD_UNDO,       "Ctrl-Z"),
    UUI_MENU("Redo",             CMD_REDO,       "Ctrl-Y"),
    UUI_MENU_SEP,
    UUI_MENU("Cut",              CMD_CUT,        "Ctrl-X"),
    UUI_MENU("Copy",             CMD_COPY,       "Ctrl-C"),
    UUI_MENU("Paste",            CMD_PASTE,      "Ctrl-V"),
    UUI_MENU_SEP,
    UUI_MENU("Find...",          CMD_FIND,       "Ctrl-F"),
    UUI_MENU("Find next",        CMD_FIND_NEXT,  "F3"),
    UUI_MENU_SEP,
    UUI_MENU("Select All",       CMD_SELECT_ALL, "Ctrl-A"),
    UUI_MENU("Delete Selection", CMD_DELETE,     "Del"),
};

// THE CONTEXT MENU IS THE SAME WIDGET WITH NO BAR, its rows the Edit
// menu's -- a context menu with a different set of the same verbs is
// how the two drift apart.
static const struct uui_menu_item ctx_items[] = {
    UUI_MENU("Undo",       CMD_UNDO,       "Ctrl-Z"),
    UUI_MENU("Redo",       CMD_REDO,       "Ctrl-Y"),
    UUI_MENU_SEP,
    UUI_MENU("Cut",        CMD_CUT,        "Ctrl-X"),
    UUI_MENU("Copy",       CMD_COPY,       "Ctrl-C"),
    UUI_MENU("Paste",      CMD_PASTE,      "Ctrl-V"),
    UUI_MENU_SEP,
    UUI_MENU("Select All", CMD_SELECT_ALL, "Ctrl-A"),
    UUI_MENU("Delete",     CMD_DELETE,     "Del"),
};

static const struct uui_menu_item goto_items[] = {
    UUI_MENU("Top of file",    CMD_GOTO_TOP, 0),
    UUI_MENU("Bottom of file", CMD_GOTO_END, 0),
};

// The first seven rows are where tools/menubar_test.py finds Go to and
// Status bar; new rows go at the end.
static const struct uui_menu_item view_items[] = {
    UUI_MENU("Markdown preview", CMD_MARKDOWN, "Ctrl-E"),
    UUI_MENU_SEP,
    UUI_MENU("Word wrap",  CMD_WORDWRAP, "Alt+Z"),
    UUI_MENU_SEP,
    UUI_SUBMENU("Go to", goto_items),
    UUI_MENU_SEP,
    UUI_MENU("Status bar", CMD_STATUSBAR, 0),
    UUI_MENU("Line numbers", CMD_LINENUMS, 0),
};

static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
};

// --- the command bar --------------------------------------------------
//
// Coloured by what each command DOES (docs/gui-guidelines.md, the app
// design language); the two view toggles sit at the end and latch.
static const struct uui_toolbar_item tb_items[] = {
    { "tb-new",     "New tab (Ctrl+N)",   CMD_NEW,      0, 0,          "Ctrl-N", UTHEME_ACT_CREATE },
    { "tb-open",    "Open (Ctrl+O)",      CMD_OPEN,     0, 0,          "Ctrl-O", UTHEME_ACT_NAV },
    { "tb-save",    "Save (Ctrl+S)",      CMD_SAVE,     0, 0,          "Ctrl-S", UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-undo",    "Undo (Ctrl+Z)",      CMD_UNDO,     0, 0,          "Ctrl-Z", UTHEME_ACT_EDIT },
    { "tb-redo",    "Redo (Ctrl+Y)",      CMD_REDO,     0, 0,          "Ctrl-Y", UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-cut",     "Cut (Ctrl+X)",       CMD_CUT,      0, 0,          "Ctrl-X", UTHEME_ACT_EDIT },
    { "tb-copy",    "Copy (Ctrl+C)",      CMD_COPY,     0, 0,          "Ctrl-C", UTHEME_ACT_EDIT },
    { "tb-paste",   "Paste (Ctrl+V)",     CMD_PASTE,    0, 0,          "Ctrl-V", UTHEME_ACT_EDIT },
    UUI_TOOLBAR_SEP,
    { "tb-find",    "Find (Ctrl+F)",      CMD_FIND,     0, 0,          "Ctrl-F", UTHEME_ACT_VIEW },
    { "tb-wrap",    "Word wrap (Alt+Z)",  CMD_WORDWRAP, 0, UUI_TB_END, "Alt+Z",  UTHEME_ACT_ARRANGE },
    { "tb-preview", "Markdown preview (Ctrl+E)", CMD_MARKDOWN, 0, 0,   "Ctrl-E", UTHEME_ACT_ARRANGE },
};

// --- the file chooser and the unsaved-changes dialog ------------------

// ~20 KB, so file-scope and never a local -- the ring-3 frame budget is
// 2 KiB.
static struct uui_filedialog g_fd;
static int g_dlg_saving;

// EVERY ACTION THAT WOULD THROW A DOCUMENT AWAY ASKS THROUGH ONE PLACE:
// closing a tab and closing the window. Save / Don't Save / Cancel is
// what Notepad, gedit and Kate all put up. The answer arrives frames
// later, so the action is PARKED; `g_after_save` says the chooser is
// finishing a Save that an ask started.
static struct uui_dialog g_ask;

enum { ASK_SAVE = 1, ASK_DISCARD, ASK_CANCEL };
enum { PEND_NONE = 0, PEND_CLOSE, PEND_CLOSE_TAB };
static int g_pending;
static int g_after_save;
static char g_ask_line[96];
static const char *g_ask_rows[1];

// --- the routed widgets ---------------------------------------------
//
// In DRAW order; input is offered in the reverse. `.name` is what the
// layout log reports (ui/uui_describe.h), and tests read "markdown".
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,      .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_toolbar_ops,   .widget = &g_tb,        .id = ID_TOOLBAR, .name = "toolbar" },
    { .ops = &uui_tabs_ops,      .widget = &g_tabs,      .id = ID_TABS,    .name = "tabs" },
    { .ops = &uui_statusbar_ops, .widget = &g_statusbar, .id = ID_STATUS,  .name = "status" },
    { .ops = &uui_markdown_ops,  .widget = &g_md,        .id = ID_MD,      .name = "markdown", .hidden = 1 },
    { .ops = &uui_splitter_ops,  .widget = &g_split,     .id = ID_SPLIT,   .name = "split",    .hidden = 1 },
    { .ops = &uui_findbar_ops,   .widget = &g_find,      .id = ID_FIND,    .name = "find",     .hidden = 1 },
    { .ops = &uui_menubar_ops,   .widget = &g_ctx,       .id = ID_CTX,     .name = "ctxmenu" },
    // LAST: an overlay is offered every press first.
    { .ops = &uui_dialog_ops,    .widget = &g_ask,       .id = ID_ASK,     .name = "ask-save" },
};

// By ID, never by index (docs/conventions/gui.md).
static struct uui_item *item_by_id(struct uui_item *items, int n, int id) {
    for (int i = 0; i < n; i++) if (items[i].id == id) return &items[i];
    return NULL;
}
#define WIDGET(id)   item_by_id(g_widgets, (int)(sizeof g_widgets / sizeof g_widgets[0]), (id))

static void set_status(const char *s) { strlcpy(g_status, s, sizeof g_status); }

// **.md OPENS WITH ITS PREVIEW, every other extension as text** -- and
// the pane is a toggle either way.
static int looks_like_markdown(const char *path) {
    int n = (int)strlen(path);
    return n > 3 && path[n - 3] == '.' &&
           (path[n - 2] == 'm' || path[n - 2] == 'M') &&
           (path[n - 1] == 'd' || path[n - 1] == 'D');
}

// The title is the path with a dirty marker, and is sent only when it
// changed -- not a TWP message per keystroke saying the same thing.
static char g_shown_title[WIN_TITLE_LEN];

static void set_title(struct uapp *a) {
    char t[WIN_TITLE_LEN];
    struct doc *d = cur();
    snprintf(t, sizeof t, "%s%s", doc_dirty(d) ? "*" : "", d->path[0] ? d->path : "untitled");
    if (strcmp(t, g_shown_title) == 0) return;
    strlcpy(g_shown_title, t, sizeof g_shown_title);
    uapp_set_title(a, t);
}

// Most-recent-first, deduplicated, capped; on every open and save.
static void recent_push(const char *path) {
    int at = RECENT_MAX - 1;
    for (int i = 0; i < g_recent_count; i++)
        if (strcmp(g_recent[i], path) == 0) { at = i; break; }
    for (int i = at; i > 0; i--) strlcpy(g_recent[i], g_recent[i - 1], PATH_MAX_LEN);
    strlcpy(g_recent[0], path, PATH_MAX_LEN);
    if (g_recent_count < RECENT_MAX) g_recent_count++;
}

// --- documents: made, switched, closed --------------------------------

static void find_run(int select);

static struct doc *doc_new(void) {
    if (g_ndocs >= MAX_DOCS) { set_status("too many tabs -- close one first"); return NULL; }
    struct doc *d = (struct doc *)calloc(1, sizeof *d);
    if (!d) { set_status("out of memory"); return NULL; }
    if (!doc_reserve(d, 0)) { free(d); set_status("out of memory"); return NULL; }
    utext_init_buf(&d->text, d->buf, d->cap);
    d->text.animate = 1;   // Toykit redraws per frame while a scroll glides
    d->text.gutter = g_linenums;
    d->text.line_highlight = 1;
    d->undo_mem = (unsigned char *)malloc(UNDO_BYTES);
    uui_undo_init(&d->undo, d->undo_mem, d->undo_mem ? UNDO_BYTES : 0);
    d->text.ed.undo = &d->undo;
    g_docs[g_ndocs++] = d;
    return d;
}

static void doc_free(struct doc *d) {
    free(d->buf);
    free(d->undo_mem);
    free(d);
}

// An empty, unnamed, untouched document: what Open replaces rather than
// opening a tab beside -- Windows 11 Notepad's rule.
static int doc_pristine(const struct doc *d) {
    return !d->path[0] && d->text.count == 0 && !doc_dirty(d);
}

// The tab active before this one: closing a tab goes back to it, as
// Kate and every browser do, rather than to whichever is beside it.
static int g_prev = -1;

static void switch_to(int i) {
    if (i < 0 || i >= g_ndocs) return;
    if (g_find_open) T->marks = NULL, T->mark_count = 0;
    if (i != g_cur) g_prev = g_cur;
    g_cur = i;
    g_tabs.selected = i;
    if (g_find_open) find_run(0);
}

// Removes tab `i` with no questions -- the asking is confirm_close().
static void doc_remove(int i) {
    if (i < 0 || i >= g_ndocs) return;
    doc_free(g_docs[i]);
    for (int k = i; k + 1 < g_ndocs; k++) g_docs[k] = g_docs[k + 1];
    g_ndocs--;
    if (g_prev > i) g_prev--;
    else if (g_prev == i) g_prev = -1;
    int next = g_cur;
    if (g_cur == i && g_prev >= 0) next = g_prev;
    else if (g_cur > i) next = g_cur - 1;
    if (g_ndocs == 0) { doc_new(); next = 0; }   // the window always has a document
    if (next >= g_ndocs) next = g_ndocs - 1;
    g_cur = next;
    g_prev = -1;
    switch_to(g_cur);
}

static void refresh_tabs(void) {
    for (int i = 0; i < g_ndocs; i++) {
        struct doc *d = g_docs[i];
        strlcpy(d->label, d->path[0] ? k_path_basename(d->path) : "untitled", sizeof d->label);
        g_tablist[i].label = d->label;
        g_tablist[i].closable = 1;
        g_tablist[i].modified = doc_dirty(d);
    }
    g_tabs.count = g_ndocs;
    g_tabs.selected = g_cur;
}

// --- file I/O ---------------------------------------------------------

static int load_into(struct uapp *a, struct doc *d, const char *path) {
    struct sys_stat st;
    if (sys_stat(path, &st) < 0) { set_status("open failed"); return 0; }
    if (st.size > (uint64_t)DOC_MAX_BYTES) {
        // REFUSED, and the document on screen is left alone -- a kept
        // tail with the path set would let Ctrl-S overwrite the file.
        snprintf(g_status, sizeof g_status, "too large: %u KB, limit %d KB",
                  (unsigned)(st.size / 1024), DOC_MAX_BYTES / 1024);
        return 0;
    }
    if (!doc_reserve(d, (int)st.size)) { set_status("out of memory"); return 0; }

    uapp_busy_begin(a);   // a whole file with the event loop stopped (ui/uapp.h)
    utext_clear(&d->text);
    size_t got = 0;
    enum ufile_result r = ufile_read_into(path, d->buf, (size_t)d->cap, &got);
    uapp_busy_end(a);

    if (r != UFILE_OK && r != UFILE_SHORT) { set_status("open failed"); return 0; }
    d->text.count = (int)got;
    d->text.rev++;   // the bytes arrived behind utext's back; see utext.h
    uui_undo_reset(&d->undo);   // a load, not an edit
    if (r == UFILE_SHORT) set_status("read ended early -- file changed?");
    else set_status("opened");

    d->crlf = 0;
    for (int i = 0; i + 1 < (int)got; i++)
        if (d->buf[i] == '\r' && d->buf[i + 1] == '\n') { d->crlf = 1; break; }
    d->text.ed.cursor = 0;
    utext_sel_clear(&d->text);
    utext_scroll_top(&d->text);   // 0 is the BOTTOM (utext.h)
    d->preview = looks_like_markdown(path);
    strlcpy(d->path, path, PATH_MAX_LEN);
    doc_mark_clean(d);
    recent_push(path);
    return 1;
}

// OPEN: a file already open is SWITCHED TO; a pristine tab is reused;
// anything else opens a tab beside it.
static int open_path(struct uapp *a, const char *path) {
    for (int i = 0; i < g_ndocs; i++)
        if (strcmp(g_docs[i]->path, path) == 0) { switch_to(i); set_status("already open"); return 1; }
    if (doc_pristine(cur())) return load_into(a, cur(), path);
    if (!doc_new()) return 0;
    int at = g_ndocs - 1;
    if (!load_into(a, g_docs[at], path)) {
        doc_free(g_docs[at]);
        g_ndocs--;
        return 0;
    }
    switch_to(at);
    return 1;
}

static int save_file(struct uapp *a, struct doc *d, const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { set_status("save failed"); return 0; }
    uapp_busy_begin(a);
    // Straight from the buffer, in runs SYS_WRITE takes (SYS_WRITE_MAX).
    for (int i = 0; i < d->text.count;) {
        int n = d->text.count - i;
        if (n > 65536) n = 65536;
        if (sys_write(fd, d->buf + i, (size_t)n) < 0) {
            sys_close(fd);
            uapp_busy_end(a);
            set_status("write failed");
            return 0;
        }
        i += n;
    }
    sys_close(fd);
    uapp_busy_end(a);

    strlcpy(d->path, path, PATH_MAX_LEN);
    doc_mark_clean(d);
    recent_push(path);
    set_status("saved");
    return 1;
}

// --- layout -----------------------------------------------------------
//
// Derived from the content SIZE rather than from a surface, because the
// event callbacks need it too. That is what makes the window resizable
// with no resize code.

// **THE CHROME IS MEASURED IN THE INTERFACE FACE, WHATEVER IS SELECTED.**
// Every size below derives from the current font, and the document's
// code runs inside doc_font() -- so measured there, the bars came out
// taller and the scrollbar narrower than the toolkit draws them: a grey
// slice above the text and a thumb cut down by the splitter.
static const struct ugfx_font *ui_font(void) {
    return ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
}
#define IN_UI_FONT(expr) do { const struct ugfx_font *was_ = ui_font(); expr; ugfx_set_font(was_); } while (0)

static int menubar_h(void)   { int h; IN_UI_FONT(h = uui_menubar_height(&g_menu)); return h; }
static int toolbar_h(void)   { int h; IN_UI_FONT(h = uui_toolbar_height(&g_tb)); return h; }
static int tabs_h(void)      { int h; IN_UI_FONT(h = uui_tabs_height()); return h; }
static int statusbar_h(void) {
    int h = 0;
    if (g_show_status) IN_UI_FONT(h = uui_statusbar_height(&g_statusbar));
    return h;
}
static int chrome_top(void) { return menubar_h() + toolbar_h() + tabs_h(); }

// The Start menu's overlay strip (ui/uui_scrollbar.h).
static int scrollbar_w(void) {
    int w;
    IN_UI_FONT(w = uui_scrollbar_overlay_width());
    return w;
}

// Only when the document is NOT wrapped: wrapping puts nothing to the
// right of the view (Windows Notepad hides it for the same reason).
static int hbar_h(void) {
    if (utext_get_wrap(T) != UTEXT_WRAP_OFF) return 0;
    return scrollbar_w();
}

// The editor's column: the whole width, or the splitter's first share
// when the preview is open.
static int editor_w(int cw) {
    if (!cur()->preview) return cw;
    // ALL of it in the interface face: the splitter's own position
    // depends on its band, whose thickness is font-derived too.
    int ew;
    IN_UI_FONT(uui_splitter_set_track(&g_split, 0, cw, 12 * ugfx_char_w(), 12 * ugfx_char_w());
               ew = uui_splitter_before(&g_split));
    return ew;
}

static void text_rect_for(int cw, int ch, int *x, int *y, int *w, int *h) {
    int ew = editor_w(cw);
    *x = g_linenums ? 0 : TEXT_PAD;
    *y = chrome_top();
    *w = ew - *x - scrollbar_w();
    *h = ch - *y - statusbar_h() - hbar_h();
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

// One derivation each for the two bars, used by draw and by hit-test --
// two copies is how a scrollbar draws in one place and answers in another.
static void scrollbar_rect(int tx, int ty, int tw, int th,
                            int *bx, int *by, int *bw, int *bh) {
    *bx = tx + tw;
    *by = ty;
    *bw = scrollbar_w();
    *bh = th;
}

static void hbar_rect(int tx, int ty, int tw, int th,
                       int *bx, int *by, int *bw, int *bh) {
    *bx = tx;
    *by = ty + th;
    *bw = tw;
    *bh = hbar_h();
}

// Every widget placed against the CONTENT rect, every draw: the app is
// resizable and no `.layout` places these.
static void layout_chrome(int cw, int ch) {
    int y = 0;
    uui_menubar_set_geometry(&g_menu, 0, y, cw, menubar_h());
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    y += menubar_h();
    uui_toolbar_ops.set_geometry(&g_tb, 0, y, cw, toolbar_h());
    y += toolbar_h();
    uui_tabs_set_geometry(&g_tabs, 0, y, cw, tabs_h());
    y += tabs_h();

    uui_statusbar_set_geometry(&g_statusbar, 0, ch - statusbar_h(), cw, statusbar_h());
    WIDGET(ID_STATUS)->hidden = !g_show_status;

    int body_h = ch - y - statusbar_h();
    if (body_h < 1) body_h = 1;
    int preview = cur()->preview;
    int ew = editor_w(cw);
    WIDGET(ID_MD)->hidden = !preview;
    WIDGET(ID_SPLIT)->hidden = !preview;
    if (preview) {
        int sw = uui_splitter_thickness();
        uui_splitter_set_geometry(&g_split, ew, y, sw, body_h);
        uui_markdown_set_geometry(&g_md, ew + sw, y, cw - ew - sw, body_h);
    }

    // Find floats at the editor's top right, clear of its scrollbar.
    WIDGET(ID_FIND)->hidden = !g_find_open;
    if (g_find_open) {
        int fw = 0, fh = 0;
        uui_findbar_natural_size(&g_find, &fw, &fh);
        int room = ew - scrollbar_w() - 16;
        int cap = 34 * ugfx_char_advance('n');   // a field, not a banner
        if (fw > cap) fw = cap;
        if (fw > room) fw = room;
        if (fw < 1) fw = 1;
        uui_findbar_set_geometry(&g_find, ew - scrollbar_w() - 8 - fw, y + 6, fw, fh);
    }

    uui_menubar_set_bounds(&g_ctx, 0, 0, cw, ch);
    uui_dialog_set_bounds(&g_ask, 0, 0, cw, ch);
}

// --- the status bar ---------------------------------------------------

// Thousands grouped with a thin space, as the mockup's "1 664 characters".
static void group(char *out, int cap, int n) {
    char raw[16];
    snprintf(raw, sizeof raw, "%d", n);
    int len = (int)strlen(raw), o = 0;
    for (int i = 0; i < len && o < cap - 2; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

// LOGICAL lines, not wrapped rows: "Ln 12" in every editor counts
// newlines, and agrees with `sed -n 12p`.
static void update_indicators(void) {
    int line, col;
    utext_line_col(T, T->ed.cursor, &line, &col);
    snprintf(g_lncol, sizeof g_lncol, "Ln %d, Col %d", line + 1, col + 1);
    char n[16];
    group(n, sizeof n, T->count);
    snprintf(g_chars, sizeof g_chars, "%s character%s", n, T->count == 1 ? "" : "s");
    strlcpy(g_kind, cur()->path[0] && looks_like_markdown(cur()->path) ? "Markdown" : "Plain text",
            sizeof g_kind);
    strlcpy(g_eol, cur()->crlf ? "Windows (CRLF)" : "Unix (LF)", sizeof g_eol);
}

// --- the clipboard ----------------------------------------------------
//
// **THE KEYS ARE THIS APP'S, NOT THE WM's** (docs/decisions/gui.md):
// Ctrl+C is INTR in a terminal, so a compositor that grabbed it would
// take interrupt away from the GUI Terminal.

// STATIC: struct uclip embeds the whole 64 KiB payload (lib/uclip.h).
static struct uclip g_clip;
// Kept current by on_clipboard(), because menu_item_flags() runs every
// draw and a syscall per row per frame to grey one item is absurd.
static int g_clip_has_text;

static void clip_refresh(void) {
    uclip_load(&g_clip);
    g_clip_has_text = uclip_text(&g_clip, NULL) != 0;
}

// 1 if the selection reached the clipboard -- what Cut must know before
// it deletes anything.
static int do_copy(void) {
    if (!utext_sel_present(T)) { set_status("nothing selected"); return 0; }
    int n = utext_sel_text(T, 0, 0);   // the TRUE length first
    if (n > UCLIP_TEXT_MAX) {
        // A REFUSAL SAID OUT LOUD: a silent one pastes an hour-old copy.
        snprintf(g_status, sizeof g_status,
                  "selection too large to copy: %d KB, limit %d KB",
                  n / 1024, (UCLIP_TEXT_MAX + 1) / 1024);
        return 0;
    }
    char *tmp = (char *)malloc((size_t)n + 1);
    if (!tmp) { set_status("out of memory"); return 0; }
    utext_sel_text(T, tmp, n + 1);
    int ok = uclip_set_text(tmp, n);
    free(tmp);
    if (!ok) { set_status("copy refused"); return 0; }
    snprintf(g_status, sizeof g_status, "copied %d character%s", n, n == 1 ? "" : "s");
    g_clip_has_text = 1;   // the broadcast confirms it; this is for THIS frame
    return 1;
}

// CUT IS COPY THEN DELETE, and the delete happens only if the copy
// landed -- a refused copy that still destroyed the selection loses it.
static void do_cut(void) {
    if (!do_copy()) return;
    utext_sel_delete(T);
    set_status("cut");
}

static void do_paste(void) {
    clip_refresh();
    int n = 0;
    const char *txt = uclip_text(&g_clip, &n);
    if (!txt || n == 0) { set_status("clipboard holds no text"); return; }
    int put = utext_insert_text(T, txt, n);
    if (put < n)
        snprintf(g_status, sizeof g_status, "pasted %d of %d -- document full", put, n);
    else
        snprintf(g_status, sizeof g_status, "pasted %d character%s", put, put == 1 ? "" : "s");
}

// --- find -------------------------------------------------------------
//
// The toolkit's find bar (ui/uui_findbar.h) over the editor; the hits
// are utext MARKS, so the text draws them behind itself. Case-blind, and
// the hit at or after the caret is the current one -- an editor's
// search starts where you are.

#define FIND_MAX 2048
static struct utext_mark g_marks[FIND_MAX];
static int g_nhits, g_hit_cur = -1;
static unsigned g_find_rev;
static int g_find_doc = -1;
static char g_find_q[UUI_TEXTBOX_MAX];

#define HIT_BG     ugfx_rgb(255, 231, 163)
#define HIT_CUR_BG ugfx_rgb(245, 184, 76)

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void find_paint(void) {
    for (int i = 0; i < g_nhits; i++) g_marks[i].bg = i == g_hit_cur ? HIT_CUR_BG : HIT_BG;
    T->marks = g_nhits ? g_marks : NULL;
    T->mark_count = g_nhits;
    g_find.matches = g_nhits;
    g_find.current = g_hit_cur + 1;
}

static void reveal_cursor(void);

static void find_select_current(void) {
    if (g_hit_cur < 0) return;
    T->ed.sel_anchor = g_marks[g_hit_cur].start;
    T->ed.cursor = g_marks[g_hit_cur].end;
    T->ed.sel_active = 1;
    reveal_cursor();
}

// Re-search the current document. `select` moves the selection to the
// current hit -- what typing in the field does, incrementally.
static void find_run(int select) {
    const char *q = uui_findbar_query(&g_find);
    int n = (int)strlen(q);
    strlcpy(g_find_q, q, sizeof g_find_q);
    g_nhits = 0;
    g_hit_cur = -1;
    int from = T->ed.cursor;
    if (utext_sel_present(T)) utext_sel_range(T, &from, 0);
    if (n > 0) {
        for (int i = 0; i + n <= T->count && g_nhits < FIND_MAX; i++) {
            int k = 0;
            while (k < n && fold(T->buf[i + k]) == fold(q[k])) k++;
            if (k < n) continue;
            g_marks[g_nhits].start = i;
            g_marks[g_nhits].end = i + n;
            if (g_hit_cur < 0 && i >= from) g_hit_cur = g_nhits;
            g_nhits++;
            i += n - 1;
        }
        if (g_hit_cur < 0 && g_nhits) g_hit_cur = 0;   // wraps, as every find does
    }
    g_find_rev = T->rev;
    g_find_doc = g_cur;
    find_paint();
    if (select) find_select_current();
}

static void find_step(int delta) {
    if (g_find_rev != T->rev || g_find_doc != g_cur) find_run(0);
    if (g_nhits <= 0) { set_status(g_find_q[0] ? "no matches" : "nothing to find"); return; }
    g_hit_cur = (g_hit_cur + delta + g_nhits) % g_nhits;
    find_paint();
    find_select_current();
}

static void find_open(void) {
    g_find_open = 1;
    // A selection on one line seeds the query, as in every editor.
    if (utext_sel_present(T)) {
        int s, e;
        utext_sel_range(T, &s, &e);
        if (e - s > 0 && e - s < UUI_TEXTBOX_MAX - 1) {
            char seed[UUI_TEXTBOX_MAX];
            int k = 0;
            for (int i = s; i < e && T->buf[i] != '\n'; i++) seed[k++] = T->buf[i];
            seed[k] = '\0';
            if (k == e - s) uui_textbox_set_text(&g_find.field, seed);
        }
    }
    uui_findbar_activate(&g_find);
    find_run(0);
}

static void find_close(void) {
    g_find_open = 0;
    uui_textbox_set_active(&g_find.field, 0);
    T->marks = NULL;
    T->mark_count = 0;
    g_nhits = 0;
    g_hit_cur = -1;
}

// --- item state -------------------------------------------------------
//
// Asked by the menu bar AND the command bar, per item, every draw: there
// is no "refresh the menu" step anywhere in this app.
static unsigned menu_item_flags(int code) {
    struct doc *d = cur();
    switch (code) {
    case CMD_SAVE:
        return doc_dirty(d) ? 0 : UUI_MI_DISABLED;
    case CMD_UNDO:
        return uui_undo_can_undo(&d->undo) ? 0 : UUI_MI_DISABLED;
    case CMD_REDO:
        return uui_undo_can_redo(&d->undo) ? 0 : UUI_MI_DISABLED;
    case CMD_DELETE:
    case CMD_CUT:
    case CMD_COPY:
        return utext_sel_present(T) ? 0 : UUI_MI_DISABLED;
    case CMD_PASTE:
        return g_clip_has_text ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT:
    case CMD_RECENT_0: return g_recent_count > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_1: return g_recent_count > 1 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_2: return g_recent_count > 2 ? 0 : UUI_MI_DISABLED;
    case CMD_STATUSBAR:
        return g_show_status ? UUI_MI_CHECKED : 0;
    case CMD_LINENUMS:
        return g_linenums ? UUI_MI_CHECKED : 0;
    case CMD_WORDWRAP:
        return utext_get_wrap(T) != UTEXT_WRAP_OFF ? UUI_MI_CHECKED : 0;
    case CMD_MARKDOWN:
        return d->preview ? UUI_MI_CHECKED : 0;
    case CMD_FIND:
        return g_find_open ? UUI_MI_CHECKED : 0;
    case CMD_FIND_NEXT:
        return g_find_q[0] ? 0 : UUI_MI_DISABLED;
    default:
        return 0;
    }
}

// --- drawing ----------------------------------------------------------

// THE OVERLAY BAR (ui/uui_scrollbar.h): a thin thumb at rest, the groove
// and a full thumb under the pointer -- the preview's beside it and the
// Start menu's look. No arrows: neither Windows 11 nor Plasma draws them.
#define NP_SCROLLBAR_FLAGS 0
static int g_vbar_hot, g_hbar_hot;
static int g_scrollbar_drag, g_hbar_drag;

// **utext WRAPS BY DIVIDING A WIDTH BY A CELL** (ui/utext.c), which is
// only true of a fixed advance, so everything that MEASURES or DRAWS the
// document selects the monospace family and puts the previous one back.
// The chrome is drawn by the toolkit afterwards, in the interface face.
static const struct ugfx_font *doc_font(void) {
    return ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
}

static void draw_scrollbar(struct ugfx_surface *s, int tx, int ty, int tw, int th) {
    int total, visible;
    utext_metrics(T, tw, th, &total, &visible);
    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
    // In PIXELS with the glide folded in, so the thumb moves with the lines.
    int total_px, vis_px, off_px;
    utext_bar_units(T, total, visible, &total_px, &vis_px, &off_px);
    // An overlay draws only its thumb: the column is the page's white.
    ugfx_fill_rect(s, bx, by, bw, bh, UTHEME_WHITE);
    uui_scrollbar_draw_overlay(s, bx, by, bw, bh, total_px, vis_px, off_px,
                               UTHEME_WHITE, UTHEME_TEXT,
                               (g_vbar_hot || g_scrollbar_drag) ? 255 : 0,
                               NP_SCROLLBAR_FLAGS | (g_scrollbar_drag ? UUI_SCROLLBAR_HELD : 0));
}

// The text columns a box holds, past the gutter -- measured as the
// widget measures, so the horizontal bar agrees with it.
static int doc_cols(int tw) {
    int g = 0;
    if (T->gutter) {
        int n = utext_line_count(T), digits = 1;
        while (n >= 10) { n /= 10; digits++; }
        if (digits < 3) digits = 3;
        // text-measure-ok: inside doc_font(), the mono family -- a fixed grid
        g = (digits + 2) * ugfx_char_w();
    }
    // text-measure-ok: inside doc_font(), the mono family -- a fixed grid
    int cols = (tw - g) / ugfx_char_w();
    return cols < 1 ? 1 : cols;
}

static void draw_document(struct ugfx_surface *s, int focused) {
    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(s->w, s->h, &tx, &ty, &tw, &th);
    // The caret shows only with keyboard focus and nothing over the
    // text taking the keys: a modal, an open menu, the find field.
    int caret = focused && !uui_filedialog_is_open(&g_fd) &&
                !uui_menubar_is_open(&g_menu) && !uui_menubar_is_open(&g_ctx) &&
                !(g_find_open && g_find.field.active);
    utext_draw(T, s, tx, ty, tw, th, UTHEME_TEXT, UTHEME_WHITE, UTHEME_SELECTION, caret);
    draw_scrollbar(s, tx, ty, tw, th);

    if (hbar_h() > 0) {
        int bx, by, bw, bh;
        hbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
        ugfx_fill_rect(s, bx, by, bw + scrollbar_w(), bh, UTHEME_WHITE);   // and the corner
        uui_scrollbar_draw_overlay(s, bx, by, bw, bh,
                                   utext_widest_line(T, tw, th), doc_cols(tw), T->hscroll,
                                   UTHEME_WHITE, UTHEME_TEXT,
                                   (g_hbar_hot || g_hbar_drag) ? 255 : 0,
                                   NP_SCROLLBAR_FLAGS | UUI_SCROLLBAR_HORIZ);
    }
    // The splitter's band on the page's white rather than the window grey
    // -- it draws only its hairline at rest.
    if (cur()->preview)
        ugfx_fill_rect(s, g_split.x, g_split.y, g_split.w, g_split.h, UTHEME_WHITE);
    ugfx_set_font(was_doc);
}

// THE PREVIEW FOLLOWS THE EDITOR: fed the buffer again when it changed,
// and scrolled to the block holding the editor's top line whenever that
// line or the text moved. Both are a walk of the document, so neither
// happens on a frame where nothing did.
static int g_md_doc = -1;
static unsigned g_md_rev;
static int g_md_top = -1;

// THE TOP STAYS PUT ACROSS A RESIZE. utext's scroll counts from the
// bottom (utext.h), so a window opened at one size and then given its
// remembered one showed a file from line 30. The first character on
// screen is remembered per frame and put back at the top after a resize.
static int g_view_w, g_view_h, g_view_doc = -1, g_view_top;

static void keep_top(int cw, int ch) {
    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(cw, ch, &tx, &ty, &tw, &th);
    if (g_view_doc == g_cur && (tw != g_view_w || th != g_view_h))
        utext_scroll_index_to_top(T, tw, th, g_view_top);
    g_view_w = tw;
    g_view_h = th;
    g_view_doc = g_cur;
    g_view_top = utext_top_index(T, tw, th);
    ugfx_set_font(was_doc);
}

static void sync_preview(int cw, int ch) {
    if (!cur()->preview) { g_md_doc = -1; return; }
    int changed = 0;
    if (g_md_doc != g_cur || g_md_rev != T->rev) {
        uui_markdown_set_text_live(&g_md, T->buf, T->count);
        g_md_doc = g_cur;
        g_md_rev = T->rev;
        changed = 1;
    }
    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(cw, ch, &tx, &ty, &tw, &th);
    int top = utext_top_line(T, tw, th);
    ugfx_set_font(was_doc);
    if (changed || top != g_md_top) {
        g_md.scroll = top == 0 ? 0 : uui_markdown_y_of(&g_md, utext_line_index(T, top));
        g_md_top = top;
    }
}

static void reveal_cursor(void) {
    if (!g_app) return;
    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(uapp_width(g_app), uapp_height(g_app), &tx, &ty, &tw, &th);
    utext_reveal_cursor(T, tw, th);
    ugfx_set_font(was_doc);
}

// --- the file chooser -------------------------------------------------

static void run_pending(struct uapp *a);

static void dlg_done(void *ctx, const char *path) {
    struct uapp *a = (struct uapp *)ctx;
    int after = g_after_save;
    g_after_save = 0;
    if (!path) {
        // A cancelled chooser cancels the CLOSE too: quitting here is
        // what the ask was put up to prevent.
        if (after) g_pending = PEND_NONE;
        set_status("cancelled");
        uapp_redraw(a);
        return;
    }
    if (g_dlg_saving) {
        int ok = save_file(a, cur(), path);
        if (after && ok) run_pending(a);
        else if (after) g_pending = PEND_NONE;   // the reason is in the status bar
    } else {
        open_path(a, path);
    }
    uapp_redraw(a);
}

// WHAT AN EDITOR CALLS A TEXT FILE is a convention about the NAME, not a
// probe: any file at all opens in an editor, and "All files" is beside it.
#define TEXT_EXTS ".txt .md .conf .log .c .h .py .sh .cfg .ini .json " \
                  ".desktop .scheme .saver .ids .service"

static int keep_text(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx; (void)dir;
    if (e->is_dir) return 0;
    const char *dot = strrchr(e->name, '.');
    return dot && uopen_ext_matches(TEXT_EXTS, dot);
}

static void file_dialog(int saving) {
    if (uui_filedialog_is_open(&g_fd)) return;
    g_dlg_saving = saving;
    uui_menubar_close(&g_menu);   // a modal owns the input

    struct doc *d = cur();
    char dir[PATH_MAX_LEN];
    if (!d->path[0] || !k_path_dirname(d->path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);

    static const struct uui_filedialog_filter types[] = {
        { "Text files", keep_text, 0 },
        UUI_FILEDIALOG_ALL_FILES,
    };
    struct uui_filedialog_opts o = {
        .mode = saving ? UUI_FILEDIALOG_SAVE : UUI_FILEDIALOG_OPEN,
        .title = saving ? "Save As" : "Open",
        .start_dir = dir,
        .initial_name = (saving && d->path[0]) ? k_path_basename(d->path) : "",
        .filters = types,
        .filter_count = (int)(sizeof types / sizeof types[0]),
    };
    if (!uui_filedialog_open(g_app, &g_fd, &o, dlg_done, g_app))
        set_status("cannot open the chooser");
}

// --- closing: tabs and the window -------------------------------------

static void ask_discard(struct uapp *a, int pending) {
    if (uui_dialog_is_open(&g_ask)) return;   // already asking; that answer decides
    g_pending = pending;
    static const struct uui_dialog_button btns[] = {
        { "Save", ASK_SAVE, 0 }, { "Don't Save", ASK_DISCARD, 0 }, { "Cancel", ASK_CANCEL, 0 },
    };
    snprintf(g_ask_line, sizeof g_ask_line, "Save changes to %s?",
             cur()->path[0] ? k_path_basename(cur()->path) : "this document");
    g_ask_rows[0] = g_ask_line;
    uui_menubar_close(&g_menu);   // a modal owns the input
    uui_menubar_close(&g_ctx);
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_ask, "Unsaved changes", g_ask_rows, 1, btns, 3, 0, ASK_CANCEL);
    // HERE as well as in on_motion: Alt+F4 and the X move no pointer, so
    // a gate that ran only on motion would leave the I-beam showing.
    uapp_set_cursor(a, WIN_CURSOR_DEFAULT);
    uapp_redraw(a);
}

// CLOSING THE WINDOW ASKS FOR EACH UNSAVED TAB IN TURN, showing it, and
// quits once none is left -- Kate's and Windows 11 Notepad's order.
static void close_all_step(struct uapp *a) {
    for (int i = 0; i < g_ndocs; i++) {
        if (!doc_dirty(g_docs[i])) continue;
        switch_to(i);
        ask_discard(a, PEND_CLOSE);
        return;
    }
    uapp_quit(a, 0);
}

static void close_tab(struct uapp *a, int i) {
    if (i < 0 || i >= g_ndocs) return;
    switch_to(i);
    if (!doc_dirty(cur())) { doc_remove(i); return; }
    ask_discard(a, PEND_CLOSE_TAB);
}

// The asked-about document is safe to throw away: saved or discarded.
static void run_pending(struct uapp *a) {
    int what = g_pending;
    g_pending = PEND_NONE;
    switch (what) {
    case PEND_CLOSE:
        doc_remove(g_cur);   // a quit is coming; this keeps the walk short
        close_all_step(a);
        break;
    case PEND_CLOSE_TAB:
        doc_remove(g_cur);
        break;
    default: break;
    }
}

// --- commands ---------------------------------------------------------
//
// The ONE place a command happens: the menu, the command bar and the
// keys all route here.

static void do_command(struct uapp *a, int code) {
    // An ACTION is an event a test waits for exactly once, so it is
    // never a layout line (docs/conventions/gui.md).
    ulogf("notepad: action %d\n", code);
    // dispatch-ok: THE SET IS THE APP'S OWN CMD_* LIST, which the menu,
    // the command bar and the key table all name -- the File Manager's
    // do_command() case: entries that are not uniform, one line each.
    switch (code) {
    case CMD_NEW:
        if (doc_new()) { switch_to(g_ndocs - 1); set_status("new tab"); }
        break;
    case CMD_OPEN:
        file_dialog(0);
        break;
    case CMD_SAVE:
        if (cur()->path[0]) save_file(a, cur(), cur()->path);
        else file_dialog(1);
        break;
    case CMD_SAVE_AS:
        file_dialog(1);
        break;
    case CMD_EXIT:
        close_all_step(a);
        break;
    case CMD_CLOSE_TAB:
        close_tab(a, g_cur);
        break;
    case CMD_NEXT_TAB:
        switch_to((g_cur + 1) % g_ndocs);
        break;
    case CMD_PREV_TAB:
        switch_to((g_cur + g_ndocs - 1) % g_ndocs);
        break;
    case CMD_RECENT_0:
    case CMD_RECENT_1:
    case CMD_RECENT_2: {
        int i = code - CMD_RECENT_0;
        if (i < g_recent_count) {
            // A COPY: opening calls recent_push(), which rewrites the slot.
            char path[PATH_MAX_LEN];
            strlcpy(path, g_recent[i], sizeof path);
            open_path(a, path);
        }
        break;
    }
    case CMD_UNDO:
        if (utext_undo(T)) { reveal_cursor(); set_status("undone"); }
        break;
    case CMD_REDO:
        if (utext_redo(T)) { reveal_cursor(); set_status("redone"); }
        break;
    case CMD_SELECT_ALL:
        utext_sel_all(T);
        set_status("selected all");
        break;
    case CMD_DELETE:
        if (utext_sel_present(T)) utext_sel_delete(T);
        break;
    case CMD_GOTO_TOP:
        T->ed.cursor = 0;
        utext_sel_clear(T);
        utext_scroll_top(T);
        break;
    case CMD_GOTO_END:
        T->ed.cursor = T->count;
        utext_sel_clear(T);
        utext_scroll_bottom(T);
        break;
    case CMD_STATUSBAR:
        g_show_status = !g_show_status;
        break;
    case CMD_LINENUMS:
        g_linenums = !g_linenums;
        for (int i = 0; i < g_ndocs; i++) g_docs[i]->text.gutter = g_linenums;
        break;
    case CMD_MARKDOWN:
        cur()->preview = !cur()->preview;
        set_status(cur()->preview ? "preview on -- Ctrl-E to hide it" : "preview off");
        break;
    case CMD_WORDWRAP: {
        int on = utext_get_wrap(T) != UTEXT_WRAP_OFF;
        utext_set_wrap(T, on ? UTEXT_WRAP_OFF : UTEXT_WRAP_WORD);
        set_status(on ? "word wrap off" : "word wrap on");
        break;
    }
    case CMD_FIND:
        if (g_find_open && g_find.field.active) find_close();
        else find_open();
        break;
    case CMD_FIND_NEXT:
        find_step(+1);
        break;
    case CMD_FIND_PREV:
        find_step(-1);
        break;
    case CMD_CUT:
        do_cut();
        break;
    case CMD_COPY:
        do_copy();
        break;
    case CMD_PASTE:
        do_paste();
        break;
    default:
        break;
    }
}

// --- keys -------------------------------------------------------------

// The accelerators, as a TABLE of command ids -- the same ids the menu
// and the bar run. Ctrl+letter arrives as its control code.
static const struct { int key; unsigned mods; int cmd; } g_keys[] = {
    { 0x0E, 0, CMD_NEW },        { 0x0F, 0, CMD_OPEN },      { 0x13, 0, CMD_SAVE },
    { 0x17, 0, CMD_CLOSE_TAB },  { 0x01, 0, CMD_SELECT_ALL }, { 0x18, 0, CMD_CUT },
    { 0x03, 0, CMD_COPY },       { 0x16, 0, CMD_PASTE },     { 0x05, 0, CMD_MARKDOWN },
    { 0x06, 0, CMD_FIND },       { 0x1A, 0, CMD_UNDO },      { 0x19, 0, CMD_REDO },
    { 'z', KEY_MOD_ALT, CMD_WORDWRAP },
    { KEY_PAGE_DOWN, KEY_MOD_CTRL, CMD_NEXT_TAB },
    { KEY_PAGE_UP,   KEY_MOD_CTRL, CMD_PREV_TAB },
    { KEY_F3, KEY_MOD_SHIFT, CMD_FIND_PREV },
    { KEY_F3, 0, CMD_FIND_NEXT },
};

static int accelerator(struct uapp *a, int key, unsigned mods) {
    for (unsigned i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++) {
        unsigned want = g_keys[i].mods;
        if (g_keys[i].key != key) continue;
        // A modifier the row names must be held; Ctrl on a control code
        // is implied by the code itself.
        if (want && (mods & want) != want) continue;
        if (!want && (mods & (KEY_MOD_ALT | KEY_MOD_SHIFT)) && key >= 0x80) continue;
        do_command(a, g_keys[i].cmd);
        return 1;
    }
    return 0;
}

static void editor_key(int key) {
    // Paging is about the VIEW rather than the text, so it is this app's:
    // a page here scrolls without moving the caret.
    if (key == KEY_PAGE_UP)   { utext_scroll(T, 5);  return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(T, -5); return; }

    // Everything else is the SHARED keymap (ui/uui_edit.h).
    if (utext_key(T, key, 0)) return;

    // Enter is declined by the core: a field commits, a document inserts
    // a newline. Over a selection the two are one step of the history.
    if (key == '\n' || key == '\r') {
        uui_undo_begin(&cur()->undo);
        utext_sel_delete(T);
        utext_insert(T, '\n');
        uui_undo_end(&cur()->undo);
    }
}

// --- Toykit callbacks -------------------------------------------------

static int g_dragging;
static int g_scrollbar_grab; // how far down the thumb the drag started
static int g_hbar_grab;

// Set when the router named a widget for the press now being delivered
// (on_widget runs before on_press for the same event), so a click a
// menu swallowed to dismiss itself never also moves the caret.
static int g_press_taken;

// docs/gui-guidelines.md: a GUI test asks the app where things are.
// `layout scrollbar` LEADS every frame -- tests cut the log there.
static void log_layout(struct uapp *a) {
    int tx, ty, tw, th, x, y, w, h;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);
    scrollbar_rect(tx, ty, tw, th, &x, &y, &w, &h);
    uapp_logf_layout("notepad: layout scrollbar %d %d %d %d\n", x, y, w, h);
    uapp_logf_layout("notepad: layout text %d %d %d %d\n", tx, ty, tw, th);
    if (hbar_h() > 0) {
        hbar_rect(tx, ty, tw, th, &x, &y, &w, &h);
        uapp_logf_layout("notepad: layout hscrollbar %d %d %d %d\n", x, y, w, h);
    }
    uapp_logf_layout("notepad: layout tabcount %d current %d\n", g_ndocs, g_cur);
    uapp_log_layout(a, "notepad");
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    refresh_tabs();
    set_title(a);
    layout_chrome(s->w, s->h);
    if (g_show_status) update_indicators();
    if (g_find_open && (g_find_rev != T->rev || g_find_doc != g_cur)) find_run(0);
    keep_top(s->w, s->h);
    sync_preview(s->w, s->h);
    draw_document(s, uapp_focused(a));
    log_layout(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (reason == UUI_REASON_PRESS) g_press_taken = 1;
    switch (id) {
    case ID_MENU: {
        int code = uui_menubar_take_code(&g_menu);   // PARKED in the widget
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_CTX: {
        int code = uui_menubar_take_code(&g_ctx);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(&g_tb);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_FIND:
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_NEXT:  find_step(+1); break;
        case UUI_FIND_PREV:  find_step(-1); break;
        case UUI_FIND_CLOSE: find_close(); break;
        default: break;
        }
        break;
    case ID_ASK: {
        // **-1 IS "NOTHING WAS COMMITTED", AND IT ARRIVES EVERY PRESS** --
        // the dialog parks its code on the release.
        int code = uui_dialog_take_code(&g_ask);
        if (code < 0) break;
        switch (code) {
        case ASK_DISCARD:
            run_pending(a);
            break;
        case ASK_SAVE:
            if (!cur()->path[0]) { g_after_save = 1; file_dialog(1); }
            else if (save_file(a, cur(), cur()->path)) run_pending(a);
            else g_pending = PEND_NONE;   // the failure is in the status bar
            break;
        case ASK_CANCEL:
        default:
            g_pending = PEND_NONE;
            break;
        }
        break;
    }
    default:
        break;
    }
    uapp_redraw(a);
}

// The strip's own callbacks say what happened directly.
static void tab_selected(void *ctx, int i) { (void)ctx; switch_to(i); if (g_app) uapp_redraw(g_app); }
static void tab_closed(void *ctx, int i)   { (void)ctx; if (g_app) { close_tab(g_app, i); uapp_redraw(g_app); } }
static void tab_new(void *ctx)             { (void)ctx; if (g_app) { do_command(g_app, CMD_NEW); uapp_redraw(g_app); } }

static void on_key(struct uapp *a, int key, unsigned mods) {
    // AN OPEN POPUP TAKES THE KEY, wherever the app thinks it is.
    int code;
    if (uui_menubar_key(&g_ctx, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }

    // THE FIND FIELD, while it has the keys: typing searches as it goes.
    if (g_find_open && g_find.field.active && key != 0x06 &&
        uui_findbar_key(&g_find, key, mods)) {
        switch (uui_findbar_take(&g_find)) {
        case UUI_FIND_CHANGED: find_run(1); break;
        case UUI_FIND_NEXT:    find_step(+1); break;
        case UUI_FIND_PREV:    find_step(-1); break;
        case UUI_FIND_CLOSE:   find_close(); break;
        default: break;
        }
        uapp_redraw(a);
        return;
    }

    if (accelerator(a, key, mods)) {
        uapp_redraw(a);
        return;
    }

    // Esc is NOT a quit key; Alt+F4 never reaches here (the WM asks
    // through on_close_cb). With find open, Esc closes it.
    if (key == 0x1B) {
        if (g_find_open) { find_close(); uapp_redraw(a); }
        return;
    }
    // Ctrl or Alt with a character is a shortcut, never typed.
    if (uui_key_is_shortcut(key, mods)) return;

    editor_key(key);
    // THE CARET STAYS ON SCREEN: typing where the view is not looking has
    // to bring the view along.
    reveal_cursor();
    uapp_redraw(a);
}

// A click in the trough jumps there; on the thumb it starts a drag.
static int scrollbar_press(int px, int py, int tx, int ty, int tw, int th) {
    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
    int total, visible;
    utext_metrics(T, tw, th, &total, &visible);
    enum uui_scrollbar_zone z =
        uui_scrollbar_hit(bx, by, bw, bh, total, visible, T->scroll_offset,
                           px, py, NP_SCROLLBAR_FLAGS);
    switch (z) {
    case UUI_SB_NONE:  return 0;
    case UUI_SB_UP:    utext_scroll(T, 1); return 1;
    case UUI_SB_DOWN:  utext_scroll(T, -1); return 1;
    case UUI_SB_ABOVE: utext_scroll(T, visible); return 1;
    case UUI_SB_BELOW: utext_scroll(T, -visible); return 1;
    case UUI_SB_THUMB: {
        // WHERE on the thumb, or the thumb leaps by that much on the drag.
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(by, bh, total, visible, T->scroll_offset,
                                  &thumb_y, &thumb_h, bw, NP_SCROLLBAR_FLAGS);
        g_scrollbar_grab = py - thumb_y;
        g_scrollbar_drag = 1;
        return 1;
    }
    }
    return 1;
}

// hscroll is the app's to clamp: utext has no view to measure against
// outside a draw.
static void clamp_hscroll(int tw, int th) {
    int max = utext_widest_line(T, tw, th) - doc_cols(tw);
    if (max < 0) max = 0;
    if (T->hscroll > max) T->hscroll = max;
    if (T->hscroll < 0) T->hscroll = 0;
}

static int hbar_press(int px, int py, int tx, int ty, int tw, int th) {
    if (hbar_h() <= 0) return 0;
    int bx, by, bw, bh;
    hbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
    int cols = doc_cols(tw);
    int total = utext_widest_line(T, tw, th);
    unsigned flags = NP_SCROLLBAR_FLAGS | UUI_SCROLLBAR_HORIZ;
    enum uui_scrollbar_zone z =
        uui_scrollbar_hit(bx, by, bw, bh, total, cols, T->hscroll, px, py, flags);
    switch (z) {
    case UUI_SB_NONE:  return 0;
    case UUI_SB_UP:    T->hscroll -= 1;    break;   // the LEFT arrow
    case UUI_SB_DOWN:  T->hscroll += 1;    break;
    case UUI_SB_ABOVE: T->hscroll -= cols; break;
    case UUI_SB_BELOW: T->hscroll += cols; break;
    case UUI_SB_THUMB: {
        int thumb_x, thumb_w;
        uui_scrollbar_thumb_rect(bx, bw, total, cols, T->hscroll,
                                  &thumb_x, &thumb_w, bh, flags);
        g_hbar_grab = px - thumb_x;
        g_hbar_drag = 1;
        return 1;
    }
    }
    clamp_hscroll(tw, th);
    return 1;
}

static void on_wheel(struct uapp *a, int notches) {
    // Only reached when no widget took the wheel (the preview takes its
    // own). Three lines a notch.
    utext_scroll(T, notches * 3);
    uapp_redraw(a);
}

// A SECONDARY CLICK ARMS THE CONTEXT MENU; it opens on the RELEASE.
static int g_ctx_armed;
static int g_ctx_x, g_ctx_y;

static int in_editor(struct uapp *a, int x, int y) {
    int ew = editor_w(uapp_width(a));
    return x < ew && y >= chrome_top() && y < uapp_height(a) - statusbar_h();
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_press_taken) { g_press_taken = 0; return; }
    if (uui_filedialog_is_open(&g_fd)) return;   // the modal keeps it
    if (!in_editor(a, x, y)) return;

    if (buttons & 0x2) {
        if (uui_menubar_is_open(&g_ctx)) {
            uui_menubar_close(&g_ctx);   // a second right-click dismisses
        } else {
            uui_menubar_close(&g_menu);  // one popup at a time
            g_ctx_armed = 1;
            g_ctx_x = x;
            g_ctx_y = y;
        }
        uapp_redraw(a);
        return;   // NEVER moves the caret: it acts on the selection there
    }

    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);
    if (scrollbar_press(x, y, tx, ty, tw, th)) {
    } else if (hbar_press(x, y, tx, ty, tw, th)) {
    } else if (x < tx + tw && y >= ty && y < ty + th) {
        // A click in the text gives it the keys back from the find field.
        if (g_find_open) uui_textbox_set_active(&g_find.field, 0);
        T->ed.cursor = utext_index_at_point(T, tx, ty, tw, th, x, y);
        utext_sel_start(T);
        if (T->ed.undo) uui_undo_break(T->ed.undo);   // a moved caret ends a typing run
        g_dragging = 1;
    }
    uapp_redraw(a);
    ugfx_set_font(was_doc);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    const struct ugfx_font *was_doc = doc_font();
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);

    // The I-beam over the text and nothing else, set on EVERY motion --
    // it is a state. Not under a modal (the toolkit walks through an
    // overlay, docs/bugs.md), and not over the splitter, whose own
    // resize cursor the toolkit names.
    if (!uui_filedialog_is_open(&g_fd) && in_editor(a, x, y)) {
        int over_text = x >= tx && x < tx + tw && y >= ty && y < ty + th &&
                        !uui_menubar_is_open(&g_menu) &&
                        !uui_dialog_is_open(&g_ask) &&
                        !(g_find_open && uui_hit(g_find.x, g_find.y, g_find.w, g_find.h, x, y));
        uapp_set_cursor(a, over_text ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT);
    }

    // The overlay bars widen under the pointer.
    {
        int bx, by, bw, bh, vhot, hhot = 0;
        scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
        vhot = uui_hit(bx, by, bw, bh, x, y);
        if (hbar_h() > 0) {
            hbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
            hhot = uui_hit(bx, by, bw, bh, x, y);
        }
        if (vhot != g_vbar_hot || hhot != g_hbar_hot) {
            g_vbar_hot = vhot;
            g_hbar_hot = hhot;
            uapp_redraw(a);
        }
    }

    if (!buttons) { ugfx_set_font(was_doc); return; }
    if (g_scrollbar_drag) {
        int total, visible;
        utext_metrics(T, tw, th, &total, &visible);
        if (total > visible) {
            int bx, by, bw, bh;
            scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
            utext_scroll_set(T,
                uui_scrollbar_offset_for_drag(by, bh, total, visible, y,
                                               g_scrollbar_grab, bw, NP_SCROLLBAR_FLAGS));
            uapp_redraw(a);
        }
    } else if (g_hbar_drag) {
        int bx, by, bw, bh;
        hbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
        T->hscroll = uui_scrollbar_offset_for_drag(bx, bw, utext_widest_line(T, tw, th),
                                                   doc_cols(tw), x, g_hbar_grab, bh,
                                                   NP_SCROLLBAR_FLAGS | UUI_SCROLLBAR_HORIZ);
        uapp_redraw(a);
    } else if (g_dragging) {
        T->ed.cursor = utext_index_at_point(T, tx, ty, tw, th, x, y);
        uapp_redraw(a);
    }
    ugfx_set_font(was_doc);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    g_dragging = 0;
    g_scrollbar_drag = 0;
    g_hbar_drag = 0;
    if (g_ctx_armed) {
        g_ctx_armed = 0;
        if (!uui_filedialog_is_open(&g_fd))
            uui_menubar_open_at(&g_ctx, ctx_items,
                                 (int)(sizeof ctx_items / sizeof ctx_items[0]),
                                 g_ctx_x, g_ctx_y);
    }
    uapp_redraw(a);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uui_toolbar_tick(&g_tb);   // the tooltips
}

// A file named on the command line, opened once the window exists --
// the File Manager spawns this with a path (Handles= in the .desktop).
static char g_arg_path[PATH_MAX_LEN];

// Somebody replaced the clipboard -- possibly this app. Paste greys and
// ungreys with it.
static void on_clipboard_cb(struct uapp *a, int op, unsigned serial) {
    (void)op; (void)serial;
    clip_refresh();
    uapp_redraw(a);
}

static void on_open_cb(struct uapp *a) {
    g_app = a;
    if (!doc_new()) { uapp_quit(a, 1); return; }
    g_cur = 0;
    set_status("F10 for the menu -- Ctrl-O open, Ctrl-S save, Ctrl-F find");

    for (int i = 0; i < RECENT_MAX; i++) strlcpy(g_recent[i], "(empty)", PATH_MAX_LEN);

    uui_menubar_init(&g_ctx, 0, 0);   // no bar of its own: open_at() supplies the rows
    g_ctx.item_flags = menu_item_flags;
    uui_markdown_init(&g_md);
    uui_findbar_init(&g_find);
    g_find.pill = 1;
    uui_splitter_init(&g_split, 1, 500);
    g_split.line = UTHEME_SEPARATOR;
    uui_tabs_init(&g_tabs, g_tablist, 0, 0);
    g_tabs.on_select = tab_selected;
    g_tabs.on_close = tab_closed;
    g_tabs.on_new = tab_new;
    g_tabs.show_new = 1;
    g_tabs.baseline = UTHEME_SEPARATOR;   // a light rule over the white page
    refresh_tabs();
    clip_refresh();   // the broadcast only fires on a CHANGE, so ask once

    uui_dialog_init(&g_ask);
    uui_statusbar_init(&g_statusbar);
    g_statusbar.count = 5;
    g_statusbar.panes[0].text = g_status; g_statusbar.panes[0].chars = 0;
    g_statusbar.panes[1].text = g_lncol;  g_statusbar.panes[1].chars = 14;
    g_statusbar.panes[2].text = g_chars;  g_statusbar.panes[2].chars = 16;
    g_statusbar.panes[3].text = g_kind;   g_statusbar.panes[3].chars = 10;
    g_statusbar.panes[4].text = g_eol;    g_statusbar.panes[4].chars = 13;
    update_indicators();

    // AFTER the widgets exist: opening writes the status and the title.
    if (g_arg_path[0]) {
        open_path(a, g_arg_path);
        set_title(a);
    }
}

// The X, Alt+F4 and the window menu. Refusing is not returning 1 -- the
// asks answer frames later and quit through run_pending().
static int on_close_cb(struct uapp *a) {
    if (!any_dirty()) return 1;
    close_all_step(a);
    return 0;
}

// SIZED FROM THE FONT: seventy columns of the document's face and
// twenty rows, plus the chrome -- the desktop stays visible around it. Runs before on_open, so the bars it measures
// are initialised in main().
static void default_size(int *w, int *h) {
    const struct ugfx_font *was = doc_font();
    // text-measure-ok: the mono family -- the document is a fixed grid
    int cw = ugfx_char_w(), chh = ugfx_char_h();
    ugfx_set_font(was);
    *w = 70 * cw + scrollbar_w();
    *h = 20 * chh + menubar_h() + toolbar_h() + tabs_h() + uui_statusbar_height(&g_statusbar);
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) strlcpy(g_arg_path, argv[1], sizeof g_arg_path);

    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;
    uui_toolbar_init(&g_tb, tb_items, (int)(sizeof tb_items / sizeof tb_items[0]));
    g_tb.item_flags = menu_item_flags;
    g_tb.accent_latch = 1;
    g_tb.overflow = 1;
    uui_statusbar_init(&g_statusbar);

    struct uapp_desc desc = {
        .title        = "untitled",
        .app_id       = "notepad",
        .on_size      = default_size,
        .x            = 180,
        .y            = 90,
        // Resizable with no resize code: the text area is derived from
        // the content size. The minimum is the one tools/popup_test.py
        // opens it at.
        .flags        = UAPP_RESIZABLE,
        .min_w        = 240,
        .min_h        = 120,
        .tick_ms      = 500,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open_cb,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_wheel     = on_wheel,
        .on_tick      = on_tick,
        .on_clipboard = on_clipboard_cb,
        .on_close     = on_close_cb,
    };
    return uapp_run(&desc);
}
