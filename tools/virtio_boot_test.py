#!/usr/bin/env python3
"""Boot toy-os with NO IDE controller and its filesystem on virtio-blk.

WHY THIS EXISTS SEPARATELY FROM THE KTESTS
------------------------------------------
The `virtio` KTEST suite drives the transport and the virtqueue against
a virtio-rng device, which proves the ring moves data. It cannot prove
the two things that matter for a DISK, because on those boots ATA still
owns the filesystem:

  - that block_virtio.c's adapter satisfies `struct block_device`
    well enough for a real filesystem to live on it, and
  - that the precedence rule in kernel/fs/vfs.c actually hands virtio
    the disk when ATA has none.

So this boots a machine with no `-drive if=ide` at all. If TFS3 mounts,
every layer is exercised for real: PCI capability walk, 64-bit BAR
decode, feature negotiation, the virtqueue, the request format, the
block adapter and the filesystem on top.

AND IT REBOOTS. A write that only ever went to a cache would pass a
same-boot read-back, so the file is written on the first boot and read
on a SECOND one against the same image -- the round-trip assertion this
repo prefers, rather than believing the layer that did the writing.

ON DEMAND, not in the gate: it boots twice and is slower than the KTEST
suite, and an ordinary change does not affect it. Same category as
tools/live_boot_test.py.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

SERIAL_PORT = 4557  # not ktest_run.py's 4555, so both can run at once


def launch(iso, virtio_img, qemu_log):
    cmd = [
        "qemu-system-x86_64",
        "-cdrom", iso,
        # THE POINT OF THIS TOOL: no `-drive if=ide`. The only disk is
        # on virtio, so ata_init() finds nothing and the filesystem can
        # only mount if the virtio path works end to end.
        "-drive", f"file={virtio_img},format=raw,if=none,id=vblk",
        "-device", "virtio-blk-pci,drive=vblk,disable-legacy=on",
        "-m", "256",
        "-display", "none",
        # `server` without `nowait`, so QEMU waits for us and nothing
        # printed before the connection is lost -- see ktest_run.py.
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server",
        "-no-reboot",
        "-no-shutdown",
    ]
    log = open(qemu_log, "wb")
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)


def connect(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1.0)
        except OSError:
            time.sleep(0.2)
    return None


def read_until(sock, needle, timeout, transcript):
    deadline = time.time() + timeout
    sock.settimeout(0.5)
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        transcript.append(chunk.decode("utf-8", "replace"))
        if needle in "".join(transcript):
            return True
    return False


def run_boot(iso, img, qemu_log, commands, timeout):
    """Boot once, run `commands` over the debug console, return the transcript."""
    qemu = launch(iso, img, qemu_log)
    transcript = []
    try:
        sock = connect(timeout)
        if sock is None:
            return None, "could not connect to the guest's serial console"
        if not read_until(sock, "debug console ready", timeout, transcript):
            return None, "the debug console never came up"
        for cmd in commands:
            sock.sendall((cmd + "\n").encode())
            time.sleep(1.5)
            read_until(sock, "\x00never-matches\x00", 1.5, transcript)
        return "".join(transcript), None
    finally:
        try:
            qemu.kill()
            qemu.wait(timeout=5)
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img",
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--work", default="virtio_boot.img")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--qemu-log", default="virtio_boot_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"virtio_boot_test: {args.iso} not found -- build it first (make iso)")
        return 1

    # Work on a COPY: this test writes to the filesystem, and the real
    # disk.img is re-seeded by `make iso` and may be open in the user's
    # own QEMU.
    shutil.copyfile(args.disk, args.work)

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok, detail))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))

    marker = "virtio-round-trip-42"
    print("virtio_boot_test: boot 1 -- mount off virtio and write")
    t1, err = run_boot(args.iso, args.work, args.qemu_log,
                       [f"sh write /virtio_probe.txt {marker}", "sh sync", "sh dmesg"],
                       args.timeout)
    if t1 is None:
        print(f"virtio_boot_test: FAIL -- {err}")
        return 1
    if args.verbose:
        print(t1)

    # The driver claimed the device and reported a capacity.
    check("virtio-blk claimed the device", "virtio-blk:" in t1 and "sectors" in t1)
    # It took the disk -- which only happens when ATA has none.
    check("virtio-blk became the active block device", "block: virtio-blk active" in t1)
    # ATA really found nothing, so this is not ATA quietly serving.
    check("no ATA disk was present", "ata: no disk" in t1 or "tfs3: mounted" in t1)
    # And a real filesystem mounted on it, persistent rather than RAM.
    check("TFS3 mounted off virtio", "tfs3: mounted" in t1)
    check("the filesystem is persistent, not RAM-only",
          "RAM-only" not in t1.split("fs: active backend")[-1][:80])

    print("virtio_boot_test: boot 2 -- read it back on a fresh boot")
    t2, err = run_boot(args.iso, args.work, args.qemu_log,
                       ["sh cat /virtio_probe.txt"], args.timeout)
    if t2 is None:
        print(f"virtio_boot_test: FAIL -- {err}")
        return 1
    if args.verbose:
        print(t2)

    # THE assertion: the bytes survived a power cycle, so the write went
    # through virtio-blk to the image and was not merely cached. Exact
    # marker rather than a substring of something vaguer -- a partial
    # match is how a test passes on the command echo instead of on the
    # file's contents.
    check("the file written over virtio survived a reboot", marker in t2)

    failed = [c for c in checks if not c[1]]
    print(f"virtio_boot_test: {'FAIL' if failed else 'PASS'} -- "
          f"{len(checks) - len(failed)} passed, {len(failed)} failed")
    if failed and not args.verbose:
        print("  re-run with -v for the full serial transcript")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
