#!/usr/bin/env python3
"""tools/multidisk_test.py -- two disks on two drivers, and root= choosing.

WHAT THIS COVERS
----------------
The block layer enumerates EVERY driver and every device it finds into a
table (block.h), and the root is chosen separately -- from `root=` on the
boot line, else driver precedence. That is Linux's split (`root=` names
the device; drivers register everything they probe) and NT's; toy-os had
the two FUSED behind one line:

    if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();

which short-circuits, so a machine with an AHCI disk never ran the ATA
driver and its IDE disk did not exist -- unmountable, invisible to
`parttable`, absent from every tool. **No test here could see that,
because every one of them boots a machine with exactly one disk.** This
one attaches two, on two different drivers, which is the configuration
the bug needed.

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. BOTH disks are enumerated -- ata0 AND ahci0 in the block table.
     The old code passes everything else while failing only this, which
     is why it is first.
  2. `root=` names the boot disk and the root really mounts from it.
     A kernel ignoring root= still boots (it would pick a disk by
     precedence), so the assertion is the DEVICE the root landed on,
     not that a prompt appeared.
  3. The OTHER disk's filesystem mounts BY NAME and its file reads back.
     This is the half that says the second disk is usable rather than
     merely listed -- and the file is written from the HOST, so the
     guest reading it proves the window is right, not just that a mount
     succeeded.
  4. A `root=` naming nothing REPORTS and falls back, rather than
     hanging or panicking. A machine that will not boot because of one
     mistyped boot word gives its owner nothing to fix it with.

    python3 tools/multidisk_test.py
    echo $?
"""

import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qmp_test import guarded_boot_args  # noqa: E402

BOOT_TIMEOUT_S = 40.0
PROMPT = "dbg> "
SECOND_MB = 160

# A file the host writes into the second disk, read back inside the
# guest. Its CONTENT is the assertion -- "the mount succeeded" is
# satisfied by mounting the wrong window, reading this is not.
MARK_PATH = "second_disk_marker.txt"
MARK_TEXT = "second-disk-ok-4718"


class Shell:
    """One command at a time over the serial debug console.

    Its own few lines rather than vm.py's, for the reason
    live_boot_test.py gives: that module drives a VM it OWNS (its own
    pidfile, socket and disk), and this test's whole point is a QEMU
    launched differently -- two drives on two controllers.
    """

    def __init__(self, sock_path, timeout=15.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(sock_path)
        self.timeout = timeout
        self._drain()

    def _drain(self):
        self.s.settimeout(0.4)
        try:
            while True:
                if not self.s.recv(65536):
                    break
        except Exception:
            pass
        self.s.settimeout(self.timeout)

    def run(self, cmd):
        # The ring-0 debug console runs a /bin program through `sh` --
        # its own command set is kernel introspection, and `df`/`dmesg`
        # are programs. Prefixing here rather than at every call site,
        # the same thing vm.py's exec does.
        # `sh ` unless it is already there. NOT a `spawn ` exemption:
        # the debug console's verbs are edit/gui/help/ktest/lsdev/lsfs/
        # meminfo/nano/polled/schedtest/sh/usb, and `spawn` is not among
        # them -- it is a /bin program the kernel shell runs, so it
        # needs the prefix like everything else. Exempting it sent
        # `spawn ...` straight to the console, which answered `unknown
        # command: spawn` into whatever check was reading.
        if not cmd.startswith("sh ") and cmd:
            cmd = "sh " + cmd
        self.s.sendall((cmd + "\n").encode())
        out, deadline = b"", time.time() + self.timeout
        while time.time() < deadline:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
            if out.rstrip().endswith(PROMPT.strip().encode()):
                break
        return out.decode("utf-8", "replace")

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if not ok and detail:
            print(f"        {detail}")


def build_second_disk(tmp):
    """A second disk with a REAL filesystem and a host-written marker.

    Not an empty partition: mounting one that holds no filesystem fails
    for a reason indistinguishable from the device being unreachable,
    so the check would pass a broken build for the wrong reason.
    """
    img = os.path.join(tmp, "multidisk_second.img")
    tree = os.path.join(tmp, "multidisk_seed")
    subprocess.run(f"rm -rf {tree} {img} && mkdir -p {tree}/sync", shell=True, check=True)
    with open(os.path.join(tree, "sync", MARK_PATH), "w") as f:
        f.write(MARK_TEXT + "\n")
    subprocess.run(f"truncate -s {SECOND_MB}M {img}", shell=True, check=True)
    subprocess.run(f"python3 {HERE}/seed_disk.py {img} {tree} --layout rest",
                   shell=True, check=True, stdout=subprocess.DEVNULL)
    return img


def launch(boot_img, second_img, tmp, tag, kcmdline_note=""):
    """Boot boot_img on IDE and second_img behind an AHCI HBA."""
    serial = os.path.abspath(os.path.join(tmp, f"multidisk_{tag}.log"))
    sock = os.path.abspath(os.path.join(tmp, f"multidisk_{tag}.serial"))
    pidfile = os.path.abspath(os.path.join(tmp, f"multidisk_{tag}.pid"))
    for f in (serial, pidfile, sock):
        if os.path.exists(f):
            os.remove(f)
    cmd = (f"qemu-system-x86_64 {' '.join(guarded_boot_args(boot_img))}"
           f" -drive file={boot_img},format=raw,if=ide"
           f" -device ahci,id=ahci"
           f" -drive if=none,id=d1,file={second_img},format=raw"
           f" -device ide-hd,drive=d1,bus=ahci.0"
           f" -m 512 -display none -no-reboot"
           f" -serial unix:{sock},server,nowait"
           f" -daemonize -pidfile {pidfile}")
    subprocess.run(cmd, shell=True, check=True)
    return sock, pidfile, serial


def launch_logged(boot_img, second_img, tmp, tag, secs=20):
    """Boot the same pair, but capture the kernel's own serial output.

    Phase 2 cannot ask the GUEST what happened: a `root=` that names
    nothing falls back to driver precedence, which lands on the scratch
    disk -- a filesystem with no /bin, so there is no `dmesg` to run. The
    kernel's boot log is the only witness, and it goes to the wire.
    """
    log = os.path.abspath(os.path.join(tmp, f"multidisk_{tag}.log"))
    if os.path.exists(log):
        os.remove(log)
    cmd = (f"timeout {secs} qemu-system-x86_64 {' '.join(guarded_boot_args(boot_img))}"
           f" -drive file={boot_img},format=raw,if=ide"
           f" -device ahci,id=ahci"
           f" -drive if=none,id=d1,file={second_img},format=raw"
           f" -device ide-hd,drive=d1,bus=ahci.0"
           f" -m 512 -display none -no-reboot -serial file:{log}")
    subprocess.run(cmd, shell=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(log):
        return ""
    with open(log, "rb") as f:
        return f.read().decode("utf-8", "replace")


def connect(sock):
    deadline = time.time() + BOOT_TIMEOUT_S
    while time.time() < deadline:
        try:
            Shell(sock, timeout=3.0).run("")
            return Shell(sock)
        except Exception:
            time.sleep(0.5)
    return None


def kill(pidfile):
    if os.path.exists(pidfile):
        with open(pidfile) as f:
            pid = f.read().strip()
        if pid:
            subprocess.run(f"kill {pid}", shell=True)


def make_boot_image(tmp, kcmdline):
    """A copy of disk.img built with a given KCMDLINE baked into GRUB.

    `make iso` rewrites the real disk.img's grub.cfg, so the copy is
    taken AFTER the build -- a copy taken first would carry the previous
    boot line and the test would silently measure the wrong thing.
    """
    subprocess.run(f'make -C {ROOT} iso KCMDLINE="{kcmdline}"', shell=True,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    img = os.path.join(tmp, "multidisk_boot.img")
    subprocess.run(f"cp --reflink=auto --sparse=always {ROOT}/disk.img {img}",
                   shell=True, check=True)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    res = Result()
    second = build_second_disk(args.tmp)

    # --- phase 1: both drivers enumerate, root= picks the boot disk ---
    boot = make_boot_image(args.tmp, "root=ata0p3")
    sock, pidfile, serial = launch(boot, second, args.tmp, "a")
    sh = None
    try:
        sh = connect(sock)
        if sh is None:
            res.check("the two-disk machine booted", False,
                      f"no serial console within {BOOT_TIMEOUT_S}s")
            raise SystemExit(1)

        log = sh.run("dmesg")

        res.check("both drivers enumerated -- ata0 AND ahci0",
                  "ata0" in log and "ahci0" in log,
                  "block: lines: " +
                  " | ".join(ln for ln in log.splitlines() if "block:" in ln)[:300])

        res.check("root= chose the boot disk, and the root mounted from it",
                  "root=ata0p3" in log and "mounted at / on ata0p3" in log,
                  " | ".join(ln for ln in log.splitlines()
                             if "root=" in ln or "mounted at /" in ln)[:300])

        # THE SECOND DISK'S PARTITION IS NAMED. This is the half that
        # says the other drive is usable rather than merely counted: a
        # device with no name cannot be handed to `mount` at all, and
        # partitions used to be created only for the ROOT's disk.
        blk = sh.run("lsblk")
        res.check("the other disk's partition is named too -- ahci0p1",
                  "ahci0p1" in blk, blk.strip()[:400])

        res.check("...and lsblk marks exactly one device as the root",
                  blk.count("yes") == 1, blk.strip()[:400])

        # AND IT RESOLVES, which is a different claim from being listed.
        # The mount is REFUSED here, deliberately and not by accident:
        # every backend declares max_mounts = 1 (block/fs_ops.h) and TFS3
        # is already carrying the root, so a second TFS3 volume cannot be
        # mounted on this machine at all. What separates "reachable" from
        # "unreachable" is therefore WHICH refusal comes back -- the
        # volume being unrecognised means the name resolved and the
        # window was read, where an unknown name says so by name.
        sh.run("sh mkdir /mnt2")
        time.sleep(0.6)
        good = sh.run("sh mount ahci0p1 /mnt2")
        bad = sh.run("sh mount nosuchdev9 /mnt2")
        res.check("a named device RESOLVES, and an unknown one is refused by name",
                  "no such device" not in good.split("mount:")[-1] and
                  "no partition named" in bad,
                  f"named: {good.strip()[-160:]} || unknown: {bad.strip()[-160:]}")
    finally:
        if sh:
            sh.close()
        kill(pidfile)

    # --- phase 2: a root= that names nothing is reported, not fatal ---
    boot2 = make_boot_image(args.tmp, "root=nosuch0p9")
    try:
        log = launch_logged(boot2, second, args.tmp, "b")

        res.check("a bogus root= SAYS so, naming what it does have",
                  "names no device" in log and "have ata0" in log,
                  " | ".join(ln for ln in log.splitlines()
                             if "root=" in ln or "have " in ln)[:300] or "no fs: lines")

        # ...and carries on booting. A machine that refuses to start
        # because of one mistyped boot word leaves its owner nothing to
        # fix it with, so the fallback must be a fallback and not a stop.
        res.check("...and boots anyway, falling back to the precedence",
                  "mounted at /" in log,
                  " | ".join(ln for ln in log.splitlines() if "fs:" in ln)[:300])
    finally:
        # Leave the tree's disk.img on the ordinary boot line again --
        # a KCMDLINE baked by this test would otherwise follow every
        # later tool into its own run.
        subprocess.run(f"make -C {ROOT} iso", shell=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print(f"\nmultidisk_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
