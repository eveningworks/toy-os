#!/usr/bin/env python3
"""Clipboard keys and the shared right-click edit menu, in a text field.

Every `uui_textbox` (and every text widget answering
uui_widget_ops.edit_target) gets Ctrl+X/C/V, the CUA trio and a
right-click menu from the TOOLKIT, with no app code (ui/uui_editmenu.h).
This drives UI Demo's field, which declares nothing about any of it, and
reads the field back from the app's OWN report (`uidemo: key N
text="..."`, `uidemo: edit text="..."`) -- the buffer, not pixels.

  1. Keys: Ctrl+A Ctrl+C, End, Ctrl+V doubles the text; Ctrl+A Ctrl+X
     empties it; Ctrl+V brings it back. Then the CUA trio: Shift+Delete
     cuts, Shift+Insert pastes, Ctrl+Insert copies.
  2. The menu: a right-click on the field opens it (the app's layout log
     reports `editmenu.open 1` with its rows), Select All then Delete
     from it empties the field, Paste refills it, Undo takes the paste
     back -- each one reported by the app as an `edit` it was told about.
  3. The Menu key opens it at the focused field of an app with a focus
     ring (Help's search), and Esc closes it.

Positive controls (mutate.py): break uui_edit.c's Ctrl-V case and the
key checks go red; break uui_editmenu.c's ED_PASTE row and the menu's.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/editmenu_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402

UIDEMO = "/bin/wm/demos/uidemo"
HELP = "/bin/wm/apps/help"

CTRL_A, CTRL_C, CTRL_V, CTRL_X = "0x01", "0x03", "0x16", "0x18"
K_END, K_INSERT, K_DELETE = "0xf787", "0xf882", "0xf788"
K_MENU, K_ESC = "0xf883", "0x1b"

# ui/uui_editmenu.c's g_rows, by INDEX -- separators count.
ROW = {"Undo": 0, "Redo": 1, "Cut": 3, "Copy": 4, "Paste": 5, "Delete": 6, "Select All": 8}
START = "type here"   # uidemo.c's initial text


class T:
    def __init__(self, dbg):
        self.dbg = dbg
        self.passes, self.fails = [], []

    def check(self, ok, what, detail=""):
        (self.passes if ok else self.fails).append(what)
        print(("  ok   " if ok else "  FAIL ") + what + ("" if ok else f"  [{detail}]"))
        return ok


def last_text(lines, kind=("key", "edit")):
    """The field's text in the newest `key ... text=` / `edit text=` line."""
    got = None
    for ln in lines:
        m = re.search(r'uidemo: (key \d+|edit) text="(.*)"$', ln)
        if m and m.group(1).split()[0] in kind:
            got = m.group(2)
    return got


def menu_rows(lines, prefix):
    """{index: (x, y, w, h)} of the edit menu's level-0 rows, and open."""
    rows, is_open = {}, False
    for ln in lines:
        m = re.search(prefix + r": layout editmenu\.item 0 (\d+) (-?\d+) (-?\d+) (\d+) (\d+)$", ln)
        if m:
            rows[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
        if re.search(prefix + r": layout editmenu\.open 1$", ln):
            is_open = True
    return rows, is_open


def wait_lines(dbg, prefix, ok, timeout=4.0):
    acc = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        acc += dbg.logs(prefix + ":", clear=True)
        if ok(acc):
            break
        time.sleep(0.2)
    return acc


def key(dbg, k, mods=""):
    dbg.send(f"gui key {k} {mods}".rstrip())
    dbg.settle()


def open_menu(t, win, cx, cy, prefix, nrows=9):
    """Right-click (cx, cy) in content coordinates; the menu's rows."""
    t.dbg.logs(prefix + ":", clear=True)
    c = win["content"]
    t.dbg.rclick(c["x"] + cx, c["y"] + cy)
    lines = wait_lines(t.dbg, prefix,
                       lambda a: menu_rows(a, prefix)[1] and len(menu_rows(a, prefix)[0]) >= nrows)
    return menu_rows(lines, prefix)


def pick(t, win, rows, label, prefix, row=None):
    """Click a row; the app's report of what the field says after."""
    x, y, w, h = rows[ROW[label] if row is None else row]
    c = win["content"]
    t.dbg.logs(prefix + ":", clear=True)
    t.dbg.click(c["x"] + x + w // 2, c["y"] + y + h // 2)
    return wait_lines(t.dbg, prefix, lambda a: last_text(a, ("edit",)) is not None, 3.0)


def run_keys(t, win, field):
    c = win["content"]
    fx, fy = c["x"] + field["x"] + field["w"] // 2, c["y"] + field["y"] + field["h"] // 2
    t.dbg.click(fx, fy)
    t.dbg.logs("uidemo:", clear=True)

    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, CTRL_C, "ctrl")
    key(t.dbg, K_END)
    key(t.dbg, CTRL_V, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == START + START, "Ctrl+A Ctrl+C, End, Ctrl+V doubles the text", got)

    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, CTRL_X, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == "", "Ctrl+A Ctrl+X empties the field", got)

    key(t.dbg, CTRL_V, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == START + START, "Ctrl+V puts back what Ctrl+X took", got)

    # The CUA trio, on a known-different clipboard: copy only "type".
    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, K_DELETE, "shift")                      # Shift+Delete = cut
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == "", "Shift+Delete cuts", got)
    key(t.dbg, K_INSERT, "shift")                      # Shift+Insert = paste
    key(t.dbg, K_INSERT, "shift")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == START * 4, "Shift+Insert pastes (twice: four copies)", got)
    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, K_INSERT, "ctrl")                       # Ctrl+Insert = copy
    key(t.dbg, K_END)
    key(t.dbg, CTRL_V, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == START * 8, "Ctrl+Insert copies (pasted after: eight copies)", got)

    # Back to one copy on the clipboard and in the field, for the menu.
    key(t.dbg, CTRL_A, "ctrl")
    for ch in START:
        key(t.dbg, "0x20" if ch == " " else ch)
    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, CTRL_C, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got == START, "retyped the starting text", got)


def run_menu(t, win, field):
    cx, cy = field["x"] + field["w"] // 2, field["y"] + field["h"] // 2
    rows, is_open = open_menu(t, win, cx, cy, "uidemo")
    if not t.check(is_open and all(i in rows for i in ROW.values()),
                   "a right-click on the field opens the edit menu, every row reported",
                   f"open={is_open} rows={sorted(rows)}"):
        return
    m = rows[ROW["Undo"]]
    t.check(m[0] >= 0 and m[1] >= 0, "the menu opens inside the window, at the pointer", m)

    got = last_text(pick(t, win, rows, "Select All", "uidemo"), ("edit",))
    # Select All changes no text, so the app hears nothing: no edit line.
    t.check(got is None, "Select All changes no text (the app is told nothing)", got)

    rows, _ = open_menu(t, win, cx, cy, "uidemo")
    got = last_text(pick(t, win, rows, "Delete", "uidemo"), ("edit",))
    t.check(got == "", "Delete from the menu removes the selection", got)

    rows, _ = open_menu(t, win, cx, cy, "uidemo")
    got = last_text(pick(t, win, rows, "Paste", "uidemo"), ("edit",))
    t.check(got == START, "Paste from the menu inserts the clipboard", got)

    rows, _ = open_menu(t, win, cx, cy, "uidemo")
    got = last_text(pick(t, win, rows, "Undo", "uidemo"), ("edit",))
    t.check(got == "", "Undo from the menu takes the paste back in one step", got)

    # Closed after a commit: a fresh layout report carries no menu.
    t.dbg.logs("uidemo:", clear=True)
    t.dbg.click(win["content"]["x"] + cx, win["content"]["y"] + cy)
    lines = wait_lines(t.dbg, "uidemo", lambda a: any("layout textbox" in ln for ln in a), 3.0)
    t.check(not menu_rows(lines, "uidemo")[1], "the menu is closed after a row ran")


def run_readonly(t, win, field):
    """The scrollback is a READ-ONLY uui_textview: Copy and Select All
    only, and what it copies pastes into the one-line field as its FIRST
    LINE (uui_edit_paste()'s single-line rule)."""
    view = t.dbg.widgets("UI Demo").get("scrollback")
    if not t.check(view is not None, "UI Demo reports its scrollback"):
        return
    vx, vy = view["x"] + 40, view["y"] + 10
    rows, is_open = open_menu(t, win, vx, vy, "uidemo", nrows=3)
    t.check(is_open and sorted(rows) == [0, 1, 2],
            "read-only text gets a three-row menu (Copy, a separator, Select All)",
            f"open={is_open} rows={sorted(rows)}")
    if not is_open:
        return
    pick(t, win, rows, "Select All", "uidemo", row=2)
    rows, _ = open_menu(t, win, vx, vy, "uidemo", nrows=3)
    pick(t, win, rows, "Copy", "uidemo", row=0)

    c = win["content"]
    t.dbg.click(c["x"] + field["x"] + field["w"] // 2, c["y"] + field["y"] + field["h"] // 2)
    t.dbg.logs("uidemo:", clear=True)
    key(t.dbg, CTRL_A, "ctrl")
    key(t.dbg, CTRL_V, "ctrl")
    got = last_text(t.dbg.logs("uidemo:", clear=True))
    t.check(got is not None and re.fullmatch(r"line \d+ of 20", got or "") is not None,
            "copied from the read-only view; a one-line field pastes its first line", got)


def run_menu_key(t):
    t.dbg.spawn(HELP, "Help")
    win = t.dbg.window_settled("Help")
    if not t.check(win is not None, "Help opens"):
        return
    names = t.dbg.widgets("Help")
    field = next((w for n, w in names.items() if "search" in n), None)
    if not t.check(field is not None, "Help reports its search field", sorted(names)):
        return
    t.dbg.click(field["screen"]["x"] + field["w"] // 2, field["screen"]["y"] + field["h"] // 2)
    t.dbg.logs("help:", clear=True)
    key(t.dbg, K_MENU)
    lines = wait_lines(t.dbg, "help", lambda a: menu_rows(a, "help")[1])
    rows, is_open = menu_rows(lines, "help")
    t.check(is_open, "the Menu key opens the edit menu on the focused field")
    if is_open and ROW["Undo"] in rows:
        x, y, w, h = rows[ROW["Undo"]]
        fy = field["y"] + field["h"]
        t.check(abs(y - fy) <= h, "...under the field, at its caret", (y, fy))
    t.dbg.logs("help:", clear=True)
    key(t.dbg, K_ESC)
    key(t.dbg, "x")   # typed into the field only once the menu is gone
    lines = wait_lines(t.dbg, "help", lambda a: any("layout" in ln for ln in a), 2.0)
    t.check(not menu_rows(lines, "help")[1], "Esc closes it")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "editmenu_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    t = T(dbg)

    dbg.spawn(UIDEMO, "UI Demo")
    win = dbg.window_settled("UI Demo")
    if not t.check(win is not None, "UI Demo opens"):
        return 1
    field = dbg.widgets("UI Demo").get("textbox")
    if not t.check(field is not None, "UI Demo reports its text field"):
        return 1

    print("== keys ==")
    run_keys(t, win, field)
    print("== right-click menu ==")
    run_menu(t, win, field)
    print("== read-only text ==")
    run_readonly(t, win, field)
    print("== Menu key ==")
    run_menu_key(t)

    print(f"\neditmenu_test: {len(t.passes)} passed, {len(t.fails)} failed")
    for f in t.fails:
        print("  FAILED:", f)
    dbg.close()
    return 1 if t.fails else 0


if __name__ == "__main__":
    sys.exit(main())
