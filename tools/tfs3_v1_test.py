#!/usr/bin/env python3
"""Prove the kernel still mounts and uses a TFS3 **v1** image.

TFS3 got a second on-disk layout (v2: a 32-slot journal, GDT at 42,
group 0 at 58 -- see docs/tfs3-spec.md). v1 images stay mountable
read/write on purpose, and NOT out of politeness: a probe that answered
"not mine" would hand the image to the VFS's blank-disk policy, which
formats it. Refusing to read an old format is a way of destroying it.

That support had no test and, worse, no way to get tested: once
`format` started writing v2, nothing in the repo could produce a v1
image at all. This closes both halves -- `tfs3_writer.py format
--fs-version 1` writes one, and this drives a kernel against it.
Same reasoning as `ata nodma` making the PIO path reachable: a fallback
nothing can reach is a guess, not a fallback.

What it asserts (7 checks):
  1. the kernel mounts it, and says v1 with 4 journal slots
  2. a file written by the HOST tool reads back in the OS
  3. a plain file rename works (3 credits -- fits v1)
  4. a same-parent directory rename works (3 credits)
  5. a cross-parent directory move is REFUSED, with the message naming
     the fix -- the one operation v1's journal genuinely cannot hold
  6. that refusal changed nothing (both directories exactly as before)
  7. truncate works, and fsck is clean at the end

Check 5 is the point. It is the only place the credit reservation
(`txn_begin()`) is observable end to end, and a version of the kernel
that "helpfully" attempted the move anyway would corrupt the namespace
rather than fail. Run this after touching TFS3's geometry, journal, or
any operation's credit count.

    python3 tools/tfs3_v1_test.py [--keep]
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WRITER = os.path.join(HERE, "tfs3_writer.py")
VM = os.path.join(HERE, "vm.py")

# Slot 3: keeps this runnable alongside a slot-0 VM someone already has
# up, the same reason gui_regress.py leases slots (see CLAUDE.md).
SLOT = "3"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not ok else ""))
    return ok


def run(*args):
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr, file=sys.stderr)
        raise SystemExit(f"command failed: {' '.join(args)}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the image for poking at")
    args = ap.parse_args()

    if not os.path.exists(os.path.join(ROOT, "toy-os.iso")):
        raise SystemExit("toy-os.iso missing -- run `make iso` first")

    tmp = tempfile.mkdtemp(prefix="tfs3v1-")
    img = os.path.join(tmp, "v1.img")
    seeded = os.path.join(tmp, "marker.txt")
    with open(seeded, "w") as f:
        f.write("written-by-the-host-tool-into-a-v1-image\n")

    print(f"tfs3_v1_test: building a v1 image at {img}")
    # 512 MB is plenty and keeps the format fast; the geometry under
    # test doesn't vary with volume size.
    #
    # PARTITIONED, because the kernel refuses a whole-disk volume now
    # (kernel/fs/vfs.c, docs/rootfs-design.md) -- a flat v1 image would
    # boot to ramfs and every check below would fail for a reason that
    # has nothing to do with v1. The layout is one data partition: this
    # image is data, booted from the ISO, so it needs no boot partition.
    sys.path.insert(0, HERE)
    import mkpart_test
    with open(img, "wb") as f:
        f.truncate(512 * 1024 * 1024)
    mkpart_test.real_gpt(img, "rest")
    base, sectors = mkpart_test.volume_of(img)
    at = ["--at-lba", str(base), "--sectors", str(sectors)]
    run(sys.executable, WRITER, "format", img, "--fs-version", "1", "--force", *at)
    run(sys.executable, WRITER, "write", img, seeded, "/marker.txt", *at)
    info = run(sys.executable, WRITER, "info", img, *at)
    if "TFS3 v1" not in info:
        raise SystemExit(f"the writer didn't produce a v1 image: {info.strip()}")

    print("tfs3_v1_test: booting it")
    # `stat`, not `ls`: ls is a real /bin/ls ELF, and a freshly
    # formatted image has no /bin, so every directory check would have
    # been measuring whether the test seeded the image rather than
    # whether the kernel renamed anything. (It was, at first.)
    # `rescue cat`, not `cat`. The everyday commands are /bin PROGRAMS
    # now, and this image is a bare filesystem with no /bin on it -- so
    # the kernel's own copies are the only ones that can run here. The
    # tool drove the bare names for months after they moved and every
    # check failed with "`cat` is a program, and /bin does not have it",
    # which is the rot an on-demand tool accumulates when nobody runs it.
    out = run(sys.executable, VM, "--instance", SLOT, "--disk", img, "run",
              "rescue cat /marker.txt",
              "write /a.txt renameme",
              "rescue mv /a.txt /b.txt",
              "rescue cat /b.txt",
              "rescue mkdir /d1", "rescue mkdir /d2", "rescue mkdir /d1/sub",
              "rescue mv /d1/sub /d1/moved",   # same parent: fits v1
              "rescue stat /d1/moved", "rescue stat /d1/sub",
              "rescue mv /d1/moved /d2/moved", # cross parent: must refuse
              "rescue stat /d1/moved", "rescue stat /d2/moved",
              "rescue truncate /b.txt 4", "rescue cat /b.txt",
              "rescue fsck", "rescue dmesg")

    def section(cmd):
        """The output between `--- cmd ---` and the next `---` marker."""
        head = f"--- {cmd} ---"
        if head not in out:
            return ""
        rest = out.split(head, 1)[1]
        return rest.split("\n--- ", 1)[0]

    print("\ntfs3_v1_test: checks")
    # The mount line goes to the kernel log, which `dmesg` replays --
    # vm.py's per-command output doesn't carry the boot log.
    check("kernel mounts the v1 image as v1, with 4 journal slots",
          "tfs3: mounted (v1" in out and "4 journal slots" in out,
          "mount line not in dmesg")
    check("a host-written file reads back in the OS",
          "written-by-the-host-tool-into-a-v1-image" in section("rescue cat /marker.txt"))
    check("a file rename works", "/a.txt -> /b.txt" in section("rescue mv /a.txt /b.txt"))
    check("the renamed file still has its content",
          "renameme" in section("rescue cat /b.txt"))
    check("a same-parent directory rename works",
          "type:     directory" in section("rescue stat /d1/moved")
          and "no such" in section("rescue stat /d1/sub"))

    # `mv` says only that it failed; the WHY is the kernel's line, which
    # reaches `dmesg` and not the command's reply (the kernel log has
    # its own serial port).
    refusal = section("rescue mv /d1/moved /d2/moved")
    log = section("rescue dmesg")
    check("a cross-parent directory move is refused, naming the fix",
          "mv: failed" in refusal and "journal is too small" in log and "fsformat tfs3 confirm" in log,
          refusal.strip()[:80])
    # Refused must mean UNCHANGED -- a half-done move is the failure
    # this whole credit-reservation mechanism exists to prevent. Take
    # the LAST `stat /d1/moved`, the one after the refusal.
    src_after = out.rsplit("--- rescue stat /d1/moved ---", 1)[1].split("\n--- ", 1)[0]
    dst_after = section("rescue stat /d2/moved")
    check("the refusal changed nothing -- source intact, destination absent",
          "type:     directory" in src_after and "no such" in dst_after)
    # The LAST `cat /b.txt` -- section() takes the first, which is the
    # one BEFORE the truncate and still reads "renameme". Asserting on
    # that would have passed whether truncate did anything or not.
    cat_after = out.rsplit("--- rescue cat /b.txt ---", 1)[1].split("\n--- ", 1)[0]
    check("truncate works and fsck is clean",
          cat_after.strip() == "rena" and "fsck: clean" in out,
          repr(cat_after.strip()[:40]))

    if not args.keep:
        subprocess.run(["rm", "-rf", tmp])
    else:
        print(f"\ntfs3_v1_test: image kept at {img}")

    failed = [n for n, ok, _ in results if not ok]
    print(f"\ntfs3_v1_test: {len(results) - len(failed)} passed, {len(failed)} failed")
    if failed:
        print("tfs3_v1_test: FAIL -- " + "; ".join(failed))
        return 1
    print("tfs3_v1_test: PASS -- v1 images still mount, work, and refuse "
          "exactly the one operation their journal can't hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
