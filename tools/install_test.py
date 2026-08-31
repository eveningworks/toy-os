#!/usr/bin/env python3
"""install_test -- does toy-os install itself onto another disk, and does
that disk then BOOT?

Two phases, and the second is the one that matters. The first drives
`/bin/install` and reads what it says; the second boots the disk it wrote
WITH NOTHING ELSE ATTACHED and asserts the machine that comes up is the
installed one. Without that second phase the first proves very little: a
guest with the ISO still in the drive boots the ISO's kernel and can
mount the target's root, which reads exactly like a successful install.

THE DISK ARRANGEMENT IS THE POINT, and it is not the obvious one. The
target has to be a disk the running system does NOT boot from, and disk
precedence here is virtio-blk, then AHCI, then ATA -- so a blank virtio
disk beside an IDE root OUTRANKS it, the root scan finds no filesystem on
it, and the machine comes up on ramfs with no /bin at all. So this is
inverted from the way it reads: the SYSTEM goes on virtio and the blank
TARGET on IDE. That costs nothing and is why this tool needs no special
build (`hires_test.py`'s KCMDLINE dance is what the alternative looks
like).

    python3 tools/install_test.py [--instance N] [--keep]

Run `make iso` first, as with every headless test here.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

VM = [sys.executable, os.path.join(HERE, "vm.py")]

# 512 MiB: big enough for the system (~50 MB) plus the 64 MiB ESP with
# room to spare, small enough that a sparse file costs nothing.
TARGET_MB = 512


class Checks:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=""):
        self.rows.append((name, bool(ok), detail))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail else ""))
        return ok

    def report(self, prog):
        bad = [n for n, ok, _ in self.rows if not ok]
        print(f"\n{prog}: {len(self.rows) - len(bad)}/{len(self.rows)} passed")
        return 1 if bad else 0


def vm(args, *cmds, timeout=600):
    """Run vm.py with this run's disks, returning its stdout."""
    base = VM + ["--instance", str(args.instance), "--disk", args.target,
                 "--virtio-disk", args.system]
    p = subprocess.run(base + list(cmds), cwd=ROOT, capture_output=True,
                       text=True, timeout=timeout)
    return p.stdout + p.stderr


def boot_target_alone(target, log, seconds=30):
    """Boot `target` with NO ISO and NO other drive, and return its serial
    output. A plain qemu line rather than vm.py's, because vm.py always
    attaches the ISO -- and 'no ISO' is the whole assertion."""
    pidfile = log + ".pid"
    for stale in (log, pidfile):
        if os.path.exists(stale):
            os.remove(stale)
    subprocess.run(["qemu-system-x86_64", "-m", "512",
                    "-drive", f"file={target},format=raw,if=ide,index=0",
                    "-boot", "order=c", "-display", "none",
                    "-serial", f"file:{log}", "-no-reboot",
                    "-pidfile", pidfile, "-daemonize"],
                   check=True, capture_output=True)
    try:
        # Poll for the line that ends the boot rather than sleeping the
        # whole budget: a fast host is done in a few seconds.
        deadline = time.time() + seconds
        while time.time() < deadline:
            time.sleep(1)
            if os.path.exists(log):
                with open(log, errors="replace") as f:
                    if "init: target" in f.read():
                        time.sleep(2)   # let the desktop's lines land
                        break
    finally:
        if os.path.exists(pidfile):
            with open(pidfile) as f:
                pid = int(f.read().strip())
            try:
                os.kill(pid, 15)
            except ProcessLookupError:
                pass
    with open(log, errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--keep", action="store_true",
                    help="leave the images behind for inspection")
    ap.add_argument("--scratch", default="/tmp/toyos-install-test")
    ap.add_argument("--positive-control", action="store_true",
                    help="zero the installed boot sector before phase 2, so "
                         "the checks that prove the disk boots must go RED. A "
                         "clean run proves nothing until this has been seen "
                         "to fail.")
    args = ap.parse_args()

    os.makedirs(args.scratch, exist_ok=True)
    args.system = os.path.join(args.scratch, "system.img")
    args.target = os.path.join(args.scratch, "target.img")

    # A COPY of disk.img, and a sparse one: it is ~50 MB of data in a 9 GB
    # sparse file, so a hole-filling copy costs 9 GB of whatever this
    # lands on (CLAUDE.md).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    os.path.join(ROOT, "disk.img"), args.system], check=True)
    if os.path.exists(args.target):
        os.remove(args.target)
    with open(args.target, "wb") as f:
        f.truncate(TARGET_MB * 1024 * 1024)

    c = Checks()
    print("install_test: phase 1 -- installing onto a blank disk")
    out = vm(args, "start", timeout=300)
    if "ready" not in out:
        print(out)
        print("install_test: the guest did not come up")
        return 1
    try:
        seen = vm(args, "exec", "lsblk")
        c.add("the blank target is a disk the system did not boot from",
              "ata0" in seen and "virtio0p3" in seen)

        out = vm(args, "--timeout", "400", "exec", "install --disk ata0 confirm",
                 timeout=900)
        c.add("the installer runs to completion", "install: done." in out,
              "" if "install: done." in out else out.strip().splitlines()[-1:][0] if out.strip() else "no output")
        c.add("it partitioned the target", "3 partition(s) named" in out)
        c.add("it formatted both target filesystems",
              "formatted as fat32" in out and "formatted as tfs3" in out)
        c.add("it mounted the target while its own root stayed mounted",
              "mounted at /mnt on ata0p3" in out and "mounted at /mnt/boot on ata0p2" in out)
        c.add("it copied the system and /boot",
              "copying the system" in out and "/boot/boot" in out)
        c.add("nothing was truncated or ran out of room",
              "no space left" not in out)
        c.add("it wrote a boot sector and a core image",
              "boot sector written, core image at LBA" in out)
    finally:
        vm(args, "stop", timeout=120)

    print("install_test: phase 2 -- booting that disk with nothing else attached")
    if args.positive_control:
        # Exactly what SYS_INSTALL_BOOT writes at LBA 0, undone. The
        # partitions and every file survive, so a phase 2 that still
        # passes is a phase 2 measuring something other than booting.
        print("install_test: POSITIVE CONTROL -- zeroing the installed boot sector")
        with open(args.target, "r+b") as f:
            f.write(b"\0" * 512)
    log = os.path.join(args.scratch, "target-boot.log")
    if not shutil.which("qemu-system-x86_64"):
        c.add("the installed disk boots on its own", False, "no qemu-system-x86_64")
    else:
        boot = boot_target_alone(args.target, log)
        # The kernel could only have come from the target's own boot
        # sector: nothing else is attached.
        c.add("the installed disk boots on its own", "kernel_main reached" in boot)
        c.add("it mounts ITS OWN root, not somebody else's",
              "tfs3 mounted at / on ata0p3" in boot)
        # THE DISCRIMINATOR between "the install worked" and "something
        # else booted and mounted the target": the source's root is
        # 18,739,167 sectors and this one is derived from TARGET_MB, so
        # a machine that came up on the wrong volume says so in a number.
        want = TARGET_MB * 1024 * 1024 // 512 - 135168 - 33   # GPT keeps the last 33
        c.add("its root partition is the target's size, not the source's",
              f"{want} sectors" in boot, f"{want} sectors")
        c.add("/boot is the ESP this install wrote",
              "fat32 mounted at /boot on ata0p2" in boot)
        c.add("init reaches its target and the desktop starts",
              "init: target" in boot and "toywm" in boot)
        if not all(ok for _, ok, _ in c.rows[-5:]):
            print("---- the installed disk's serial log ----")
            print(boot[-3000:])

    if not args.keep:
        for f in (args.system, args.target):
            if os.path.exists(f):
                os.remove(f)
    else:
        print(f"install_test: images kept in {args.scratch}")
    return c.report("install_test")


if __name__ == "__main__":
    sys.exit(main())
