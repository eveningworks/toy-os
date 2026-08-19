#!/usr/bin/env python3
"""Runs the in-kernel test suite and turns it into a process exit code.

Boots toy-os.iso headlessly, drives the `ktest` command over the serial
debug console (COM1 -- see kernel/core/debug_console.c), reads the
report back off the same wire, and exits 0 if every test passed.

Why the serial console rather than the graphical shell: it needs no
display, no QMP, no keyboard emulation and no screenshot -- just a pipe
in and a pipe out. Same reasoning as tools/boot_smoke_test.py, which
this is modelled on, except that one only reads and this one also
writes.

    python3 tools/ktest_run.py                 # every test
    python3 tools/ktest_run.py --suite fs      # one suite
    python3 tools/ktest_run.py -v              # dump the full log

Exit code 0 = "ktest: PASSED" seen. 1 = a test failed, the report never
appeared within the timeout, or the kernel panicked on the way.

Note this boots against the REAL disk.img by default, and the tests
write to it (creating and deleting /.ktest_tmp). Pass --disk to point at
a scratch copy if that matters.
"""

import argparse
import os
import socket
import subprocess
import sys
import time

DEFAULT_TIMEOUT = 60.0
SERIAL_PORT = 4555  # COM1 exposed as a TCP socket; not the QMP port (4445)


def launch(iso, disk, qemu_log):
    cmd = [
        "qemu-system-x86_64",
        "-cdrom", iso,
        # discard=unmap -- see kernel/drivers/ata.c's ata_trim().
        "-drive", f"file={disk},format=raw,if=ide,discard=unmap",
        "-m", "256",
        "-display", "none",
        # COM1 as a listening socket instead of a file: this script has
        # to TYPE the `ktest` command, not just read output, and a
        # `-serial file:` sink is write-only from the guest's side.
        #
        # `server` WITHOUT `nowait` on purpose -- QEMU then blocks until
        # this script connects before starting the guest. With `nowait`
        # the guest boots immediately and everything it prints before the
        # connection lands is discarded, so the debug console's banner
        # (which this script waits for) had already gone by the time it
        # connected, and it hung until the timeout every single run.
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server",
        # A virtio device on the bus, purely so the PCI capability walk
        # and the 64-bit BAR decode (kernel/include/kernel/pci_internal.h)
        # are COVERED by `make test` rather than only when a virtio disk
        # is deliberately attached.
        #
        # Measured, which is why it is here: QEMU's default pc-i440fx
        # topology publishes NO PCI capabilities at all -- not on the
        # host bridge, the PIIX3 IDE/ISA, the PIIX4 ACPI bridge, stdvga
        # or the e1000 -- and no 64-bit BAR either. So the walk that
        # every virtio device depends on ran zero times and its KTEST
        # reported a skip. virtio-rng-pci publishes five vendor-specific
        # capabilities and puts its registers in a 64-bit BAR, which is
        # exactly the two uncovered branches.
        #
        # rng rather than blk deliberately: it is not the device under
        # test, so it also exercises virtio_blk_init() DECLINING a virtio
        # device of the wrong type. Nothing here asserts a PCI device
        # count, so adding one disturbs no existing test.
        "-device", "virtio-rng-pci",
        "-no-reboot",
        "-no-shutdown",
    ]
    log = open(qemu_log, "wb")
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--suite", default="", help="run only this suite (e.g. fs, mm, lib)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    ap.add_argument("--qemu-log", default="ktest_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the whole serial transcript")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"ktest_run: {args.iso} not found -- build it first (make iso)")
        return 1

    qemu = launch(args.iso, args.disk, args.qemu_log)
    transcript = ""
    verdict = None
    try:
        # QEMU is waiting for this connection before it boots anything
        # (see the launch flags), so connecting is step one and the
        # transcript starts at the very first byte the kernel prints.
        sock = None
        deadline = time.time() + args.timeout
        while time.time() < deadline and sock is None:
            try:
                sock = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1.0)
            except OSError:
                time.sleep(0.2)
        if sock is None:
            print("ktest_run: FAIL -- QEMU's serial socket never accepted a connection")
            return 1
        sock.settimeout(0.5)

        def pump():
            nonlocal transcript
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                return
            if chunk:
                transcript += chunk.decode("utf-8", errors="replace")

        # Wait for the debug console to announce itself.
        while time.time() < deadline and "debug console ready" not in transcript:
            pump()
        if "debug console ready" not in transcript:
            print("ktest_run: FAIL -- the serial debug console never came up")
            return 1

        cmd = f"ktest {args.suite}\n" if args.suite else "ktest\n"
        sock.sendall(cmd.encode())

        while time.time() < deadline:
            pump()
            if "ktest: PASSED" in transcript:
                verdict = True
                break
            if "ktest: FAILED" in transcript or "KERNEL PANIC" in transcript:
                verdict = False
                break

        # Drain whatever is still in flight before killing QEMU. The
        # verdict substring arrives mid-line -- the counts that follow it
        # ("14 passed, 0 failed, ...") are still on the wire -- so
        # breaking out and terminating immediately truncates the summary
        # this script then tries to print.
        drain_until = time.time() + 1.0
        while time.time() < drain_until:
            pump()
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()

    if args.verbose or verdict is not True:
        # On failure the interesting part is the per-test lines, which
        # are the whole point of having a report rather than a bare code.
        for line in transcript.splitlines():
            if line.startswith(("ktest:", "  [", "    ")) or "FAIL" in line or "PANIC" in line:
                print(line)

    if verdict is True:
        summary = next((l for l in transcript.splitlines() if l.startswith("ktest: PASSED")), "")
        print(f"ktest_run: PASS -- {summary.replace('ktest: PASSED -- ', '')}")
        return 0
    if verdict is False:
        print("ktest_run: FAIL -- see the report above")
        return 1
    print(f"ktest_run: FAIL -- no verdict within {args.timeout}s (timed out)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
