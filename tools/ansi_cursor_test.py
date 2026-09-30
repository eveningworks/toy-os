#!/usr/bin/env python3
"""ANSI cursor movement and erasing, checked as PIXELS on the console.

kernel/lib/ansi.c is a pure state machine and its KTESTs assert what a
sequence RESOLVES to with no display at all. This is the other half:
whether kernel/drivers/vga.c then puts ink in the right cell. Only
pixels can answer that, which is why this is a Python tool and not a
KTEST.

THE DESKTOP OWNS THE SCREEN, so it has to go first. This deletes the
toywm service descriptor and kills the process -- the same
`unsupervise then kill` pattern compositor_death_test.py uses, and for
the same reason: init restarts a killed desktop with a zero backoff, so
killing it without removing the descriptor races a replacement that has
already re-claimed the framebuffer. The image is a throwaway copy, so
deleting a seeded file costs nothing.

EVERY CHECK HAS A BLANK NEIGHBOUR. "Something was drawn" is satisfied by
a console that ignores cursor movement and prints everything in a row;
only the gaps tell that apart from working. Run on demand:

    python3 tools/ansi_cursor_test.py [--instance auto]
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import QMPSession                          # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
VM = os.path.join(HERE, "vm.py")


def vm(*argv, check=False):
    """One shell command in the guest, through tools/vm.py.

    Built on vm.py rather than on launch_qemu_cmd() directly because
    vm.py already owns the pieces this needs -- a booted guest, a serial
    socket to talk to it on, and a QMP port to photograph it through --
    and rederiving that here would be a second copy of the launch dance
    the module docstring of qmp_test.py exists to prevent.
    """
    r = subprocess.run([sys.executable, VM] + list(argv),
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout + r.stderr)
    return r.stdout + r.stderr


TOYWM_SVC = "/etc/services.d/toywm"
CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))


def ink_bands(values):
    """Contiguous runs of True in `values`, as (start, end) pairs."""
    out = []
    for i, v in enumerate(values):
        if not v:
            continue
        if out and i == out[-1][1] + 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return out


def calibrate(img, fallback):
    """Derive the cell size from the pattern rather than assuming it.

    Rows 3 and 6 of the pattern both carry ink and are three cells
    apart; the two marks on row 3 are four columns apart. Both come out
    of the same image the checks then read, so a font change cannot put
    the two out of step.

    Falls back to `fallback` (and says so) if the pattern is not there
    at all -- in which case every check is about to fail anyway, and
    failing on the checks is more informative than failing here.
    """
    w, h = img.size
    bg = img.getpixel((0, 0))
    rows = ink_bands([any(img.getpixel((x, y)) != bg for x in range(0, min(w, 240)))
                      for y in range(h)])
    cols = ink_bands([any(img.getpixel((x, y)) != bg for y in range(0, min(h, 240)))
                      for x in range(min(w, 240))])
    if len(rows) < 2 or len(cols) < 2:
        return fallback
    ch = (rows[1][0] - rows[0][0]) // 3
    # The second column band is the row-3 mark at column 9; the first
    # band starts at column 1. Eight columns between their left edges.
    cw = (cols[1][0] - cols[0][0]) // 8
    if ch < 4 or cw < 4:
        return fallback
    return cw, ch


def cell_has_ink(img, cw, ch, row, col, bg):
    """True if any pixel in the 1-BASED cell (row, col) differs from `bg`.

    A margin is trimmed off every edge: a glyph does not fill its cell,
    and neighbouring cells' antialiasing (and the cursor block's border,
    when one is showing) can bleed a pixel. Sampling the middle asks
    about THIS cell rather than about the seam.
    """
    x0 = (col - 1) * cw
    y0 = (row - 1) * ch
    mx = max(1, cw // 6)
    my = max(1, ch // 6)
    for y in range(y0 + my, y0 + ch - my):
        for x in range(x0 + mx, x0 + cw - mx):
            if img.getpixel((x, y)) != bg:
                return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the VM running")
    # The console's cell size, which vga.c derives from gfx_char_w/h.
    # Passed rather than probed because there is no shell command that
    # reports it, and a wrong guess fails LOUDLY here (every cell reads
    # blank) rather than quietly.
    # Only a FALLBACK now: the cell size is derived from the pattern
    # (see calibrate()), because this project's layout is font-derived
    # and a hardcoded size is exactly the kind of constant that rots.
    ap.add_argument("--cell", nargs=2, type=int, default=(8, 14),
                    metavar=("W", "H"))
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("ansi_cursor_test: needs Pillow (pip install pillow)")
        return 2

    vm("stop")
    print("booting")
    vm("start", check=True)
    try:
        # Take the desktop out of init's hands, then kill it, so the
        # physical console is what the framebuffer shows.
        # Copied aside and put back in the finally: the image is shared
        # with every later tool in a sweep (2026-09-30).
        vm("exec", f"cp {TOYWM_SVC} /var/tmp/toywm.service.saved")
        vm("exec", f"rm {TOYWM_SVC}")
        ps = vm("exec", "ps")
        pid = None
        for line in ps.splitlines():
            if "toywm" in line:
                pid = line.split()[0]
                break
        check("found the desktop to stop", pid is not None, ps.strip()[:60])
        if pid:
            vm("exec", f"kill {pid}")
            time.sleep(1.5)

        vm("exec", "spawn /tests/ansidraw")
        time.sleep(2.0)

        session = QMPSession(port=4445)
        shot = os.path.abspath("/tmp/ansi_cursor.png")
        session.screenshot(shot)
        img = Image.open(shot).convert("RGB")

        # SELF-CALIBRATING, and it has to be. The console's cell size is
        # FONT-DERIVED (gfx_char_w/h), which this project treats as a
        # one-line change -- so a hardcoded 8x16 here would silently
        # start reading the wrong cells the day the default font size
        # moves. It did: the first version assumed 16 and the real
        # height is 14, which showed up as every row after the third
        # being "one row high" while the kernel was correct.
        #
        # The PATTERN carries the calibration. The marks are three rows
        # apart by construction, so the gap between the first two ink
        # bands is three cells; the two marks on the first row are four
        # columns apart.
        cw, ch = calibrate(img, args.cell)
        print(f"  note   calibrated to {cw}x{ch} cells from the pattern itself")
        # The background is read from a corner AFTER the clear rather
        # than assumed, so a theme change cannot silently make every
        # check pass by making everything look blank.
        bg = img.getpixel((cw // 2, ch // 2))

        def ink(r, c):
            return cell_has_ink(img, cw, ch, r, c, bg)

        # --- absolute positioning, with a gap ---
        check("a mark landed at row 3 col 5", ink(3, 5))
        check("and another at col 9", ink(3, 9))
        check("with the cells BETWEEN them blank",
              not ink(3, 6) and not ink(3, 7) and not ink(3, 8))

        # --- erase to end of line ---
        check("row 6 columns 1-4 survived the erase",
              ink(6, 1) and ink(6, 2) and ink(6, 3) and ink(6, 4))
        check("and columns 5-8 were erased",
              not ink(6, 5) and not ink(6, 6) and not ink(6, 7) and not ink(6, 8))

        # --- relative movement ---
        check("a relative move landed on row 9 col 5", ink(9, 5))
        check("with columns 1-4 blank",
              not ink(9, 1) and not ink(9, 2) and not ink(9, 3) and not ink(9, 4))

        # --- an explicit zero is a default ---
        check("ESC[0A moved up exactly one row (row 11, not 12)",
              ink(11, 3) and not ink(12, 3))

        # --- the clear actually cleared ---
        check("the screen above the pattern is blank",
              not ink(1, 1) and not ink(2, 1) and not ink(1, 40))
    finally:
        vm("exec", f"cp /var/tmp/toywm.service.saved {TOYWM_SVC}")
        if not args.keep:
            vm("stop")

    passed = sum(1 for _, ok, _ in CHECKS if ok)
    print(f"\nansi_cursor_test: {passed}/{len(CHECKS)} checks passed")
    return 0 if passed == len(CHECKS) else 1


if __name__ == "__main__":
    sys.exit(main())
