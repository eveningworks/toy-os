"""tools/latency_under_io.py -- what heavy disk I/O does to desktop latency.

WHY THIS EXISTS
---------------
"The desktop feels slow under disk I/O" is the premise of the
interruptible-syscall work in docs/roadmap.md, and it had no number
behind it. This is the yardstick that item asks for: the SAME workload,
measured the same way, before and after -- so a change to the kernel's
blocking paths can be shown to have moved something rather than argued
to have.

WHAT IT MEASURES, FROM BOTH ENDS
--------------------------------
The compositor's own distributions (`gui latency`), which are the EFFECT:

  work      how long a frame's work took
  wake      how much LATER than asked the frame loop woke. This is the
            one that matters, and it is cyclictest's measurement: when
            another process holds the CPU the WM does no work at all, so
            every frame it eventually runs looks fast and only the gap
            moves.
  ping      the compositor <-> client round trip, i.e. what a client's
            own responsiveness looks like from outside it.

And the kernel's stall table (`stalls`), which is the CAUSE: a syscall
handler runs with interrupts off, so its duration is time in which
nothing else on the machine ran at all. One is how late; the other is
which syscall spent it.

HOW TO READ THE RESULT
----------------------
Quote DIFFERENCES between two builds measured the same way on the same
host, never an absolute. Everything here rides on the guest's timing:
the compositor's figures come from the system clocksource, which under
TCG is the PIT and quantises to 10 ms, so its `avg` columns are floor
noise and only its tens-of-milliseconds tail is evidence. `stalls` is
timed with the TSC and resolves microseconds on any machine, which is
why the attribution half is trustworthy where the effect half is coarse.

  python3 tools/vm.py --instance 3 start
  python3 tools/latency_under_io.py --instance 3

For the sharpest numbers, give the guest a TSC-backed clocksource so the
compositor's half resolves too -- this is the only way to reach it here:

  python3 tools/vm.py --instance 3 --kvm --cpu host,+invtsc start

EXIT CODE
---------
0 when it collected both arms, 1 when it did not -- no threshold on the
latency itself. There is no measured baseline to pick a ceiling from
yet, and a guessed one is the shape this repo keeps deleting; once the
trap-gate work lands and a real number exists, this grows a flag.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard                                        # noqa: E402
from qmp_test import QMPSession                          # noqa: E402
from gui_debug import DebugConsole, enter_gui            # noqa: E402

# Clients for the ping arm to have something to talk to. Three, so one
# app being slow to start does not leave the distribution empty.
APPS = ["Calculator", "UI Demo", "About"]

# Where diskbench writes its own report. /var/tmp is the DISK
# (docs/conventions/storage.md) -- /tmp is a ramfs mount and a workload
# aimed at it would measure memory.
BENCH_OUT = "/var/tmp/latency_under_io.txt"

DISTS = ("work", "wake", "ping")


def pct(dist, want):
    """The `want` percentile of a log2-us histogram, to a factor of two.

    Reports the bucket's LOWER edge rather than interpolating inside it.
    Interpolation would imply a precision the histogram does not have,
    and the comparison this tool exists for -- quiet against loaded --
    moves by orders of magnitude, not by fractions of a bucket.
    """
    buckets = dist.get("buckets") or []
    n = dist.get("n", 0)
    if not n or not buckets:
        return 0
    target = n * want / 100.0
    seen = 0
    for i, c in enumerate(buckets):
        seen += c
        if seen >= target:
            return 1 << i
    return 1 << (len(buckets) - 1)


def read_arm(dbg):
    """Both halves of one measurement: the compositor's, and the kernel's."""
    lat = dbg.json("gui latency --json")
    return {
        "latency": lat,
        "stalls": read_stalls(dbg),
        "clock_ns": lat.get("clock_ns", 0),
    }


def read_stalls(dbg):
    """`stalls` as a list of dicts, worst-first -- the program sorts.

    Parsing the columnar output rather than asking for JSON: no /bin
    program here emits JSON, and stalls.c's header declares this shape a
    contract for exactly this reader.
    """
    rows = []
    for line in dbg.send("sh stalls").splitlines():
        f = line.split()
        # `name(nr)  calls  max  avg  total` -- five fields, four numeric.
        if len(f) != 5 or "(" not in f[0]:
            continue
        try:
            calls, mx, avg, total = (int(x) for x in f[1:])
        except ValueError:
            continue
        rows.append({"name": f[0], "calls": calls,
                     "max_us": mx, "avg_us": avg, "total_us": total})
    return rows


def arm_stalls(dbg):
    out = dbg.send("sh stalls reset")
    if "zeroed" not in out:
        # A machine with no calibrated TSC frequency refuses, and saying
        # so beats reporting an empty attribution half as "no stalls".
        print(f"latency_under_io: could not arm the kernel stall table: {out.strip()}",
              file=sys.stderr)
        return False
    return True


def fmt_dist(name, quiet, loaded):
    q = quiet["latency"].get(name, {})
    l = loaded["latency"].get(name, {})
    return (f"  {name:<5} "
            f"n {q.get('n', 0):>6} -> {l.get('n', 0):<6} "
            f"avg {q.get('avg_us', 0):>7} -> {l.get('avg_us', 0):<8} "
            f"p90 {pct(q, 90):>7} -> {pct(l, 90):<8} "
            f"max {q.get('max_us', 0):>8} -> {l.get('max_us', 0):<9}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--quiet-seconds", type=float, default=10.0,
                    help="how long to sample the idle baseline (default 10)")
    ap.add_argument("--size", type=int, default=32,
                    help="diskbench working set in MiB (default 32)")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="give up on the workload after this long (default 300)")
    ap.add_argument("--top", type=int, default=6,
                    help="how many syscalls to attribute (default 6)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "latency_under_io")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    quiet = loaded = None
    try:
        for name in APPS:
            dbg.open_app(name)
            dbg.settle()

        # --- ARM 1: quiet. Nothing is driven, deliberately: the question
        # is what the machine does when it is left alone, which is the
        # only baseline a loaded number means anything against.
        if not arm_stalls(dbg):
            return 1
        dbg.send("gui latency reset")
        time.sleep(args.quiet_seconds)
        quiet = read_arm(dbg)

        # --- ARM 2: loaded. `gui spawn` rather than `sh spawn`, because
        # the shell's spawn WAITS -- it would park this console for the
        # length of the benchmark and the sampling would never happen.
        if not arm_stalls(dbg):
            return 1
        dbg.send("gui latency reset")
        out = dbg.send(f"gui spawn /bin/diskbench --size {args.size} --out {BENCH_OUT}")
        if "spawned" not in out:
            print(f"latency_under_io: diskbench did not start: {out.strip()}",
                  file=sys.stderr)
            return 1

        # Sample WHILE it runs, and stop at its own `done` line rather
        # than after a fixed sleep -- a guest under load takes as long as
        # it takes, and a sleep that ends early measures a quiet machine
        # and calls it loaded.
        deadline = time.time() + args.timeout
        finished = False
        while time.time() < deadline:
            time.sleep(2.0)
            if "diskbench: done" in dbg.send(f"sh cat {BENCH_OUT}"):
                finished = True
                break
        loaded = read_arm(dbg)
        if not finished:
            print(f"latency_under_io: diskbench did not finish within "
                  f"{args.timeout:.0f}s -- the numbers below cover a partial run",
                  file=sys.stderr)
    finally:
        if dbg:
            dbg.send("sh stalls track off")
            dbg.send(f"sh rm {BENCH_OUT}")
            dbg.close()

    if not quiet or not loaded:
        return 1

    clock = loaded.get("clock_ns", 0)
    print(f"latency_under_io: diskbench --size {args.size}, "
          f"{args.quiet_seconds:.0f}s quiet baseline")
    # ZERO IS THE WORST READING, NOT THE BEST. The granularity is
    # measured by timing adjacent reads and keeping the smallest
    # INCREASE; 0 means no read ever saw one, i.e. the clock did not
    # move at all -- which is what the PIT tick counter does over a
    # window shorter than 10 ms. Printed raw it reads as perfect
    # precision, which is the opposite of what it says.
    coarse = clock == 0 or clock > 1_000_000
    print("  compositor clock granularity "
          + (f"{clock} ns" if clock else "0 ns (i.e. it never advanced)"))
    if coarse:
        print("      <-- coarser than a millisecond: the avg and p90 columns below\n"
              "          are floor noise and only the max tail is evidence.\n"
              "          `vm.py --kvm --cpu host,+invtsc` resolves them. The stall\n"
              "          table further down is TSC-timed and unaffected.")
    print("\ncompositor, quiet -> loaded (microseconds):")
    for name in DISTS:
        print(fmt_dist(name, quiet, loaded))

    print("\nwhat held the CPU under load (syscall handlers run with interrupts off):")
    rows = loaded["stalls"][:args.top]
    if not rows:
        print("  (nothing measured -- was the kernel stall table armed?)")
    else:
        quiet_max = {r["name"]: r["max_us"] for r in quiet["stalls"]}
        print(f"  {'syscall':<20} {'calls':>7} {'max us':>10} {'avg us':>9} "
              f"{'quiet max':>10}")
        for r in rows:
            print(f"  {r['name']:<20} {r['calls']:>7} {r['max_us']:>10} "
                  f"{r['avg_us']:>9} {quiet_max.get(r['name'], 0):>10}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
