#!/usr/bin/env python3
"""Search into subfolders in the File Manager (fm_search.c over
lib/uwalk.h): typing filters the folder, Enter walks the tree below it.

1. **Typing filters THIS folder only**: one match at the top, the deep
   ones not listed -- the behaviour a search into subfolders must not
   replace.
2. **Enter lists every match below, and only those**: a file three
   levels down, a matching FOLDER, the top one -- and NOT the one under
   a hidden folder, nor the non-matching file. The view is
   "search:/sg", with no "..".
3. **A result is opened where it lives**: Enter on the deep file's row
   reports its real path (`files: open .../a/b/c/...`), not search:/.
4. **"This folder" on the strip goes back to the filtered folder.**

The fixture is /sg; the expected set is built here, by hand.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/sg"
K_CTRL_F = "0x06"


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    for d in ("", "/a", "/a/b", "/a/b/c", "/a/needle-dir", "/.hidden"):
        sh(f"mkdir {ROOT}{d}")
    for f in ("/needle-top.txt", "/other.txt", "/a/b/c/needle-deep.txt",
              "/.hidden/needle-hidden.txt"):
        sh(f"touch {ROOT}{f}")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the fixture", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT)

    def logged(text, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            fm._collect(dbg)
            if any(text in ln for ln in fm._ALL_BUF):
                return True
            time.sleep(0.3)
        return False

    # 1. typing filters this folder
    dbg.key(K_CTRL_F)
    for ch in "needle":
        dbg.key(ch)
    lay = fm.wait_layout(dbg, win, lambda l: l.rows.get(0) == 2) or fm.layout_now(dbg, win)
    res.check("typing filters this folder only: '..' and needle-top.txt",
              lay.rows.get(0) == 2, f"rows={lay.rows.get(0)}")

    # 2. Enter searches below
    dbg.key(fm.K_ENTER)
    # THE FRAME AFTER THE LAST RESULT IS THE ONE TO READ, and an idle
    # window draws no other: waiting on the log first would swallow it.
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == f"search:{ROOT}" and l.rows.get(0) == 3,
                         timeout=25.0) or fm.last_layout() or fm.layout_now(dbg, win)
    done = logged("files: search done")
    found = [ln.strip() for ln in fm._ALL_BUF if "files: search done" in ln][-1:]
    res.check("Enter lists the three matches below, not the hidden one, no '..'",
              done and lay.dir.get(0) == f"search:{ROOT}" and lay.rows.get(0) == 3,
              f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)} log={found}")

    # 3. a result opens where it lives
    dbg.key(fm.K_HOME)
    for ch in "needle-de":
        dbg.key(ch)
    lay = fm.wait_layout(dbg, win, lambda l: l.selected == "needle-deep.txt") or fm.layout_now(dbg, win)
    dbg.key(fm.K_ENTER)
    opened = logged(f"files: open {ROOT}/a/b/c/needle-deep.txt", 10.0)
    res.check("Enter on a deep result opens it at its real path",
              opened, f"selected={lay.selected!r} last={[l for l in fm._ALL_BUF if 'open' in l][-2:]}")
    # What opened (Notepad) is not under test, and must not sit over the
    # strip the next step clicks.
    time.sleep(1.0)
    for w2 in sorted(dbg.windows(), key=lambda w2: -w2["z"]):
        if w2["title"] != fm.TITLE:
            dbg.send(f"gui close {w2['z']}")
            time.sleep(0.3)

    # 4. "This folder" goes back to the filter
    lay = fm.layout_now(dbg, win) or fm.last_layout()
    rect = None
    for ln in reversed(fm._ALL_BUF):
        if "files: layout scope " in ln:
            rect = [int(v) for v in ln.split("files: layout scope ", 1)[1].split()[:4]]
            break
    if rect:
        x, y, w, h = rect
        fm.sure_click(dbg, qmp, lay.ox + x + w // 6, lay.oy + y + h // 2)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT) or fm.layout_now(dbg, win)
    res.check("'This folder' on the strip goes back to the folder, filtered",
              rect is not None and lay.dir.get(0) == ROOT and lay.rows.get(0) == 2,
              f"scope={rect} dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")

    sh(f"rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "search_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nsearch_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
