// KTESTs for the input core. See kernel/include/kernel/input.h.
//
// These need NO input hardware beyond what every boot has: they drive
// input_report_*() directly, which is the contract a new device driver
// codes against. That is deliberate -- the virtio and PS/2 drivers are
// each testable only on a machine that has one, while the vocabulary
// they share is testable everywhere, and the vocabulary is where a
// mistranslation would silently change what a key means.
//
// THEY TOUCH LIVE STATE, so each establishes its own preconditions:
// injecting a key pushes into the same ring the console and the
// compositor read, and moving the pointer moves the real one. Both are
// restored, and the injection is done with preemption disabled so that
// win_input_poll() cannot consume the key between the report and the
// read (it runs from scheduler_idle(), i.e. potentially between any two
// instructions here).
#include "input.h"
#include "keyboard.h"
#include "keyboard_layout.h" // the parity check asks the LAYOUT what it maps
#include "kfmt.h"            // klog_printf -- name the unreachable key
#include "mouse.h"
#include "scheduler.h"
#include "ktest.h"

// evdev keycodes for the keys used below. Named here rather than in
// input.h because input.h carries only what a DRIVER reports; these are
// test fixtures.
#define EVDEV_A 30      // 'a' -- AT set 1 make code 0x1E, identically
#define EVDEV_1 2       // '1'
#define EVDEV_UNKNOWN 200  // a media key: real, and nothing here consumes it

KTEST("input", "an evdev keycode becomes the character the layout says") {
    // Drain anything already queued, so what is read below is what was
    // written above it rather than somebody else's keystroke.
    scheduler_preempt_disable();
    uint8_t mods = 0;
    while (keyboard_try_getchar_mods(&mods) != -1) { }

    // A keycode at or below 83 IS the AT set-1 make code -- that is the
    // identity the translation relies on, so a test that only used
    // extended keys would not notice it breaking.
    input_report_key(EVDEV_A, 1);
    input_report_key(EVDEV_A, 0);
    int c = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();
    KTEST_ASSERT_EQ(c, 'a'); // an evdev 'a' did not arrive as 'a'

    scheduler_preempt_disable();
    input_report_key(EVDEV_1, 1);
    input_report_key(EVDEV_1, 0);
    c = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();
    KTEST_ASSERT_EQ(c, '1');
}

KTEST("input", "an extended keycode arrives as one key, not as two") {
    scheduler_preempt_disable();
    uint8_t mods = 0;
    while (keyboard_try_getchar_mods(&mods) != -1) { }

    // The extended block is where the identity STOPS: evdev 103 is UP,
    // whose wire form is the two bytes 0xE0 0x48. If the translation
    // fed only the second byte, this would arrive as whatever plain
    // 0x48 means; if it fed them as two keys, there would be a spare
    // one left in the ring.
    input_report_key(INPUT_KEY_UP, 1);
    int c = keyboard_try_getchar_mods(&mods);
    int extra = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(c, KEY_ARROW_UP);
    KTEST_ASSERT_EQ(extra, -1); // the 0xE0 prefix leaked into the ring as its own key
}

KTEST("input", "a keycode nothing here maps is dropped, not guessed") {
    scheduler_preempt_disable();
    uint8_t mods = 0;
    while (keyboard_try_getchar_mods(&mods) != -1) { }

    input_report_key(EVDEV_UNKNOWN, 1);
    input_report_key(EVDEV_UNKNOWN, 0);
    int c = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();

    // A real keyboard reports many keys this kernel has no name for.
    // Guessing (say, by feeding the low byte as a scancode) would make
    // a media key type a letter.
    KTEST_ASSERT_EQ(c, -1);
}

KTEST("input", "an absolute report lands where the arithmetic says") {
    // The POINTER's bounds, not the display's. They are usually the
    // same once a compositor has set them, and they are not the same on
    // a boot where nothing has -- which is exactly what this test found
    // when it asked gfx_width() instead.
    int w = 0, h = 0;
    mouse_get_bounds(&w, &h);
    if (w <= 1 || h <= 1) KTEST_SKIP("the pointer has no usable bounds yet");

    int save_x = 0, save_y = 0;
    uint8_t buttons = 0;
    mouse_get_state(&save_x, &save_y, &buttons);

    // A tablet's range is its own; the scaling to the screen is the
    // core's job. Using a range that is NOT the screen size is the
    // point -- a driver that passed the position through unscaled would
    // pass this test if the two happened to match.
    const int RANGE = 32767;
    input_report_abs(RANGE / 2, RANGE / 4, RANGE, RANGE);

    int x = 0, y = 0;
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, (RANGE / 2) * (w - 1) / RANGE);
    KTEST_ASSERT_EQ(y, (RANGE / 4) * (h - 1) / RANGE);

    // The extremes, because clamping is where an off-by-one shows up:
    // full range must be the LAST pixel, not one past it.
    input_report_abs(RANGE, RANGE, RANGE, RANGE);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, w - 1);
    KTEST_ASSERT_EQ(y, h - 1);

    input_report_abs(0, 0, RANGE, RANGE);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, 0);
    KTEST_ASSERT_EQ(y, 0);

    // A device with no range at all must be ignored rather than divided
    // by -- and ignoring means the pointer does not move.
    input_report_abs(100, 100, 0, 0);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, 0);
    KTEST_ASSERT_EQ(y, 0);

    // Put the pointer back where it was found: this moved the REAL one,
    // and a desktop is very likely running.
    input_report_abs(save_x, save_y, w - 1, h - 1);
}

KTEST("input", "buttons are a mask, so one button does not clear another") {
    int x = 0, y = 0;
    uint8_t before = 0;
    mouse_get_state(&x, &y, &before);

    input_report_buttons(0x1);
    uint8_t now = 0;
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0x1);

    input_report_buttons(0x3);   // left AND right held
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0x3);

    // Bits above the three real buttons are dropped rather than stored:
    // consumers test bit0/1/2 and a stray high bit would make
    // "buttons != 0" true forever.
    input_report_buttons(0xF0);
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0);

    input_report_buttons(before);
}

KTEST("input", "the PS/2 pair registered itself with the core") {
    // The registry is what makes a second kind of input device
    // possible; if the built-in pair stopped appearing in it, a `lsdev`
    // listing would still look plausible while the seam had rotted.
    KTEST_ASSERT(input_source_count() >= 2);

    int saw_keyboard = 0, saw_pointer = 0;
    for (int i = 0; i < input_source_count(); i++) {
        const struct input_source *src = input_source_at(i);
        KTEST_ASSERT(src != 0);
        KTEST_ASSERT(src->name != 0);
        if (src->caps & INPUT_CAP_KEYS) saw_keyboard = 1;
        if (src->caps & (INPUT_CAP_REL | INPUT_CAP_ABS)) saw_pointer = 1;
    }
    KTEST_ASSERT(saw_keyboard);
    KTEST_ASSERT(saw_pointer);

    // Out of range is NULL rather than a wild pointer.
    KTEST_ASSERT_EQ((const void *)input_source_at(-1), (const void *)0);
    KTEST_ASSERT_EQ((const void *)input_source_at(input_source_count()), (const void *)0);
}

// --- EVERY KEY MUST WORK WHATEVER REPORTED IT ------------------------
//
// The property the input core exists for: which driver a key came from
// is not supposed to be observable. PS/2 hands keyboard.c an AT set-1
// scancode directly; virtio-input hands input_report_key() an evdev
// keycode, which this file translates to the same scancode. If the
// translation has a hole, a key works on one machine and silently does
// nothing on another -- which is not a "some keys are unsupported"
// situation, it is the same keyboard behaving differently for reasons
// the user cannot see.
//
// One hole existed and this is the check that would have caught it:
// KEY_102ND, the extra key an ISO keyboard has between Left Shift and
// Z, which carries `|` on every Nordic layout. It sat just past the
// direct range and was not in the table, so `cat x | grep y` could be
// typed on a PS/2 boot and NOT on an `INPUT=virtio` one.

KTEST("input", "every scancode the layout maps is reachable from a keycode") {
    // Build the reverse map once: keycode -> scancode, over every
    // keycode a device could plausibly report. 255 is evdev's own
    // KEY_MAX for the range that matters here; anything above it is
    // media and consumer keys with no character.
    int reachable[128];
    for (int i = 0; i < 128; i++) reachable[i] = 0;
    for (uint16_t kc = 1; kc <= 255; kc++) {
        uint8_t sc; int prefixed;
        if (!input_keycode_to_scancode(kc, &sc, &prefixed)) continue;
        // A PREFIXED code is a DIFFERENT key from the bare one -- 0xE0
        // 0x35 is the keypad slash, not the scancode 0x35 the layout
        // maps. Only unprefixed codes can satisfy a layout entry, and
        // counting them together is how this check would have passed
        // while the bug was present.
        if (prefixed) continue;
        if (sc < 128) reachable[sc] = 1;
    }

    // Every scancode the ACTIVE layout gives a character to, at any
    // level. Asked of the layout rather than listed here, so this covers
    // whichever layout the machine booted with and gains new keys when
    // a layout does.
    int missing = -1, missing_count = 0;
    for (int sc = 1; sc < 128; sc++) {
        int mapped = keyboard_layout_translate((uint8_t)sc, 0, 0) != 0
                  || keyboard_layout_translate((uint8_t)sc, 1, 0) != 0
                  || keyboard_layout_translate((uint8_t)sc, 0, 1) != 0;
        if (!mapped || reachable[sc]) continue;
        if (missing < 0) missing = sc;
        missing_count++;
    }
    // Named rather than counted: "3 unreachable" sends the next reader
    // looking, "0x56" tells them which key.
    if (missing >= 0)
        klog_printf("input: scancode 0x%x maps a character no keycode reaches "
                    "(%d in total)\n", missing, missing_count);
    KTEST_ASSERT_EQ(missing_count, 0);
}

KTEST("input", "the ISO key that carries `|` translates, and is not prefixed") {
    // The specific regression, pinned by name. The check above is the
    // general property and would catch this too -- but only while some
    // layout maps 0x56, and a US-only boot does not. This one holds
    // whatever is loaded.
    uint8_t sc = 0;
    int prefixed = 1;
    KTEST_ASSERT(input_keycode_to_scancode(INPUT_KEY_102ND, &sc, &prefixed));
    KTEST_ASSERT_EQ((int)sc, 0x56);
    // NOT prefixed: feeding 0xE0 0x56 would make keyboard.c read it as
    // an extended key of that number, which is nothing -- so the key
    // would still vanish, just for a second reason.
    KTEST_ASSERT_EQ(prefixed, 0);

    // ...and a keycode this kernel has no name for is still refused,
    // so the check above cannot pass by the translation accepting
    // everything.
    KTEST_ASSERT(!input_keycode_to_scancode(700, &sc, &prefixed));
}
