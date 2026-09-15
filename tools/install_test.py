#!/usr/bin/env python3
"""install_test -- does toy-os install itself onto another disk, and does
that disk then BOOT?

Two phases, and the second is the one that matters. The first drives
`/bin/install` and reads what it says; the second boots the disk it wrote
WITH NOTHING ELSE ATTACHED and asserts the machine that comes up is the
installed one. Without that second phase the first proves very little: a
guest with the ISO still in the drive boots the ISO's kernel and can
mount the target's root, which reads exactly like a successful install.

THREE RUNS, because they are different code paths:

  disk   the system on a disk, installing onto another disk.
  live   toy-os-live.iso, whose root is a RAM image with no /boot at all
         -- which is why the install payload lives in /install rather
         than being read out of /boot. Needs `make live-iso`.
  grown  a live install onto a disk that ALREADY has a table describing a
         SMALLER layout -- a small image dd'd onto a big disk. The
         partition table is rewritten correctly and the windows the
         previous table named are stale, and until the rescan learned to
         RELEASE them, `mkfs` formatted the old one: a 119 GB partition
         holding a 441 MB filesystem, on a machine that booted perfectly
         and used 0.4% of its disk.

THE FILESYSTEM MUST FILL ITS PARTITION, and that is checked on every run
rather than only the grown one -- it is the assertion the bug above
walked straight through, because everything else about that install was
right.

THE DISK ARRANGEMENT IS THE POINT for the first, and it is not the
obvious one. The target has to be a disk the running system does NOT boot
from, and disk precedence here is virtio-blk, then AHCI, then ATA -- so a
blank virtio disk beside an IDE root OUTRANKS it, the root scan finds no
filesystem on it, and the machine comes up on ramfs with no /bin at all.
So it is inverted from the way it reads: the SYSTEM goes on virtio and
the blank TARGET on IDE. That costs nothing and is why this tool needs no
special build (`hires_test.py`'s KCMDLINE dance is the alternative). The
live run needs no such trick -- its root is RAM, which outranks nothing.

    python3 tools/install_test.py [--media disk|live|both] [--keep]

Run `make iso` first (and `make live-iso` for the live half), as with
every headless test here.
"""

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import port_guard  # noqa: E402

VM = [sys.executable, os.path.join(HERE, "vm.py")]

# 512 MiB: big enough for the system (~50 MB) plus the 64 MiB ESP with
# room to spare, small enough that a sparse file costs nothing.
TARGET_MB = 512

# What a "grown" run expands the target to before reinstalling -- the
# shape of a small image dd'd onto a big disk, which is how the
# maintainer's laptop got there.
GROWN_MB = 4096

LIVE_ISO = "toy-os-live.iso"

# TFS3 puts T3_BPG (32768) blocks of T3_BLOCK (4096) in a group, so a
# group is 128 MiB. The mount line names the group count, which is the
# only report of the FILESYSTEM's size in a boot log -- the other size
# there is the PARTITION's, and the whole point of this check is that the
# two can disagree.
TFS3_GROUP_BYTES = 32768 * 4096


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


def vm(args, media, *cmds, timeout=600):
    """Run vm.py with this run's disks, returning its stdout."""
    if media == "mbr":
        media = "live"     # the same medium; only the install FLAG differs
    base = VM + ["--instance", str(args.instance), "--disk", args.target]
    if media in ("live", "grown"):
        # The ISO is the system; the only drive is the blank target.
        base += ["--iso", LIVE_ISO, "--boot", "cd"]
    else:
        base += ["--virtio-disk", args.system]
    env = dict(os.environ)
    if media in ("live", "grown"):
        # iso_guard only knows how to judge toy-os.iso; the live one is
        # built by its own target and legitimately lags.
        env["TOYOS_ALLOW_STALE_ISO"] = "1"
    p = subprocess.run(base + list(cmds), cwd=ROOT, capture_output=True,
                       text=True, timeout=timeout, env=env)
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


def run_media(args, c, media):
    """One install, then a boot of what it wrote. Prefixes every check
    with the medium, so a mixed run says which half failed."""
    def add(name, ok, detail=""):
        return c.add(f"[{media}] {name}", ok, detail)

    # MBR has no BIOS-boot partition, so every partition shifts down one
    # and the GPT's last-33 reservation does not apply.
    mbr = (media == "mbr")
    boot_n, root_n = (1, 2) if mbr else (2, 3)

    if os.path.exists(args.target):
        os.remove(args.target)
    with open(args.target, "wb") as f:
        f.truncate(TARGET_MB * 1024 * 1024)

    if media == "grown":
        # Install once at the small size to get a REAL table, then grow
        # the file -- which is what dd'ing a small image onto a big disk
        # leaves behind: a disk reporting its full size whose GPT still
        # describes the smaller layout.
        print(f"install_test: {media} -- seeding a smaller install first")
        if "vm: ready" not in vm(args, "live", "start", timeout=400):
            return add("the seeding guest came up", False)
        seed_out = vm(args, "live", "--timeout", "400", "exec",
                      "install --disk ata0 confirm", timeout=900)
        vm(args, "live", "stop", timeout=120)
        if not add("a smaller install lands first", "install: done." in seed_out):
            return False
        with open(args.target, "r+b") as f:
            f.truncate(GROWN_MB * 1024 * 1024)

    print(f"install_test: {media} -- installing onto a blank disk")
    out = vm(args, media, "start", timeout=400)
    # "vm: ready", NOT "ready": vm.py's refusal is "vm: already running",
    # and "already" contains "ready". That made a guest this run never
    # started look like one that came up, so every command afterwards
    # went to whatever guest was left over -- ten checks failing for a
    # reason none of them named.
    if "vm: ready" not in out:
        print(out)
        return add("the guest came up", False)
    try:
        # WAIT ON THE CONSOLE ANSWERING, not on `start` returning. On the
        # live media `vm.py` reports ready before the debug console is
        # taking commands, so the first exec comes back EMPTY and every
        # check after it fails for a reason none of them names. `lsblk`
        # always prints its NAME header, so that is the observable.
        seen = ""
        for _ in range(10):
            seen = vm(args, media, "exec", "lsblk")
            if "NAME" in seen:
                break
            time.sleep(3)
        if not add("the console answers", "NAME" in seen, seen[:120]):
            return False
        if media in ("live", "grown", "mbr"):
            # Its root is a RAM image, so the IDE disk is the only thing
            # on the machine that could be a target.
            add("the live root is RAM and the target is a plain disk",
                "ram0" in seen and "ata0" in seen)
            add("the payload rode along in /install",
                "kernel.bin" in vm(args, media, "exec", "ls /install"))
        if media == "grown":
            # The fixture: the disk is big and its table still is not.
            add("the target's old table describes a SMALLER layout",
                "ata0p3" in seen and "445.9M" in seen,
                "stale p3 named before the install")
        elif media == "disk":
            add("the blank target is a disk the system did not boot from",
                "ata0" in seen and "virtio0p3" in seen)

        cmd = ("install --disk ata0 --mbr confirm" if mbr
               else "install --disk ata0 confirm")
        out = vm(args, media, "--timeout", "400", "exec", cmd, timeout=900)
        add("the installer runs to completion", "install: done." in out)
        add("it partitioned the target",
            ("2 partition(s) named" if mbr else "3 partition(s) named") in out)
        add("it formatted both target filesystems",
            "formatted as fat32" in out and "formatted as tfs3" in out)
        add("it mounted the target while its own root stayed mounted",
            f"mounted at /mnt on ata0p{root_n}" in out and
            f"mounted at /mnt/boot on ata0p{boot_n}" in out)
        add("it copied the system and wrote the target's /boot",
            "copying the system" in out and "/install/kernel.bin" in out)
        add("nothing was truncated or ran out of room", "no space left" not in out)
        add("it wrote a boot sector and a core image",
            "boot sector written, core image at LBA" in out)
    finally:
        vm(args, media, "stop", timeout=120)

    print(f"install_test: {media} -- booting that disk with nothing else attached")
    if args.positive_control:
        # Exactly what SYS_INSTALL_BOOT writes at LBA 0, undone. The
        # partitions and every file survive, so a phase 2 that still
        # passes is a phase 2 measuring something other than booting.
        print("install_test: POSITIVE CONTROL -- zeroing the installed boot sector")
        with open(args.target, "r+b") as f:
            f.write(b"\0" * 512)
    log = os.path.join(args.scratch, f"target-boot-{media}.log")
    if not shutil.which("qemu-system-x86_64"):
        return add("the installed disk boots on its own", False, "no qemu-system-x86_64")

    boot = boot_target_alone(args.target, log)
    # The kernel could only have come from the target's own boot sector:
    # nothing else is attached.
    add("the installed disk boots on its own", "kernel_main reached" in boot)
    add("it mounts ITS OWN root, not somebody else's",
        f"tfs3 mounted at / on ata0p{root_n}" in boot)
    # THE DISCRIMINATOR between "the install worked" and "something else
    # booted and mounted the target": the source's root is a different
    # size, so a machine that came up on the wrong volume says so in a
    # number. It also proves the 4.7 MB kernel round-tripped through the
    # host writer's double-indirect map -- GRUB executed it.
    size_mb = GROWN_MB if media == "grown" else TARGET_MB
    # GPT keeps the last 33 sectors for its backup header; MBR keeps
    # none, and its root starts 2048 sectors earlier for want of a
    # BIOS-boot partition.
    want = (size_mb * 1024 * 1024 // 512 - 133120 if mbr
            else size_mb * 1024 * 1024 // 512 - 135168 - 33)
    add("its root partition is the target's size, not the source's",
        f"{want} sectors" in boot, f"{want} sectors")
    add("/boot is the boot partition this install wrote",
        f"fat32 mounted at /boot on ata0p{boot_n}" in boot)

    # THE FILESYSTEM FILLS ITS PARTITION. Two different numbers in the
    # boot log: the partition's sectors, and TFS3's group count -- and a
    # stale partition window makes them disagree while everything else
    # still looks right.
    part = re.search(rf"mounting tfs3 from partition {root_n} \(LBA \d+, (\d+) sectors\)", boot)
    groups = re.search(r"tfs3: mounted \(v\d+, (\d+) groups", boot)
    if part and groups:
        want_groups = math.ceil(int(part.group(1)) * 512 / TFS3_GROUP_BYTES)
        got = int(groups.group(1))
        # Within one: the metadata a format reserves can cost the last
        # group, and that is not what this is looking for.
        add("the filesystem fills its partition",
            got >= want_groups - 1,
            f"{got} groups, partition wants ~{want_groups}")
    else:
        add("the filesystem fills its partition", False, "no mount line in the log")
    add("init reaches its target and the desktop starts",
        "init: target" in boot and "toywm" in boot)
    if not all(ok for _, ok, _ in c.rows[-5:]):
        print(f"---- the installed disk's serial log ({media}) ----")
        print(boot[-3000:])
    return True


def run_bootloader(args, c):
    """`install --bootloader` on the RUNNING disk -- does that disk still
    boot, and can it then boot a COMPRESSED kernel?

    THE CONTROL IS PART OF THE PHASE, because every check here would pass
    on a machine whose bootloader was never rewritten: the disk booted
    before too. So the last check installs the same gzipped kernel behind
    a `core.img` with `gzio` REMOVED and requires it NOT to boot -- which
    is the bare-metal failure this mode exists for, reproduced:

        error: ... grub_multiboot2_load: no multiboot header found
    """
    def add(name, ok, detail=""):
        return c.add(f"[bootloader] {name}", ok, detail)

    import install_grub

    media = os.path.join(ROOT, "build", "kernel.media")
    if not os.path.exists(media):
        add("build/kernel.media exists", False, "run `make all` first")
        return

    self_img = os.path.join(args.scratch, "self.img")
    ctrl_img = os.path.join(args.scratch, "nogzio.img")
    for img in (self_img, ctrl_img):
        subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                        os.path.join(ROOT, "disk.img"), img], check=True)

    base = VM + ["--instance", str(args.instance), "--disk", self_img]
    env = dict(os.environ)
    env["TOYOS_ALLOW_STALE_ISO"] = "1"

    def guest(*cmds):
        p = subprocess.run(base + list(cmds), cwd=ROOT, capture_output=True,
                           text=True, timeout=600, env=env)
        return p.stdout + p.stderr

    guest("start")
    try:
        plan = guest("exec", "install --bootloader")
        add("refuses without `confirm`, and says what it would write",
            "core image" in plan and "Re-run with `confirm`" in plan)
        add("reads the layout off the disk rather than taking a flag",
            "GPT layout" in plan)
        add("--mbr is refused in this mode",
            "do not apply" in guest("exec", "install --bootloader --mbr confirm"))

        done = guest("exec", "install --bootloader confirm", "sync")
        add("writes the bootloader", "boots the newly written GRUB" in done)
        add("the kernel logged the write", "install_boot:" in done and
            "boot sector written" in done)
        add("records what it installed in /etc/grub-core.modules",
            "gzio" in guest("exec", "cat /etc/grub-core.modules"))
    finally:
        guest("stop")

    log = os.path.join(args.scratch, "self-boot.log")
    add("the disk still boots on the bootloader it wrote itself",
        "init: target" in boot_target_alone(self_img, log))

    # The payoff: the same disk, now carrying a GZIPPED kernel.
    _bios, esp = install_grub.parts_of(self_img)
    install_grub.mcopy_into(self_img, esp, [media], "::/boot/kernel.bin")
    log = os.path.join(args.scratch, "self-gz.log")
    add("and then boots a COMPRESSED kernel",
        "init: target" in boot_target_alone(self_img, log))

    # THE CONTROL. Same gzipped kernel, a core.img built without gzio.
    saved = install_grub.CORE_MODULES
    try:
        install_grub.CORE_MODULES = tuple(m for m in saved if m != "gzio")
        install_grub.install(ctrl_img, media,
                             os.path.join(ROOT, "build", "grub-disk.cfg"),
                             verbose=False)
    finally:
        install_grub.CORE_MODULES = saved
    log = os.path.join(args.scratch, "nogzio.log")
    add("CONTROL: without gzio that kernel does NOT boot",
        "init: target" not in boot_target_alone(ctrl_img, log, seconds=20))

    if not args.keep:
        for f in (self_img, ctrl_img):
            if os.path.exists(f):
                os.remove(f)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--media",
                    choices=("disk", "live", "grown", "mbr", "bootloader", "all"),
                    default="all",
                    help="which run to make (default every one)")
    ap.add_argument("--keep", action="store_true",
                    help="leave the images behind for inspection")
    ap.add_argument("--scratch", default="/tmp/toyos-install-test")
    ap.add_argument("--positive-control", action="store_true",
                    help="zero the installed boot sector before each boot "
                         "phase, so the checks that prove the disk boots must "
                         "go RED. A clean run proves nothing until this has "
                         "been seen to fail.")
    args = ap.parse_args()

    # RESOLVE `auto` ONCE. Every vm.py call below is handed this value,
    # and vm.py resolves `auto` per invocation -- so passing the word
    # through gave `start` one slot and each later `exec` a DIFFERENT
    # free one, with no guest in it. The execs came back empty and every
    # check failed for a reason none of them named.
    if args.instance == "auto":
        n = port_guard.find_free_instance()
        if n is None:
            print("install_test: no free VM slot")
            return 2
        args.instance = n
        print(f"install_test: using VM slot {args.instance}")

    media = (("disk", "live", "grown", "mbr", "bootloader")
             if args.media == "all" else (args.media,))
    if ("live" in media or "grown" in media) and not os.path.exists(os.path.join(ROOT, LIVE_ISO)):
        print(f"install_test: no {LIVE_ISO} -- run `make live-iso` "
              "(or pass --media disk)")
        return 1

    os.makedirs(args.scratch, exist_ok=True)
    args.system = os.path.join(args.scratch, "system.img")
    args.target = os.path.join(args.scratch, "target.img")

    # A COPY of disk.img, and a sparse one: it is ~50 MB of data in a 9 GB
    # sparse file, so a hole-filling copy costs 9 GB of whatever this
    # lands on (CLAUDE.md).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    os.path.join(ROOT, "disk.img"), args.system], check=True)

    c = Checks()
    for m in media:
        if m == "bootloader":
            run_bootloader(args, c)   # rewrites a COPY of this disk's own GRUB
        else:
            run_media(args, c, m)

    if not args.keep:
        for f in (args.system, args.target):
            if os.path.exists(f):
                os.remove(f)
    else:
        print(f"install_test: images kept in {args.scratch}")
    return c.report("install_test")


if __name__ == "__main__":
    sys.exit(main())
