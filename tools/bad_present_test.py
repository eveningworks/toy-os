"""tools/bad_present_test.py -- a refused buffer replacement keeps the
last good frame, and the compositor alive.

`/tests/badpresent` is a raw-protocol client: it presents buffer 0 at
generation 1, then presents the SAME buffer -- the one on screen -- at
generation 2 with a size its object cannot hold, so the compositor's
re-open is refused. The compositor used to unmap the old mapping before
trying the new one, and kept drawing from the dropped address: a client
error became a toywm page fault. Asserted:

  * toywm is the SAME process afterwards (its pid did not change -- init
    restarts a crashed desktop, so "the desktop answers" alone would
    pass on the broken build);
  * the window is still there, still showing generation 1.

uapp never re-presents its front buffer, which is why nothing in the
suite reached this before; only a raw client can.

    python3 tools/bad_present_test.py [--instance N] [--in-gui]
"""
import argparse
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

TITLE = "Bad Present"


def wm_pid(dbg):
    ps = [p for p in dbg.processes_named("toywm")]
    return ps[0]["pid"] if ps else None


def wait_log(dbg, text, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(text in ln for ln in dbg.logs("badpresent:", clear=False)):
            return True
        time.sleep(0.25)
    return False


def run(dbg, res):
    before = wm_pid(dbg)
    dbg.send("gui spawn /tests/badpresent")
    if not wait_log(dbg, "presented"):
        res.check("the raw client presents its first frame", False,
                  f"log: {dbg.logs('badpresent:', clear=False)}")
        return
    time.sleep(1.0)
    win = dbg.window(TITLE)
    res.check("its window shows generation 1", win is not None
              and (win.get("buf") or {}).get("gen") == 1, f"window: {win}")
    if not wait_log(dbg, "bad present sent"):
        res.check("the bad present is sent", False, "no log line")
        return
    time.sleep(2.0)
    dbg.settle()
    after = wm_pid(dbg)
    res.check("toywm survives a refused replacement of the buffer on screen",
              before is not None and after == before,
              f"toywm pid {before} -> {after} (a restart means it crashed)")
    win = dbg.window(TITLE)
    res.check("...and the window keeps its last good frame (generation 1)",
              win is not None and (win.get("buf") or {}).get("gen") == 1,
              f"window: {win}")
    for p in dbg.processes_named("badpresent"):
        dbg.send(f"sh kill {p['pid']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "bad_present_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    print("bad_present_test: checks")
    try:
        run(dbg, res)
    finally:
        dbg.close()
        qmp.close()
    print(f"\nbad_present_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
