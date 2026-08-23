// DOOM, as an ordinary ring-3 client.
//
// **THE TOY-OS SIDE OF THE SEAM.** This file includes `api/keyboard.h`
// and NEVER doomgeneric's `doomkeys.h` -- both define `KEY_F2`,
// `KEY_F3`, `KEY_F4` and `KEY_F10` with different values, and neither is
// ours to rename. `userland/doom/dg_toyos.h` is the header they meet
// through; the static assertions below are what stop its copied
// constants drifting from the real ones, and they live here because this
// is the file that can see both without a collision.
//
// WHAT MAKES THIS AN ORDINARY CLIENT, which is the interesting claim:
// it declares a `uapp_desc` and gets a window, chrome, a taskbar button,
// focus, a close button and the window menu from the toolkit, exactly as
// Minesweeper does. There is no Doom-shaped special case anywhere in the
// WM or the kernel. What the port needed was not a privileged path but
// two ordinary capabilities the OS did not have: an image bigger than
// 1 MiB, and a key that can come UP.
//
// WHAT IT DELIBERATELY DOES NOT DO:
//   * **No sound.** doomgeneric's `i_sound.c` is compiled and resolves
//     to silence with no module registered -- upstream behaviour, not a
//     patch. There is no audio device driver in this OS yet.
//   * **No mouse look.** Doom's mouse is relative-motion aim, and a
//     windowed client gets position, not deltas -- it would need a
//     pointer grab TWS does not have. Keyboard controls are complete.
//   * **No mouse look**, as above. Keyboard controls are complete.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "win_proto.h"  // WIN_CLIENT_MAX_W -- the scratch row's bound
#include "doom/dg_toyos.h"

// THE COPIES IN dg_toyos.h, CHECKED AGAINST THE REAL THING. A drift in
// either direction is a build error rather than a key that quietly stops
// working -- which is the failure this would otherwise have, since a
// wrong constant maps to a key Doom does nothing with and simply looks
// like an unbound control.
_Static_assert(TOYKEY_ARROW_UP    == KEY_ARROW_UP,    "TOYKEY_ARROW_UP drifted");
_Static_assert(TOYKEY_ARROW_DOWN  == KEY_ARROW_DOWN,  "TOYKEY_ARROW_DOWN drifted");
_Static_assert(TOYKEY_ARROW_LEFT  == KEY_ARROW_LEFT,  "TOYKEY_ARROW_LEFT drifted");
_Static_assert(TOYKEY_ARROW_RIGHT == KEY_ARROW_RIGHT, "TOYKEY_ARROW_RIGHT drifted");
_Static_assert(TOYKEY_PAGE_UP     == KEY_PAGE_UP,     "TOYKEY_PAGE_UP drifted");
_Static_assert(TOYKEY_PAGE_DOWN   == KEY_PAGE_DOWN,   "TOYKEY_PAGE_DOWN drifted");
_Static_assert(TOYKEY_HOME        == KEY_HOME,        "TOYKEY_HOME drifted");
_Static_assert(TOYKEY_END         == KEY_END,         "TOYKEY_END drifted");
_Static_assert(TOYKEY_DELETE      == KEY_DELETE,      "TOYKEY_DELETE drifted");
_Static_assert(TOYKEY_F2          == KEY_F2,          "TOYKEY_F2 drifted");
_Static_assert(TOYKEY_F3          == KEY_F3,          "TOYKEY_F3 drifted");
_Static_assert(TOYKEY_F4          == KEY_F4,          "TOYKEY_F4 drifted");
_Static_assert(TOYKEY_F10         == KEY_F10,         "TOYKEY_F10 drifted");
_Static_assert(TOYKEY_SHIFT       == KEY_SHIFT,       "TOYKEY_SHIFT drifted");
_Static_assert(TOYKEY_CTRL        == KEY_CTRL,        "TOYKEY_CTRL drifted");
_Static_assert(TOYKEY_ALT         == KEY_ALT,         "TOYKEY_ALT drifted");
_Static_assert(TOYKEY_ALTGR       == KEY_ALTGR,       "TOYKEY_ALTGR drifted");

// WHERE THE IWAD LIVES. A name, not a search path: `d_iwad.c` can hunt
// through a list of directories and several filenames, and on an OS with
// exactly one place for read-only game data that is machinery with
// nothing to do. docs/filesystem-layout.md owns this path.
#define WAD_DIR  "/usr/share/doom"
#define WAD_PATH WAD_DIR "/doom1.wad"

// WHERE SAVEGAMES GO, and why this is done with chdir() of all things.
//
// doomgeneric's `GetDefaultConfigDir()` returns "." unconditionally --
// there is no `-savedir` or `-configdir` parm to pass, and d_main.c
// calls `M_SetConfigDir(NULL)` before anything here could intervene. So
// the CWD is the only lever, and a WM-spawned process inherits "/":
// without this Doom creates `/.savegame/` in the filesystem root, which
// is where tools/check_layout.py found it.
//
// `/var/games` is the FHS location for exactly this (mutable game
// state), and it is what this repo's own `/var` row already describes.
// Doom then makes `.savegame/<iwad>/` underneath, which is upstream's
// structure and not ours to tidy -- docs/filesystem-layout.md documents
// it as what it is.
#define SAVE_DIR "/var/games/doom"

struct doom_state {
    struct uapp *app;
    int started;        // doomgeneric_Create() has run
    int failed;         // no WAD, or it would not load
    int frames;
    unsigned long long last_report_ms;
};

static struct doom_state g_st;

// --- the frame ---------------------------------------------------------
//
// doomgeneric calls this from inside its tick when a frame is finished.
// It does NOT draw: it marks the window dirty and lets the toolkit's
// loop paint once before it next blocks. Drawing here would be drawing
// outside on_draw, which is the one thing docs/gui-guidelines.md is
// unambiguous about -- the WM sets the clip around on_draw and nowhere
// else.
// A frame every FPS_REPORT_FRAMES gets a line saying how fast the last
// batch actually ran. Doom targets 35Hz and sleeps to hold it, so this
// answers a question nothing else here can: whether the emulator is
// keeping up, or whether the game is running slower than it thinks.
// Every automated test in this repo runs TCG, where that is a real
// question (CLAUDE.md), and tools/doom_test.py asserts on this line.
#define FPS_REPORT_FRAMES 175   // ~5s at Doom's own 35Hz

static void on_frame_ready(void *ctx) {
    struct doom_state *st = ctx;
    st->frames++;

    if (st->frames % FPS_REPORT_FRAMES == 0) {
        unsigned long long now = sys_monotonic_ns() / 1000000ULL;
        if (st->last_report_ms) {
            unsigned long long ms = now - st->last_report_ms;
            // Integer tenths rather than a float: this is a log line,
            // and printf_float is a whole page of code to pull in for
            // one decimal place.
            unsigned long fps10 = ms ? (unsigned long)(FPS_REPORT_FRAMES * 10000ULL / ms) : 0;
            ulogf("doom: %d frames, %lu.%lu fps over the last %llu ms",
                  st->frames, fps10 / 10, fps10 % 10, ms);
        }
        st->last_report_ms = now;
    }
    uapp_redraw(st->app);
    // Flushed rather than left for the loop, because the tick that
    // produced this frame is about to run the NEXT one without ever
    // returning to the event loop -- doomgeneric's timing lives inside
    // doomgeneric_Tick(). Without this the window would update only when
    // something else happened to wake the loop.
    uapp_flush(st->app);
}

// --- ASPECT-CORRECT SCALING --------------------------------------------
//
// **DOOM'S PIXELS ARE NOT SQUARE.** 320x200 was displayed on a 4:3
// screen, so each pixel is 20% taller than it is wide, and everything
// from the sprite art to the automap's circles was drawn for that. A
// 640x400 window shows a 16:10 image: geometrically "unscaled", and
// visually squashed -- which is why every modern source port ships an
// "aspect ratio correction" option and why Chocolate Doom has it on by
// default.
//
// So the image is presented as 4:3 at whatever size fits, centred, with
// black bars on whichever axis is left over. The default window is
// 640x480 rather than 640x400 precisely so that the common case is
// aspect-correct AND fills the window exactly -- a 640x400 default would
// have had to pillarbox to 533x400 to be correct, which looks like a
// bug.
//
// The scale is nearest-neighbour and NOT restricted to integer factors.
// Integer-only would be the right call for pixel art shown at 1:1, but
// the correction is 1.2x vertically to begin with, so there is no
// integer factor to preserve -- and refusing to fill a maximized window
// in order to protect a purity that the 4:3 stretch already gives up
// would be the wrong trade.
#define DOOM_ASPECT_W 4
#define DOOM_ASPECT_H 3

// One destination row, and the source column each destination column
// reads from. The x map is rebuilt only when the width changes, which
// turns a divide per pixel into a lookup per pixel -- worth it at
// 1280x960 and 35 frames a second on an emulated CPU.
//
// Static rather than on the stack: USERLAND_CFLAGS caps a frame at 2048
// bytes, and either of these is several times that on its own.
static uint32_t g_row[WIN_CLIENT_MAX_W];
static int g_xmap[WIN_CLIENT_MAX_W];
static int g_xmap_for_w = -1;

// The largest 4:3 rect that fits `cw` x `ch`, centred.
static void fit_rect(int cw, int ch, int *ox, int *oy, int *ow, int *oh) {
    int w = cw;
    int h = w * DOOM_ASPECT_H / DOOM_ASPECT_W;
    if (h > ch) {
        h = ch;
        w = h * DOOM_ASPECT_W / DOOM_ASPECT_H;
    }
    if (w > WIN_CLIENT_MAX_W) w = WIN_CLIENT_MAX_W;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *ow = w;
    *oh = h;
    *ox = (cw - w) / 2;
    *oy = (ch - h) / 2;
}

static void draw_scaled(struct ugfx_surface *s, const uint32_t *px,
                         int dx, int dy, int dw, int dh) {
    if (dw != g_xmap_for_w) {
        for (int x = 0; x < dw; x++) g_xmap[x] = x * DOOM_RESX / dw;
        g_xmap_for_w = dw;
    }
    for (int y = 0; y < dh; y++) {
        const uint32_t *src = px + (size_t)(y * DOOM_RESY / dh) * DOOM_RESX;
        for (int x = 0; x < dw; x++) g_row[x] = src[g_xmap[x]];
        // A row at a time rather than a whole scaled frame: a full one
        // would be WIN_CLIENT_MAX_W * _MAX_H * 4 = 8 MB of buffer to
        // hold a copy of something that is about to be copied again.
        ugfx_blit(s, dx, dy + y, dw, 1, g_row, dw);
    }
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct doom_state *st = uapp_state(a);
    struct ugfx_surface *s = uapp_surface(d);

    const uint32_t *px = dg_frame_pixels();
    if (!px) {
        // Before the first frame, or after a failed start. Say why, in
        // the window, rather than showing black: a black window is
        // indistinguishable from a crashed client, and the commonest
        // reason to be here is a missing WAD, which is a thing the
        // person can fix.
        ugfx_fill(s, ugfx_rgb(0, 0, 0));
        int y = uapp_height(a) / 2 - ugfx_char_h() * 2;
        const char *lines[] = {
            st->failed ? "No IWAD found." : "Loading...",
            st->failed ? "Put doom1.wad in " WAD_DIR : "",
            st->failed ? "and start DOOM again." : "",
        };
        for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            if (!lines[i][0]) continue;
            int w = ugfx_text_width(lines[i]);
            ugfx_draw_string(s, (uapp_width(a) - w) / 2, y,
                              lines[i], ugfx_rgb(200, 40, 40), UGFX_TRANSPARENT);
            y += ugfx_char_h() * 3 / 2;
        }
        return;
    }

    // NO PIXEL CONVERSION, EVER. doomgeneric's rgba8888 mode packs
    // 0x00RRGGBB (i_video.c's red at offset 16, green 8, blue 0), which
    // is exactly what ugfx_blit() takes -- so the palette lookup and
    // doomgeneric's own 2x happen inside the game, on the buffer it
    // already owns, and what is left here is at most a stretch.
    int cw = uapp_width(a), ch = uapp_height(a);
    int dx, dy, dw, dh;
    fit_rect(cw, ch, &dx, &dy, &dw, &dh);

    // THE BARS FIRST, AND ONLY THE BARS. Filling the whole surface and
    // then drawing over it would repaint every pixel twice at 35 frames
    // a second; these four rects are empty in the common case, because
    // the default window is exactly 4:3.
    uint32_t bg = ugfx_rgb(0, 0, 0);
    if (dy > 0)           ugfx_fill_rect(s, 0, 0, cw, dy, bg);
    if (dy + dh < ch)     ugfx_fill_rect(s, 0, dy + dh, cw, ch - dy - dh, bg);
    if (dx > 0)           ugfx_fill_rect(s, 0, dy, dx, dh, bg);
    if (dx + dw < cw)     ugfx_fill_rect(s, dx + dw, dy, cw - dx - dw, dh, bg);

    if (dw == DOOM_RESX && dh == DOOM_RESY) {
        // The unscaled case, kept as a straight copy rather than left to
        // the general path: it is one blit against dh of them, and it is
        // what a window sized 640x400 by hand gets.
        ugfx_blit(s, dx, dy, DOOM_RESX, DOOM_RESY, px, DOOM_RESX);
    } else {
        draw_scaled(s, px, dx, dy, dw, dh);
    }
}

// --- input --------------------------------------------------------------
//
// Both edges, straight into the backend's queue. No filtering: Doom
// keeps its own `gamekeydown[]` and wants every transition, and deciding
// here which ones matter would be re-implementing its key bindings from
// the outside.
static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    dg_push_key(key, 1);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    dg_push_key(key, 0);
}

// --- the game loop ------------------------------------------------------
//
// `tick_ms` is 0, so the toolkit POLLS: on_tick runs once per pass and
// yields, which is what ui/uapp.h says an animation with no natural rate
// wants. Doom has its own rate (it sleeps inside its tick to hold 35Hz),
// so arming a TWS timer on top would be two clocks fighting.
static int on_tick(struct uapp *a) {
    struct doom_state *st = uapp_state(a);
    if (!st->started || st->failed) return 0;
    dg_tick();
    // 0: the repaint is on_frame_ready's business, and returning 1 here
    // would ask for one per tick whether a frame was produced or not.
    return 0;
}

void dg_title_changed(const char *title) {
    if (g_st.app) uapp_set_title(g_st.app, title);
}

static void on_open(struct uapp *a) {
    struct doom_state *st = uapp_state(a);
    st->app = a;
    dg_set_frame_ready(on_frame_ready, st);

    // CHECKED BEFORE STARTING, so a missing WAD is a message in a window
    // rather than doomgeneric's own I_Error taking the process down. The
    // app is the only layer that can say it politely -- I_Error exits.
    if (access(WAD_PATH, F_OK) != 0) {
        ulogf("doom: no IWAD at %s", WAD_PATH);
        st->failed = 1;
        uapp_redraw(a);
        return;
    }

    // The savegame directory, made and entered before the game starts.
    // Each level of the path in turn, because SYS_MKDIR creates ONE
    // level (POSIX's mkdir, not `mkdir -p`), and every result is ignored
    // on purpose: the interesting failure is the chdir below, and
    // "already exists" is the normal case on every boot after the first.
    mkdir("/var", 0755);
    mkdir("/var/games", 0755);
    mkdir(SAVE_DIR, 0755);
    if (chdir(SAVE_DIR) != 0) {
        // Not fatal. Doom will write its saves relative to whatever the
        // cwd is instead, which is untidy rather than broken -- and
        // refusing to start a game because a savegame directory could
        // not be made would be the wrong trade.
        ulogf("doom: could not enter %s -- saves will land in the cwd", SAVE_DIR);
    }

    // argv, as Doom expects it. `-iwad <path>` rather than letting
    // d_iwad.c search: see WAD_PATH above.
    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char arg2[] = WAD_PATH;
    static char *argv[] = { arg0, arg1, arg2, 0 };

    ulogf("doom: starting with %s", WAD_PATH);
    dg_start(3, argv);
    st->started = 1;
    ulog("doom: ready");
}

int main(void) {
    struct uapp_desc desc = {
        .title  = "DOOM",
        .app_id = "doom",
        // One copy. Two Dooms would be two games competing for the
        // keyboard and twice the CPU, and neither would be playable --
        // the case ui/uapp.h's UAPP_SINGLE_INSTANCE exists for.
        // RESIZABLE, so the WM will maximize it. The content is
        // letterboxed to 4:3 at whatever size it gets, so there is no
        // size at which the game looks wrong -- which is what makes
        // opting in safe with no on_resize at all.
        .flags  = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE,
        // 640x480, not 640x400: see DOOM_ASPECT_W above. The default
        // window is the aspect-corrected size, so it fills exactly.
        .w      = DOOM_RESX,
        .h      = DOOM_RESX * DOOM_ASPECT_H / DOOM_ASPECT_W,
        // Below this the status bar stops being readable. Still 4:3.
        .min_w  = 320,
        .min_h  = 240,
        .state  = &g_st,
        .on_open    = on_open,
        .on_draw    = on_draw,
        .on_key     = on_key,
        .on_key_up  = on_key_up,
        .on_tick    = on_tick,
    };
    return uapp_run(&desc);
}
