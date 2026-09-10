#!/usr/bin/env python3
"""Runs the in-kernel test suite and turns it into a process exit code.

Boots toy-os.iso headlessly, drives the `ktest` command over the serial
debug console (COM1 -- see kernel/core/debug_console.c), reads the
report back off the same wire, and exits 0 if every test passed.

Why the serial console rather than the graphical shell: it needs no
display, no QMP, no keyboard emulation and no screenshot -- just a pipe
in and a pipe out. Same reasoning as tools/boot_smoke_test.py, which
this is modelled on, except that one only reads and this one also
writes. The socket, the launch flags and the read loop live in
tools/serial_console.py, shared with tools/faulttest_run.py.

    python3 tools/ktest_run.py                 # every test
    python3 tools/ktest_run.py --suite fs      # one suite
    python3 tools/ktest_run.py -v              # dump the full log

Exit code 0 = "ktest: PASSED" seen. 1 = a test failed, the report never
appeared within the timeout, or the kernel panicked on the way.

A run that never got a verdict prints WHY as facts -- how long it
waited, how many bytes arrived, whether QEMU is alive, what it last
said. That is not decoration: this harness has a recorded intermittent
(docs/bugs.md) whose two symptoms, a bare ConnectionResetError and a
verdict-free timeout, measure nothing and used to look like unrelated
problems.

Note this boots against the REAL disk.img by default, and the tests
write to it (creating and deleting /.ktest_tmp). Pass --disk to point at
a scratch copy if that matters.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_console import DEFAULT_PORT, SerialGuest  # noqa: E402

DEFAULT_TIMEOUT = 60.0


def fail(message, facts):
    """One failure shape: the verdict, then the facts behind it.

    Takes the facts already GATHERED rather than the guest, because
    diagnostics have to be read while the guest is still up -- sampling
    them after teardown reports "QEMU exited with code 0" for every
    failure, which is this harness's own teardown and says nothing
    about the run.
    """
    print(f"ktest_run: FAIL -- {message}")
    for line in facts:
        print(f"              {line}")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--suite", default="", help="run only this suite (e.g. fs, mm, lib)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                    help="seconds allowed for the boot, and again for the "
                         "suite from the moment it is sent (a hang guard, "
                         "not a budget for both)")
    ap.add_argument("--virtio-disk", default=None,
                    help="also attach PATH as a virtio-blk disk. With the `virtioblk` "
                         "boot flag baked into the ISO, the filesystem then lives on "
                         "virtio while the [ata] KTESTs keep the IDE drive.")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help="COM1's TCP port on the host")
    ap.add_argument("--mem", type=int, default=256, metavar="MIB",
                    help="guest RAM in MiB (default 256). 8192 is what makes the "
                         "mm suite's above-4-GiB check run rather than skip.")
    ap.add_argument("--qemu-log", default="ktest_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the whole serial transcript")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"ktest_run: {args.iso} not found -- build it first (make iso)")
        return 1

    guest = SerialGuest(args.iso, args.disk, port=args.port,
                        qemu_log=args.qemu_log, virtio_disk=args.virtio_disk,
                        memory=args.mem)
    verdict = None
    timeout_facts = []
    try:
        guest.start()
        deadline = time.time() + args.timeout

        # QEMU is waiting for this connection before it boots anything
        # (see serial_console.py's launch flags), so connecting is step
        # one and the transcript starts at the very first byte.
        if not guest.connect(deadline):
            return fail("QEMU's serial socket never accepted a connection", guest.diagnostics())

        if not guest.wait_for("debug console ready", deadline):
            return fail("the serial debug console never came up", guest.diagnostics())

        # FREE THE COMPOSITOR ROLE BEFORE THE SUITE. It is one global,
        # and on a graphical boot the ring-3 desktop holds it -- so the
        # winshare KTESTs refuse to take it (win_role_test.c) and the
        # gate would lose that coverage. `service stop` rather than
        # deleting the descriptor: init keeps `admin_stopped` in memory
        # and the request file is in /tmp, so nothing survives to the
        # next boot.
        #
        # THE WAIT IS THE TRAP: the debug console is up before init has
        # read /etc/services.d, and a stop sent then is answered
        # `supervises no service called toywm` while the desktop starts
        # anyway. A boot with no desktop never prints the line, and the
        # bounded wait falls through with the role already free.
        guest.wait_for("toywm is ready", min(deadline, time.time() + 20))
        guest.send("sh service stop toywm")
        guest.wait_for("toywm stopped", min(deadline, time.time() + 10))

        if not guest.send(f"ktest {args.suite}".strip()):
            return fail("could not send the ktest command", guest.diagnostics())

        # THE SUITE GETS ITS OWN BUDGET, from the moment it is sent. One
        # deadline from QEMU start covered the boot, the desktop wait,
        # the `service stop` AND the suite -- and once the suite reached
        # ~40s the verdict landed at second 61 on a slow boot and a
        # PASSED transcript was reported as "no verdict" (2 runs in 2,
        # 2026-09-10). A hang guard is sized against the thing it guards.
        deadline = time.time() + args.timeout

        while time.time() < deadline and guest.sock is not None:
            guest.pump()
            if "ktest: PASSED" in guest.transcript:
                verdict = True
                break
            if "ktest: FAILED" in guest.transcript or "KERNEL PANIC" in guest.transcript:
                verdict = False
                break

        # Drain whatever is still in flight before killing QEMU. The
        # verdict substring arrives mid-line -- the counts that follow it
        # ("14 passed, 0 failed, ...") are still on the wire -- so
        # breaking out and terminating immediately truncates the summary
        # this script then tries to print.
        guest.drain(1.0)

        # Sampled here, with the guest still alive -- see fail().
        if verdict is None:
            timeout_facts = guest.diagnostics()
    finally:
        guest.stop()

    transcript = guest.transcript
    if args.verbose:
        # THE WHOLE TRANSCRIPT, boot messages included -- which is what
        # this flag has always claimed and did not do.
        #
        # It used to apply the same filter as the failure report below,
        # so every line the KERNEL printed before the test suite ran --
        # `pci:`, `block:`, `tfs3: mounted`, every driver's bring-up --
        # was silently dropped. That cost real time: a CI run was
        # diagnosed as "virtio-blk never came up on the runner" purely
        # because grepping this output for `block: virtio-blk active`
        # found nothing, and the same grep found nothing locally on a
        # run where virtio demonstrably WAS carrying the filesystem.
        # A verbose flag that filters is worse than no verbose flag.
        print(transcript)
    elif verdict is not True:
        # On failure the interesting part is the per-test lines, which
        # are the whole point of having a report rather than a bare code.
        for line in transcript.splitlines():
            if line.startswith(("ktest:", "  [", "    ")) or "FAIL" in line or "PANIC" in line:
                print(line)

    if verdict is True:
        # A suite that SKIPPED the tests this tool went out of its way to
        # enable is not a pass, it is a silent loss of coverage -- the
        # exact shape this whole change exists to stop.
        if "a compositor holds the role" in transcript:
            return fail("the compositor role was never freed, so the winshare "
                        "KTESTs skipped -- `service stop toywm` did not take "
                        "effect before the suite ran",
                        [l for l in transcript.splitlines()
                         if "compositor holds the role" in l or "toywm" in l][:6])

        summary = next((l for l in transcript.splitlines() if l.startswith("ktest: PASSED")), "")
        print(f"ktest_run: PASS -- {summary.replace('ktest: PASSED -- ', '')}")
        return 0
    if verdict is False:
        print("ktest_run: FAIL -- see the report above")
        return 1
    return fail(f"no verdict within {args.timeout:.0f}s", timeout_facts)


if __name__ == "__main__":
    sys.exit(main())
