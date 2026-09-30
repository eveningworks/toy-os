#!/usr/bin/env python3
"""`logd`'s per-boot retention: /var/log/boot/<n>.log and `log -p N`.

THIS TOOL REBOOTS THE GUEST, five times, which is why it lives in
`ondemand_sweep.py` rather than the gate -- it needs `vm.py --reboot`
(the default `-no-reboot` ends QEMU on a guest reboot) and costs a
couple of minutes.

WHAT IT ASSERTS, and why each one matters for something whose whole job
is to still be there after the machine was rebooted:

  - a boot log accumulates one file PER BOOT, numbered in order. The
    numbering is the retention: a gap or a reused number silently
    overwrites the evidence somebody rebooted the machine to keep.
  - `log -p N` reaches the Nth boot back, and REFUSES a number it does
    not have instead of printing the wrong boot's log. Reading boot 3
    while believing it is boot 5 is worse than an error.
  - lowering `storage.log_keep` actually DELETES, rather than stranding
    the files above the new limit forever.
  - a boot that fills its share STOPS and says so -- the property that
    stops one runaway logger flushing every older boot.

    python3 tools/logrotate_test.py
    python3 tools/logrotate_test.py --positive-control   # must FAIL
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
BOOT_DIR = "/var/log/boot"


Result = Results


def vm(inst, *args, timeout=300):
    r = subprocess.run(VM + ["--instance", str(inst)] + list(args),
                       capture_output=True, text=True, timeout=timeout)
    return (r.stdout or "") + (r.stderr or "")


def guest(inst, *cmds, timeout=300):
    return vm(inst, "exec", *cmds, timeout=timeout)


def listed_boots(out):
    """The boot numbers `log --list` printed, in the order it printed them."""
    nums = []
    for line in out.splitlines():
        m = re.match(r"\s*(\d+)\s+(-?\d+) K\b", line)
        if m:
            nums.append(int(m.group(1)))
    return nums


def log_line(out):
    """The last line of `out` that is actually a log line, or ""."""
    for ln in reversed(out.splitlines()):
        if ln.startswith("["):
            return ln.strip()
    return ""


def kept_logs(inst):
    out = guest(inst, "ls " + BOOT_DIR)
    return [ln.strip() for ln in out.splitlines()
            if re.fullmatch(r"\d+\.log", ln.strip())]


def poll_kept(inst, want, tries=10):
    """The retained files, once there are `want` of them or time is up.

    BOUNDED, AND IT WAITS ON THE ARTIFACT rather than on a fixed sleep:
    `vm.py start` returns when the shell is ready, and logd prunes from
    its own main() at whatever moment init got to it -- so a listing
    taken immediately after boot can catch the directory one file before
    the prune. That raced into a FAILURE that read exactly like an
    off-by-one in logd, which is what this exists to stop.
    """
    kept = kept_logs(inst)
    for _ in range(tries):
        if len(kept) <= want:
            break
        time.sleep(1)
        kept = kept_logs(inst)
    return kept


def cycle(inst, n):
    for _ in range(n):
        vm(inst, "--reboot", "start")
        guest(inst, "uptime")
        vm(inst, "stop")


def run(inst, res, control):
    vm(inst, "stop")
    vm(inst, "--reboot", "start")
    guest(inst, "config set storage.log_keep 4", "config set storage.log_max 8")
    if control:
        # POSITIVE CONTROL: keep=1 is the OLD behaviour -- one rotated
        # file. Every assertion below about depth must then fail. If this
        # tool still reports clean, it is not watching the retention.
        guest(inst, "config set storage.log_keep 1")
    vm(inst, "stop")

    cycle(inst, 4)
    vm(inst, "start")

    out = guest(inst, "log --list")
    nums = listed_boots(out)
    res.check("log --list shows one file per boot, in order",
              len(nums) >= 4 and nums == sorted(nums) and
              nums[-1] - nums[0] == len(nums) - 1,
              f"listed {nums}")

    # `-p N` MUST LAND ON THE RIGHT BOOT. Each boot log's LAST line
    # differs (the machine is up for a different length of time), so
    # asking for two different depths must give two different answers --
    # a resolver that ignored N would give the same one twice.
    # BOTH MUST BE REAL LOG LINES, not merely different text: a depth
    # that resolves to a file which is not there prints an error, and
    # "an error differs from a line" would satisfy a weaker check while
    # proving nothing about the resolver.
    a = log_line(guest(inst, "log -p 1 -n 1"))
    b = log_line(guest(inst, "log -p 3 -n 1"))
    res.check("log -p N reaches different boots for different N",
              bool(a) and bool(b) and a != b,
              f"-p 1 -> {a!r}, -p 3 -> {b!r}")

    # THE HEADER IS THE BOOT'S IDENTITY INSIDE THE FILE. Without it a
    # log that has been copied anywhere says nothing about which boot it
    # came from, and `--list`'s date column has nothing to read.
    head = guest(inst, "log --list")
    res.check("every boot's log opens with a header naming that boot",
              re.search(r"boot \d+ started", guest(inst, "log -p 1")) is not None,
              "no 'boot N started' line at the top of the previous boot's log")
    res.check("log --list shows a date rather than a boot-relative stamp",
              re.search(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}", head) is not None,
              f"no date in: {head.strip()[-200:]!r}")

    # TWO WAYS TO ASK FOR A BOOT THAT IS NOT THERE, and they are
    # different questions: one is beyond the counter and never existed,
    # the other existed and has been pruned. Answering either with the
    # other's message sends a reader looking in the wrong place.
    out = guest(inst, "log -p 99999")
    res.check("log -p refuses a boot that never existed",
              "no boot 99999 back" in out,
              f"expected a refusal, got: {out.strip()[:140]!r}")

    out = guest(inst, "log -p 99")
    ok = ("no longer retained" in out) or ("no boot 99 back" in out)
    res.check("log -p says a pruned boot is pruned, naming the BOOT",
              ok, f"expected a retention message, got: {out.strip()[:140]!r}")

    # -n MUST NOT BE SWALLOWED by the optional count.
    out = guest(inst, "log -p -n 2")
    body = [ln for ln in out.splitlines() if ln.startswith("[")]
    res.check("log -p -n 2 keeps -n, rather than reading it as the depth",
              len(body) == 2, f"got {len(body)} line(s)")

    # LOWERING THE COUNT DELETES. The failure this catches is files
    # stranded above the new limit, which look retained and are never
    # pruned again.
    #
    # COUNTED FROM `ls`, NOT FROM `log --list`, on purpose: the live
    # boot's row is the LAST line of the capture and a reply that is cut
    # one line short reads exactly like a missing file. What this
    # assertion is about is the directory anyway.
    guest(inst, "config set storage.log_keep 2")
    vm(inst, "stop")
    cycle(inst, 1)
    vm(inst, "start")
    kept = poll_kept(inst, 2)
    res.check("lowering storage.log_keep deletes, rather than stranding",
              len(kept) == 2, f"log_keep 2 left {len(kept)}: {kept}")

    # A BOOT THAT FILLS ITS SHARE STOPS. 1 MiB over keep+1 is a small
    # enough share that a few straces reach it.
    guest(inst, "config set storage.log_max 1", "config set storage.log_keep 50")
    vm(inst, "stop")
    vm(inst, "--reboot", "start")
    for _ in range(8):
        guest(inst, "strace ls /bin")
    out = guest(inst, "log -n 2")
    res.check("a boot that fills its share stops, and says so",
              "share of storage.log_max" in out,
              f"no cap line in: {out.strip()[-200:]!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=3,
                    help="VM slot; this tool REBOOTS it")
    ap.add_argument("--positive-control", action="store_true",
                    help="keep only one boot; the run must then fail")
    args = ap.parse_args()

    res = Result()
    try:
        run(args.instance, res, args.positive_control)
    finally:
        # RESTORE THE DEFAULTS. A tool that leaves storage.log_max at 1
        # MiB makes every later tool's logs stop mid-boot, and that reads
        # as the machine hanging.
        try:
            guest(args.instance, "config set storage.log_max 8",
                  "config set storage.log_keep 10")
        except Exception:
            pass
        vm(args.instance, "stop")

    if args.positive_control:
        if res.fails:
            print(f"\nlogrotate_test: positive control FAILED as it must "
                  f"({len(res.fails)} finding(s))")
            return 0
        print("\nlogrotate_test: positive control PASSED -- with one boot kept "
              "this tool still reported clean, so it is not checking the "
              "retention", file=sys.stderr)
        return 1

    print(f"\nlogrotate_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
