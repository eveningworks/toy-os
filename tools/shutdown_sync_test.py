#!/usr/bin/env python3
"""A write made just before `reboot` must survive it.

THIS TOOL REBOOTS THE GUEST, four times, so it lives in
`ondemand_sweep.py` rather than the gate and needs `vm.py --reboot`.

WHY IT EXISTS. `system_reboot()` flushed the ATA SECTOR CACHE and
nothing else. That is a layer BELOW the filesystem: `storage.sync =
batched` (the default) keeps a TFS3 journal transaction open across
writes, so blocks the transaction still owns have never reached the
sector cache and a flush cannot save them. Measured 2026-09-19 -- a
`config set` followed at once by `reboot` left a 0-BYTE
/etc/storage.conf and the setting silently back at its default. Worse
than losing the new value: `fs_write()` truncates first and the
truncation DID land, so the file's previous contents went too.

It cost a 40-boot soak its log retention, silently, because `config
get` answered from memory and said the new value the whole time.

WHAT IT ASSERTS:
  - a setting written immediately before a reboot reads back after it
  - ...including when the config file did not exist beforehand, which
    is the case that destroys a file rather than merely losing a write
  - an ordinary file written immediately before a reboot survives too,
    so the property is the filesystem's and not one setting's

    python3 tools/shutdown_sync_test.py
    python3 tools/shutdown_sync_test.py --positive-control   # must FAIL
"""

import argparse
import os
import subprocess
import sys
import time
from harness import Results  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VM = [sys.executable, os.path.join(ROOT, "tools", "vm.py")]
CONF = "/etc/storage.conf"


Result = Results


def vm(inst, *args, timeout=300):
    r = subprocess.run(VM + ["--instance", str(inst)] + list(args),
                       capture_output=True, text=True, timeout=timeout)
    return (r.stdout or "") + (r.stderr or "")


def guest(inst, *cmds, timeout=300):
    return vm(inst, "exec", *cmds, timeout=timeout)


def wait_up(inst, deadline_s=300):
    """True once the guest answers again.

    GENEROUS ON PURPOSE. Each probe can itself burn vm.py's 30 s reply
    timeout while the serial side reconnects, so a deadline that looks
    like a minute of slack is really only two attempts -- and a
    too-short one here does not report "slow", it reports whichever
    assertion came next as a data loss. It made the positive control
    fail for the wrong reason, which is a control proving nothing.
    """
    end = time.time() + deadline_s
    while time.time() < end:
        if "up " in guest(inst, "uptime"):
            return True
        time.sleep(5)
    return False


def read_back(inst, cmd, want, tries=6):
    """`cmd`'s output once it contains `want`, or the last thing it said.

    RETRIED, BOUNDED. `wait_up()` returns as soon as `uptime` answers,
    and the guest can still be finishing boot when the next command
    arrives -- which times out and reads exactly like the value having
    been lost. That flake reported a PASSING case as a failure once
    already; the data was on disk the whole time.
    """
    out = ""
    for _ in range(tries):
        out = guest(inst, cmd)
        if want in out:
            return out
        time.sleep(5)
    return out


def set_and_reboot(inst, *cmds):
    """Run `cmds` and reboot with NO sync and NO pause between them.

    The gap is the whole experiment: `sync` first, or a second of idle,
    and the deferred transaction commits on its own and nothing is
    proved.
    """
    guest(inst, *cmds, "reboot")
    return wait_up(inst)


def run(inst, res, control):
    scratch = "/tmp/shutdown_probe" if control else "/var/tmp/shutdown_probe"
    # POSITIVE CONTROL: /tmp is a tmpfs mount, so a file there CANNOT
    # survive a reboot by design. If this tool still reports clean with
    # the probe pointed at it, it is not reading back across the reboot
    # at all.

    vm(inst, "stop")
    vm(inst, "--reboot", "start")
    if not wait_up(inst):
        res.check("the guest came up", False)
        return

    # --- an EXISTING config file, updated then rebooted ---------------
    guest(inst, "config set storage.log_keep 9", "sync")
    if not set_and_reboot(inst, "config set storage.log_keep 33"):
        res.check("the guest came back after the first reboot", False)
        return
    out = read_back(inst, "config get storage.log_keep", "33")
    res.check("a setting written just before reboot survives it",
              "33" in out, f"config get answered: {out.strip()[-80:]!r}")

    # ...and the PREVIOUS value must not have been destroyed either.
    out = read_back(inst, "cat " + CONF, "log_keep")
    res.check("...and the config file is not left empty",
              "log_keep" in out, f"{CONF} reads: {out.strip()[-80:]!r}")

    # --- a config file that did not EXIST -----------------------------
    guest(inst, "rm " + CONF, "sync")
    if not set_and_reboot(inst, "config set storage.log_keep 17"):
        res.check("the guest came back after the second reboot", False)
        return
    out = read_back(inst, "config get storage.log_keep", "17")
    res.check("a setting that had to CREATE its file survives too",
              "17" in out, f"config get answered: {out.strip()[-80:]!r}")

    # --- an ordinary file, so this is the filesystem's property --------
    # `cp`, NOT `echo >`: vm.py's exec hands the command straight to a
    # shell that does not parse redirections, so the `>` arrives as an
    # ARGUMENT and the file is never written -- which reads exactly like
    # the reboot having lost it. It reported a false failure once.
    if not set_and_reboot(inst, f"cp {CONF} {scratch}"):
        res.check("the guest came back after the third reboot", False)
        return
    out = read_back(inst, "cat " + scratch, "log_keep")
    res.check("an ordinary file written just before reboot survives",
              "log_keep" in out, f"{scratch} reads: {out.strip()[-80:]!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=4,
                    help="VM slot; this tool REBOOTS it")
    ap.add_argument("--positive-control", action="store_true",
                    help="probe a tmpfs path, which cannot survive; must fail")
    args = ap.parse_args()

    res = Result()
    try:
        run(args.instance, res, args.positive_control)
    finally:
        try:
            guest(args.instance, "config set storage.log_keep 10", "sync")
        except Exception:
            pass
        vm(args.instance, "stop")

    if args.positive_control:
        if res.fails:
            print(f"\nshutdown_sync_test: positive control FAILED as it must "
                  f"({len(res.fails)} finding(s))")
            return 0
        print("\nshutdown_sync_test: positive control PASSED -- a tmpfs file "
              "appeared to survive a reboot, so this tool is not reading back "
              "across one", file=sys.stderr)
        return 1

    print(f"\nshutdown_sync_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
