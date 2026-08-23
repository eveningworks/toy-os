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
//   DG_GetKey        a queue the app fills from on_key/on_key_up.
//
// The fifth is the one this whole port waited on. Its signature is
// `int DG_GetKey(int *pressed, unsigned char *key)` -- it asks for an
// EDGE, and a press-only OS cannot answer it. WIN_EV_KEY_UP had to exist
// first.
#include "dg_toyos.h"

#include "doomgeneric.h"
#include "doomkeys.h"

#include "rt/sys.h"

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
// Filled by the app's on_key/on_key_up, drained by DG_GetKey. A QUEUE and
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

void dg_push_key(int code, int down) {
    unsigned next = (keyq_head + 1) % KEYQ_MAX;
    if (next == keyq_tail) keyq_tail = (keyq_tail + 1) % KEYQ_MAX;
    keyq[keyq_head].code = code;
    keyq[keyq_head].down = down ? 1 : 0;
    keyq_head = next;
}

// toy-os's code -> Doom's. Doom's key space is mostly ASCII, which is
// why the default case is a pass-through and why only the specials need
// a table at all.
//
// **THE THREE MODIFIERS ARE THE POINT.** Doom's stock bindings are fire
// on Ctrl, run on Shift and strafe on Alt (m_controls.c's key_fire =
// KEY_RCTRL, key_speed = KEY_RSHIFT, key_strafe = KEY_RALT). None of
// them produces a character, so before toy-os delivered modifier keys as
// keys they reached a client by no path at all -- three of the five
// controls a player actually uses.
static unsigned char to_doom_key(int code) {
    switch (code) { // dispatch-ok: bounded by the key codes api/keyboard.h defines
    case TOYKEY_ARROW_UP:    return KEY_UPARROW;
    case TOYKEY_ARROW_DOWN:  return KEY_DOWNARROW;
    case TOYKEY_ARROW_LEFT:  return KEY_LEFTARROW;
    case TOYKEY_ARROW_RIGHT: return KEY_RIGHTARROW;
    case TOYKEY_CTRL:        return KEY_RCTRL;   // fire
    case TOYKEY_SHIFT:       return KEY_RSHIFT;  // run
    case TOYKEY_ALT:         return KEY_RALT;    // strafe
    // AltGr is deliberately Alt here too. On a Nordic layout it is the
    // key under the right thumb, and a player who reaches for "the other
    // Alt" to strafe should get strafe rather than nothing. toy-os keeps
    // them distinct because a LAYOUT needs to (it picks a third
    // character); a game does not.
    case TOYKEY_ALTGR:       return KEY_RALT;
    case TOYKEY_F2:          return KEY_F2;
    case TOYKEY_F3:          return KEY_F3;
    case TOYKEY_F4:          return KEY_F4;
    case TOYKEY_F10:         return KEY_F10;
    case TOYKEY_HOME:        return KEY_HOME;
    case TOYKEY_END:         return KEY_END;
    case TOYKEY_PAGE_UP:     return KEY_PGUP;
    case TOYKEY_PAGE_DOWN:   return KEY_PGDN;
    case TOYKEY_DELETE:      return KEY_DEL;
    // 0x08 is what this keyboard sends for Backspace (Ctrl-H's control
    // code, terminal-style); Doom wants 0x7f. Without this the menu's
    // "erase a character" key does nothing.
    case 0x08:               return KEY_BACKSPACE;
    default: break;
    }
    // An ordinary character. Doom compares menu input against LOWERCASE
    // (m_menu.c tolowers before matching), and its cheat-code parser
    // does its own folding, so passing the character through as typed is
    // right for both.
    if (code >= 0 && code < 128) return (unsigned char)code;
    return 0; // nothing Doom could do with it
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
