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
// app in this tree, and the board's drawing and rules are the app's
// own; if a second one ever lands -- Sudoku, a memory game, a chess
// board -- the shared part is a `uui_grid` and this app is where it
// comes from. What IS shared is generic by nature: the win's confetti
// (ui/uui_confetti.h) and its "Solved in" note (ui/uui_toast.h).
//
// THE ONE PLACE IT DIVERGES FROM THE HOUSE STYLE, STATED
// ------------------------------------------------------
// docs/gui-guidelines.md says colours come from the theme. The window,
// the panel, the menu bar, the tiles and every border here do. The
// NUMBERS do not: 1-8 keep Minesweeper's own palette (1 blue, 2 green,
// 3 red...), because those colours are content, not chrome -- they are
// what a player reads the board with, the same argument syntax
// highlighting makes, and every implementation from winmine.exe to
// KMines to gnome-mines keeps them. The look is the classic one
// modernised (chosen from mockups, 2026-10-05): the face and the two
// counters stay, the 3D bevel becomes a rounded, faintly lit tile.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ui/ulog.h"
#include "keyboard.h"
#include "rt/sys.h"
#include "fixed.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_anim.h"
#include "ui/uui_confetti.h"
#include "ui/uui_dialog.h"
#include "ui/uui_toast.h"
#include "lib/usnd.h"
#include "ui/uapp.h"
#include "ui/utheme.h"

// --- the board -------------------------------------------------------

#define MAX_COLS 30
#define MAX_ROWS 16
#define MAX_CELLS (MAX_COLS * MAX_ROWS)

// The three classic boards, in Windows' own sizes. Expert is 30x16 and
// not 16x30 for the same reason it is there: a wide board fits a
// landscape screen.
struct level { const char *name; const char *key; int cols, rows, mines; };
static const struct level LEVELS[] = {
    { "Beginner",     "beginner",      9,  9, 10 },
    { "Intermediate", "intermediate", 16, 16, 40 },
    { "Expert",       "expert",       30, 16, 99 },
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

// --- best times ------------------------------------------------------
//
// One line per level, "<key> <seconds>", in /var/lib: state a program
// must keep (docs/filesystem-layout.md). A missing or unreadable file
// is "no best yet", never an error.
#define BEST_DIR  "/var/lib/mines"
#define BEST_PATH BEST_DIR "/best"

static int g_best[LEVEL_COUNT];   // seconds, 0 = none yet

static void best_load(void) {
    for (int i = 0; i < LEVEL_COUNT; i++) g_best[i] = 0;
    FILE *f = fopen(BEST_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        char key[24];
        int sec;
        if (sscanf(line, "%23s %d", key, &sec) != 2 || sec <= 0) continue;
        for (int i = 0; i < LEVEL_COUNT; i++)
            if (!strcmp(key, LEVELS[i].key)) g_best[i] = sec;
    }
    fclose(f);
}

static void best_save(void) {
    mkdir(BEST_DIR, 0755);   // first use; an existing one is fine
    FILE *f = fopen(BEST_PATH, "w");
    if (!f) { ulog("mines: cannot write " BEST_PATH "\n"); return; }
    for (int i = 0; i < LEVEL_COUNT; i++)
        if (g_best[i]) fprintf(f, "%s %d\n", LEVELS[i].key, g_best[i]);
    fclose(f);
}

static void fmt_time(char *out, int cap, int sec) {
    snprintf(out, (size_t)cap, "%d:%02d", sec / 60, sec % 60);
}

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

// --- animation -------------------------------------------------------
//
// EVERY EFFECT IS A CELL'S START TIME, not a running state machine:
// an action schedules each cell it touches at an absolute time, and the
// draw works out where each one is from the clock. So a flood that
// opens eighty cells costs eighty stores, a slow frame skips ahead
// rather than slowing down, and nothing has to be stepped or cleaned up
// -- a cell whose effect has finished simply draws as its state says.
//
// Durations are at the desktop's NORMAL speed and go through
// uui_anim_ms() once per action, so `desktop.animation_speed` scales
// them and "off"/"instant" makes g_anim_k 0, which schedules nothing:
// every cell then draws in its final state on the very next frame.
#define ANIM_BASE     250   // what uui_anim_ms() is asked to scale
#define REVEAL_MS     300   // a cover lifting off
#define RING_MS        34   // between one ring of a flood and the next
#define FLAG_MS       380   // a flag dropping in
#define MINE_MS       420   // a mine going off
#define MINE_LEAD_MS  140   // the hit mine, then the rest
#define MINE_STEP_MS   70   // per cell of distance from the hit one
#define SHAKE_MS      400
#define AUTOFLAG_MS    55   // per cell of distance, the win's own flags
#define WAVE_MS       480
#define WAVE_STEP_MS   26   // per diagonal
#define CONFETTI_MS  1700
#define TOAST_MS      320

enum { FX_NONE = 0, FX_REVEAL, FX_FLAG, FX_MINE };

static uint8_t g_fx[MAX_CELLS];
static unsigned long long g_fx_t0[MAX_CELLS];
static unsigned g_anim_k;                 // uui_anim_ms(ANIM_BASE); 0 = no motion
static unsigned long long g_now;          // the clock, read once per action
static unsigned long long g_anim_end;     // the last scheduled cell effect ends
static int g_shake;
static unsigned long long g_shake_t0;
static int g_wave;
static unsigned long long g_wave_t0;
static struct uui_confetti g_confetti;
static struct uui_toast g_toast;

static unsigned ams(unsigned ms) {
    return (unsigned)((unsigned long long)ms * g_anim_k / ANIM_BASE);
}
static unsigned long long ns_of(unsigned ms) { return (unsigned long long)ams(ms) * 1000000ull; }

static unsigned fx_dur(int kind) {
    switch (kind) {
    case FX_REVEAL: return REVEAL_MS;
    case FX_FLAG:   return FLAG_MS;
    case FX_MINE:   return MINE_MS;
    default:        return 0;
    }
}

static void anim_begin(void) {
    g_anim_k = uui_anim_ms(ANIM_BASE);
    g_now = uui_anim_now_ns();
}

static void anim_reset(void) {
    for (int i = 0; i < MAX_CELLS; i++) g_fx[i] = FX_NONE;
    g_anim_end = 0;
    g_shake = 0;
    g_wave = 0;
    uui_confetti_stop(&g_confetti);
    uui_toast_hide(&g_toast);
}

static void schedule_at(int i, int kind, unsigned long long t0) {
    if (!g_anim_k) { g_fx[i] = FX_NONE; return; }
    g_fx[i] = (uint8_t)kind;
    g_fx_t0[i] = t0;
    unsigned long long end = t0 + ns_of(fx_dur(kind));
    if (end > g_anim_end) g_anim_end = end;
}

// How far through its effect cell `i` is at `now`, in permille: -1
// before it starts (it still looks as it did), 1000 once done.
static int fx_progress(int i, unsigned long long now) {
    if (g_fx[i] == FX_NONE) return 1000;
    if (now < g_fx_t0[i]) return -1;
    unsigned long long el = now - g_fx_t0[i];
    unsigned long long d = ns_of(fx_dur(g_fx[i]));
    if (!d || el >= d) return 1000;
    return (int)(el * 1000 / d);
}

// Distance between two cells in tenths of a cell, so a ripple outward
// from a point is round rather than square.
static int dist10(int a, int b) {
    int dc = a % g_cols - b % g_cols, dr = a / g_cols - b / g_cols;
    int sq = (dc * dc + dr * dr) * 100;
    int r = 0;
    while ((r + 1) * (r + 1) <= sq) r++;
    return r;
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
    anim_reset();
}

// The minefield, for a test: which cells are mines, so it can play a
// game to the end instead of guessing. Layout log only (off by
// default), like every other "where is it" line here.
static void log_minefield(void) {
    char line[128];
    int n = 0, len = 0;
    for (int i = 0; i < cell_count(); i++) {
        if (!g_mine[i]) continue;
        if (n % 20 == 0) {
            if (n) uapp_logf_layout("%s\n", line);
            len = snprintf(line, sizeof line, "mines: layout minefield %d", n);
        }
        len += snprintf(line + len, sizeof line - (size_t)len, " %d", i);
        n++;
    }
    if (n) uapp_logf_layout("%s\n", line);
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
    log_minefield();
}

// The flood fill's worklist is a STATIC, not a local: MAX_CELLS ints is
// ~2 KB and userland builds with -Wframe-larger-than (CLAUDE.md's note
// on the 20 KB frame that stepped over a guard page). Iterative rather
// than recursive for the same reason -- an empty Expert board would
// recurse 480 deep.
static int g_work[MAX_CELLS];
static uint16_t g_ring[MAX_CELLS];

// A cell is MARKED WHEN IT IS PUSHED, not when it is popped. That is
// what bounds the worklist at one entry per cell: marking on pop lets
// the same cell be queued once per neighbour -- up to eight times -- and
// an Expert board's 480 cells would overrun a 480-entry array. The
// visited set and the result being the same array is what makes this
// safe with no second bitmap.
//
// A QUEUE, so cells come out in rings around the click: each ring's
// distance is when its covers lift, which is the ripple.
static void reveal_from(int start, int base_ring) {
    if (g_st[start] != ST_COVERED) return;
    int head = 0, tail = 0;
    g_st[start] = ST_REVEALED;
    g_revealed++;
    g_ring[start] = (uint16_t)base_ring;
    g_work[tail++] = start;
    while (head < tail) {
        int i = g_work[head++];
        schedule_at(i, FX_REVEAL, g_now + ns_of((unsigned)g_ring[i] * RING_MS));
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
                g_ring[j] = (uint16_t)(g_ring[i] + 1);
                g_work[tail++] = j;
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
          "flags=%d revealed=%d boom=%d elapsed=%d best=%d\n",
          what, g_phase, g_level, g_cols, g_rows, g_mine_count,
          g_flags, g_revealed, g_boom, g_elapsed, g_best[g_level]);
}

// --- sound ------------------------------------------------------------
//
// The mixer's second real caller (lib/usnd.h): four clips, fired off
// and overlapping, with no stream involved. A game is why the library
// mixes at all -- a click landing while the previous one is still
// ringing is normal, and one voice would cut it.
//
// **NO SOUND IS NOT AN ERROR.** The default boot has no AC97 and the
// stream is exclusive, so `usnd_init()` failing (-ENODEV, or -EBUSY
// while the Audio Player has it) leaves every clip unloaded and every
// play a no-op. The game is unchanged; it is just quiet.
#define SFX_DIR "/usr/share/sounds/"

enum { SFX_CLICK, SFX_FLAG, SFX_BOOM, SFX_WIN, SFX_COUNT };

static struct usnd_clip g_sfx[SFX_COUNT];
static int g_have_sound;

static void sound_init(void) {
    static const char *const files[SFX_COUNT] = {
        SFX_DIR "click.wav", SFX_DIR "flag.wav",
        SFX_DIR "boom.wav",  SFX_DIR "win.wav",
    };
    if (usnd_init() != 0) { ulogf("mines: no sound -- %s\n", usnd_last_error()); return; }
    g_have_sound = 1;
    for (int i = 0; i < SFX_COUNT; i++)
        if (usnd_clip_load(files[i], &g_sfx[i]) != 0)
            ulogf("mines: %s -- %s\n", files[i], usnd_last_error());
}

static void sound_free(void) {
    if (!g_have_sound) return;
    for (int i = 0; i < SFX_COUNT; i++) usnd_clip_free(&g_sfx[i]);
    usnd_shutdown();
}

// Gains are per effect and deliberately not equal: a reveal happens
// dozens of times a game and a mine happens once.
static void sfx(int which, int gain) {
    if (g_have_sound) usnd_clip_play(&g_sfx[which], gain);
}

static void end_lost(int at) {
    g_phase = PHASE_LOST;
    g_boom = at;
    schedule_at(at, FX_MINE, g_now);
    for (int i = 0; i < cell_count(); i++) {
        if (!g_mine[i] || g_st[i] == ST_FLAGGED || i == at) continue;
        g_st[i] = ST_REVEALED;
        schedule_at(i, FX_MINE, g_now + ns_of(MINE_LEAD_MS +
                                              (unsigned)dist10(at, i) * MINE_STEP_MS / 10));
    }
    if (g_anim_k) {
        g_shake = 1;
        g_shake_t0 = g_now;
        unsigned long long end = g_now + ns_of(SHAKE_MS);
        if (end > g_anim_end) g_anim_end = end;
    }
    sfx(SFX_BOOM, 256);
    log_state("lost");
}

// What a win looks like, in order: the last covers lift, the remaining
// mines get their flags, the board waves corner to corner, and the
// confetti and the note come with the wave.
static void win_effects(int from, int is_best, int prev_best) {
    unsigned long long after_reveal = g_anim_end > g_now ? g_anim_end : g_now;
    for (int i = 0; i < cell_count(); i++) {
        if (!g_mine[i] || g_st[i] == ST_FLAGGED) continue;
        g_st[i] = ST_FLAGGED;
        g_flags++;
        schedule_at(i, FX_FLAG, after_reveal + ns_of(180 + (unsigned)dist10(from, i) * AUTOFLAG_MS / 10));
    }
    char t[24], b[24], msg[64];
    fmt_time(t, sizeof t, g_elapsed);
    if (is_best) snprintf(msg, sizeof msg, "Solved in %s - new best", t);
    else { fmt_time(b, sizeof b, prev_best); snprintf(msg, sizeof msg, "Solved in %s - best %s", t, b); }

    unsigned long long settle = g_anim_end > g_now ? g_anim_end : g_now;
    if (g_anim_k) {
        g_wave = 1;
        g_wave_t0 = settle + ns_of(150);
        unsigned long long end = g_wave_t0 + ns_of((unsigned)(g_cols + g_rows) * WAVE_STEP_MS + WAVE_MS);
        if (end > g_anim_end) g_anim_end = end;
    }
    // Launched by on_draw, which knows where the board is drawn.
    uui_confetti_stop(&g_confetti);
    uui_toast_show(&g_toast, msg, ams(TOAST_MS), g_anim_k ? g_wave_t0 + ns_of(250) : g_now);
}

static int g_confetti_pending;

static void check_won(int from) {
    if (g_phase != PHASE_PLAY) return;
    if (g_revealed != cell_count() - g_mine_count) return;
    g_phase = PHASE_WON;
    g_elapsed = (int)((sys_ticks() - g_start_ticks) / 100);
    if (g_elapsed < 1) g_elapsed = 1;
    if (g_elapsed > 999) g_elapsed = 999;
    int prev = g_best[g_level];
    int is_best = !prev || g_elapsed < prev;
    if (is_best) { g_best[g_level] = g_elapsed; best_save(); }
    // Flag whatever is left, as every version does -- the counter
    // reading 000 is half of what winning looks like.
    win_effects(from, is_best, prev);
    g_confetti_pending = g_anim_k != 0;
    sfx(SFX_WIN, 224);
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
    reveal_from(i, 0);
    check_won(i);
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
            reveal_from(j, 1);
        }
    check_won(i);
}

static void toggle_flag(int i) {
    if (g_phase == PHASE_WON || g_phase == PHASE_LOST) return;
    if (g_st[i] == ST_REVEALED) return;
    if (g_st[i] == ST_FLAGGED) { g_st[i] = ST_COVERED; g_flags--; g_fx[i] = FX_NONE; }
    else                       { g_st[i] = ST_FLAGGED; g_flags++; schedule_at(i, FX_FLAG, g_now); }
    sfx(SFX_FLAG, 160);
}

// --- geometry, all font-derived --------------------------------------
//
// docs/gui-guidelines.md: layout is derived from the font, never
// written in pixels, which is what makes `fontsize` a real setting.

static int cell_px(void) {
    int c = ugfx_char_h() + 8;
    return c < 16 ? 16 : c;
}
static int pad_px(void)   { return 10; }
static int frame_px(void) { return 3; }   // the rounded rim round the board
static int panel_h(void)  { return ugfx_char_h() + 18; }
static int menu_h(void)   { return uui_menubar_height(&g_menu); }
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
    int s = panel_h() - 8;
    *w = s; *h = s;
    *x = (content_w - s) / 2;
    *y = menu_h() + pad_px() + 4;
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

// The numbers' own palette. See the header comment on why these are not
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

// The board's colours, from the theme: a covered tile is a control, an
// opened one sits a little off-white, and the rim between is a quiet
// step towards the outline.
static uint32_t col_cover(void) { return UTHEME_BUTTON_BG; }
static uint32_t col_open(void)  { return ugfx_blend(UTHEME_WINDOW_BG, UTHEME_WHITE, 180); }
static uint32_t col_line(void)  { return ugfx_blend(col_open(), UTHEME_SEPARATOR, 110); }
static uint32_t col_rim(void)   { return ugfx_blend(UTHEME_WINDOW_BG, UTHEME_OUTLINE, 90); }

static void draw_flag(struct ugfx_surface *s, int x, int y, int c) {
    uint32_t pole = ugfx_rgb(40, 40, 40);
    uint32_t cloth = ugfx_rgb(210, 45, 45);
    int m = c / 5;
    if (m < 2) m = 2;
    int px = x + c * 3 / 5;          // the pole, just right of centre
    int top = y + m, bot = y + c - m;
    ugfx_fill_rect(s, px, top, 2, bot - top, pole);
    ugfx_fill_rect(s, x + m, bot - 2, c - 2 * m, 2, pole);   // the base
    // The pennant: widest at the top, tapering to the pole. Rows,
    // because a triangle this small is the same pixels either way.
    int ch = (bot - top) / 2;
    int cw = px - (x + m);
    for (int r = 0; r < ch; r++) {
        int wdt = cw * (ch - r) / ch;
        if (wdt > 0) ugfx_fill_rect(s, px - wdt, top + r, wdt, 1, cloth);
    }
}

// `scale` in permille of the mine's resting size.
static void draw_mine(struct ugfx_surface *s, int cx, int cy, int r, int scale) {
    uint32_t ink = ugfx_rgb(25, 25, 30);
    r = r * scale / 1000;
    if (r < 1) return;
    int spike = r + r / 2;
    ugfx_fill_circle(s, cx, cy, r, ink);
    ugfx_fill_rect(s, cx - spike, cy, 2 * spike + 1, 1, ink);
    ugfx_fill_rect(s, cx, cy - spike, 1, 2 * spike + 1, ink);
    int d = spike * 7 / 10;
    ugfx_draw_line(s, cx - d, cy - d, cx + d, cy + d, ink, GEOM_ALIASED);
    ugfx_draw_line(s, cx - d, cy + d, cx + d, cy - d, ink, GEOM_ALIASED);
    if (r >= 3) ugfx_fill_rect(s, cx - r / 2, cy - r / 2, 2, 2, ugfx_rgb(235, 235, 235));
}

static void draw_clock(struct ugfx_surface *s, int cx, int cy, int r) {
    uint32_t ink = ugfx_rgb(25, 25, 30);
    ugfx_draw_circle(s, cx, cy, r, ink, GEOM_AA);
    ugfx_draw_circle(s, cx, cy, r - 1, ink, GEOM_AA);
    ugfx_draw_line(s, cx, cy, cx, cy - r + 3, ink, GEOM_ALIASED);
    ugfx_draw_line(s, cx, cy, cx + r / 2, cy + r / 3, ink, GEOM_ALIASED);
}

// The counters: three digits in a dark rounded inset, the one piece of
// the classic look worth keeping literally, because it is how a player
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
    uui_fill_round_rect(s, x, y, w, h, 6, box);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int tw = ugfx_text_width(t);
    ugfx_draw_string(s, x + (w - tw) / 2, y + (h - ugfx_char_h()) / 2, t, lit, box);
    ugfx_set_font(was);
}

static void draw_face(struct ugfx_surface *s, int x, int y, int sz, int pressed, int worried) {
    uint32_t skin = ugfx_rgb(250, 214, 80);
    uint32_t ink = ugfx_rgb(35, 35, 35);
    uint32_t bg = pressed ? uui_state_bg(UTHEME_BUTTON_BG, UUI_STATE_PRESSED) : UTHEME_BUTTON_BG;
    int cx = x + sz / 2, cy = y + sz / 2;
    ugfx_fill_circle(s, cx, cy, sz / 2, ugfx_blend(bg, UTHEME_OUTLINE, 120));
    ugfx_fill_circle(s, cx, cy, sz / 2 - 1, bg);
    int r = sz / 2 - 4;
    if (r < 4) r = 4;
    ugfx_fill_circle(s, cx, cy, r, skin);
    ugfx_draw_circle(s, cx, cy, r, ink, GEOM_AA);

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

    // Mouth: a smile; a frown when lost; an "o" while a cell is held
    // down, which is winmine's own tell.
    int my = cy + r / 3;
    int mw = r / 2;
    if (worried) {
        ugfx_draw_circle(s, cx, my, r / 4 > 1 ? r / 4 : 2, ink, GEOM_AA);
    } else if (g_phase == PHASE_LOST) {
        ugfx_fill_rect(s, cx - mw, my + 1, 2 * mw, 2, ink);
        ugfx_fill_rect(s, cx - mw - 1, my - 1, 2, 2, ink);
        ugfx_fill_rect(s, cx + mw - 1, my - 1, 2, 2, ink);
    } else {
        ugfx_fill_rect(s, cx - mw, my - 1, 2 * mw, 2, ink);
        ugfx_fill_rect(s, cx - mw - 1, my - 3, 2, 2, ink);
        ugfx_fill_rect(s, cx + mw - 1, my - 3, 2, 2, ink);
    }
}

// A covered tile: rounded, inset a pixel, lit along its top edge and
// shaded along its bottom -- the bevel, quietened.
static void draw_cover(struct ugfx_surface *s, int x, int y, int c, int pressed) {
    uint32_t face = pressed ? uui_state_bg(col_cover(), UUI_STATE_PRESSED) : col_cover();
    uui_fill_round_rect(s, x + 1, y + 1, c - 2, c - 2, 3, face);
    if (pressed) return;
    ugfx_blend_hspan(s, x + 4, y + 1, c - 8, UTHEME_WHITE, 0, 190);
    ugfx_blend_hspan(s, x + 4, y + c - 2, c - 8, UTHEME_BORDER, 0, 30);
}

static void draw_open(struct ugfx_surface *s, int x, int y, int c, uint32_t bg) {
    ugfx_fill_rect(s, x, y, c, c, bg);
    ugfx_fill_rect(s, x + c - 1, y, 1, c, col_line());
    ugfx_fill_rect(s, x, y + c - 1, c, 1, col_line());
}

static void draw_number(struct ugfx_surface *s, int x, int y, int c, int n, uint32_t bg, int alpha) {
    const uint8_t *rgb = NUM_RGB[n];
    uint32_t ink = ugfx_blend(bg, ugfx_rgb(rgb[0], rgb[1], rgb[2]), (uint8_t)alpha);
    char t[2] = { (char)('0' + n), '\0' };
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int tw = ugfx_text_width(t);
    ugfx_draw_string(s, x + (c - tw) / 2, y + (c - ugfx_char_h()) / 2, t, ink, bg);
    ugfx_set_font(was);
}

static void draw_cell(struct ugfx_surface *s, int i, int x, int y, int c, unsigned long long now) {
    int st = g_st[i];
    if (st == ST_COVERED) { draw_cover(s, x, y, c, i == g_armed); return; }

    int p = fx_progress(i, now);
    if (st == ST_FLAGGED) {
        draw_cover(s, x, y, c, 0);
        if (p < 0) return;
        // Falls from half a cell up, then a small bounce.
        int dy = 0;
        if (p < 700) dy = -(c / 2) * (700 - p) * (700 - p) / (700 * 700);
        else if (p < 1000) dy = -(int)(((long long)(c / 8) * fx_sin((fx_t)((long long)(p - 700) * FX_ONE / 600))) >> FX_SHIFT);
        draw_flag(s, x, y + dy, c);
        if (g_phase == PHASE_LOST && !g_mine[i]) {
            // A wrong flag, crossed out.
            uint32_t red = ugfx_rgb(200, 40, 40);
            for (int k = 0; k < 2; k++) {
                ugfx_draw_line(s, x + 4 + k, y + 4, x + c - 5 + k, y + c - 5, red, GEOM_ALIASED);
                ugfx_draw_line(s, x + 4 + k, y + c - 5, x + c - 5 + k, y + 4, red, GEOM_ALIASED);
            }
        }
        return;
    }

    if (p < 0) { draw_cover(s, x, y, c, 0); return; }   // its ring has not come yet

    if (g_mine[i]) {
        // A flash of yellow, settling to a pale red; the one that was
        // hit is solid red from the start.
        uint32_t yellow = ugfx_rgb(255, 224, 138), pale = ugfx_rgb(242, 202, 202);
        uint32_t bg;
        if (i == g_boom)     bg = ugfx_rgb(224, 90, 90);
        else if (p >= 1000)  bg = pale;
        else if (p < 250)    bg = ugfx_blend(col_cover(), yellow, (uint8_t)(p * 255 / 250));
        else                 bg = ugfx_blend(yellow, pale, (uint8_t)((p - 250) * 255 / 750));
        draw_open(s, x, y, c, bg);
        // Pops a little past its size, then settles.
        int scale = p < 700 ? p * 1150 / 700 : 1150 - (p - 700) / 2;
        draw_mine(s, x + c / 2, y + c / 2, c / 2 - 5 > 2 ? c / 2 - 5 : 2, scale);
        return;
    }

    uint32_t bg = col_open();
    draw_open(s, x, y, c, bg);
    if (g_adj[i] > 0 && p >= 350)
        draw_number(s, x, y, c, g_adj[i], bg, p >= 750 ? 255 : (p - 350) * 255 / 400);
    if (p < 1000) {
        // The cover lifting off: it shrinks towards its centre and
        // fades into the opened colour.
        int full = c - 2;
        int sz = full - full * 85 * p / 100000;
        uint32_t col = ugfx_blend(col_cover(), bg, (uint8_t)(p * 255 / 1000));
        uui_fill_round_rect(s, x + (c - sz) / 2, y + (c - sz) / 2, sz, sz, 3, col);
    }
}

// The whole board shudders sideways when a mine goes off.
static int shake_dx(unsigned long long now) {
    if (!g_shake || now < g_shake_t0) return 0;
    unsigned long long d = ns_of(SHAKE_MS), el = now - g_shake_t0;
    if (!d || el >= d) { g_shake = 0; return 0; }
    int p = (int)(el * 1000 / d);
    int amp = cell_px() / 4 * (1000 - p) / 1000;
    return (int)(((long long)amp * fx_sin((fx_t)((long long)p * 3 * FX_ONE / 1000))) >> FX_SHIFT);
}

// A win's wave: each diagonal of tiles lifts and settles in turn.
static int wave_dy(int col, int row, unsigned long long now) {
    if (!g_wave) return 0;
    unsigned long long t0 = g_wave_t0 + ns_of((unsigned)(col + row) * WAVE_STEP_MS);
    if (now < t0) return 0;
    unsigned long long d = ns_of(WAVE_MS), el = now - t0;
    if (!d || el >= d) return 0;
    int p = (int)(el * 1000 / d);
    return -(int)(((long long)(cell_px() / 5) * fx_sin((fx_t)((long long)p * FX_ONE / 2000))) >> FX_SHIFT);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    int cw = uapp_width(a);
    unsigned long long now = uui_anim_now_ns();

    ugfx_fill(s, UTHEME_WINDOW_BG);
    uui_menubar_draw(s, &g_menu);

    // --- the panel: mines remaining, the face, the clock -------------
    int px = pad_px(), py = menu_h() + pad_px();
    int pw = cw - 2 * pad_px(), ph = panel_h();
    uui_fill_round_rect(s, px, py, pw, ph, 8, ugfx_blend(UTHEME_PANEL_BG, UTHEME_OUTLINE, 130));
    uui_fill_round_rect(s, px + 1, py + 1, pw - 2, ph - 2, 7, UTHEME_PANEL_BG);

    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int dw = ugfx_text_width("000") + 12;
    ugfx_set_font(was);
    int dh = ph - 12;
    int ic = ugfx_char_h() / 2;   // the icons' radius
    int cy = py + ph / 2;
    draw_mine(s, px + 8 + ic, cy, ic * 2 / 3, 1000);
    draw_counter(s, px + 8 + 2 * ic + 6, py + 6, dw, dh, g_mine_count - g_flags);
    draw_counter(s, px + pw - 8 - 2 * ic - 6 - dw, py + 6, dw, dh, g_elapsed);
    draw_clock(s, px + pw - 8 - ic, cy, ic);

    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    int worried = g_armed >= 0 && (g_phase == PHASE_READY || g_phase == PHASE_PLAY);
    draw_face(s, fx, fy, fw, g_face_armed, worried);

    // --- the board ---------------------------------------------------
    int bx = board_x(cw) + shake_dx(now), by = board_y();
    int c = cell_px();
    int f = frame_px();
    uui_fill_round_rect(s, bx - f, by - f, board_w() + 2 * f, board_h() + 2 * f, 6, col_rim());
    uui_fill_round_rect(s, bx, by, board_w(), board_h(), 3, col_open());

    for (int r = 0; r < g_rows; r++)
        for (int col = 0; col < g_cols; col++)
            draw_cell(s, idx_of(col, r), bx + col * c, by + r * c + wave_dy(col, r, now), c, now);

    // --- the win: confetti from the board, and the note --------------
    if (g_confetti_pending && g_wave && now >= g_wave_t0) {
        g_confetti_pending = 0;
        uui_confetti_start(&g_confetti, bx + board_w() / 2, by + board_h() / 3,
                           board_h() * 2 / 3 + c, 64, ams(CONFETTI_MS), rng_next(), now);
    }
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, 0, menu_h(), cw, uapp_height(a) - menu_h());
    uui_confetti_draw(s, &g_confetti, now);
    ugfx_clip_restore(s, &saved);
    uui_toast_draw(s, &g_toast, bx, by, board_w(), board_h(), now);

    if (now < g_anim_end || g_confetti_pending) uui_anim_request();
}

static struct uui_dialog g_dlg;

// The menu's popup is painted LAST, over everything -- immediate mode
// means z-order is call order (ui/uui_menubar.h). The dialog is over
// even that.
static void on_draw_over(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    uui_menubar_draw_popup(uapp_surface(d), &g_menu);
    if (uui_dialog_is_open(&g_dlg)) uui_dialog_draw(uapp_surface(d), &g_dlg);
}

// --- input -----------------------------------------------------------

enum { CMD_NEW = 1, CMD_LEVEL_0, CMD_LEVEL_1, CMD_LEVEL_2, CMD_BEST, CMD_EXIT };
enum { DLG_OK = 1, DLG_RESET };

static void relayout(struct uapp *a) {
    int cw = uapp_width(a), ch = uapp_height(a);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, menu_h());
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_dialog_set_bounds(&g_dlg, 0, 0, cw, ch);
}

static void log_layout(struct uapp *a) {
    int cw = uapp_width(a);
    int fx, fy, fw, fh;
    face_rect(cw, &fx, &fy, &fw, &fh);
    // The menu reports itself (titles, popup rows) in the shared
    // vocabulary (ui/uui_describe.h); the board, face and cell are this
    // app's own, drawn by hand.
    uapp_log_widget(a, "mines", "menu", &uui_menubar_ops, &g_menu);
    uapp_logf_layout("mines: layout face %d %d %d %d\n", fx, fy, fw, fh);
    uapp_logf_layout("mines: layout board %d %d %d %d\n", board_x(cw), board_y(),
          board_w(), board_h());
    uapp_logf_layout("mines: layout cell %d\n", cell_px());
    int tx, ty, tw, th;
    if (uui_toast_rect(&g_toast, board_x(cw), board_y(), board_w(), board_h(), &tx, &ty, &tw, &th))
        uapp_logf_layout("mines: layout toast %d %d %d %d\n", tx, ty, tw, th);
    if (uui_dialog_is_open(&g_dlg))
        uapp_log_widget(a, "mines", "best", &uui_dialog_ops, &g_dlg);
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

// The rows are the dialog's to point at, so they live here.
static char g_best_rows[LEVEL_COUNT][48];
static const char *g_best_row_ptr[LEVEL_COUNT];

static void open_best(struct uapp *a) {
    for (int i = 0; i < LEVEL_COUNT; i++) {
        char t[16];
        if (g_best[i]) fmt_time(t, sizeof t, g_best[i]);
        else snprintf(t, sizeof t, "--");
        snprintf(g_best_rows[i], sizeof g_best_rows[i], "%-14s %s", LEVELS[i].name, t);
        g_best_row_ptr[i] = g_best_rows[i];
    }
    static const struct uui_dialog_button btns[] = {
        { "Reset", DLG_RESET, UUI_DLG_DANGER },
        { "OK",    DLG_OK,    UUI_DLG_PLAIN },
    };
    uui_dialog_set_bounds(&g_dlg, 0, 0, uapp_width(a), uapp_height(a));
    uui_dialog_open(&g_dlg, "Best Times", g_best_row_ptr, LEVEL_COUNT, btns, 2, 1, DLG_OK);
    log_layout(a);
    uapp_redraw(a);
}

static void dialog_answered(struct uapp *a) {
    int code = uui_dialog_take_code(&g_dlg);
    if (code < 0) return;
    uui_dialog_close(&g_dlg);
    if (code == DLG_RESET) {
        for (int i = 0; i < LEVEL_COUNT; i++) g_best[i] = 0;
        best_save();
        log_state("best-reset");
    }
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
    case CMD_BEST:    open_best(a); break;
    case CMD_EXIT:    uapp_quit(a, 0); break;
    default: break;
    }
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    int secondary = (buttons & 0x2) != 0;

    if (uui_dialog_is_open(&g_dlg)) {
        uui_dialog_ops.press(&g_dlg, x, y, 0);
        uapp_redraw(a);
        return;
    }

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
        anim_begin();
        toggle_flag(i);
        log_state("flag");
        uapp_redraw(a);
        return;
    }

    g_armed = i;   // the depressed cell; the dig happens on release
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (uui_dialog_is_open(&g_dlg)) {
        if (uui_dialog_ops.motion(&g_dlg, x, y, buttons)) uapp_redraw(a);
        return;
    }
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

    if (uui_dialog_is_open(&g_dlg)) {
        uui_dialog_ops.release(&g_dlg, x, y);
        dialog_answered(a);
        uapp_redraw(a);
        return;
    }

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

    int before = g_revealed;
    anim_begin();
    if (g_st[armed] == ST_REVEALED) chord(armed);
    else                            dig(armed);
    // ONE click per user action, not one per cascaded cell -- a first
    // click opening forty cells would fire forty voices into eight.
    // Losing and winning have their own sound and are not also a click.
    if (g_phase == PHASE_PLAY && g_revealed != before) sfx(SFX_CLICK, 200);
    log_state("dig");
    if (g_phase == PHASE_WON) log_layout(a);   // the toast is somewhere now
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (uui_dialog_is_open(&g_dlg)) {
        uui_dialog_key(&g_dlg, key);
        dialog_answered(a);
        uapp_redraw(a);
        return;
    }
    int code;
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
    best_load();
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
        UUI_MENU("Best Times...", CMD_BEST,   0),
        UUI_MENU_SEP,
        UUI_MENU("Exit",         CMD_EXIT,    "Alt+F4"),
    };
    static const struct uui_menu_item menu_bar[] = {
        UUI_SUBMENU("Game", game_menu),
    };
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;
    uui_dialog_init(&g_dlg);

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
    sound_init();
    int rc = uapp_run(&desc);
    sound_free();
    return rc;
}
