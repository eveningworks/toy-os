#!/usr/bin/env python3
"""A .zip as a folder in the File Manager (fm_zip.c over lib/uzip.h).

The archive is written HERE by Python's zipfile -- not by anything in
toy-os -- and put on the guest, so the reader is checked against a
foreign writer: a stored member, deflated ones, a folder two deep.

1. **Opening the archive lists its top level as a folder**: the folder
   "dir" (derived from the names below it) and hello.txt, no "..".
2. **A folder inside opens in place**, and Up returns to the top.
3. **Up from the top goes to the folder the archive is in.**
4. **Extract all writes every member beside the archive** -- in a folder
   named for it -- with each file's size as zipfile wrote it, and the
   pane goes there.
5. **Opening a member extracts it and opens the copy** (/tmp/zip-open).
"""
import argparse
import io
import os
import sys
import tempfile
import time
import zipfile
import subprocess

import filemanager_test as fm
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = "/home/zt.zip"
MEMBERS = {
    "hello.txt": b"Hello from Python's zipfile.\n",
    "dir/lorem.txt": b"lorem ipsum dolor sit amet " * 60,
    "dir/sub/deep.txt": b"three levels down\n" * 9,
}


def make_zip():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(zipfile.ZipInfo("hello.txt"), MEMBERS["hello.txt"])   # stored
        for name in ("dir/lorem.txt", "dir/sub/deep.txt"):
            z.writestr(name, MEMBERS[name], compress_type=zipfile.ZIP_DEFLATED)
    return buf.getvalue()


def put_archive(instance):
    """Before the console is ours: `vm.py put` talks over the same
    debug socket, and a second client on it while the test holds it
    loses the file without a word."""
    with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as f:
        f.write(make_zip())
        host = f.name
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(instance),
                    "put", host, ARCHIVE], capture_output=True)
    os.unlink(host)


def run(dbg, qmp, res, instance):
    def sh(cmd):
        return dbg.send(f"sh {cmd}") or ""

    sh("rm -r /home/zt")

    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=details", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} /home", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home")

    def seek(name):
        dbg.key(fm.K_HOME)
        for ch in name:
            dbg.key("0x2e" if ch == "." else ch)
        return fm.wait_layout(dbg, win, lambda l: l.selected == name) or fm.layout_now(dbg, win)

    # 1.
    seek("zt.zip")
    dbg.key(fm.K_ENTER)
    root = f"zip:{ARCHIVE}"
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root and l.rows.get(0) == 2) or fm.layout_now(dbg, win)
    res.check("opening the archive lists its top level: 'dir' and hello.txt, no '..'",
              lay.dir.get(0) == root and lay.rows.get(0) == 2, f"dir={lay.dir.get(0)!r} rows={lay.rows.get(0)}")

    # 2.
    seek("dir")
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root + "/dir") or fm.layout_now(dbg, win)
    inside = lay.dir.get(0) == root + "/dir" and lay.rows.get(0) == 2
    dbg.key(fm.K_BACKSPACE)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root) or fm.layout_now(dbg, win)
    res.check("a folder inside opens in place (lorem.txt, sub), and Up returns to the top",
              inside and lay.dir.get(0) == root, f"inside={inside} back={lay.dir.get(0)!r}")

    # 3.
    dbg.key(fm.K_BACKSPACE)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home") or fm.layout_now(dbg, win)
    res.check("Up from the top goes to the folder the archive is in",
              lay.dir.get(0) == "/home", f"dir={lay.dir.get(0)!r}")

    # 4. Extract all: the zip toolbar's first item
    seek("zt.zip")
    dbg.key(fm.K_ENTER)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root and 0 in l.tbitems) or fm.layout_now(dbg, win)
    r = lay.tbitems.get(0)
    if r:
        fm.sure_click(dbg, qmp, lay.ox + r[0] + r[2] // 2, lay.oy + r[1] + r[3] // 2)
    lay = fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home/zt", timeout=30) or fm.layout_now(dbg, win)
    sizes = {n: fm.size_of(dbg, "/home/zt/" + os.path.dirname(n), os.path.basename(n))
             for n in MEMBERS}
    want = {n: len(b) for n, b in MEMBERS.items()}
    res.check("Extract all writes every member beside the archive, sizes as written, and goes there",
              lay.dir.get(0) == "/home/zt" and sizes == want,
              f"dir={lay.dir.get(0)!r} sizes={sizes} want={want}")

    # 5. open a member
    fm.wait_layout(dbg, win, lambda l: True)
    dbg.key(fm.K_BACKSPACE)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == "/home")
    seek("zt.zip")
    dbg.key(fm.K_ENTER)
    fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == root)
    seek("hello.txt")
    dbg.key(fm.K_ENTER)
    end, opened = time.time() + 20, False
    while time.time() < end and not opened:
        fm._collect(dbg)
        opened = any("files: open /tmp/zip-open/hello.txt" in ln for ln in fm._ALL_BUF)
        time.sleep(0.3)
    res.check("opening a member extracts it to /tmp/zip-open and opens that copy",
              opened, f"last={[l for l in fm._ALL_BUF if 'open' in l][-2:]}")
    for w2 in dbg.windows():
        if w2["title"] != fm.TITLE:
            dbg.send(f"gui close {w2['z']}")
    sh(f"rm {ARCHIVE}")
    sh("rm -r /home/zt")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "zip_gui_test")
    res = Results()
    put_archive(args.instance if args.instance is not None else 0)
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res, args.instance if args.instance is not None else 0)
    finally:
        dbg.close()
    print(f"\nzip_gui_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
