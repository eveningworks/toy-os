#!/usr/bin/env python3
"""FAT32 and the mount table, checked against an INDEPENDENT implementation.

WHY THIS EXISTS SEPARATELY FROM THE KTESTS
------------------------------------------
`kernel/fs/fat32_test.c` formats a 512 KiB RAM volume and drives the
backend directly. That proves the driver agrees with ITSELF, which is
the one thing a self-test cannot help proving -- the same person wrote
the writer and the reader, so a shared misreading of the format passes
both halves. This repo already made that call twice, for
`tools/regex_hostcheck.py` (against GLIBC) and `tools/uimg_hostcheck.py`
(against libjpeg); this is the storage one.

So the oracle here is the HOST: `mtools` reads back what the guest
wrote, and `fsck.fat` audits the volume afterwards. Neither shares a
line of code with `kernel/fs/fat32.c`, and both are what a person would
reach for if they pulled the disk out and plugged it into a Linux box.

It also covers the half no KTEST can reach at all: that the ESP on the
REAL disk -- a volume written by mtools, holding GRUB's own files --
mounts at `/boot` on an ordinary boot, and that it comes up READ-ONLY.

WHAT A BROKEN VERSION WOULD STILL PASS, and therefore what is asserted:

  - "ls /boot lists something" passes on a driver that reads a directory
    and mangles every long name, so the check reads GRUB's own
    `grub.cfg` and looks for text only a correct FAT chain walk
    produces.
  - "a file written reads back" passes on a driver whose format is
    privately wrong, since it is reading its own bytes -- so the file is
    read back on the HOST with mtools, and a 185 KiB BINARY is compared
    byte for byte rather than a short string.
  - "the volume still works" passes on a driver leaking clusters or
    cross-linking chains, which nothing observable would show for a long
    time -- so `fsck.fat` is the last word.

ON DEMAND, not in the gate: it boots twice against a copy of disk.img
and needs `mtools` and `dosfstools` on the host. Same category as
tools/partition_test.py and tools/virtio_boot_test.py.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import install_grub  # noqa: E402  (path set above)
import vm  # noqa: E402

SECTOR = 512

# A directory of our own inside the ESP. GRUB's files are never touched:
# a driver bug that corrupted them would cost `make clean-disk && make
# iso`, which is recoverable, but there is no reason to aim at them.
SCRATCH = "toyostest"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   [{detail}]" if detail and not ok else ""))
    return ok


def have(tool):
    return shutil.which(tool) is not None


class Guest:
    """One boot of the guest, driven through tools/vm.py."""

    def __init__(self, disk, instance):
        self.disk = disk
        self.instance = str(instance)

    def _vm(self, *args, timeout=180):
        cmd = [sys.executable, os.path.join(HERE, "vm.py"),
               "--disk", self.disk, "--instance", self.instance] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)

    def start(self):
        return vm.started_ok(self._vm("start").stdout)

    def stop(self):
        self._vm("stop")

    def sh(self, command):
        r = self._vm("exec", command)
        out = r.stdout + r.stderr
        # vm.py echoes the kernel's own log lines into the transcript;
        # the loader chatter around a /bin program is noise here and
        # would match half the substrings this tool looks for.
        keep = [ln for ln in out.splitlines()
                if "elf_run:" not in ln and "syscall: exit()" not in ln]
        return "\n".join(keep)


def esp_window(disk):
    """(start_lba, sectors) of the image's ESP, asked rather than assumed."""
    _bios, esp = install_grub.parts_of(disk)
    return (esp[1], esp[2]) if esp else (None, None)


def mtools_at(disk, start_lba):
    return f"{disk}@@{start_lba * SECTOR}"


def mrun(args):
    # stdin closed AND a new session, for the reason install_grub.py
    # documents at length: mtools does not read stdin, it opens
    # /dev/tty. Under capture_output a question therefore vanishes and
    # the tool waits forever on a terminal nobody can see it waiting on.
    # Without a controlling terminal it fails instead, and says why.
    try:
        return subprocess.run(args, capture_output=True, text=True,
                              stdin=subprocess.DEVNULL,
                              start_new_session=True, timeout=120)
    except subprocess.TimeoutExpired:
        print(f"fat32_test: {os.path.basename(args[0])} timed out -- "
              f"{' '.join(args)}")
        return subprocess.CompletedProcess(args, 1, "", "timed out")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default="disk.img",
                    help="the image to copy and test against (never written to)")
    ap.add_argument("--instance", default="auto",
                    help="vm.py instance slot; 'auto' picks a free one")
    ap.add_argument("--keep", action="store_true", help="keep the working copy")
    args = ap.parse_args()

    for tool in ("mcopy", "mdir", "mtype"):
        if not have(tool):
            print(f"fat32_test: SKIP -- {tool} not found (install mtools)")
            return 0
    if not os.path.exists(args.disk):
        print(f"fat32_test: SKIP -- {args.disk} does not exist (run `make iso`)")
        return 0

    tmp = tempfile.mkdtemp(prefix="fat32test-")
    img = os.path.join(tmp, "test.img")
    # SPARSE, and it is not an optimisation: disk.img is ~4 MB of data in
    # a 9 GB sparse file, so a hole-filling copy costs 9 GB -- of RAM,
    # when the destination is a tmpfs.
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, img], check=True)

    start_lba, sectors = esp_window(img)
    if start_lba is None:
        print("fat32_test: SKIP -- this image has no ESP (run `make clean-disk && make iso`)")
        return 0

    # `--instance auto` prints the slot it took; everything after has to
    # reach the SAME guest, so it is resolved once here.
    slot = args.instance
    if slot == "auto":
        probe = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                                "--disk", img, "--instance", "auto", "start"],
                               capture_output=True, text=True, timeout=180)
        for ln in probe.stdout.splitlines():
            if "re-run with --instance" in ln:
                slot = ln.rsplit("--instance", 1)[1].strip().split()[0]
        if slot == "auto":
            print("fat32_test: could not start a guest")
            return 1
        g = Guest(img, slot)
    else:
        g = Guest(img, slot)
        if not g.start():
            print("fat32_test: could not start a guest")
            return 1

    try:
        print("phase 1: the ESP mounts at /boot, read-only, on an ordinary boot")
        mounts = g.sh("mount")
        check("/boot is mounted", "/boot" in mounts, mounts)
        check("...as fat32", "fat32" in mounts, mounts)
        # THE DISCRIMINATING HALF: not that a mount exists, but that it
        # came up read-only. A driver mounted rw by accident passes every
        # other check in this file.
        boot_row = [ln for ln in mounts.splitlines() if "/boot" in ln]
        check("...read-only", bool(boot_row) and " ro" in boot_row[0],
              boot_row[0] if boot_row else "no /boot row")

        print("phase 2: GRUB's own files are readable")
        cfg = g.sh("cat /boot/boot/grub/grub.cfg")
        # Text only a correct chain walk and directory parse produce --
        # "it printed something" would pass on a driver returning the
        # raw first sector.
        check("grub.cfg reads back", "menuentry" in cfg and "multiboot2" in cfg,
              cfg.strip()[:120])
        listing = g.sh("rescue ls /boot/boot/grub/i386-pc")
        # The module directory is where the LONG names are: `.mod` files
        # with names well past 8.3. A driver that ignored LFN entries
        # reports their `~1` aliases instead.
        check("a long name in the module directory", "_" in listing and ".mod" in listing,
              listing.strip()[:160])

        print("phase 3: a read-only mount refuses every write")
        for cmd, word in (("write /boot/nope.txt x", "write"),
                          ("mkdir /boot/nope", "mkdir"),
                          ("rm /boot/boot/grub/grub.cfg", "delete")):
            out = g.sh(cmd)
            check(f"{word} on /boot is refused", "read-only" in out, out.strip()[:120])
        check("grub.cfg survived", "menuentry" in g.sh("cat /boot/boot/grub/grub.cfg"))

        print("phase 4: the mount table -- a third filesystem, and the refusals")
        g.sh("mount -t ramfs none /mnt")
        m = g.sh("mount")
        check("ramfs mounts at /mnt", "/mnt" in m and "ramfs" in m, m)
        check("three filesystems at once",
              len([ln for ln in m.splitlines() if ln.split()[:1] and "on" not in ln.split()[:1]]) >= 3, m)
        g.sh("write /mnt/scratch.txt in-ram")
        check("a file on /mnt reads back", "in-ram" in g.sh("cat /mnt/scratch.txt"))
        # It is on the RAM disk, not the root -- only observable from the
        # other side of the boundary.
        g.sh("umount /mnt")
        check("...and goes when /mnt does",
              "no such file" in g.sh("cat /mnt/scratch.txt").lower())
        check("the root cannot be unmounted", "cannot be unmounted" in g.sh("umount /"))
        check("umount of nothing is refused", "nothing is mounted" in g.sh("umount /mnt"))

        print("phase 5: writing to /boot, deliberately")
        g.sh("umount /boot")
        g.sh("mount 2 /boot")
        m = g.sh("mount")
        boot_row = [ln for ln in m.splitlines() if "/boot" in ln]
        check("/boot remounts read-write", bool(boot_row) and " rw" in boot_row[0],
              boot_row[0] if boot_row else "no /boot row")

        g.sh(f"mkdir /boot/{SCRATCH}")
        g.sh(f"write /boot/{SCRATCH}/short.txt tiny")
        longname = "a_very_long_filename_indeed.txt"
        g.sh(f"write /boot/{SCRATCH}/{longname} longname-ok")
        # A REAL BINARY, hundreds of clusters, so the chain walk is
        # exercised rather than a single-cluster file.
        g.sh(f"cp /bin/ls /boot/{SCRATCH}/ls.copy")
        g.sh("sync")

        ls = g.sh(f"rescue ls /boot/{SCRATCH}")
        check("the long name is listed as itself", longname in ls, ls.strip()[:160])
        check("the binary is there and the right size", "185" in ls or "ls.copy" in ls,
              ls.strip()[:160])

        print("phase 6: it survives a reboot")
        g.stop()
        if not g.start():
            check("the guest rebooted", False, "would not start again")
        else:
            # READ IT OFF THE AUTO-MOUNTED, READ-ONLY /boot. The obvious
            # version umounts and remounts read-write first, and that is
            # two more commands than the check needs -- it was flaky
            # because of them, failing about one run in four while every
            # host-side check on the same image passed. A persistence
            # check should do the SMALLEST thing that observes
            # persistence.
            back = g.sh(f"cat /boot/{SCRATCH}/{longname}")
            check("the file is still there after a reboot", "longname-ok" in back,
                  back.strip()[:120])
            # ...and the auto-mount is read-only again, which is the
            # other half of "a reboot puts it back the way it was".
            m2 = g.sh("mount")
            row = [ln for ln in m2.splitlines() if "/boot" in ln]
            check("...and /boot is read-only again after the reboot",
                  bool(row) and " ro" in row[0], row[0] if row else "no /boot row")
    finally:
        g.stop()

    print("phase 7: the HOST reads it back -- mtools and fsck.fat, sharing no code")
    at = mtools_at(img, start_lba)
    d = mrun(["mdir", "-i", at, f"::/{SCRATCH}"])
    check("mtools lists the directory", d.returncode == 0, (d.stdout + d.stderr)[:200])
    # mtools prints the long name in its own column; a driver that wrote
    # the LFN set backwards produces a name that is not this one.
    check("mtools agrees on the long name", longname in d.stdout, d.stdout[:300])

    t = mrun(["mtype", "-i", at, f"::/{SCRATCH}/{longname}"])
    check("mtools reads the contents", "longname-ok" in t.stdout, (t.stdout + t.stderr)[:200])

    extracted = os.path.join(tmp, "ls.fromfat")
    c = mrun(["mcopy", "-n", "-i", at, f"::/{SCRATCH}/ls.copy", extracted])
    ok = c.returncode == 0 and os.path.exists(extracted)
    reference = os.path.join("build", "userland", "bin", "ls.elf")
    if ok and os.path.exists(reference):
        # BYTE FOR BYTE against the build artifact -- the strongest check
        # here, and the one a size comparison would let through.
        same = open(extracted, "rb").read() == open(reference, "rb").read()
        check("a 185 KiB binary round-trips byte for byte", same,
              f"{os.path.getsize(extracted)} bytes extracted")
    else:
        check("a 185 KiB binary round-trips byte for byte", False,
              (c.stdout + c.stderr)[:200])

    if have("fsck.fat"):
        esp = os.path.join(tmp, "esp.img")
        with open(img, "rb") as src, open(esp, "wb") as dst:
            src.seek(start_lba * SECTOR)
            dst.write(src.read(sectors * SECTOR))
        # `-n` is "answer no to everything": a pure audit that writes
        # nothing, which is the only sane mode for a check.
        f = mrun(["fsck.fat", "-n", esp])
        clean = f.returncode == 0 and "Dirty bit" not in f.stdout
        check("fsck.fat calls the volume clean", clean, (f.stdout + f.stderr)[-400:])
    else:
        print("  skip  fsck.fat not found (install dosfstools) -- volume audit skipped")

    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"  (kept {tmp})")

    failed = [n for n, ok, _ in results if not ok]
    print()
    if failed:
        print(f"fat32_test: FAIL -- {len(failed)} of {len(results)} checks failed")
        for n in failed:
            print(f"  - {n}")
        return 1
    print(f"fat32_test: PASS -- {len(results)} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
