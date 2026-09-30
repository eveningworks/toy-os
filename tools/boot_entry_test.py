#!/usr/bin/env python3
"""A one-shot GRUB entry is taken ONCE, and GRUB is what ends it.

`reboot --entry` (userland/lib/ubootmenu.h) writes next_entry into GRUB's
environment block; grub.cfg's stanza makes it the default and saves it
EMPTY before booting. Nothing in a KTEST or /tests program can see GRUB,
so this boots a guest, reboots it twice, and asks what came up:

  1. a test entry is added whose command line says `target=text`, so the
     boot it takes is visible from inside (no toywm);
  2. loadenv.mod is DELETED from /boot's module directory first, so the
     pass depends on the CORE IMAGE carrying loadenv -- which is all an
     installed laptop has. Without that, the module directory would
     satisfy GRUB and this would pass for the wrong reason;
  3. `reboot --entry` -> the text boot, with next_entry already empty;
  4. a plain `reboot` -> the default again, graphical.

And the two refusals: an entry that does not exist, and a machine whose
recorded core image has no loadenv (/etc/grub-core.modules).

Runs against a COPY of disk.img.

    python3 tools/boot_entry_test.py [--instance N]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")

TEST_TITLE = "toy-os (text test)"
TEST_ENTRY = ('menuentry "%s" {\n    multiboot2 /boot/kernel.bin debugcon target=text\n'
              '    boot\n}\n' % TEST_TITLE)


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


def wait_back(inst, want, deadline=240):
    """Poll until the guest answers `ps` again, bounded. The debug console
    comes back with the machine; a reboot is the one thing that closes it."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline:
        try:
            out = vm(inst, "exec", "ps", "cat /boot/boot/grub/grubenv", timeout=20)
        except subprocess.TimeoutExpired:
            continue
        if want in out:
            return out
        time.sleep(2)
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    a = ap.parse_args()
    inst = a.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="boot_entry_test_")
    disk = os.path.join(tmp, "disk.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always",
                    os.path.join(REPO, "disk.img"), disk], check=True)
    try:
        print("boot_entry_test: booting a guest that may reboot")
        out = vm(inst, "--disk", disk, "--reboot", "start", timeout=300)
        if "ready" not in out:
            print(f"boot_entry_test: guest did not boot\n{out}")
            return 1

        dev = ""
        for ln in vm(inst, "exec", "mount").splitlines():
            if ln.split()[1:2] == ["/boot"]:
                dev = ln.split()[0]
        r.check("/boot is mounted, read-only as shipped", bool(dev) and " ro" in
                vm(inst, "exec", "mount"), vm(inst, "exec", "mount"))

        cfg = vm(inst, "exec", "cat /boot/boot/grub/grub.cfg").split("\n", 1)[1]
        r.check("the shipped grub.cfg carries the one-shot stanza", "save_env next_entry" in cfg)
        local = os.path.join(tmp, "grub.cfg")
        with open(local, "w") as f:
            f.write(cfg.rstrip("\n") + "\n" + TEST_ENTRY)
        vm(inst, "put", local, "/tmp/grub.cfg")
        vm(inst, "exec", "umount /boot", f"mount {dev} /boot",
           "rm /boot/boot/grub/i386-pc/loadenv.mod",
           "cp /tmp/grub.cfg /boot/boot/grub/grub.cfg",
           "umount /boot", f"mount -r {dev} /boot")
        gone = vm(inst, "exec", "ls /boot/boot/grub/i386-pc/loadenv.mod")
        r.check("loadenv.mod is gone, so only the core image can supply it",
                "no such file" in gone, gone.strip()[-200:])

        entries = vm(inst, "exec", "reboot --entries")
        r.check("reboot --entries lists the test entry", TEST_TITLE in entries, entries)
        bad = vm(inst, "exec", 'reboot --entry "no such entry"')
        r.check("an unknown entry is refused, and the machine stays up",
                "no boot entry" in bad, bad.strip()[-200:])

        # A core recorded WITHOUT loadenv must be refused by name: GRUB
        # would ignore the choice and boot the default, silently.
        # Copied in from the host: the debug console's shell has no
        # redirection, and an `echo >` that silently wrote nothing let
        # the next line REBOOT the guest in an earlier draft.
        stamp = os.path.join(tmp, "grub-core.modules")
        with open(stamp, "w") as f:
            f.write("biosdisk part_gpt fat normal multiboot2\n")
        vm(inst, "put", stamp, "/tmp/grub-core.modules")
        vm(inst, "exec", "cp /tmp/grub-core.modules /etc/grub-core.modules")
        planted = vm(inst, "exec", "cat /etc/grub-core.modules")
        if "multiboot2" not in planted:
            r.check("the no-loadenv stamp was planted", False, planted.strip()[-200:])
            return 1
        nol = vm(inst, "exec", "reboot --entry 1")
        r.check("a core image recorded without loadenv is refused",
                "loadenv" in nol and "cannot choose" in nol, nol.strip()[-200:])
        if "next boot" in nol:
            return 1      # it rebooted: nothing after this would measure anything
        vm(inst, "exec", "rm /etc/grub-core.modules")

        try:
            vm(inst, "exec", f'reboot --entry "{TEST_TITLE}"', timeout=30)
        except subprocess.TimeoutExpired:
            pass
        out = wait_back(inst, "next_entry")
        r.check("the machine came back after the one-shot reboot", bool(out))
        r.check("it booted the chosen entry (target=text: no toywm)",
                bool(out) and "toywm" not in out, out[-400:])
        r.check("GRUB cleared the choice before booting it",
                "next_entry=\n" in out.replace("\r", ""), out[-300:])

        try:
            vm(inst, "exec", "reboot", timeout=30)
        except subprocess.TimeoutExpired:
            pass
        out = wait_back(inst, "toywm")
        r.check("the next plain reboot takes the default again (toywm up)",
                "toywm" in out, out[-400:])
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"boot_entry_test: {'PASS' if not r.fails else f'{r.fails} FAILED'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
