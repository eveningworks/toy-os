// DOOM, as an ordinary ring-3 client.
//
// **THE TOY-OS SIDE OF THE SEAM.** This file includes `api/keyboard.h`
// and NEVER doomgeneric's `doomkeys.h` -- both define `KEY_F2`,
// `KEY_F3`, `KEY_F4` and `KEY_F10` with different values, and neither is
// ours to rename. `userland/backends/doom/dg_toyos.h` is the header they meet
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rt/sys.h"
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/ucrt.h"
#include "lib/uconf.h"
#include "win_proto.h"  // WIN_CLIENT_MAX_W -- the scratch row's bound
#include "backends/doom/dg_toyos.h"
#include "input_keys.h"   // INPUT_KEY_ENTER -- a key by position
// The front end -- launcher, game data, F1 sheet -- and which IWAD
// (userland/doom/). WHERE AN IWAD LIVES is one directory, not d_iwad.c's
// search path: docs/filesystem-layout.md owns DOOM_WAD_DIR.
#include "doom/doom_internal.h"

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
// Doom's argv cap. Three for `-iwad <path>` plus room for a handful of
// switches; a longer command line is truncated rather than refused,
// because the useful ones are one or two words.
#define DOOM_MAXARGS 16
static int g_argc;
static char **g_argv;

#define SAVE_DIR "/var/games/doom"

struct doom_state {
    struct uapp *app;
    int started;        // doomgeneric_Create() has run
    int failed;         // no WAD, or it would not load
    int frames;
    unsigned long long last_report_ms;
};

static struct doom_state g_st;

// --- the screen effect -------------------------------------------------
//
// The Terminal's CRT presets (ui/ucrt.h) over the picture, cycled by
// Alt+C and kept in /etc/doom.conf. A scanline closes each of DOOM's own
// rows however tall the window makes them, and the picture is scaled
// into g_pic and composed from there: a SCANOUT surface is never read.
#define DOOM_LINES (DOOM_RESY / 2)   // doomgeneric draws 320x200 doubled

static const char *const EFFECT_WORDS[] = { "off", "subtle", "classic", "curved" };
static const char *const EFFECT_MESSAGES[] = {
    "Screen effect: off", "Screen effect: Subtle", "Screen effect: Classic CRT",
    "Screen effect: Curved",
};
_Static_assert(sizeof EFFECT_WORDS / sizeof EFFECT_WORDS[0] == UCRT_PRESET_COUNT + 1,
               "one word per ucrt preset, plus off");

static int g_effect;            // 0 off, else ucrt_presets[g_effect - 1]
static struct ucrt g_crt;
static uint32_t *g_pic;         // the scaled picture, when the effect is on
static int g_pic_w, g_pic_h;
static unsigned long long g_effect_ns;   // composing time since the last FPS line
static int g_effect_frames;

static void effect_load(void) {
    char v[16];
    if (!uconf_get(DOOM_CONF, "screen_effect", v, sizeof v)) return;
    for (int i = 0; i <= UCRT_PRESET_COUNT; i++)
        if (!strcmp(v, EFFECT_WORDS[i])) g_effect = i;
}

static void effect_cycle(struct uapp *a) {
    g_effect = (g_effect + 1) % (UCRT_PRESET_COUNT + 1);
    if (!uconf_set(DOOM_CONF, "screen_effect", EFFECT_WORDS[g_effect]))
        ulogf("doom: could not save the screen effect to %s\n", DOOM_CONF);
    ulogf("doom: screen effect %s\n", EFFECT_WORDS[g_effect]);
    dg_message(EFFECT_MESSAGES[g_effect]);
    uapp_redraw(a);
}

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
            if (g_effect_frames) {
                unsigned long us = (unsigned long)(g_effect_ns / 1000 / (unsigned)g_effect_frames);
                ulogf("doom: %d frames, %lu.%lu fps over the last %llu ms, screen effect %lu.%lu ms a frame\n",
                      st->frames, fps10 / 10, fps10 % 10, ms, us / 1000, us % 1000 / 100);
            } else {
                ulogf("doom: %d frames, %lu.%lu fps over the last %llu ms\n",
                      st->frames, fps10 / 10, fps10 % 10, ms);
            }
            g_effect_ns = 0;
            g_effect_frames = 0;
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

static void scale_row(const uint32_t *px, int y, int dw, int dh, uint32_t *out) {
    if (dw != g_xmap_for_w) {
        for (int x = 0; x < dw; x++) g_xmap[x] = x * DOOM_RESX / dw;
        g_xmap_for_w = dw;
    }
    const uint32_t *src = px + (size_t)(y * DOOM_RESY / dh) * DOOM_RESX;
    for (int x = 0; x < dw; x++) out[x] = src[g_xmap[x]];
}

static void draw_scaled(struct ugfx_surface *s, const uint32_t *px,
                         int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        scale_row(px, y, dw, dh, g_row);
        // A row at a time rather than a whole scaled frame: a full one
        // would be up to WIN_CLIENT_MAX_W * _MAX_H * 4 bytes of buffer to
        // hold a copy of something that is about to be copied again.
        ugfx_blit(s, dx, dy + y, dw, 1, g_row, dw);
    }
}

// The picture through the screen effect. -1 when there is no memory for
// it, and the caller draws it plain.
static int draw_effect(struct ugfx_surface *s, const uint32_t *px,
                       int dx, int dy, int dw, int dh) {
    if (dw != g_pic_w || dh != g_pic_h) {
        free(g_pic);
        g_pic = malloc((size_t)dw * dh * 4);
        g_pic_w = g_pic ? dw : 0;
        g_pic_h = g_pic ? dh : 0;
        if (!g_pic) return -1;
    }
    for (int y = 0; y < dh; y++) scale_row(px, y, dw, dh, g_pic + (size_t)y * dw);

    unsigned long long t0 = sys_monotonic_ns();
    g_crt.look = ucrt_presets[g_effect - 1];
    g_crt.src_lines = DOOM_LINES;
    g_crt.period = dh / DOOM_LINES < 2 ? 2 : dh / DOOM_LINES;
    int rc = ucrt_apply_from(&g_crt, g_pic, dw, s, dx, dy, dw, dh);
    g_effect_ns += sys_monotonic_ns() - t0;
    g_effect_frames++;
    return rc == 0 ? 0 : -1;
}

// THE FRONT END AND THE KEY SHEET BLEND, and blending reads the
// destination -- which a SCANOUT surface must never be (ui/uapp.h). So
// they are drawn here and copied over in one write-only blit.
static uint32_t *g_off;
static int g_off_w, g_off_h;

static struct ugfx_surface *offscreen(int w, int h) {
    static struct ugfx_surface o;
    if (w != g_off_w || h != g_off_h) {
        free(g_off);
        g_off = malloc((size_t)w * h * 4);
        g_off_w = g_off ? w : 0;
        g_off_h = g_off ? h : 0;
    }
    if (!g_off) return NULL;
    o = ugfx_surface_for_pixels(g_off, w, h);
    return &o;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct doom_state *st = uapp_state(a);
    struct ugfx_surface *win = uapp_surface(d), *s = win;
    if (doom_front_up() || doom_help_up()) {
        struct ugfx_surface *o = offscreen(win->w, win->h);   // the buffer's size, not the request's
        if (o) s = o;
    }

    if (doom_front_up()) {
        doom_front_draw(a, s);
        if (s != win) ugfx_blit(win, 0, 0, g_off_w, g_off_h, g_off, g_off_w);
        return;
    }
    const uint32_t *px = dg_frame_pixels();
    if (!px) {
        // Between Start and the first frame: say so rather than show
        // black, which is indistinguishable from a crashed client.
        // Straight to the window: no sheet is drawn over this.
        ugfx_fill(win, ugfx_rgb(0, 0, 0));
        const char *t = st->failed ? "DOOM could not start." : "Loading...";
        ugfx_draw_string(win, (uapp_width(a) - ugfx_text_width(t)) / 2, uapp_height(a) / 2,
                         t, ugfx_rgb(200, 40, 40), UGFX_TRANSPARENT);
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

    if (g_effect && draw_effect(s, px, dx, dy, dw, dh) == 0) {
        // drawn through the effect
    } else if (dw == DOOM_RESX && dh == DOOM_RESY) {
        // The unscaled case, kept as a straight copy rather than left to
        // the general path: it is one blit against dh of them, and it is
        // what a window sized 640x400 by hand gets.
        ugfx_blit(s, dx, dy, DOOM_RESX, DOOM_RESY, px, DOOM_RESX);
    } else {
        draw_scaled(s, px, dx, dy, dw, dh);
    }
    doom_help_draw(a, s);   // F1's sheet over the paused game, if it is up
    if (s != win) ugfx_blit(win, 0, 0, g_off_w, g_off_h, g_off, g_off_w);
}

// --- input --------------------------------------------------------------
//
// KEYS BY POSITION (uapp.h's on_phys_key), both edges, straight into the
// backend's queue. The translated keys fold a held modifier into the code
// -- Ctrl+1 is no key at all, Shift+arrow a code of its own -- and Doom
// plays with Ctrl, Shift and Alt HELD. No filtering: Doom keeps its own
// `gamekeydown[]` and wants every transition.
//
// Alt+Enter toggles fullscreen -- a key the game does not see, because
// it is the desktop's convention (every DOOM port since the DOS days),
// not the game's binding. The toggle is taken from the translated key;
// its Enter is kept from the game on both paths. Alt+C is the other.
static int is_fullscreen_toggle(int key, unsigned mods) {
    return (key == 0x0A || key == 0x0D) && (mods & KEY_MOD_ALT);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (is_fullscreen_toggle(key, mods)) {
        uapp_set_fullscreen(a, !uapp_fullscreen(a));
        return;
    }
    if (doom_front_up()) doom_front_key(a, key, mods);
}

// F1 is the KEY SHEET's, and F1 on the sheet is DOOM's own help: the
// game's HELP1/HELP2 pages stay one key further on, never lost. While
// the sheet is up nothing else reaches the game; Esc or Enter closes it.
static void sheet_key(struct uapp *a, int keycode, int down) {
    if (!down) return;
    if (keycode == INPUT_KEY_F1) {
        doom_help_close(a);
        dg_push_key(INPUT_KEY_F1, 1);
        dg_push_key(INPUT_KEY_F1, 0);
    } else if (keycode == INPUT_KEY_ESC || keycode == INPUT_KEY_ENTER ||
               keycode == INPUT_KEY_KPENTER) {
        doom_help_close(a);
    }
}

static void on_phys_key(struct uapp *a, int keycode, int down, unsigned mods) {
    if ((keycode == INPUT_KEY_ENTER || keycode == INPUT_KEY_KPENTER) &&
        (mods & KEY_MOD_ALT))
        return;
    if (doom_front_up()) {                  // the front end reads on_key...
        doom_front_phys(keycode, down);     // ...and wants Enter's release
        return;
    }
    if (doom_help_up()) { sheet_key(a, keycode, down); return; }
    if (keycode == INPUT_KEY_F1 && !(mods & (KEY_MOD_ALT | KEY_MOD_CTRL))) {
        if (down && dg_frame_pixels()) doom_help_open(a);   // a sheet needs a game under it
        return;
    }
    // Alt+C cycles the screen effect, kept from the game as Alt+Enter is.
    // By position, both edges: on_key's translated Alt+C is not one code.
    if (keycode == INPUT_KEY_C && (mods & KEY_MOD_ALT)) {
        if (down) effect_cycle(a);
        return;
    }
    dg_push_key(keycode, down);
}

// No releases come for keys held as focus leaves (abi/win_proto.h), so
// they are released here -- or the player walks on in a window behind.
static void on_focus(struct uapp *a, int focused) {
    (void)a;
    if (!focused) dg_release_all();
}

// --- the game loop ------------------------------------------------------
//
// UAPP_POLL, so the toolkit POLLS: on_tick runs once per pass and
// yields. Doom has its own rate (it sleeps inside its tick to hold
// 35Hz), so a TWS timer on top would be two clocks fighting.
static int on_tick(struct uapp *a) {
    struct doom_state *st = uapp_state(a);
    if (!st->started || st->failed) return 0;
    dg_tick();
    // DOOM's own menu opening and closing, said once each -- the one
    // way a test can tell F1-on-the-sheet reached the game's help.
    static int menu_was;
    if (dg_menu_active() != menu_was) {
        menu_was = dg_menu_active();
        ulogf("doom: menu %s\n", menu_was ? "open" : "closed");
    }
    // 0: the repaint is on_frame_ready's business, and returning 1 here
    // would ask for one per tick whether a frame was produced or not.
    return 0;
}

void dg_title_changed(const char *title) {
    if (g_st.app) uapp_set_title(g_st.app, title);
}

static void on_press(struct uapp *a, int x, int y, unsigned mods) {
    if (doom_front_up() || doom_help_up()) doom_front_press(a, x, y, mods);
}
static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (doom_front_up() || doom_help_up()) doom_front_motion(a, x, y, buttons);
}
static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    if (doom_front_up() || doom_help_up()) doom_front_release(a, x, y, buttons);
}

void doom_start_game(struct uapp *a, const struct doom_iwad *w) {
    struct doom_state *st = uapp_state(a);
    doom_front_leave();
    if (st->started) return;
    uapp_poll_pause(a, 0);   // the game polls: it has its own 35 Hz

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
        ulogf("doom: could not enter %s -- saves will land in the cwd\n", SAVE_DIR);
    }

    // argv, as Doom expects it. `-iwad <path>` rather than letting
    // d_iwad.c search: see DOOM_WAD_DIR.
    //
    // ANYTHING THIS PROGRAM WAS GIVEN IS APPENDED, so Doom's own
    // switches work: `gui spawn /bin/wm/apps/doom -nomusic`, `-nosfx`,
    // `-warp 1 3`. That is what lets a test drive the two audio
    // subsystems apart -- with music off the recording goes from
    // continuous to bursts, which no single boot can show.
    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char wad[160];
    static char *argv[DOOM_MAXARGS];
    doom_iwad_path(w, wad, sizeof wad);
    int n = 0;
    argv[n++] = arg0;
    argv[n++] = arg1;
    argv[n++] = wad;
    for (int i = 1; i < g_argc && n < DOOM_MAXARGS - 1; i++) argv[n++] = g_argv[i];
    argv[n] = 0;

    ulogf("doom: starting with %s (%d arg(s))\n", wad, n - 3);
    st->started = 1;
    uapp_redraw(a);
    uapp_flush(a);   // "Loading..." on screen while the WAD loads
    dg_start(n, argv);
    ulog("doom: ready\n");
}

static void on_open(struct uapp *a) {
    struct doom_state *st = uapp_state(a);
    st->app = a;
    dg_set_frame_ready(on_frame_ready, st);
    ucrt_init(&g_crt);
    g_crt.bezel = ugfx_rgb(0, 0, 0);
    effect_load();

    // CHECKED BEFORE STARTING, so a missing WAD is the game-data card
    // rather than doomgeneric's own I_Error taking the process down.
    // Arguments ask for a particular start, so they skip the launcher.
    // The front end waits for events like any app; only the game polls.
    uapp_poll_pause(a, 1);
    if (doom_front_open(a, g_argc > 1)) doom_start_game(a, doom_iwad_chosen());
    uapp_redraw(a);
}

int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;

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
        // SCANOUT: the scaler stores every pixel of the content rect
        // and reads none back, so fullscreen may draw the display's own
        // buffer (docs/scanout-design.md).
        .flags  = UAPP_SINGLE_INSTANCE | UAPP_RESIZABLE | UAPP_SCANOUT | UAPP_POLL,
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
        .on_key      = on_key,
        .on_phys_key = on_phys_key,
        .on_focus    = on_focus,
        .on_tick    = on_tick,
        .on_press   = on_press,
        .on_motion  = on_motion,
        .on_release = on_release,
        .on_user    = doom_front_user,
    };
    return uapp_run(&desc);
}
