#!/usr/bin/env python3
"""Refuse a driver that declares itself to nothing, and a driver file
that never says which of the two it is.

WHY THIS EXISTS. `lsdrv` answers "which drivers does this build have,
and what is each one driving" -- the question no per-class registry
answers, because a driver that bound nothing appears in none of them.
That only works if every driver declares itself, and on 2026-09-01
three did not: kernel/drivers/input/i8042.c (the PS/2 keyboard and
mouse, the input path every default boot uses), virtio_input.c and
virtio_rng.c. Nothing noticed, because nothing was looking.

The same audit found the inverse and worse: DRIVER_REGISTER used to be
a CALL inside a driver's init(), so whether a driver appeared depended
on where in the probe somebody put the line. e1000's was after the "no
card on this bus" return, so a build containing the driver listed no
driver -- the exact case lsdrv exists to answer. A declaration is data
now (DRIVER_DECLARE, into the .drivers linker section) and cannot be
skipped by a return, which is the half no checker was ever going to
catch. This checks the half that is left: that the declaration is
there at all.

THE RULES:

  1. Every .c under kernel/drivers/ must contain either a
     DRIVER_DECLARE(...) or a `driver-none: <reason>` comment. Most
     files there are not drivers -- a class registry, a query provider,
     a shared transport -- and saying so once is the point: "this is
     deliberately not a driver" is a fact worth stating where the next
     person looks.

  2. Any .c ANYWHERE that registers with a device class must too. That
     is what catches a driver outside kernel/drivers/ -- the TSC
     clocksource lives in kernel/arch/x86_64/ and is as much a driver
     as the AHCI one.

Test files (*_test.c) are exempt from rule 1: a KTEST beside a driver
is not a second driver, and the exemption is mechanical enough not to
need a marker on every one.

Waive with a `driver-none: <reason>` comment, the same shape as
check_dispatch.py's `dispatch-ok:` and check_widget_ops.py's
`widget-ops-ok:`. The reason is the mechanism, not the waiver.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRIVERS_DIR = ROOT / "kernel" / "drivers"
KERNEL_DIR = ROOT / "kernel"

# The entry point of every device class registry. A file that calls one
# is registering a device, which means something drove it.
REGISTRY_CALLS = [
    "blk_register",
    "net_register",
    "input_register_source",
    "sound_register",
    "display_register",
    "clocksource_register",
    "krandom_register_source",
]
REGISTRY_RE = re.compile(r"\b(" + "|".join(REGISTRY_CALLS) + r")\s*\(")

DECLARE_RE = re.compile(r"\bDRIVER_DECLARE\s*\(")
WAIVER_RE = re.compile(r"driver-none:\s*(\S.*)")
COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def code_of(text):
    """`text` with comments blanked out. A file that merely NAMES
    net_register() in a comment is not calling it -- kernel.c does
    exactly that, and matching it sends the reader at the wrong file."""
    return COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def declares_or_waives(text):
    """(declared, waived) for one file's source. The declaration must be
    real code; the waiver must be a comment, which is where it lives."""
    return bool(DECLARE_RE.search(code_of(text))), bool(WAIVER_RE.search(text))


def main():
    failures = []

    for path in sorted(KERNEL_DIR.rglob("*.c")):
        rel = path.relative_to(ROOT)
        text = path.read_text(errors="replace")
        declared, waived = declares_or_waives(text)
        if declared or waived:
            continue

        under_drivers = DRIVERS_DIR in path.parents
        is_test = path.name.endswith("_test.c")

        if under_drivers and not is_test:
            failures.append((rel, "is under kernel/drivers/ and says neither"))
            continue

        m = REGISTRY_RE.search(code_of(text))
        if m and not is_test:
            failures.append((rel, f"calls {m.group(1)}() and says neither"))

    if not failures:
        print("check_drivers: OK")
        return 0

    print(f"check_drivers: FAIL -- {len(failures)} file(s)\n")
    for rel, why in failures:
        print(f"  {rel}: {why}")
    print("\nA driver declares itself at FILE SCOPE:")
    print('    DRIVER_DECLARE("ahci", "block", "SATA AHCI host controller");')
    print("Anything else says so, once, in a comment:")
    print("    // driver-none: the block class registry, not a driver")
    return 1


if __name__ == "__main__":
    sys.exit(main())
