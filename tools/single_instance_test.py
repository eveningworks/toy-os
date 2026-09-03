#!/usr/bin/env python3
"""Single-instance apps: WIN_REQ_ACTIVATE and UAPP_SINGLE_INSTANCE.

WHAT IS UNDER TEST
------------------
A ring-3 app that declares `.app_id` plus UAPP_SINGLE_INSTANCE asks TWS,
before it opens anything, whether a window already carries that id. If
one does, TWS raises it and the second copy exits 0 without ever
appearing. Task Manager and About opted in; everything else did not.

Three separate claims, and the middle one is the load-bearing one:

  1. Launching a single-instance app twice leaves ONE window.
  2. The second launch RAISES the first -- it does not merely decline to
     start. This is the half that makes the behaviour feel correct
     rather than broken, and it is the half an implementation is most
     likely to skip: an app that just exits passes check 1 perfectly.
  3. An app that did NOT opt in still opens as many copies as it is
     asked to. Without this the suite cannot tell "single-instance
     works" from "spawning a second window is broken everywhere", which
     is the same shape as this repo's "pair it changed with something
     that must not change" rule.

Check 3 is the built-in positive control. It is why the test spawns UI
Demo twice as well: UI Demo declares no app id, so two windows is the
correct answer there, and a change that made ACTIVATE match too eagerly
(an empty id matching every window that never set one, say) reddens
exactly that check while leaving 1 and 2 green.

HOW "IT WAS RAISED" IS ASSERTED
-------------------------------
`gui windows --json` reports z-order bottom to top, so the LAST entry is
frontmost and focused (see wm_debug.c's cmd_windows). The test buries
Task Manager under a second window first, then launches it again and
requires it to be back on top. Asserting focus without burying it first
would pass against an implementation that does nothing at all, since a
freshly opened window is already frontmost -- the same trap as this
repo's "a test must not assume the thing it is testing".

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/single_instance_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TASKMGR = "/bin/wm/system/taskmgr"
ABOUT = "/bin/wm/system/about"
# Declares no app id, so it is the multi-instance control.
MULTI = "/bin/wm/demos/uidemo"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def windows(dbg, title=None):
    """Open windows, bottom to top. The last one is frontmost/focused."""
    wins = dbg.json("gui windows --json")["windows"]
    if title is not None:
        wins = [w for w in wins if w["title"] == title]
    return wins


def spawn(dbg, path, settle=1.2):
    """Launch a binary and give it time to present its first frame.

    Deliberately NOT DebugConsole.spawn(): that waits for a window to
    appear, and the whole point here is that the second launch must
    NOT produce one. Waiting for something that should never happen
    would turn a pass into a timeout.
    """
    dbg.send(f"gui spawn {path}")
    dbg.settle()
    time.sleep(settle)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "single_instance_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("single instance (WIN_REQ_ACTIVATE, UAPP_SINGLE_INSTANCE)")

    # --- 1. it opens once -------------------------------------------
    spawn(dbg, TASKMGR, settle=1.6)
    tm = windows(dbg, "Task Manager")
    if not check("Task Manager opened", len(tm) == 1, f"{len(tm)} window(s)"):
        print("\nnothing to drive -- the checks below would be vacuous")
        return report()

    # --- 2. a second launch adds no window ---------------------------
    spawn(dbg, TASKMGR)
    tm = windows(dbg, "Task Manager")
    check("second launch opened no second window", len(tm) == 1,
          f"{len(tm)} window(s)")

    # --- 3. ...and it RAISED the first one ---------------------------
    # Bury it first, or "it is frontmost" is satisfied by doing nothing.
    #
    # The window is identified by its client_pid, not by its title. That
    # is what makes this check load-bearing: with the raise disabled, a
    # brand-new Task Manager window opens and IS frontmost, so a
    # title-only assertion passes against exactly the bug it exists to
    # catch. Measured -- it stayed green through the positive control
    # until this was changed to compare pids.
    original_pid = tm[0]["client_pid"]

    spawn(dbg, MULTI, settle=1.6)
    top = windows(dbg)
    buried = bool(top) and top[-1]["title"] != "Task Manager"
    check("Task Manager buried under another window", buried,
          f"top is {top[-1]['title'] if top else 'nothing'}")

    spawn(dbg, TASKMGR)
    top = windows(dbg)
    raised = bool(top) and top[-1].get("client_pid") == original_pid
    check("relaunch raised the ORIGINAL window (same pid)", raised,
          f"top pid {top[-1].get('client_pid') if top else '-'}, "
          f"original {original_pid}")
    check("...and still only one of it", len(windows(dbg, "Task Manager")) == 1)

    # --- 4. the control: an app that did NOT opt in ------------------
    # UI Demo is already open once from the burying step above.
    before = len(windows(dbg, "UI Demo"))
    spawn(dbg, MULTI, settle=1.6)
    after = len(windows(dbg, "UI Demo"))
    check("a non-single-instance app still opens twice", after == before + 1,
          f"{before} -> {after}")

    # --- 5. a second single-instance app is independent --------------
    # Two different ids must not match each other -- a truncation bug or
    # a prefix match would make About raise Task Manager instead.
    spawn(dbg, ABOUT, settle=1.6)
    ab = windows(dbg, "About")
    check("About opened despite Task Manager holding an id", len(ab) == 1,
          f"{len(ab)} window(s)")
    spawn(dbg, ABOUT)
    check("About is single-instance too", len(windows(dbg, "About")) == 1)
    check("...and did not disturb Task Manager",
          len(windows(dbg, "Task Manager")) == 1)

    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nsingle_instance_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
