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
