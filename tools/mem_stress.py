#!/usr/bin/env python3
"""Several memory hogs at once: does the machine survive, and is the memory right?

`/tests/memtest` takes everything SYS_SBRK will give it (~14 MiB, the
per-process bound in kernel/include/kernel/uaddr.h), writes a pattern
derived from the ADDRESS and its own random salt, and reads it back.
This runs several copies at once, which is what turns that into a real
test of the system rather than of one process:

  * **Exhaustion.** One process cannot fill the machine -- the heap
    bound stops it long before RAM does. N copies can, and what is
    under test is that running out is an ordinary refusal rather than a
    crash. `--mem` shrinks the guest so exhaustion is actually
    reachable: at ~14 MiB each, 8 copies need ~112 MiB, so a 128 MiB
    guest reaches the edge and a 2 GiB one never would.
  * **Integrity across processes.** Each copy salts its pattern
    differently, so if two processes ever end up sharing a physical
    frame, the loser reads back a value that is a valid pattern for
    somebody else and says so. A constant fill could not tell the
    difference -- both copies would read what they wrote.
  * **The allocator's own books.** Afterwards it runs `meminfo audit`,
    which walks every live address space and checks no mapping points
    at a frame the allocator thinks is free.

WHAT A PASS DOES NOT PROVE: that memory was exhausted. A guest with
room to spare will have every copy succeed, which is a fine result but
a different one -- so the summary says which happened rather than
printing PASS either way.

Usage:
    python3 tools/mem_stress.py                  # 4 copies, default guest
    python3 tools/mem_stress.py -n 8 --mem 128   # 8 copies on a small guest
    python3 tools/mem_stress.py -n 8 --mem 128 --cap 4
"""
import argparse
import re
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=4, help="copies to run at once")
    ap.add_argument("--mem", type=int, default=0,
                    help="guest RAM in MiB (smaller makes exhaustion reachable)")
    ap.add_argument("--cap", type=int, default=0,
                    help="MiB each copy should stop at (0 = take everything)")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    subprocess.run(["python3", "tools/vm.py", "stop"], capture_output=True)
    # --mem is a GLOBAL option on vm.py, so it goes before the
    # subcommand -- argparse rejects it after `start`.
    cmd = ["python3", "tools/vm.py"]
    if args.mem:
        cmd += ["--mem", str(args.mem)]
    cmd += ["start"]
    if subprocess.run(cmd, capture_output=True).returncode != 0:
        print("mem_stress: could not start the VM")
        return 2

    failures = []
    try:
        con = DebugConsole(".vm.serial", timeout=30.0)
        spawn = "/tests/memtest" + (" %d" % args.cap if args.cap else "")

        # All of them started before any of them finishes -- overlapping
        # is the entire point. Started one after another rather than
        # truly simultaneously, which is as close as the debug console
        # gets and is enough: each takes seconds to write its memory.
        for _ in range(args.n):
            con.send("sh spawn " + spawn)
        print("mem_stress: %d copies started, waiting" % args.n)

        deadline = time.time() + 120
        log = ""
        passed = failed = refused = 0
        while time.time() < deadline:
            time.sleep(2.0)
            # REPLACE, never append: `dmesg` returns the whole buffer
            # every time, so accumulating it counts each copy's result
            # once per poll -- eight copies reported as sixteen passes,
            # and the early-exit then fired before they had all run.
            log = con.send("sh dmesg")
            passed = len(re.findall(r"memtest: PASSED", log))
            failed = len(re.findall(r"memtest: FAIL", log))
            if passed + failed >= args.n:
                break
        refused = len(re.findall(r"sbrk\(\) rejected", log))

        for line in log.splitlines():
            if "MISMATCH" in line or "ALIASED" in line or "memtest: FAIL" in line:
                print("  " + line.strip())

        print("mem_stress: %d passed, %d failed, %d sbrk refusals seen"
              % (passed, failed, refused))
        if failed:
            failures.append("%d copies reported bad memory" % failed)
        if passed + failed < args.n:
            failures.append("only %d of %d copies reported at all -- did one die?"
                            % (passed + failed, args.n))

        # The kernel must still be answering, and its books must balance.
        audit = con.send("sh meminfo --audit")
        print(audit.strip().splitlines()[-1] if audit.strip() else "(no audit output)")
        if "DANGLING" in audit:
            failures.append("meminfo audit found dangling mappings afterwards")
        if "no dangling" not in audit:
            failures.append("meminfo audit did not report a clean result")

        if refused:
            print("mem_stress: memory WAS exhausted (sbrk refused) -- "
                  "the interesting case")
        else:
            print("mem_stress: memory was NOT exhausted; every copy fitted. "
                  "Use --mem to shrink the guest if you wanted the edge case.")
    finally:
        if not args.keep:
            subprocess.run(["python3", "tools/vm.py", "stop"], capture_output=True)

    if failures:
        print("\nmem_stress: FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print("\nmem_stress: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
