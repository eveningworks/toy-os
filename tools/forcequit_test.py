#!/usr/bin/env python3
"""tools/forcequit_test.py -- not-responding detection and force quit.

WHAT THIS COVERS
----------------
The liveness half of TWP (`WIN_EV_PING`/`WIN_REQ_PONG`), the WM's
not-responding marking, the force-quit dialog, `scheduler_kill()`, and
the scheduler-slot reaping that makes any of it repeatable.

THE CHECK THAT JUSTIFIES THE DESIGN
-----------------------------------
A client that REFUSES to close and a client that is WEDGED look
identical to a plain close timeout: in both cases the window is still
there N seconds later. Offering to force-quit an app that deliberately
declined would be obnoxious; not offering it for one that is hung is the
entire problem. Only a liveness ping separates them.

So the two are tested against each other:

  * `winclient` refuses its first two close requests and keeps pumping
    its queue. Asking it to close must NOT raise the dialog, ever.
  * `hangclient` stops pumping entirely. Asking it to close must raise
    the dialog, within a few seconds.

Neither check means much alone -- "no dialog appeared" is also what a
completely broken detector does, and "a dialog appeared" is what a naive
timeout does for both apps. The pair is the assertion.

SLOT REAPING
------------
Force quit is worth little if it works four times per boot. A launched
ring-3 client used to leak its scheduler slot (nothing polled it -- the
Terminal's children are reaped by the ring-3 shell's waitpid, but a
Start-menu launch has no shell), and with MAX_PROCS at 4 the fifth
launch silently did nothing. Measured before it was fixed: the fifth
`gui open Shapes` produced no window. So this runs SIX spawn/force-quit
cycles, and a regression shows up as a spawn that stops working.

POSITIVE CONTROL
----------------
Raise WM_PING_TIMEOUT_TICKS to something enormous (say 30000) and
rebuild. "Alt+F4 on a hung client offers Force Quit" goes red and
everything else stays green -- including the winclient checks, which is
the point: they pass because nothing is offered, and that is also what
a dead detector looks like. Their value is entirely in being paired.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
HANG_BIN = "/tests/hangclient"
WIN_BIN = "/tests/winclient"
HANG_TITLE = "Hang Test"
CLIENT_TITLE = "Ring 3 Client"

ALT_F4 = "gui key 0xa5 alt"

# The WM's not-responding timeout, shortened for this run.
#
# THIS TOOL WAITS OUT THAT TIMEOUT ABOUT TEN TIMES -- once per dialog it
# raises, six of them in the repeat loop -- which made it the slowest
# tool in gui_regress.py and therefore the suite's entire wall-clock
# floor (72s, against a 462s total across 24 tools running 8 at a time).
# The timeout's VALUE is not what is under test here; the detection, the
# dialog and force quit are. So it is turned down to 0.4s over the debug
# console (`gui pingtimeout`, wm_internal.h) and the waits below scale
# with it.
#
# The negative checks are the reason this cannot simply be tiny: they
# prove a dialog does NOT appear, and they have to wait longer than a
# real one would take. They wait a MULTIPLE of the timeout, so shrinking
# it keeps that relationship intact rather than quietly weakening them.
PING_TIMEOUT_TICKS = 40          # 0.4s at the PIT's 100Hz
PING_TIMEOUT_S = PING_TIMEOUT_TICKS / 100.0

# Rather more than the timeout, so "slower than expected" is
# distinguishable from "never happens".
DIALOG_WAIT_S = max(2.0, PING_TIMEOUT_S * 6)

CYCLES = 6


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def gui_json(dbg, command):
    """`gui ... --json`, tolerating a window manager that is no longer there.

    A RING-3 desktop can die mid-test -- which is exactly the bug this
    tool was used to find (a force quit unmapped the compositor's view
    of the dying client's buffer and the WM faulted blitting it). The
    console then answers "gui: no window manager running", which is not
    JSON, and letting that raise replaced the whole pass/fail table with
    a traceback naming `gui windows` rather than the failure. Returning
    {} lets the remaining checks report, and "the desktop survives a
    force quit" is the one that names it.
    """
    try:
        return dbg.json(command)
    except ValueError as exc:
        if "no window manager" in str(exc):
            return {}
        raise


def wm_alive(dbg):
    return bool(gui_json(dbg, "gui state --json"))


def titles(dbg):
    return [w.get("title", "") for w in gui_json(dbg, "gui windows --json").get("windows", [])]


def has_window(dbg, want):
    return any(t.startswith(want) for t in titles(dbg))


def marked(dbg, want):
    """Is the WM treating this window as not responding?

    Asks for the FLAG. The "(Not Responding)" suffix is added by
    wm_render.c at draw time, so it is not in the window's title and a
    test grepping the title for it finds nothing -- which is exactly
    what the first version of this check did, and it failed against a
    perfectly working feature.
    """
    for w in gui_json(dbg, "gui windows --json").get("windows", []):
        if w.get("title", "").startswith(want):
            return bool(w.get("not_responding"))
    return False


def dialog_open(dbg):
    return bool(gui_json(dbg, "gui state --json").get("overlays", {}).get("confirm_dialog"))


def wait_dialog(dbg, secs=DIALOG_WAIT_S):
    deadline = time.time() + secs
    while time.time() < deadline:
        if dialog_open(dbg):
            return True
        time.sleep(0.4)
    return False


def button(dbg, label):
    """A dialog button's centre, as the WM reports it.

    Not scanned for by colour and not derived from the message width:
    the labels here are "Force Quit"/"Wait", so anything that assumed
    "Yes"/"No" sizing would click the wrong place.
    """
    for b in gui_json(dbg, "gui dialog --json").get("buttons", []):
        if b.get("label") == label:
            return (b["cx"], b["cy"])
    return None


def spawn(dbg, path, title, timeout=8.0):
    dbg.send(f"gui spawn {path}")
    dbg.settle()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if has_window(dbg, title):
            return True
        time.sleep(0.3)
    return False


def close_dialog_if_open(dbg):
    if dialog_open(dbg):
        b = button(dbg, "Wait") or button(dbg, "No")
        if b:
            dbg.click(*b)
            dbg.settle()


def run(dbg, qmp, tmp, res):
    # --- a HEALTHY client that declines -------------------------------
    #
    # winclient refuses its first two close requests and keeps pumping.
    # It must be left alone: no not-responding mark, no dialog.
    res.check("a ring-3 client spawns without a Terminal in the loop",
              spawn(dbg, WIN_BIN, CLIENT_TITLE))
    if not has_window(dbg, CLIENT_TITLE):
        return

    dbg.send(ALT_F4)
    dbg.settle()
    offered = wait_dialog(dbg)
    res.check("a client that REFUSES a close is never offered for force quit",
              not offered,
              "the dialog appeared for an app that is answering perfectly well")
    close_dialog_if_open(dbg)

    res.check("...and it keeps its window", has_window(dbg, CLIENT_TITLE))
    res.check("...and is not marked as not responding",
              not marked(dbg, CLIENT_TITLE))

    # Tidy up: it has been asked twice now, so a third ask closes it.
    dbg.send(ALT_F4)
    dbg.settle()
    for _ in range(40):
        if not has_window(dbg, CLIENT_TITLE):
            break
        time.sleep(0.15)

    # --- a WEDGED client ----------------------------------------------
    res.check("the hang-test client spawns", spawn(dbg, HANG_BIN, HANG_TITLE))
    if not has_window(dbg, HANG_TITLE):
        return

    dbg.send("gui key h")   # it stops pumping its event queue, permanently
    dbg.settle()
    for _ in range(40):
        if any("hanging now" in l for l in dbg.logs("hangclient:", clear=True)):
            break
        time.sleep(0.15)

    # It is hung, but nobody has asked it for anything, so it must NOT
    # interrupt the user. A modal appearing on its own, over whatever
    # they were doing, for a window they never touched, is worse than
    # the hang.
    res.check("a hung app nobody is closing does NOT raise a dialog on its own",
              not wait_dialog(dbg, max(2.0, PING_TIMEOUT_S * 4)))

    dbg.send(ALT_F4)
    dbg.settle()
    got = wait_dialog(dbg)
    res.check("Alt+F4 on a hung client offers Force Quit", got,
              f"no dialog within {DIALOG_WAIT_S}s")
    if not got:
        return

    info = gui_json(dbg, "gui dialog --json")
    res.check("the dialog names the app and its buttons are verbs",
              HANG_TITLE in info.get("message", "")
              and button(dbg, "Force Quit") is not None
              and button(dbg, "Wait") is not None,
              f"dialog: {info}")

    res.check("the hung window is marked as not responding", marked(dbg, HANG_TITLE))

    # ...and the mark actually REACHES THE SCREEN. The flag being set is
    # not the same as the user being told (docs/gui-guidelines.md: "it
    # responds" is not "it is drawn"), and the title-bar suffix is the
    # only thing that tells them.
    win = None
    for w in gui_json(dbg, "gui windows --json").get("windows", []):
        if w.get("title", "").startswith(HANG_TITLE):
            win = w
    if win:
        from PIL import Image
        p = os.path.abspath(os.path.join(tmp, "fq_title.png"))
        qmp.screenshot(p)
        im = Image.open(p).convert("RGB")
        # The title bar strip, right of where the name ends -- blank on a
        # healthy window, glyphs once the suffix is there.
        y = win["y"] + 8
        x0 = win["x"] + 8 + len(HANG_TITLE) * 8
        band = [im.getpixel((x, y + dy))
                for x in range(x0, min(x0 + 90, win["x"] + win["w"] - 60))
                for dy in range(0, 8)]
        res.check("the '(Not Responding)' mark is actually drawn in the title bar",
                  len(set(band)) > 1,
                  "the title bar strip past the app's name is a flat colour")

    # --- Wait ----------------------------------------------------------
    dbg.click(*button(dbg, "Wait"))
    dbg.settle()
    for _ in range(40):
        if not dialog_open(dbg):
            break
        time.sleep(0.15)
    res.check("Wait dismisses the dialog and keeps the window",
              not dialog_open(dbg) and has_window(dbg, HANG_TITLE))

    # And Wait must not be permanent. It was: the WM reported only the
    # TRANSITION into not-responding, so leaving the flag set meant no
    # later close attempt could ever offer the dialog again.
    dbg.send(ALT_F4)
    dbg.settle()
    res.check("asking again after Wait offers the dialog again", wait_dialog(dbg))

    # --- Force Quit ------------------------------------------------------
    b = button(dbg, "Force Quit")
    if b:
        dbg.click(*b)
        dbg.settle()
        for _ in range(60):
            if not has_window(dbg, HANG_TITLE):
                break
            time.sleep(0.15)
    # `wm_alive` first: a dead desktop reports NO windows, so the window
    # check would otherwise pass vacuously on precisely the failure the
    # next one is about.
    res.check("Force Quit removes the hung window",
              wm_alive(dbg) and not has_window(dbg, HANG_TITLE),
              f"titles: {titles(dbg)}")
    res.check("the desktop survives a force quit",
              gui_json(dbg, "gui state --json").get("screen", {}).get("w", 0) > 0)

    # --- repeatable ------------------------------------------------------
    #
    # The real assertion about scheduler_kill() + reaping: a killed
    # process must give its slot back. MAX_PROCS is 4, so a leak shows up
    # as a spawn that stops producing a window part-way through.
    ok_cycles = 0
    for _ in range(CYCLES):
        if not spawn(dbg, HANG_BIN, HANG_TITLE):
            break
        ok_cycles += 1
        dbg.send("gui key h")
        dbg.settle()
        # Wait for the client to SAY it hung, rather than sleeping at it.
        # The ordering is load-bearing: if 'h' has not landed, the Alt+F4
        # below closes the window normally and no dialog ever appears, so
        # a fixed sleep here is both slower than it needs to be and, on a
        # loaded machine, not long enough.
        hung_deadline = time.time() + 6.0
        while time.time() < hung_deadline:
            if any("hanging now" in l for l in dbg.logs("hangclient:", clear=True)):
                break
            time.sleep(0.15)
        dbg.send(ALT_F4)
        dbg.settle()
        if not wait_dialog(dbg):
            break
        fq = button(dbg, "Force Quit")
        if not fq:
            break
        dbg.click(*fq)
        dbg.settle()
        # Poll for the window actually going, instead of assuming 1.2s
        # is enough. Faster in the common case AND a stronger statement:
        # the next cycle's spawn depends on this one's slot having been
        # returned, so "the window is gone" is the precondition rather
        # than an incidental.
        gone_deadline = time.time() + 8.0
        while has_window(dbg, HANG_TITLE) and time.time() < gone_deadline:
            time.sleep(0.15)

    res.check(f"force quit is repeatable ({CYCLES} spawn/kill cycles)",
              ok_cycles == CYCLES,
              f"only {ok_cycles} of {CYCLES} cycles spawned -- a leaked "
              f"scheduler slot stops the desktop launching anything after 4")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "forcequit_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        # Shorten the WM's not-responding timeout for this run, and
        # ASSERT it took rather than assuming: if the command were
        # missing or refused, every wait below would silently be shorter
        # than the timeout it is waiting for, and the whole tool would
        # fail in a way that looks like the feature being broken.
        got = dbg.send(f"gui pingtimeout {PING_TIMEOUT_TICKS}")
        res.check("the ping timeout was shortened for this run",
                  f"{PING_TIMEOUT_TICKS} ticks" in (got or ""),
                  f"`gui pingtimeout` said {got!r}")
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nforcequit_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
