#!/usr/bin/env python3
"""Check the shipped Terminal colour schemes against what the loader does.

NO GUEST. A `.scheme` file is written in ANSI order -- black, red,
green, yellow, blue, magenta, cyan, white, then the eight bright forms,
which is the order every published palette is written in -- and a cell
in the emulator holds a VGA index, where blue is 1 and red is 4.
`userland/term/term_conf.c` permutes between the two through the
kernel parser's own `ansi_color()`.

That permutation is the part nobody can check by looking, and it has
already been wrong once: `Foreground=`/`Background=` name a `Color<N>`
like everything else in the file, and taking one as a VGA slot put
Solarized Dark's foreground on light red instead of base0. It rendered,
it looked like a colour scheme, and it was not the one in the file.

So this reimplements the permutation from the ANSI table INDEPENDENTLY
-- the point being that it shares no code with term_conf.c -- and
reports what each scheme actually resolves to, refusing a file whose
keys are missing, malformed or out of range.

    python3 tools/term_scheme_hostcheck.py
    python3 tools/term_scheme_hostcheck.py --positive-control
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCHEME_DIR = os.path.join(REPO, "data/usr/share/terminal")

# kernel/lib/ansi.c's ANSI_TO_VGA, written out rather than parsed: an
# oracle that read the table it is checking would agree with it by
# construction. If ansi.c's table ever changes, this must fail and be
# updated by hand -- that is the point of it being a second copy.
ANSI_TO_VGA = [0, 4, 2, 6, 1, 5, 3, 7]
VGA_NAMES = ["black", "blue", "green", "cyan", "red", "magenta", "brown", "lt grey",
             "dk grey", "lt blue", "lt green", "lt cyan", "lt red", "lt magenta",
             "yellow", "white"]


def vga_slot(ansi_index):
    """Where Color<ansi_index> lands in the emulator's palette."""
    return ANSI_TO_VGA[ansi_index & 7] + (8 if ansi_index >= 8 else 0)


def parse(path):
    keys = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            return None, f"not a name=value line: {line!r}"
        k, v = line.split("=", 1)
        keys[k.strip()] = v.strip()
    return keys, None


def check(path, control=False):
    """Returns (ok, [messages])."""
    name = os.path.basename(path)
    msgs = []
    keys, err = parse(path)
    if err:
        return False, [f"{name}: {err}"]

    for required in ("Name", "Foreground", "Background", "Cursor"):
        if required not in keys:
            return False, [f"{name}: no {required}="]

    pal = {}
    for i in range(16):
        k = f"Color{i}"
        if k not in keys:
            return False, [f"{name}: no {k}="]
        v = keys[k]
        if not re.fullmatch(r"[0-9a-fA-F]{6}", v):
            return False, [f"{name}: {k}={v!r} is not six hex digits"]
        pal[vga_slot(i)] = v.lower()

    if not re.fullmatch(r"[0-9a-fA-F]{6}", keys["Cursor"]):
        return False, [f"{name}: Cursor={keys['Cursor']!r} is not six hex digits"]

    ok = True
    for which in ("Foreground", "Background"):
        v = keys[which]
        if not re.fullmatch(r"\d+", v) or not 0 <= int(v) <= 15:
            return False, [f"{name}: {which}={v!r} is not a Color number 0..15"]
        slot = vga_slot(int(v))
        rgb = pal[slot]
        msgs.append(f"    {which:<10} Color{int(v):<2} -> vga {slot:<2} ({VGA_NAMES[slot]:<10}) #{rgb}")

    fg = pal[vga_slot(int(keys["Foreground"]))]
    bg = pal[vga_slot(int(keys["Background"]))]
    # THE ONE CHECK THAT IS ABOUT THE RESULT RATHER THAN THE FORMAT: a
    # scheme whose default pair is the same colour draws invisible text,
    # which parses perfectly and is unusable.
    if fg == bg:
        msgs.append(f"    default pair is #{fg} on #{bg} -- invisible")
        ok = False
    if control:
        # The positive control: pretend the permutation is the identity,
        # which is the bug this file was written for. Every scheme whose
        # default pair is not already at 0/7 must then move.
        naive_fg = pal[int(keys["Foreground"])]
        if naive_fg != fg:
            msgs.append(f"    CONTROL: an unpermuted read would give #{naive_fg}, not #{fg}")
            ok = False
    return ok, msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dir", default=SCHEME_DIR)
    ap.add_argument("--positive-control", action="store_true",
                    help="also fail any scheme the ANSI->VGA permutation "
                         "actually moves, proving this can go red at all")
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.dir) if f.endswith(".scheme"))
    if not files:
        print(f"term_scheme_hostcheck: FAIL -- no .scheme files in {args.dir}")
        return 1

    bad = 0
    moved = 0
    for f in files:
        path = os.path.join(args.dir, f)
        ok, msgs = check(path, args.positive_control)
        print(f"  {'PASS' if ok else 'FAIL'}  {f}")
        for m in msgs:
            print(m)
            if "CONTROL:" in m:
                moved += 1
        if not ok:
            bad += 1

    if args.positive_control:
        # A control that reddens nothing has measured nothing -- the
        # same rule damage_sweep.py states.
        if moved == 0:
            print("\nterm_scheme_hostcheck: CONTROL FAILED -- the permutation moved "
                  "no scheme's default pair, so a clean run proves nothing")
            return 1
        print(f"\nterm_scheme_hostcheck: control OK -- {moved} default colour(s) "
              "would be wrong without the permutation")
        return 0

    if bad:
        print(f"\nterm_scheme_hostcheck: FAIL -- {bad} of {len(files)} scheme(s) are wrong")
        return 1
    print(f"\nterm_scheme_hostcheck: PASS -- {len(files)} scheme(s), every colour "
          "resolves and no default pair is invisible")
    return 0


if __name__ == "__main__":
    sys.exit(main())
