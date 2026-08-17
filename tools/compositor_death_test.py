#!/usr/bin/env python3
"""The compositor death path -- Milestone 41's R7.

WHAT IS UNDER TEST
------------------
`killing the compositor must not panic the kernel` is the exit criterion
of the whole milestone, and it is the one property that only becomes
true at stage 4d. This asserts it now, against a STAND-IN compositor,
because it is kernel-side and does not need the ring-3 WM to exist:
`screenclient` claims the compositor role and takes the framebuffer
grant, which is exactly the state the real WM will be in when it dies.

Three deaths, one path. The kernel routes a clean deregistration, a
`kill`, and a fault through `win_server_set_compositor(0, 0)` -- so this
kills the process outright, which is the least graceful of the three and
the one a crashing WM actually takes.

WHAT THE KERNEL MUST DO
-----------------------
1. Survive. Checked by asking the WM a question afterwards: `gui state`
   is dispatched from inside `wm_run()`, so an answer proves the kernel
   AND the desktop are still running. A panic answers nothing.
2. Revoke the framebuffer grant, so a dead compositor's mapping cannot
   outlive it.
3. ASK every client window to close (WIN_EV_CLOSE) rather than
   destroying it. Destroying frees the client's own buffer pages, so a
   client mid-draw would fault -- the compositor dying would cascade.
   See win_server.c's compositor_gone().
4. Let the next compositor register. A role that cannot be re-claimed
   after a crash is a desktop that cannot be restarted.

WHAT IT DELIBERATELY DOES NOT ASSERT
------------------------------------
That clients are asked to close and the text console comes back -- the
other half of R7. Neither can happen yet, and the kernel is right to
skip them: while the RING-0 WM is registered it still owns the screen
and the window list, so a stand-in compositor leaving is a second
consumer going away, not the desktop dying. Tearing down there kills
live windows the WM is still drawing.

So this asserts the SKIP, which is a real property and one that was
broken within an hour of being written (the first version of
compositor_gone() had no such guard, and `compositor_test.py` failed
with UI Demo going silent). The two checks invert the day the WM is the
compositor: `win_server_active()` is false then, the teardown runs, and
`tools/` gains a scenario that drives it from the PHYSICAL shell, where
no ring-0 WM exists to own anything.

WHICH CHECKS ARE LOAD-BEARING
-----------------------------
Measured, not assumed: disabling `compositor_gone()` reddens exactly TWO
of the ten -- "reports asking client windows to close" and "console is
left to the ring-0 WM". The other eight stay green, correctly, because
they were already true before R7: stage 4a made the role-clear path a
single chokepoint that revokes the framebuffer grant, so surviving the
kill, releasing the role and re-claiming it afterwards are REGRESSION
cover for that work rather than tests of this.

Worth knowing before trusting a green run here: two checks are testing
the new path and eight are guarding the old one.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/compositor_death_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
COMP = "/tests/screenclient"
CLIENT = "/tests/winclient"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"    [{detail}]" if detail else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)
    dbg = DebugConsole(args.sock)

    print("compositor death (M41 R7):")

    # A real client window, so there is something for the kernel to ask
    # to close. Without one the interesting half of the path is not
    # exercised at all and the test would pass against a kernel that
    # simply ignores clients.
    dbg.send(f"gui spawn {CLIENT}")
    time.sleep(1.2)
    wins_before = dbg.windows() or []
    client_wins = [w for w in wins_before if w.get("client_pid")]
    check("a client window exists to be asked to close",
          len(client_wins) >= 1, f"{len(client_wins)} client window(s)")

    # The stand-in compositor: claims the role and maps the real screen.
    dbg.logs()
    dbg.send(f"gui spawn {COMP}")
    time.sleep(1.2)
    startup = "\n".join(dbg.logs())
    check("the stand-in compositor registered and took the grant",
          "screenclient: registered" in startup and "screenclient: screen " in startup,
          "registered + screen line")

    comp = dbg.json("gui compositor --json") or {}
    comp_pid = comp.get("pid", 0)
    check("the kernel agrees it is the compositor", comp_pid > 0, f"pid={comp_pid}")

    if comp_pid <= 0:
        return summarise()

    # --- the death, by kill: the least graceful of the three paths ----
    dbg.logs()
    dbg.send(f"gui kill {comp_pid}")
    time.sleep(1.5)
    after = "\n".join(dbg.logs())

    # 1. The kernel survived, and so did the desktop. `gui state` is
    #    dispatched from inside wm_run(), so an answer is a liveness
    #    proof for both -- a panic or a wedged loop answers nothing.
    state = dbg.state()
    check("the kernel survives killing the compositor", bool(state),
          "gui state answered")

    # 3. The teardown is SKIPPED, because the ring-0 WM still owns the
    #    desktop. A stand-in compositor leaving is a second consumer
    #    going away (stage 2's design), not the desktop dying -- and the
    #    first version of this change got that wrong, tearing down live
    #    windows the WM was still drawing. `compositor_test.py` caught it
    #    as UI Demo going silent; this check is what keeps it caught
    #    here, next to the code, instead of as a puzzling failure in a
    #    tool about something else.
    check("a stand-in compositor leaving does NOT tear down the desktop",
          "still owns the screen" in after,
          next((l for l in after.split("\n") if "compositor" in l), "no line"))

    # ...and the window is still there, because asking is not destroying.
    # This is the check that separates the chosen design from R7's
    # original "drop every client window": a destroyed window would be
    # gone from this list AND its client would be faulting.
    wins_after = dbg.windows() or []
    still = [w for w in wins_after if w.get("client_pid")]
    check("the client window still exists (asked, not destroyed)",
          len(still) >= 1, f"{len(still)} client window(s) after")

    # ...and specifically the console is not repainted over the live
    # desktop, which is the visible half of the same mistake.
    check("the console is not repainted over the live desktop",
          "console restored" not in after,
          next((l for l in after.split("\n") if "compositor" in l), "no line"))

    # 2 + 4. The role is free and re-claimable -- a desktop that cannot
    # be restarted after a crash is not survivable in any useful sense.
    comp2 = dbg.json("gui compositor --json") or {}
    check("the compositor role is released", comp2.get("pid", -1) == 0,
          f"pid={comp2.get('pid')}")

    dbg.logs()
    dbg.send(f"gui spawn {COMP}")
    time.sleep(1.2)
    again = "\n".join(dbg.logs())
    comp3 = dbg.json("gui compositor --json") or {}
    check("a new compositor can claim the role after the crash",
          comp3.get("pid", 0) > 0 and "screenclient: screen " in again,
          f"pid={comp3.get('pid')}")

    # Leave nothing running for the next tool.
    if comp3.get("pid", 0) > 0:
        dbg.send(f"gui kill {comp3['pid']}")
        time.sleep(0.8)
    check("the desktop is still alive at the end", bool(dbg.state()),
          "gui state answered")

    return summarise()


def summarise():
    bad = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(bad)}/{len(checks)} checks passed")
    if bad:
        print("FAILED: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
