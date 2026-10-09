#!/usr/bin/env python3
"""tools/osk_test.py -- the on-screen keyboard (userland/wm/osk.c).

WHAT IS UNDER TEST
------------------
A tray toggle opens a panel of keycaps; clicking one types into the
FOCUSED client, encoded the way the physical keyboard encodes it.

THE LOAD-BEARING CHECK IS A ROUND TRIP THROUGH THE FILESYSTEM, not ink
on a screen. Typing into Notepad and watching the pixel count rise is
exactly the "it responds, therefore it works" trap CLAUDE.md names: a
blinking caret moves those pixels too. So this types `mkdir /<name>`
into the Terminal and then asks the SHELL whether the directory exists
-- one assertion that covers the keycap hit-test, wm_client_send_key(),
the client, the line editor and the filesystem, and that a broken
version cannot pass.

THE TWO ENCODING BRANCHES GET DISCRIMINATING TESTS, because both fail
SILENTLY:

  * Shift -- `mkdir /Zz` proves the shifted cap AND that the sticky
    modifier is consumed by exactly one key (a modifier that stuck
    would give `ZZ`, one that never armed `zz`).
  * Ctrl  -- type `xyz`, then Ctrl-C, then a real command. Ctrl-C
    discards the line, so the directory appears. If Ctrl were sent as
    'c' plus KEY_MOD_CTRL -- which is what a naive OSK does, and what
    api/keyboard.h warns against -- a literal 'c' would land in the
    line, the command would be `xyzcmkdir /...`, and nothing would be
    created. That is the whole reason the check is shaped this way.

WARP THE CURSOR, NEVER `click_at` ALONE. The WM ACCELERATES an injected
delta, so an open-loop move misses a 42x40 tray item; that cost this
tool's author several "the click does nothing" readings that were the
harness, not the keyboard. DebugConsole.warp_cursor() confirms the
position against the WM before clicking.

    python3 tools/osk_test.py [--instance N]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gui_debug  # noqa: E402
import port_guard  # noqa: E402
from gui_debug import DebugConsole  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from harness import copy_disk  # noqa: E402

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'ok   ' if ok else 'FAIL '} {name}" + (f"  -- {detail}" if detail else ""))
    return bool(ok)


class Keyboard:
    """Clicks keycaps. Every position comes from `gui osk`, never from
    arithmetic on the panel rect -- the panel is font-derived, so a
    second layout implementation here would drift from osk.c's."""

    def __init__(self, dbg, qmp):
        self.d, self.q = dbg, qmp

    def _centre(self, text):
        m = re.search(r"centre=\((\d+),(\d+)\)", text)
        return (int(m.group(1)), int(m.group(2))) if m else None

    def tray(self):
        out = self.d.send("gui osk") or ""
        return self._centre(out.split("tray")[1]) if "tray" in out else None

    def key(self, cap):
        return self._centre(self.d.send(f"gui osk key {cap}") or "")

    def click(self, x, y, settle=0.22):
        self.d.warp_cursor(self.q, x, y)
        self.q.click_at(x, y)
        time.sleep(settle)

    def press(self, cap):
        p = self.key(cap)
        if p:
            self.click(*p)
        return p is not None

    def type_caps(self, caps):
        for c in caps:
            self.press(c)

    def is_open(self):
        return "osk: open" in (self.d.send("gui osk") or "")

    def mods(self):
        m = re.search(r"mods=0x([0-9a-f]+)", self.d.send("gui osk") or "")
        return int(m.group(1), 16) if m else -1


def spell(word):
    """`mkdir /<word>` as a list of keycap labels."""
    return list("mkdir") + ["Space", "/"] + list(word) + ["Enter"]


def listing(dbg):
    dbg.logs()          # drain app logs, or they interleave with the reply
    time.sleep(0.2)
    return dbg.send("sh ls /") or ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", default="auto",
                    help="VM slot, or `auto` to take the lowest free one")
    args = ap.parse_args()
    n = (port_guard.find_free_instance() if args.instance == "auto"
         else int(args.instance))
    if n is None:
        print("osk_test: no free VM slot")
        return 2

    img = os.path.join(tempfile.gettempdir(), f"osk_test_{n}.img")
    copy_disk("disk.img", img, cwd=REPO)
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                    "--instance", str(n), "stop"], capture_output=True)
    boot = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                           "--instance", str(n), "--disk", img, "start"], cwd=REPO)
    print("osk_test: the on-screen keyboard")
    if not check("the guest boots", boot.returncode == 0):
        return 1

    try:
        qmp = QMPSession(port=port_guard.instance_qmp(n))
        sock = port_guard.instance_sock(n)
        gui_debug.enter_gui(qmp, sock=sock)
        dbg = DebugConsole(sock)
        kb = Keyboard(dbg, qmp)

        check("the tray offers a keyboard item", kb.tray() is not None,
              str(kb.tray()))
        check("...and the panel starts closed", not kb.is_open())

        tray = kb.tray()
        if tray:
            kb.click(*tray, settle=0.4)
        if not check("clicking the tray item opens it", kb.is_open()):
            return 1

        dbg.open_app("Terminal")
        dbg.settle()
        time.sleep(1.0)
        check("the keyboard survives an app opening over it", kb.is_open())

        before = listing(dbg)
        check("the fixture is clean", "oskok/" not in before)

        # --- THE ROUND TRIP ------------------------------------------
        kb.type_caps(spell("oskok"))
        time.sleep(1.2)
        after = listing(dbg)
        check("typing a command creates the directory it names",
              "oskok/" in after, "letters, Space and Enter all arrived")

        # --- SHIFT ----------------------------------------------------
        kb.press("Shift")
        check("Shift arms as a sticky modifier", kb.mods() == 0x1,
              f"mods=0x{kb.mods():x}")
        kb.type_caps(["z"])
        check("...and one key consumes it", kb.mods() == 0, f"mods=0x{kb.mods():x}")

        # That 'Z' went into the Terminal's line, so clear it before the
        # next command -- a check that leaves state behind makes the NEXT
        # one fail for a reason that has nothing to do with what it tests
        # (this one read as "Shift is broken" until the stray Z was found).
        kb.type_caps(["Ctrl", "c"])
        time.sleep(0.4)

        kb.type_caps(list("mkdir") + ["Space", "/", "Shift", "z", "z", "Enter"])
        time.sleep(1.2)
        after = listing(dbg)
        check("Shift types the shifted cap, and only for one key",
              "Zz/" in after, "wanted Zz/ -- ZZ/ means it stuck, zz/ that it never armed")

        # --- CTRL: the discriminating one ----------------------------
        kb.type_caps(list("xyz") + ["Ctrl", "c"])
        time.sleep(0.6)
        kb.type_caps(spell("ctrlok"))
        time.sleep(1.2)
        after = listing(dbg)
        check("Ctrl folds to a control code, so Ctrl-C discards the line",
              "ctrlok/" in after,
              "a literal 'c' would have made the command xyzcmkdir")

        # --- THE CONFIGURED LAYOUT: Finnish, typed through ring 3's
        # copy of the kernel's translator. AltGr+2 is @ there and Shift+7
        # is /, so a panel still typing US produces neither directory.
        k = dbg.json("gui osk --json")
        check("the bar names the layout", k.get("layout") == "English (US)",
              f"layout={k.get('layout')!r}")
        dbg.send("sh config set system.keyboard_layout fi")
        dbg.settle()
        time.sleep(0.6)
        k = dbg.json("gui osk --json")
        check("...and follows the setting to Finnish", k.get("layout") == "Finnish",
              f"layout={k.get('layout')!r}")
        fi_slash = ["Space", "Shift", "7"]
        kb.type_caps(list("mkdir") + fi_slash + ["a", "AltGr", "2", "b", "Enter"])
        time.sleep(1.2)
        after = listing(dbg)
        check("AltGr types the layout's third level", "a@b/" in after,
              "wanted a@b/ -- a2b/ means AltGr typed the base, no directory that / was US")

        # THE DEAD KEY: acute, then e, is ONE character. A panel that never
        # composed types the accent and the e (two characters), and one
        # that swallowed the dead key types a bare e -- neither is one
        # non-ASCII byte between x and y. (The listing is UTF-8-decoded,
        # so a lone Latin-1 byte reads as U+FFFD.)
        kb.type_caps(list("mkdir") + fi_slash + ["x", "kc13"])
        k = dbg.json("gui osk --json")
        check("a dead key latches until its letter", k.get("pending") == 13,
              f"pending={k.get('pending')}")
        kb.type_caps(["e", "y", "Enter"])
        time.sleep(1.2)
        after = listing(dbg)
        check("dead acute then e composes to one character", "x\ufffdy/" in after,
              "x\ufffdey/ means it never composed, xey/ that the accent was lost")
        dbg.send("sh config set system.keyboard_layout us")
        dbg.settle()
        time.sleep(0.6)

        # --- FLOATING: dragged by its bar, docked, closed by its x ---
        k = dbg.json("gui osk --json")
        check("it opens floating, narrower than the screen", not k["docked"] and k["w"] < 1280,
              f"w={k['w']} docked={k['docked']}")
        sx, sy = k["x"] + 60, k["y"] + k["bar_h"] // 2
        dbg.send(f"gui drag {sx} {sy} {sx - 80} {sy - 120} 8")
        dbg.settle()
        time.sleep(0.4)
        m = dbg.json("gui osk --json")
        check("dragging its bar moves it", (m["x"], m["y"]) == (k["x"] - 80, k["y"] - 120),
              f"{(k['x'], k['y'])} -> {(m['x'], m['y'])}")
        kb.type_caps(spell("moved"))
        time.sleep(1.2)
        check("...and a moved keyboard still types", "moved/" in listing(dbg))
        kb.click(m["dock"]["cx"], m["dock"]["cy"], settle=0.4)
        m = dbg.json("gui osk --json")
        check("Dock makes it full width above the taskbar", m["docked"] and m["w"] >= 1280,
              f"w={m['w']} docked={m['docked']}")
        kb.click(m["dock"]["cx"], m["dock"]["cy"], settle=0.4)
        check("...and the same button floats it again", not dbg.json("gui osk --json")["docked"])
        m = dbg.json("gui osk --json")
        kb.click(m["close"]["cx"], m["close"]["cy"], settle=0.4)
        check("its close button closes it", not kb.is_open())
        tray = kb.tray()
        if tray:
            kb.click(*tray, settle=0.4)
        check("...and the tray item opens it again", kb.is_open())

        # --- the toggle closes it again ------------------------------
        tray = kb.tray()
        if tray:
            kb.click(*tray, settle=0.4)
        check("clicking the tray item again closes it", not kb.is_open())

        dbg.close()
    finally:
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nosk_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
