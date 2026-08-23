// Minesweeper -- a ring-3 GUI application, and the first GAME on this
// desktop.
//
// WHY A GAME IS HERE AT ALL
// -------------------------
// The same reason Windows shipped Reversi in 1.0, Solitaire in 3.0 and
// Minesweeper in 3.1, and the reason GNOME still ships gnome-mines: a
// bundled game is a mouse-training program that also happens to be an
// end-to-end test of the toolkit. Solitaire taught drag-and-drop;
// Minesweeper taught precise left-and-right clicking. This one earns
// its place the same way -- it is the first client that needed a
// SECONDARY click, and building it is what closed that gap in the
// windowing protocol (abi/win_proto.h's WIN_EV_MOUSE_DOWN, and
// wm_input.c's wm_handle_right_click()). Before it, a right-click
// anywhere on a window -- content included -- was seized by the WM for
// the window menu, and no ring-3 app could ever see one.
//
// WHAT IT DELIBERATELY IS NOT
// ---------------------------
// It is not built on a reusable grid widget. There is one grid-shaped
// app in this tree, and a widget invented for a single caller is an API
// designed for nobody (CLAUDE.md's second-real-caller bar). If a second
// one ever lands -- Sudoku, a memory game, a chess board -- the shared
// part is a `uui_grid` and this app is where it comes from.
//
// THE ONE PLACE IT DIVERGES FROM THE HOUSE STYLE, STATED
// ------------------------------------------------------
// docs/gui-guidelines.md says colours come from the theme. The window,
// the panel, the menu bar and every border here do. The BOARD does not:
// the numbers 1-8 keep Minesweeper's own palette (1 blue, 2 green,
// 3 red...), because those colours are content, not chrome -- they are
// what a player reads the board with, the same argument syntax
// highlighting makes, and every implementation from winmine.exe to
// KMines to gnome-mines keeps them. What IS dropped is the 3D bevel:
// cells are flat, per docs/gui-guidelines.md, which is also what modern
// GNOME/KDE versions look like.
#include <stdint.h>
#include "ui/ulog.h"
#include "keyboard.h"
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"

// --- the board -------------------------------------------------------

#define MAX_COLS 30
#define MAX_ROWS 16
#define MAX_CELLS (MAX_COLS * MAX_ROWS)

// The three classic boards, in Windows' own sizes. Expert is 30x16 and
// not 16x30 for the same reason it is there: a wide board fits a
// landscape screen.
struct level { const char *name; int cols, rows, mines; };
static const struct level LEVELS[] = {
    { "Beginner",      9,  9, 10 },
    { "Intermediate", 16, 16, 40 },
    { "Expert",       30, 16, 99 },
};
#define LEVEL_COUNT ((int)(sizeof LEVELS / sizeof LEVELS[0]))

#define ST_COVERED  0
#define ST_REVEALED 1
#define ST_FLAGGED  2

#define PHASE_READY  0 // no mines placed yet -- the first click decides where
#define PHASE_PLAY   1
#define PHASE_WON    2
#define PHASE_LOST   3

static uint8_t g_mine[MAX_CELLS];
static uint8_t g_adj[MAX_CELLS];
static uint8_t g_st[MAX_CELLS];

static int g_level = 0;
static int g_cols = 9, g_rows = 9, g_mine_count = 10;
static int g_phase = PHASE_READY;
static int g_revealed;      // revealed non-mine cells
static int g_flags;         // flags placed
static int g_boom = -1;     // the mine that ended it, or -1

static unsigned long g_start_ticks;
static int g_elapsed;       // seconds, frozen once the game ends

static int g_armed = -1;    // cell held down by the primary button
static int g_face_armed;

static struct uui_menubar g_menu;

// --- randomness ------------------------------------------------------
//
// xorshift32, seeded from the kernel's real generator. Deliberately not
// krandom quality and it does not need to be: this picks mine positions,
// not keys, and api/krandom.h is explicit that a caller should know
// which it is getting rather than assume (CLAUDE.md).
static uint32_t g_rng = 0x9E3779B9u;

static void rng_seed(void) {
    uint32_t s = 0;
    if (sys_getrandom(&s, sizeof s) != (int)sizeof s || s == 0)
        s = (uint32_t)sys_ticks() * 2654435761u + 1u;
    g_rng = s ? s : 0x9E3779B9u;
}

static uint32_t rng_next(void) {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng = x;
}

// --- board logic -----------------------------------------------------

static int cell_count(void) { return g_cols * g_rows; }
static int idx_of(int c, int r) { return r * g_cols + c; }

static void reset_board(void) {
    for (int i = 0; i < MAX_CELLS; i++) { g_mine[i] = 0; g_adj[i] = 0; g_st[i] = ST_COVERED; }
    g_phase = PHASE_READY;
    g_revealed = 0;
    g_flags = 0;
    g_boom = -1;
    g_elapsed = 0;
    g_armed = -1;
    g_face_armed = 0;
}

// THE FIRST CLICK IS ALWAYS SAFE, and its whole 3x3 neighbourhood is
// too -- so the first click always OPENS something rather than landing
// on a bare "1" and requiring a guess. The original winmine only spared
// the clicked cell; every modern implementation (gnome-mines, KMines)
// spares the neighbourhood, and this follows them. Mines are placed
// AFTER that click, which is what makes the guarantee possible at all.
static void place_mines(int safe) {
    int sc = safe % g_cols, sr = safe / g_cols;
    int placed = 0;
    int n = cell_count();
    while (placed < g_mine_count) {
        int i = (int)(rng_next() % (uint32_t)n);
        if (g_mine[i]) continue;
        int c = i % g_cols, r = i / g_cols;
        int dc = c - sc, dr = r - sr;
        if (dc < 0) dc = -dc;
        if (dr < 0) dr = -dr;
        if (dc <= 1 && dr <= 1) continue; // the opening
        g_mine[i] = 1;
        placed++;
    }
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            int n2 = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (!dr && !dc) continue;
                    int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr >= g_rows || cc < 0 || cc >= g_cols) continue;
                    if (g_mine[idx_of(cc, rr)]) n2++;
                }
            }
            g_adj[idx_of(c, r)] = (uint8_t)n2;
        }
    }
}

// The flood fill's worklist is a STATIC, not a local: MAX_CELLS ints is
// ~2 KB and userland builds with -Wframe-larger-than (CLAUDE.md's note
// on the 20 KB frame that stepped over a guard page). Iterative rather
// than recursive for the same reason -- an empty Expert board would
// recurse 480 deep.
static int g_work[MAX_CELLS];

// A cell is MARKED WHEN IT IS PUSHED, not when it is popped. That is
// what bounds the worklist at one entry per cell: marking on pop lets
// the same cell be queued once per neighbour -- up to eight times -- and
// an Expert board's 480 cells would overrun a 480-entry array. The
// visited set and the result being the same array is what makes this
// safe with no second bitmap.
static void reveal_from(int start) {
    if (g_st[start] != ST_COVERED) return;
    int top = 0;
    g_st[start] = ST_REVEALED;
    g_revealed++;
    g_work[top++] = start;
    while (top > 0) {
        int i = g_work[--top];
        if (g_adj[i] != 0) continue; // a number stops the spread
        int c = i % g_cols, r = i / g_cols;
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int rr = r + dr, cc = c + dc;
                if (rr < 0 || rr >= g_rows || cc < 0 || cc >= g_cols) continue;
                int j = idx_of(cc, rr);
                // A FLAG BLOCKS THE FLOOD, as it does in every version:
                // the player has said "mine here", and an auto-open that
                // ignored that would lose games the player did not lose.
                if (g_st[j] != ST_COVERED || g_mine[j]) continue;
                g_st[j] = ST_REVEALED;
                g_revealed++;
                g_work[top++] = j;
            }
        }
    }
}

static void log_state(const char *what) {
    // The line a test asserts on. Everything a game rule can be checked
    // against is here, so mines_test.py never has to read a pixel to
    // know whether a click did the right thing -- pixels answer the
    // separate question of whether it was DRAWN (docs/gui-guidelines.md
    // on "it responds" not being "it is drawn").
    ulogf("mines: state %s phase=%d level=%d cols=%d rows=%d mines=%d "
          "flags=%d revealed=%d boom=%d elapsed=%d\n",
          what, g_phase, g_level, g_cols, g_rows, g_mine_count,
          g_flags, g_revealed, g_boom, g_elapsed);
}

static void end_lost(int at) {
    g_phase = PHASE_LOST;
    g_boom = at;
    for (int i = 0; i < cell_count(); i++)
        if (g_mine[i] && g_st[i] != ST_FLAGGED) g_st[i] = ST_REVEALED;
    log_state("lost");
}

static void check_won(void) {
    if (g_phase != PHASE_PLAY) return;
    if (g_revealed != cell_count() - g_mine_count) return;
    g_phase = PHASE_WON;
    // Flag whatever is left, as every version does -- the counter
    // reading 000 is half of what winning looks like.
    for (int i = 0; i < cell_count(); i++)
        if (g_mine[i] && g_st[i] != ST_FLAGGED) { g_st[i] = ST_FLAGGED; g_flags++; }
    log_state("won");
}

static void start_clock(void) {
    g_start_ticks = sys_ticks();
    g_elapsed = 0;
}

static void dig(int i) {
    if (g_phase == PHASE_WON || g_phase == PHASE_LOST) return;
    if (g_st[i] != ST_COVERED) return;
    if (g_phase == PHASE_READY) {
        place_mines(i);
        g_phase = PHASE_PLAY;
        start_clock();
    }
    if (g_mine[i]) { g_st[i] = ST_REVEALED; end_lost(i); return; }
    reveal_from(i);
    check_won();
}

// CHORDING: a click on a satisfied number opens its unflagged
// neighbours. Windows binds this to both buttons or the middle one and
// ALSO to a plain left click on a revealed number, which is the binding
// every player actually uses -- so that is the one implemented. A
// number whose flags are wrong opens a mine and loses, exactly as it
// should: chording is a claim that the flags are right.
static void chord(int i) {
    if (g_phase != PHASE_PLAY) return;
    if (g_st[i] != ST_REVEALED || g_adj[i] == 0) return;
    int c = i % g_cols, r = i / g_cols;
    int flags = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int rr = r + dr, cc = c + dc;
            if (rr < 0 || rr >= g_rows || cc < 0 || cc >= g_cols) continue;
            if (g_st[idx_of(cc, rr)] == ST_FLAGGED) flags++;
        }
    if (flags != g_adj[i]) return;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            int rr = r + dr, cc = c + dc;
            if (rr < 0 || rr >= g_rows || cc < 0 || cc >= g_cols) continue;
            int j = idx_of(cc, rr);
            if (g_st[j] != ST_COVERED) continue;
            if (g_mine[j]) { g_st[j] = ST_REVEALED; end_lost(j); return; }
            reveal_from(j);
        }
    check_won();
}

static void toggle_flag(int i) {
    if (g_phase == PHASE_WON || g_phase == PHASE_LOST) return;
    if (g_st[i] == ST_REVEALED) return;
    if (g_st[i] == ST_FLAGGED) { g_st[i] = ST_COVERED; g_flags--; }
    else                       { g_st[i] = ST_FLAGGED; g_flags++; }
}

// --- geometry, all font-derived --------------------------------------
//
// docs/gui-guidelines.md: layout is derived from the font, never
// written in pixels, which is what makes `fontsize` a real setting.

static int cell_px(void) {
    int c = ugfx_char_h() + 8;
    return c < 16 ? 16 : c;
}
static int pad_px(void)   { return 8; }
static int panel_h(void)  { return ugfx_char_h() + 14; }
static int menu_h(void)   { int h = 0; uui_menubar_natural_size(&g_menu, 0, &h); return h; }
static int board_w(void)  { return g_cols * cell_px(); }
static int board_h(void)  { return g_rows * cell_px(); }

static void content_size(int *w, int *h) {
    int mw = 0;
    uui_menubar_natural_size(&g_menu, &mw, 0);
    int cw = board_w() + 2 * pad_px();
    if (cw < mw + 8) cw = mw + 8;
    *w = cw;
    *h = menu_h() + pad_px() + panel_h() + pad_px() + board_h() + pad_px();
}

static void on_size(int *w, int *h) { content_size(w, h); }

static int board_x(int content_w) { return (content_w - board_w()) / 2; }
static int board_y(void)          { return menu_h() + pad_px() + panel_h() + pad_px(); }

static void face_rect(int content_w, int *x, int *y, int *w, int *h) {
    int s = panel_h() - 6;
    *w = s; *h = s;
    *x = (content_w - s) / 2;
    *y = menu_h() + pad_px() + 3;
}

// Which cell is under a content-relative point, or -1.
static int cell_at(struct uapp *a, int px, int py) {
    int bx = board_x(uapp_width(a)), by = board_y();
    int c = cell_px();
    if (px < bx || py < by) return -1;
    int col = (px - bx) / c, row = (py - by) / c;
    if (col < 0 || col >= g_cols || row < 0 || row >= g_rows) return -1;
    return idx_of(col, row);
}

// --- drawing ---------------------------------------------------------

// The board's own palette. See the header comment on why these are not
// theme colours.
static const uint8_t NUM_RGB[9][3] = {
    {   0,   0,   0 }, // 0 -- never drawn
    {  25,  70, 200 }, // 1 blue
    {  20, 120,  45 }, // 2 green
    { 200,  40,  40 }, // 3 red
    {  30,  35, 130 }, // 4 navy
    { 130,  40,  40 }, // 5 maroon
    {  20, 130, 130 }, // 6 teal
    {  30,  30,  30 }, // 7 black
    { 110, 110, 110 }, // 8 grey
};

static void draw_flag(struct ugfx_surface *s, int x, int y, int c) {
    uint32_t pole = ugfx_rgb(40, 40, 40);
    uint32_t cloth = ugfx_rgb(210, 45, 45);
    int m = c / 5;
    if (m < 2) m = 2;
    int px = x + c * 3 / 5;          // the pole, just right of centre
    int top = y + m, bot = y + c - m;
    ugfx_fill_rect(s, px, top, 2, bot - top, pole);
    ugfx_fill_rect(s, x + m, bot - 2, c - 2 * m, 2, pole);   // the base
    // The pennant: widest at the top, tapering to the pole. Drawn as
    // rows because ugfx has no filled triangle, and one caller does not
    // justify adding one (CLAUDE.md's second-real-caller bar).
    int ch = (bot - top) / 2;
    int cw = px - (x + m);
    for (int r = 0; r < ch; r++) {
        int wdt = cw * (ch - r) / ch;
        if (wdt > 0) ugfx_fill_rect(s, px - wdt, top + r, wdt, 1, cloth);
    }
}

static void draw_mine(struct ugfx_surface *s, int x, int y, int c) {
    uint32_t ink = ugfx_rgb(25, 25, 30);
    int cx = x + c / 2, cy = y + c / 2;
    int r = c / 2 - 3;
    if (r < 2) r = 2;
    ugfx_fill_circle(s, cx, cy, r, ink);
    ugfx_fill_rect(s, cx - r - 2, cy, 2 * r + 4, 1, ink);
    ugfx_fill_rect(s, cx, cy - r - 2, 1, 2 * r + 4, ink);
    ugfx_fill_rect(s, cx - r / 2, cy - r / 2, 2, 2, ugfx_rgb(235, 235, 235));
}

// The counters: three digits in a dark inset, the one piece of the
// classic look worth keeping literally, because it is how a player
// finds the two numbers without reading labels.
static void draw_counter(struct ugfx_surface *s, int x, int y, int w, int h, int v) {
    uint32_t box = ugfx_rgb(30, 30, 34);
    uint32_t lit = ugfx_rgb(235, 60, 50);
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    char t[4];
    t[0] = (char)('0' + (v / 100) % 10);
    t[1] = (char)('0' + (v / 10) % 10);
    t[2] = (char)('0' + v % 10);
    t[3] = '\0';
    ugfx_fill_rect(s, x, y, w, h, box);
    int tw = ugfx_text_width(t);
    ugfx_draw_string(s, x + (w - tw) / 2, y + (h - ugfx_char_h()) / 2, t, lit, box);
}

static void draw_face(struct ugfx_surface *s, int x, int y, int sz, int pressed) {
    uint32_t skin = ugfx_rgb(250, 214, 80);
    uint32_t ink = ugfx_rgb(35, 35, 35);
    uint32_t bg = pressed ? uui_state_bg(UTHEME_PANEL_BG, UUI_STATE_PRESSED)
                          : UTHEME_PANEL_BG;
    ugfx_fill_rect(s, x, y, sz, sz, bg);
    ugfx_draw_rect(s, x, y, sz, sz, UTHEME_BORDER);
    int cx = x + sz / 2, cy = y + sz / 2;
    int r = sz / 2 - 3;
    if (r < 4) r = 4;
    ugfx_fill_circle(s, cx, cy, r, skin);
    ugfx_draw_circle(s, cx, cy, r, ink, GEOM_ALIASED);

    int ex = r / 2, ey = r / 3;
    if (g_phase == PHASE_LOST) {
        // X eyes.
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            int px = cx + sgn * ex;
            ugfx_draw_line(s, px - 2, cy - ey - 2, px + 2, cy - ey + 2, ink, GEOM_ALIASED);
            ugfx_draw_line(s, px - 2, cy - ey + 2, px + 2, cy - ey - 2, ink, GEOM_ALIASED);
        }
    } else if (g_phase == PHASE_WON) {
        // Sunglasses -- one bar, two lenses.
        ugfx_fill_rect(s, cx - ex - 3, cy - ey - 1, 2 * ex + 6, 3, ink);
    } else {
        ugfx_fill_rect(s, cx - ex - 1, cy - ey - 1, 2, 3, ink);
        ugfx_fill_rect(s, cx + ex - 1, cy - ey - 1, 2, 3, ink);
    }

    // Mouth: a smile, or a frown when the game is lost. Three segments
    // rather than an arc -- at this size an arc and three segments are
    // the same pixels.
    int my = cy + r / 3;
    int mw = r / 2;
    if (g_phase == PHASE_LOST) {
        ugfx_fill_rect(s, cx - mw, my + 1, 2 * mw, 2, ink);
        ugfx_fill_rect(s, cx - mw - 1, my - 1, 2, 2, ink);
        ugfx_fill_rect(s, cx + mw - 1, my - 1, 2, 2, ink);
    } else {
        ugfx_fill_rect(s, cx - mw, my - 1, 2 * mw, 2, ink);
        ugfx_fill_rect(s, cx - mw - 1, my - 3, 2, 2, ink);
        ugfx_fill_rect(s, cx + mw - 1, my - 3, 2, 2, ink);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    int cw = uapp_width(a);

    ugfx_fill(s, UTHEME_WINDOW_BG);
    uui_menubar_draw(s, &g_menu);

    // --- the panel: mines remaining, the face, the clock -------------
    int py = menu_h() + pad_px();
    int ph = panel_h();
    ugfx_fill_rect(s, pad_px(), py, cw - 2 * pad_px(), ph, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, pad_px(), py, cw - 2 * pad_px(), ph, UTHEME_BORDER);

    int dw = ugfx_text_width("000") + 10;
    int dh = ph - 8;
    draw_counter(s, pad_px() + 6, py + 4, dw, dh, g_mine_count - g_flags);
    draw_counter(s, cw - pad_px() - 6 - dw, py + 4, dw, dh, g_elapsed);

    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    draw_face(s, fx, fy, fw, g_face_armed);

    // --- the board ---------------------------------------------------
    int bx = board_x(cw), by = board_y();
    int c = cell_px();
    uint32_t cover = UTHEME_BUTTON_BG;
    uint32_t open_bg = UTHEME_WHITE;
    uint32_t grid = UTHEME_BORDER;

    for (int r = 0; r < g_rows; r++) {
        for (int col = 0; col < g_cols; col++) {
            int i = idx_of(col, r);
            int x = bx + col * c, y = by + r * c;
            uint32_t bg;
            if (g_st[i] == ST_REVEALED) {
                bg = (i == g_boom) ? ugfx_rgb(220, 90, 90) : open_bg;
            } else {
                bg = (i == g_armed) ? uui_state_bg(cover, UUI_STATE_PRESSED) : cover;
            }
            ugfx_fill_rect(s, x, y, c, c, bg);
            ugfx_draw_rect(s, x, y, c, c, grid);

            if (g_st[i] == ST_FLAGGED) {
                draw_flag(s, x, y, c);
            } else if (g_st[i] == ST_REVEALED) {
                if (g_mine[i]) {
                    draw_mine(s, x, y, c);
                } else if (g_adj[i] > 0) {
                    char t[2] = { (char)('0' + g_adj[i]), '\0' };
                    const uint8_t *rgb = NUM_RGB[g_adj[i]];
                    int tw = ugfx_text_width(t);
                    ugfx_draw_string(s, x + (c - tw) / 2,
                                     y + (c - ugfx_char_h()) / 2, t,
                                     ugfx_rgb(rgb[0], rgb[1], rgb[2]), bg);
                }
            }
        }
    }
}

// The menu's popup is painted LAST, over everything -- immediate mode
// means z-order is call order (ui/uui_menubar.h).
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    uui_menubar_draw_popup(uapp_surface(d), &g_menu);
}

// --- input -----------------------------------------------------------

enum { CMD_NEW = 1, CMD_LEVEL_0, CMD_LEVEL_1, CMD_LEVEL_2, CMD_EXIT };

static void relayout(struct uapp *a) {
    int cw = uapp_width(a), ch = uapp_height(a);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, menu_h());
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
}

static void log_layout(struct uapp *a) {
    int cw = uapp_width(a);
    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    ulogf("mines: layout menubar %d %d %d %d\n", g_menu.x, g_menu.y, g_menu.w, g_menu.h);
    ulogf("mines: layout face %d %d %d %d\n", fx, fy, fw, fh);
    ulogf("mines: layout board %d %d %d %d\n", board_x(cw), board_y(),
          board_w(), board_h());
    ulogf("mines: layout cell %d\n", cell_px());
    // The menu's own rects, so a test clicks rows BY NAME's position
    // rather than by counting item heights in Python -- the trap
    // gui_flow.py's calibrated numbers already paid for. Titles always;
    // the rows only exist while a popup is open.
    for (int i = 0; ; i++) {
        int x, y, w, h;
        if (!uui_menubar_title_rect(&g_menu, i, &x, &y, &w, &h)) break;
        ulogf("mines: layout menutitle %d %d %d %d %d\n", i, x, y, w, h);
    }
    for (int l = 0; l < uui_menubar_depth(&g_menu); l++) {
        int x, y, w, h;
        for (int i = 0; uui_menubar_item_rect(&g_menu, l, i, &x, &y, &w, &h); i++)
            ulogf("mines: layout menuitem %d %d %d %d %d %d\n", l, i, x, y, w, h);
    }
}

static void new_game(struct uapp *a) {
    reset_board();
    log_state("new");
    log_layout(a);
    uapp_redraw(a);
}

static void set_level(struct uapp *a, int lv) {
    if (lv < 0 || lv >= LEVEL_COUNT) return;
    g_level = lv;
    g_cols = LEVELS[lv].cols;
    g_rows = LEVELS[lv].rows;
    g_mine_count = LEVELS[lv].mines;
    reset_board();
    int w, h;
    content_size(&w, &h);
    // The window follows the board, not the other way round -- a
    // 30x16 Expert board in a Beginner-sized window would simply be
    // cut off, since uui_layout (and this app) OVERFLOW rather than
    // shrink (CLAUDE.md).
    uapp_resize(a, w, h);
    relayout(a);
    log_state("level");
    log_layout(a);
    uapp_redraw(a);
}

static unsigned menu_item_flags(int code) {
    if (code >= CMD_LEVEL_0 && code <= CMD_LEVEL_2)
        return (code - CMD_LEVEL_0 == g_level) ? UUI_MI_CHECKED : 0;
    return 0;
}

static void do_command(struct uapp *a, int code) {
    switch (code) {
    case CMD_NEW:     new_game(a); break;
    case CMD_LEVEL_0: set_level(a, 0); break;
    case CMD_LEVEL_1: set_level(a, 1); break;
    case CMD_LEVEL_2: set_level(a, 2); break;
    case CMD_EXIT:    uapp_quit(a, 0); break;
    default: break;
    }
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    int secondary = (buttons & 0x2) != 0;

    if (!secondary) {
        if (uui_menubar_press(&g_menu, x, y)) {
            log_layout(a);   // the popup's rows now exist -- report them
            uapp_redraw(a);
            return;
        }
    } else if (uui_menubar_is_open(&g_menu)) {
        // A right-click while a menu is open dismisses it and does
        // nothing else, which is what every menu does.
        uui_menubar_close(&g_menu);
        uapp_redraw(a);
        return;
    }

    int cw = uapp_width(a);
    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    if (!secondary && uui_hit(fx, fy, fw, fh, x, y)) {
        g_face_armed = 1;             // arms here, acts on release
        uapp_redraw(a);
        return;
    }

    int i = cell_at(a, x, y);
    if (i < 0) return;

    if (secondary) {
        // FLAGGING COMMITS ON PRESS, and that is deliberate. It is the
        // documented shape of a secondary click in this GUI -- the same
        // exception docs/gui-guidelines.md already grants a menu, which
        // opens on press -- and it is what every Minesweeper does: a
        // flag is instant, and it is undone by right-clicking again
        // rather than by dragging off.
        toggle_flag(i);
        log_state("flag");
        uapp_redraw(a);
        return;
    }

    g_armed = i;   // the depressed cell; the dig happens on release
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (uui_menubar_motion(&g_menu, x, y)) uapp_redraw(a);
    if (uui_menubar_is_open(&g_menu)) return;
    if (!(buttons & 0x1)) return;
    // Dragging off the armed cell un-arms it, so a press can be
    // cancelled -- docs/gui-guidelines.md's rule, and Windows'
    // behaviour here too.
    int i = cell_at(a, x, y);
    if (i != g_armed) { g_armed = i; uapp_redraw(a); }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;

    int code = uui_menubar_release(&g_menu, x, y);
    if (code >= 0) { do_command(a, code); return; }
    if (uui_menubar_is_open(&g_menu)) { uapp_redraw(a); return; }

    int cw = uapp_width(a);
    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    if (g_face_armed) {
        g_face_armed = 0;
        if (uui_hit(fx, fy, fw, fh, x, y)) { new_game(a); return; }
        uapp_redraw(a);
        return;
    }

    int armed = g_armed;
    g_armed = -1;
    if (armed < 0) { uapp_redraw(a); return; }
    if (cell_at(a, x, y) != armed) { uapp_redraw(a); return; } // dragged off

    if (g_st[armed] == ST_REVEALED) chord(armed);
    else                            dig(armed);
    log_state("dig");
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    int code = -1;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code >= 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }
    if (key == KEY_F2) new_game(a);
}

// Once a second, and it BLOCKS in between -- see uapp.h's tick_ms. The
// elapsed time is derived from sys_ticks() rather than counted in ticks
// here, so a late wake-up cannot make the clock run slow.
static int on_tick(struct uapp *a) {
    (void)a;
    if (g_phase != PHASE_PLAY) return 0;
    int e = (int)((sys_ticks() - g_start_ticks) / 100);
    if (e > 999) e = 999;
    if (e == g_elapsed) return 0;
    g_elapsed = e;
    return 1;
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)w; (void)h;
    relayout(a);
    log_layout(a);
}

static void on_open(struct uapp *a) {
    rng_seed();
    reset_board();
    relayout(a);
    log_layout(a);
    log_state("open");
    ulog("mines: ready -- left click digs, right click flags, F2 is a new game\n");
}

int main(void) {
    if (!ugfx_font_init()) return 2; // every metric below comes from it

    static const struct uui_menu_item game_menu[] = {
        UUI_MENU("New Game",     CMD_NEW,     "F2"),
        UUI_MENU_SEP,
        UUI_MENU("Beginner",     CMD_LEVEL_0, "9x9, 10"),
        UUI_MENU("Intermediate", CMD_LEVEL_1, "16x16, 40"),
        UUI_MENU("Expert",       CMD_LEVEL_2, "30x16, 99"),
        UUI_MENU_SEP,
        UUI_MENU("Exit",         CMD_EXIT,    "Alt+F4"),
    };
    static const struct uui_menu_item menu_bar[] = {
        UUI_SUBMENU("Game", game_menu),
    };
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;

    g_cols = LEVELS[0].cols;
    g_rows = LEVELS[0].rows;
    g_mine_count = LEVELS[0].mines;

    struct uapp_desc desc = {
        .title        = "Minesweeper",
        .app_id       = "mines",
        .x            = 300,
        .y            = 120,
        .on_size      = on_size,
        .tick_ms      = 1000,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_draw_over = on_draw_over,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_key       = on_key,
        .on_tick      = on_tick,
        .on_resize    = on_resize,
    };
    return uapp_run(&desc);
}
