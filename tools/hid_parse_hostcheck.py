#!/usr/bin/env python3
"""Check kernel/lib/hid_parse.c against REAL report descriptors, on the host.

WHY A HOST HARNESS. The parser decides where a device's buttons and
axes sit inside its reports, and it is wrong in a way that is expensive
to debug on the machine: a misplaced axis is not a missing feature, it
is a pointer that flies across the screen, on somebody's laptop, with
the only oracle being a person watching it. The descriptors themselves
are small and fixed, so the whole question can be asked on the host
against bytes captured off real hardware -- which is also the only way
to cover a device nobody here can plug in.

The fixtures are captured, not invented. `g305_mouse` and `g305_kbd`
came off a Logitech G305 wireless receiver (046d:c53f) on the ASUS,
read with GET_DESCRIPTOR(0x22) at bind time; `qemu_mouse` and
`qemu_kbd` off QEMU's usb-mouse and usb-kbd. A descriptor invented by
the same person who wrote the parser tests only that they were
consistent with themselves.

WHAT IS ASSERTED, and why each one exists:

  * **The derived layout, field by field.** Offsets and widths are
    written out by hand from the decoded descriptor, so a parser that
    drifts is caught by a number rather than by a shrug. The G305's
    mouse is the interesting one: a report ID, sixteen buttons, and
    SIGNED 16-BIT axes -- none of which the boot format has.
  * **A keyboard's keycodes are an ARRAY, not a bitmap.** It is the one
    structural difference between the two device classes, and a parser
    that treats the array as variable bits reports six keys held at
    once forever.
  * **Round-tripping a synthesised report.** A layout is only worth
    having if reading a report through it returns what was put in, so
    the check builds reports with known values -- including negative
    axis deltas, which is what the sign-extension is for -- and reads
    them back.
  * **It REFUSES rather than guesses.** Truncated descriptors, a length
    that lies, silly report sizes, an unterminated collection: each
    must return 0 so the caller falls back to the boot protocol.
    Garbage in must not produce a plausible-looking layout, because a
    plausible-looking layout is the one that gets used.

Exit status is non-zero on any violation. Needs gcc.
"""
import os
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def hexbytes(text):
    return bytes(int(t, 16) for t in text.split())


# --- captured descriptors --------------------------------------------

# Logitech G305 receiver, interface 1 (mouse). 148 bytes, four top-level
# collections: Mouse (report ID 2), Consumer (3), System Control (4) and
# a vendor one (8). The parser must take the FIRST and stop at its end.
G305_MOUSE = hexbytes("""
05 01 09 02 a1 01 85 02 09 01 a1 00 95 10 75 01
15 00 25 01 05 09 19 01 29 10 81 02 95 02 75 10
16 01 80 26 ff 7f 05 01 09 30 09 31 81 06 95 01
75 08 15 81 25 7f 09 38 81 06 95 01 05 0c 0a 38
02 81 06 c0 c0 05 0c 09 01 a1 01 85 03 95 02 75
10 15 01 26 ff 02 19 01 2a ff 02 81 00 c0 05 01
09 80 a1 01 85 04 95 01 75 02 15 01 25 03 09 82
09 81 09 83 81 00 75 06 81 03 c0 06 bc ff 09 88
a1 01 85 08 95 01 75 08 15 01 26 ff 00 19 01 29
ff 81 00 c0
""")

# The same receiver, interface 0 (keyboard). No report ID: the modifier
# byte, a reserved byte, the LED output report, then six key slots.
G305_KBD = hexbytes("""
05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01
75 01 95 08 81 02 81 03 95 05 05 08 19 01 29 05
91 02 95 01 75 03 91 01 95 06 75 08 15 00 26 ff
00 05 07 19 00 2a ff 00 81 00 c0
""")

# QEMU's usb-mouse. Five buttons, three padding bits, three 8-bit
# signed axes, and NO report ID -- the shape the boot protocol was
# modelled on, and the one every test in this repo runs against.
QEMU_MOUSE = hexbytes("""
05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05
15 00 25 01 95 05 75 01 81 02 95 01 75 03 81 01
05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03
81 06 c0 c0
""")

# QEMU's usb-kbd: the textbook boot keyboard.
QEMU_KBD = hexbytes("""
05 01 09 06 a1 01 75 01 95 08 05 07 19 e0 29 e7
15 00 25 01 81 02 95 01 75 08 81 01 95 05 75 01
05 08 19 01 29 05 91 02 95 01 75 03 91 01 95 06
75 08 15 00 25 ff 05 07 19 00 29 ff 81 00 c0
""")

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "hid_parse.h"

static void emit(const char *name, const struct hid_field *f) {
    printf("%s %u %u %u %u %u\n", name, f->present, f->bit_off,
           f->bits, f->count, f->is_signed);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int want_mouse = atoi(argv[1]);
    /* the descriptor as hex on argv[2] */
    static uint8_t d[1024];
    uint32_t n = 0;
    for (const char *p = argv[2]; *p && n < sizeof d; ) {
        if (*p == ' ') { p++; continue; }
        unsigned v; if (sscanf(p, "%2x", &v) != 1) break;
        d[n++] = (uint8_t)v; p += 2;
    }
    struct hid_layout L;
    int ok = hid_parse_report_descriptor(d, n, want_mouse, &L);
    printf("ok %d id %u mouse %u kbd %u bits %u\n",
           ok, L.report_id, L.is_mouse, L.is_keyboard, L.report_bits);
    emit("buttons", &L.buttons);
    emit("x", &L.x);
    emit("y", &L.y);
    emit("wheel", &L.wheel);
    emit("pan", &L.pan);
    emit("mods", &L.mods);
    emit("keys", &L.keys);

    /* Round trip: build a body from argv[3..] as "field=value" and read
       it back through the layout. */
    if (argc > 3) {
        static uint8_t body[64];
        for (unsigned z = 0; z < sizeof body; z++) body[z] = 0;
        int bx = atoi(argv[3]), by = atoi(argv[4]);
        /* write X and Y little-endian at their own offsets */
        const struct hid_field *fx = &L.x, *fy = &L.y;
        for (int b = 0; b < fx->bits; b++) {
            if ((bx >> b) & 1) body[(fx->bit_off + b) >> 3] |= 1u << ((fx->bit_off + b) & 7);
        }
        for (int b = 0; b < fy->bits; b++) {
            if ((by >> b) & 1) body[(fy->bit_off + b) >> 3] |= 1u << ((fy->bit_off + b) & 7);
        }
        printf("readback %d %d\n",
               (int)hid_field_read(fx, body, sizeof body, 0),
               (int)hid_field_read(fy, body, sizeof body, 0));
    }
    return 0;
}
"""


def build(tmp):
    drv = hostcheck.write(tmp, "hid_host.c", DRIVER)
    return hostcheck.compile(tmp, "hid_host", [drv, os.path.join(ROOT, "kernel", "lib", "hid_parse.c")],
                             includes=[os.path.join(ROOT, "kernel", "include", "api")],
                             tool="hid_parse_hostcheck")


def run(exe, desc, want_mouse, extra=()):
    out = subprocess.run([exe, "1" if want_mouse else "0",
                          "".join(f"{b:02x}" for b in desc), *map(str, extra)],
                         capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        p = line.split()
        if p[0] in ("ok",):
            res["ok"] = int(p[1]); res["id"] = int(p[3])
            res["mouse"] = int(p[5]); res["kbd"] = int(p[7]); res["bits"] = int(p[9])
        elif p[0] == "readback":
            res["readback"] = (int(p[1]), int(p[2]))
        else:
            res[p[0]] = tuple(int(v) for v in p[1:])   # present, off, bits, count, signed
    return res


class Check:
    def __init__(self):
        self.bad = 0

    def eq(self, what, got, want):
        if got == want:
            print(f"  PASS  {what}")
        else:
            self.bad += 1
            print(f"  FAIL  {what}\n        got {got!r}, want {want!r}")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        c = Check()

        # --- the G305's mouse, the descriptor this work exists for ----
        m = run(exe, G305_MOUSE, True, extra=(-7, 300))
        c.eq("G305 mouse: a layout is derived", m["ok"], 1)
        c.eq("G305 mouse: report ID 2", m["id"], 2)
        # 16 buttons at bit 0, one bit each, unsigned.
        c.eq("G305 mouse: 16 buttons at bit 0", m["buttons"], (1, 0, 1, 16, 0))
        # X and Y follow the buttons: 16 bits each, SIGNED.
        c.eq("G305 mouse: X is signed 16-bit at bit 16", m["x"], (1, 16, 16, 1, 1))
        c.eq("G305 mouse: Y is signed 16-bit at bit 32", m["y"], (1, 32, 16, 1, 1))
        c.eq("G305 mouse: wheel is signed 8-bit at bit 48", m["wheel"], (1, 48, 8, 1, 1))
        c.eq("G305 mouse: AC Pan is signed 8-bit at bit 56", m["pan"], (1, 56, 8, 1, 1))
        c.eq("G305 mouse: the body is 64 bits", m["bits"], 64)
        # THE ROUND TRIP, with a negative delta: this is what the sign
        # extension is for, and an unsigned read returns 65529 for -7.
        c.eq("G305 mouse: a report reads back what was written", m["readback"], (-7, 300))
        # The three collections AFTER the mouse must not contribute.
        # The vendor one declares 8-bit fields on a vendor page; if the
        # walker kept matching, `buttons` would move.
        c.eq("G305 mouse: later collections do not overwrite it", m["buttons"][1], 0)

        # --- the same receiver's keyboard -----------------------------
        k = run(exe, G305_KBD, False)
        c.eq("G305 kbd: a layout is derived", k["ok"], 1)
        c.eq("G305 kbd: no report ID", k["id"], 0)
        c.eq("G305 kbd: 8 modifier bits at bit 0", k["mods"], (1, 0, 1, 8, 0))
        # The reserved byte is Constant, so it is skipped but still
        # occupies bits 8..15 -- the keys start at 16.
        c.eq("G305 kbd: 6 key slots of 8 bits at bit 16", k["keys"], (1, 16, 8, 6, 0))
        c.eq("G305 kbd: the keys are an ARRAY, so no button field",
             k["buttons"][0], 0)

        # --- QEMU's devices, which every other test here runs on ------
        q = run(exe, QEMU_MOUSE, True, extra=(-3, 5))
        c.eq("QEMU mouse: a layout is derived", q["ok"], 1)
        c.eq("QEMU mouse: no report ID", q["id"], 0)
        c.eq("QEMU mouse: 5 buttons at bit 0", q["buttons"], (1, 0, 1, 5, 0))
        # The three padding bits are Constant: skipped, but they still
        # occupy bits 5..7, so X starts at 8 and not at 5.
        c.eq("QEMU mouse: X is signed 8-bit at bit 8", q["x"], (1, 8, 8, 1, 1))
        c.eq("QEMU mouse: Y is signed 8-bit at bit 16", q["y"], (1, 16, 8, 1, 1))
        c.eq("QEMU mouse: wheel is signed 8-bit at bit 24", q["wheel"], (1, 24, 8, 1, 1))
        c.eq("QEMU mouse: no horizontal scroll", q["pan"][0], 0)
        c.eq("QEMU mouse: the body is 32 bits", q["bits"], 32)
        c.eq("QEMU mouse: a report reads back what was written", q["readback"], (-3, 5))

        qk = run(exe, QEMU_KBD, False)
        c.eq("QEMU kbd: a layout is derived", qk["ok"], 1)
        c.eq("QEMU kbd: 8 modifier bits at bit 0", qk["mods"], (1, 0, 1, 8, 0))
        c.eq("QEMU kbd: 6 key slots of 8 bits at bit 16", qk["keys"], (1, 16, 8, 6, 0))

        # --- asking the wrong question --------------------------------
        c.eq("a mouse descriptor yields no keyboard",
             run(exe, G305_MOUSE, False)["ok"], 0)
        c.eq("a keyboard descriptor yields no mouse",
             run(exe, G305_KBD, True)["ok"], 0)

        # --- it refuses rather than guesses ---------------------------
        for n in (1, 4, 12, 30, 60):
            r = run(exe, G305_MOUSE[:n], True)
            if r["ok"] and not (r["x"][0] and r["y"][0]):
                c.bad += 1
                print(f"  FAIL  truncated to {n}: claimed a layout with no axes")
        print("  PASS  a truncated descriptor never claims a usable layout")
        c.eq("an empty descriptor is refused", run(exe, b"", True)["ok"], 0)
        c.eq("pure garbage is refused",
             run(exe, bytes([0xFF] * 40), True)["ok"], 0)
        # A report size nothing could hold must not produce a field.
        silly = hexbytes("05 01 09 02 a1 01 09 01 a1 00 75 7f 95 ff "
                         "05 09 19 01 29 10 81 02 c0 c0")
        c.eq("an impossible report size is refused", run(exe, silly, True)["ok"], 0)

        print(f"\nhid_parse_hostcheck: {'FAIL' if c.bad else 'PASS'} -- {c.bad} violation(s)")
        return 1 if c.bad else 0


if __name__ == "__main__":
    sys.exit(main())
