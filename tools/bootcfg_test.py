#!/usr/bin/env python3
"""`bootcfg` against a real /boot: edits land, refusals change nothing,
and a trial entry is booted ONCE.

lib/ubootcfg.h's model is covered on fixtures by /tests/bootcfg_test;
what only a booted guest can show is the rest:

  1. an edit reaches the installed grub.cfg, the old file becomes
     grub.cfg.bak, no .new is left, and /boot is read-only again after;
  2. a misspelt word is refused WITH a suggestion and the file is
     byte-identical afterwards; --force writes it and `undo` takes it out;
  3. `bootcfg try 0 +target=text`, then a reboot, comes up in the TEXT
     target (no toywm) with the trial's mark on QUERY_CMDLINE and
     `bootcfg` calling it booted -- the reboot after that is the default
     again (toywm up), because GRUB cleared the choice;
  4. `try --drop` removes the trial.

Runs against a COPY of disk.img and reboots it twice.

    python3 tools/bootcfg_test.py [--instance N]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from harness import copy_disk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")
CFG = "/boot/boot/grub/grub.cfg"


class Result:
    def __init__(self):
        self.fails = 0

    def check(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}"
              + (f"\n          {detail}" if detail and not ok else ""))
        self.fails += 0 if ok else 1


def vm(inst, *args, timeout=180):
    p = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=REPO,
                       capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def body(out):
    """A single command's output, without vm.py's `--- cmd ---` header."""
    return out.split("\n", 1)[1] if out.startswith("---") else out


def wait_back(inst, want, deadline=240):
    """Poll until the guest answers again and `want` is in `ps`, bounded."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        try:
            out = vm(inst, "exec", "ps", "bootcfg", timeout=20)
        except subprocess.TimeoutExpired:
            continue
        if want in out:
            return out
        time.sleep(2)
    return ""


def reboot(inst):
    try:
        vm(inst, "exec", "reboot", timeout=30)
    except subprocess.TimeoutExpired:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    a = ap.parse_args()
    inst = a.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="bootcfg_test_")
    disk = os.path.join(tmp, "disk.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    try:
        print("bootcfg_test: booting a guest that will reboot")
        out = vm(inst, "--disk", disk, "--reboot", "start", timeout=300)
        if "ready" not in out:
            print(f"bootcfg_test: guest did not boot\n{out}")
            return 1

        before = body(vm(inst, "exec", f"cat {CFG}"))
        out = vm(inst, "exec", "bootcfg words 0 +nokaslr")
        after = body(vm(inst, "exec", f"cat {CFG}"))
        bak = body(vm(inst, "exec", f"cat {CFG}.bak"))
        r.check("an edit is saved and shown as a diff", "saved" in out and "+ " in out, out[-400:])
        r.check("the boot line carries the word now",
                "multiboot2 /boot/kernel.bin debugcon nokaslr" in after, after[-600:])
        r.check("the old file is the .bak, byte for byte", bak == before)
        r.check("exactly one line changed",
                len(set(after.splitlines()) ^ set(before.splitlines())) == 2)
        new = vm(inst, "exec", f"ls {CFG}.new")
        r.check("no .new is left behind", "no such file" in new, new[-200:])
        mounts = vm(inst, "exec", "mount")
        r.check("/boot is read-only again", any(ln.split()[1:2] == ["/boot"] and ln.split()[-1].split(",")[0] == "ro"
                                                for ln in mounts.splitlines()), mounts)

        out = vm(inst, "exec", "bootcfg words 0 +vidoe=1280x720")
        same = body(vm(inst, "exec", f"cat {CFG}"))
        r.check("a misspelt word is refused with a suggestion",
                'did you mean "video="' in out and "--force" in out, out[-400:])
        r.check("and the file is unchanged", same == after)
        out = vm(inst, "exec", "bootcfg words 0 +frobnicate --force")
        r.check("--force writes it", "frobnicate" in body(vm(inst, "exec", f"cat {CFG}")), out[-300:])
        out = vm(inst, "exec", "bootcfg undo")
        r.check("undo takes it out again", body(vm(inst, "exec", f"cat {CFG}")) == after, out[-300:])

        out = vm(inst, "exec", "bootcfg try 0 +target=text")
        r.check("a trial is saved and chosen for the next boot",
                "next boot only" in out and "(trial)" in out, out[-400:])
        reboot(inst)
        out = wait_back(inst, "booted")
        r.check("the machine came back into the trial", bool(out), out[-400:])
        r.check("the trial's words took effect (target=text: no toywm)",
                bool(out) and "toywm" not in out, out[-400:])
        r.check("its mark is on the running command line, and bootcfg says booted",
                "bootcfg.trial" in out and "This boot came from the trial" in out, out[-500:])

        reboot(inst)
        out = wait_back(inst, "toywm")
        r.check("the reboot after is the default again (toywm up)", "toywm" in out, out[-400:])
        r.check("and the trial is waiting, not booted",
                "A trial entry is waiting" in out, out[-400:])
        out = vm(inst, "exec", "bootcfg try --drop", "bootcfg")
        r.check("--drop removes the trial", "saved" in out and "(trial)" not in out.split("--- bootcfg ---")[-1],
                out[-500:])
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"bootcfg_test: {'PASS' if not r.fails else f'{r.fails} FAILED'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
