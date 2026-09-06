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
#include "driver.h"
#include "string.h"

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
// is not supposed to be observable.
//
// **THAT IS NOW TRUE BY CONSTRUCTION FOR EVERYTHING EXCEPT PS/2.**
// /etc/kbs is keyed on evdev keycodes, virtio-input reports evdev
// keycodes, and input_report_key() hands one straight to
// keyboard_key_event() -- there is no table in between to have a hole
// in. It used to translate evdev DOWN into AT set-1 scancodes, and that
// table did have a hole: KEY_102ND, the ISO key carrying `|` on every
// Nordic layout, so a pipeline could be typed on PS/2 and not on
// virtio-input.
//
// What remains is the LEGACY direction -- the PS/2 wire's set-1 bytes
// translated up into keycodes, inside the PS/2 driver, exactly where
// Linux keeps it. These check that one, so a hole there fails the build
// instead of quietly disabling a key on one machine.

KTEST("input", "every keycode the layout maps is reachable from the PS/2 wire") {
    // Build the forward map once: every wire byte, prefixed and not,
    // to the keycode it produces.
    // STATIC: 256 ints is a kilobyte, against the kernel's 1024-byte
    // frame budget and a 16 KiB stack with one guard page below it. A
    // KTEST is single-threaded and runs once, so .bss is the right home.
    static int reachable[256];
    for (int i = 0; i < 256; i++) reachable[i] = 0;
    for (int ext = 0; ext <= 1; ext++) {
        for (int sc = 1; sc < 128; sc++) {
            uint16_t kc = 0;
            if (!keyboard_wire_keycode((uint8_t)sc, ext, &kc)) continue;
            if (kc < 256) reachable[kc] = 1;
        }
    }

    // Every keycode the ACTIVE layout gives a character to, at any
    // level. Asked of the layout rather than listed here, so this covers
    // whichever one the machine booted with and gains new keys when a
    // layout does.
    int missing = -1, missing_count = 0;
    for (int kc = 1; kc < 256; kc++) {
        int mapped = keyboard_layout_translate((uint16_t)kc, 0, 0) != 0
                  || keyboard_layout_translate((uint16_t)kc, 1, 0) != 0
                  || keyboard_layout_translate((uint16_t)kc, 0, 1) != 0;
        if (!mapped || reachable[kc]) continue;
        if (missing < 0) missing = kc;
        missing_count++;
    }
    // Named rather than counted: "3 unreachable" sends the next reader
    // looking, "86" tells them which key.
    if (missing >= 0)
        klog_printf("input: keycode %d maps a character no PS/2 wire byte "
                    "produces (%d in total)\n", missing, missing_count);
    KTEST_ASSERT_EQ(missing_count, 0);
}

KTEST("input", "the ISO key that carries `|` survives the PS/2 wire unprefixed") {
    // The specific regression, pinned by name. The check above is the
    // general property and would catch it too -- but only while some
    // layout maps keycode 86, and a US-only boot does not. This one
    // holds whatever is loaded.
    uint16_t kc = 0;
    KTEST_ASSERT(keyboard_wire_keycode(0x56, 0, &kc));
    KTEST_ASSERT_EQ((int)kc, INPUT_KEY_102ND);

    // PREFIXED, 0x56 is a different key and this kernel has no name for
    // it -- so a driver that added a stray 0xE0 would still lose the
    // key, just for a second reason.
    KTEST_ASSERT(!keyboard_wire_keycode(0x56, 1, &kc));
}

KTEST("input", "a keycode goes to the layout unchanged, whichever driver reported it") {
    // The evdev numbering is what BOTH paths now carry, so the layout
    // sees the same number either way. Asserted through the layout
    // rather than by injecting a key: a KTEST runs in the live kernel,
    // and feeding the real key ring would steal a keystroke from
    // whoever is typing.
    //
    // The escape key is the fixture because every layout has it at
    // keycode 1 and its character (0x1B) is the same everywhere, so this
    // does not depend on which layout booted.
    KTEST_ASSERT_EQ((int)keyboard_layout_translate(INPUT_KEY_ESC, 0, 0), 0x1B);

    // ...and a keycode past the table is refused rather than read out of
    // bounds, which is what stops the loop above walking off the end.
    KTEST_ASSERT_EQ((int)keyboard_layout_translate(60000, 0, 0), 0);
}

KTEST("input", "the scroll settings transform the wheel where it is consumed") {
    int save_step = mouse_scroll_step();
    int save_inv = mouse_scroll_invert();

    // Preemption off: win_input_poll() consumes the same delta from
    // scheduler_idle(), i.e. potentially between the feed and the read.
    scheduler_preempt_disable();
    (void)mouse_get_wheel_delta(); // drain whatever the desktop had coming

    mouse_set_scroll_step(3);
    mouse_set_scroll_invert(1);
    mouse_feed_wheel(2);
    KTEST_ASSERT_EQ(mouse_get_wheel_delta(), -6);

    // Defaults again: a multiplier must not compound across reads.
    mouse_set_scroll_step(1);
    mouse_set_scroll_invert(0);
    mouse_feed_wheel(-1);
    KTEST_ASSERT_EQ(mouse_get_wheel_delta(), -1);

    mouse_set_scroll_step(save_step);
    mouse_set_scroll_invert(save_inv);
    scheduler_preempt_enable();
}

// --- what the registry will and will not accept -------------------------
//
// THESE EXIST BECAUSE A REFUSAL SHIPPED AND KILLED ALL USB INPUT ON REAL
// HARDWARE. A check refusing a source with no capability bits looked
// obviously right and contradicted a documented, deliberate use:
// `xhci.c` registers `usb-xhci` with `caps = 0` whose only job is to be
// POLLED, and every HID device's decode rides that one poll. Nothing
// caught it because the default QEMU boot has no xHCI controller, so the
// path never ran -- `vm.py --usb xhci+mouse` is what exercises it, and
// nothing in the gate does. These do, in ring 0, on every `make test`.

static void probe_poll(void) { }

KTEST("input", "a source with no capability bits is accepted if it is polled") {
    // The shape xhci.c registers: it reports no events itself, it exists
    // to be serviced. A capability set says what a source REPORTS; it
    // says nothing about whether the source is worth polling.
    static const struct input_source svc = {
        .name = "ktest-service", .driver = "ktest", .caps = 0,
        .poll = probe_poll,
    };
    int before = input_source_count();
    input_register_source(&svc);
    KTEST_ASSERT_EQ(input_source_count(), before + 1);
    input_unregister_source(&svc);
    KTEST_ASSERT_EQ(input_source_count(), before);
}

KTEST("input", "a source that is neither polled nor on an interrupt is refused") {
    // The half of the check that IS right: nothing would ever service
    // it, which is silent and reads exactly like dead hardware.
    static const struct input_source orphan = {
        .name = "ktest-orphan", .driver = "ktest", .caps = INPUT_CAP_KEYS,
    };
    int before = input_source_count();
    input_register_source(&orphan);
    KTEST_ASSERT_EQ(input_source_count(), before);
}

// The bug this covers: input.c registered the source with `lsdrv` and
// never told it on the way out, so an unplugged HID device kept its row
// and a REPLUG added a second one. That is the "phantom second mouse"
// docs/bugs.md carried as an enumeration fault for weeks -- it was a
// reporting leak, and only the second half of the round trip was
// missing. Asserting the count either side is what makes that visible.
KTEST("input", "unregistering a source takes its `lsdrv` row with it") {
    static const struct input_source svc = {
        .name = "ktest-lsdrv", .driver = "ktest-lsdrv-drv", .caps = 0,
        .poll = probe_poll,
    };

    // Count how many times this driver names this device, so the check
    // is about THIS source rather than about the table's length -- a
    // stale row survives a shorter table perfectly well.
    input_register_source(&svc);
    int named_while_registered = 0;
    for (int i = 0; i < driver_count(); i++)
        if (k_strcmp(driver_name_at(i), "ktest-lsdrv-drv") == 0 &&
            k_strstr(driver_devices_at(i), "ktest-lsdrv"))
            named_while_registered = 1;

    input_unregister_source(&svc);
    int named_after = 0;
    for (int i = 0; i < driver_count(); i++)
        if (k_strcmp(driver_name_at(i), "ktest-lsdrv-drv") == 0 &&
            k_strstr(driver_devices_at(i), "ktest-lsdrv"))
            named_after = 1;

    KTEST_ASSERT(named_while_registered);
    KTEST_ASSERT(!named_after);
}
