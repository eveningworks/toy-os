#!/usr/bin/env python3
"""tools/nvme_test.py -- the root filesystem on an NVMe SSD, and a 4K namespace beside it.

WHAT THIS COVERS
----------------
kernel/drivers/nvme.c, which nothing else here reaches: every other tool
attaches IDE, AHCI or virtio. One QEMU NVMe controller carries TWO
namespaces, and there is no other disk on the machine:

  * namespace 1 -- a copy of disk.img, 512-byte blocks. SeaBIOS boots
    GRUB off it and the kernel mounts its root from it, so if the driver
    is broken there is no root at all.
  * namespace 2 -- blank, 4096-byte blocks. `mkpart`, `mkfs`, a mount,
    a copy, and TRIM, all on a 4K-sector namespace.

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. The controller is claimed with BOTH namespaces, on MSI-X, and the
     root mounts from nvme0p3 -- a kernel whose driver declined the
     controller still boots the ISO into ramfs, so "a prompt appeared"
     proves nothing; the DEVICE the root is on is the assertion.
  2. `ktest nvme` passes with NOTHING SKIPPED. Each nvme KTEST skips when
     no controller is on the bus; a skip here means the suite was green
     because nothing ran.
  3. A file written to each namespace reads back with the same `sum`
     after a REBOOT, from the disk rather than any cache.
  4. TRIM, from the HOST: 40 MiB written to the 4K namespace grows its
     sparse image, and deleting it shrinks it again. The GROWTH is
     asserted too -- without it, the shrink passes vacuously.
  5. No command timed out, no controller failure, and no misaligned
     transfer was logged.

    python3 tools/nvme_test.py
    echo $?
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qmp_test import guarded_boot_args  # noqa: E402
from multidisk_test import Result, connect, kill  # noqa: E402

NS2_MB = 1024
PAYLOAD = "/bin/ls"


def launch(ns1, ns2, tmp, tag):
    sock = os.path.join(tmp, f"nvme_{tag}.serial")
    pidfile = os.path.join(tmp, f"nvme_{tag}.pid")
    for f in (sock, pidfile):
        if os.path.exists(f):
            os.remove(f)
    cmd = (f"qemu-system-x86_64 {' '.join(guarded_boot_args(ns1))}"
           f" -device nvme,id=nvm,serial=toyos-nvme-test"
           f" -drive file={ns1},format=raw,if=none,id=ns1,discard=unmap"
           f" -device nvme-ns,drive=ns1,bus=nvm,nsid=1"
           f" -drive file={ns2},format=raw,if=none,id=ns2,discard=unmap"
           f" -device nvme-ns,drive=ns2,bus=nvm,nsid=2,"
           f"logical_block_size=4096,physical_block_size=4096"
           f" -m 512 -display none -no-reboot"
           f" -serial unix:{sock},server,nowait"
           f" -daemonize -pidfile {pidfile}")
    subprocess.run(cmd, shell=True, check=True)
    return sock, pidfile


def sum_of(sh, path):
    out = sh.run(f"sum {path}")
    for line in out.splitlines():
        parts = line.split()
        if parts and path in line and parts[0].isalnum() and parts[0] != "sum":
            return parts[0]
    return None


def mounted(sh, dev, point):
    out = sh.run("mount")
    return any(dev in ln and point in ln for ln in out.splitlines()), out


def allocated(path):
    return os.stat(path).st_blocks * 512


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"))
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = ap.parse_args()

    res = Result()
    tmp = tempfile.mkdtemp(prefix="nvme_")
    ns1 = os.path.join(tmp, "ns1.img")
    ns2 = os.path.join(tmp, "ns2.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, ns1], check=True)
    subprocess.run(["truncate", "-s", f"{NS2_MB}M", ns2], check=True)

    sums = {}
    # --- boot 1 ---
    sock, pidfile = launch(ns1, ns2, tmp, "a")
    sh = None
    try:
        sh = connect(sock)
        if sh is None:
            res.check("the machine booted off the NVMe namespace", False, "no serial console")
            return 1
        log = sh.run("dmesg")
        line = next((ln for ln in log.splitlines() if 'nvme: "' in ln), "")
        res.check("the controller is claimed with both namespaces",
                  "2 namespaces" in line, line or "no nvme controller line")
        res.check("completions are interrupt-driven (MSI-X)", "MSI-X" in line, line)
        res.check("the root mounted from the NVMe namespace",
                  "mounted at / on nvme0p3" in log,
                  " | ".join(ln for ln in log.splitlines() if "mounted at /" in ln)[:300])
        blk = sh.run("lsblk")
        row = next((ln for ln in blk.splitlines() if ln.startswith("nvme1 ")), "")
        res.check("namespace 2 is nvme1, with 4096-byte sectors", "4096" in row.split(), blk[:400])

        kt = sh.verb("ktest nvme", timeout=60)
        res.check("the nvme KTESTs pass", "PASSED" in kt and "0 failed" in kt, kt[-400:])
        res.check("no nvme KTEST skipped (the driver really ran)", " 0 skipped" in kt, kt[-200:])
        # Counted from the SOURCE, so a test added there and not run
        # here -- a suite filter gone stale -- is a failure, not a pass.
        with open(os.path.join(ROOT, "kernel", "drivers", "nvme_test.c")) as f:
            want = f.read().count('KTEST("nvme"')
        res.check(f"every nvme KTEST ran ({want})", f"{want} run" in kt, kt[-200:])

        sh.run(f"cp {PAYLOAD} /nvme_probe")
        sums["orig"] = sum_of(sh, PAYLOAD)
        out = sh.run("mkpart --disk nvme1 rest confirm")
        res.check("mkpart on the 4K namespace", "GPT partition table written" in out, out[:300])
        out = sh.run("mkfs nvme1p1 confirm")
        res.check("mkfs on the 4K namespace", "rror" not in out and "refused" not in out, out[:300])
        sh.run("mkdir /n4")
        sh.run("mount nvme1p1 /n4")
        on4, mo = mounted(sh, "nvme1p1", "/n4")
        res.check("it mounts", on4, mo[:300])
        sh.run(f"cp {PAYLOAD} /n4/ls")
        s_root, s_4k = sum_of(sh, "/nvme_probe"), sum_of(sh, "/n4/ls")
        res.check("both copies read back in the same boot",
                  on4 and sums["orig"] and s_root == sums["orig"] and s_4k == sums["orig"],
                  f"orig={sums['orig']} root={s_root} 4k={s_4k}")
        sh.run("umount /n4")
        sh.run("sync")
    finally:
        if sh:
            sh.close()
        kill(pidfile)
    time.sleep(1)

    # --- boot 2: reboot, read back, then TRIM ---
    base = allocated(ns2)
    sock, pidfile = launch(ns1, ns2, tmp, "b")
    sh = None
    grown = reclaimed = None
    try:
        sh = connect(sock)
        if sh is None:
            res.check("the machine booted a second time", False, "no serial console")
            return 1
        sh.run("mkdir /n4")
        sh.run("mount nvme1p1 /n4")
        on4, _ = mounted(sh, "nvme1p1", "/n4")
        s_root, s_4k = sum_of(sh, "/nvme_probe"), sum_of(sh, "/n4/ls")
        res.check("after a reboot both files still match",
                  on4 and sums.get("orig") and s_root == sums["orig"] and s_4k == sums["orig"],
                  f"orig={sums.get('orig')} root={s_root} 4k={s_4k}")

        sh.run("mkdir /n4/trim")
        sh.verb("sh mkfiles /n4/trim 40 1000000", timeout=120)
        sh.run("sync")
        grown = allocated(ns2)
        sh.verb("sh rm -r /n4/trim", timeout=60)
        sh.run("sync")
        reclaimed = allocated(ns2)
        sh.run("umount /n4")
        log = sh.run("dmesg")
        bad = [ln for ln in log.splitlines()
               if "nvme:" in ln and ("timed out" in ln or "disabled" in ln or "failed" in ln)
               or "not whole" in ln]
        res.check("no NVMe command failed and no transfer was misaligned", not bad,
                  " | ".join(bad)[:300])
    finally:
        if sh:
            sh.close()
        kill(pidfile)

    if grown is not None:
        res.check("writing 40 MiB grows the 4K namespace's image",
                  grown - base > 30 * 1024 * 1024, f"grew by {(grown - base) >> 20} MiB")
        res.check("deleting it gives the space back (DSM deallocate reached the device)",
                  grown - reclaimed > 30 * 1024 * 1024,
                  f"shrank by {(grown - reclaimed) >> 20} MiB")

    if not args.keep:
        subprocess.run(["rm", "-rf", tmp])
    print(f"nvme_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
