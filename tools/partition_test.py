#!/usr/bin/env python3
"""Boot toy-os with its filesystem INSIDE an MBR or GPT partition.

WHY THIS EXISTS SEPARATELY FROM THE KTESTS
------------------------------------------
The `partition` KTEST suite writes a table to a RAM disk and reads it
back, which proves the encoder and the parser agree. It cannot prove
the thing that actually matters, because on those boots the filesystem
is still flat at LBA 0:

  - that kernel/fs/vfs.c's boot-time scan finds a partition, and
  - that kernel/drivers/block/block_part.c's window is correct enough
    for a real filesystem to live in it and survive a reboot.

The stock `disk.img` is partitioned now, so the ordinary suite covers
the GPT path incidentally. What it does NOT cover is the MBR path, a
SECOND partition, or a partition whose size differs from the disk's --
and "it booted" cannot tell a correct window from a lucky one. So this
builds images whose only filesystem is inside partition 1, at a size
nothing else on the image shares, and boots them. If TFS3 mounts, every layer is exercised for real: the table
parser, the partition block device, the volume-relative backend and the
scan that ties them together.

THE DISCRIMINATING CHECK IS `df`, NOT "IT BOOTED". A kernel that
ignored partitions entirely would still boot -- on a flat read of LBA 0
it finds nothing, refuses to format and runs RAM-only, which looks like
a bad disk rather than a missing feature. And a kernel that found the
partition but got the WINDOW wrong would mount something and report the
DISK's size. So the assertion is that the mounted volume is the size of
partition 1 and not the size of the image, which only a correct window
can produce.

AND IT REBOOTS. A write that only ever reached a cache would pass a
same-boot read-back, so a directory is created on the first boot and
looked for on a SECOND one against the same image.

ON DEMAND, not in the gate: it builds two images and boots four times.
Same category as tools/virtio_boot_test.py and tools/live_boot_test.py.
"""
import argparse
import os
import re
import socket
import subprocess
import tempfile
import sys
import time

SERIAL_PORT = 4559  # not ktest_run.py's 4555 nor virtio_boot_test.py's 4557

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512


def launch(iso, img, qemu_log):
    cmd = [
        "qemu-system-x86_64",
        # `-boot order=d` IS LOAD-BEARING HERE and nowhere else in this
        # repo: the image carries a partition table, so its 0x55AA
        # signature makes SeaBIOS treat it as a bootable hard disk. It
        # then jumps into 446 bytes of filesystem data and hangs with NO
        # serial output at all -- indistinguishable from a kernel that
        # died before its first print.
        "-boot", "order=d",
        "-cdrom", iso,
        "-drive", f"file={img},format=raw,if=ide,index=0",
        "-m", "256",
        "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server",
        "-no-reboot", "-no-shutdown",
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


def build_image(path, size_bytes, kind, layout, seed_dir):
    """A fresh image with a table and TFS3 inside partition 1."""
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(size_bytes)
    r = subprocess.run([sys.executable, os.path.join(HERE, "seed_disk.py"),
                        path, seed_dir, "--partition", kind, "--layout", layout],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return None
    m = re.search(r"partition 1 is LBA (\d+), (\d+) sectors", r.stdout)
    return (int(m.group(1)), int(m.group(2))) if m else None


def df_size(transcript):
    """The size column of `df`'s one data row, e.g. '507.6M'.

    THE KERNEL LOG LINE `tfs3: mounted ...` STARTS WITH THE SAME WORD
    as df's row, and a naive startswith() returns "mounted" as the size
    -- which reads as a broken feature rather than a broken parser.
    Every klog line here is "subsystem: ", so the colon is what tells
    them apart; virtio_boot_test.py's answer_after() draws the same
    line for the same reason.
    """
    for ln in transcript.splitlines():
        ln = ln.strip()
        if re.match(r"^[a-z][a-z0-9_]*: ", ln):
            continue
        parts = ln.split()
        if len(parts) >= 2 and parts[0] == "tfs3":
            return parts[1]
    return None


def to_mib(size_str):
    """'507.6M' -> 507.6 MiB. None if it cannot be read."""
    if not size_str:
        return None
    unit, num = size_str[-1], size_str[:-1]
    try:
        v = float(num)
    except ValueError:
        return None
    return {"K": v / 1024, "M": v, "G": v * 1024}.get(unit)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--seed", default="seed")
    # A TEMP DIRECTORY, not the current one. These are 2 GiB images and
    # the sweep runs tools from the repo root, so defaulting to CWD
    # dropped four gigabytes into the working tree -- which a `git add
    # -A` then swept into a commit that GitHub rejected. Nothing that
    # writes an image this size may default to somewhere tracked.
    ap.add_argument("--work",
                    default=os.path.join(tempfile.gettempdir(), "partition_boot.img"))
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--qemu-log",
                    default=os.path.join(tempfile.gettempdir(), "partition_boot_qemu.log"))
    ap.add_argument("--kind", choices=("gpt", "mbr", "both"), default="both")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"partition_test: {args.iso} not found -- build it first (make iso)")
        return 1

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail and not ok else ""))

    kinds = ("gpt", "mbr") if args.kind == "both" else (args.kind,)

    for kind in kinds:
        # 2 GiB image, a 256 MiB partition 1 and the rest in partition 2.
        # The two sizes must be FAR APART and neither may be the image's,
        # or "df reports the partition" cannot be told from "df reports
        # the disk".
        img = f"{os.path.splitext(args.work)[0]}_{kind}.img"
        print(f"partition_test: building a {kind.upper()} image ({img})")
        plan = build_image(img, 2 * 1024 ** 3, kind, "256M,rest", args.seed)
        if not plan:
            print("partition_test: FAIL -- could not build the image")
            return 1
        start, sectors = plan
        want_mib = sectors * SECTOR / (1 << 20)

        print(f"partition_test: {kind.upper()} boot 1 -- mount from partition 1 and write")
        t1, err = run_boot(args.iso, img, args.qemu_log,
                           ["sh mkdir /etc/parttest", "sh sync", "sh df", "sh parttable"],
                           args.timeout)
        if t1 is None:
            print(f"partition_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t1)

        check(f"[{kind}] the scan found the table",
              f"{kind.upper()} partition table" in t1)
        check(f"[{kind}] a partition became the active block device",
              re.search(r"block: \w+1 active \(\d+ sectors at LBA %d" % start, t1) is not None)
        check(f"[{kind}] TFS3 mounted FROM partition 1",
              "mounting tfs3 from partition 1" in t1)

        # THE DISCRIMINATING CHECK. A flat mount would report the whole
        # 2 GiB image; a wrong window would report something else again.
        got = df_size(t1)
        got_mib = to_mib(got)
        check(f"[{kind}] df reports the PARTITION's size, not the disk's",
              got_mib is not None and abs(got_mib - want_mib) < want_mib * 0.05,
              f"df said {got!r} (~{got_mib} MiB), partition 1 is {want_mib:.1f} MiB")

        check(f"[{kind}] the filesystem is persistent, not RAM-only",
              "RAM-only" not in t1)

        # `parttable` reads the WHOLE DISK while the mounted volume is a
        # partition. If it read through the partition window instead it
        # would find no table at all -- which is exactly what the old
        # ata_read_sector() path would now do.
        check(f"[{kind}] parttable still sees the table from inside a partition",
              t1.count(f"{kind.upper()} partition table") >= 2)

        print(f"partition_test: {kind.upper()} boot 2 -- does the write survive?")
        t2, err = run_boot(args.iso, img, args.qemu_log, ["sh ls /etc"], args.timeout)
        if t2 is None:
            print(f"partition_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t2)
        check(f"[{kind}] the directory written on boot 1 is still there",
              "parttest" in t2)
        check(f"[{kind}] and it is still mounted from partition 1",
              "mounting tfs3 from partition 1" in t2)

    # ---- /bin/mkpart, in the guest -----------------------------------
    #
    # Run on the GPT image, which is already partitioned -- so this also
    # covers repartitioning a disk that has a table, not only a blank
    # one. It is the LAST thing done to that image on purpose: it
    # destroys the filesystem the checks above depend on.
    img = f"{os.path.splitext(args.work)[0]}_{kinds[0]}.img"
    print("partition_test: /bin/mkpart in the guest")
    t3, err = run_boot(args.iso, img, args.qemu_log,
                       ["sh mkpart --mbr 100M rest",         # no `confirm` -- must refuse
                        "sh mkpart --mbr 100M rest confirm",
                        "sh parttable"],
                       args.timeout)
    if t3 is None:
        print(f"partition_test: FAIL -- {err}")
        return 1
    if args.verbose:
        print(t3)

    check("mkpart without `confirm` refuses", "re-run with `confirm`" in t3)
    check("mkpart with `confirm` writes an MBR", "MBR partition table written" in t3)
    # Read back through the parser, in the same boot: the table changed
    # under a running system, and the ONE thing that must not change is
    # the mounted volume (SYS_MKPART does not remount).
    check("...and the new MBR reads back", "MBR partition table, 2 entries" in t3)
    check("...while the running system stays on the old volume",
          "mounting tfs3 from partition 1" in t3)

    failed = [n for n, ok in checks if not ok]
    print()
    if failed:
        print(f"partition_test: FAIL -- {len(failed)} of {len(checks)} checks failed")
        for n in failed:
            print(f"  - {n}")
        return 1
    print(f"partition_test: PASS -- {len(checks)} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
