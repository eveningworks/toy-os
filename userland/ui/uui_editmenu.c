// The shared edit menu -- see ui/uui_editmenu.h.
#include "ui/uui_editmenu.h"
#include "ui/uui_menubar.h"
#include "ui/uui_undo.h"

enum { ED_UNDO = 1, ED_REDO, ED_CUT, ED_COPY, ED_PASTE, ED_DELETE, ED_SELECT_ALL };

static const struct uui_menu_item g_rows[] = {
    UUI_MENU_ICON("Undo",       ED_UNDO,       "Ctrl-Z", "tb-undo",   0),
    UUI_MENU_ICON("Redo",       ED_REDO,       "Ctrl-Y", "tb-redo",   0),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Cut",        ED_CUT,        "Ctrl-X", "tb-cut",    0),
    UUI_MENU_ICON("Copy",       ED_COPY,       "Ctrl-C", "tb-copy",   0),
    UUI_MENU_ICON("Paste",      ED_PASTE,      "Ctrl-V", "tb-paste",  0),
    UUI_MENU_ICON("Delete",     ED_DELETE,     "Del",    "tb-delete", 0),
    UUI_MENU_SEP,
    UUI_MENU("Select All",      ED_SELECT_ALL, "Ctrl-A"),
};
// Read-only text: the rows that would change it are LEFT OUT, not
// greyed -- a viewer has nothing to undo or paste into, ever.
static const struct uui_menu_item g_ro_rows[] = {
    UUI_MENU_ICON("Copy",       ED_COPY,       "Ctrl-C", "tb-copy",   0),
    UUI_MENU_SEP,
    UUI_MENU("Select All",      ED_SELECT_ALL, "Ctrl-A"),
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static struct uui_menubar g_menu;
static struct uui_edit_target g_t;   // valid while the menu is open
static int g_target_id;
static int g_inited;
static int g_clip_text;   // read once at open: the flags run every draw

static struct uui_item g_item = {
    .ops = &uui_menubar_ops, .widget = &g_menu, .id = UUI_EDITMENU_ID, .name = "editmenu",
};

struct uui_item *uui_editmenu_item(void) { return &g_item; }

static int has_undo(int redo) {
    if (!g_t.ed->undo) return 0;
    return redo ? uui_undo_can_redo(g_t.ed->undo) : uui_undo_can_undo(g_t.ed->undo);
}

static unsigned row_flags(int code) {
    if (!g_t.ed) return UUI_MI_DISABLED;
    int sel = uui_edit_has_selection(g_t.ed);
    int masked = (g_t.ed->flags & UUI_EDIT_MASKED) != 0;
    int ok;
    switch (code) {
    case ED_UNDO:       ok = has_undo(0); break;
    case ED_REDO:       ok = has_undo(1); break;
    case ED_CUT:
    case ED_COPY:       ok = sel && !masked; break;
    case ED_PASTE:      ok = g_clip_text; break;
    case ED_DELETE:     ok = sel; break;
    case ED_SELECT_ALL: ok = g_t.ops->len(g_t.text) > 0; break;
    default:            ok = 0; break;
    }
    return ok ? 0 : UUI_MI_DISABLED;
}

void uui_editmenu_open(const struct uui_edit_target *t, int target_id,
                       int x, int y, int bw, int bh) {
    if (!g_inited) {
        uui_menubar_init(&g_menu, 0, 0);   // no bar: a context menu (ui/uui_menubar.h)
        g_menu.item_flags = row_flags;
        g_inited = 1;
    }
    g_t = *t;
    g_target_id = target_id;
    g_clip_text = uui_edit_clip_has_text();
    uui_menubar_set_bounds(&g_menu, 0, 0, bw, bh);
    if (t->ed->flags & UUI_EDIT_READONLY)
        uui_menubar_open_at(&g_menu, g_ro_rows, COUNT(g_ro_rows), x, y);
    else
        uui_menubar_open_at(&g_menu, g_rows, COUNT(g_rows), x, y);
}

int uui_editmenu_is_open(void) { return g_inited && uui_menubar_is_open(&g_menu); }

void uui_editmenu_close(void) { if (g_inited) uui_menubar_close(&g_menu); }

// Runs one row. 1 when the text changed.
static int run(int code) {
    struct uui_edit *e = g_t.ed;
    const struct uui_edit_ops *ops = g_t.ops;
    void *text = g_t.text;
    if (!e || (row_flags(code) & UUI_MI_DISABLED)) return 0;
    switch (code) {
    case ED_UNDO:       return uui_edit_undo(e, ops, text);
    case ED_REDO:       return uui_edit_redo(e, ops, text);
    case ED_CUT:        return uui_edit_cut(e, ops, text);
    case ED_COPY:       uui_edit_copy(e, ops, text); return 0;
    case ED_PASTE:      return uui_edit_paste(e, ops, text) > 0;
    case ED_DELETE:
        if (e->undo) uui_undo_break(e->undo);
        return uui_edit_delete_selection(e, ops, text);
    case ED_SELECT_ALL: uui_edit_select_all(e, ops, text); return 0;
    default:            return 0;
    }
}

int uui_editmenu_take(int *out_target_id) {
    int code = uui_menubar_take_code(&g_menu);
    if (code <= 0 || !run(code)) return 0;
    if (out_target_id) *out_target_id = g_target_id;
    return 1;
}

int uui_editmenu_key(int key, int *out_target_id) {
    if (!uui_editmenu_is_open()) return 0;
    int code;
    if (!uui_menubar_key(&g_menu, key, &code)) return 0;
    if (code > 0 && run(code)) {
        if (out_target_id) *out_target_id = g_target_id;
        return 2;
    }
    return 1;
}
