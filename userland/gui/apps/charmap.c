// Character Map -- every character the installed fonts can draw, and
// the fonts themselves. GNOME Characters and GNOME Fonts in one window,
// as Windows' charmap is one: chosen from mockups on 2026-10-09 (C3), a
// RAIL of two pages.
//
//   Characters  search (a name, U+XXXX, or the character itself), the
//               font as chips, a Unicode block, the GRID of glyphs, a
//               detail strip for the selected one (its name, code point,
//               UTF-8, block, which fonts have it) and a line of text to
//               copy that a double-click adds to -- charmap's "Characters
//               to copy".
//   Fonts       a card per family and the chosen one's page: a sample at
//               a ladder of sizes, its files, coverage and licence, and
//               "Use for the interface" / "for terminals".
//
// The glyphs are rasterized from the font FILES by ui/uglyph.h, since
// the session font holds ASCII and Latin-1 only; the names and blocks
// are lib/uunicode.h's, generated for exactly what these fonts map.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "rt/sys.h"
#include "kpath.h"
#include "lib/uclip.h"
#include "lib/usetting.h"
#include "lib/uunicode.h"
#include "ui/uapp.h"
#include "ui/uglyph.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui.h"
#include "ui/uui_button.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_focus.h"
#include "ui/uui_grid.h"
#include "ui/uui_primitives.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_textbox.h"
#include "keyboard.h"

#define FONT_DIR   "/usr/share/fonts"
#define MAX_FACES  8
#define MAX_CPS    4096
#define MAX_TEXT   64
#define SAMPLE     "The quick brown fox jumps over the lazy dog"

enum { PAGE_CHARS, PAGE_FONTS };
enum {
    ID_RAIL = 1, ID_SEARCH, ID_FONT, ID_FONTLIST, ID_BLOCK, ID_GRID,
    ID_COPY, ID_ADD, ID_COPY_ALL, ID_CLEAR, ID_CARDS, ID_SAMPLE, ID_USE_UI, ID_USE_MONO,
};

struct face {
    char stem[48];              // the file's name without .ttf: what the settings name
    char family[48];
    struct uglyph_face reg, bold;
    int has_bold, mono, named;  // `named`: characters with a Unicode name it maps
    char licence[48];           // the LICENSE-*.txt beside it, or empty
    unsigned long bytes;
};

static struct uapp *g_app;
static struct face g_faces[MAX_FACES];
static int g_nfaces, g_cur;
static int g_page = PAGE_CHARS;
static const char *g_font_names[MAX_FACES];

// Characters: the grid's contents, the block list, the text to copy.
static uint32_t g_cps[MAX_CPS];
static int g_ncps;
static int g_block_ids[64];
static const char *g_block_names[64];
static int g_nblocks;
static int g_shown_block = -1;     // the block fill() last listed
static uint32_t g_text[MAX_TEXT];
static int g_ntext;
static char g_note[96];            // the status bar's message
static char g_status_font[64];

static struct uui_sidebar_row g_rail_rows[2];
static struct uui_sidebar g_rail;
static struct uui_textbox g_search, g_sample;
static struct uui_segmented g_chips;
static struct uui_dropdown g_fontlist, g_block;
static struct uui_grid g_grid, g_cards;
static struct uui_button g_copy, g_add, g_copy_all, g_clear, g_use_ui, g_use_mono;
static struct uui_statusbar g_status;
static struct uui_focus g_focus;
static struct uui_focusable g_ring[4];
enum { RING_RAIL, RING_SEARCH, RING_BLOCK, RING_GRID };   // the Characters page's tab order

// Geometry the drawing needs.
static int g_rail_w, g_top_h, g_detail_y, g_detail_h, g_text_y, g_text_h;
static int g_page_x;

// --- fonts -----------------------------------------------------------------

static int by_family(const void *a, const void *b) {
    return strcmp(((const struct face *)a)->family, ((const struct face *)b)->family);
}

static void load_faces(void) {
    static struct sys_dirent e[64];
    int n = sys_listdir(FONT_DIR, e, 64);
    for (int i = 0; i < n && g_nfaces < MAX_FACES; i++) {
        size_t len = strlen(e[i].name);
        if (len < 5 || strcmp(e[i].name + len - 4, ".ttf")) continue;
        if (len > 9 && !strcmp(e[i].name + len - 9, "-bold.ttf")) continue;   // a weight, not a family
        struct face *f = &g_faces[g_nfaces];
        memset(f, 0, sizeof *f);
        snprintf(f->stem, sizeof f->stem, "%.*s", (int)(len - 4), e[i].name);
        char path[128];
        snprintf(path, sizeof path, "%s/%s", FONT_DIR, e[i].name);
        if (!uglyph_open(&f->reg, path)) continue;
        f->bytes = (unsigned long)f->reg.len;
        snprintf(path, sizeof path, "%s/%s-bold.ttf", FONT_DIR, f->stem);
        f->has_bold = uglyph_open(&f->bold, path);
        if (f->has_bold) f->bytes += (unsigned long)f->bold.len;
        if (!uglyph_family(&f->reg, f->family, sizeof f->family))
            snprintf(f->family, sizeof f->family, "%s", f->stem);
        f->mono = uglyph_text_width(&f->reg, "i", 20) == uglyph_text_width(&f->reg, "W", 20);
        for (int k = 0; k < uunicode_named_count(); k++)
            f->named += uglyph_has(&f->reg, uunicode_named(k));
        // The licence its stem names up to the first dash:
        // dejavu-sans-mono -> LICENSE-DejaVu.txt, vera-mono -> LICENSE-Vera.txt.
        size_t w = strcspn(f->stem, "-");
        for (int k = 0; k < n; k++)
            if (!strncmp(e[k].name, "LICENSE-", 8) && !strncasecmp(e[k].name + 8, f->stem, w) &&
                !strcmp(e[k].name + 8 + w, ".txt"))
                snprintf(f->licence, sizeof f->licence, "%s", e[k].name);
        g_nfaces++;
    }
    qsort(g_faces, (size_t)g_nfaces, sizeof g_faces[0], by_family);
    for (int i = 0; i < g_nfaces; i++) g_font_names[i] = g_faces[i].family;
    ulogf("charmap: %d font famil%s, %d names\n", g_nfaces, g_nfaces == 1 ? "y" : "ies", uunicode_named_count());
}

// The faces the desktop uses now, for the cards' "interface"/"terminals".
static char g_ui_face[48], g_term_face[48];

static void read_usage(void) {
    if (!usetting_get("system.font_face", g_ui_face, sizeof g_ui_face)) g_ui_face[0] = '\0';
    if (!usetting_get("system.font_mono", g_term_face, sizeof g_term_face)) g_term_face[0] = '\0';
}

static struct face *cur(void) { return g_nfaces ? &g_faces[g_cur] : 0; }

// --- the grid's contents ------------------------------------------------------

// The blocks the current font has anything in, for the dropdown.
static void list_blocks(void) {
    int was = uui_dropdown_selected(&g_block);
    int keep_id = was >= 0 && was < g_nblocks ? g_block_ids[was] : -1;
    g_nblocks = 0;
    struct face *f = cur();
    if (!f) return;
    for (int b = 0; b < uunicode_block_count() && g_nblocks < 64; b++) {
        const struct uunicode_block *bl = uunicode_block(b);
        int any = 0;
        for (int k = 0; k < uunicode_named_count() && !any; k++) {
            uint32_t cp = uunicode_named(k);
            any = cp >= bl->lo && cp <= bl->hi && uglyph_has(&f->reg, cp);
        }
        if (!any) continue;
        g_block_ids[g_nblocks] = b;
        g_block_names[g_nblocks] = bl->name;
        g_nblocks++;
    }
    // The same BLOCK if the new font has it, not the same row.
    int sel = 0;
    for (int i = 0; i < g_nblocks; i++) if (g_block_ids[i] == keep_id) sel = i;
    uui_dropdown_set_items(&g_block, g_block_names, g_nblocks);
    uui_dropdown_set_selected(&g_block, sel);
}

// The grid shows the search's matches while there is a query, else the
// block's characters -- each only if the current font draws it.
static void cell(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state);

static void fill(void) {
    struct face *f = cur();
    uint32_t keep = g_grid.selected >= 0 && g_grid.selected < g_ncps ? g_cps[g_grid.selected] : 0;
    g_ncps = 0;
    const char *q = uui_textbox_text(&g_search);
    if (f && q[0]) {
        static uint32_t found[MAX_CPS];
        int n = uunicode_search(q, found, MAX_CPS);
        g_shown_block = -1;
        if (n > MAX_CPS) n = MAX_CPS;
        for (int i = 0; i < n; i++)
            if (uglyph_has(&f->reg, found[i])) g_cps[g_ncps++] = found[i];
        snprintf(g_note, sizeof g_note, "%d match%s in %s", g_ncps, g_ncps == 1 ? "" : "es", f->family);
    } else if (f && g_nblocks) {
        int b = uui_dropdown_selected(&g_block);
        g_shown_block = b;
        const struct uunicode_block *bl = uunicode_block(g_block_ids[b < 0 ? 0 : b]);
        for (int k = 0; k < uunicode_named_count() && g_ncps < MAX_CPS; k++) {
            uint32_t cp = uunicode_named(k);
            if (cp >= bl->lo && cp <= bl->hi && uglyph_has(&f->reg, cp)) g_cps[g_ncps++] = cp;
        }
        snprintf(g_note, sizeof g_note, "%s, %d characters", bl->name, g_ncps);
    }
    uui_grid_set(&g_grid, g_ncps, cell, 0);
    // The one still listed stays selected -- unless a name IS the query,
    // which wins: "rightwards arrow" means U+2192, not U+0362 COMBINING
    // DOUBLE RIGHTWARDS ARROW BELOW, which sorts first.
    int sel = 0, exact = -1;
    for (int i = 0; i < g_ncps; i++) {
        if (g_cps[i] == keep) sel = i;
        const char *nm = uunicode_name(g_cps[i]);
        if (q[0] && exact < 0 && nm && !strcasecmp(nm, q)) exact = i;
    }
    if (exact >= 0) sel = exact;
    if (q[0]) ulogf("charmap: search \"%s\" %d\n", q, g_ncps);
    uui_grid_select(&g_grid, g_ncps ? sel : -1);
}

// A space has no ink, so it is shown by what it is called: a dashed box
// round a short label, the way GNOME Characters and Word mark one.
static void blank_mark(struct ugfx_surface *s, struct uglyph_face *face, uint32_t cp, int x, int y, int w, int h,
                       uint32_t ink) {
    const char *label = cp == 0x20 ? "SP" : cp == 0xA0 ? "NBSP" : cp == 0xAD ? "SHY" : "";
    char hex[8];
    if (!label[0]) { snprintf(hex, sizeof hex, "%04X", (unsigned)cp); label = hex; }
    int bw = w * 3 / 4, bh = h / 2, bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
    for (int i = 0; i < bw; i += 4) {
        ugfx_fill_rect(s, bx + i, by, 2, 1, ink);
        ugfx_fill_rect(s, bx + i, by + bh - 1, 2, 1, ink);
    }
    for (int i = 0; i < bh; i += 4) {
        ugfx_fill_rect(s, bx, by + i, 1, 2, ink);
        ugfx_fill_rect(s, bx + bw - 1, by + i, 1, 2, ink);
    }
    int px = h / 4, tw = uglyph_text_width(face, label, px);
    if (tw > bw - 4) px = px * (bw - 4) / tw, tw = uglyph_text_width(face, label, px);
    uglyph_text(s, face, label, px, x + (w - tw) / 2, y + h / 2 + uglyph_ascent(face, px) / 2 - 1, ink);
}

// A block chosen: its characters from the top, the search cleared.
static void show_block(void) {
    if (g_block.open || uui_dropdown_selected(&g_block) == g_shown_block) return;
    uui_textbox_set_text(&g_search, "");
    fill();
    g_grid.scroll = 0;
    uui_grid_select(&g_grid, g_ncps ? 0 : -1);
}

static void cell(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx;
    struct face *f = cur();
    if (!f || index < 0 || index >= g_ncps) return;
    uint32_t ink = (state & UUI_GRID_SELECTED) ? UTHEME_ACCENT_TEXT : UTHEME_TEXT;
    if (uglyph_draw(s, &f->reg, g_cps[index], h * 11 / 20, x, y, w, h, ink) == UGLYPH_NO_INK)
        blank_mark(s, &f->reg, g_cps[index], x, y, w, h, ink);
}

static uint32_t selected_cp(void) {
    return g_grid.selected >= 0 && g_grid.selected < g_ncps ? g_cps[g_grid.selected] : 0;
}

// Text here is Latin-1 (docs/decisions/drivers.md), so characters that
// all fit are copied as Latin-1 and paste as themselves anywhere; one
// past U+00FF makes the whole copy UTF-8, the only encoding that can
// carry it -- X11's STRING beside UTF8_STRING, as one entry.
static void copy_text(const uint32_t *cps, int n, const char *what) {
    char buf[MAX_TEXT * 4 + 1];
    int o = 0, wide = 0;
    for (int i = 0; i < n; i++) wide |= cps[i] > 0xFF;
    for (int i = 0; i < n; i++) {
        char u[5];
        int k = wide ? uunicode_utf8(cps[i], u) : 1;
        if (!wide) u[0] = (char)cps[i];
        memcpy(buf + o, u, (size_t)k);
        o += k;
    }
    buf[o] = '\0';
    if (uclip_set_text(buf, o)) snprintf(g_note, sizeof g_note, "Copied %s%s", what, wide ? " as UTF-8" : "");
    else snprintf(g_note, sizeof g_note, "Could not copy");
    ulogf("charmap: copied %d character(s), %d bytes, %s\n", n, o, wide ? "UTF-8" : "Latin-1");
}

static void add_selected(void) {
    uint32_t cp = selected_cp();
    if (!cp || g_ntext >= MAX_TEXT) return;
    g_text[g_ntext++] = cp;
    ulogf("charmap: text %d character(s)\n", g_ntext);
}

// --- pages -------------------------------------------------------------------

static struct uui_item g_chars_items[] = {
    { .ops = &uui_sidebar_ops,   .widget = &g_rail,     .id = ID_RAIL,     .name = "rail" },
    { .ops = &uui_textbox_ops,   .widget = &g_search,   .id = ID_SEARCH,   .name = "search" },
    { .ops = &uui_segmented_ops, .widget = &g_chips,    .id = ID_FONT,     .name = "fonts" },
    { .ops = &uui_dropdown_ops,  .widget = &g_fontlist, .id = ID_FONTLIST, .name = "fontlist" },
    { .ops = &uui_grid_ops,      .widget = &g_grid,     .id = ID_GRID,     .name = "grid" },
    { .ops = &uui_button_ops,    .widget = &g_copy,     .id = ID_COPY,     .name = "copy" },
    { .ops = &uui_button_ops,    .widget = &g_add,      .id = ID_ADD,      .name = "add" },
    { .ops = &uui_button_ops,    .widget = &g_copy_all, .id = ID_COPY_ALL, .name = "copyall" },
    { .ops = &uui_button_ops,    .widget = &g_clear,    .id = ID_CLEAR,    .name = "clear" },
    // Last: its popup paints over the grid.
    { .ops = &uui_dropdown_ops,  .widget = &g_block,    .id = ID_BLOCK,    .name = "block" },
};
#define W_CHIPS    2
#define W_FONTLIST 3
#define W_USE_MONO 4

static struct uui_item g_fonts_items[] = {
    { .ops = &uui_sidebar_ops, .widget = &g_rail,     .id = ID_RAIL,     .name = "rail" },
    { .ops = &uui_grid_ops,    .widget = &g_cards,    .id = ID_CARDS,    .name = "cards" },
    { .ops = &uui_textbox_ops, .widget = &g_sample,   .id = ID_SAMPLE,   .name = "sample" },
    { .ops = &uui_button_ops,  .widget = &g_use_ui,   .id = ID_USE_UI,   .name = "useui" },
    { .ops = &uui_button_ops,  .widget = &g_use_mono, .id = ID_USE_MONO, .name = "usemono" },
};

static void show_page(struct uapp *a, int page) {
    g_page = page;
    if (page == PAGE_CHARS) {
        uapp_set_widgets(a, g_chars_items, (int)(sizeof g_chars_items / sizeof g_chars_items[0]));
        g_ring[RING_RAIL] = (struct uui_focusable){ &g_rail, &uui_sidebar_ops };
        g_ring[RING_SEARCH] = (struct uui_focusable){ &g_search, &uui_textbox_focus_ops };
        g_ring[RING_BLOCK] = (struct uui_focusable){ &g_block, &uui_dropdown_focus_ops };
        g_ring[RING_GRID] = (struct uui_focusable){ &g_grid, &uui_grid_ops };
        uui_focus_init(&g_focus, g_ring, 4);
        uui_focus_set(&g_focus, RING_GRID);
        if (g_nfaces) fill();
    } else {
        uapp_set_widgets(a, g_fonts_items, (int)(sizeof g_fonts_items / sizeof g_fonts_items[0]));
        g_ring[0] = (struct uui_focusable){ &g_rail, &uui_sidebar_ops };
        g_ring[1] = (struct uui_focusable){ &g_cards, &uui_grid_ops };
        g_ring[2] = (struct uui_focusable){ &g_sample, &uui_textbox_focus_ops };
        uui_focus_init(&g_focus, g_ring, 3);
        uui_focus_set(&g_focus, 1);
        uui_grid_select(&g_cards, g_cur);
        read_usage();
        snprintf(g_note, sizeof g_note, "%d font famil%s in %s", g_nfaces, g_nfaces == 1 ? "y" : "ies", FONT_DIR);
    }
    uui_sidebar_select_id(&g_rail, page);
    ulogf("charmap: page %s\n", page == PAGE_CHARS ? "characters" : "fonts");
}

static void choose_font(int i) {
    if (i < 0 || i >= g_nfaces) return;
    g_cur = i;
    g_chips.selected = i;
    uui_dropdown_set_selected(&g_fontlist, i);
    list_blocks();
    fill();
    snprintf(g_status_font, sizeof g_status_font, "%s", g_faces[i].family);
    ulogf("charmap: font %s, %d named characters\n", g_faces[i].family, g_faces[i].named);
}

// --- layout ------------------------------------------------------------------

static void layout(int cw, int ch) {
    int lh = ugfx_char_h(), pad = ugfx_char_w();
    int bh = lh + pad, sbh = uui_statusbar_height(&g_status);
    uui_statusbar_set_geometry(&g_status, 0, ch - sbh, cw, sbh);
    ch -= sbh;
    g_rail_w = pad * 11;
    uui_sidebar_set_geometry(&g_rail, 0, 0, g_rail_w, ch);
    int x = g_rail_w + 1, w = cw - x;
    g_page_x = x;
    if (g_page == PAGE_CHARS) {
        g_top_h = bh + pad * 2;
        uui_textbox_set_geometry(&g_search, x + pad, pad, pad * 22, bh);
        int fx = x + pad * 24, bw = pad * 20, sw, sh;
        uui_segmented_ops.natural_size(&g_chips, &sw, &sh);
        // Chips while they fit beside the Block list, else a dropdown.
        int chips = fx + sw + pad * 2 + ugfx_text_width("Block") + pad + bw <= cw - pad;
        g_chars_items[W_CHIPS].hidden = !chips;
        g_chars_items[W_FONTLIST].hidden = chips;
        if (chips) {
            uui_segmented_ops.set_geometry(&g_chips, fx, pad + (bh - sh) / 2, sw, sh);
        } else {
            int room = cw - pad - bw - pad - ugfx_text_width("Block") - pad * 2 - fx;
            uui_dropdown_set_geometry(&g_fontlist, fx, pad, room < pad * 22 ? room : pad * 22, bh);
        }
        uui_dropdown_set_geometry(&g_block, cw - pad - bw, pad, bw, bh);
        g_text_h = bh + pad * 2;
        g_text_y = ch - g_text_h;
        g_detail_h = lh * 4 + pad;
        g_detail_y = g_text_y - g_detail_h;
        g_grid.cell_w = g_grid.cell_h = lh * 5 / 2;
        uui_grid_ops.set_geometry(&g_grid, x, g_top_h, w, g_detail_y - g_top_h);
        int b1 = pad * 10, b2 = pad * 13;
        uui_button_set_geometry(&g_add, cw - pad - b2, g_detail_y + (g_detail_h - bh) / 2, b2, bh);
        uui_button_set_geometry(&g_copy, cw - pad * 2 - b2 - b1, g_detail_y + (g_detail_h - bh) / 2, b1, bh);
        uui_button_set_geometry(&g_clear, cw - pad - b1, g_text_y + pad, b1, bh);
        uui_button_set_geometry(&g_copy_all, cw - pad * 2 - b1 * 2, g_text_y + pad, b1, bh);
    } else {
        int listw = pad * 20, rx = x + listw + pad * 2, rw = cw - rx - pad * 2;
        // Measured here, not at start-up: the session font is loaded by then.
        g_cards.cell_w = listw - uui_scrollbar_overlay_width();
        g_cards.cell_h = lh * 3;
        uui_grid_ops.set_geometry(&g_cards, x, 0, listw, ch);
        uui_textbox_set_geometry(&g_sample, rx, pad, rw, bh);
        int by = ch - bh - pad;
        int b1 = ugfx_text_width(g_use_ui.label) + pad * 4, b2 = ugfx_text_width(g_use_mono.label) + pad * 4;
        uui_button_set_geometry(&g_use_ui, rx, by, b1, bh);
        uui_button_set_geometry(&g_use_mono, rx + b1 + pad, by, b2, bh);
        // Terminals need a fixed cell; a proportional face is not offered.
        g_fonts_items[W_USE_MONO].hidden = !(cur() && cur()->mono);
    }
}

// --- drawing -----------------------------------------------------------------

static void draw_detail(struct ugfx_surface *s, int cw) {
    int lh = ugfx_char_h(), pad = ugfx_char_w();
    uint32_t panel = UTHEME_PANEL_BG, dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    ugfx_fill_rect(s, g_page_x, g_detail_y, cw - g_page_x, g_detail_h, panel);
    ugfx_fill_rect(s, g_page_x, g_detail_y, cw - g_page_x, 1, UTHEME_SEPARATOR);
    uint32_t cp = selected_cp();
    struct face *f = cur();
    if (!cp || !f) return;
    int tile = g_detail_h - pad * 2, tx = g_page_x + pad * 2, ty = g_detail_y + pad;
    uapp_logf_layout("charmap: layout tile %d %d %d %d\n", tx, ty, tile, tile);
    uint32_t dark = ugfx_rgb(29, 36, 51);
    uui_fill_round_rect(s, tx, ty, tile, tile, 8, dark);
    if (uglyph_draw(s, &f->reg, cp, tile * 2 / 3, tx, ty, tile, tile, ugfx_rgb(255, 255, 255)) == UGLYPH_NO_INK)
        blank_mark(s, &f->reg, cp, tx, ty, tile, tile, ugfx_rgb(255, 255, 255));
    int x = tx + tile + pad * 2, room = g_copy.x - x - pad * 2;
    const char *name = uunicode_name(cp);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, x, ty, room, name ? name : "(no name)", UTHEME_TEXT, panel);
    ugfx_set_font(was);
    char u[5], hex[16] = "", line[160];
    int n = uunicode_utf8(cp, u);
    for (int i = 0; i < n; i++) snprintf(hex + i * 3, sizeof hex - (size_t)i * 3, "%02X ", (unsigned char)u[i]);
    int b = uunicode_block_of(cp);
    snprintf(line, sizeof line, "U+%04X    UTF-8 %s   %s", (unsigned)cp, hex, b >= 0 ? uunicode_block(b)->name : "");
    ugfx_draw_string_clipped(s, x, ty + lh + pad / 2, room, line, dim, panel);
    char in[160] = "In ";
    int first = 1;
    for (int i = 0; i < g_nfaces; i++)
        if (uglyph_has(&g_faces[i].reg, cp)) {
            size_t l = strlen(in);
            snprintf(in + l, sizeof in - l, "%s%s", first ? "" : ", ", g_faces[i].family);
            first = 0;
        }
    ugfx_draw_string_clipped(s, x, ty + lh * 2 + pad, room, in, dim, panel);
}

static void draw_text_line(struct ugfx_surface *s, int cw) {
    int lh = ugfx_char_h(), pad = ugfx_char_w();
    ugfx_fill_rect(s, g_page_x, g_text_y, cw - g_page_x, g_text_h, UTHEME_WHITE);
    ugfx_fill_rect(s, g_page_x, g_text_y, cw - g_page_x, 1, UTHEME_SEPARATOR);
    int lx = g_page_x + pad * 2, ly = g_text_y + (g_text_h - lh) / 2;
    const char *label = "Text to copy";
    ugfx_draw_string(s, lx, ly, label, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_WHITE);
    int bx = lx + ugfx_text_width(label) + pad * 2, bw = g_copy_all.x - pad - bx;
    uapp_logf_layout("charmap: layout textline %d %d %d %d\n", bx, g_text_y + pad, bw, g_text_h - pad * 2);
    uui_fill_round_rect(s, bx, g_text_y + pad, bw, g_text_h - pad * 2, 5, UTHEME_OUTLINE);
    uui_fill_round_rect(s, bx + 1, g_text_y + pad + 1, bw - 2, g_text_h - pad * 2 - 2, 4, UTHEME_WHITE);
    // Each character in the first font that has it: the line can hold
    // an arrow the session font cannot draw.
    struct face *f = cur();
    int x = bx + pad, cell = lh + 2;
    for (int i = 0; i < g_ntext && f && x + cell < bx + bw; i++) {
        struct face *g = f;
        for (int k = 0; k < g_nfaces && !uglyph_has(&g->reg, g_text[i]); k++) g = &g_faces[k];
        uglyph_draw(s, &g->reg, g_text[i], lh, x, g_text_y + pad, cell, g_text_h - pad * 2, UTHEME_TEXT);
        x += cell;
    }
    if (!g_ntext)
        ugfx_draw_string_clipped(s, bx + pad, ly, bw - pad * 2, "Double-click a character to add it",
                                 uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_WHITE);
}

static void card(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx;
    if (index < 0 || index >= g_nfaces) return;
    struct face *f = &g_faces[index];
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    int sel = state & UUI_GRID_SELECTED;
    uint32_t ink = sel ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, ground = sel ? UTHEME_ACCENT : g_cards.bg;
    int aa = h * 2 / 5;
    uglyph_text(s, &f->reg, "Aa", aa, x + pad, y + (h + uglyph_ascent(&f->reg, aa)) / 2, ink);
    int tx = x + pad * 2 + aa * 3 / 2, room = w - (tx - x) - pad, ty = y + (h - lh * 2 - 2) / 2;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, tx, ty, room, f->family, ink, ground);
    ugfx_set_font(was);
    char sub[64];
    const char *use = !strcmp(g_ui_face, f->stem) ? ", interface" : !strcmp(g_term_face, f->stem) ? ", terminals" : "";
    snprintf(sub, sizeof sub, "%d style%s%s", f->has_bold ? 2 : 1, f->has_bold ? "s" : "", use);
    ugfx_draw_string_clipped(s, tx, ty + lh + 2, room, sub, sel ? ink : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED),
                             ground);
}

static void draw_font_page(struct ugfx_surface *s, int cw) {
    struct face *f = cur();
    if (!f) return;
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    int x = g_sample.x, right = cw - pad * 2, y = g_sample.y + g_sample.h + pad;
    uapp_logf_layout("charmap: layout waterfall %d %d %d %d\n", x, y, right - x, g_use_ui.y - y - pad);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, x, 0, right - x, g_use_ui.y - pad);
    const char *text = uui_textbox_text(&g_sample)[0] ? uui_textbox_text(&g_sample) : SAMPLE;
    static const int SIZES[] = { 12, 16, 24, 36 };
    int lx = x + ugfx_text_width("Bold") + pad * 2;
    for (int i = 0; i < (int)(sizeof SIZES / sizeof SIZES[0]) + f->has_bold; i++) {
        int bold = i == (int)(sizeof SIZES / sizeof SIZES[0]);
        int px = bold ? 24 : SIZES[i];
        struct uglyph_face *g = bold ? &f->bold : &f->reg;
        char sz[8];
        snprintf(sz, sizeof sz, "%d", px);
        y += uglyph_ascent(g, px) + pad;
        ugfx_draw_string(s, x, y - lh + 3, bold ? "Bold" : sz, dim, UTHEME_WINDOW_BG);
        uglyph_text(s, g, text, px, lx, y, UTHEME_TEXT);
    }
    ugfx_clip_restore(s, &saved);
    y += pad * 2;
    ugfx_fill_rect(s, x, y, right - x, 1, UTHEME_SEPARATOR);
    y += pad;
    char files[96], chars[64];
    snprintf(files, sizeof files, "%s.ttf%s, %lu KB", f->stem, f->has_bold ? " and -bold.ttf" : "", f->bytes / 1024);
    snprintf(chars, sizeof chars, "%d with a Unicode name%s", f->named, f->mono ? ", monospaced" : "");
    const char *rows[][2] = {
        { "Files", files }, { "Characters", chars },
        { "Licence", f->licence[0] ? f->licence : "No licence file beside it" },
    };
    int vx = x + ugfx_text_width("Characters") + pad * 2;
    for (int i = 0; i < 3; i++, y += lh + 4) {
        ugfx_draw_string(s, x, y, rows[i][0], dim, UTHEME_WINDOW_BG);
        ugfx_draw_string_clipped(s, vx, y, right - vx, rows[i][1], UTHEME_TEXT, UTHEME_WINDOW_BG);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = d->surface;
    layout(s->w, s->h);
    uapp_log_layout(a, "charmap");
    // page, font, characters listed, the selected one, the text's length, its last
    uapp_logf_layout("charmap: layout state %d %d %d %d %d %d\n", g_page, g_cur, g_ncps, (int)selected_cp(), g_ntext,
                     g_ntext ? (int)g_text[g_ntext - 1] : 0);
    ugfx_fill_rect(s, g_rail_w, 0, 1, g_status.y, UTHEME_SEPARATOR);
    uui_statusbar_draw(s, &g_status);
    if (g_page == PAGE_CHARS) {
        ugfx_fill_rect(s, g_page_x, 0, s->w - g_page_x, g_top_h, UTHEME_CHROME);
        ugfx_fill_rect(s, g_page_x, g_top_h - 1, s->w - g_page_x, 1, UTHEME_SEPARATOR);
        int lw = ugfx_text_width("Block");
        ugfx_draw_string(s, g_block.x - lw - ugfx_char_w(), g_block.y + (g_block.h - ugfx_char_h()) / 2, "Block",
                         uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), UTHEME_CHROME);
        draw_detail(s, s->w);
        draw_text_line(s, s->w);
    } else {
        draw_font_page(s, s->w);
    }
}

// --- input ------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    switch (id) {
    case ID_RAIL: {
        int want = uui_sidebar_selected_id(&g_rail);
        if (want >= 0 && want != g_page) show_page(a, want);
        break;
    }
    case ID_SEARCH: fill(); break;
    case ID_FONT: if (g_chips.selected != g_cur) choose_font(g_chips.selected); break;
    case ID_FONTLIST: if (uui_dropdown_selected(&g_fontlist) != g_cur) choose_font(uui_dropdown_selected(&g_fontlist)); break;
    case ID_BLOCK: show_block(); break;
    case ID_GRID:
        if (uui_grid_take(&g_grid) >= 0) add_selected();
        break;
    case ID_CARDS:
        if (g_cards.selected >= 0 && g_cards.selected != g_cur) choose_font(g_cards.selected);
        break;
    default: break;
    }
    uapp_redraw(a);
}

static void on_action(struct uapp *a, int code) {
    uint32_t cp = selected_cp();
    char what[16];           // the status bar is Latin-1: name it, don't show it
    switch (code) {
    case ID_COPY:
        snprintf(what, sizeof what, "U+%04X", (unsigned)cp);
        if (cp) copy_text(&cp, 1, what);
        break;
    case ID_ADD:      add_selected(); break;
    case ID_COPY_ALL: if (g_ntext) copy_text(g_text, g_ntext, "the text"); break;
    case ID_CLEAR:    g_ntext = 0; break;
    case ID_USE_UI:
    case ID_USE_MONO: {
        const char *key = code == ID_USE_UI ? "system.font_face" : "system.font_mono";
        int rc = usetting_set(key, cur()->stem);
        snprintf(g_note, sizeof g_note, rc == SETTING_SAVED ? "%s now use %s" : "Could not change what %s use",
                 code == ID_USE_UI ? "The interface" : "Terminals", cur()->family);
        ulogf("charmap: %s = %s (%d)\n", key, cur()->stem, rc);
        read_usage();
        break;
    }
    default: break;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int chars = g_page == PAGE_CHARS, at = g_focus.current;
    if (chars && key == 0x03 && at == RING_GRID) { on_action(a, ID_COPY); return; }     // Ctrl+C
    if (chars && key == 0x06) { uui_focus_set(&g_focus, RING_SEARCH); uapp_redraw(a); return; }  // Ctrl+F
    if (chars && key == '\b' && at == RING_GRID && g_ntext) { g_ntext--; uapp_redraw(a); return; }
    if (chars && at == RING_SEARCH) fill();   // the query changed under a key
    if (chars && at == RING_BLOCK) show_block();
    if (chars && at == RING_GRID && uui_grid_take(&g_grid) >= 0) add_selected();
    if (g_page == PAGE_FONTS && g_cards.selected >= 0 && g_cards.selected != g_cur) choose_font(g_cards.selected);
    uapp_redraw(a);
}

static void on_open(struct uapp *a) {
    g_app = a;
    show_page(a, PAGE_CHARS);
}

static void on_size(int *w, int *h) {
    *w = ugfx_char_w() * 80;
    *h = ugfx_char_h() * 30;
}

int main(void) {
    load_faces();
    for (int i = 0; i < 2; i++)
        g_rail_rows[i] = (struct uui_sidebar_row){ .label = i ? "Fonts" : "Characters", .kind = UUI_SIDEBAR_TOP,
                                                   .icon = i ? "tb-font" : "tb-omega", .id = i };
    uui_sidebar_init(&g_rail, 0, 0, 0, 0, g_rail_rows, 2);
    uui_statusbar_init(&g_status);
    g_status.panes[0].text = g_note;
    g_status.panes[1] = (struct uui_status_pane){ .text = g_status_font, .chars = 26 };
    g_status.count = 2;
    uui_textbox_init(&g_search, "");
    g_search.placeholder = "Search by name or U+";
    uui_textbox_init(&g_sample, "");
    g_sample.placeholder = SAMPLE;
    uui_segmented_init(&g_chips, g_font_names, g_nfaces, 0);
    uui_dropdown_init(&g_fontlist, 0, 0, 0, 0, g_font_names, g_nfaces);
    uui_dropdown_init(&g_block, 0, 0, 0, 0, g_block_names, 0);
    uui_grid_init(&g_grid, ugfx_char_h() * 5 / 2, ugfx_char_h() * 5 / 2);
    uui_grid_init(&g_cards, ugfx_char_w() * 28, ugfx_char_h() * 4);
    uui_grid_set(&g_cards, g_nfaces, card, 0);
    g_cards.bg = UTHEME_PANEL_BG;
    uui_button_init(&g_copy, 0, 0, 0, 0, "Copy", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_COPY);
    uui_button_init(&g_add, 0, 0, 0, 0, "Add to text", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_ADD);
    uui_button_init(&g_copy_all, 0, 0, 0, 0, "Copy all", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_COPY_ALL);
    uui_button_init(&g_clear, 0, 0, 0, 0, "Clear", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CLEAR);
    uui_button_init(&g_use_ui, 0, 0, 0, 0, "Use for the interface", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_USE_UI);
    uui_button_init(&g_use_mono, 0, 0, 0, 0, "Use for terminals", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_USE_MONO);
    // The interface's face first, as GNOME Characters opens on the system font.
    read_usage();
    int first = 0;
    for (int i = 0; i < g_nfaces; i++) if (!strcmp(g_faces[i].stem, g_ui_face)) first = i;
    choose_font(first);

    struct uapp_desc desc = {
        .title = "Character Map",
        .app_id = "charmap",
        .flags = UAPP_RESIZABLE,
        .min_w = 560, .min_h = 360,
        .widgets = g_chars_items,
        .widget_count = (int)(sizeof g_chars_items / sizeof g_chars_items[0]),
        .focus = &g_focus,
        .on_open = on_open,
        .on_size = on_size,
        .on_draw = on_draw,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
    };
    int rc = uapp_run(&desc);
    for (int i = 0; i < g_nfaces; i++) {
        uglyph_close(&g_faces[i].reg);
        if (g_faces[i].has_bold) uglyph_close(&g_faces[i].bold);
    }
    return rc;
}
