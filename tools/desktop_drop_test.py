#!/usr/bin/env python3
"""tools/desktop_drop_test.py -- dropping onto desktop icons, and their art.

WHAT THIS IS
------------
A drag that rests on a desktop icon says what a release does there, and
the release does it (userland/wm/desktop.c, "dropping ONTO an icon"):

  1. Each icon is drawn with its TYPE's art -- a folder, a text file, a
     picture, the Recycle Bin -- read from `gui icons --json`'s "art",
     the same lookup the draw loop paints with.
  2. A file held over a FOLDER lights it ("target") and the label says
     "Move to Box"; the release moves it in (`ls` of the folder, and it
     is gone from the desktop). A plain FILE under the pointer is no
     target -- the control, so a desktop that lit everything fails.
  3. Over an app that does not open the type: "Notepad can't open .qoi
     files", and the release does NOTHING -- the file stays, in its cell.
  4. Over the Recycle Bin: "Move to Recycle Bin"; the release puts it in
     the bin (`trash list`), and the bin's art turns "trash-full".
  5. Over an app that does: "Open with Image Viewer", and the release
     opens a window of it. Last, because that window covers the icons.

HOW
---
Held drags with the REAL pointer (DebugConsole.warp_cursor + QMP button
down/up): `gui drag` is one atomic gesture and a test about what is shown
DURING a drag has to stop in the middle of one and ask.

    python3 tools/vm.py start && python3 tools/desktop_drop_test.py
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
from harness import Results                     # noqa: E402

DESK = "/home/desktop"
_res = Results()
check = _res.check


def icons(dbg):
    j = dbg.json("gui icons --json")
    return {i["name"]: i for i in j.get("icons", [])}, j.get("drop", "")


def centre(i):
    return i["x"] + i["w"] // 2, i["y"] + i["h"] // 3


def hold_over(dbg, qmp, src, dst):
    """Press on `src`, move onto `dst` and stay there; returns what the
    desktop reports with the button still down. Call release() after."""
    ic, _ = icons(dbg)
    x0, y0 = centre(ic[src])
    x1, y1 = centre(ic[dst])
    dbg.warp_cursor(qmp, x0, y0)
    qmp.mouse_down()
    for k in range(1, 7):
        dbg.warp_cursor(qmp, x0 + (x1 - x0) * k // 6, y0 + (y1 - y0) * k // 6)
    time.sleep(0.5)
    return icons(dbg)


def release(dbg, qmp):
    qmp.mouse_up()
    time.sleep(1.0)
    dbg.settle()


def wait_for(fn, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.3)
    return fn()


def run(dbg, qmp):
    for c in (f"mkdir {DESK}/Box", f"write {DESK}/notes.txt hello", f"write {DESK}/junk.txt bye",
              f"cp /usr/share/icons/about.qoi {DESK}/pic.qoi",
              f"cp /usr/wm/applications/imgview.desktop {DESK}/imgview.desktop"):
        dbg.send(f"sh {c}")
    want = {"Box", "notes.txt", "junk.txt", "pic.qoi", "Image Viewer", "Recycle Bin", "Notepad"}
    ic = wait_for(lambda: (lambda d: d if want <= set(d) else None)(icons(dbg)[0]))
    if not check("the fixtures are on the desktop", ic is not None and want <= set(ic),
                 f"have {sorted(ic or [])}"):
        return

    # 1. Art by type.
    arts = {n: ic[n].get("art") for n in ("Box", "notes.txt", "pic.qoi", "Recycle Bin")}
    check("each icon is drawn with its type's art",
          arts == {"Box": "folder", "notes.txt": "file-text", "pic.qoi": "file-image",
                   "Recycle Bin": "trash-empty"}, str(arts))

    # 2. A folder takes it; a plain file does not.
    ic, drop = hold_over(dbg, qmp, "notes.txt", "junk.txt")
    check("a plain file is no target", drop == "" and not any(i.get("target") for i in ic.values()),
          f"drop={drop!r}")
    release(dbg, qmp)
    ic, drop = hold_over(dbg, qmp, "notes.txt", "Box")
    lit = [n for n, i in ic.items() if i.get("target")]
    check("over a folder: it is lit, and only it", lit == ["Box"], str(lit))
    check("...and the drag says where it goes", drop == "Move to Box", repr(drop))
    release(dbg, qmp)
    moved = wait_for(lambda: "notes.txt" in dbg.send(f"sh ls {DESK}/Box") and "notes.txt" not in icons(dbg)[0])
    check("the release moves it into the folder", bool(moved), dbg.send(f"sh ls {DESK}/Box")[-120:])

    # 3. An app that does not open the type refuses, and nothing moves.
    before = icons(dbg)[0]["pic.qoi"]
    ic, drop = hold_over(dbg, qmp, "pic.qoi", "Notepad")
    check("over an app that cannot open it: it says so", drop == "Notepad can't open .qoi files", repr(drop))
    release(dbg, qmp)
    after = icons(dbg)[0].get("pic.qoi")
    check("...and the release leaves the file where it was",
          after is not None and (after["x"], after["y"]) == (before["x"], before["y"]),
          f"{(before['x'], before['y'])} -> {after and (after['x'], after['y'])}")

    # 4. The Recycle Bin.
    ic, drop = hold_over(dbg, qmp, "junk.txt", "Recycle Bin")
    check("over the Recycle Bin: it says so", drop == "Move to Recycle Bin", repr(drop))
    release(dbg, qmp)
    binned = wait_for(lambda: "junk.txt" in dbg.send("sh trash list"))
    check("the release puts it in the bin", bool(binned))
    full = wait_for(lambda: icons(dbg)[0].get("Recycle Bin", {}).get("art") == "trash-full")
    check("...and the bin is drawn full", bool(full), str(icons(dbg)[0].get("Recycle Bin", {}).get("art")))

    # 5. An app that opens it. Last: its window covers the icons.
    ic, drop = hold_over(dbg, qmp, "pic.qoi", "Image Viewer")
    check("over an app that opens it: Open with", drop == "Open with Image Viewer", repr(drop))
    release(dbg, qmp)
    win = wait_for(lambda: dbg.window("Image Viewer"))
    check("the release opens it there", win is not None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "desktop_drop_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("desktop drop targets")
    run(dbg, qmp)
    print(f"\ndesktop_drop_test: {len(_res.passes)} passed, {len(_res.fails)} failed")
    return 1 if _res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
