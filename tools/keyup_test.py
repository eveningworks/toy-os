#!/usr/bin/env python3
"""tools/keyup_test.py -- key RELEASES reach a ring-3 client, end to end.

WHAT THIS COVERS
----------------
`WIN_EV_KEY` was press-only, so a client could know a key had been
STRUCK and never that it was HELD. That is the whole input model of a
game, and three of Doom's five stock controls (Ctrl to fire, Shift to
run, Alt to strafe) are modifier keys that produce no character at all
and so reached a client by no path whatsoever.

The chain under test is five layers deep and every one of them is new
here: the keyboard driver's transition queue (`kernel/drivers/input/
keyboard.c`), the kernel's raw-event push (`kernel/proc/win_input.c`),
the compositor's raw-input queue (`userland/wm/wm_rawin.c`), the WM's
routing (`userland/wm/wm.c`), and Toykit's `on_key_up`
(`userland/ui/uapp.c`). A test that only proved "a keystroke arrives"
would pass on the OLD kernel.

WHY IT DRIVES winclient
-----------------------
`userland/tests/winclient.c` is this tree's protocol-edge-case client --
it is where the `on_close` veto lives, for the same reason -- and it is
the only thing here that keeps a model of what is currently HELD. Every
real app acts on the press, because every real app edits text or clicks
buttons.

THE ASSERTION THAT MATTERS
--------------------------
Check 2. `QMPSession.send_key()` uses QMP's `send-key`, which presses
and releases in one go, so a test built on it cannot tell a working
release path from a guest that invented the release itself. `key_down()`
leaves the key physically down: the client must still report it held
several frames later, and must report it up only when `key_up()` is
sent. Nothing press-only can pass that.

Check 4 is the correctness property X11 and Wayland get by delivering
physical keycodes: the release must carry what the PRESS produced, so a
key pressed as 'w' and released after Shift went down still clears 'w'.
A client that saw 'w' go down and 'W' come up holds 'w' forever.

POSITIVE CONTROL
----------------
Run with --control to have the tool tell you what to break. The cheap
one: in `userland/ui/uapp.c`'s `WIN_EV_KEY_UP` arm, return without
calling `on_key_up`. MEASURED, not predicted: 5 pass and 5 fail -- 1b,
2c, 3b, 4 and 5 go red while 1a, 2a and 3a stay green, which is exactly
the shape of a press-only system. Check 5 reports "still holds 5 keys",
which is what a stuck key looks like from inside the client.

**AND ONE CHECK PASSES VACUOUSLY UNDER THAT CONTROL: 2b.** It asserts
that no release arrives while the key is down, and a build that delivers
no releases at all satisfies it perfectly. That is not a flaw to fix --
2b exists to catch a release SYNTHESISED from a timeout, which is a
different failure -- but it means 2b is only worth anything paired with
2c, and a future edit that drops 2c leaves a check that cannot fail.
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
TITLE = "Ring 3 Client"
SPAWN_PATH = "/tests/winclient"
SPAWN_TIMEOUT_S = 15.0

# api/keyboard.h. The modifier codes exist ONLY on the transition path.
KEY_CTRL = 0xA8
KEY_SHIFT = 0xA7


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def transitions(dbg):
    """winclient's held-set changes since the last call, as (what, code)."""
    out = []
    for line in dbg.logs("winclient: key"):
        parts = line.split()
        # "winclient: keydown 119 held=1"
        for i, p in enumerate(parts):
            if p in ("keydown", "keyup", "keyup-unmatched") and i + 1 < len(parts):
                try:
                    out.append((p, int(parts[i + 1])))
                except ValueError:
                    pass
                break
    return out


def settle_input(dbg, seconds=0.6):
    """Let the WM pump and the client run. Not a screenshot comparison, so
    there is nothing to make stable -- this waits for a LOG to arrive,
    and dbg.settle() is what pumps the console."""
    dbg.settle()
    time.sleep(seconds)
    dbg.settle()


def run(dbg, qmp, res):
    dbg.send(f"gui spawn {SPAWN_PATH}")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win:
            break
        time.sleep(0.3)
    res.check("winclient runs as a ring-3 client with its own window", win is not None,
              f"no window titled {TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    # Focus it explicitly rather than assuming the spawn left it focused:
    # keys go to the FOCUSED window, so an unfocused client would report
    # nothing and every check below would fail for the wrong reason.
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] - 20)
    settle_input(dbg)
    transitions(dbg)   # discard anything the focusing click produced

    # --- 1. a struck key reports BOTH edges ---------------------------
    qmp.send_key("w")
    settle_input(dbg)
    t = transitions(dbg)
    res.check("1a. a struck key reports a press", ("keydown", ord("w")) in t, str(t))
    res.check("1b. a struck key reports a release", ("keyup", ord("w")) in t, str(t))

    # --- 2. THE ONE THAT MATTERS: a held key stays held ---------------
    #
    # Down only. The client must still believe it is down after several
    # frames, and must NOT report a release until one is sent. A press-
    # only system cannot pass 2b, and a system that synthesises releases
    # from a timeout cannot pass 2a.
    qmp.key_down("a")
    settle_input(dbg, 1.2)
    t = transitions(dbg)
    res.check("2a. a held key reports a press", ("keydown", ord("a")) in t, str(t))
    res.check("2b. a held key reports NO release while it is down",
              ("keyup", ord("a")) not in t,
              f"a release arrived without one being sent: {t}")

    qmp.key_up("a")
    settle_input(dbg)
    t = transitions(dbg)
    res.check("2c. releasing it reports the release", ("keyup", ord("a")) in t, str(t))

    # --- 3. a MODIFIER reaches the client at all ----------------------
    #
    # Ctrl produces no character, so before the transition queue it
    # reached a client by no path at all -- it was only ever a bit riding
    # with some other key. This is Doom's fire button.
    qmp.key_down("ctrl")
    settle_input(dbg)
    t = transitions(dbg)
    res.check("3a. a modifier press reaches the client",
              ("keydown", KEY_CTRL) in t,
              f"no KEY_CTRL press; the byte stream cannot carry one: {t}")

    qmp.key_up("ctrl")
    settle_input(dbg)
    t = transitions(dbg)
    res.check("3b. a modifier release reaches the client",
              ("keyup", KEY_CTRL) in t, str(t))

    # --- 4. the release carries what the PRESS produced ---------------
    #
    # Press w, then Shift, then release w. The release must say 'w' (119)
    # and not 'W' (87): the client watched 'w' go down, and clearing 'W'
    # would leave it holding 'w' for the rest of the session.
    qmp.key_down("w")
    settle_input(dbg)
    transitions(dbg)          # discard the press itself

    qmp.key_down("shift")
    settle_input(dbg)
    qmp.key_up("w")
    settle_input(dbg)
    t = transitions(dbg)
    qmp.key_up("shift")
    settle_input(dbg)

    res.check("4. a release carries the code its PRESS produced, not the shifted one",
              ("keyup", ord("w")) in t and ("keyup", ord("W")) not in t,
              f"expected a release of 'w' (119), got {t}")

    # --- 4b. THE KEYS THAT USED TO REPORT NOTHING ---------------------
    #
    # Insert, the Menu key, the locks and the whole numeric keypad were
    # silently dropped: no layout entry, so keyboard_layout_translate()
    # returned 0 and the key was indistinguishable from one nobody
    # pressed. An app cannot bind what it never sees.
    #
    # Checked through the SAME held-set log as everything above, so this
    # is not a special path -- if a key arrives at all, winclient reports
    # it going down.
    for qcode, want, name in (
        ("kp_7", ord("7"), "keypad 7 types a 7"),
        ("kp_enter", ord("\n"), "keypad Enter is the same newline as Enter"),
        ("f6", 0xAD, "F6 (quicksave in Doom) reports a code"),
        ("insert", 0xB3, "Insert reports a code"),
    ):
        qmp.send_key(qcode)
        settle_input(dbg)
        t = transitions(dbg)
        res.check(f"4b. {name}", ("keydown", want) in t,
                  f"expected a press of {want}, got {t}")

    # --- 5. the held set comes back to empty --------------------------
    #
    # The end state, which is what a stuck key actually looks like. Read
    # from the client's own held= counter rather than inferred from the
    # transitions above, so a miscounted set cannot hide behind a
    # correct-looking sequence.
    qmp.send_key("z")
    settle_input(dbg)
    lines = dbg.logs("winclient: key")
    last_held = None
    for line in lines:
        for p in line.split():
            if p.startswith("held="):
                try:
                    last_held = int(p[5:])
                except ValueError:
                    pass
    res.check("5. every key that went down has come back up",
              last_held == 0,
              f"winclient still holds {last_held} key(s) -- see the log above")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--control", action="store_true",
                    help="print the positive control and exit")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "keyup_test")

    if args.control:
        print(__doc__.split("POSITIVE CONTROL")[1].strip())
        return 0

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nkeyup_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
