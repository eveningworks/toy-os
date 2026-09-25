#!/usr/bin/env python3
"""Boot toy-os with its filesystem on a SATA drive behind an AHCI HBA.

WHY THIS EXISTS SEPARATELY FROM THE KTESTS
------------------------------------------
The `ahci` KTEST suite is the only thing that drives the port, the
command list and the PRDT -- and on every default boot it SKIPS, because
the machine has a legacy IDE disk and no host bus adapter at all. A
suite that is green because nothing ran is the failure mode this repo
keeps finding, so the load-bearing assertion here is not "the tests
passed" but **0 skipped**.

Around that it proves the two things a KTEST cannot:

  - that block_ahci.c satisfies `struct block_device` well enough for a
    real filesystem to live on it, and
  - that the precedence rule in kernel/fs/mount.c hands AHCI the disk
    when there is no virtio device, and steps down to legacy IDE when
    the boot line says `noahci`.

AND IT REBOOTS. A write that only reached a buffer would pass a
same-boot read-back, so files are written on boot 1 and read on boot 2
against the same image.

TWO INDEPENDENT ORACLES. mtools: the guest reads /boot/grub/grub.cfg
through the AHCI driver and FAT32, and the host reads the same file out
of the same image with `mtype`, which shares no line of code with either.
"the file reads back" is what a privately-wrong driver passes; "the host
agrees byte for byte" is not. That phase SKIPS without mtools.

ON DEMAND, not in the gate: it boots three times, and an ordinary change
does not affect it. Same category as tools/virtio_boot_test.py.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_grub  # noqa: E402
from qmp_test import guarded_boot_args  # noqa: E402

SERIAL_PORT = 4558  # not virtio_boot_test.py's 4557, so both can run at once


def launch(img, qemu_log):
    cmd = [
        "qemu-system-x86_64",
        # The disk carries GRUB and the kernel, so it is the boot medium
        # and the ISO is not attached at all. Never NO order -- see
        # tools/vm.py on what SeaBIOS does with a bare partition table.
        *guarded_boot_args(img, ports=(SERIAL_PORT,)),   # a copy of disk.img: order=c
        # THE POINT OF THIS TOOL: the image hangs off an ICH9 host bus
        # adapter, not the legacy IDE controller. The IDE controller is
        # still on the bus (this is an i440fx machine) with nothing
        # attached, which is what makes `noahci` below a real fallback
        # rather than a no-disk boot.
        "-device", "ich9-ahci,id=ahci",
        "-drive", f"file={img},format=raw,if=none,id=sata0,discard=unmap",
        "-device", "ide-hd,drive=sata0,bus=ahci.0",
        "-m", "512",
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


def run_boot(img, qemu_log, commands, timeout, settle=2.0):
    """Boot once, run `commands` on the debug console, return the transcript."""
    qemu = launch(img, qemu_log)
    transcript = []
    try:
        sock = connect(timeout)
        if sock is None:
            return None, "could not connect to the guest's serial console"
        if not read_until(sock, "debug console ready", timeout, transcript):
            return None, "the debug console never came up"
        for cmd, wait in commands:
            sock.sendall((cmd + "\n").encode())
            # Wait on the ARTIFACT where there is one -- a fixed sleep
            # long enough for `ktest` is far too long for everything else.
            if wait:
                read_until(sock, wait, timeout, transcript)
            else:
                read_until(sock, "\x00never-matches\x00", settle, transcript)
        return "".join(transcript), None
    finally:
        try:
            qemu.kill()
            qemu.wait(timeout=5)
        except Exception:
            pass


# ---- the `noahci` boot word -----------------------------------------
#
# GRUB's command line is baked into /boot/grub/grub.cfg on the image at
# `make iso` time, so testing a boot word means rewriting that file --
# inside the image COPY, with the same mtools install_grub.py uses. The
# alternative is `make iso KCMDLINE="noahci"`, which rebuilds the media
# every other tool shares.

def esp_of(img):
    _bios, esp = install_grub.parts_of(img)
    return esp


def host_grub_cfg(img):
    esp = esp_of(img)
    if not esp:
        return None
    return install_grub.mtype(img, esp, "::/boot/grub/grub.cfg")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default="disk.img",
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--work", default="ahci_test.img")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--qemu-log", default="ahci_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.disk):
        print(f"ahci_test: {args.disk} not found -- build it first (make iso)")
        return 1

    # A COPY, and a SPARSE one: disk.img is ~4 MB of data in a 9 GB
    # sparse file, so a hole-filling copy costs 9 GB.
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, args.work],
                   check=True)

    checks, skipped = [], []

    def check(name, ok, detail=""):
        checks.append((name, ok, detail))
        # Detail on FAILURE only: a reason printed beside a green check
        # reads as a finding.
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))

    def skip(name, why):
        skipped.append((name, why))
        print(f"  skip  {name}  -- {why}")

    marker_dir = "/ahci_probe"

    # ---- boot 1: claim the drive, run the KTESTs, write ---------------
    print("ahci_test: boot 1 -- claim the drive, run the suite, write")
    t1, err = run_boot(args.work, args.qemu_log, [
        ("sh ahci", None),
        ("sh df", None),
        # The ESP is mounted at /boot and its OWN layout puts grub.cfg
        # under boot/ -- hence the doubling, the same way a Linux ESP at
        # /boot/efi shows /boot/efi/EFI/... (docs/conventions/storage.md).
        ("sh cat /boot/boot/grub/grub.cfg", None),
        ("sh ktest ahci", "ktest: "),
        (f"sh mkdir {marker_dir}", None),
        (f"sh mkfiles {marker_dir} 4 20000", None),
        ("sh sync", None),
    ], args.timeout)
    if t1 is None:
        print(f"ahci_test: FAIL -- {err}")
        return 1
    if args.verbose:
        print(t1)

    check("the HBA was found and its ports enumerated",
          re.search(r"ahci: HBA \d+\.\d+, \d+ ports? implemented", t1) is not None)
    check("a SATA drive was identified on a port",
          re.search(r'ahci: port \d+: "[^"]+", \d+ sectors', t1) is not None)
    check("completions are interrupt-driven, not polled", "IRQ-driven" in t1)
    # The registry names the DEVICE (`block: ahci0 active`), not the driver.
    check("AHCI became the active block device", re.search(r"block: ahci\d* active", t1) is not None)
    # "It booted" proves nothing -- a kernel that ignored the controller
    # falls through to ramfs and still reaches a prompt. A mounted,
    # PERSISTENT tfs3 is what only a working driver produces.
    check("TFS3 mounted off the AHCI drive", "fs: tfs3 mounted at / on ahci" in t1)
    # THE ROOT, not the transcript. This used to also require the word
    # "ramfs" to be absent anywhere in the boot log, which stopped being
    # satisfiable when /tmp became a ramfs mount -- the check then failed
    # on a perfectly healthy AHCI boot and had been red ever since.
    check("the root is persistent, not ramfs",
          re.search(r"ramfs\s+/\s", t1) is None
          and re.search(r"tfs3\s+/\s+\S+\s+\S+\s+\S+\s+\S+\s+yes", t1) is not None)
    check("/bin/ahci reports the drive and marks one port in use",
          "<- in use" in t1 and t1.count("<- in use") == 1)

    # THE LOAD-BEARING CHECK. Every ahci KTEST skips on a machine without
    # a controller, so a suite reporting "passed" says nothing on its
    # own -- this asserts they actually RAN.
    m = re.search(r"ktest: (PASSED|FAILED) -- (\d+) passed, (\d+) failed, (\d+) skipped", t1)
    if not m:
        check("the ahci KTESTs ran", False, "no ktest summary in the transcript")
    else:
        passed, failed, skipped_n = int(m.group(2)), int(m.group(3)), int(m.group(4))
        check("the ahci KTESTs passed", m.group(1) == "PASSED" and failed == 0,
              f"{passed} passed, {failed} failed")
        check("no ahci KTEST skipped (the driver really ran)", skipped_n == 0,
              f"{skipped_n} skipped -- the suite is green because nothing ran")
        check("every ahci KTEST is accounted for", passed >= 5, f"only {passed} ran")

    check("files were written through the AHCI path", "created 4 file(s)" in t1)

    # ---- the independent oracle --------------------------------------
    host_cfg = host_grub_cfg(args.work)
    if host_cfg is None:
        skip("the guest's read of /boot agrees with mtools", "mtools not available")
    else:
        # The guest printed the same file, read through AHCI and FAT32.
        # Compare the lines that carry content -- the console wraps and
        # interleaves klog output, so a whole-text compare would measure
        # the terminal rather than the driver.
        wanted = [ln.strip() for ln in host_cfg.splitlines()
                  if ln.strip() and not ln.strip().startswith("#")]
        missing = [ln for ln in wanted if ln not in t1]
        check("the guest's read of /boot agrees with mtools byte for byte",
              not missing, f"{len(missing)} of {len(wanted)} lines differ: {missing[:2]}")

    # ---- boot 2: the round trip --------------------------------------
    print("ahci_test: boot 2 -- the same image, read back")
    t2, err = run_boot(args.work, args.qemu_log, [
        (f"sh ls {marker_dir}", None),
        (f"sh stat {marker_dir}/f000003", None),
    ], args.timeout)
    if t2 is None:
        print(f"ahci_test: FAIL -- {err}")
        return 1
    if args.verbose:
        print(t2)

    check("the files survived a reboot", "f000003" in t2)
    check("and their size survived with them", "20000 bytes" in t2)

    # ---- TRIM, measured from the HOST --------------------------------
    #
    # THE ONLY ORACLE THAT CANNOT BE FOOLED BY THE GUEST. A drive that
    # acknowledges DSM and discards nothing looks identical from inside
    # toy-os -- that exact failure shipped once in ata.c, where the
    # range list went out over PIO and never arrived. What it cannot fake
    # is the ALLOCATED size of the sparse image on this filesystem.
    def allocated(path):
        return os.stat(path).st_blocks * 512

    print("ahci_test: TRIM -- write 40 MiB, delete it, watch the image")
    base = allocated(args.work)
    t4, err = run_boot(args.work, args.qemu_log, [
        ("sh mkdir /trim_probe", None),
        ("sh mkfiles /trim_probe 40 1000000", "created"),
        ("sh sync", None),
    ], args.timeout)
    if t4 is None:
        print(f"ahci_test: FAIL -- {err}")
        return 1
    grown = allocated(args.work)
    check("writing 40 MiB grows the host image", grown - base > 30 * 1024 * 1024,
          f"grew by {(grown - base) // (1024 * 1024)} MiB")

    t5, err = run_boot(args.work, args.qemu_log, [
        ("sh rm -r /trim_probe", None),
        ("sh sync", None),
    ], args.timeout)
    if t5 is None:
        print(f"ahci_test: FAIL -- {err}")
        return 1
    reclaimed = allocated(args.work)
    # Back to within a megabyte of where it started. Not exactly equal:
    # the deletion itself writes metadata.
    check("deleting it hands the blocks back (TRIM reached the drive)",
          reclaimed - base < 1024 * 1024,
          f"still {(reclaimed - base) // 1024} KiB above baseline")

    # ---- boot 3: `noahci` steps down ---------------------------------
    print("ahci_test: boot 3 -- `noahci` hands the disk back to legacy IDE")
    ok, why = install_grub.add_boot_word(args.work, "noahci")
    if not ok:
        skip("`noahci` steps the precedence down one rung", why)
    else:
        # NO `sh ahci` HERE: the root is ramfs on this boot, so there is
        # no /bin to run it out of. The boot log is what says the driver
        # still ran, and it is the only thing that can.
        t3, err = run_boot(args.work, args.qemu_log, [("sh df", None)], args.timeout)
        if t3 is None:
            print(f"ahci_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t3)
        # The DRIVER still runs and still finds the drive -- `noahci` is
        # a block-layer precedence word, not a driver kill switch. What
        # it must change is who carries the filesystem, and with no IDE
        # drive on this machine that is ramfs.
        check("`noahci` leaves the driver running",
              "ahci: HBA" in t3 and re.search(r'ahci: port \d+: "', t3) is not None)
        check("`noahci` keeps AHCI out of the block layer",
              "block: ahci active" not in t3)
        check("and the root falls through to ramfs", "ramfs" in t3)

    failures = [n for n, ok, _ in checks if not ok]
    print()
    print(f"ahci_test: {len(checks) - len(failures)}/{len(checks)} checks passed"
          + (f", {len(skipped)} skipped" if skipped else ""))
    if failures:
        print("ahci_test: FAILED -- " + "; ".join(failures))
        return 1
    print("ahci_test: PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
