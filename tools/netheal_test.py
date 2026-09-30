#!/usr/bin/env python3
"""`/bin/netheal`: reboot once when a machine comes up with no network.

THIS TOOL REBOOTS THE GUEST ON PURPOSE, twice, which is why it lives in
`ondemand_sweep.py` and not in the gate: it needs `vm.py --reboot` (the
default `-no-reboot` makes a guest reboot end QEMU instead), it costs a
couple of minutes, and a suite that reboots its own VMs is a suite whose
other failures get harder to read.

WHAT IT ASSERTS, and why each one matters for something that reboots
machines by itself:

  - with NO network it reboots, and STOPS after MAX_ATTEMPTS. The
    counter in /var/lib/netheal is the whole safety property: without
    it, a machine with genuinely no NIC reboots forever.
  - it says so once when it gives up, rather than going quiet.
  - with a network it CLEARS the counter and does nothing, so a reboot
    for an unrelated reason later does not start half way through the
    budget.

    python3 tools/netheal_test.py
    python3 tools/netheal_test.py --positive-control   # must FAIL
"""

import argparse
import os
import re
import subprocess
import sys
import time
from harness import Results  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = [sys.executable, os.path.join(ROOT, "tools", "vm.py")]
WAIT_S = 8          # what the guest is told to wait for an address
MAX_ATTEMPTS = 2    # netheal.c's own ceiling


Result = Results


def vm(inst, *args, timeout=240):
    r = subprocess.run(VM + ["--instance", str(inst)] + list(args),
                       capture_output=True, text=True, timeout=timeout)
    return (r.stdout or "") + (r.stderr or "")


def guest(inst, *cmds, timeout=240):
    return vm(inst, "exec", *cmds, timeout=timeout)


def uptime_s(inst):
    out = guest(inst, "uptime")
    m = re.search(r"up ([0-9.]+) sec", out)
    if m:
        return float(m.group(1))
    m = re.search(r"up (\d+) min", out)
    return float(m.group(1)) * 60 if m else None


def wait_for_guest(inst, deadline_s=180):
    end = time.time() + deadline_s
    while time.time() < end:
        if uptime_s(inst) is not None:
            return True
        time.sleep(5)
    return False


def run(inst, res, control):
    # --- arm it, on a machine that HAS a network ----------------------
    vm(inst, "stop")
    vm(inst, "--reboot", "start", timeout=300)
    if not wait_for_guest(inst):
        res.check("the guest came up to arm the setting", False)
        return
    guest(inst, "config set system.net_recover on",
          f"config set system.net_recover_wait {WAIT_S}")
    if control:
        # POSITIVE CONTROL: with the feature OFF, nothing should reboot
        # and the counter should never appear. If this tool reports a
        # clean run in that state it is not watching anything.
        guest(inst, "config set system.net_recover off")
    vm(inst, "stop")

    # --- no network at all --------------------------------------------
    vm(inst, "--net", "none", "--reboot", "start", timeout=300)
    if not wait_for_guest(inst):
        res.check("the guest came up with no network", False)
        return

    # It needs time for MAX_ATTEMPTS reboots, each WAIT_S plus a boot.
    deadline = time.time() + 240
    counter = None
    while time.time() < deadline:
        out = guest(inst, "cat /var/lib/netheal")
        m = re.search(r"^\s*(\d+)\s*$", out, re.M)
        if m and int(m.group(1)) >= MAX_ATTEMPTS:
            counter = int(m.group(1))
            break
        time.sleep(10)

    res.check(f"with no network it reboots and the counter reaches {MAX_ATTEMPTS}",
              counter == MAX_ATTEMPTS, f"counter={counter}")

    # AND THEN IT STOPS. Uptime must GROW across samples -- a machine
    # still rebooting would keep resetting it, and that is the failure
    # this whole design exists to prevent.
    first = uptime_s(inst)
    time.sleep(45)
    second = uptime_s(inst)
    res.check("...and then it STOPS rebooting",
              first is not None and second is not None and second > first,
              f"uptime went {first} -> {second}")

    out = guest(inst, "log -n 400")
    res.check("...and says once that it gave up",
              "giving up" in out and "netheal" in out,
              "no 'giving up' line in the log")

    # --- a healthy boot clears the counter ----------------------------
    vm(inst, "stop")
    vm(inst, "--reboot", "start", timeout=300)
    if not wait_for_guest(inst):
        res.check("the guest came up with a network again", False)
        return
    cleared = None
    deadline = time.time() + 90
    while time.time() < deadline:
        out = guest(inst, "cat /var/lib/netheal")
        m = re.search(r"^\s*(\d+)\s*$", out, re.M)
        if m:
            cleared = int(m.group(1))
            if cleared == 0:
                break
        time.sleep(5)
    res.check("with an address it clears the counter", cleared == 0,
              f"counter={cleared}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3,
                    help="VM slot; this tool REBOOTS it")
    ap.add_argument("--positive-control", action="store_true",
                    help="leave the feature off; the run must then fail")
    args = ap.parse_args()

    res = Result()
    try:
        run(args.instance, res, args.positive_control)
    finally:
        try:
            guest(args.instance, "config set system.net_recover off")
        except Exception:
            pass
        vm(args.instance, "stop")

    if args.positive_control:
        if res.fails:
            print(f"\nnetheal_test: positive control FAILED as it must "
                  f"({len(res.fails)} finding(s))")
            return 0
        print("\nnetheal_test: positive control PASSED -- with the feature off "
              "this tool still reported clean, so it is not checking anything",
              file=sys.stderr)
        return 1

    print(f"\nnetheal_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
