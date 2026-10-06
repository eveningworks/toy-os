#!/usr/bin/env python3
"""Pin to Places in the File Manager (lib/upins.h): a folder's context
menu pins it, the side column lists it under Pinned, a click there goes
to it, and Unpin takes it away -- each read back from /etc/places.conf
with `cat` as well as from the app.

1. **"Pin to Places" writes the folder to /etc/places.conf** and the
   side column gains a row for it.
2. **That row goes to the folder.**
3. **"Unpin from Places" takes it out of the file and the column.**

The context menu's rows are the ones the app logs (`ctxmenu.item`); a
folder's menu is Open, Open in new tab, then Pin (or Unpin) -- row 2.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

START = "/usr/share"
TARGET = "/usr/share/sounds"   # not a standard place: Documents IS /usr/share/doc
PIN_ROW = 2


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh("rm /etc/places.conf")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {START}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == START and l.rowy) or fm.layout_now(dbg, win)

    def pinned():
        return TARGET in [ln.strip() for ln in sh("cat /etc/places.conf").splitlines()]

    def menu_on_doc():
        """Right-click the target's row and wait for its menu. Typed in
        full: "soundfonts" shares the first five letters."""
        dbg.key(fm.K_HOME)
        for ch in "sounds":
            dbg.key(ch)
        l = fm.wait_layout(dbg, win, lambda l: l.selected == "sounds") or fm.layout_now(dbg, win)
        # The selected row's VIEW position: rows after ".." in name order.
        names = sorted(n.rstrip("/") for n in fm.listing(dbg, START))
        view_row = 1 + names.index("sounds")
        px, py, pw, ph = l.pane[0]
        fm.sure_rclick(dbg, qmp, l.ox + px + pw // 2,
                       l.oy + l.rowy[0] + l.rowh * view_row + l.rowh // 2)
        return fm.wait_layout(dbg, win, lambda l: l.ctx == 1 and PIN_ROW in l.popitems)

    def tree_row(l, path):
        rows = [r for r, p in sorted(l.treerow.items()) if p == path]
        return rows[0] if rows and l.treebox else None

    # 1. pin
    m = menu_on_doc()
    if m:
        x, y, w, h = m.popitems[PIN_ROW]
        fm.sure_click(dbg, qmp, m.ox + x + w // 2, m.oy + y + h // 2)
    end = time.time() + 10
    while time.time() < end and not pinned():
        time.sleep(0.3)
    lay = fm.wait_layout(dbg, win, lambda l: tree_row(l, TARGET) is not None) or fm.layout_now(dbg, win)
    row = tree_row(lay, TARGET)
    res.check("'Pin to Places' writes the folder to /etc/places.conf and adds a row",
              m is not None and pinned() and row is not None,
              f"menu={m is not None} file={pinned()} row={row} tree={lay.treerow}")

    # 2. the row goes there
    aim = None
    if row is not None:
        tx, ty, tw, th, trh, _ = lay.treebox
        aim = (lay.ox + tx + tw // 2, lay.oy + ty + row * trh + trh // 2)
        fm.sure_click(dbg, qmp, *aim)
    # Generous, and with the cached layout to fall back on: under TCG the
    # navigation's one frame can land before this wait starts listening,
    # and an idle window draws no other.
    lay = (fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == TARGET, timeout=25.0)
           or fm.layout_now(dbg, win) or fm.last_layout())
    trail = [ln.strip() for ln in fm._ALL_BUF if "tree" in ln and "layout tree.row " not in ln][-8:]
    res.check("the pinned row goes to the folder", lay is not None and lay.dir.get(0) == TARGET,
              f"dir={lay.dir.get(0) if lay else None!r} aim={aim} treebox={lay.treebox if lay else None} "
              f"treesel={lay.treesel if lay else None} app={trail}")

    # 3. unpin, from the folder's own menu in its parent
    dbg.key(fm.K_BACKSPACE)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == START, timeout=25.0)
    m = menu_on_doc()
    if m:
        x, y, w, h = m.popitems[PIN_ROW]
        fm.sure_click(dbg, qmp, m.ox + x + w // 2, m.oy + y + h // 2)
    end = time.time() + 10
    while time.time() < end and pinned():
        time.sleep(0.3)
    lay = (fm.wait_layout(dbg, win, lambda l: tree_row(l, TARGET) is None)
           or fm.layout_now(dbg, win) or fm.last_layout())
    res.check("'Unpin from Places' takes it out of the file and the column",
              m is not None and not pinned() and tree_row(lay, TARGET) is None,
              f"menu={m is not None} file={pinned()} row={tree_row(lay, TARGET)}")
    sh("rm /etc/places.conf")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "pins_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\npins_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
