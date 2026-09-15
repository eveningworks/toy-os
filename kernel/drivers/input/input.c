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
#include "driver.h" // driver_bound() -- `lsdrv`

// driver-none: the input class registry itself

// Small and fixed, like display.c's driver table: PS/2 keyboard, PS/2
// mouse, and however many virtio input devices are attached. A linked
// list would need each source to carry a mutable `next`, which would
// stop them being `const`.
#define MAX_INPUT_SOURCES 8

static const struct input_source *g_sources[MAX_INPUT_SOURCES];
static int g_count;

void input_register_source(const struct input_source *src) {
    if (!src || !src->name || g_count >= MAX_INPUT_SOURCES) return;

    // The honesty check block and display already make, and it is ONE
    // rule rather than two: a source that is neither polled nor on an
    // interrupt is never serviced, which is silent and looks exactly
    // like dead hardware.
    //
    // NO CAPABILITY BITS IS LEGITIMATE and was briefly refused here, at
    // the cost of all USB input on real hardware: xhci.c registers a
    // source with `caps = 0` whose only job is to be POLLED -- the
    // controller reports no events of its own, and every HID device's
    // decode rides that one poll. Refusing it stopped devices
    // enumerating at all. A capability set is a claim about what a
    // source REPORTS; it says nothing about whether the source is worth
    // servicing.
    if (!src->poll && !src->irq && !src->msi_vector) {
        klog_printf(KLOG_ERR "input: REFUSED %s -- neither polled nor on an "
                    "interrupt\n", src->name);
        return;
    }

    g_sources[g_count++] = src;
    driver_bound(src->driver, src->name);
    klog_printf("input: %s registered (%s%s%s%s)\n", src->name,
                (src->caps & INPUT_CAP_KEYS)  ? "keys " : "",
                (src->caps & INPUT_CAP_REL)   ? "rel "  : "",
                (src->caps & INPUT_CAP_ABS)   ? "abs "  : "",
                (src->caps & INPUT_CAP_WHEEL) ? "wheel" : "");
}

// USB hot-unplug is the caller: a source whose device is gone must
// leave the registry, or lsdev keeps naming a mouse that is not there.
// Compacting under input_poll_sources()' feet is safe on this
// uniprocessor -- the unregister runs FROM a source's own poll, so the
// walk merely sees a shorter list on its next index -- but a removed
// source's poll must never run again, which is why callers clear their
// own in_use flag first.
void input_unregister_source(const struct input_source *src) {
    for (int i = 0; i < g_count; i++) {
        if (g_sources[i] != src) continue;
        for (int j = i; j + 1 < g_count; j++) g_sources[j] = g_sources[j + 1];
        g_count--;
        // AND TELL `lsdrv`, which register_source() already tells. Every
        // other class registry that can lose a device does this from its
        // unregister path (net.c, sound.c); this one did not, so an
        // unplugged HID device kept its row and a replug added a SECOND
        // -- the "phantom second mouse" docs/bugs.md had recorded as an
        // enumeration fault for weeks. It was a reporting leak.
        driver_unbound(src->driver, src->name);
        klog_printf("input: %s unregistered\n", src->name);
        return;
    }
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

// --- a key, straight through -----------------------------------------
//
// **THERE IS NO TRANSLATION HERE ANY MORE, AND THAT IS THE FIX.** This
// used to convert evdev keycodes DOWN into AT set-1 scancodes, because
// the layout tables (/etc/kbs) were keyed on scancodes -- so every
// non-PS/2 device had to speak a legacy encoding to be understood. The
// table was hand-kept and duly grew a hole: KEY_102ND, the ISO key that
// carries `|` on every Nordic layout, sat just past the end of it, so a
// pipeline could be typed on a PS/2 boot and NOT on an `INPUT=virtio`
// one, with nothing to notice.
//
// The layout is keyed on evdev keycodes now, so this hands the keycode
// over unchanged and the only translation left in the kernel is set-1
// -> keycode inside the PS/2 driver -- which is where Linux keeps it
// (`atkbd`), and where a hole would break the LEGACY path rather than
// the modern one. See docs/decisions.md.
void input_report_key(uint16_t keycode, int down) {
    if (!keycode) return;
    keyboard_key_event(keycode, down);
}

void input_report_rel(int dx, int dy) { mouse_feed_rel(dx, dy); }

void input_report_abs(int x, int y, int max_x, int max_y) {
    mouse_feed_abs(x, y, max_x, max_y);
}

void input_report_buttons(uint8_t mask) { mouse_feed_buttons(mask); }

void input_report_wheel(int notches) { mouse_feed_wheel(notches); }
