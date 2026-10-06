#!/usr/bin/env python3
"""Folder sizes in the File Manager (View > Folder sizes; fm_sizes.c over
lib/uwalk.h): each folder of the one on screen is counted in the
background, and the count is the bytes of EVERY file below it.

1. **A folder's size includes its subfolders**: /zg/a holds three
   1000-byte files and, one level down, two of 500 -- 4000, not 3000.
2. **Each folder is its own count**: /zg/b is 700, /zg/c (empty) is 0.

The counts are read from the app's `files: size <bytes> <path>` lines
and compared with sizes made here by `mkfiles`, a different writer from
the reader under test.
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

ROOT = "/zg"
WANT = {f"{ROOT}/a": 4000, f"{ROOT}/b": 700, f"{ROOT}/c": 0}


def run(dbg, qmp, res):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh(f"rm -r {ROOT}")
    for d in ("", "/a", "/a/deep", "/b", "/c"):
        sh(f"mkdir {ROOT}{d}")
    sh(f"mkfiles {ROOT}/a 3 1000")
    sh(f"mkfiles {ROOT}/a/deep 2 500")
    sh(f"mkfiles {ROOT}/b 1 700")
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "folder_sizes=1"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {ROOT}", fm.TITLE)
    res.check("the File Manager opens on the fixture", win is not None, "no window")
    if not win:
        return

    got = {}
    end = time.time() + 30
    while time.time() < end and len(got) < len(WANT):
        fm._collect(dbg)
        for ln in fm._ALL_BUF:
            if "files: size " in ln:
                p = ln.split("files: size ", 1)[1].split()
                if len(p) == 2 and p[1] in WANT:
                    got[p[1]] = int(p[0])
        time.sleep(0.3)
    res.check("a folder's size includes its subfolders (a = 3x1000 + 2x500)",
              got.get(f"{ROOT}/a") == 4000, f"got={got}")
    res.check("each folder is its own count (b = 700, empty c = 0)",
              got.get(f"{ROOT}/b") == 700 and got.get(f"{ROOT}/c") == 0, f"got={got}")
    sh(f"rm -r {ROOT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sizes_gui_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nsizes_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
