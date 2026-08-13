#!/usr/bin/env python3
"""Fast, non-GUI boot verification: boots toy-os.iso headlessly in QEMU,
watches the serial log for the expected kernel init sequence, and exits
0/1 -- no QMP, no mouse/keyboard, no screenshots. Most kernel-level
changes (a new driver, a filesystem backend, a syscall) don't actually
need the full QMP GUI-testing dance (see qmp_test.py) to sanity-check;
they need "does it still boot cleanly to the point the shell/WM takes
over," which is what this answers, in a few seconds instead of a manual
screenshot-reading session.

This does NOT replace qmp_test.py for anything that touches rendering,
input, or window behavior -- a clean boot log says nothing about
whether a button is drawn in the right place. Use this as a fast first
check for kernel/driver-level changes, and qmp_test.py for GUI changes,
same division CLAUDE.md's testing section describes.

Usage:
    python3 tools/boot_smoke_test.py                 # uses toy-os.iso, disk.img
    python3 tools/boot_smoke_test.py --iso other.iso --timeout 15

Exit code 0 = every expected line appeared and no failure pattern did,
within the timeout. Exit code 1 = timed out missing something, or a
failure pattern (a kernel panic, or QEMU exiting/crashing outright)
showed up. Either way, prints a short report; add -v to also dump the
full serial log on failure for debugging.

No third-party dependencies -- doesn't need Pillow (screenshot()'s only
consumer), unlike qmp_test.py.
"""

import argparse
import os
import re
import subprocess
import sys
import time

# Every one of these substrings must appear in the serial log for a
# boot to count as successful. Deliberately loose (substring match, not
# exact-line match) so small wording tweaks to a serial_write() message
# don't spuriously break this -- see kernel/core/kernel.c's
# serial_write() calls for where each of these actually comes from.
PASS_PATTERNS = [
    "kernel_main reached, initializing",
    "GDT/TSS initialized",
    "IDT/PIC/PIT initialized, interrupts enabled",
    "physical frame allocator initialized",
    "fs: ",  # one of "loaded persistent"/"formatted a fresh"/"no disk found" -- all are fine, see fs_init()
    "scheduler initialized",
]

# Any of these appearing anywhere in the serial log fails the test
# immediately, even before the timeout -- no point waiting the full
# window out if the kernel already panicked. "PANIC: " is
# kernel/arch/x86_64/idt.c's unrecoverable-fault message (see
# idt.c's serial_write(recoverable ? "RING-3 CRASH: " : "PANIC: ", ...)
# -- deliberately NOT matching "RING-3 CRASH:" here, since that's the
# *recoverable* per-process fault path (ring3test/elftest trigger it on
# purpose and the kernel keeps running) and this smoke test never runs
# those commands during a plain boot anyway.
FAIL_PATTERNS = [
    "PANIC:",
]

DEFAULT_TIMEOUT = 12.0
POLL_INTERVAL = 0.2


def launch_qemu(iso, disk, serial_log, qemu_log):
    # -no-reboot -no-shutdown: a triple fault would otherwise just
    # silently reset the VM (or QEMU would exit) with nothing useful in
    # either log -- these two flags make QEMU stop and report instead,
    # so a boot that gets that badly wrong shows up as a clear failure
    # rather than a confusing timeout. No display head at all (unlike
    # qmp_test.py's launch_qemu_cmd(), which needs one for mouse input
    # routing) -- this test never sends QMP input, only reads the
    # serial log, so -display none is fine here (see qmp_test.py's
    # module docstring for why it's NOT fine when QMP input is
    # involved).
    cmd = [
        "qemu-system-x86_64",
        "-cdrom", iso,
        "-drive", f"file={disk},format=raw,if=ide",
        "-m", "256",
        "-display", "none",
        "-serial", f"file:{serial_log}",
        "-no-reboot",
        "-no-shutdown",
    ]
    qlog = open(qemu_log, "wb")
    return subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)


def tail(path):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    ap.add_argument("--serial-log", default="smoke_serial.log")
    ap.add_argument("--qemu-log", default="smoke_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true", help="dump full serial log on failure")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"boot_smoke_test: {args.iso} not found -- build it first (make iso)", file=sys.stderr)
        return 1

    # A fresh disk each run, same reasoning as `make run`'s $(DISK_IMG)
    # target -- this test always wants a known-clean 1MiB image, not
    # whatever a previous session's disk.img happens to hold.
    if not os.path.exists(args.disk):
        with open(args.disk, "wb") as f:
            f.truncate(1024 * 1024)

    for p in (args.serial_log, args.qemu_log):
        if os.path.exists(p):
            os.remove(p)

    proc = launch_qemu(args.iso, args.disk, args.serial_log, args.qemu_log)

    deadline = time.monotonic() + args.timeout
    seen_fail = None
    try:
        while time.monotonic() < deadline:
            log = tail(args.serial_log)

            for pat in FAIL_PATTERNS:
                if pat in log:
                    seen_fail = pat
                    break
            if seen_fail:
                break

            if all(pat in log for pat in PASS_PATTERNS):
                elapsed = args.timeout - (deadline - time.monotonic())
                print(f"boot_smoke_test: PASS -- all {len(PASS_PATTERNS)} expected lines seen in {elapsed:.1f}s")
                return 0

            if proc.poll() is not None:
                # QEMU exited on its own (crash, or -no-reboot/-no-shutdown
                # tripping on a triple fault) before we saw everything --
                # that's a failure, not a "keep waiting" situation.
                print(f"boot_smoke_test: FAIL -- qemu exited early (code {proc.returncode}) before boot completed",
                      file=sys.stderr)
                seen_fail = f"qemu exited early (code {proc.returncode})"
                break

            time.sleep(POLL_INTERVAL)
        else:
            log = tail(args.serial_log)

        if seen_fail:
            print(f"boot_smoke_test: FAIL -- saw failure indicator: {seen_fail}", file=sys.stderr)
        else:
            missing = [p for p in PASS_PATTERNS if p not in log]
            print(f"boot_smoke_test: FAIL -- timed out after {args.timeout}s, missing: {missing}", file=sys.stderr)

        if args.verbose:
            print("----- serial log -----", file=sys.stderr)
            print(log, file=sys.stderr)
            print("----- qemu log -----", file=sys.stderr)
            print(tail(args.qemu_log), file=sys.stderr)
        else:
            print("(rerun with -v to see the full serial/qemu logs)", file=sys.stderr)

        return 1
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
