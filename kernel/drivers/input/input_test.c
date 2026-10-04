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
#include "termkey.h"   // a key crosses a terminal as a sequence
#include "string.h"
#include "keyboard_layout.h" // the parity check asks the LAYOUT what it maps
#include "kfmt.h"            // klog_printf -- name the unreachable key
#include "mouse.h"
#include "tty.h"              // the bypass: which side of the split a key lands on
#include "win_proto.h"        // the raw event types a compositor is handed
#include "win_input.h"        // the real drain and queue, for window_events()
#include "win_role.h"         // win_server_compositor_pid(): a live desktop's streams

// THE KEY STREAMS BELONG TO A LIVE COMPOSITOR when one holds the role:
// keyboard_events_attach() would empty them under it, so a test that
// attaches skips instead (win_input_test.c has the same guard).
#define SKIP_IF_ROLE_HELD                                                     \
    do {                                                                      \
        if (win_server_compositor_pid())                                      \
            KTEST_SKIP("a compositor holds the role");                        \
    } while (0)
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

KTEST("input", "Caps Lock capitalises a letter key and nothing else") {
    // THE LAYOUT DECIDES, so the fixtures are found in it rather than
    // assumed: any key whose unshifted symbol is a letter and shifted
    // one its capital, and the key that types '1'.
    int letter = -1, digit = -1;
    for (int kc = 1; kc < 256; kc++) {
        int lo = keyboard_layout_translate((uint16_t)kc, 0, 0);
        int up = keyboard_layout_translate((uint16_t)kc, 1, 0);
        if (letter < 0 && lo >= 'a' && lo <= 'z' && up == lo - 'a' + 'A') letter = kc;
        if (digit < 0 && lo == '1') digit = kc;
    }
    KTEST_ASSERT(letter >= 0 && digit >= 0);
    int lo = keyboard_layout_translate((uint16_t)letter, 0, 0);
    int up = keyboard_layout_translate((uint16_t)letter, 1, 0);
    KTEST_ASSERT_EQ((int)keyboard_layout_translate_caps((uint16_t)letter, 0, 0, 1), (int)up);
    // Caps+Shift is lowercase, as on Windows and Linux.
    KTEST_ASSERT_EQ((int)keyboard_layout_translate_caps((uint16_t)letter, 1, 0, 1), (int)lo);
    KTEST_ASSERT_EQ((int)keyboard_layout_translate_caps((uint16_t)letter, 0, 0, 0), (int)lo);
    // A digit is not a letter: Caps leaves it alone, Shift still works.
    KTEST_ASSERT_EQ((int)keyboard_layout_translate_caps((uint16_t)digit, 0, 0, 1), '1');
    KTEST_ASSERT_EQ((int)keyboard_layout_translate_caps((uint16_t)digit, 1, 0, 1),
                    (int)keyboard_layout_translate((uint16_t)digit, 1, 0));
}

KTEST("input", "the Caps Lock key toggles on its press, not on a repeat of it") {
    int letter = -1;
    for (int kc = 1; kc < 84 && letter < 0; kc++) {   // <= 83: a PS/2 make code too
        int lo = keyboard_layout_translate((uint16_t)kc, 0, 0);
        if (lo >= 'a' && lo <= 'z') letter = kc;
    }
    KTEST_ASSERT(letter >= 0);
    int lo = keyboard_layout_translate((uint16_t)letter, 0, 0);
    uint8_t mods = 0;

    scheduler_preempt_disable();
    while (keyboard_try_getchar_mods(&mods) != -1) { }
    // Pressed, REPEATED (typematic: another make, no break), released.
    input_report_key(INPUT_KEY_CAPSLOCK, 1);
    input_report_key(INPUT_KEY_CAPSLOCK, 1);
    input_report_key(INPUT_KEY_CAPSLOCK, 0);
    while (keyboard_try_getchar_mods(&mods) != -1) { }   // the KEY_CAPS_LOCK codes
    input_report_key((uint16_t)letter, 1);
    input_report_key((uint16_t)letter, 0);
    int on = keyboard_try_getchar_mods(&mods);
    // Off again, so the machine is left as it was found.
    input_report_key(INPUT_KEY_CAPSLOCK, 1);
    input_report_key(INPUT_KEY_CAPSLOCK, 0);
    while (keyboard_try_getchar_mods(&mods) != -1) { }
    input_report_key((uint16_t)letter, 1);
    input_report_key((uint16_t)letter, 0);
    int off = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(on, (int)(lo - 'a' + 'A'));   // a repeat toggled it back off
    KTEST_ASSERT_EQ(off, (int)lo);
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
    //
    // **ONE KEY IS NOW ONE SEQUENCE**, because what crosses a terminal
    // is ANSI: Up is `ESC [ A` (api/termkey.h). The claim is unchanged
    // -- one key in, one key out, nothing left over -- but it is read
    // through the decoder rather than as a single private byte, which
    // is what a program on this terminal does too.
    input_report_key(INPUT_KEY_UP, 1);

    struct termkey_state st;
    k_memset(&st, 0, sizeof st);
    int key = TERMKEY_MORE;
    int bytes = 0;
    for (int c; (c = keyboard_try_getchar_mods(&mods)) != -1; ) {
        bytes++;
        key = termkey_feed(&st, c);
        if (key != TERMKEY_MORE) break;
    }
    int extra = keyboard_try_getchar_mods(&mods);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(key, KEY_ARROW_UP);
    KTEST_ASSERT_EQ(bytes, 3);  // ESC [ A -- and not a stray 0xE0 prefix
    KTEST_ASSERT_EQ(extra, -1); // nothing left over after the sequence
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

KTEST("input", "a warp puts the pointer there, and cannot leave the bounds") {
    int w = 0, h = 0;
    mouse_get_bounds(&w, &h);
    if (w <= 1 || h <= 1) KTEST_SKIP("the pointer has no usable bounds yet");

    int save_x = 0, save_y = 0;
    uint8_t buttons = 0;
    mouse_get_state(&save_x, &save_y, &buttons);

    int x = 0, y = 0;
    mouse_set_position(w / 4, h / 3);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, w / 4);
    KTEST_ASSERT_EQ(y, h / 3);

    // CLAMPED like any other motion: a compositor asking for somewhere
    // off screen must not be able to strand the pointer where no device
    // could put it, and the last pixel is the last pixel.
    mouse_set_position(w + 1000, h + 1000);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, w - 1);
    KTEST_ASSERT_EQ(y, h - 1);

    mouse_set_position(-50, -50);
    mouse_get_state(&x, &y, &buttons);
    KTEST_ASSERT_EQ(x, 0);
    KTEST_ASSERT_EQ(y, 0);

    // PUT IT BACK: these tests run in the LIVE kernel, and a pointer
    // left in a corner is a desktop somebody has to rescue by hand.
    mouse_set_position(save_x, save_y);
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

    // THE THUMB BUTTONS ARE REAL BITS NOW (SIDE 0x08, EXTRA 0x10), and
    // this assertion used to be `0xF0 -> 0`. It is kept as a mask test
    // rather than deleted: a driver reporting a button nothing here
    // names must still be dropped, because consumers ask "buttons != 0"
    // and a stray high bit would answer yes forever.
    input_report_buttons(0x18);
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0x18);

    input_report_buttons(0xE0);   // above EXTRA: no name, no bit
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0);

    // AND THE THUMB BITS COMPOSE WITH THE ORDINARY ONES, which is the
    // property the whole mask exists for -- holding left while pressing
    // back must not release left.
    input_report_buttons(0x09);
    mouse_get_state(&x, &y, &now);
    KTEST_ASSERT_EQ(now, 0x09);

    input_report_buttons(before);
    // Every report above queued an edge, and this runs in the live
    // kernel: left there, a desktop would be handed a fistful of
    // phantom clicks the moment win_input_poll() next ran.
    while (mouse_try_get_button_transition(0)) ;
}

KTEST("input", "a press and its release both survive one polling pass") {
    // THE BUG THIS REJECTS: mouse_get_state()'s mask is a LEVEL, and
    // everything above it samples. A thumb tap whose press and release
    // land between two passes of scheduler_idle() read back as no
    // change at all, so the click never reached a client -- which is
    // what "the back button works about one time in twenty" was.
    //
    // **NOTHING IS ASSERTED WHILE PREEMPTION IS OFF.** A KTEST_ASSERT
    // returns from the test body, so an assertion inside the guarded
    // region leaves the counter raised and the machine wedges -- which
    // is what the first version of this test did to its own positive
    // control: a hang instead of a red line.
    scheduler_preempt_disable();
    int x = 0, y = 0;
    uint8_t before = 0;
    mouse_get_state(&x, &y, &before);
    while (mouse_try_get_button_transition(0)) ;   // whatever the desktop had coming

    input_report_buttons(0x08);   // SIDE down -- the thumb's "back"
    input_report_buttons(0);      // ...and up again, inside one pass

    uint8_t level = 0;
    mouse_get_state(&x, &y, &level);
    uint8_t e1 = 0xFF, e2 = 0xFF, e3 = 0xFF;
    int got1 = mouse_try_get_button_transition(&e1);
    int got2 = mouse_try_get_button_transition(&e2);
    int got3 = mouse_try_get_button_transition(&e3);

    // A repeat of the same mask is not an edge: a device that re-reports
    // what is already held must not manufacture a second press.
    input_report_buttons(0x01);
    input_report_buttons(0x01);
    uint8_t r1 = 0xFF, r2 = 0xFF;
    int rgot1 = mouse_try_get_button_transition(&r1);
    int rgot2 = mouse_try_get_button_transition(&r2);

    input_report_buttons(before);
    while (mouse_try_get_button_transition(0)) ;
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(level, 0);    // the level says nothing happened...
    KTEST_ASSERT(got1);           // ...and the queue says otherwise
    KTEST_ASSERT_EQ(e1, 0x08);
    KTEST_ASSERT(got2);
    KTEST_ASSERT_EQ(e2, 0);
    KTEST_ASSERT(!got3);

    KTEST_ASSERT(rgot1);
    KTEST_ASSERT_EQ(r1, 0x01);
    KTEST_ASSERT(!rgot2);
}

KTEST("input", "a button edge keeps the position it was reported at") {
    // A press at A and a release at B drained in one pass must not both
    // read B, or the compositor sees a click become a drag. Same
    // preemption rule as the test above: nothing asserted while it is off.
    scheduler_preempt_disable();
    int x0 = 0, y0 = 0, bw = 0, bh = 0;
    uint8_t before = 0;
    mouse_get_state(&x0, &y0, &before);
    mouse_get_bounds(&bw, &bh);
    while (mouse_try_get_button_edge(0, 0, 0)) ;

    int ax = bw / 4, ay = bh / 4, bx = bw / 2, by = bh / 2;
    mouse_set_position(ax, ay);
    input_report_buttons(0x01);
    mouse_set_position(bx, by);
    input_report_buttons(0);

    uint8_t m1 = 0xFF, m2 = 0xFF;
    int x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    int got1 = mouse_try_get_button_edge(&m1, &x1, &y1);
    int got2 = mouse_try_get_button_edge(&m2, &x2, &y2);

    mouse_set_position(x0, y0);
    input_report_buttons(before);
    while (mouse_try_get_button_edge(0, 0, 0)) ;
    scheduler_preempt_enable();

    KTEST_ASSERT(got1 && got2);
    KTEST_ASSERT_EQ(m1, 0x01);
    KTEST_ASSERT_EQ(x1, ax);
    KTEST_ASSERT_EQ(y1, ay);
    KTEST_ASSERT_EQ(m2, 0);
    KTEST_ASSERT_EQ(x2, bx);
    KTEST_ASSERT_EQ(y2, by);
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

// ONE KEY PRESS WITH A MODIFIER HELD, read back as (code, mods) pairs.
// Returns how many codes arrived, up to `cap`.
// A WINDOW reads the key event stream (presses only are counted here,
// and the modifier keys' own edges are skipped); a TERMINAL reads bytes.
static int is_modifier_code(int c) {
    return c == KEY_SHIFT || c == KEY_CTRL || c == KEY_ALT || c == KEY_ALTGR || c == KEY_SUPER;
}

static int modified_key(uint16_t mod, uint16_t key, int *codes, uint8_t *mods, int cap) {
    uint8_t m = 0;
    int window = keyboard_events_attached();
    while (keyboard_try_getchar_mods(&m) != -1) { }
    while (keyboard_try_get_key(0, 0, 0)) { }
    input_report_key(mod, 1);
    input_report_key(key, 1);
    input_report_key(key, 0);
    input_report_key(mod, 0);
    int n = 0;
    if (window) {
        int c, d;
        while (keyboard_try_get_key(&c, &d, &m))
            if (d && !is_modifier_code(c) && n < cap) { codes[n] = c; mods[n++] = m; }
        return n;
    }
    for (int c; n < cap && (c = keyboard_try_getchar_mods(&m)) != -1; n++) {
        codes[n] = c;
        mods[n] = m;
    }
    return n;
}

// The two halves of api/keyboard.h's "Ctrl and Alt": a WINDOW (the tty
// bypassed, as while a compositor holds it) gets the key and its
// modifier bit; a TERMINAL gets what a terminal sends.
KTEST("input", "Alt+key is the key with KEY_MOD_ALT for a window, ESC then the key for a terminal") {
    SKIP_IF_ROLE_HELD;
    struct tty *t = tty_console();
    int was = tty_bypassed(t), was_att = keyboard_events_attached();
    int codes[4];
    uint8_t mods[4];

    scheduler_preempt_disable();
    tty_set_bypass(t, 1);
    keyboard_events_attach(1);
    int n_win = modified_key(INPUT_KEY_LEFTALT, EVDEV_A, codes, mods, 4);
    int win0 = codes[0], winmods = mods[0];
    keyboard_events_attach(0);
    tty_set_bypass(t, 0);
    int n_term = modified_key(INPUT_KEY_LEFTALT, EVDEV_A, codes, mods, 4);
    keyboard_events_attach(was_att);
    tty_set_bypass(t, was);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(n_win, 1);             // ONE key, not an ESC before it
    KTEST_ASSERT_EQ(win0, 'a');
    KTEST_ASSERT(winmods & KEY_MOD_ALT);
    KTEST_ASSERT_EQ(n_term, 2);            // readline's meta prefix
    KTEST_ASSERT_EQ(codes[0], 0x1B);
    KTEST_ASSERT_EQ(codes[1], 'a');
}

KTEST("input", "Ctrl+digit reaches a window with KEY_MOD_CTRL and a terminal not at all") {
    SKIP_IF_ROLE_HELD;
    struct tty *t = tty_console();
    int was = tty_bypassed(t), was_att = keyboard_events_attached();
    int codes[4];
    uint8_t mods[4];

    scheduler_preempt_disable();
    tty_set_bypass(t, 1);
    keyboard_events_attach(1);
    int n_win = modified_key(INPUT_KEY_LEFTCTRL, EVDEV_1, codes, mods, 4);
    int win0 = codes[0], winmods = mods[0];
    keyboard_events_attach(0);
    tty_set_bypass(t, 0);
    int n_term = modified_key(INPUT_KEY_LEFTCTRL, EVDEV_1, codes, mods, 4);
    // Ctrl+letter is unchanged on both sides: the control code.
    int n_letter = modified_key(INPUT_KEY_LEFTCTRL, EVDEV_A, codes, mods, 4);
    keyboard_events_attach(was_att);
    tty_set_bypass(t, was);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(n_win, 1);             // it used to be dropped here too
    KTEST_ASSERT_EQ(win0, '1');
    KTEST_ASSERT(winmods & KEY_MOD_CTRL);
    KTEST_ASSERT_EQ(n_term, 0);            // a terminal has no code for it
    KTEST_ASSERT_EQ(n_letter, 1);
    KTEST_ASSERT_EQ(codes[0], 0x01);
}

// THE PHYSICAL STREAM: a key by position, both edges, no repeats, and a
// modifier that changes nothing about what is reported -- Ctrl+1 is the
// key INPUT_KEY_1 going down with KEY_MOD_CTRL set, not a dropped key.
KTEST("input", "the physical stream reports positions, both edges, and no repeats") {
    SKIP_IF_ROLE_HELD;
    scheduler_preempt_disable();
    int was_att = keyboard_events_attached();
    keyboard_events_attach(1);            // the stream exists only for a compositor
    uint8_t cm;
    input_report_key(INPUT_KEY_LEFTCTRL, 1);
    input_report_key(INPUT_KEY_1, 1);
    input_report_key(INPUT_KEY_1, 1);    // a typematic repeat: NOT an edge
    input_report_key(INPUT_KEY_1, 0);
    input_report_key(INPUT_KEY_LEFTCTRL, 0);
    uint16_t kcs[6]; int downs[6]; uint8_t mods[6];
    int n = 0;
    while (n < 6 && keyboard_try_get_physical(&kcs[n], &downs[n], &mods[n])) n++;
    while (keyboard_try_getchar_mods(&cm) != -1) { }
    keyboard_events_attach(was_att);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(n, 4);
    KTEST_ASSERT_EQ(kcs[0], INPUT_KEY_LEFTCTRL);
    KTEST_ASSERT_EQ(downs[0], 1);
    KTEST_ASSERT_EQ(kcs[1], INPUT_KEY_1);
    KTEST_ASSERT_EQ(downs[1], 1);
    KTEST_ASSERT(mods[1] & KEY_MOD_CTRL);
    KTEST_ASSERT_EQ(kcs[2], INPUT_KEY_1);
    KTEST_ASSERT_EQ(downs[2], 0);
    KTEST_ASSERT_EQ(kcs[3], INPUT_KEY_LEFTCTRL);
    KTEST_ASSERT_EQ(downs[3], 0);
}

// --- THE KEY EVENT STREAM, as a compositor reads it -----------------------
//
// keyboard_try_get_key(): one ordered queue of edges while the console is
// bypassed. A fixture layout, so no XKB data is assumed; the real one is
// reloaded after the preemption guard drops (it reads /etc/kbs, whose
// mount lock sleeps). Nothing is asserted while preemption is off.
static const char DEAD_FIXTURE[] =
    "kc_18=e\nkc_45=x\nkc_13=dead:acute\n"
    "dead:acute=0xB4\ndead:acute:e=0xE9\n";

#define EVLOG_MAX 600
struct evlog { int n, code[EVLOG_MAX], down[EVLOG_MAX]; };
static struct evlog g_ev, g_ph;   // keys, and the same keys by position

static void tap(uint16_t kc) { input_report_key(kc, 1); input_report_key(kc, 0); }

// Runs `script` as a window would see it -- under a stand-in compositor,
// through the real drain into the real queue -- and records every key
// event (g_ev) and positional edge (g_ph).
static void window_events(void (*script)(void)) {
    char name[KB_LAYOUT_NAME_MAX];
    k_strlcpy(name, keyboard_layout_current(), sizeof name);
    k_memset(&g_ev, 0, sizeof g_ev);
    k_memset(&g_ph, 0, sizeof g_ph);
    int pid = 0;
    for (int p = SCHED_PID_MAX - 1; p > 0 && !pid; p--)   // a pid nobody holds
        if (!scheduler_pid_valid(p)) pid = p;
    if (!pid) return;                    // the caller's count assertion fails

    scheduler_preempt_disable();
    win_server_set_compositor(pid, 0);   // attaches the keyboard, empties the queues
    keyboard_layout_load_text(DEAD_FIXTURE, sizeof DEAD_FIXTURE - 1);
    script();
    // THE REAL DRAIN: win_input_poll()'s key half, then the compositor
    // consumes what it queued -- round after round until nothing is left.
    for (int took = 1; took; ) {
        took = 0;
        win_input_drain_keys();
        struct win_event e;
        while (win_input_pop(&e)) {
            took = 1;
            struct evlog *l = e.type == WIN_EV_RAW_KEY_PHYS ? &g_ph : &g_ev;
            int down = e.type == WIN_EV_RAW_KEY_PHYS ? e.b : e.type == WIN_EV_RAW_KEY;
            if (l->n < EVLOG_MAX) { l->code[l->n] = e.a; l->down[l->n++] = down; }
        }
    }
    keyboard_layout_compose_reset();
    win_server_set_compositor(0, 0);
    scheduler_preempt_enable();
    keyboard_layout_load(name);
}

// Every code that went down ends up, and nothing comes up that was not
// down (autorepeat is several presses before one release).
static int every_down_has_up(void) {
    static uint8_t down[256];
    k_memset(down, 0, sizeof down);
    for (int i = 0; i < g_ev.n; i++) {
        int c = g_ev.code[i] & 0xFF;
        if (g_ev.down[i]) down[c] = 1;
        else if (!down[c]) return 0;         // a release with no press
        else down[c] = 0;
    }
    for (int c = 0; c < 256; c++) if (down[c]) return 0;
    return 1;
}
static int still_held(int code) {
    int n = 0;
    for (int i = 0; i < g_ev.n; i++)
        if (g_ev.code[i] == code) n = g_ev.down[i] ? 1 : 0;
    return n;
}

static void script_dead_x(void) { tap(13); tap(45); }
KTEST("input", "dead key + a key it does not compose with: both characters come back UP") {
    SKIP_IF_ROLE_HELD;
    // Two characters from one press. The accent has no key, so it goes
    // down and up at once; the key's own release reports the key.
    window_events(script_dead_x);
    KTEST_ASSERT_EQ(g_ev.n, 4);
    KTEST_ASSERT(g_ev.code[0] == 0xB4 && g_ev.down[0] == 1);
    KTEST_ASSERT(g_ev.code[1] == 0xB4 && g_ev.down[1] == 0);
    KTEST_ASSERT(g_ev.code[2] == 'x' && g_ev.down[2] == 1);
    KTEST_ASSERT(g_ev.code[3] == 'x' && g_ev.down[3] == 0);
}

static void script_dead_kp(void) { tap(13); tap(INPUT_KEY_KP1); tap(18); }
KTEST("input", "a keypad character ends a pending accent like any other key") {
    SKIP_IF_ROLE_HELD;
    // "dead acute, KP1, e" is "acute 1 e" -- the keypad is text.
    window_events(script_dead_kp);
    KTEST_ASSERT_EQ(g_ev.n, 6);
    KTEST_ASSERT_EQ(g_ev.code[0], 0xB4);
    KTEST_ASSERT_EQ(g_ev.code[2], '1');
    KTEST_ASSERT_EQ(g_ev.code[4], 'e');
}

static void script_interleave(void) { tap(18); tap(45); tap(13); tap(45); }
KTEST("input", "a window gets presses and releases in the order they happened") {
    SKIP_IF_ROLE_HELD;
    // Each press before ITS release, the second key's press before the
    // second key's release -- two queues read separately broke exactly that.
    window_events(script_interleave);
    static const int want_code[] = { 'e', 'e', 'x', 'x', 0xB4, 0xB4, 'x', 'x' };
    KTEST_ASSERT_EQ(g_ev.n, 8);
    for (int i = 0; i < 8; i++) {
        KTEST_ASSERT_EQ(g_ev.code[i], want_code[i]);
        KTEST_ASSERT_EQ(g_ev.down[i], (i % 2) == 0);
    }
}

#define BURST 100
static void script_burst(void) { for (int i = 0; i < BURST; i++) tap(i % 2 ? 45 : 18); }
KTEST("input", "a burst typed between two polls all arrives, in order, every press released") {
    SKIP_IF_ROLE_HELD;
    // 200 events, far more than a compositor's queue holds: each poll
    // takes what fits and the rest WAITS -- nothing evicted, nothing lost.
    window_events(script_burst);
    KTEST_ASSERT_EQ(g_ev.n, 2 * BURST);
    KTEST_ASSERT(every_down_has_up());
    for (int i = 0; i < BURST; i++) {
        int want = i % 2 ? 'x' : 'e';
        KTEST_ASSERT_EQ(g_ev.code[2 * i], want);
        KTEST_ASSERT_EQ(g_ev.down[2 * i], 1);
        KTEST_ASSERT_EQ(g_ev.code[2 * i + 1], want);
        KTEST_ASSERT_EQ(g_ev.down[2 * i + 1], 0);
    }
}

static void script_repeat(void) {
    input_report_key(45, 1);                         // x held...
    input_report_key(18, 1);                         // ...then e, which repeats
    for (int i = 0; i < 400; i++) input_report_key(18, 1);
    input_report_key(18, 0);
    input_report_key(45, 0);
}
KTEST("input", "a long autorepeat cannot crowd out a release that is owed") {
    SKIP_IF_ROLE_HELD;
    // 400 repeats overflow the queue: the newest presses are refused,
    // and both releases still arrive -- a queued press always has room
    // for its release.
    window_events(script_repeat);
    KTEST_ASSERT(g_ev.n > 4);
    KTEST_ASSERT(every_down_has_up());
    KTEST_ASSERT(!still_held('e'));
    KTEST_ASSERT(!still_held('x'));
    KTEST_ASSERT_EQ(g_ev.code[0], 'x');
    KTEST_ASSERT_EQ(g_ev.code[g_ev.n - 1], 'x');   // the last release, last
    KTEST_ASSERT_EQ(g_ev.down[g_ev.n - 1], 0);
}

KTEST("input", "every KEY_* special has a C1 low byte and IS_SPECIAL_KEY knows it") {
    // keyboard.h's promise: a special truncated to a byte by mistake is
    // a C1 control, never a Latin-1 letter or Ctrl-C.
    static const int all[] = {
        KEY_ARROW_UP, KEY_ARROW_DOWN, KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_ARROW_LEFT,
        KEY_ARROW_RIGHT, KEY_HOME, KEY_END, KEY_DELETE, KEY_F2, KEY_F3,
        KEY_SHIFT_ARROW_LEFT, KEY_SHIFT_ARROW_RIGHT, KEY_SHIFT_ARROW_UP,
        KEY_SHIFT_ARROW_DOWN, KEY_SHIFT_HOME, KEY_SHIFT_END, KEY_CTRL_ARROW_LEFT,
        KEY_CTRL_ARROW_RIGHT, KEY_F10, KEY_F4, KEY_SUPER, KEY_SHIFT, KEY_CTRL,
        KEY_ALT, KEY_ALTGR, KEY_F1, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
        KEY_F11, KEY_F12, KEY_INSERT, KEY_MENU, KEY_CAPS_LOCK, KEY_NUM_LOCK,
        KEY_SCROLL_LOCK, KEY_PAUSE, KEY_PRINT_SCREEN,
    };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
        KTEST_ASSERT(IS_SPECIAL_KEY(all[i]));
        KTEST_ASSERT((all[i] & 0xFF) >= 0x80 && (all[i] & 0xFF) <= 0x9F);
        KTEST_ASSERT(!IS_PRINTABLE_KEY(all[i] & 0xFF));
    }
    KTEST_ASSERT(!IS_SPECIAL_KEY(KEY_PRINT_SCREEN + 1));
    KTEST_ASSERT(!IS_SPECIAL_KEY(0xE9));
}

static void script_hold_enter(void) { input_report_key(INPUT_KEY_ENTER, 1); }
KTEST("input", "a compositor change empties the key streams and forgets owed releases") {
    SKIP_IF_ROLE_HELD;
    // A restart or handoff: the old compositor's queued Enter must not
    // reach the new one, and the release of a key it saw go down must
    // not either -- the new one never saw the press.
    struct tty *t = tty_console();
    int was = tty_bypassed(t), was_att = keyboard_events_attached();
    int c, d, n_after = 0, n_held = 0, n_ring = 0;
    uint8_t m;
    uint16_t pk; int pd; uint8_t pm;

    scheduler_preempt_disable();
    tty_set_bypass(t, 1);
    keyboard_events_attach(1);
    script_hold_enter();                       // Enter down, still held
    keyboard_events_attach(1);                 // ...the compositor changes
    input_report_key(INPUT_KEY_ENTER, 0);      // its release arrives
    while (keyboard_try_get_key(&c, &d, &m)) n_after++;
    while (keyboard_try_get_physical(&pk, &pd, &pm)) n_after++;
    // Detached with the screen held (a restart in progress): typed keys
    // reach neither the stream nor the console behind the desktop.
    keyboard_events_attach(0);
    while (keyboard_try_getchar_mods(&m) != -1) { }
    tap(INPUT_KEY_ENTER);
    while (keyboard_try_get_key(&c, &d, &m)) n_held++;
    while (keyboard_try_getchar_mods(&m) != -1) n_ring++;
    keyboard_events_attach(was_att);
    tty_set_bypass(t, was);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(n_after, 0);   // nothing from before, no orphan release
    KTEST_ASSERT_EQ(n_held, 0);
    KTEST_ASSERT_EQ(n_ring, 0);
}

static void pause_wire(void) {
    static const uint8_t seq[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    for (unsigned i = 0; i < sizeof seq; i++) keyboard_feed_byte(seq[i]);
}
static void script_pause(void) {
    pause_wire();
    pause_wire();
    keyboard_feed_byte(0x12);   // 'e' (set 1 make 0x12, evdev 18) right behind:
    keyboard_feed_byte(0x92);   // ...it must not be swallowed as Pause's tail
}
KTEST("input", "PS/2 Pause, which has no break code, comes back up in both streams") {
    // The wire reports the release Pause never sends, so the key stream,
    // the positional stream and the tap all see a press and a release --
    // and a SECOND Pause is a second press, not a repeat of a stuck one.
    SKIP_IF_ROLE_HELD;
    window_events(script_pause);
    KTEST_ASSERT_EQ(g_ev.n, 6);
    for (int i = 0; i < 4; i++) {
        KTEST_ASSERT_EQ(g_ev.code[i], KEY_PAUSE);
        KTEST_ASSERT_EQ(g_ev.down[i], (i % 2) == 0);
    }
    KTEST_ASSERT_EQ(g_ev.code[4], 'e');
    KTEST_ASSERT_EQ(g_ph.n, 6);
    for (int i = 0; i < 4; i++) {
        KTEST_ASSERT_EQ(g_ph.code[i], INPUT_KEY_PAUSE);
        KTEST_ASSERT_EQ(g_ph.down[i], (i % 2) == 0);
    }
}

static void script_pause_usb(void) { input_report_key(INPUT_KEY_PAUSE, 1); }
KTEST("input", "a keyboard with a real Pause release can hold Pause") {
    SKIP_IF_ROLE_HELD;
    window_events(script_pause_usb);
    KTEST_ASSERT_EQ(g_ev.n, 1);           // down, and held: no invented release
    KTEST_ASSERT(g_ev.code[0] == KEY_PAUSE && g_ev.down[0] == 1);
}
