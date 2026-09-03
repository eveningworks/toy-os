#!/usr/bin/env python3
"""tools/blank_window_test.py -- no app opens a blank window.

WHY THIS EXISTS
---------------
UI Demo shipped completely empty and a 35-check suite passed it. Every
check asserted on the app's LOG -- click a coordinate, read the line it
produced -- and the widgets were live, hit-testable and reporting
correctly. They were simply never painted, because the app's on_draw
cleared the surface after the toolkit had drawn them.

That is `docs/gui-guidelines.md`'s oldest rule ("it responds" is not "it
is drawn") failing for the third time in this project, so the answer is
not another check inside one app's test. This tool asks the same
question of EVERY app, and it asks the KERNEL which apps exist rather
than carrying a list -- so an app added tomorrow is covered without
anyone remembering to add it here.

WHAT IT ASSERTS
---------------
For each entry in the Start-menu registry: open it, wait for a window,
and require its content area to contain more than a handful of distinct
colours. A window that draws nothing is one flat fill (plus whatever
chrome the WM puts around it); anything that actually renders text or
widgets produces dozens, because the font is anti-aliased.

Deliberately a WEAK threshold. This is a smoke test for "did anything
appear at all", not a rendering check -- the per-app tools own the
question of whether the right thing appeared, and a tight threshold
here would fail on a legitimately sparse app instead.

    python3 tools/vm.py start
    python3 tools/blank_window_test.py
    echo $?
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

# A blank window is one fill; text and widgets are dozens once the font's
# anti-aliasing is counted. Six separates those two cases with room to
# spare in both directions.
MIN_COLOURS = 6

# How long a launcher entry gets to spawn its process and present a first
# frame. Generous: this runs after a cold boot, and a slow spawn scored
# as "blank" would be a false failure -- the worst kind for a tool whose
# whole job is to be believed.
OPEN_TIMEOUT_S = 15.0


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def close_everything(dbg, timeout=10.0):
    """Close every window AND wait for the processes behind them to go.

    MAX_PROCS is 4 (scheduler.c) and most registry entries are launchers,
    so walking the whole registry exhausts the process table unless each
    app's process is actually gone before the next one starts. A closed
    WINDOW is not yet an exited PROCESS -- the client has to notice the
    close, return from its loop and exit, and the WM has to reap it.
    `gui state`'s "launched" list is the WM's own reaped table, so this
    waits on the real thing rather than sleeping and hoping.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        ws = dbg.json("gui windows --json")["windows"]
        if ws:
            dbg.send(f"gui close {len(ws) - 1}")
            dbg.settle()
            continue
        if not dbg.json("gui state --json").get("launched", []):
            return
        time.sleep(0.2)


def app_names(dbg):
    """The registry, asked of the kernel rather than hardcoded here.

    This is the property that makes the tool survive the apps changing:
    `gui apps` lists what open_app() can actually launch, so a new entry
    in userland/wm/gui_apps.c is covered the first time this runs afterwards.
    """
    out = dbg.send("gui apps")
    names = []
    for line in out.splitlines():
        # "  Task Manager    resizable=1 multi_instance=0" -- the name is
        # everything before the first attribute, and it can contain a
        # space ("Task Manager"), so split on the run of spaces the
        # kernel pads with rather than on whitespace generally.
        if "resizable=" not in line:
            continue
        name = line.split("resizable=")[0].strip()
        if name:
            names.append(name)
    return names


def content_colours(qmp, tmp, win, tag):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, f"blank_{tag}.png"))
    qmp.screenshot(p)
    c = win["content"]
    with Image.open(p) as im:
        box = im.convert("RGB").crop(
            (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"]))
        return len(box.getcolors(maxcolors=1 << 24) or [])


def run(dbg, qmp, tmp, res):
    names = app_names(dbg)
    if not names:
        res.check("the app registry could be read", False,
                  "`gui apps` returned nothing parseable")
        return
    print(f"blank_window_test: {len(names)} apps in the registry")

    for name in names:
        close_everything(dbg)
        dbg.logs("", clear=True)
        dbg.send(f"gui open {name}")

        # A launcher spawns a process, so the window may take a moment --
        # and its TITLE may not be the app's name (Notepad opens
        # "untitled"), which is why this waits for any window rather than
        # a named one.
        deadline = time.time() + OPEN_TIMEOUT_S
        win = None
        while time.time() < deadline:
            ws = dbg.json("gui windows --json")["windows"]
            if ws:
                win = ws[-1]      # topmost
                break
            time.sleep(0.3)

        if win is None:
            res.check(f"{name} opened a window", False,
                      f"no window within {OPEN_TIMEOUT_S}s")
            continue

        dbg.settle()
        time.sleep(0.5)           # let the client present its first frame
        n = content_colours(qmp, tmp, win, name.replace(" ", "_"))
        res.check(f"{name} draws something", n >= MIN_COLOURS,
                  f"only {n} distinct colour(s) in its content area -- "
                  f"a blank window")

    close_everything(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "blank_window_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, res)
    finally:
        dbg.close()

    print(f"\nblank_window_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
