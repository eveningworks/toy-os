"""tools/ping_rtt.py -- the compositor <-> client round trip, in microseconds.

WHY THIS EXISTS
---------------
Stage 8 of docs/winserver-ring3-design.md moved a client's events off a
kernel queue and onto the client's own channel ring. The one thing a
kernel-relayed queue is plausibly BETTER at is latency -- the kernel
wakes the client directly -- so the move needed a number on each side
of it rather than an argument. The compositor already pings every
client on a cadence (WIN_EV_PING out, WIN_REQ_PONG back), and that
round trip crosses the event path once and the request path once; its
figure is what this reads, through `gui compositor --json`.

WHAT IT DOES
------------
Attaches to a running guest (start one with `python3 tools/vm.py
--instance N start`), enters the desktop, opens a few apps so there are
clients to ping, waits for a number of pings to happen, and prints the
last / worst / mean round trip. Exit 0 once at least `--pings` answers
have been counted; 1 if that never happens, which is a client not
answering at all.

Quote DIFFERENCES between two builds measured the same way on the same
host, and quote the CYCLES: the microsecond figure comes from the
guest's clocksource, which under QEMU is the PIT and quantises every
round trip to one 10 ms tick. rdtsc does not.

    python3 tools/vm.py --instance 3 start
    python3 tools/ping_rtt.py --instance 3
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard                                        # noqa: E402
from qmp_test import QMPSession                          # noqa: E402
from gui_debug import DebugConsole, enter_gui            # noqa: E402

APPS = ["Calculator", "UI Demo", "About"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--pings", type=int, default=12,
                    help="how many answered pings to wait for (default 12)")
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "ping_rtt")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        for name in APPS:
            dbg.open_app(name)
            dbg.settle()
        deadline = time.time() + args.timeout
        stats = {}
        while time.time() < deadline:
            stats = dbg.json("gui compositor --json")
            if stats.get("pings", 0) >= args.pings:
                break
            time.sleep(1.0)
    finally:
        dbg.close()

    n = stats.get("pings", 0)
    # TWO UNITS, and the cycles are the ones to compare: under QEMU the
    # guest's clocksource is the PIT, so every round trip reads as one
    # 10 ms tick in microseconds, while rdtsc resolves it.
    print(f"ping_rtt: {n} pings  "
          f"cycles last {stats.get('ping_cyc_last')} max {stats.get('ping_cyc_max')} avg {stats.get('ping_cyc_avg')}  "
          f"| us last {stats.get('ping_us_last')} max {stats.get('ping_us_max')} avg {stats.get('ping_us_avg')}  "
          f"(kernel queue: pending {stats.get('pending')} dropped {stats.get('dropped')})")
    return 0 if n >= args.pings else 1


if __name__ == "__main__":
    sys.exit(main())
