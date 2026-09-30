#!/usr/bin/env python3
"""The panic store: a kernel panic survives the warm reset that follows it.

kernel/panic_store.h keeps the kernel log in a fixed range of RAM when the
kernel panics; panic_finish() then counts down and resets; the next boot
checks the record and logd appends it to the DEAD boot's log file. Only a
warm reset carries RAM across, which is why this needs a whole guest
rather than a KTEST (the record format has KTESTs of its own).

WHAT IT ASSERTS, in one guest that is allowed to reboot (`vm.py --reboot`):
  - a COLD boot recovers nothing -- RAM that never held a record fails
    the check rather than being read as one;
  - `config set kernel.crash gp-fault` panics the machine, and the
    countdown restarts it after about panic='s 10 s rather than at once;
  - the next boot says the previous one panicked, logd appends the
    kernel's last lines -- the deliberate-crash announcement and the
    #GP report -- to the end of THAT boot's file, and clears it;
  - a further warm reboot recovers nothing: a cleared record stays cleared.

THE POSITIVE CONTROL (--positive-control) replaces the warm reset with a
cold one -- QEMU is stopped and started again after the panic -- so the
RAM is gone and every recovery check must go red. A run where they stay
green is measuring something other than the store.

The image is a COPY with `faultinject` added to its GRUB line: kernel
faults are disarmed on an ordinary boot, on purpose (kernel/core/crashtest.c).

ON DEMAND, not in the gate: two or three boots and a 10 s countdown, and a
suite that reboots its own VMs is a suite whose failures are hard to read.

    python3 tools/panic_store_test.py [--instance 3] [--positive-control]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import install_grub  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

VM = [sys.executable, os.path.join(TOOLS, "vm.py")]
BOOT_MARK = "\nidt: syscall gate"        # a boot's raw line; dmesg's echo has a [stamp]
PANIC_MARK = "panic: restarting in"


Result = Results


def vm(inst, disk, log, *args, timeout=240):
    cmd = VM + ["--instance", str(inst), "--disk", disk, "--serial-log", log] + list(args)
    try:
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ""
    return (r.stdout or "") + (r.stderr or "")


def serial(log):
    try:
        with open(log, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_serial(log, pred, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        if pred(serial(log)):
            return True
        time.sleep(0.25)
    return False


def wait_for_guest(g, deadline_s=180):
    end = time.time() + deadline_s
    while time.time() < end:
        if re.search(r"up [0-9]", g("uptime")):
            return True
        time.sleep(3)
    return False


def run(inst, disk, log, res, cold):
    g = lambda *c, timeout=60: vm(inst, disk, log, "exec", *c, timeout=timeout)  # noqa: E731
    vm(inst, disk, log, "--reboot", "start", timeout=300)
    if not wait_for_guest(g):
        res.check("the guest boots", False, serial(log)[-600:])
        return
    boot1 = g("dmesg")
    res.check("a cold boot recovers nothing",
              "nothing recovered" in boot1 and "PREVIOUS BOOT PANICKED" not in boot1,
              "\n".join(l for l in boot1.splitlines() if "panic_store" in l)[-400:])

    boots_before = serial(log).count(BOOT_MARK)
    # IN THE BACKGROUND: a panicked guest never answers, so waiting on the
    # exec would start the countdown's clock a whole timeout late.
    trigger = subprocess.Popen(
        VM + ["--instance", str(inst), "exec", "config set kernel.crash gp-fault"],
        cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t_panic = None
    if wait_serial(log, lambda s: PANIC_MARK in s, 30):
        t_panic = time.time()
    try:
        trigger.wait(timeout=30)
    except subprocess.TimeoutExpired:
        trigger.kill()
    res.check("config set kernel.crash panics the machine", t_panic is not None,
              serial(log)[-600:])
    if t_panic is None:
        return

    if cold:
        # THE CONTROL: RAM does not survive this, so nothing may be recovered.
        vm(inst, disk, log, "stop")
        vm(inst, disk, log, "--reboot", "start", timeout=300)
    else:
        rebooted = wait_serial(log, lambda s: s.count(BOOT_MARK) > boots_before, 60)
        waited = time.time() - t_panic
        res.check("the countdown restarts the machine after ~10 s, not at once",
                  rebooted and 8.0 <= waited <= 40.0, f"rebooted={rebooted} after {waited:.1f}s")
    if not wait_for_guest(g):
        res.check("the guest comes back", False, serial(log)[-600:])
        return

    boot2 = g("dmesg")
    res.check("the next boot says the previous one PANICKED",
              "PREVIOUS BOOT PANICKED" in boot2,
              "\n".join(l for l in boot2.splitlines() if "panic_store" in l)[-400:])

    # logd files it within a poll or two of starting.
    filed = ""
    end = time.time() + 30
    while time.time() < end:
        filed = g("log -p 1 -n 200", timeout=60)
        if "THIS BOOT PANICKED" in filed:
            break
        time.sleep(2)
    marker = filed.find("THIS BOOT PANICKED")
    after = filed[marker:] if marker >= 0 else ""
    res.check("logd appends the record to the dead boot's log", marker >= 0, filed[-600:])
    res.check("...with the kernel's last lines: the crash it was asked for, and the fault",
              'deliberately triggering "gp-fault"' in after
              and "PANIC: General protection fault" in after,
              after[-800:])
    res.check("...and the record is cleared once filed",
              "panic_store: record cleared" in g("dmesg"))

    boots_before = serial(log).count(BOOT_MARK)
    g("reboot", timeout=10)
    wait_serial(log, lambda s: s.count(BOOT_MARK) > boots_before, 60)
    if not wait_for_guest(g):
        res.check("the guest comes back from `reboot`", False, serial(log)[-600:])
        return
    boot3 = g("dmesg")
    res.check("a cleared record is not reported again on the next warm boot",
              "nothing recovered" in boot3 and "PREVIOUS BOOT PANICKED" not in boot3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3,
                    help="VM slot; this tool PANICS and REBOOTS it")
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"),
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--positive-control", action="store_true",
                    help="cold-restart after the panic; the recovery checks must then fail")
    ap.add_argument("--keep", help="keep the serial log at this path")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="panic_store_test.")
    disk = os.path.join(work, "disk.img")
    log = os.path.join(work, "serial.log")
    copy_disk(args.disk, disk)
    ok, why = install_grub.add_boot_word(disk, "faultinject")
    if not ok:
        print(f"panic_store_test: cannot arm the copy: {why}")
        return 2

    res = Result()
    try:
        run(args.instance, disk, log, res, args.positive_control)
    finally:
        vm(args.instance, disk, log, "stop")
        if args.keep:
            shutil.copy(log, args.keep)
        shutil.rmtree(work, ignore_errors=True)

    if args.positive_control:
        if res.fails:
            print(f"\npanic_store_test: positive control FAILED as it must "
                  f"({len(res.fails)} finding(s))")
            return 0
        print("\npanic_store_test: positive control PASSED -- a cold restart still "
              "reported a recovered panic, so this is not checking the store",
              file=sys.stderr)
        return 1

    print(f"\npanic_store_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
