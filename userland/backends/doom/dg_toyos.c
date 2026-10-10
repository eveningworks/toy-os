// doomgeneric's platform layer, for toy-os.
//
// **THE DOOM SIDE OF THE SEAM.** This file includes doomgeneric's
// headers and NEVER `api/keyboard.h` -- see dg_toyos.h for why the two
// cannot meet in one translation unit. Everything toy-os-shaped that
// this needs (a clock, a sleep) comes through `rt/sys.h`, which has no
// `KEY_*` macros of its own.
//
// doomgeneric reduces a Doom port to five functions. What each one costs
// here:
//
//   DG_Init          nothing -- the window already exists, because the
//                    app created it before starting the game.
//   DG_DrawFrame     a callback into the app, which repaints. This
//                    layer never touches a surface: only the app has
//                    one, and reaching for it from inside a game tick
//                    would be drawing outside on_draw.
//   DG_SleepMs       sys_sleep_ms.
//   DG_GetTicksMs    sys_monotonic_ns / 1e6.
//   DG_GetKey        a queue the app fills from on_phys_key.
//
// The fifth is the one this whole port waited on. Its signature is
// `int DG_GetKey(int *pressed, unsigned char *key)` -- it asks for an
// EDGE, and a press-only OS cannot answer it. WIN_EV_KEY_UP had to exist
// first.
#include "dg_toyos.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "input_keys.h"   // key POSITIONS -- no KEY_* of its own to collide

#include "doomstat.h"      // players[], consoleplayer -- for dg_message()
#include "rt/sys.h"

extern boolean message_dontfuckwithme;   // hu_stuff.c's, in no header

// --- the frame ---------------------------------------------------------

static void (*g_frame_cb)(void *ctx);
static void *g_frame_ctx;

void dg_set_frame_ready(void (*cb)(void *ctx), void *ctx) {
    g_frame_cb = cb;
    g_frame_ctx = ctx;
}

const uint32_t *dg_frame_pixels(void) { return (const uint32_t *)DG_ScreenBuffer; }

// --- the key queue ------------------------------------------------------
//
// Filled by the app's on_phys_key, drained by DG_GetKey. A QUEUE and
// not a "currently held" bitmap, because Doom wants the transitions: it
// keeps its own held state (`gamekeydown[]`) and a missed edge desyncs
// it -- a missed release being the one that leaves the player walking.
//
// 64 is far past what a frame can produce at human typing rates. On
// overflow the OLDEST is dropped, matching the two queues below this in
// the stack (the driver's and the compositor's) so that a full pipeline
// behaves one way rather than three.
#define KEYQ_MAX 64
static struct dg_key_event keyq[KEYQ_MAX];
static unsigned keyq_head, keyq_tail;

// Which positions are down, so a focus loss can release exactly those.
static uint8_t g_held[128 / 8];

void dg_push_key(int keycode, int down) {
    if (keycode >= 0 && keycode < 128) {
        if (down) g_held[keycode >> 3] |= (uint8_t)(1u << (keycode & 7));
        else      g_held[keycode >> 3] &= (uint8_t)~(1u << (keycode & 7));
    }
    unsigned next = (keyq_head + 1) % KEYQ_MAX;
    if (next == keyq_tail) keyq_tail = (keyq_tail + 1) % KEYQ_MAX;
    keyq[keyq_head].code = keycode;
    keyq[keyq_head].down = down ? 1 : 0;
    keyq_head = next;
}

void dg_release_all(void) {
    for (int k = 0; k < 128; k++)
        if (g_held[k >> 3] & (1u << (k & 7))) dg_push_key(k, 0);
}

// A key POSITION (abi/input_keys.h) -> Doom's key. Doom's key space is
// ASCII for the printable range, so a letter's position maps to what it
// prints on a US keyboard -- the layout a WASD-style binding and Doom's
// own defaults assume, whatever the user's layout prints there.
//
// **THE THREE MODIFIERS ARE THE POINT.** Doom's stock bindings are fire
// on Ctrl, run on Shift and strafe on Alt, and a modifier held for one of
// them changes nothing about the other keys reported here -- the reason
// this path exists (abi/win_proto.h's WIN_EV_KEY_PHYS). The translated
// codes turned Ctrl+1 into nothing and Alt+Space into an Esc press.
//
// **FIRE AND USE ARE ABSTRACT CODES, NOT THE KEYS THEY LOOK LIKE.**
// m_controls.c binds `key_fire = KEY_FIRE` and `key_use = KEY_USE`,
// which no physical key produces; the platform maps onto them, as
// doomgeneric_sdl.c does. `key_speed` and `key_strafe` really are
// KEY_RSHIFT and KEY_RALT. AltGr strafes too: a game has no use for a
// third-level character.
//
// Pause reports a press with NO release on PS/2 (keyboard.c); Doom only
// tests the press. A key with no entry is 0 and is skipped by the caller.
static const unsigned char PHYS_TO_DOOM[128] = {
    [INPUT_KEY_ESC] = KEY_ESCAPE,
    [INPUT_KEY_1] = '1', [INPUT_KEY_2] = '2', [INPUT_KEY_3] = '3', [INPUT_KEY_4] = '4',
    [INPUT_KEY_5] = '5', [INPUT_KEY_6] = '6', [INPUT_KEY_7] = '7', [INPUT_KEY_8] = '8',
    [INPUT_KEY_9] = '9', [INPUT_KEY_0] = '0',
    [INPUT_KEY_MINUS] = KEY_MINUS, [INPUT_KEY_EQUAL] = KEY_EQUALS,
    [INPUT_KEY_BACKSPACE] = KEY_BACKSPACE, [INPUT_KEY_TAB] = KEY_TAB,
    [INPUT_KEY_Q] = 'q', [INPUT_KEY_W] = 'w', [INPUT_KEY_E] = 'e', [INPUT_KEY_R] = 'r',
    [INPUT_KEY_T] = 't', [INPUT_KEY_Y] = 'y', [INPUT_KEY_U] = 'u', [INPUT_KEY_I] = 'i',
    [INPUT_KEY_O] = 'o', [INPUT_KEY_P] = 'p',
    [INPUT_KEY_LEFTBRACE] = '[', [INPUT_KEY_RIGHTBRACE] = ']',
    [INPUT_KEY_ENTER] = KEY_ENTER, [INPUT_KEY_KPENTER] = KEY_ENTER,
    [INPUT_KEY_A] = 'a', [INPUT_KEY_S] = 's', [INPUT_KEY_D] = 'd', [INPUT_KEY_F] = 'f',
    [INPUT_KEY_G] = 'g', [INPUT_KEY_H] = 'h', [INPUT_KEY_J] = 'j', [INPUT_KEY_K] = 'k',
    [INPUT_KEY_L] = 'l',
    [INPUT_KEY_SEMICOLON] = ';', [INPUT_KEY_APOSTROPHE] = '\'', [INPUT_KEY_GRAVE] = '`',
    [INPUT_KEY_BACKSLASH] = '\\',
    [INPUT_KEY_Z] = 'z', [INPUT_KEY_X] = 'x', [INPUT_KEY_C] = 'c', [INPUT_KEY_V] = 'v',
    [INPUT_KEY_B] = 'b', [INPUT_KEY_N] = 'n', [INPUT_KEY_M] = 'm',
    [INPUT_KEY_COMMA] = ',', [INPUT_KEY_DOT] = '.', [INPUT_KEY_SLASH] = '/',
    [INPUT_KEY_SPACE] = KEY_USE,
    [INPUT_KEY_LEFTCTRL] = KEY_FIRE, [INPUT_KEY_RIGHTCTRL] = KEY_FIRE,
    [INPUT_KEY_LEFTSHIFT] = KEY_RSHIFT, [INPUT_KEY_RIGHTSHIFT] = KEY_RSHIFT,
    [INPUT_KEY_LEFTALT] = KEY_RALT, [INPUT_KEY_RIGHTALT] = KEY_RALT,
    [INPUT_KEY_CAPSLOCK] = KEY_CAPSLOCK,
    [INPUT_KEY_F1] = KEY_F1, [INPUT_KEY_F2] = KEY_F2, [INPUT_KEY_F3] = KEY_F3,
    [INPUT_KEY_F4] = KEY_F4, [INPUT_KEY_F5] = KEY_F5, [INPUT_KEY_F6] = KEY_F6,
    [INPUT_KEY_F7] = KEY_F7, [INPUT_KEY_F8] = KEY_F8, [INPUT_KEY_F9] = KEY_F9,
    [INPUT_KEY_F10] = KEY_F10, [INPUT_KEY_F11] = KEY_F11, [INPUT_KEY_F12] = KEY_F12,
    [INPUT_KEY_KP0] = '0', [INPUT_KEY_KP1] = '1', [INPUT_KEY_KP2] = '2', [INPUT_KEY_KP3] = '3',
    [INPUT_KEY_KP4] = '4', [INPUT_KEY_KP5] = '5', [INPUT_KEY_KP6] = '6', [INPUT_KEY_KP7] = '7',
    [INPUT_KEY_KP8] = '8', [INPUT_KEY_KP9] = '9',
    [INPUT_KEY_KPMINUS] = KEY_MINUS, [INPUT_KEY_KPPLUS] = KEY_EQUALS,
    [INPUT_KEY_UP] = KEY_UPARROW, [INPUT_KEY_DOWN] = KEY_DOWNARROW,
    [INPUT_KEY_LEFT] = KEY_LEFTARROW, [INPUT_KEY_RIGHT] = KEY_RIGHTARROW,
    [INPUT_KEY_HOME] = KEY_HOME, [INPUT_KEY_END] = KEY_END,
    [INPUT_KEY_PAGEUP] = KEY_PGUP, [INPUT_KEY_PAGEDOWN] = KEY_PGDN,
    [INPUT_KEY_INSERT] = KEY_INS, [INPUT_KEY_DELETE] = KEY_DEL,
    [INPUT_KEY_PAUSE] = KEY_PAUSE,
};

static unsigned char to_doom_key(int keycode) {
    return keycode >= 0 && keycode < 128 ? PHYS_TO_DOOM[keycode] : 0;
}

int DG_GetKey(int *pressed, unsigned char *key) {
    while (keyq_tail != keyq_head) {
        struct dg_key_event e = keyq[keyq_tail];
        keyq_tail = (keyq_tail + 1) % KEYQ_MAX;

        unsigned char k = to_doom_key(e.code);
        // A key Doom has no code for is SKIPPED, not reported as key 0:
        // 0 is a valid index into gamekeydown[] and reporting it would
        // let an unmapped key hold down a slot Doom thinks is a real
        // binding.
        if (!k) continue;

        if (pressed) *pressed = e.down;
        if (key) *key = k;
        return 1;
    }
    return 0;
}

// --- the rest of the platform layer -------------------------------------

void DG_Init(void) {
    // Nothing. The window, its buffer and the event loop all exist
    // before doomgeneric_Create() is called -- the app owns them, and
    // this layer deliberately owns nothing it would have to tear down.
}

void DG_DrawFrame(void) {
    if (g_frame_cb) g_frame_cb(g_frame_ctx);
}

void DG_SleepMs(uint32_t ms) { sys_sleep_ms((int)ms); }

uint32_t DG_GetTicksMs(void) {
    // Monotonic, never wall clock -- Doom measures INTERVALS with this,
    // and a clock that can step backwards over an NTP-shaped correction
    // would make the game run backwards for a frame. That distinction is
    // one of this kernel's own conventions (docs/conventions/kernel.md).
    return (uint32_t)(sys_monotonic_ns() / 1000000ULL);
}

void DG_SetWindowTitle(const char *title) {
    // Doom retitles its window per level. Routed through the app rather
    // than done here, because the title belongs to the window and the
    // window belongs to the app -- and TWS wants a WIN_REQ_TITLE, not a
    // string written into a buffer.
    dg_title_changed(title);
}

// --- what the app calls -------------------------------------------------

int dg_start(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    return 0;
}

void dg_tick(void) { doomgeneric_Tick(); }

extern boolean sendpause;   // g_game.c's, in no header

static int g_held_pause;    // dg_hold() asked for the pause in force

void dg_hold(int on) {
    if (on) {
        if (!paused && !sendpause && gamestate == GS_LEVEL && !menuactive &&
            !demoplayback && !netgame) {
            sendpause = true;   // what the Pause key sets: taken on the next tic
            g_held_pause = 1;
        }
        return;
    }
    if (!g_held_pause) return;
    g_held_pause = 0;
    if (sendpause) sendpause = false;   // not taken yet: withdrawn
    else if (paused) sendpause = true;
}

int dg_paused(void) { return paused; }
int dg_menu_active(void) { return menuactive; }
int dg_playing(void) {
    return gamestate == GS_LEVEL && !menuactive && !paused && !demoplayback;
}

void dg_message(const char *text) {
    players[consoleplayer].message = (char *)text;
    message_dontfuckwithme = true;
}
