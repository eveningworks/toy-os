#!/usr/bin/env python3
"""The Recycle Bin in the File Manager: Delete, Shift+Delete, the bin's
folder and Restore -- each checked against the disk through /bin/trash
and `ls`, a different reader from the app that did the work.

1. **Delete asks "Move ... to the Recycle Bin?" and moves the file there**
   -- gone from its folder AND listed by `trash list`; a delete that only
   unlinked would pass the first half.
2. **Shift+Delete deletes for good**: gone, and NOT in the bin.
3. **`trash:/` opens the bin as a folder**: the pane reports it, with one
   row per item and no "..".
4. **Restore on the bin's command bar puts the file back**, and the bin
   is empty after.
5. **Back leaves the bin for the folder before it.**
6. **The desktop's Delete moves an icon's file to the bin** (the dialog's
   button says "Move"), and its Shift+Delete deletes for good.

Shares filemanager_test.py's layout reader; the fixture is /tg.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/tg"
K_DELETE = "0xf788"


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    def names(path):
        return fm.listing(dbg, path)

    def binned():
        # The name column is 24 wide and a name may hold a space.
        return [ln[:24].strip() for ln in sh("trash list").splitlines()
                if ln.strip() and "empty" not in ln and "exit" not in ln
                and not ln.startswith(("sh", "trash"))]

    def wait(pred, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.4)
        return False

    sh("trash empty")

    # 6. the desktop, first -- before a window covers its icons
    def desk_delete(name, mods):
        sh(f"touch /home/desktop/{name}")
        fi, end = None, time.time() + 10
        while fi is None and time.time() < end:   # the desktop polls its folder
            icons = dbg.json("gui icons --json") or {}
            fi = next((i for i in icons.get("icons", []) if i["name"] == name), None)
            if fi is None:
                dbg.settle()
        if not fi:
            return None, f"{name} never became an icon"
        dbg.send(f"gui click {fi['x'] + fi['w'] // 2} {fi['y'] + fi['w'] // 2}")
        dbg.settle()
        dbg.key(K_DELETE, mods=mods)
        dbg.settle()
        btns = (dbg.json("gui dialog --json") or {}).get("buttons", [])
        if len(btns) != 2:
            return None, f"dialog buttons {btns}"
        dbg.send(f"gui click {btns[0]['cx']} {btns[0]['cy']}")
        dbg.settle()
        return btns[0]["label"], ""

    label, why = desk_delete("tg-desk.txt", "")
    moved = wait(lambda: "tg-desk.txt" not in names("/home/desktop") and "tg-desk.txt" in binned())
    res.check("the desktop's Delete asks 'Move' and moves the file to the bin",
              label == "Move" and moved, f"button={label!r} {why} bin={binned()}")
    label, why = desk_delete("tg-gone.txt", "shift")
    gone = wait(lambda: "tg-gone.txt" not in names("/home/desktop"))
    res.check("the desktop's Shift+Delete deletes for good",
              label == "Delete" and gone and "tg-gone.txt" not in binned(),
              f"button={label!r} {why} bin={binned()}")
    sh("trash empty")

    sh(f"rm -r {ROOT}")
    sh(f"mkdir {ROOT}")
    sh(f"touch {ROOT}/keep.txt")
    sh(f"touch {ROOT}/kept.txt")
    sh(f"touch {ROOT}/gone.txt")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "confirm_delete=1", "left_view=details"])

    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the fixture", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT) or fm.layout_now(dbg, win)

    def select(name):
        dbg.key(fm.K_HOME)
        for ch in name[:3]:
            dbg.key(ch)
        return fm.wait_layout(dbg, win, lambda l: l.selected == name) or fm.layout_now(dbg, win)

    # 1. Delete -> the bin
    lay = select("keep.txt")
    dbg.key(K_DELETE)
    lay = fm.wait_layout(dbg, win, lambda l: l.dialog == 1) or lay
    asked = lay.dialog == 1
    dbg.key(fm.K_ENTER)
    moved = wait(lambda: "keep.txt" not in names(ROOT) and "keep.txt" in binned())
    res.check("Delete asks, then moves the file to the Recycle Bin",
              asked and moved, f"dialog={asked} dir={names(ROOT)} bin={binned()}")

    # 2. Shift+Delete -> gone for good
    lay = select("gone.txt")
    dbg.key(K_DELETE, mods="shift")
    lay = fm.wait_layout(dbg, win, lambda l: l.dialog == 1) or lay
    dbg.key(fm.K_ENTER)
    gone = wait(lambda: "gone.txt" not in names(ROOT))
    res.check("Shift+Delete deletes for good: gone, and not in the bin",
              gone and "gone.txt" not in binned(), f"dir={names(ROOT)} bin={binned()}")

    # 3. the bin as a folder
    dbg.key(fm.K_CTRL_L)
    fm.wait_layout(dbg, win, lambda l: l.pathedit == 1)
    for ch in "trash:/":
        dbg.key("0x2f" if ch == "/" else ch)
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "trash:/") or fm.layout_now(dbg, win)
    res.check("trash:/ opens the bin: one row per item, no '..'",
              lay.dir.get(0) == "trash:/" and lay.rows.get(0) == 1,
              f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")

    # 4. Restore, from the bin's own command bar (its first item)
    lay = select("keep.txt")
    lay = fm.wait_layout(dbg, win, lambda l: 0 in l.tbitems) or lay
    r = lay.tbitems.get(0)
    if r:
        fm.sure_click(dbg, qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
    back = wait(lambda: "keep.txt" in names(ROOT))
    lay = fm.wait_layout(dbg, win, lambda l: l.rows.get(0) == 0) or fm.layout_now(dbg, win)
    res.check("Restore puts the file back where it was, and the bin is empty after",
              r is not None and back and binned() == [] and lay.rows.get(0) == 0,
              f"button={r} dir={names(ROOT)} bin={binned()} rows={lay.rows.get(0)}")

    # 5. Back leaves the bin
    dbg.key(fm.K_LEFT, mods="alt")
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT) or fm.layout_now(dbg, win)
    res.check("Back leaves the bin for the folder before it",
              lay.dir.get(0) == ROOT, f"dir={lay.dir.get(0)!r}")

    sh(f"rm -r {ROOT}")
    sh("trash empty")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "trash_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\ntrash_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
