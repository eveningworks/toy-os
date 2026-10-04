#!/usr/bin/env python3
"""tools/desktop_menu_test.py -- the desktop's menus, its glass, Rename and Properties.

WHAT THIS IS
------------
The 2026-10-01 redesign (docs/gui-guidelines.md, "Menus" and "The
desktop") put every menu on one card -- rows with action-coloured
icons, shortcuts, a command strip -- grouped the desktop's Open > by
category, gave the WM's context menu a keyboard, and added a glass
selection, a hover wash, Rename in place and Properties to the desktop.
This drives each through the WM's own geometry (`gui ctxmenu --json`,
`gui icons --json`), never derived pixels.

  1. The background menu's rows, in order; Paste GREYED with nothing on
     the clipboard and live once a file is copied -- both directions,
     so a menu that never greys and one that always does both fail.
  2. Open > is the categories, and their apps together are every app
     the desktop lists -- past sixteen, which is where the old flat list
     stopped (Task Manager and Terminal fell off the end).
  3. The KEYBOARD: a letter opens Icon size >, a second commits Large
     (the setting changes), and Esc closes one level at a time.
  4. The icon menu's strip: Cut, Copy, Rename, Delete on ONE row at four
     different x, then Open and Properties; Rename greys out for a
     selection of two.
  5. Rename in place: F2 opens the field, Esc abandons it, Enter commits
     -- a folder through sys_rename, a launcher through its Name= line.
  6. Alt+Enter opens Properties for the selection.
  7. The hover wash: a parked pointer lights its icon (the guest says so
     AND the pixels brighten), a second icon does not (the control), and
     leaving clears it.
  8. The selection is GLASS: the wallpaper's gradient still shows through
     it, which a solid fill (what it replaced) cannot pass.
  9. An APP's popup surface has round corners, cut by the compositor:
     Notepad's File menu reads the scene at its corner pixel and its own
     ground one radius in.

    python3 tools/vm.py start
    python3 tools/desktop_menu_test.py --instance 0
    echo $?                       # 0 = every check passed

POSITIVE CONTROLS, run against this tool when it was written:
  * can_paste() returning 1 always -> check 1's "greyed" goes red.
  * the 16-app cap put back in build_open_menu() -> check 2 goes red
    and names the missing apps.
  * context_menu_key() unregistered from wm_overlay.c -> check 3 goes
    red ("Icon size > did not open").
  * the glass selection replaced by a solid ugfx_fill_rect -> check 8
    goes red and check 7's control stays green.
  * corners_round() skipped for popups -> check 9 goes red. Its first
    version asked only that the corner be darker than the card's inside,
    which the uncut EDGE also is, and stayed green.

CAVEAT
------
Injected input enters below the PS/2 driver (tools/gui_debug.py), so a
clean run says nothing about the real mouse or keyboard path.
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui        # noqa: E402
from qmp_test import QMPSession                      # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

BG_ROWS = ["Open", "-", "New folder", "Paste", "-", "Refresh", "Sort by name",
           "Icon size", "-", "System Settings"]
EMPTY = (700, 300)          # a point of bare desktop at the default 1280x720
GLASS_FILL = 56             # desktop.c's selected (not hovered) fill alpha


def shot(qmp, tmp, name):
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, name))
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def icons(dbg):
    return dbg.json("gui icons --json").get("icons", [])


def icon(dbg, name):
    for i in icons(dbg):
        if i["name"] == name:
            return i
    return None


def close_menu(dbg):
    for _ in range(4):
        if not dbg.ctxmenu().get("open"):
            return
        dbg.key("0x1b")


def rows(m):
    return [r["label"] for r in m.get("rows", [])]


def row(m, label):
    for r in m.get("rows", []):
        if r["label"] == label:
            return r
    return None


def lum(px):
    return (px[0] * 299 + px[1] * 587 + px[2] * 114) // 1000


def mean_lum(img, box):
    x0, y0, x1, y1 = box
    tot = n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            tot += lum(img.getpixel((x, y)))
            n += 1
    return tot / max(1, n)


def hl_strip(ic):
    """A band inside the highlight but outside the art and the label: the
    two px above the icon tile, across the cell. The glass lands here and
    nothing else does."""
    return (ic["x"] - 16, ic["y"] - 3, ic["x"] + ic["w"] + 16, ic["y"] - 1)


def empty_clipboard(dbg):
    """ESTABLISH "nothing to paste" rather than inherit it: a cut is spent
    by its paste (desktop.c, the File Manager's rule), and pasting onto
    the desktop a file already there moves nothing."""
    dbg.send("sh touch /home/desktop/clipreset.txt")
    time.sleep(1.5)
    ic = icon(dbg, "clipreset.txt")
    if ic:
        dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
        dbg.key("x", mods="ctrl")
        dbg.key("v", mods="ctrl")
        dbg.click(*EMPTY)
    dbg.send("sh rm -f /home/desktop/clipreset.txt")
    time.sleep(1.0)


def check_background_menu(dbg, res):
    close_menu(dbg)
    empty_clipboard(dbg)
    dbg.rclick(*EMPTY)
    m = dbg.ctxmenu()
    res.check("background menu rows, in order", rows(m) == BG_ROWS, f"rows={rows(m)}")
    p = row(m, "Paste")
    res.check("Paste is greyed with nothing to paste", bool(p) and p.get("disabled") is True,
              f"paste={p}")
    close_menu(dbg)
    # Copy a file icon, then the same row must be live.
    dbg.send("sh touch /home/desktop/menutest.txt")
    time.sleep(1.5)
    ic = icon(dbg, "menutest.txt")
    if ic:
        dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
        dbg.key("c", mods="ctrl")
        dbg.click(*EMPTY)
    dbg.rclick(*EMPTY)
    p = row(dbg.ctxmenu(), "Paste")
    res.check("...and live once a file is copied", bool(ic) and bool(p) and p.get("disabled") is False,
              f"icon={bool(ic)} paste={p}")
    close_menu(dbg)


def desktop_apps(dbg):
    """The registry's names the DESKTOP lists (`gui apps`' show_in)."""
    out = dbg.send("gui apps") or ""
    names = []
    for line in out.splitlines():
        if "show_in=desktop" in line and "resizable=" in line:
            names.append(line[:line.index("resizable=")].strip())
    return names


def check_open_by_category(dbg, res):
    dbg.rclick(*EMPTY)
    m = dbg.ctxmenu()
    o = row(m, "Open")
    cats, seen = [], set()
    if o:
        dbg.send(f"gui warp {m['x'] + 20} {o['cy']}")
        dbg.settle()
        sub = dbg.ctxmenu().get("sub") or {}
        cats = [r["label"] for r in sub.get("rows", [])]
        for r in sub.get("rows", []):
            dbg.send(f"gui warp {sub['x'] + 20} {r['cy']}")
            dbg.settle()
            for a in (dbg.ctxmenu().get("sub2") or {}).get("rows", []):
                seen.add(a["label"])
    res.check("Open > lists categories, not apps",
              "Utilities" in cats and "System" in cats and "Notepad" not in cats, f"cats={cats}")
    want = set(desktop_apps(dbg))
    missing = sorted(want - seen)
    res.check("...and their apps are EVERY app the desktop lists, past the old cap of 16",
              len(want) > 16 and not missing, f"listed={len(want)} reached={len(seen)} missing={missing}")
    close_menu(dbg)


def check_keyboard(dbg, res):
    dbg.rclick(*EMPTY)
    dbg.key("i")                         # the unique I: Icon size >, opened
    m = dbg.ctxmenu()
    opened = bool(m.get("sub")) and "Large" in rows(m.get("sub") or {})
    res.check("a letter opens Icon size >", opened, f"sub={m.get('sub')}")
    dbg.key("l")                         # Large: commits
    time.sleep(0.6)
    size = dbg.json("gui icons --json").get("size_word")
    res.check("...and a second commits Large", size == "large" and not dbg.ctxmenu().get("open"),
              f"size={size}")
    dbg.rclick(*EMPTY)
    dbg.key("i")
    dbg.key("0x1b")
    m = dbg.ctxmenu()
    res.check("Esc closes ONE level", m.get("open") and not m.get("sub"), f"menu={m}")
    dbg.key("0x1b")
    res.check("...and the next closes the menu", not dbg.ctxmenu().get("open"))
    dbg.rclick(*EMPTY)
    dbg.key("i")
    dbg.key("m")                         # back to Medium
    time.sleep(0.6)


def check_icon_menu(dbg, res):
    ic = icon(dbg, "menutest.txt")
    if not ic:
        res.check("icon menu: the test file is on the desktop", False)
        return
    cx, cy = ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2
    dbg.click(*EMPTY)
    dbg.rclick(cx, cy)
    m = dbg.ctxmenu()
    strip = [row(m, n) for n in ("Cut", "Copy", "Rename", "Delete")]
    ok = all(strip) and len({r["y"] for r in strip}) == 1 and \
        [r["cx"] for r in strip] == sorted({r["cx"] for r in strip})
    res.check("Cut/Copy/Rename/Delete share one strip, left to right", ok,
              f"strip={[(r or {}).get('cx') for r in strip]} ys={[(r or {}).get('y') for r in strip]}")
    res.check("...then Open and Properties as rows",
              rows(m)[-2:] == ["Open", "Properties"], f"rows={rows(m)}")
    res.check("Rename is live for one icon", (row(m, "Rename") or {}).get("disabled") is False)
    close_menu(dbg)
    # Two selected (a band across the first column): Rename greys out.
    first = sorted(icons(dbg), key=lambda i: (i["x"], i["y"]))[:2]
    if len(first) == 2:
        x0 = first[0]["x"] + first[0]["w"] + 30
        dbg.drag(x0, first[0]["y"] - 8, first[0]["x"] - 8, first[1]["y"] + first[1]["h"] - 4)
        sel = [i["name"] for i in icons(dbg) if i["selected"]]
        dbg.rclick(first[0]["x"] + first[0]["w"] // 2, first[0]["y"] + first[0]["w"] // 2)
        r = row(dbg.ctxmenu(), "Rename")
        res.check("Rename greys out for a selection of two",
                  len(sel) >= 2 and bool(r) and r.get("disabled") is True, f"selected={sel} rename={r}")
        close_menu(dbg)
    dbg.click(*EMPTY)


def check_rename(dbg, res):
    dbg.send("sh mkdir '/home/desktop/Rename me'")
    time.sleep(1.5)
    ic = icon(dbg, "Rename me")
    if not ic:
        res.check("rename: the folder is on the desktop", False)
        return
    dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
    dbg.key("0xf789")                       # F2
    res.check("F2 opens the caption for editing", (icon(dbg, "Rename me") or {}).get("renaming") is True)
    dbg.key("q")
    dbg.key("0x1b")
    time.sleep(0.5)
    res.check("Esc abandons it", icon(dbg, "Rename me") is not None and
              not any(i.get("renaming") for i in icons(dbg)))
    ic = icon(dbg, "Rename me")
    dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
    dbg.key("0xf789")
    for c in "Done":
        dbg.key(c)
    dbg.key("0x0a")
    time.sleep(1.5)
    listing = dbg.send("sh ls /home/desktop") or ""
    res.check("Enter renames the folder on disk", "Done" in listing and "Rename me" not in listing,
              f"ls={listing.strip()[:120]}")
    dbg.send("sh rm -r /home/desktop/Done")
    # A launcher: its caption is Name=, and the file keeps its name.
    ic = icon(dbg, "Terminal")
    if ic:
        dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
        dbg.key("0xf789")
        for c in "Shell":
            dbg.key(c)
        dbg.key("0x0a")
        time.sleep(1.5)
        text = dbg.send("sh cat /home/desktop/terminal.desktop") or ""
        res.check("a launcher's rename rewrites Name= and keeps the rest",
                  "Name=Shell" in text and "Exec=" in text and icon(dbg, "Shell") is not None,
                  f"file={text.strip()[:120]}")
        dbg.send("sh rm /home/desktop/terminal.desktop")   # a fresh disk restores it
    else:
        res.check("rename: the Terminal launcher is on the desktop", False,
                  "a fresh disk seeds it; a rerun on the same guest has renamed it")


def check_properties(dbg, res):
    ic = icon(dbg, "menutest.txt")
    if not ic:
        return
    before = len(dbg.windows())
    dbg.click(ic["x"] + ic["w"] // 2, ic["y"] + ic["w"] // 2)
    dbg.key("0x0a", mods="alt")
    titles = []
    for _ in range(20):
        titles = [w["title"] for w in dbg.windows()]
        if any("Properties" in t for t in titles):
            break
        time.sleep(0.3)
    res.check("Alt+Enter opens Properties for the selection",
              any(t == "menutest.txt Properties" for t in titles) and len(titles) > before,
              f"windows={titles}")
    for i, w in enumerate(dbg.windows()):
        if "Properties" in w["title"]:
            dbg.send(f"gui close {i}")
            time.sleep(0.5)
            break


def check_hover_and_glass(dbg, qmp, res, tmp):
    dbg.click(*EMPTY)
    ics = sorted(icons(dbg), key=lambda i: (i["x"], i["y"]))
    if len(ics) < 3:
        res.check("hover: three icons to work with", False)
        return
    a, ctl = ics[0], ics[2]
    dbg.warp_cursor(qmp, *EMPTY)
    dbg.settle()
    rest = shot(qmp, tmp, "rest.png")
    dbg.warp_cursor(qmp, a["x"] + a["w"] // 2, a["y"] + a["w"] // 2)
    dbg.settle()
    hov = shot(qmp, tmp, "hover.png")
    state = {i["name"]: i.get("hovered") for i in icons(dbg)}
    res.check("the guest reports the hovered icon", state.get(a["name"]) is True and
              sum(1 for v in state.values() if v) == 1, f"hovered={state}")
    d_hover = mean_lum(hov, hl_strip(a)) - mean_lum(rest, hl_strip(a))
    d_ctl = mean_lum(hov, hl_strip(ctl)) - mean_lum(rest, hl_strip(ctl))
    res.check("its highlight brightens, a second icon's does not",
              d_hover > 4 and abs(d_ctl) < 1.0, f"hover={d_hover:.1f} control={d_ctl:.1f}")
    dbg.warp_cursor(qmp, *EMPTY)
    dbg.settle()
    res.check("leaving clears it", not any(i.get("hovered") for i in icons(dbg)))
    # GLASS: two icons over different parts of the wallpaper's gradient,
    # both selected. Each must read as ITS OWN wallpaper blended with
    # white at the selection's alpha -- a solid fill is one colour for
    # both and cannot match two different predictions.
    top, low = ics[0], max(ics, key=lambda i: i["y"])
    dbg.drag(top["x"] + top["w"] + 30, top["y"] - 8, top["x"] - 8, low["y"] + 8, steps=24)
    dbg.warp_cursor(qmp, *EMPTY)
    dbg.settle()
    sel = shot(qmp, tmp, "selected.png")
    chosen = {i["name"] for i in icons(dbg) if i["selected"]}

    def mean_rgb(img, box):
        x0, y0, x1, y1 = box
        px = [img.getpixel((x, y)) for y in range(y0, y1) for x in range(x0, x1)]
        return tuple(sum(p[k] for p in px) / len(px) for k in range(3))

    errs, unders = [], []
    for ic in (top, low):
        under = mean_rgb(rest, hl_strip(ic))
        got = mean_rgb(sel, hl_strip(ic))
        want = tuple(u + (255 - u) * GLASS_FILL / 255 for u in under)
        errs.append(max(abs(got[k] - want[k]) for k in range(3)))
        unders.append(under)
    apart = max(abs(unders[0][k] - unders[1][k]) for k in range(3))
    res.check("the selection is glass: each icon's own wallpaper, lightened",
              {top["name"], low["name"]} <= chosen and apart > 10 and max(errs) <= 6,
              f"selected={sorted(chosen)} wallpaper-apart={apart:.0f} error={[round(e, 1) for e in errs]}")
    dbg.click(*EMPTY)


def check_app_popup_corners(dbg, qmp, res, tmp):
    dbg.send("gui open Notepad")
    w = None
    for _ in range(20):
        w = dbg.window("untitled")
        if w:
            break
        time.sleep(0.3)
    if not w:
        res.check("app popup: Notepad opened", False)
        return
    menu = dbg.widgets("untitled").get("menu") or {}
    scr = menu.get("screen") or {}
    mx = scr.get("x", w["content"]["x"]) + 16
    my = scr.get("y", w["content"]["y"]) + 8
    dbg.click(mx, my)
    dbg.warp_cursor(qmp, w["x"] + w["w"] - 20, w["y"] + w["h"] - 20)
    dbg.settle()
    pop = None
    for win in dbg.windows():
        if win.get("popup"):
            pop = win
    img = shot(qmp, tmp, "popup.png")
    if not pop:
        res.check("app popup: the File menu opened as a surface", False, f"windows={dbg.windows()}")
    else:
        # Uncut, the corner pixel IS the card's edge (the widget paints a
        # square card); cut, it is the scene behind. Darker-than-inside
        # is not the test -- the edge is darker than inside too, which is
        # what the positive control showed.
        corner = img.getpixel((pop["x"], pop["y"] + pop["h"] - 1))
        edge = img.getpixel((pop["x"] + pop["w"] // 2, pop["y"] + pop["h"] - 1))
        res.check("an app's popup has its corners cut by the compositor",
                  max(abs(corner[k] - edge[k]) for k in range(3)) > 12,
                  f"corner={corner} edge={edge}")
    dbg.key("0x1b")
    for i, win in enumerate(dbg.windows()):
        if win["title"] == "untitled":
            dbg.send(f"gui close {i}")
            break
    time.sleep(0.5)


def run(dbg, qmp, tmp, res):
    check_background_menu(dbg, res)
    check_open_by_category(dbg, res)
    check_keyboard(dbg, res)
    check_icon_menu(dbg, res)
    check_rename(dbg, res)
    check_properties(dbg, res)
    check_hover_and_glass(dbg, qmp, res, tmp)
    check_app_popup_corners(dbg, qmp, res, tmp)
    dbg.send("sh rm -f /home/desktop/menutest.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "desktop_menu_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = args.shot or tempfile.mkdtemp(prefix="desktop-menu-")
    os.makedirs(tmp, exist_ok=True)
    print("desktop_menu_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\ndesktop_menu_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
