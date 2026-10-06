#!/usr/bin/env python3
"""Undo and Redo in the File Manager (lib/ufileundo.h), each checked
against the disk through `ls` and /bin/trash -- a different reader from
the app that did the work.

1. **Delete offers Undo on a toast**, and the toast's own button puts the
   file back; the toast then offers Redo.
2. **Ctrl+Y redoes it** (the file goes to the bin again) **and Ctrl+Z
   undoes it** -- the keys and the button are one journal.
3. **A rename is undone** by Ctrl+Z: the old name is back.
4. **A new folder is undone into the Recycle Bin**, never deleted for
   good: it leaves the folder and is in the bin.

Shares filemanager_test.py's layout reader; the fixture is /ug.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/ug"
K_DELETE = "0xf788"
K_CTRL_Z, K_CTRL_Y = "0x1a", "0x19"


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

    def toast():
        """The last toast line: (shown, button rect, action) or None."""
        fm._collect(dbg)
        for ln in reversed(fm._ALL_BUF):
            if "files: layout toast" in ln:
                p = ln.split("files: layout toast", 1)[1].split()
                return int(p[4]), [int(v) for v in p[5:9]], p[9]
        return None

    sh("trash empty")
    sh(f"rm -r {ROOT}")
    sh(f"mkdir {ROOT}")
    sh(f"touch {ROOT}/apple.txt")
    sh(f"touch {ROOT}/berry.txt")
    # No confirmation, so Delete acts at once and the toast is the answer.
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "confirm_delete=0",
                                    "left_view=details"])
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

    # 1. Delete -> toast -> its Undo button
    select("apple.txt")
    dbg.key(K_DELETE)
    binnedok = wait(lambda: "apple.txt" in binned())
    t = None
    end = time.time() + 10
    while time.time() < end:
        fm.layout_now(dbg, win)
        t = toast()
        if t and t[0] == 1 and t[2] == "Undo":
            break
        time.sleep(0.3)
    res.check("Delete moves the file to the bin and a toast offers Undo",
              binnedok and t is not None and t[0] == 1 and t[2] == "Undo", f"toast={t} bin={binned()}")
    if t and t[0] == 1:
        x, y, w, h = t[1]
        fm.sure_click(dbg, qmp, lay.ox + x + w // 2, lay.oy + y + h // 2)
    back = wait(lambda: "apple.txt" in names(ROOT) and "apple.txt" not in binned())
    fm.layout_now(dbg, win)
    t2 = toast()
    res.check("the toast's Undo puts it back, and the toast then offers Redo",
              back and t2 is not None and t2[2] == "Redo", f"dir={names(ROOT)} toast={t2}")

    # 2. Ctrl+Y, then Ctrl+Z
    dbg.key(K_CTRL_Y)
    redone = wait(lambda: "apple.txt" not in names(ROOT) and "apple.txt" in binned())
    dbg.key(K_CTRL_Z)
    undone = wait(lambda: "apple.txt" in names(ROOT))
    res.check("Ctrl+Y redoes the delete and Ctrl+Z undoes it again",
              redone and undone, f"redone={redone} undone={undone} dir={names(ROOT)}")

    # 3. a rename, undone
    select("berry.txt")
    dbg.key(fm.K_F2)
    rl = fm.wait_layout(dbg, win, lambda l: l.renaming is not None and 1 in l.renaming[:2])
    editing = rl is not None
    for ch in "cherry":   # the field selects the stem; the extension stays
        dbg.key(ch)
    dbg.key(fm.K_ENTER)
    renamed = wait(lambda: "cherry.txt" in names(ROOT))
    dbg.key(K_CTRL_Z)
    restored = wait(lambda: "berry.txt" in names(ROOT) and "cherry.txt" not in names(ROOT))
    fm._collect(dbg)
    trail = [ln.strip() for ln in fm._ALL_BUF if "rename" in ln or "undo" in ln][-6:]
    res.check("Ctrl+Z undoes a rename", renamed and restored,
              f"editing={editing} renamed={renamed} dir={names(ROOT)} app={trail}")

    # 4. a new folder, undone into the bin
    dbg.key(fm.K_F7)
    fm.wait_layout(dbg, win, lambda l: l.renaming is not None)
    dbg.key(fm.K_ENTER)
    made = wait(lambda: "New folder" in names(ROOT))
    dbg.key(K_CTRL_Z)
    gone = wait(lambda: "New folder" not in names(ROOT))
    res.check("Ctrl+Z undoes a new folder into the Recycle Bin, not for good",
              made and gone and "New folder" in binned(),
              f"made={made} dir={names(ROOT)} bin={binned()}")

    sh(f"rm -r {ROOT}")
    sh("trash empty")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "undo_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nundo_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
