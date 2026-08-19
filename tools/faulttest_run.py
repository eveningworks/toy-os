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

Each test gets its OWN QEMU, because a ring-3 crash takes the serial
debug console down with it (tools/vm.py cannot drive these at all -- it
gets no response to anything afterwards, `crash_test` included). The
command is typed at the PHYSICAL shell over QMP instead, and the result
is read out of the serial log as text.

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
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_test import launch_qemu_cmd  # noqa: E402
from shell_flow import ShellFlow  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (binary, [required substrings], [forbidden substrings]).
#
# Matched against the whole serial log, so a required string that also
# appears during boot would pass vacuously -- none of these do, and a
# new entry should be checked against a plain boot log before being
# trusted. The forbidden list is where the specificity lives: dropping
# it turns every entry into "something crashed", which every one of
# these binaries satisfies by construction.
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
]

# `stack_smash_test` is deliberately NOT here. Its report comes from
# userland/rt/stack_chk.c via sys_write(1, ...) -- the process's own
# stdout, which for a shell-run program is the physical console and not
# the serial port -- so there is nothing in the log to assert on. Adding
# it would mean either reading the screen (OCR, which this repo avoids)
# or making the runtime log to stderr, which is a change to the thing
# under test rather than to the test. It stays a manual check until one
# of those is worth doing.

# How long to wait after pressing Enter before reading the log. A fault
# is reported synchronously, but the binary has to be read off the disk
# and loaded first, and `stackovf_test` recurses a few thousand times
# under TCG before it gets there.
SETTLE = 6.0

# The physical shell is ready once this has appeared. Same substring
# boot_smoke_test.py's own PASS_PATTERNS end on.
BOOT_MARKER = "scheduler initialized"
BOOT_TIMEOUT = 40.0


def read_log(path):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        return f.read()


def wait_for_boot(log_path, deadline):
    while time.time() < deadline:
        if BOOT_MARKER in read_log(log_path):
            return True
        time.sleep(0.3)
    return False


def run_one(name, required, forbidden, workdir, slot, keep_logs):
    """Boots one guest, types `run <name>` at the physical shell, and
    returns (ok, detail, log_text)."""
    disk = os.path.join(workdir, f"disk{slot}.img")
    # --sparse=always: disk.img is a few MB of data in a 9 GB sparse
    # file, and a hole-filling copy costs the full 9 GB of the
    # destination filesystem (tmpfs, i.e. RAM, here).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    os.path.join(REPO, "disk.img"), disk], check=True)

    serial_log = os.path.join(workdir, f"serial{slot}.log")
    pidfile = os.path.join(workdir, f"qemu{slot}.pid")
    cmd = launch_qemu_cmd(iso=os.path.join(REPO, "toy-os.iso"), disk=disk,
                           serial_log=serial_log, qmp_port=4445 + slot,
                           vnc_display=5 + slot, pidfile=pidfile)
    subprocess.run(cmd, shell=True, check=True, cwd=REPO)

    try:
        if not wait_for_boot(serial_log, time.time() + BOOT_TIMEOUT):
            return False, f"never reached '{BOOT_MARKER}' within {BOOT_TIMEOUT:.0f}s", ""

        flow = ShellFlow(qmp_port=4445 + slot)
        flow.type_command(f"run {name}")
        flow.session.send_key("ret")
        time.sleep(SETTLE)

        log = read_log(serial_log)
        missing = [s for s in required if s not in log]
        present = [s for s in forbidden if s in log]
        if missing:
            return False, f"log never said {missing[0]!r}", log
        if present:
            return False, f"log said {present[0]!r}, which this test forbids", log
        return True, required[0] if required else "ok", log
    finally:
        if keep_logs:
            os.makedirs(keep_logs, exist_ok=True)
            shutil.copyfile(serial_log, os.path.join(keep_logs, f"{name}.log"))
        # Kill only the PID our own launch wrote -- never a pattern
        # match across every qemu-system-x86_64, which cannot tell this
        # guest apart from an interactive `make run` the user has open.
        try:
            with open(pidfile) as f:
                os.kill(int(f.read().strip()), 15)
        except (OSError, ValueError):
            pass


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
                ok, detail, _ = run_one(name, req, forb, workdir, slot, args.keep_logs)
            except Exception as e:  # a harness failure is not a pass -- see below
                ok, detail = False, f"harness error: {e}"
            print("ok" if ok else "FAIL")
            if not ok:
                print(f"  {'':20s}     {detail}")
                failures += 1

    print()
    print(f"faulttest: {len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
