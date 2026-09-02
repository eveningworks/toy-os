#!/usr/bin/env python3
"""The frame allocator on a guest with more than 4 GiB: managed, idle, zoned.

Runs the `mm` KTEST suite on an 8 GiB guest and requires that its
above-4-GiB check RAN. On the ordinary 256 MiB boot that check skips,
and `ktest_run.py` reports a skip as a pass of everything else -- so
`make test` alone can never say whether frames above 4 GiB are
accounted for. This tool exists so that question has a runner.

What a PASS says: the firmware map's memory above 4 GiB is managed by
pmm, none of it is allocated, PMM_ZONE_DMA32 stays below 4 GiB and
PMM_ZONE_ANY reaches above it. What it does NOT say: that anything
can USE that memory -- the identity map still stops at 4 GiB until
"More than 4 GiB of RAM" stage 2 lands, and stage 5 is where this tool
grows a consumer that actually touches a high frame.

    python3 tools/highmem_test.py
    python3 tools/highmem_test.py --mem 6144    # any size past 4 GiB
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mem", type=int, default=8192, metavar="MIB",
                    help="guest RAM in MiB (default 8192; must exceed 4096)")
    ap.add_argument("--port", type=int, default=None, help="serial port for ktest_run.py")
    args = ap.parse_args()
    if args.mem <= 4096:
        print("highmem_test: FAIL -- --mem must exceed 4096 or the check under test skips")
        return 1

    cmd = [sys.executable, os.path.join(HERE, "ktest_run.py"),
           "--suite", "mm", "--mem", str(args.mem)]
    if args.port:
        cmd += ["--port", str(args.port)]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    out = r.stdout + r.stderr
    tail = [ln for ln in out.splitlines() if ln.strip()][-8:]
    if r.returncode != 0:
        print(f"highmem_test: FAIL -- the mm suite failed on a {args.mem} MiB guest")
        for ln in tail:
            print(f"    {ln}")
        return 1
    # The suite passing is not enough: the check must have RUN. A guest
    # that somehow came up with less memory than asked would skip it,
    # and a skip here is the whole hazard this tool is for.
    m = re.search(r"(\d+) passed, (\d+) failed, (\d+) skipped", out)
    if not m or int(m.group(3)) != 0:
        print("highmem_test: FAIL -- the above-4-GiB check skipped instead of running")
        for ln in tail:
            print(f"    {ln}")
        return 1
    print(f"highmem_test: PASS -- {m.group(1)} mm checks ran on a {args.mem} MiB guest, "
          f"none skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
