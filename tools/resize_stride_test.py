#!/usr/bin/env python3
"""tools/resize_stride_test.py -- a resized window's buffers agree with
the size the client is drawing at.

WHAT THIS COVERS
----------------
A window has two buffers, and only the one being drawn into is rebuilt
on a resize (`abi/win_proto.h`'s configure/ack). The other is the
client's to bring up to date before it draws into it next, which
Toykit's `buf_ensure()` does -- replacing the object if it needs to, and
telling the server the new dimensions with `WIN_REQ_BUFFER`.

When that second half does not happen, the client draws at one stride
and the compositor composites at another, because a present carries the
SERVER's record of the front buffer's size. The window is then sheared
one pixel per row, and it stays sheared until a further resize happens
to correct the record -- which is exactly what a person reports as
"resizing sometimes skews the window".

THE ASSERTION
-------------
At rest, after the client has repainted, `lswin` must report BOTH of a
window's buffers at the window's own size. This is not a pixel test on
purpose: the shear is a disagreement between two numbers the kernel
already prints, and reading them is both sharper and cheaper than
looking for a diagonal in a screenshot.

WHY IT STEPS BY ONE PIXEL
-------------------------
The bug is invisible at any larger step. A buffer's length is page
rounded, and the old check compared lengths -- so it caught a resize
that crossed a page boundary and missed one that did not. A 7-pixel
drag moves w*h*4 by enough to cross one nearly every time; a 1-pixel
drag usually does not. Whatever replaces this check, the sweep has to
keep single-pixel steps or it measures nothing.

POSITIVE CONTROL
----------------
Run with --control for the exact edit. MEASURED against the pre-fix
build: 8 of the 16 sweep steps went red on a 986-wide Terminal, in the
alternating pattern page rounding predicts, and the "size held" check
went red with them.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui     # noqa: E402
from qmp_test import QMPSession                   # noqa: E402
import port_guard                                 # noqa: E402

TITLE = "Terminal"
STEPS = 16

CONTROL = """In userland/ui/uapp.c's buf_ensure(), put the length
comparison back in place of the dimension one:

    uint64_t want = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    if (g_px[buf] && g_px_bytes[buf] == want) return 1;

then `make iso` and re-run. Roughly half the sweep steps must go red.
"""

# `lswin`'s rows: pid, window, w, h, front, then w x h and a generation
# per buffer. Matched rather than split on whitespace so a changed
# column count fails loudly here instead of shifting an index silently.
ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+"
                 r"(\d+)x(\d+)\s+g(\d+)\s+(\d+)x(\d+)\s+g(\d+)\s*$")


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def lswin_row(dbg, pid):
    """The kernel's row for `pid`'s window: (w, h, front, bufs)."""
    for line in dbg.send("sh lswin").replace("\r", "").split("\n"):
        m = ROW.match(line)
        if not m or int(m.group(1)) != pid:
            continue
        g = [int(x) for x in m.groups()]
        return g[2], g[3], g[4], [(g[5], g[6]), (g[8], g[9])]
    return None


def run(dbg, res):
    if not dbg.window(TITLE):
        dbg.open_app(TITLE)
        time.sleep(2.0)
    win = dbg.window(TITLE)
    res.check("Terminal opens as a ring-3 client", win is not None)
    if not win:
        return

    pid = win.get("client_pid")
    res.check("the window reports its client pid", pid is not None,
              "no client_pid in `gui windows` -- cannot find its lswin row")
    if pid is None:
        return

    w0, h0 = win["content"]["w"], win["content"]["h"]
    bad = []
    for i in range(STEPS):
        want_w = w0 + i
        dbg.send(f"gui resize {want_w} {h0}")
        time.sleep(1.0)
        dbg.settle()
        row = lswin_row(dbg, pid)
        if row is None:
            bad.append(f"{want_w}: no lswin row")
            continue
        kw, kh, _front, bufs = row
        if kw != want_w or kh != h0:
            # The client declined or the WM clamped -- not this test's
            # subject, and asserting on it would fail for the wrong
            # reason at the screen edge.
            continue
        if any(b != (kw, kh) for b in bufs):
            bad.append(f"{kw}x{kh}: buffers {bufs}")

    res.check(f"both buffers match the window at every size ({STEPS} 1px steps)",
              not bad, "; ".join(bad))

    # The window really did move, so the sweep above was not asserting
    # over a client that ignored every proposal.
    win = dbg.window(TITLE)
    res.check("the sweep actually resized the window",
              win is not None and win["content"]["w"] == w0 + STEPS - 1,
              f"ended at {win['content'] if win else None}, wanted w={w0 + STEPS - 1}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--control", action="store_true",
                    help="print the positive control and exit")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "resize_stride_test")

    if args.control:
        print(CONTROL)
        return 0

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, res)
    qmp.close()

    print(f"\nresize_stride_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
