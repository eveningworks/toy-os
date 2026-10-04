#!/usr/bin/env python3
"""tools/start_settings_test.py -- the Start menu's settings (Desktop > Start).

Sets each `desktop.start_*` key through `sh config`, opens the menu and
asks the WM (`gui menu --json`) what it built, with pixels where JSON
cannot answer. The settings are read once per OPEN, so every check
reopens. What a broken version would still pass, per setting:

  * start_list: Detailed is one column of tall rows; Compact is one
    column of SHORTER rows showing MORE apps, on a NARROWER card of the
    same height whose footer still holds its buttons; Grid is several
    columns, more apps again, on the Detailed card. A grid that reported cells but drew a list fails the pixel
    check (the first cell's rect holds an icon, which a list row's left
    end at that spot does not).
  * Grid keyboard: Down selects the first cell, Right the next, Down a
    line on, Up twice back to no selection -- where Right moves the
    FOLDER again. A grid that kept the list's keys passes Down alone.
  * start_power: the footer's actions are exactly the chosen tail, and
    search no longer finds a hidden one.
  * start_opens: each choice opens on its folder; `last` reopens on a
    folder chosen by hand, which a menu that always reset would fail.
  * start_recent: off removes the Recent folder AND the launches recorded
    in /etc/start-menu.conf (read back with `sh cat`, an independent
    path); on records again.
  * start_hover: on opens a folder the pointer rests on; off does not --
    the pair, so a menu that ignored the setting fails one of them.

Positive controls (tools/mutate.py), each reddens its own checks only:
  * `if (L->cols < 1) L->cols = 1;` -> `L->cols = 1;` in layout(): the
    grid checks.
  * `return g_cfg.power == SM_POWER_SHUTDOWN ? 1` -> `return 0 ? 1` in
    foot_count(): the power checks.

Usage:
    python3 tools/start_settings_test.py --instance 0
"""
import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402
from harness import Results  # noqa: E402

KEY_SUPER = "0xf795"
KEY_ESC = "0x1b"
KEY_DOWN = "0xf781"
KEY_UP = "0xf780"
KEY_RIGHT = "0xf785"
KEYS = ("start_list", "start_opens", "start_recent", "start_hover", "start_power")


def rows_of(menu, kind):
    return [r for r in menu.get("rows", []) if r["kind"] == kind]


def selected_folder(menu):
    return next((c["label"] for c in rows_of(menu, "category") if c["selected"]), None)


def set_key(dbg, key, value):
    if value is None:
        dbg.send(f"sh config unset desktop.{key}")
    else:
        dbg.send(f"sh config set desktop.{key} {value}")


def close_menu(dbg):
    for _ in range(4):
        if not dbg.menu().get("open"):
            return
        dbg.key(KEY_ESC)
        time.sleep(0.2)


def reopen(dbg, tries=6):
    """Close, then open with Super (its gesture is on the release), so the
    menu re-reads its settings; returns the menu's report."""
    close_menu(dbg)
    for _ in range(tries):
        if dbg.menu().get("open"):
            break
        dbg.key(KEY_SUPER)
        dbg.key(KEY_SUPER, mods="up")
        time.sleep(0.3)
    dbg.settle()
    return dbg.menu()


def ink(qmp, tmp, name, r):
    """Pixels in rect r that differ from its top-left corner's colour."""
    from PIL import Image
    p = os.path.join(tmp, name)
    qmp.stable_pixels(p)
    im = Image.open(p).convert("RGB").crop((r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"]))
    bg = im.getpixel((1, 1))
    raw = im.tobytes()
    return sum(1 for i in range(0, len(raw), 3)
               if sum(abs(raw[i + c] - bg[c]) for c in range(3)) > 60)


def run(dbg, qmp, tmp, res):
    for k in KEYS:
        set_key(dbg, k, None)
    dbg.warp_cursor(qmp, 900, 200)   # off the menu: hover is its own check

    # --- start_list ----------------------------------------------------
    m = reopen(dbg)
    det = rows_of(m, "app")
    foot = (m["w"], m["h"])
    res.check("Detailed (the default) is one column",
              len(det) > 1 and len({r["x"] for r in det}) == 1, f"{[(r['x'], r['h']) for r in det[:3]]}")
    set_key(dbg, "start_list", "compact")
    m = reopen(dbg)
    comp = rows_of(m, "app")
    res.check("Compact is one column of shorter rows, more of them",
              len({r["x"] for r in comp}) == 1 and comp and det and comp[0]["h"] < det[0]["h"]
              and len(comp) > len(det),
              f"compact {len(comp)} x {comp[0]['h'] if comp else '-'}, detailed {len(det)} x {det[0]['h'] if det else '-'}")
    acts = rows_of(m, "action")
    res.check("...on a narrower card of the same height, its footer buttons inside it",
              m["w"] < foot[0] - 100 and m["h"] == foot[1] and len(acts) == 3
              and all(m["x"] <= r["x"] and r["x"] + r["w"] <= m["x"] + m["w"] for r in acts),
              f"{(m['w'], m['h'])} vs {foot}, actions {[(r['x'], r['w']) for r in acts]}")
    set_key(dbg, "start_list", "grid")
    m = reopen(dbg)
    grid = rows_of(m, "app")
    cols = len({r["x"] for r in grid})
    res.check("Grid is several columns, more apps than Compact shows",
              cols >= 3 and len(grid) > len(comp), f"{cols} columns, {len(grid)} cells")
    res.check("...on the Detailed card", (m["w"], m["h"]) == foot, f"{(m['w'], m['h'])} vs {foot}")
    # DRAWN as a grid: the second cell's rect holds its icon and label. In
    # a list that rect is the right end of the first row's pill -- blank.
    if len(grid) > 1:
        n = ink(qmp, tmp, "grid.png", grid[1])
        res.check("a grid cell is drawn (its icon and name)", n > 200, f"{n} inked pixels in {grid[1]}")
    # Grid keyboard.
    dbg.key(KEY_DOWN)
    s0 = [i for i, r in enumerate(rows_of(dbg.menu(), "app")) if r["selected"]]
    dbg.key(KEY_RIGHT)
    s1 = [i for i, r in enumerate(rows_of(dbg.menu(), "app")) if r["selected"]]
    dbg.key(KEY_DOWN)
    s2 = [i for i, r in enumerate(rows_of(dbg.menu(), "app")) if r["selected"]]
    res.check("Grid: Down, Right, Down walk the cells by column and by line",
              s0 == [0] and s1 == [1] and s2 == [1 + cols], f"{s0} {s1} {s2} with {cols} columns")
    before = selected_folder(dbg.menu())
    dbg.key(KEY_UP)
    dbg.key(KEY_UP)
    m = dbg.menu()
    none = not any(r["selected"] for r in rows_of(m, "app"))
    dbg.key(KEY_RIGHT)
    after = selected_folder(dbg.menu())
    res.check("...Up from the top line leaves the cells, and Right moves the folder",
              none and after and after != before, f"no selection {none}, folder {before} -> {after}")
    set_key(dbg, "start_list", None)

    # --- start_power ---------------------------------------------------
    for value, want in (("shutdown", ["Shutdown"]), ("restart", ["Restart", "Shutdown"]),
                        (None, ["Exit to shell", "Restart", "Shutdown"])):
        set_key(dbg, "start_power", value)
        m = reopen(dbg)
        got = [r["label"] for r in rows_of(m, "action")]
        res.check(f"Power buttons {value or 'all'}: the footer is {want}", got == want, f"{got}")
    # The pair: search finds Exit to shell while it is offered, and not
    # once it is hidden -- the first half is what makes an empty result
    # mean something.
    found = {}
    for value in (None, "shutdown"):
        set_key(dbg, "start_power", value)
        reopen(dbg)
        for ch in "exit":
            dbg.key(ch)
        found[value] = [r["label"] for r in rows_of(dbg.menu(), "app")]
    res.check("...and search finds only the actions offered",
              "Exit to shell" in found[None] and "Exit to shell" not in found["shutdown"], f"{found}")
    set_key(dbg, "start_power", None)

    # --- start_recent, then start_opens --------------------------------
    close_menu(dbg)
    dbg.open_app("Calculator")       # a launch: Recent exists from here
    time.sleep(0.5)
    m = reopen(dbg)
    cats = [c["label"] for c in rows_of(m, "category")]
    conf = dbg.send("sh cat /etc/start-menu.conf") or ""
    res.check("a launch makes a Recent folder, recorded in the file",
              "Recent" in cats and "run.calculator" in conf, f"{cats} {conf.strip()[:200]!r}")
    set_key(dbg, "start_opens", "all")
    res.check("Opens on All Apps", selected_folder(reopen(dbg)) == "All Apps",
              f"{selected_folder(dbg.menu())}")
    set_key(dbg, "start_opens", "recent")
    res.check("Opens on Recent", selected_folder(reopen(dbg)) == "Recent",
              f"{selected_folder(dbg.menu())}")
    set_key(dbg, "start_opens", "last")
    reopen(dbg)
    dbg.menu_select_folder("Games")
    res.check("Opens on the last folder used",
              selected_folder(reopen(dbg)) == "Games", f"{selected_folder(dbg.menu())}")
    set_key(dbg, "start_opens", None)

    set_key(dbg, "start_recent", "off")
    m = reopen(dbg)
    cats = [c["label"] for c in rows_of(m, "category")]
    conf = dbg.send("sh cat /etc/start-menu.conf") or ""
    res.check("Recent apps off: no Recent folder", "Recent" not in cats, f"{cats}")
    res.check("...and the recorded launches are gone from the file",
              "run.calculator" not in conf.lower(), f"{conf.strip()[:200]!r}")
    close_menu(dbg)
    dbg.open_app("Calculator")
    time.sleep(0.5)
    conf = dbg.send("sh cat /etc/start-menu.conf") or ""
    res.check("...and a launch while off is not recorded", "run." not in conf, f"{conf.strip()[:200]!r}")
    set_key(dbg, "start_recent", None)

    # --- start_hover ---------------------------------------------------
    for value in ("on", None):
        set_key(dbg, "start_hover", value)
        m = reopen(dbg)
        start = selected_folder(m)
        target = next((c for c in rows_of(m, "category") if c["label"] not in (start, "Recent")), None)
        if not target:
            res.check("a second folder to hover", False, f"{rows_of(m, 'category')}")
            continue
        dbg.warp_cursor(qmp, target["cx"], target["cy"])
        time.sleep(0.8)
        now = selected_folder(dbg.menu())
        if value == "on":
            res.check("Open folders on hover: resting on a folder opens it",
                      now == target["label"], f"{start} -> {now}, hovered {target['label']}")
        else:
            res.check("...and off (the default), hovering opens nothing",
                      now == start, f"{start} -> {now}, hovered {target['label']}")
        dbg.warp_cursor(qmp, 900, 200)

    close_menu(dbg)
    for k in KEYS:
        set_key(dbg, k, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "start_settings_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    res = Results()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, tempfile.mkdtemp(prefix="start_settings_"), res)
    return res.finish("start_settings_test")


if __name__ == "__main__":
    sys.exit(main())
