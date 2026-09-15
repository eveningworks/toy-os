#!/usr/bin/env python3
"""tools/install_grub.py -- put GRUB, and the kernel, ON disk.img.

toy-os boots from its own disk now: the kernel lives at /boot/kernel.bin
inside a FAT32 partition, GRUB's core.img is embedded in a BIOS boot
partition in front of it, and the ISO is only one of the media rather
than the only one. This is the tool that writes all three pieces.

WHY THERE IS A SEPARATE BOOT PARTITION AT ALL. GRUB cannot read TFS3 --
there is no module for it and writing one means a third implementation
of the format, in GRUB's source tree, under GPLv3. Every real system
that hits this answers it the same way: a small boot volume in a
filesystem the loader ALREADY understands. Linux gets away with /boot on
ext4 because GRUB ships an ext4 driver; where it does not (early btrfs,
ZFS, an encrypted root) the answer is a separate /boot, and UEFI made
that universal -- the ESP is FAT32 because firmware speaks only FAT.
Windows does the same with its System Reserved volume. So: FAT32, which
mtools writes from the host with no root and no loop device, and which
the planned FAT32 backend will one day let toy-os read for itself.

WHAT IT DOES NOT DO. It does not partition -- tools/seed_disk.py owns
the table, and this tool refuses an image that has no boot partition
rather than reshaping one (`make clean-disk` is how a checkout adopts a
new layout, deliberately, the same rule the seeder follows). It does not
touch the filesystem partition. And it installs the BIOS (i386-pc)
loader only; a UEFI one would be a second core image in the same ESP,
which is the point of typing that partition as an ESP now.

THE THREE PLACES A BYTE HAS TO LAND, and what makes each one work:

  LBA 0        GRUB's boot.img, with two patches: the LBA of core.img at
               offset 0x5c, and the partition table + disk signature
               (bytes 0x1b8-0x200) kept from what was already there. A
               boot sector that forgets the second one boots beautifully
               and describes an empty disk.
  BIOS boot    core.img, contiguous -- or, on an MBR, the gap between
  partition    the boot sector and the first partition. Its FIRST sector
               is diskboot.img, which finds the REST of core.img
               through a block list in
               its own last 12 bytes -- (start LBA, sector count,
               segment) at 0x1f4. grub-bios-setup patches that; so does
               this, and a core.img written without it loads one sector
               and jumps into nothing.
  FAT32 /boot  kernel.bin, grub.cfg, and the module directory. The paths
               are the ISO's paths exactly, so ONE grub.cfg serves both
               media and neither drifts.

THE FAT VOLUME IS FORMATTED ONCE, not per build -- reformatting would
throw away anything written into /boot from inside toy-os, which is the
whole point of the partition being real. The module directory carries a
stamp naming the GRUB that wrote it, so a host GRUB upgrade re-copies
the modules rather than leaving them one version behind core.img.

Usage:
    python3 tools/install_grub.py disk.img --kernel build/kernel.bin \\
                                           --grub-cfg build/grub-disk.cfg
    python3 tools/install_grub.py disk.img --check
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import mkpart_test  # noqa: E402  (path set above)

SECTOR = 512

# Where GRUB's i386-pc module directory lives, by distribution. Same
# shape as the Makefile's grub-mkrescue/grub2-mkrescue fallback, and for
# the same reason: this repo should build on a checkout that installed
# GRUB under either name.
MODULE_DIRS = ("/usr/lib/grub/i386-pc", "/usr/share/grub/i386-pc",
               "/usr/lib/grub2/i386-pc")

# What core.img must be able to do BEFORE it can read a config file:
# find the disk (biosdisk), read the table (part_gpt/part_msdos), read
# the filesystem (fat), and run a menu (normal). multiboot2 is what
# actually boots this kernel, and all_video is what gives it a
# FRAMEBUFFER -- without it GRUB hands over no framebuffer tag and the
# console silently falls back to VGA text, which looks like a display
# driver bug and is not one.
CORE_MODULES = ("biosdisk", "part_gpt", "part_msdos", "fat", "normal",
                "configfile", "multiboot2", "all_video", "gfxterm", "echo",
                "test", "search", "search_fs_uuid", "search_label", "ls",
                # GZIO -- so a GZIPPED kernel boots from the disk. GRUB
                # decompresses any file whose CONTENT starts with the gzip
                # magic, but only if this module is in the core image; the
                # rescue ISO gets it from grub-mkrescue's full module set,
                # and this one has only what is listed here. Without it a
                # compressed kernel fails SILENTLY -- measured: no serial
                # output at all, which reads like a dead machine rather
                # than a missing module.
                "gzio")

# mformat picks a volume serial from the clock unless told otherwise,
# and this repo builds byte-identical images from identical inputs
# (mkpart_test.py's GUIDs make the same promise). Fixed: "toy" + NUL.
FAT_SERIAL = "746F7900"
FAT_LABEL = "TOYOSBOOT"

# The stamp that says which GRUB wrote the module directory, so a host
# upgrade re-copies them instead of leaving a module set one version
# behind the core.img that loads it.
STAMP = "::/boot/grub/i386-pc/toyos.stamp"


def _tool(*names):
    for n in names:
        found = shutil.which(n)
        if found:
            return found
    return None


def module_dir():
    for d in MODULE_DIRS:
        if os.path.isdir(d):
            return d
    return None


# Every child here runs in a NEW SESSION, with stdin closed and a
# timeout. All three are load-bearing, and the first one is the whole
# reason this comment exists.
#
# `capture_output=True` sends a child's prompt into a pipe nobody reads.
# The obvious guard is to close stdin -- and it is not enough, because
# **mtools does not read stdin. It opens /dev/tty and reads the
# controlling terminal directly.** So the question vanishes into the
# captured stderr, mtools blocks forever on a terminal the operator
# cannot see it waiting on, and `make iso` stops dead after the last
# echoed command with no output and no error. Measured from a hung
# build: `mmd` sat in wait_woken with fd 0 on /dev/null, fd 2 on a pipe,
# and **fd 4 on /dev/tty**.
#
# `start_new_session=True` is what actually fixes it: the child becomes
# a session leader with NO controlling terminal, so its open("/dev/tty")
# fails with ENXIO and it must give up -- at which point its complaint
# lands in the captured stderr and _run() reports it. An invisible
# infinite hang becomes a visible error naming the real problem.
#
# The cost, stated because it is real: a child in its own session does
# not receive the terminal's Ctrl-C. The timeout is what bounds it, and
# it also catches the case a session change cannot -- mtools looping on
# a damaged FAT chain rather than asking a question.
#
# 60s is already two orders of magnitude over the real cost -- every
# child here is sub-second in a normal build, and the slowest (mcopy of
# GRUB's ~300 modules) is a couple of seconds on a slow disk. It is
# deliberately not larger: a budget nobody waits out teaches nothing,
# and the first person to hit this killed the build by hand at 18s.
RUN_TIMEOUT = 60

# Filled in by install() once the ESP's offset is known, so the timeout
# message can print a command that is copy-pasteable rather than one the
# reader has to work out. A plain default keeps _run() usable before
# then (and in a unit test).
_FSCK_HINT = "/tmp/boot.img   # dd the /boot partition out first"


def _timeout_exit(cmd, seconds):
    """One message for every child that overruns, wherever it was run.

    A raw TimeoutExpired traceback names the command too, but it buries
    it in twenty lines of subprocess internals and says nothing about
    what to do -- which is exactly what the first report of this looked
    like.
    """
    sys.exit(
            f"install_grub: {os.path.basename(cmd[0])} did not finish in "
            f"{seconds}s and was killed.\n"
            f"  command: {' '.join(cmd)}\n"
            "\n"
            "  `make clean-disk && make iso` rebuilds the image and clears it.\n"
            "\n"
            "  Before you do, this is worth capturing -- it is the one state\n"
            "  that cannot be recovered afterwards:\n"
            f"    fsck.fat -n -v {_FSCK_HINT}\n"
            "  and, while it is still stuck, from another terminal:\n"
            "    pidof mmd mcopy mformat mdir | xargs -r -n1 sh -c \\\n"
            "      'echo \"== $0\"; cat /proc/$0/wchan; echo; cat /proc/$0/stack 2>/dev/null'\n"
            "\n"
            "  Nothing in a normal build takes this long; please report it.")


def _run(cmd, **kw):
    kw.setdefault("timeout", RUN_TIMEOUT)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           stdin=subprocess.DEVNULL,
                           start_new_session=True, **kw)
    except subprocess.TimeoutExpired:
        _timeout_exit(cmd, kw["timeout"])
    if r.returncode != 0:
        sys.exit(f"install_grub: {cmd[0]} failed: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout


def parts_of(disk):
    """(embed, esp) as (index, start_lba, sectors), or (None, None).

    EMBED is where core.img goes, and the two table formats answer it
    differently -- which is the same split every BIOS machine has:

      GPT   a BIOS boot partition, because a GPT's own structures run
            from LBA 1 to 33 and leave no gap to hide in. That is the
            entire reason the type was invented.
      MBR   the gap between the boot sector and the first partition,
            LBA 1 up to LBA 2047 on any 1 MiB-aligned table. This is
            where core.img has lived since GRUB 2 shipped.

    The INDEX is 1-based and is what GRUB's `(hd0,gptN)` counts, which is
    why it is carried rather than recomputed at the call site; the MBR
    gap has no index, so it reports 0.
    """
    parts = mkpart_test.read_table(disk, with_kind=True)
    bios = esp = None
    for i, (start, count, kind) in enumerate(parts):
        if kind == "bios" and bios is None:
            bios = (i + 1, start, count)
        elif kind == "esp" and esp is None:
            esp = (i + 1, start, count)
    if bios is None and parts and not _is_gpt(disk):
        gap = min(start for start, _n, _k in parts) - 1
        if gap > 0:
            bios = (0, 1, gap)
    return bios, esp


def is_bootable(disk):
    """Can a BIOS boot this image?

    Asked by every launcher in tools/ to decide between `-boot order=c`
    and the CD -- so it must answer for an image this tool has never
    touched, i.e. every disk.img built before the boot partition
    existed. Those have a protective MBR (which SeaBIOS will happily
    boot and then hang inside), so "it has a table" is exactly the wrong
    test. What is checked is GRUB's own stamp in the boot sector plus a
    boot partition to have installed into.
    """
    try:
        if not os.path.getsize(disk):
            return False
        with open(disk, "rb") as f:
            mbr = f.read(SECTOR)
    except OSError:
        return False
    if len(mbr) < SECTOR or mbr[510:512] != b"\x55\xaa":
        return False
    if b"GRUB" not in mbr:
        return False
    bios, esp = parts_of(disk)
    return bios is not None and esp is not None


def boot_medium(disk, override=None):
    """'disk' or 'cd' -- which medium a QEMU launch should boot from.

    ONE implementation, asked by the Makefile and by every launcher in
    tools/, because the wrong answer in the 'disk' direction does not
    fail: SeaBIOS boots any image with 0x55AA at LBA 0 -- which every
    partitioned data disk has -- and hangs inside the partition table
    with no serial output at all, looking exactly like a kernel that
    died before its first print.

    `override` is a user's `--boot`/`BOOT=`: 'disk' or 'cd'.
    """
    if override in ("disk", "cd"):
        return override
    if not disk:
        return "cd"
    return "disk" if is_bootable(disk) else "cd"


def qemu_boot_args(medium, iso):
    return ["-boot", "order=c"] if medium == "disk" else ["-boot", "order=d", "-cdrom", iso]


def mtools_at(disk, esp):
    """mtools' `-i <file>@@<offset>` form for this image's ESP."""
    return f"{disk}@@{esp[1] * SECTOR}"


def fat_present(disk, esp):
    with open(disk, "rb") as f:
        f.seek(esp[1] * SECTOR)
        boot = f.read(SECTOR)
    return len(boot) == SECTOR and boot[510:512] == b"\x55\xaa" and (
        boot[0x52:0x57] == b"FAT32" or boot[0x36:0x3B].startswith(b"FAT"))


def format_fat(disk, esp):
    mformat = _tool("mformat")
    if not mformat:
        sys.exit("install_grub: mformat not found -- install mtools "
                 "(the same package grub-mkrescue needs).")
    # -T: the volume is a WINDOW into a much larger file, so mtools must
    # be told its size rather than inferring it from the file.
    _run([mformat, "-i", mtools_at(disk, esp), "-F", "-T", str(esp[2]),
          "-N", FAT_SERIAL, "-v", FAT_LABEL, "::"])


def mcopy_into(disk, esp, files, dest):
    mcopy = _tool("mcopy")
    if not mcopy:
        sys.exit("install_grub: mcopy not found -- install mtools.")
    _run([mcopy, "-i", mtools_at(disk, esp), "-o", "-s", *files, dest])


def mkdirs(disk, esp, dirs):
    mmd = _tool("mmd")
    if not mmd:
        sys.exit("install_grub: mmd not found -- install mtools.")
    for d in dirs:
        # An existing directory is not an error here -- this tool runs on
        # every build and only the first one creates anything.
        # A non-zero exit is EXPECTED and ignored -- the directory
        # usually already exists -- but a TIMEOUT is not, and must not
        # surface as a raw traceback out of subprocess internals.
        try:
            subprocess.run([mmd, "-i", mtools_at(disk, esp), d],
                           stdin=subprocess.DEVNULL, start_new_session=True,
                           timeout=RUN_TIMEOUT, capture_output=True, text=True)
        except subprocess.TimeoutExpired:
            _timeout_exit([mmd, "-i", mtools_at(disk, esp), d], RUN_TIMEOUT)


def mtype(disk, esp, path):
    mtype_bin = _tool("mtype")
    if not mtype_bin:
        return None   # no stamp readable -- the modules get re-copied, which is safe
    try:
        r = subprocess.run([mtype_bin, "-i", mtools_at(disk, esp), path],
                           capture_output=True, text=True,
                           stdin=subprocess.DEVNULL, start_new_session=True,
                           timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        # Swallowed deliberately, unlike everywhere else here: an
        # unreadable stamp just means the modules get re-copied, and the
        # mcopy that follows will hit the same wall and report properly.
        return None
    return r.stdout if r.returncode == 0 else None


def grub_version(mkimage):
    return _run([mkimage, "--version"]).strip()


def has_boot_partition(disk):
    bios, esp = parts_of(disk)
    return bios is not None and esp is not None


# THE PAYLOAD toy-os NEEDS TO INSTALL ITSELF, staged into the root
# filesystem as ordinary files.
#
# WHY IT IS NOT `/boot`. On a disk boot, `/boot` is the ESP and already
# holds the kernel and grub.cfg -- but a LIVE boot has no ESP at all
# (GRUB loads the kernel and a filesystem image into RAM, and after that
# nothing drives the medium; there is no USB mass-storage driver here).
# Installing from live media is the path a real machine uses, so the
# payload travels in the root, where every toy-os filesystem carries it
# and `/bin/install` reads ONE path on every medium.
#
# WHAT IS NOT IN IT: GRUB's module directory, 305 files and ~4 MB.
# `core.img` already contains every module grub.cfg's `insmod` asks for,
# so a target installed without them boots; what it cannot do is have a
# HOST `grub-install` run against it later without re-copying them.
#
# THE PREFIX IS FIXED HERE, and it is what makes the layout in
# `/bin/install` a constraint rather than a preference: core.img carries
# `(hd0,gpt2)/boot/grub` baked in, so the ESP must be partition 2 on
# whatever this ends up installed onto.
PAYLOAD_PREFIX = "(hd0,gpt2)/boot/grub"

# THE SECOND CORE IMAGE, for `install --mbr`. A prefix is baked in at
# mkimage time and names the TABLE FORMAT as well as the partition
# number, so one image cannot serve both layouts -- and the target has
# no grub-mkimage to build its own. Staging both here is what lets the
# installer offer a choice at all.
#
# msdos1, not msdos2: an MBR install needs no BIOS-boot partition (the
# core image goes in the gap before the first partition), so the FAT32
# boot partition is the first one.
PAYLOAD_PREFIX_MBR = "(hd0,msdos1)/boot/grub"


def stage_payload(outdir, kernel, grub_cfg, verbose=True):
    mkimage = _tool("grub-mkimage", "grub2-mkimage")
    mods = module_dir()
    if not mkimage or not mods:
        # A checkout without GRUB's BIOS target still builds and still
        # boots; what it cannot do is produce media that installs itself.
        # Said rather than failed, the same call `install(optional=True)`
        # makes one function down.
        print("install_grub: no grub-mkimage or i386-pc modules -- "
              f"{outdir} not staged, so this build cannot install itself")
        return False

    os.makedirs(outdir, exist_ok=True)
    core = os.path.join(outdir, "core.img")
    _run([mkimage, "-O", "i386-pc", "-d", mods, "-p", PAYLOAD_PREFIX,
          "-o", core, *CORE_MODULES])
    _run([mkimage, "-O", "i386-pc", "-d", mods, "-p", PAYLOAD_PREFIX_MBR,
          "-o", os.path.join(outdir, "core-msdos.img"), *CORE_MODULES])
    shutil.copyfile(os.path.join(mods, "boot.img"), os.path.join(outdir, "boot.img"))
    shutil.copyfile(kernel, os.path.join(outdir, "kernel.bin"))
    shutil.copyfile(grub_cfg, os.path.join(outdir, "grub.cfg"))
    # WHAT THIS CORE IMAGE CAN DO, as a line of module names. `install
    # --bootloader` copies it to /etc/grub-core.modules on the machine it
    # writes, which is the only readable record of what the bootloader in
    # the embed area is -- core.img lives in raw sectors nothing can open,
    # and it is lzma-compressed, so its module set cannot be recovered by
    # looking. `remote.py flash` reads that stamp before it dares send a
    # gzipped kernel, which GRUB unpacks only with `gzio` present.
    with open(os.path.join(outdir, "core.modules"), "w") as f:
        f.write(" ".join(CORE_MODULES) + "\n")
    if verbose:
        total = sum(os.path.getsize(os.path.join(outdir, f))
                    for f in os.listdir(outdir))
        print(f"install_grub: staged the install payload into {outdir} "
              f"({total // 1024} KiB, prefixes {PAYLOAD_PREFIX} "
              f"and {PAYLOAD_PREFIX_MBR})")
    return True


def install(disk, kernel, grub_cfg, verbose=True, optional=False):
    mkimage = _tool("grub-mkimage", "grub2-mkimage")
    if not mkimage:
        sys.exit("install_grub: grub-mkimage not found (looked for "
                 "grub-mkimage and grub2-mkimage). Install GRUB's tools -- "
                 "see README.md's dependency table.")
    mods = module_dir()
    if not mods:
        sys.exit("install_grub: no GRUB i386-pc module directory "
                 f"(looked in {', '.join(MODULE_DIRS)}). Install GRUB's "
                 "BIOS target -- see README.md's dependency table.")

    bios, esp = parts_of(disk)
    if not bios or not esp:
        msg = (f"install_grub: {disk} has no boot partition. It predates the "
               "bootable layout -- `make clean-disk && make iso` rebuilds it "
               "(destroying what is on it, which is why nothing does that "
               "for you).")
        if optional:
            # The BUILD's call. A checkout whose image predates this
            # layout must keep building and keep booting, off the ISO --
            # so this is a fact, not a failure. seed_disk.py says the
            # same thing at more length one line earlier.
            print(msg)
            return
        sys.exit(msg)

    global _FSCK_HINT
    # A REAL FILE, not a <(process substitution): fsck.fat seeks, and a
    # pipe cannot be seeked -- the first person handed the substitution
    # form got "Seek to 0:Illegal seek" and learned nothing.
    _FSCK_HINT = (f"/tmp/boot.img   # dd if={disk} bs=512 skip={esp[1]} "
                  f"count={esp[2]} of=/tmp/boot.img status=none")

    # ---- 1. the FAT32 /boot volume ---------------------------------
    fresh = not fat_present(disk, esp)
    if fresh:
        format_fat(disk, esp)
    mkdirs(disk, esp, ["::/boot", "::/boot/grub", "::/boot/grub/i386-pc"])
    mcopy_into(disk, esp, [kernel], "::/boot/kernel.bin")
    mcopy_into(disk, esp, [grub_cfg], "::/boot/grub/grub.cfg")

    stamp = grub_version(mkimage)
    if fresh or (mtype(disk, esp, STAMP) or "").strip() != stamp:
        mod_files = [os.path.join(mods, f) for f in sorted(os.listdir(mods))
                     if f.endswith((".mod", ".lst"))]
        mcopy_into(disk, esp, mod_files, "::/boot/grub/i386-pc/")
        stamp_path = os.path.join(os.path.dirname(os.path.abspath(kernel)),
                                  ".grub-stamp")
        with open(stamp_path, "w") as f:
            f.write(stamp + "\n")
        mcopy_into(disk, esp, [stamp_path], STAMP)

    # ---- 2. core.img, into the BIOS boot partition ------------------
    #
    # The prefix is where GRUB looks for grub.cfg and for any module it
    # still has to load. `(hd0,gptN)` names the FIRST BIOS disk: every
    # way this repo boots attaches exactly one, and a machine where the
    # disk is not hd0 wants a `search --file` in the config rather than
    # a cleverer guess here.
    scheme = "gpt" if _is_gpt(disk) else "msdos"
    prefix = f"(hd0,{scheme}{esp[0]})/boot/grub"
    core_path = os.path.join(os.path.dirname(os.path.abspath(kernel)), "core.img")
    _run([mkimage, "-O", "i386-pc", "-d", mods, "-p", prefix,
          "-o", core_path, *CORE_MODULES])

    core = bytearray(open(core_path, "rb").read())
    core += b"\0" * (-len(core) % SECTOR)
    nsec = len(core) // SECTOR
    if nsec > bios[2]:
        sys.exit(f"install_grub: core.img is {nsec} sectors and there is room "
                 f"for {bios[2]} -- enlarge the BIOS boot partition in "
                 f"seed_disk.py, or align the first partition further out.")

    boot = bytearray(open(os.path.join(mods, "boot.img"), "rb").read())
    with open(disk, "r+b") as f:
        mbr = bytearray(f.read(SECTOR))
        # Where boot.img must jump to read the rest of GRUB...
        struct.pack_into("<Q", boot, 0x5C, bios[1])
        # ...and the disk signature + partition table, which belong to
        # the disk and not to the boot code being written over them.
        boot[0x1B8:0x200] = mbr[0x1B8:0x200]
        # diskboot.img's own block list: the rest of core.img, contiguous.
        struct.pack_into("<Q", core, 0x1F4, bios[1] + 1)
        struct.pack_into("<H", core, 0x1FC, nsec - 1)
        f.seek(0)
        f.write(boot)
        f.seek(bios[1] * SECTOR)
        f.write(core)

    # ---- 3. the two images, as FILES in the ESP ---------------------
    #
    # So that toy-os can install itself. `/bin/install` cannot read raw
    # sectors -- there is no syscall for it, deliberately -- so the only
    # way it can put a bootloader on a target disk is to be handed the
    # bytes, and the only place it can get them is a file. This is where
    # grub-install puts them on a real system too.
    #
    # UNPATCHED, both of them: the two patches depend on where the
    # images land, which is the target's business and not this one's.
    # SYS_INSTALL_BOOT applies them.
    mcopy_into(disk, esp, [os.path.join(mods, "boot.img"), core_path],
               "::/boot/grub/i386-pc/")

    if verbose:
        where = f"partition {bios[0]}" if bios[0] else "the MBR gap"
        print(f"install_grub: {disk} -- core.img in {where} at LBA {bios[1]} ({nsec} sectors), "
              f"/boot on partition {esp[0]} ({esp[2] // 2048} MiB), prefix {prefix}"
              + (" [formatted]" if fresh else ""))


def _is_gpt(disk):
    with open(disk, "rb") as f:
        mbr = f.read(SECTOR)
        return any(mbr[446 + i * 16 + 4] == 0xEE for i in range(4))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk", nargs="?",
                    help="the image to install onto; omitted with --stage-payload")
    ap.add_argument("--kernel", help="the kernel to install at /boot/kernel.bin")
    ap.add_argument("--grub-cfg", help="the grub.cfg to install at /boot/grub/grub.cfg")
    ap.add_argument("--stage-payload", metavar="DIR",
                    help="write the four files /bin/install needs (kernel.bin, "
                         "grub.cfg, boot.img, core.img) into DIR instead of "
                         "installing onto a disk")
    ap.add_argument("--check", action="store_true",
                    help="print whether this image is bootable, and exit non-zero if not")
    ap.add_argument("--optional", action="store_true",
                    help="an image with no boot partition is reported and "
                         "skipped rather than being an error -- what the build "
                         "passes, so a pre-existing disk.img keeps working")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    if args.stage_payload:
        if not args.kernel or not args.grub_cfg:
            sys.exit("install_grub: --stage-payload needs --kernel and --grub-cfg")
        stage_payload(args.stage_payload, args.kernel, args.grub_cfg,
                      verbose=not args.quiet)
        return

    if args.check:
        ok = is_bootable(args.disk)
        print(f"{args.disk}: {'bootable' if ok else 'NOT bootable (no GRUB installed)'}")
        return 0 if ok else 1

    if not (args.kernel and args.grub_cfg):
        ap.error("--kernel and --grub-cfg are both required (or use --check)")
    install(args.disk, args.kernel, args.grub_cfg, verbose=not args.quiet,
            optional=args.optional)
    return 0


if __name__ == "__main__":
    sys.exit(main())
