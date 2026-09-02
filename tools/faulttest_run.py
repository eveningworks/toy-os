#!/usr/bin/env python3
"""Runs the ring-3 diagnostics that FAULT ON PURPOSE, and asserts on what
the kernel said about each one.

tools/usertest_run.py excludes every one of these with a good reason --
a binary that faults has no exit code and no output of its own to check,
so a "just run everything" harness reports it as a failure of the code
under test. The consequence was that nothing ran them at all: five
binaries whose whole job is to exercise the fault path, and the only way
to see the result was a person typing `run nx_test` and reading the
screen. This is the missing half.

The assertion is the KERNEL's report, not the process's. Each entry
below names a binary plus the substrings the serial log must and must
not contain afterwards, which is exactly the distinction the reports are
for: a stack overflow and a null dereference are both page faults, and a
kernel that calls them both "Page fault" is the thing this catches. Every
entry therefore doubles as the positive control for its neighbours --
`stackovf_test` requires "Stack overflow" AND requires that
`crash_test` does not produce it.

Each test gets its OWN QEMU, because a ring-3 crash takes the debug
console's command loop down with it -- the report still reaches the wire,
but nothing after it does.

THE COMMAND GOES OVER COM1, NOT THE KEYBOARD, and that is the whole
reason this tool works at all. It used to type `run <name>` at the
physical shell over QMP, which stopped working the day the desktop began
starting at boot: a compositor holding the role parks every ring-0
blocking reader (kernel/proc/win_server.c's keyboard_suspend_blocking()),
so the keystrokes went to the desktop and all three entries failed
identically, having never run. A serial console does not care who owns
the screen -- the same reason `console=ttyS0` is what kernel developers
drive a Linux guest with. See tools/serial_console.py.

Usage:
    python3 tools/faulttest_run.py               # against a copy of disk.img
    python3 tools/faulttest_run.py -k stackovf   # only matching tests
    python3 tools/faulttest_run.py --list        # what's in it
    python3 tools/faulttest_run.py --keep-logs D # keep each serial log

Exits 0 if every test's report matched, 1 otherwise, 2 if it could not
run at all.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_console import DEFAULT_PORT, SerialGuest  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (binary, [required substrings], [forbidden substrings]).
#
# Matched against what arrives AFTER the command is sent, never against
# the whole transcript. That is what stops an entry passing vacuously on
# a string the kernel happens to print during boot -- the old whole-log
# match left that as a hazard to remember rather than a thing the tool
# could not do. The forbidden list is where the specificity lives:
# dropping it turns every entry into "something crashed", which every
# one of these binaries satisfies by construction.
TESTS = [
    ("stackovf_test",
     ["RING-3 CRASH: Stack overflow"],
     ["RING-3 CRASH: Page fault"]),
    # The control for the entry above, and the reason it means anything:
    # this faults on a null pointer, one page from the bottom of the
    # address space and nowhere near the stack guard, so it must be
    # reported as an ordinary page fault.
    ("crash_test",
     ["RING-3 CRASH: Page fault"],
     ["Stack overflow"]),
    # W^X in userspace: jumping into a data page must fault as such
    # rather than executing whatever bytes were there.
    ("nx_test",
     ["RING-3 CRASH: Page fault"],
     ["Stack overflow"]),
    # THE CRASH REPORT needs a pid, which the legacy `run` loader has
    # not got -- so this entry spawns. The kernel names the file it
    # wrote; the forbidden string is the refusal the legacy path logs.
    ("spawn /tests/crash_test",
     ["RING-3 CRASH: Page fault", "crash: report written to /var/crash/crash_test-"],
     ["crash: no report"]),
]

# `stack_smash_test` is deliberately NOT here. Its report comes from
# userland/rt/stack_chk.c via sys_write(1, ...) -- the process's own
# stdout, which for a shell-run program is the physical console and not
# the serial port -- so there is nothing in the log to assert on. Adding
# it would mean either reading the screen (OCR, which this repo avoids)
# or making the runtime log to stderr, which is a change to the thing
# under test rather than to the test. It stays a manual check until one
# of those is worth doing.

# How long to keep reading after sending the command. A fault is
# reported synchronously, but the binary has to be read off the disk and
# loaded first, and `stackovf_test` recurses a few thousand times under
# TCG before it gets there. The wait ENDS EARLY on the required string,
# so this is the ceiling on a failing run, not the cost of a passing one.
SETTLE = 15.0

# The debug console has announced itself and is accepting commands.
# NOT the boundary the window starts at -- see QUIET_FOR.
BOOT_MARKER = "debug console ready"

# How long the guest must say nothing before the command is sent. The
# banner above arrives early and the kernel keeps printing for seconds
# after it (init, the desktop, the cursor theme), so a window anchored
# on the banner still contains boot output -- and a control proved it,
# passing an entry that required a string only the BOOT prints.
QUIET_FOR = 1.5
BOOT_TIMEOUT = 60.0


def run_one(name, required, forbidden, workdir, slot, keep_logs):
    """Boots one guest, sends `sh run <name>` over COM1, and returns
    (ok, detail, facts) -- `facts` being the guest diagnostics, sampled
    while it is still alive. Single exit on purpose: sampling them after
    the teardown in `finally` reports this harness killing its own QEMU
    rather than anything about the run."""
    disk = os.path.join(workdir, f"disk{slot}.img")
    # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
    # file, and a hole-filling copy costs the full 9 GB of the
    # destination filesystem (tmpfs, i.e. RAM, here).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    os.path.join(REPO, "disk.img"), disk], check=True)

    guest = SerialGuest(os.path.join(REPO, "toy-os.iso"), disk,
                        port=DEFAULT_PORT + slot,
                        qemu_log=os.path.join(workdir, f"qemu{slot}.log"))
    ok, detail = False, "did not run"
    try:
        guest.start()
        if not guest.connect(time.time() + BOOT_TIMEOUT):
            detail = "QEMU's serial socket never accepted a connection"
        elif not guest.wait_for(BOOT_MARKER, time.time() + BOOT_TIMEOUT):
            detail = f"never reached {BOOT_MARKER!r} within {BOOT_TIMEOUT:.0f}s"
        elif not guest.wait_quiet(QUIET_FOR, time.time() + BOOT_TIMEOUT):
            detail = f"the guest never went quiet for {QUIET_FOR:.1f}s -- boot output would land in the window"
        else:
            # Everything from here on is this test's evidence. Anything
            # the boot printed is behind us and cannot satisfy an
            # assertion -- which is what stops a vacuous pass.
            before = len(guest.transcript)
            # An entry may carry its own verb ("spawn /tests/x"); a bare
            # name goes through the legacy loader as before.
            if not guest.send(f"sh {name}" if " " in name else f"sh run {name}"):
                detail = "could not send the command"
            else:
                # Ends early once EVERY required string has arrived (the
                # first alone once ended a wait before the second was
                # printed), but otherwise reads to the ceiling: a
                # forbidden string arriving late still has to be caught.
                deadline = time.time() + SETTLE
                while time.time() < deadline and guest.sock is not None:
                    guest.pump()
                    if required and all(s in guest.transcript[before:] for s in required):
                        break

                window = guest.transcript[before:]
                missing = [s for s in required if s not in window]
                present = [s for s in forbidden if s in window]
                if missing:
                    detail = f"log never said {missing[0]!r}"
                elif present:
                    detail = f"log said {present[0]!r}, which this test forbids"
                else:
                    ok, detail = True, required[0] if required else "ok"

        facts = [] if ok else guest.diagnostics()
    finally:
        if keep_logs:
            os.makedirs(keep_logs, exist_ok=True)
            safe = "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in name)
            with open(os.path.join(keep_logs, f"{safe}.log"), "w") as f:
                f.write(guest.transcript)
        guest.stop()

    return ok, detail, facts


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", metavar="SUBSTR", help="only tests whose name contains SUBSTR")
    ap.add_argument("--list", action="store_true", help="list the tests and exit")
    ap.add_argument("--keep-logs", metavar="DIR", help="keep each guest's serial log here")
    args = ap.parse_args()

    tests = [t for t in TESTS if not args.k or args.k in t[0]]

    if args.list:
        for name, req, forb in TESTS:
            print(f"{name:20s} requires {req}")
            if forb:
                print(f"{'':20s} forbids  {forb}")
        return 0

    if not tests:
        print(f"faulttest: no test matches -k {args.k!r}", file=sys.stderr)
        return 2

    if not os.path.exists(os.path.join(REPO, "toy-os.iso")):
        print("faulttest: toy-os.iso not found -- run `make iso` first", file=sys.stderr)
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="faulttest-") as workdir:
        for slot, (name, req, forb) in enumerate(tests):
            print(f"  {name:20s} ... ", end="", flush=True)
            try:
                ok, detail, facts = run_one(name, req, forb, workdir, slot, args.keep_logs)
            except Exception as e:  # a harness failure is not a pass
                ok, detail, facts = False, f"harness error: {e}", []
            print("ok" if ok else "FAIL")
            if not ok:
                print(f"  {'':20s}     {detail}")
                for line in facts:
                    print(f"  {'':20s}     {line}")
                failures += 1

    print()
    print(f"faulttest: {len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
