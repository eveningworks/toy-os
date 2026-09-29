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

static int toolbar_h(void) { return uui_toolbar_height(&g_toolbar); }
static int statusbar_h(void) { return uui_statusbar_height(&g_status); }

// The top row: Back/Forward/Up beside the breadcrumb and the search box,
// as tall as the tallest of them.
static int navrow_h(void) {
    int ph, nh = uui_toolbar_height(&g_nav);
    uui_pathbar_ops.natural_size(&g_path, 0, &ph);
    return (nh > ph + 8 ? nh : ph + 8);
}

// Each pane carries its OWN path above it in SPLIT view -- an address
// bar (a uui_textbox, see fm_internal.h) -- since one breadcrumb cannot
// say where two panes are. One pane needs none: the breadcrumb is it.
static int panehdr_h(void) {
    if (g_single) return 0;
    int h;
    uui_textbox_natural_size(&g_addr[0], 0, &h);
    return h;
}

// The strip's colours. The ACTIVE pane's is the accent, which is the
// same thing the outline says and deliberately so: "which pane" should
// be readable from the text you are already looking at. While it is
// being EDITED it is a field like any other, white with a caret,
// because typing over an accent background is unreadable.
static void addr_style(int i) {
    struct uui_textbox *f = &g_addr[i];
    int active_pane = (i == g_active);
    if (g_addr_edit == i) {
        f->bg = UTHEME_WHITE; f->fg = UTHEME_TEXT; f->border = UTHEME_ACCENT;
    } else if (active_pane) {
        f->bg = UTHEME_ACCENT; f->fg = UTHEME_ACCENT_TEXT; f->border = UTHEME_ACCENT;
    } else {
        f->bg = UTHEME_PANEL_BG; f->fg = UTHEME_TEXT; f->border = UTHEME_BORDER;
    }
    // Re-synced from the pane while nobody is typing in it, so a
    // navigation by any other route (Enter, Backspace, the tree) shows
    // up here without the field having to be told.
    if (g_addr_edit != i && strcmp(uui_textbox_text(f), uui_fileview_dir(&g_pane[i])) != 0)
        uui_textbox_init(f, uui_fileview_dir(&g_pane[i]));
}

// What a pane or the side column must keep however hard the divider is
// dragged. Font-derived: eight columns is about the least in which a
// filename is still a filename, not a pixel count that stops meaning
// anything at another font size.
static int min_col_w(void) { return ugfx_char_w() * 8; }

void layout_all(int cw, int ch) {
    int nh = navrow_h(), tb = toolbar_h(), sb = statusbar_h();

    // The context menu and the drop-downs have no strip of their own;
    // only their popup bounds matter, and they are the whole window.
    uui_menubar_set_geometry(&g_ctx, 0, 0, 0, 0);
    uui_menubar_set_bounds(&g_ctx, 0, 0, cw, ch);
    uui_dialog_set_bounds(&g_dialog, 0, 0, cw, ch);

    // --- the top row: nav buttons, breadcrumb, search ------------------
    int nw, nth;
    uui_toolbar_natural_size(&g_nav, &nw, &nth);
    uui_toolbar_ops.set_geometry(&g_nav, 0, (nh - nth) / 2, nw, nth);
    g_nav.bg = g_nav.border = UTHEME_PANEL_BG;   // part of the row, not a strip of its own
    int fh, sw = ugfx_char_advance('n') * 26;
    uui_pathbar_ops.natural_size(&g_path, 0, &fh);
    if (sw > cw / 3) sw = cw / 3;
    uui_textbox_set_geometry(&g_search, cw - sw - 6, (nh - fh) / 2, sw, fh);
    int px = nw + 4, pw = cw - sw - 12 - px;
    uui_pathbar_ops.set_geometry(&g_path, px, (nh - fh) / 2, pw > 40 ? pw : 40, fh);
    if (g_search_on) { g_search.border = UTHEME_ACCENT; } else { g_search.border = UTHEME_BORDER; }

    // --- the command bar ------------------------------------------------
    uui_toolbar_ops.set_geometry(&g_toolbar, 0, nh, cw, tb);

    // --- the status bar, with the view switch and Cancel at its end ----
    int vw, vh;
    uui_toolbar_natural_size(&g_viewbar, &vw, &vh);
    if (vh > sb) vh = sb;
    // Clear of the window's resize grip, which owns the corner.
    int grip = ugfx_char_h();
    uui_toolbar_ops.set_geometry(&g_viewbar, cw - vw - grip, ch - sb + (sb - vh) / 2, vw, vh);
    g_viewbar.bg = UUI_COLOR(g_status.bg, UTHEME_BAR_BG);   // on the bar, in its colour
    int status_w = cw - vw - grip;
    // CANCEL SITS ON THE STATUS BAR, left of the view switch, and exists
    // only while an operation runs -- see g_cancel_btn's own note.
    int running = fm_job_running();
    widget_by_id(ID_CANCEL)->hidden = !running;
    if (running) {
        int bw, bh;
        uui_button_natural_size(&g_cancel_btn, &bw, &bh);
        if (bh > sb) bh = sb;
        uui_button_set_geometry(&g_cancel_btn, status_w - bw - 2, ch - sb + (sb - bh) / 2,
                                 bw, bh);
        status_w -= bw + 4;
    }
    // The bar runs the full width under the switch; its text stops short.
    uui_statusbar_set_geometry(&g_status, 0, ch - sb, cw, sb);
    (void)status_w;

    // --- the body: side column | panes | details pane -------------------
    int top = nh + tb;
    int body_h = ch - top - sb;
    int split_w = uui_splitter_thickness();
    int minw = min_col_w();

    int dw = g_dpane ? details_width(cw) : 0;
    int right_edge = cw - dw;
    widget_by_id(ID_DP_OPEN)->hidden = !g_dpane;
    widget_by_id(ID_DP_PROPS)->hidden = !g_dpane;
    if (g_dpane) details_layout(right_edge, top, dw, body_h);

    // THE SIDE COLUMN is always there -- the places, with the folder tree
    // under them when it is on -- and its divider is the old tree one.
    uui_splitter_set_track(&g_tree_split, 0, right_edge, minw,
                            (g_single ? 1 : 2) * minw + split_w);
    int side_w = uui_splitter_before(&g_tree_split);
    int places_h;
    uui_places_ops.natural_size(&g_places, 0, &places_h);
    if (!g_tree_on || places_h > body_h) places_h = body_h;
    uui_places_ops.set_geometry(&g_places, 0, top, side_w, places_h);
    if (g_tree_on)
        uui_tree_ops.set_geometry(&g_tree, 0, top + places_h, side_w, body_h - places_h);
    uui_splitter_set_geometry(&g_tree_split, uui_splitter_pos(&g_tree_split),
                               top, split_w, body_h);
    int tx = side_w + split_w;
    int pw2 = right_edge - tx;

    int hdr = panehdr_h();
    int panes_y = top + hdr;
    int panes_h = body_h - hdr;
    if (panes_h < 1) panes_h = 1;

    if (g_single) {
        // Both panes get the full rect; only the active one is SHOWN.
        // The hidden one keeps sane geometry so nothing draws from junk
        // the frame it comes back.
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y, pw2, panes_h);
        uui_fileview_set_geometry(&g_pane[1], tx, panes_y, pw2, panes_h);
    } else {
        uui_splitter_set_track(&g_pane_split, tx, right_edge, minw, minw);
        int sx = uui_splitter_pos(&g_pane_split);
        uui_fileview_set_geometry(&g_pane[0], tx, panes_y,
                                   uui_splitter_before(&g_pane_split), panes_h);
        uui_splitter_set_geometry(&g_pane_split, sx, top, split_w, body_h);
        uui_fileview_set_geometry(&g_pane[1], sx + split_w, panes_y,
                                   right_edge - sx - split_w, panes_h);
    }

    // The address bars sit in the strip above each pane, same width --
    // split view only.
    for (int i = 0; i < 2; i++) {
        int bx, by, bw2, bh2;
        uui_fileview_ops.bounds(&g_pane[i], &bx, &by, &bw2, &bh2);
        uui_textbox_set_geometry(&g_addr[i], bx, by - hdr, bw2, hdr);
        addr_style(i);
    }

    // Visibility is decided beside the geometry: the router skips a
    // hidden item, so a hidden pane cannot be clicked either.
    widget_by_id(ID_LEFT)->hidden  = g_single && g_active != 0;
    widget_by_id(ID_RIGHT)->hidden = g_single && g_active != 1;
    widget_by_id(ID_ADDR_L)->hidden = g_single || widget_by_id(ID_LEFT)->hidden;
    widget_by_id(ID_ADDR_R)->hidden = g_single || widget_by_id(ID_RIGHT)->hidden;
    widget_by_id(ID_TREE)->hidden = !g_tree_on;
    widget_by_id(ID_PANE_SPLIT)->hidden = g_single;

    // The ACTIVE pane's outline, drawn by the widget itself so it stays
    // under a menu popup -- see uui_fileview.h's active_mark. Split view
    // only: one pane is the active one without being told.
    for (int i = 0; i < 2; i++) {
        uui_fileview_set_active_mark(&g_pane[i], !g_single && i == g_active, UTHEME_ACCENT);
        // The cursor row wears a focus ring in the active pane. Marks
        // and the cursor share one background now (ui/uui_fileview.h's
        // mark_bg), so the ring is what says which row the keys are on
        // -- and this app has no focus ring of its own to set it.
        g_pane[i].table.focused = (i == g_active);
        // Rows with room to breathe, Explorer's details view: the icon
        // plus a few pixels either side.
        g_pane[i].table.row_h = ugfx_char_h() + 8;
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
        // THE SCROLL OFFSET, because a test that cannot see it cannot
        // tell "the view stayed put" from "the view moved and came
        // back". Both modes: icons scroll by PIXEL, details by table
        // row, and only one of the two is meaningful at a time.
        uapp_logf_layout("files: layout scroll %d %d %d\n", i,
                         g_pane[i].icon_scroll, g_pane[i].table.top);
    }
    const char *sel = uui_fileview_selected_name(active());
    uapp_logf_layout("files: layout active %d\n", g_active);
    // EARLY in the block, before the tree's per-row lines: an overflow
    // drops what comes LAST, and these are what a drag test reads.
    // A drag in flight: its size and copy bit, and each pane's drop
    // row (-2 none, -1 the pane, else the row) plus the tree node
    // under it as a PATH ("-" for none), so a test names its target.
    {
        const struct uui_drag *dg = uapp_drag(g_app);
        int live = dg && dg->kind == UUI_DRAG_FILES && uapp_drag_active(g_app);
        int tn = g_tree.drop_node;
        // ...with the pointer, so every motion of a drag is a NEW block:
        // the dedupe would otherwise emit nothing while the pointer
        // moves within one target, and a test confirming "the pointer
        // is over the row for /x" would read no frame at all.
        uapp_logf_layout("files: layout drag %d %d %d %d %d\n", live,
                          live ? dg->count : 0, live ? dg->copy : 0,
                          live ? dg->x : -1, live ? dg->y : -1);
        uapp_logf_layout("files: layout drop %d %d %s\n", g_pane[0].drop_row,
                          g_pane[1].drop_row,
                          tn >= 0 && tn < g_tree_count ? g_tree_path[tn] : "-");
    }
    // THE ROW HEIGHT, because a test aiming at "row 2" otherwise
    // guesses it -- and a guess that is a few pixels out lands on empty
    // space below the last row, which selects nothing and reads exactly
    // like a broken click.
    uapp_logf_layout("files: layout rowh %d\n", uui_table_row_h(&g_pane[0].table));
    uui_toolbar_ops.bounds(&g_toolbar, &x, &y, &w, &h);
    uapp_logf_layout("files: layout toolbar %d %d %d %d\n", x, y, w, h);
    for (int i = 0; uui_toolbar_item_rect(&g_toolbar, i, &x, &y, &w, &h); i++)
        uapp_logf_layout("files: layout tbitem %d %d %d %d %d\n", i, x, y, w, h);
    for (int i = 0; uui_toolbar_item_rect(&g_nav, i, &x, &y, &w, &h); i++)
        uapp_logf_layout("files: layout navitem %d %d %d %d %d\n", i, x, y, w, h);
    for (int i = 0; uui_toolbar_item_rect(&g_viewbar, i, &x, &y, &w, &h); i++)
        uapp_logf_layout("files: layout vbitem %d %d %d %d %d\n", i, x, y, w, h);
    // The popup's rect is the menu's own `menu.popup 0` line now.
    // Depth and the top popup's hot row: the one logged fact that CHANGES
    // as the pointer crosses an open menu. Without it a hover test's
    // frames are identical, the dedup drops them, and "nothing arrived"
    // reads as a wedge.
    uapp_logf_layout("files: layout menuhot %d %d\n", g_ctx.depth,
          g_ctx.depth > 0 ? g_ctx.level[0].hot : -1);
    // The details pane, and the facts it shows: its own report, since
    // nothing else on screen says which file it is describing.
    uapp_logf_layout("files: layout dpane %d %s\n", g_dpane,
                      uui_fileview_selected_name(active()) ? uui_fileview_selected_name(active()) : "-");
    uapp_logf_layout("files: layout status %s | %s\n", g_stat_dir, g_stat_items);
    // Whether the search box has the keyboard: a test types only once it
    // does, since a key can overtake the click that gave it focus.
    uapp_logf_layout("files: layout searching %d\n", g_search_on);
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
        // The selected node as a PATH, and the visible-row count -- a
        // test asserting "the tree followed" needs the path, and "it
        // expanded" needs the rows.
        int tid = uui_tree_selected_id(&g_tree);
        uapp_logf_layout("files: layout treesel %s %d\n",
              tid >= 0 && tid < g_tree_count ? g_tree_path[tid] : "-",
              uui_tree_visible_count(&g_tree));
        // Each VISIBLE row's path, so a test can aim at "the row for
        // /x" rather than hover down the rows reading hits back.
        int vis = uui_tree_visible_rows(&g_tree);
        for (int r = 0; r < vis; r++) {
            int node = uui_tree_node_at_row(&g_tree, g_tree.top + r);
            if (node < 0 || node >= g_tree_count) break;
            uapp_logf_layout("files: layout treerow %d %s\n", r, g_tree_path[node]);
        }
    }
    uapp_logf_layout("files: layout selected %s\n", sel ? sel : "-");
    uapp_logf_layout("files: layout modal %d\n", (int)g_modal);
    // An in-place rename, per pane, and what its field holds.
    uapp_logf_layout("files: layout renaming %d %d %s\n", g_pane[0].renaming, g_pane[1].renaming,
                     active()->renaming ? uui_textbox_text(&active()->rename_box) : "-");
    // The status bar's note, which is where a refusal is said.
    uapp_logf_layout("files: layout note %s\n", g_stat_note);
    // Which address bar is being edited, -1 for none -- and its text,
    // since "the field holds what was typed" is what a test asserts.
    uapp_logf_layout("files: layout addr %d %s\n", g_addr_edit,
                      g_addr_edit >= 0 ? uui_textbox_text(&g_addr[g_addr_edit]) : "-");
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
    // ...and its row count as ONE line: the popup's own per-row lines
    // are what overflow the layout block, and a count is the fact a
    // test wants ("Edit in Notepad is there, or is not").
    uapp_logf_layout("files: layout ctx %d %d\n", uui_menubar_is_open(&g_ctx),
                      uui_menubar_is_open(&g_ctx) ? g_ctx_rows : 0);
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
    uui_statusbar_draw(d->surface, &g_status);
    if (g_dpane) details_draw(d->surface);
    if (!widget_by_id(ID_CANCEL)->hidden) uui_button_draw_one(d->surface, &g_cancel_btn);
    log_layout();
    // AFTER the app's own report: tools/filemanager_test.py takes `pane 0`
    // as the start of a frame, so the widgets' lines must follow it.
    uapp_log_layout(a, "files");   // the widgets by name (ui/uui_describe.h)
}

void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a; (void)d;
    // Nothing: the prompts are uui_dialog now, drawn by the router, and
    // anything drawn here would sit on top of an open menu.
}

// Which VISIBLE pane holds this point, or -1. The pane's RECT, so a
// right-click on the empty space below the rows still gets a menu
// (Paste and New folder need no row) -- and not uui_fileview_hit(),
// which answers a ROW INDEX with -1 for a miss: tested as a boolean, a
// miss on the left pane read as a hit and every right-click in the
// right pane acted on the left one (the CLAUDE.md `hit` trap).
int pane_at(int x, int y) {
    for (int i = 0; i < 2; i++) {
        if (widget_by_id(i ? ID_RIGHT : ID_LEFT)->hidden) continue;
        int px, py, pw, ph;
        uui_fileview_ops.bounds(&g_pane[i], &px, &py, &pw, &ph);
        if (uui_hit(px, py, pw, ph, x, y)) return i;
    }
    return -1;
}
