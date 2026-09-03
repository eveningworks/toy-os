#!/usr/bin/env python3
"""tools/fs_switch_test.py -- prove live filesystem switching end-to-end.

Boots a COPY of disk.img and walks the format/remount/reboot story:

  1. the build's disk.img mounts as what its magic says (tfs3)
  2. `fsformat tfs3 confirm` reformats and remounts a LIVE disk; the
     remount leaves /etc and /tmp behind so a setting written right
     afterwards actually persists, and writes work
  3. a REBOOT re-probes, picks tfs3, and the file survived
  4. a second reformat + write + `fsck` is clean
  5. a final reboot re-probes and that file survived too

THE NAME IS AHEAD OF THE TOOL, DELIBERATELY. This used to switch
between TFS3 and TFS2 and back, which is where every check below comes
from; TFS2 was removed and there is one backend to switch between at
the moment. What survives is everything that was never really about
having two formats -- the wipefs rule, the live reformat, the remount's
mkdirs, and reboot persistence -- and those are the checks that caught
real bugs. FAT32 (docs/roadmap.md) makes the switching half real again,
at which point `other` comes back and this tool goes back to its
original shape rather than being written a second time.

Exits nonzero on the first assertion that fails. Run it after touching
anything in kernel/fs/ -- it is the one test that exercises probe
ordering, the wipefs rule, format, remount and reboot persistence
together, which no KTEST can (the suite runs inside one booted
kernel).

Usage: python3 tools/fs_switch_test.py   (builds its own disk copy;
       needs toy-os.iso + disk.img, i.e. run after `make iso`)
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")

FAILURES = []


def vm(*args):
    r = subprocess.run([sys.executable, VM, *args], capture_output=True, text=True)
    return r.stdout + r.stderr


def vm_exec(*cmds):
    return vm("exec", *cmds)


def check(label, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        FAILURES.append(label)


def active_fs():
    # `rescue df`, NOT `df`. THE RING-0 COPY IS THE ONLY ONE THIS TEST
    # CAN USE, for two independent reasons, and both bite:
    #
    #   1. A freshly formatted image has NO /bin, so every /bin program
    #      is gone for the rest of the run -- which is most of this
    #      test.
    #   2. `df` moved to /bin and its output became a TABLE; only the
    #      ring-0 copy still prints "Filesystem:". This tool kept
    #      parsing for that and silently failed ten checks against a
    #      perfectly healthy system, for however long -- it is on
    #      demand, so nothing ran it.
    #
    # Same rule for every other command below: builtins and `rescue`
    # only. A harness that reports a working OS as broken is worse than
    # no harness.
    out = vm_exec("rescue df")
    for line in out.splitlines():
        if "Filesystem:" in line:
            return line.split("Filesystem:")[1].split("(")[0].strip()
    return "?"


def main():
    disk = os.path.join(REPO, "disk.img")
    if not os.path.exists(disk):
        sys.exit("fs_switch_test: no disk.img -- run `make iso` first")
    tmp = tempfile.mkdtemp(prefix="fsswitch.")
    copy = os.path.join(tmp, "switch.img")
    # --reflink keeps the copy cheap on Btrfs/XFS; --sparse keeps a 9 GB
    # sparse image from filling its holes elsewhere (CLAUDE.md).
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", disk, copy], check=True)

    try:
        print("fs_switch_test: boot 1 (build image)")
        vm("--disk", copy, "start")
        first = active_fs()
        check(f"build image mounts as its magic says ({first})", first == "tfs3")

        print("fs_switch_test: live reformat")
        out = vm_exec("fsformat tfs3 confirm")
        check("fsformat reports success", "active filesystem is now tfs3" in out, out[-200:])
        check("df agrees", active_fs() == "tfs3")
        # The layout a mount is supposed to leave behind. `fsformat`
        # reformats and remounts a LIVE disk, and that path used to skip
        # kernel_main()'s mkdirs entirely -- so /etc and /tmp were gone
        # until the next reboot and the next `timezone`/`fontsize`
        # silently persisted nothing while reporting success.
        #
        # `rescue stat`, not `stat`. The comment here used to say "stat,
        # a builtin" -- true when it was written, false once `stat`
        # became a /bin program, and nothing noticed because a freshly
        # formatted image has no /bin and the check simply went red in a
        # tool nobody runs. See active_fs() above.
        out = vm_exec("rescue stat /etc", "rescue stat /tmp")
        check("live reformat leaves /etc and /tmp behind",
              out.count("type:     directory") >= 2, out[-300:])
        # The assertion that matters more than the directories existing:
        # a setting written after the reformat actually lands on disk.
        # A missing /etc failed this while the command still said "set".
        out = vm_exec("timezone Helsinki", "rescue cat /etc/toyos.conf")
        check("a setting persists after a live reformat",
              "timezone=helsinki" in out and "NOT saved" not in out, out[-300:])

        vm_exec("write /switch.txt made-after-reformat")
        out = vm_exec("rescue cat /switch.txt")
        check("write+read works after the reformat", "made-after-reformat" in out, out[-200:])

        print("fs_switch_test: reboot 1")
        vm("stop")
        vm("--disk", copy, "start")
        check("reboot re-probes tfs3", active_fs() == "tfs3")
        out = vm_exec("rescue cat /switch.txt")
        check("file survived the reboot", "made-after-reformat" in out, out[-200:])

        print("fs_switch_test: second reformat")
        out = vm_exec("fsformat tfs3 confirm")
        check("the second reformat reports success",
              "active filesystem is now tfs3" in out, out[-200:])
        # The file from before MUST be gone -- a reformat that left it
        # would mean the format did nothing, and every check above would
        # pass on a filesystem that was never rewritten.
        out = vm_exec("rescue cat /switch.txt")
        check("...and it really reformatted (the old file is gone)",
              "made-after-reformat" not in out, out[-200:])
        vm_exec("write /back.txt returned")
        out = vm_exec("rescue cat /back.txt", "fsck")
        check("write works after the second reformat", "returned" in out, out[-300:])
        check("fsck is clean after the round trip", "fsck: clean." in out, out[-300:])

        print("fs_switch_test: reboot 2")
        vm("stop")
        vm("--disk", copy, "start")
        check("final reboot re-probes tfs3", active_fs() == "tfs3")
        out = vm_exec("rescue cat /back.txt")
        check("file survived the final reboot", "returned" in out, out[-200:])
    finally:
        vm("stop")
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print(f"\nfs_switch_test: FAILED -- {len(FAILURES)} check(s): {FAILURES}")
        return 1
    print("\nfs_switch_test: PASS -- probe, wipefs, format, remount and reboot persistence all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
