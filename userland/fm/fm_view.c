// Layout, drawing, and the geometry a test asserts on.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_dialog.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "lib/human.h"
#include <string.h>
#include <stdio.h>

// --- layout and drawing ------------------------------------------------

static int menubar_h(void) { int h; uui_menubar_natural_size(&g_menu, 0, &h); return h; }

static int toolbar_h(void) { int h; uui_toolbar_natural_size(&g_toolbar, 0, &h); return h; }

static int statusbar_h(void) { int h; uui_statusbar_natural_size(&g_status, 0, &h); return h; }

// Each pane carries its OWN path above it. One shared status line
// cannot say where two panes are, and "which directory does F5 copy
// into" is a question the window has to answer without being asked.
static int panehdr_h(void) { return ugfx_char_h() + utheme_gap(); }

// What a pane or the tree must keep however hard the divider is
// dragged. Font-derived: eight columns is about the least in which a
// filename is still a filename, not a pixel count that stops meaning
// anything at another font size.
static int min_col_w(void) { return ugfx_char_w() * 8; }

void layout_all(int cw, int ch) {
    int mb = menubar_h(), tb = toolbar_h(), sb = statusbar_h();

    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mb);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    // The context menu has no strip of its own; only its popup bounds
    // matter, and they are the whole content area.
    uui_menubar_set_geometry(&g_ctx, 0, 0, 0, 0);
    uui_menubar_set_bounds(&g_ctx, 0, 0, cw, ch);
    uui_dialog_set_bounds(&g_dialog, 0, 0, cw, ch);
    uui_toolbar_ops.set_geometry(&g_toolbar, 0, mb, cw, tb);
    // CANCEL SITS ON THE STATUS BAR, at its right end, and exists only
    // while an operation runs -- see g_cancel_btn's own note.
    int running = fm_job_running();
    widget_by_id(ID_CANCEL)->hidden = !running;
    if (running) {
        int bw, bh;
        uui_button_natural_size(&g_cancel_btn, &bw, &bh);
        if (bh > sb) bh = sb;
        uui_button_set_geometry(&g_cancel_btn, cw - bw - 2, ch - sb + (sb - bh) / 2,
                                 bw, bh);
        // The status bar stops short of it, or the last pane's text
        // draws straight under the button.
        uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw - bw - 4, sb);
    } else {
        uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);
    }

    int top = mb + tb;
    int hdr = panehdr_h();
    int panes_y = top + hdr;
    int panes_h = ch - top - sb - hdr;
    if (panes_h < 1) panes_h = 1;

    // Both dividers span the body: from under the toolbar to above the
    // key row, so each one separates the pane HEADERS as well as the
    // listings and there is no stub of undivided strip at the top.
    int body_h = ch - top - sb;
    int split_w = uui_splitter_thickness();
    int minw = min_col_w();

    // The tree column sits left of the panes and spans their headers
    // too -- it has no path strip of its own.
    int tx = 0;
    if (g_tree_on) {
        // Its divider must leave room for BOTH panes, not one: the
        // shown-panes count is what the tree is competing with.
        uui_splitter_set_track(&g_tree_split, 0, cw, minw,
                                (g_single ? 1 : 2) * minw + split_w);
        int tw = uui_splitter_before(&g_tree_split);
        uui_tree_ops.set_geometry(&g_tree, 0, top, tw, body_h);
        uui_splitter_set_geometry(&g_tree_split, uui_splitter_pos(&g_tree_split),
                                   top, split_w, body_h);
        tx = tw + split_w;
    }
    int pw = cw - tx;

    if (g_single) {
        // Both panes get the full rect; only the active one is SHOWN.
        // The hidden one keeps sane geometry so nothing draws from junk
        // the frame it comes back.
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y, pw, panes_h);
        uui_fileview_set_geometry(&g_pane[1], tx, panes_y, pw, panes_h);
    } else {
        uui_splitter_set_track(&g_pane_split, tx, cw, minw, minw);
        int px = uui_splitter_pos(&g_pane_split);
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y,
                                   uui_splitter_before(&g_pane_split), panes_h);
        uui_splitter_set_geometry(&g_pane_split, px, top, split_w, body_h);
        uui_fileview_set_geometry(&g_pane[1], px + split_w, panes_y,
                                   cw - px - split_w, panes_h);
    }

    // Visibility is decided beside the geometry: the router skips a
    // hidden item, so a hidden pane cannot be clicked either.
    widget_by_id(ID_LEFT)->hidden  = g_single && g_active != 0;
    widget_by_id(ID_RIGHT)->hidden = g_single && g_active != 1;
    widget_by_id(ID_TREE)->hidden = !g_tree_on;
    widget_by_id(ID_TREE_SPLIT)->hidden = !g_tree_on;
    widget_by_id(ID_PANE_SPLIT)->hidden = g_single;

    // The ACTIVE pane's outline, drawn by the widget itself so it stays
    // under a menu popup -- see uui_fileview.h's active_mark. With two
    // identical panes and no other mark, "which one does F5 copy FROM"
    // is unanswerable, and a wrong guess deletes the wrong file.
    for (int i = 0; i < 2; i++)
        uui_fileview_set_active_mark(&g_pane[i], i == g_active, UTHEME_ACCENT);

}

// The path strip above each pane. The ACTIVE one is drawn in the accent
// colour, which is the same thing the outline says and deliberately so:
// the mark that answers "which pane" should be readable at a glance and
// from the text you are already looking at.
static void draw_pane_headers(struct ugfx_surface *s) {
    int hdr = panehdr_h();
    for (int i = 0; i < 2; i++) {
        if (g_single && i != g_active) continue;
        int x, y, w, h;
        uui_fileview_ops.bounds(&g_pane[i], &x, &y, &w, &h);
        (void)h;
        int active_pane = (i == g_active);
        uint32_t bg = active_pane ? UTHEME_ACCENT : UTHEME_PANEL_BG;
        uint32_t fg = active_pane ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
        ugfx_fill_rect(s, x, y - hdr, w, hdr, bg);
        // Clipped, always: a path is longer than a half-window
        // routinely, and ugfx_draw_string() does not clip
        // (docs/gui-guidelines.md's oldest trap).
        ugfx_draw_string_clipped(s, x + utheme_gap(), y - hdr + utheme_gap() / 2,
                                  w - utheme_gap() * 2, uui_fileview_dir(&g_pane[i]),
                                  fg, bg);
    }
}

// docs/gui-guidelines.md: a GUI test asks the app where things are
// rather than re-deriving geometry in Python. The grammar is notepad's
// and imgview's, deliberately -- one parser in tools/.
void log_layout(void) {
    int x, y, w, h;
    for (int i = 0; i < 2; i++) {
        uui_fileview_ops.bounds(&g_pane[i], &x, &y, &w, &h);
        uapp_logf_layout("files: layout pane %d %d %d %d %d\n", i, x, y, w, h);
        uapp_logf_layout("files: layout dir %d %s\n", i, uui_fileview_dir(&g_pane[i]));
        uapp_logf_layout("files: layout rows %d %d\n", i, uui_fileview_row_count(&g_pane[i]));
    }
    const char *sel = uui_fileview_selected_name(active());
    uapp_logf_layout("files: layout active %d\n", g_active);
    // THE ROW HEIGHT, because a test aiming at "row 2" otherwise
    // guesses it -- and a guess that is a few pixels out lands on empty
    // space below the last row, which selects nothing and reads exactly
    // like a broken click.
    uapp_logf_layout("files: layout rowh %d\n", uui_table_row_h(&g_pane[0].table));
    uui_toolbar_ops.bounds(&g_toolbar, &x, &y, &w, &h);
    uapp_logf_layout("files: layout toolbar %d %d %d %d\n", x, y, w, h);
    for (int i = 0; uui_toolbar_item_rect(&g_toolbar, i, &x, &y, &w, &h); i++)
        uapp_logf_layout("files: layout tbitem %d %d %d %d %d\n", i, x, y, w, h);
    // The popup's rect is the menu's own `menu.popup 0` line now.
    // Depth and the top popup's hot row: the one logged fact that CHANGES
    // as the pointer crosses an open menu. Without it a hover test's
    // frames are identical, the dedup drops them, and "nothing arrived"
    // reads as a wedge.
    uapp_logf_layout("files: layout menuhot %d %d\n", g_menu.depth,
          g_menu.depth > 0 ? g_menu.level[0].hot : -1);
    uapp_logf_layout("files: layout split %d %d\n",
          uui_splitter_frac(&g_tree_split), uui_splitter_frac(&g_pane_split));
    for (int i = 0; i < 2; i++) {
        const struct uui_splitter *sp = i ? &g_pane_split : &g_tree_split;
        uui_splitter_ops.bounds(sp, &x, &y, &w, &h);
        uapp_logf_layout("files: layout splitbox %d %d %d %d %d\n", i, x, y, w, h);
    }
    uapp_logf_layout("files: layout view %d %d single %d tree %d %d\n",
          (int)g_pane[0].mode, (int)g_pane[1].mode, g_single, g_tree_on,
          g_tree_on ? g_tree_count : 0);
    if (g_tree_on) {
        uui_tree_ops.bounds(&g_tree, &x, &y, &w, &h);
        uapp_logf_layout("files: layout treebox %d %d %d %d %d %d\n", x, y, w, h,
              uui_tree_row_h(&g_tree), uui_tree_selected_id(&g_tree));
    }
    uapp_logf_layout("files: layout selected %s\n", sel ? sel : "-");
    uapp_logf_layout("files: layout modal %d\n", (int)g_modal);
    // WHERE ROW 0 ACTUALLY STARTS, per pane. A test that derives it as
    // "pane top + n * row height" is off by the column header and lands
    // on the row above -- silently, since a neighbouring row is a
    // perfectly plausible thing to have clicked.
    for (int i = 0; i < 2; i++)
        uapp_logf_layout("files: layout rowy %d %d\n", i,
                          g_pane[i].table.y + uui_table_header_h(&g_pane[i].table));

    // The DIMMED count per pane -- a staged cut, which is otherwise only
    // visible as a shade of grey no test can assert on.
    uapp_logf_layout("files: layout dim %d %d\n", g_pane[0].dim_count,
                      g_pane[1].dim_count);
    // ...and whether the Cancel button is up, which is exactly "is an
    // operation running" as far as anything on screen is concerned.
    uapp_logf_layout("files: layout cancel %d\n",
                      widget_by_id(ID_CANCEL)->hidden ? 0 : 1);
    uapp_logf_layout("files: layout marked %d %d\n", uui_fileview_mark_count(&g_pane[0]),
          uui_fileview_mark_count(&g_pane[1]));
    // ...and the hovered row as a VIEW position too. `hover` above is a
    // SOURCE row, which a test cannot turn back into a screen row -- so
    // aiming the pointer at "the third row down" had no way to confirm
    // it got there, and an 18px row plus pointer acceleration means it
    // often did not.
    for (int i = 0; i < 2; i++)
        uapp_logf_layout("files: layout hoverv %d %d\n", i,
                          uui_table_view_row(&g_pane[i].table,
                                              g_pane[i].table.hovered));
    uapp_logf_layout("files: layout hover %d %d\n", g_pane[0].table.hovered,
          g_pane[1].table.hovered);
    for (int i = 0; i < 2; i++) {
        int cx, cy, cw2, ch2;
        if (!uui_fileview_cell_rect(&g_pane[i], 0, &cx, &cy, &cw2, &ch2))
            continue;
        int cols = 1, xx, yy, ww, hh;
        while (uui_fileview_cell_rect(&g_pane[i], cols, &xx, &yy, &ww, &hh) &&
                yy == cy)
            cols++;
        uapp_logf_layout("files: layout cellgrid %d %d %d %d %d %d\n", i,
              cx, cy, cw2, ch2, cols);
    }
    uapp_logf_layout("files: layout job %d %d\n", g_job_at, g_job_count);
    uapp_logf_layout("files: layout ctx %d\n", uui_menubar_is_open(&g_ctx));
    // Whether the conflict dialog is UP. A test that sleeps and then
    // types is a test whose keys go to the listing when the dialog is
    // half a second late -- and Enter on a listing descends.
    // ...and WHICH BUTTON a Return would commit. A test that counts
    // arrow presses is measuring its own keystroke delivery, not the
    // dialog (four sent, three arrived, and "Rename" read as broken).
    uapp_logf_layout("files: layout dialog %d %d\n",
                      uui_dialog_is_open(&g_dialog), g_dialog.hot);
    if (uui_menubar_popup_rect(&g_ctx, 0, &x, &y, &w, &h))
        uapp_logf_layout("files: layout ctxbox %d %d %d %d\n", x, y, w, h);
}

void on_draw(struct uapp *a, struct uapp_draw *d) {
    layout_all(d->surface->w, d->surface->h);
    ugfx_fill_rect(d->surface, 0, 0, d->surface->w, d->surface->h, UTHEME_PANEL_BG);
    draw_pane_headers(d->surface);
    uui_statusbar_draw(d->surface, &g_status);
    if (!widget_by_id(ID_CANCEL)->hidden) uui_button_draw_one(d->surface, &g_cancel_btn);
    log_layout();
    // AFTER the app's own report: tools/filemanager_test.py takes `pane 0`
    // as the start of a frame, so the widgets' lines must follow it.
    uapp_log_layout(a, "files");   // the widgets by name (ui/uui_describe.h)
}

void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    // ONLY the modal may live here: on_draw_over runs after the
    // router's overlay pass, so anything drawn from it sits on top of
    // an open menu. The active-pane outline moved into the widget for
    // exactly that reason (uui_fileview.h's active_mark).
    draw_modal(d->surface);
}

// Which VISIBLE pane holds this point, or -1.
int pane_at(int x, int y) {
    for (int i = 0; i < 2; i++) {
        if (widget_by_id(i ? ID_RIGHT : ID_LEFT)->hidden) continue;
        if (uui_fileview_hit(&g_pane[i], x, y)) return i;
    }
    return -1;
}
