#!/usr/bin/env python3
"""Crash Test: the fault paths, and the gate in front of the dangerous half.

`userland/gui/demos/crashtest.c` has two kinds of button. The RING-3
ones fault in the app's own code and kill the process; the RING-0 ones
ask the kernel to panic, and are refused unless the kernel was booted
with `faultinject`.

WHY THIS IS SAFE TO PUT IN gui_regress: kernel faults are DISARMED by
default, so the ring-0 buttons here exercise the refusal, not the panic.
That is the half worth testing automatically anyway -- a gate that stops
working is silent, whereas a panic that stops working is noticed the
next time someone needs it. To exercise the panic itself:

    make iso KCMDLINE="faultinject"

and click a Ring 0 button by hand, or drive it as tools/ do.

The ring-3 half IS exercised, and it is the more interesting assertion:
a process dying must not take the desktop with it. The WM answering a
`gui` command afterwards is the liveness check, the same trick
sched_gui_test.py uses -- a frozen WM cannot answer one.

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/crashtest_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/bin/wm/demos/crashtest"
TITLE = "Crash Test"

checks = []
_log = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def drain(dbg):
    _log.extend(l.strip() for l in dbg.logs())
    return _log


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)

    dbg = DebugConsole(args.sock)
    print("crash test")

    dbg.send(f"gui spawn {SPAWN_PATH}")
    time.sleep(1.5)
    lines = drain(dbg)

    win = dbg.window(TITLE)
    check("the app opens a window", win is not None,
          f'{win["w"]}x{win["h"]}' if win else "no window")
    if not win:
        print("\ncrashtest_test: 1 passed, 1 failed")
        return 1
    cx, cy = win["content"]["x"], win["content"]["y"]

    # --- it enumerates the KERNEL's list ------------------------------
    # Not a hardcoded count: the point of the kernel owning the table is
    # that adding a kind needs no edit here or in the app.
    armed = next((l for l in lines if "crashtest: armed" in l), None)
    m = re.search(r"armed (\d+) kinds (\d+)", armed or "")
    check("it enumerates the kernel's fault kinds",
          m is not None and int(m.group(2)) > 0,
          armed or "no armed line")

    # --- and the gate is CLOSED by default ----------------------------
    check("kernel faults are disarmed unless asked for",
          m is not None and int(m.group(1)) == 0,
          f"armed={m.group(1)}" if m else "no armed line")

    geo = {}
    for line in lines:
        g = re.search(r"crashtest: layout (\w+) (\d+) (\d+) (\d+) (\d+) pitch (\d+)", line)
        if g:
            geo[g.group(1)] = tuple(int(v) for v in g.groups()[1:])
    check("it reports its button geometry", "ring0" in geo and "ring3" in geo,
          str(sorted(geo)))
    if "ring0" not in geo or "ring3" not in geo:
        print(f"\ncrashtest_test: {sum(1 for _, o in checks if o)} passed, "
              f"{sum(1 for _, o in checks if not o)} failed")
        return 1

    # --- a ring-0 button is REFUSED, and the machine lives ------------
    bx, by, bw, bh, pitch = geo["ring0"]
    mark = len(drain(dbg))
    dbg.send(f"gui click {cx + bx + bw // 2} {cy + by + bh // 2}")
    dbg.settle()
    time.sleep(0.6)
    after = drain(dbg)[mark:]
    check("a kernel-fault button is refused while disarmed",
          any("refused" in l for l in after),
          next((l for l in after if "crashtest" in l), "no refusal logged"))

    # The refusal must not be a polite lie: the machine has to still be
    # here. A `gui` command is dispatched from inside the WM's own loop,
    # so an answer IS the liveness proof.
    st = dbg.json("gui state --json")
    check("the machine survived the refusal", st is not None and "screen" in st,
          "WM answered" if st else "no answer")

    # --- a ring-3 button kills the APP, not the desktop ---------------
    bx, by, bw, bh, pitch = geo["ring3"]
    mark = len(drain(dbg))
    dbg.send(f"gui click {cx + bx + bw // 2} {cy + by + bh // 2}")
    time.sleep(1.5)
    after = drain(dbg)[mark:]
    check("the ring-3 button faulted the app",
          any("ring3" in l for l in after) and
          not any("RETURNED without faulting" in l for l in after),
          next((l for l in after if "crashtest" in l), "nothing logged"))

    # The window must be GONE -- a crashed client that keeps its window
    # is the failure this proves absent.
    time.sleep(0.5)
    check("the crashed app's window is gone", dbg.window(TITLE) is None,
          "still listed" if dbg.window(TITLE) else "removed")

    st = dbg.json("gui state --json")
    check("the desktop survived the crash", st is not None and "screen" in st,
          "WM answered" if st else "no answer")

    failed = sum(1 for _, ok in checks if not ok)
    print(f"\ncrashtest_test: {len(checks) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
