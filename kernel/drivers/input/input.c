// The input core. See kernel/include/kernel/input.h for the design.
//
// This file is deliberately thin: it owns the SOURCE REGISTRY and the
// translation from the canonical vocabulary into the two state machines
// that already exist (keyboard.c's key ring, mouse.c's pointer). It does
// not own state of its own, because a third place to keep "where is the
// pointer" would be a third place for it to be wrong.
#include "input.h"
#include "keyboard.h"
#include "mouse.h"
#include "klog.h"
#include "kfmt.h"

// Small and fixed, like display.c's driver table: PS/2 keyboard, PS/2
// mouse, and however many virtio input devices are attached. A linked
// list would need each source to carry a mutable `next`, which would
// stop them being `const`.
#define MAX_INPUT_SOURCES 8

static const struct input_source *g_sources[MAX_INPUT_SOURCES];
static int g_count;

void input_register_source(const struct input_source *src) {
    if (!src || !src->name || g_count >= MAX_INPUT_SOURCES) return;
    g_sources[g_count++] = src;
    klog_printf("input: %s registered (%s%s%s%s)\n", src->name,
                (src->caps & INPUT_CAP_KEYS)  ? "keys " : "",
                (src->caps & INPUT_CAP_REL)   ? "rel "  : "",
                (src->caps & INPUT_CAP_ABS)   ? "abs "  : "",
                (src->caps & INPUT_CAP_WHEEL) ? "wheel" : "");
}

int input_source_count(void) { return g_count; }

const struct input_source *input_source_at(int index) {
    if (index < 0 || index >= g_count) return 0;
    return g_sources[index];
}

void input_poll_sources(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_sources[i]->poll) g_sources[i]->poll();
    }
}

// --- keycode -> AT set 1 ----------------------------------------------
//
// The seam's one piece of legacy, and it is small on purpose. Codes up
// to INPUT_KEY_EVDEV_DIRECT_MAX ARE the set-1 make codes -- that is not a
// coincidence, it is where evdev's numbering came from -- so only the
// keys that live behind an 0xE0 prefix need a table.
//
// It disappears when /etc/kbs is re-keyed to evdev codes; until then a
// non-PS/2 keyboard translates once, here, rather than every driver
// carrying its own idea of the mapping.
struct extended_key { uint16_t keycode; uint8_t scancode; };

static const struct extended_key EXTENDED[] = {
    { INPUT_KEY_KPENTER,   0x1C }, { INPUT_KEY_RIGHTCTRL, 0x1D },
    { INPUT_KEY_KPSLASH,   0x35 }, { INPUT_KEY_RIGHTALT,  0x38 },
    { INPUT_KEY_HOME,      0x47 }, { INPUT_KEY_UP,        0x48 },
    { INPUT_KEY_PAGEUP,    0x49 }, { INPUT_KEY_LEFT,      0x4B },
    { INPUT_KEY_RIGHT,     0x4D }, { INPUT_KEY_END,       0x4F },
    { INPUT_KEY_DOWN,      0x50 }, { INPUT_KEY_PAGEDOWN,  0x51 },
    { INPUT_KEY_INSERT,    0x52 }, { INPUT_KEY_DELETE,    0x53 },
    { INPUT_KEY_LEFTMETA,  0x5B }, { INPUT_KEY_RIGHTMETA, 0x5C },
    { INPUT_KEY_COMPOSE,   0x5D },
};
#define EXTENDED_COUNT ((int)(sizeof EXTENDED / sizeof EXTENDED[0]))

void input_report_key(uint16_t keycode, int down) {
    if (!keycode) return;

    if (keycode <= INPUT_KEY_EVDEV_DIRECT_MAX) {
        keyboard_feed_byte((uint8_t)(down ? keycode : (keycode | 0x80)));
        return;
    }

    for (int i = 0; i < EXTENDED_COUNT; i++) {
        if (EXTENDED[i].keycode != keycode) continue;
        // The prefix and the code are two separate feeds because that is
        // exactly what the wire looks like on PS/2, and keyboard.c's
        // state machine is written against the wire. Splitting it here
        // means that state machine needs no notion of "an injected key".
        keyboard_feed_byte(0xE0);
        keyboard_feed_byte((uint8_t)(down ? EXTENDED[i].scancode
                                          : (EXTENDED[i].scancode | 0x80)));
        return;
    }
    // Anything else is a key this kernel has no name for. Dropped
    // silently: a keyboard reports many keys nothing here consumes
    // (media keys, a second numeric block), and logging each one would
    // turn a harmless keypress into a flood.
}

void input_report_rel(int dx, int dy) { mouse_feed_rel(dx, dy); }

void input_report_abs(int x, int y, int max_x, int max_y) {
    mouse_feed_abs(x, y, max_x, max_y);
}

void input_report_buttons(uint8_t mask) { mouse_feed_buttons(mask); }

void input_report_wheel(int notches) { mouse_feed_wheel(notches); }
