#!/usr/bin/env python3
"""Recent in the File Manager (lib/urecent.h, fm_recent.c), checked
against /var/lib/recent and `ls` -- a different reader from the app.

1. **Opening a file records it with the app it opened in**: `open` on a
   .txt and a .jpg (lib/uopen.h's spawn, the double click's path) puts
   both in /var/lib/recent as Notepad and Image Viewer -- the desktop
   entries' names, not the programs'.
2. **Places has a Recent row**, and the address bar's `recent:/` lists
   every file, newest first, in day bands: a line written three days back
   makes a second band (the layout's caption count).
3. **Delete takes a row off the list and leaves the file.**
4. **Open folder goes to the folder with the file selected.**
5. **Clear list empties it.**

Shares filemanager_test.py's layout reader; the fixture is /rc.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/rc"
RECENT = "/var/lib/recent"
K_CTRL_L, K_DELETE = "0x0c", "0xf788"
TB_OPEN_FOLDER, TB_CLEAR = 0, 3   # recent_toolbar_items, separators counted


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    def recorded():
        """(app, path) per line of the list, oldest first."""
        out = []
        for ln in sh(f"cat {RECENT}").splitlines():
            if "\t/" not in ln:
                continue
            head, path = ln.split("\t", 1)
            out.append((head.split(" ", 1)[1].strip() if " " in head else "", path.strip()))
        return out

    def wait(pred, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.4)
        return False

    sh(f"rm -r {ROOT}")
    sh(f"mkdir {ROOT}")
    dbg.write_lines(f"{ROOT}/notes.txt", ["notes"])
    dbg.write_lines(f"{ROOT}/old.txt", ["old"])
    sh(f"cp /usr/share/wallpapers/dusk.jpg {ROOT}/dusk.jpg")

    # 1. open records
    sh(f"open {ROOT}/notes.txt")
    sh(f"open {ROOT}/dusk.jpg")
    want = [("Notepad", f"{ROOT}/notes.txt"), ("Image Viewer", f"{ROOT}/dusk.jpg")]
    ok = wait(lambda: all(w in recorded() for w in want))
    res.check("open records each file with its app's name", ok, f"list={recorded()}")

    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=1", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    lay = fm.wait_layout(dbg, win, lambda l: "recent:/" in l.treerow.values()) or fm.layout_now(dbg, win)
    res.check("Places has a Recent row", lay is not None and "recent:/" in lay.treerow.values(),
              f"tree={lay.treerow if lay else None}")

    # 2. the list, newest first, in bands
    def go_recent(rows):
        dbg.key(K_CTRL_L)
        for ch in "recent:/":
            dbg.key(ch)
        dbg.key(fm.K_ENTER)
        return fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "recent:/" and l.rows.get(0) == rows)

    lay = go_recent(3) or fm.layout_now(dbg, win)
    dbg.key(fm.K_HOME)
    lay = fm.wait_layout(dbg, win, lambda l: l.selected is not None) or fm.layout_now(dbg, win)
    res.check("recent:/ lists all three, the newest first, in two day bands",
              lay.dir.get(0) == "recent:/" and lay.rows.get(0) == 3 and lay.captions.get(0) == 2
              and lay.selected == "dusk.jpg",
              f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)} captions={lay.captions.get(0)} "
              f"selected={lay.selected!r}")

    # 3. Delete is Remove from list
    dbg.key(K_DELETE)
    lay = fm.wait_layout(dbg, win, lambda l: l.rows.get(0) == 2) or fm.layout_now(dbg, win)
    kept = "dusk.jpg" in fm.listing(dbg, ROOT)
    rows = lay.rows.get(0) if lay else None
    res.check("Delete takes the row off the list and leaves the file",
              rows == 2 and kept and f"{ROOT}/dusk.jpg" not in [p for _, p in recorded()],
              f"rows={rows} file kept={kept} list={recorded()}")
    if not lay:
        return

    # 4. Open folder
    dbg.key(fm.K_HOME)
    lay = fm.wait_layout(dbg, win, lambda l: l.selected == "notes.txt") or fm.layout_now(dbg, win)
    if lay and TB_OPEN_FOLDER in lay.tbitems:
        x, y, w, h = lay.tbitems[TB_OPEN_FOLDER]
        fm.sure_click(dbg, qmp, lay.ox + x + w // 2, lay.oy + y + h // 2)
    lay = (fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == ROOT and l.selected == "notes.txt")
           or fm.layout_now(dbg, win))
    res.check("Open folder goes to the folder with the file selected",
              lay is not None and lay.dir.get(0) == ROOT and lay.selected == "notes.txt",
              f"dir={lay.dir.get(0) if lay else None!r} selected={lay.selected if lay else None!r}")

    # 5. Clear list
    lay = go_recent(2) or fm.layout_now(dbg, win)
    if lay and TB_CLEAR in lay.tbitems:
        x, y, w, h = lay.tbitems[TB_CLEAR]
        fm.sure_click(dbg, qmp, lay.ox + x + w // 2, lay.oy + y + h // 2)
    lay = fm.wait_layout(dbg, win, lambda l: l.rows.get(0) == 0) or fm.layout_now(dbg, win)
    res.check("Clear list empties it", lay is not None and lay.rows.get(0) == 0 and recorded() == [],
              f"rows={lay.rows.get(0) if lay else None} list={recorded()}")

    sh(f"rm -r {ROOT}")
    sh(f"rm {RECENT}")


def put_old_line(instance):
    """The list starts with one line three days old -- "Earlier this
    week", a band of its own -- written on the host because the guest's
    echo cannot write the tab. Before the console is ours: `vm.py put`
    talks over the same socket. The guest's clock is the host's (QEMU's
    RTC is UTC from the host)."""
    with tempfile.NamedTemporaryFile("w", suffix=".recent", delete=False) as f:
        f.write(f"{int(time.time()) - 3 * 86400} Test\t{ROOT}/old.txt\n")
    r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "vm.py"),
                        "--instance", str(instance), "put", f.name, RECENT],
                       capture_output=True, text=True)
    os.unlink(f.name)
    print(f"put {RECENT}: rc={r.returncode} {(r.stdout + r.stderr).strip()[-200:]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "recent_gui_test")
    res = Results()
    put_old_line(args.instance if args.instance is not None else 0)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nrecent_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
