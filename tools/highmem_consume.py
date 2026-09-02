#!/usr/bin/env python3
"""Consume past 4 GiB from ring 3, and require the allocator's books to balance.

The proof stage of "More than 4 GiB of RAM". Everything else in that
milestone is asserted from inside the kernel: the `mm` KTESTs name the
frame behind one page and say it is high. This asks the opposite,
end-to-end question -- can ORDINARY PROCESSES actually consume more
memory than the low zone holds, and is the machine still correct
afterwards?

The arithmetic is what makes it a proof rather than a demonstration. On
an 8 GiB QEMU machine the zone below 4 GiB holds about 2.9 GiB of
usable RAM, so a peak of more than 4 GiB in ring-3 pages cannot have
come from anywhere but above it. That is why the threshold is a
fraction of the machine and not a round number someone liked.

WHAT A PASS SAYS: several `/tests/memtest` processes together held more
than `--min-gib` of memory at once; the drop was in the HIGH zone; every
copy verified its own address-derived pattern (so no two of them shared
a frame); and `meminfo --audit` then found no mapping pointing at a frame
the allocator thinks is free.

WHAT IT DOES NOT SAY: anything about a machine under memory PRESSURE.
Every copy here fits. `tools/mem_stress.py` is the tool for exhaustion,
and it deliberately uses a SMALL guest to reach it.

WHY THE PEAK IS POLLED. A copy frees everything when it exits, so the
interesting number exists only while they overlap -- reading meminfo
after they finish reports a machine with nothing in it. The poll runs
about twice a second, which is far finer than the seconds each copy
takes to write its pattern.

    python3 tools/highmem_consume.py
    python3 tools/highmem_consume.py -n 8 --cap 512    # more, smaller copies
    python3 tools/highmem_consume.py --mem 6144 --min-gib 3
"""
import argparse
import re
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole  # noqa: E402

FRAME = 4096


def fact(con, name):
    """A kernel fact as an int, or None. `config get` prints the value first."""
    out = con.send("sh config get " + name)
    for line in out.splitlines():
        line = line.strip()
        if re.fullmatch(r"\d+", line):
            return int(line)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mem", type=int, default=8192, metavar="MIB",
                    help="guest RAM in MiB (default 8192; must exceed 4096)")
    ap.add_argument("-n", type=int, default=6, help="copies to run at once")
    ap.add_argument("--cap", type=int, default=1024,
                    help="MiB each copy takes (SYS_SBRK bounds one process at ~2 GiB)")
    ap.add_argument("--min-gib", type=float, default=4.0,
                    help="GiB of high-zone frames that must be held at once")
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    args = ap.parse_args()

    if args.mem <= 4096:
        print("highmem_consume: FAIL -- --mem must exceed 4096 or there is no high zone")
        return 1

    subprocess.run([sys.executable, "tools/vm.py", "stop"], capture_output=True)
    cmd = [sys.executable, "tools/vm.py", "--mem", str(args.mem), "start"]
    if subprocess.run(cmd, capture_output=True).returncode != 0:
        print("highmem_consume: could not start the VM")
        return 2

    failures = []
    try:
        con = DebugConsole(".vm.serial", timeout=60.0)
        total_high = fact(con, "mem.frame_total_high")
        free_high0 = fact(con, "mem.frame_free_high")
        if not total_high:
            failures.append("the guest reports no frames above 4 GiB")
            raise SystemExit
        print("highmem_consume: high zone %.1f GiB, %.1f GiB free before"
              % (total_high * FRAME / 2**30, free_high0 * FRAME / 2**30))

        # A DELTA, not a total: the klog ring keeps whatever an earlier
        # run of this tool (or a hand-typed memtest) left in it, and a
        # stale PASSED satisfies the exit condition while copies are
        # still holding memory -- which then reads as a leak.
        log = con.send("sh dmesg")
        pass0 = len(re.findall(r"memtest: PASSED", log))
        fail0 = len(re.findall(r"memtest: FAIL", log))

        spawn = "sh spawn /tests/memtest %d" % args.cap
        for _ in range(args.n):
            con.send(spawn)
        print("highmem_consume: %d copies started at %d MiB each"
              % (args.n, args.cap))

        # REPLACE the log each poll, never append: `dmesg` returns the
        # whole buffer every time (mem_stress.py's note).
        low_free_high = free_high0
        deadline = time.time() + 300
        passed = failed = 0
        while time.time() < deadline:
            now = fact(con, "mem.frame_free_high")
            if now is not None and now < low_free_high:
                low_free_high = now
            log = con.send("sh dmesg")
            passed = len(re.findall(r"memtest: PASSED", log)) - pass0
            failed = len(re.findall(r"memtest: FAIL", log)) - fail0
            if passed + failed >= args.n:
                break
            time.sleep(0.5)

        held_gib = (free_high0 - low_free_high) * FRAME / 2**30
        print("highmem_consume: peak high-zone use %.2f GiB, %d passed, %d failed"
              % (held_gib, passed, failed))

        if held_gib < args.min_gib:
            failures.append("only %.2f GiB of high frames were held at once, "
                            "wanted %.1f -- raise -n or --cap"
                            % (held_gib, args.min_gib))
        if failed:
            failures.append("%d copies reported bad memory" % failed)
        if passed + failed < args.n:
            failures.append("only %d of %d copies reported at all -- did one die?"
                            % (passed + failed, args.n))
        for line in log.splitlines():
            if "MISMATCH" in line or "ALIASED" in line or "memtest: FAIL" in line:
                print("  " + line.strip())

        # The books must balance afterwards: no live mapping may point
        # at a frame the allocator has put back in its free list.
        audit = con.send("sh meminfo --audit")
        said = [ln.strip() for ln in audit.splitlines() if "audit:" in ln]
        print(said[0] if said else "(no audit output)")
        if "DANGLING" in audit or "no dangling" not in audit:
            failures.append("meminfo audit did not report a clean result")

        # And the machine gives the memory back.
        back = fact(con, "mem.frame_free_high")
        if back is not None and back < free_high0 - (total_high // 100):
            failures.append("high frames were not returned: %d free before, %d after"
                            % (free_high0, back))
    except SystemExit:
        pass
    finally:
        if not args.keep:
            subprocess.run([sys.executable, "tools/vm.py", "stop"], capture_output=True)

    if failures:
        print("\nhighmem_consume: FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print("\nhighmem_consume: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
