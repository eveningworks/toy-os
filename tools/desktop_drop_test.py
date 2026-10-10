#!/usr/bin/env python3
"""tools/desktop_drop_test.py -- dropping onto desktop icons, and their art.

WHAT THIS IS
------------
A drag that rests on a desktop icon says what a release does there, and
the release does it (userland/wm/desktop.c, "dropping ONTO an icon"):

  1. Each icon is drawn with its TYPE's art -- a folder, a text file, a
     picture, the Recycle Bin -- read from `gui icons --json`'s "art",
     the same lookup the draw loop paints with. Then a picture becomes
     its thumbnail ("thumb", made by /bin/thumb into the cache) and a
     folder shows the pictures inside it ("peeks"); a text file does not.
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


EMPTY_SPOT = (700, 300)   # bare desktop at the default 1280x720


def notice_title(dbg):
    return (dbg.json("gui state --json").get("notice") or {}).get("title")


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

    # 1b. Pictures show themselves -- made by /bin/thumb, read from the
    # cache -- and a folder shows the pictures inside it. A text file
    # stays its type icon: the control, so a desktop that drew a frame
    # for everything fails.
    for c in (f"mkdir {DESK}/Holiday", f"cp /usr/share/pictures/dusk-24bit.bmp {DESK}/Holiday/",
              f"cp /usr/share/wallpapers/aurora.jpg {DESK}/aurora.jpg"):
        dbg.send(f"sh {c}")
    shown = wait_for(lambda: (lambda d: d if d.get("pic.qoi", {}).get("thumb") and d.get("aurora.jpg", {}).get("thumb")
                              and d.get("Holiday", {}).get("peeks", 0) >= 1 else None)(icons(dbg)[0]), timeout=30)
    ic = icons(dbg)[0]
    check("pictures are drawn as thumbnails", bool(shown),
          str({n: (ic.get(n, {}).get("thumb"), ic.get(n, {}).get("peeks")) for n in ("pic.qoi", "aurora.jpg", "Holiday")}))
    check("...and a text file is not", ic.get("notes.txt", {}).get("thumb") is False)
    cache = dbg.send("sh ls /var/cache/thumbnails")
    check("...and they came from /bin/thumb's cache", "home%desktop%aurora.jpg-" in cache, cache[-200:])

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

    # 2b. Undo. The FIRST undoable operation shows a card saying what it
    # did; Ctrl+Z takes it back (the file is on the desktop again and out
    # of the folder). A second operation shows NO card: once, ever.
    card = wait_for(lambda: notice_title(dbg) == "Moved notes.txt to Box", timeout=5)
    check("the first move shows the Undo card, saying what it did", bool(card), repr(notice_title(dbg)))
    dbg.key("0x1a", mods="ctrl")
    back = wait_for(lambda: "notes.txt" in icons(dbg)[0] and "notes.txt" not in dbg.send(f"sh ls {DESK}/Box"))
    check("Ctrl+Z takes the move back", bool(back))

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

    check("a second operation shows no card", "Recycle Bin" not in (notice_title(dbg) or ""),
          repr(notice_title(dbg)))

    # 4b. New > Text document opens into rename.
    dbg.rclick(*EMPTY_SPOT)
    m = dbg.ctxmenu()
    new = next((r for r in m.get("rows", []) if r["label"] == "New"), None)
    if new:
        dbg.send(f"gui warp {m['x'] + 20} {new['cy']}")
        dbg.settle()
        sub = dbg.ctxmenu().get("sub") or {}
        td = next((r for r in sub.get("rows", []) if r["label"] == "Text document"), None)
        if td:
            dbg.click(sub["x"] + 20, td["cy"])
    made = wait_for(lambda: (icons(dbg)[0].get("New text document.txt") or {}).get("renaming"))
    check("New > Text document makes one, already renaming", bool(made),
          str(icons(dbg)[0].get("New text document.txt")))
    dbg.key("0x1b")

    # 4c. Sort by size puts the biggest file first, after the folders --
    # aurora.jpg dwarfs every other file here. By name it does not lead.
    def order():
        d = icons(dbg)[0]
        return [n for n, _ in sorted(d.items(), key=lambda kv: (kv[1]["x"], kv[1]["y"]))
                if d[n]["kind"] != "dir"]
    dbg.send("sh config set desktop.sort_by size")
    dbg.rclick(*EMPTY_SPOT)
    m = dbg.ctxmenu()
    srt = next((r for r in m.get("rows", []) if r["label"] == "Sort by"), None)
    if srt:
        dbg.send(f"gui warp {m['x'] + 20} {srt['cy']}")
        dbg.settle()
        sub = dbg.ctxmenu().get("sub") or {}
        sz = next((r for r in sub.get("rows", []) if r["label"] == "Size"), None)
        if sz:
            dbg.click(sub["x"] + 20, sz["cy"])
    first = wait_for(lambda: (order() or [None])[0] == "aurora.jpg")
    check("Sort by > Size: the biggest file leads", bool(first), str(order()[:4]))
    dbg.send("sh config set desktop.sort_by name")
    wait_for(lambda: (order() or [None])[0] != "aurora.jpg")
    check("...and by name it does not (the control)", order()[:1] != ["aurora.jpg"], str(order()[:4]))

    # 4d. Auto arrange: an icon dropped on bare desktop goes back to its
    # place in the order. Without it, the same drop moves it (the control).
    def drop_on_bare(name):
        i = icons(dbg)[0][name]
        dbg.drag(*centre(i), *EMPTY_SPOT, steps=16)
        time.sleep(1.5)
        dbg.settle()
        j = icons(dbg)[0][name]
        return (i["x"], i["y"]), (j["x"], j["y"])
    a, b = drop_on_bare("pic.qoi")
    check("without Auto arrange a drop moves the icon", a != b, f"{a} -> {b}")
    dbg.send("sh config set desktop.auto_arrange on")
    time.sleep(1.5)
    dbg.settle()
    a, b = drop_on_bare("pic.qoi")
    check("with Auto arrange it goes back into the order", a == b, f"{a} -> {b}")
    dbg.send("sh config set desktop.auto_arrange off")

    # 4e. Show desktop icons off: none to see or hit; on: all back.
    dbg.send("sh config set desktop.show_icons off")
    gone = wait_for(lambda: not icons(dbg)[0])
    check("Show desktop icons off hides every icon", bool(gone) or not icons(dbg)[0])
    dbg.send("sh config set desktop.show_icons on")
    check("...and on brings them back", bool(wait_for(lambda: "pic.qoi" in icons(dbg)[0])))

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
