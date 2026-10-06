#!/usr/bin/env python3
"""Rename many in the File Manager (lib/urename.h, ui/uui_renamer.h),
checked against the disk through `ls` and `cat` -- a different reader
from the app that did the work.

1. **F2 with several marked opens "Rename N items"**, not the in-place
   field.
2. **Enter renames by the default pattern** (`<folder>-##`, extensions
   kept). The fixture makes it a CHAIN: berry.txt must become rn-02.txt
   while rn-02.txt itself is still there, becoming rn-03.txt -- so a
   rename that went straight to the new names would refuse or overwrite.
   Each file carries its own content, and the content is what is checked.
3. **One Ctrl+Z undoes the whole set.**
4. **Esc renames nothing.**

Shares filemanager_test.py's layout reader; the fixture is /rn.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/rn"
K_CTRL_A, K_CTRL_Z = "0x01", "0x1a"
DIALOG = "Rename 3 items"
# name -> content, and what each must be called after the rename
BEFORE = {"apple.txt": "A", "berry.txt": "B", "rn-02.txt": "C"}
AFTER = {"rn-01.txt": "A", "rn-02.txt": "B", "rn-03.txt": "C"}


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    def names():
        return sorted(fm.listing(dbg, ROOT))

    def content(name):
        out = sh(f"cat {ROOT}/{name}")
        lines = [ln.strip() for ln in out.splitlines()
                 if ln.strip() and not ln.startswith(("sh", "cat")) and "exit" not in ln]
        return lines[0] if lines else None

    def matches(want):
        return names() == sorted(want) and all(content(n) == c for n, c in want.items())

    def wait(pred, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.4)
        return False

    def dialog_open(timeout=10.0):
        return wait(lambda: dbg.window(DIALOG) is not None, timeout)

    sh(f"rm -r {ROOT}")
    sh(f"mkdir {ROOT}")
    for n, c in BEFORE.items():
        dbg.write_lines(f"{ROOT}/{n}", [c])
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the fixture", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT)

    # 1. Ctrl+A, F2 -> the dialog
    dbg.key(K_CTRL_A)
    dbg.key(fm.K_F2)
    opened = dialog_open()
    res.check(f"F2 with three marked opens \"{DIALOG}\"", opened,
              f"windows={[w['title'] for w in dbg.windows()]}")
    if not opened:
        return

    # 2. Enter: the default pattern, through the chain
    dbg.key(fm.K_ENTER)
    closed = wait(lambda: dbg.window(DIALOG) is None)
    done = wait(lambda: matches(AFTER))
    res.check("Enter renames all three to rn-01..03, each keeping its own content",
              closed and done, f"closed={closed} dir={names()} "
              f"content={[(n, content(n)) for n in names()]}")

    # 3. one Ctrl+Z undoes the set
    dbg.key(K_CTRL_Z)
    undone = wait(lambda: matches(BEFORE))
    res.check("one Ctrl+Z puts every old name back", undone,
              f"dir={names()} content={[(n, content(n)) for n in names()]}")

    # 4. Esc renames nothing
    dbg.key(K_CTRL_A)
    dbg.key(fm.K_F2)
    reopened = dialog_open()
    dbg.key(fm.K_ESC)
    gone = wait(lambda: dbg.window(DIALOG) is None)
    time.sleep(1.0)
    res.check("Esc closes the dialog and renames nothing",
              reopened and gone and matches(BEFORE),
              f"reopened={reopened} gone={gone} dir={names()}")

    sh(f"rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "rename_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nrename_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
