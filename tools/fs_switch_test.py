#!/usr/bin/env python3
"""tools/fs_switch_test.py -- prove live filesystem switching end-to-end.

Boots a COPY of disk.img and walks the whole multi-backend story:

  1. the build's disk.img mounts as what its magic says (tfs3 today)
  2. `fsformat tfs2 confirm` live-switches to tfs2; writes work there,
     and the remount leaves /etc and /tmp behind so a setting written
     right afterwards actually persists
  3. a REBOOT re-probes and still picks tfs2, and the file survived
  4. `fsformat tfs3 confirm` switches back; writes work; fsck is clean
  5. a final reboot re-probes tfs3 and the file survived

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
    out = vm_exec("df")
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
    # --reflink keeps the copy cheap on Btrfs/XFS; plain copy elsewhere.
    subprocess.run(["cp", "--reflink=auto", disk, copy], check=True)

    try:
        print("fs_switch_test: boot 1 (build image)")
        vm("--disk", copy, "start")
        first = active_fs()
        check(f"build image mounts as its magic says ({first})", first in ("tfs2", "tfs3"))
        other = "tfs2" if first == "tfs3" else "tfs3"

        print(f"fs_switch_test: live-switch to {other}")
        out = vm_exec(f"fsformat {other} confirm")
        check(f"fsformat {other} reports success", "active filesystem is now " + other in out, out[-200:])
        check(f"df agrees ({other})", active_fs() == other)
        # The layout a mount is supposed to leave behind. `fsformat`
        # reformats and remounts a LIVE disk, and that path used to skip
        # kernel_main()'s mkdirs entirely -- so /etc and /tmp were gone
        # until the next reboot and the next `timezone`/`fontsize`
        # silently persisted nothing while reporting success. Asserted
        # with `stat`, a builtin: a freshly formatted image has no /bin,
        # so `ls` is not available to a test at this point.
        out = vm_exec("stat /etc", "stat /tmp")
        check("live reformat leaves /etc and /tmp behind",
              out.count("type:     directory") >= 2, out[-300:])
        # The assertion that matters more than the directories existing:
        # a setting written after the reformat actually lands on disk.
        # A missing /etc failed this while the command still said "set".
        out = vm_exec("timezone Helsinki", "cat /etc/toyos.conf")
        check("a setting persists after a live reformat",
              "timezone=helsinki" in out and "NOT saved" not in out, out[-300:])

        vm_exec("write /switch.txt made-on-" + other)
        out = vm_exec("cat /switch.txt")
        check("write+read works on " + other, "made-on-" + other in out, out[-200:])

        print("fs_switch_test: reboot 1")
        vm("stop")
        vm("--disk", copy, "start")
        check(f"reboot re-probes {other}", active_fs() == other)
        out = vm_exec("cat /switch.txt")
        check("file survived the reboot", "made-on-" + other in out, out[-200:])

        print(f"fs_switch_test: live-switch back to {first}")
        out = vm_exec(f"fsformat {first} confirm")
        check(f"fsformat {first} reports success", "active filesystem is now " + first in out, out[-200:])
        vm_exec("write /back.txt returned")
        out = vm_exec("cat /back.txt", "fsck")
        check("write works after switching back", "returned" in out, out[-300:])
        check("fsck is clean after the round trip", "fsck: clean." in out, out[-300:])

        print("fs_switch_test: reboot 2")
        vm("stop")
        vm("--disk", copy, "start")
        check(f"final reboot re-probes {first}", active_fs() == first)
        out = vm_exec("cat /back.txt")
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
