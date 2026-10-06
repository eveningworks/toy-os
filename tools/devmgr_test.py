#!/usr/bin/env python3
"""The Device Manager: its tree is drawn, its views switch, and Disable works.

WHAT IT CHECKS, and what a broken version would still pass:

  - the window opens and REPORTS its layout (tree, view tabs, the
    toggle button, the keep box) -- a window that opened and laid out
    nothing would fail here;
  - the tree and the properties pane are DRAWN: ink in each rect, read
    from a settled screenshot. "It responds" is not "it is drawn"
    (CLAUDE.md) -- a tree that reports rows and paints none passes every
    log check;
  - the ICONS are drawn: saturated pixels in the tree, which only the
    icons have -- outside the selected row, which the Device Manager
    fills in the accent (uui_tree's strong selection);
  - "By connection" switches the view and KEEPS the selected device;
  - Disable on the network card: the confirm dialog opens, its Disable
    button unbinds the card -- checked through `devctl list`, an
    INDEPENDENT reader, not the app's own report -- and Enable binds it
    back without asking;
  - Cancel leaves the card bound, and the storage controller's toggle
    opens no dialog at all (its driver cannot let go);
  - a desktop context menu opened OVER the tree takes the pointer: no
    tree row lights up under it. Measured beside the menu, on the rows'
    part the menu does not cover, with the pointer on the tree next to
    the menu as the control that the measurement sees a highlight.

Geometry is the app's own report (`devmgr: layout ...`, `devmgr:
selected <id> view <v>`), never re-derived here. Rows are clicked at
`tree.y + row * tree.row_h`, which holds because the tree opens fully
expanded and scrolled to the top.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui, changed_rows   # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEVMGR = "/bin/wm/system/devmgr"
TITLE = "Device Manager"
K_ESC = "0x1b"



_res = Results()
check = _res.check
checks = _res.rows


_lay = {}


def layout(dbg):
    """Drain the app's report into the merged layout. `rects` by name,
    plus the tree's row metrics, the dialog's buttons and the selection."""
    for line in dbg.logs("devmgr:", clear=True):
        m = re.search(r"devmgr: layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            _lay[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        m = re.search(r"devmgr: layout tree\.(row_h|rows|selected) (-?\d+)", line)
        if m:
            _lay["tree." + m.group(1)] = int(m.group(2))
        m = re.search(r"devmgr: layout (ask\.button|view\.slot) (\d+) (-?\d+) (-?\d+) (\d+) (\d+)", line)
        if m:
            _lay[f"{m.group(1)}{m.group(2)}"] = tuple(int(v) for v in m.groups()[2:])
        m = re.search(r"devmgr: selected (\S+) view (\w+)(?: format (\d))?", line)
        if m:
            _lay["selected"], _lay["view"] = m.group(1), m.group(2)
            _lay["format"] = int(m.group(3) or 0)   # a sound card's Format panel shown
    return _lay


def wait_layout(dbg, ok, timeout=6.0):
    deadline = time.time() + timeout
    lay = layout(dbg)
    while not ok(lay) and time.time() < deadline:
        time.sleep(0.1)
        lay = layout(dbg)
    return lay


def window(dbg):
    wins = [w for w in dbg.windows() if w["title"] == TITLE]
    return wins[-1] if wins else None


def click(dbg, win, x, y):
    c = win["content"]
    dbg.send("gui click %d %d" % (c["x"] + x, c["y"] + y))
    dbg.settle(0.5)


def devices(dbg):
    """`devctl list`, as {id: (type, driver, state)} -- the independent
    reader: the app's report says what the APP believes."""
    out = {}
    for line in dbg.send("sh devctl").splitlines():
        parts = line.split()
        if len(parts) >= 4 and re.match(r"(pci|usb|ps2|cpu):", parts[0]):
            # TYPE is two words in a fixed 12-column field ("Network adap").
            m = re.match(r"(\S+)\s+(.{12})\s+(\S+)\s+(\S+)", line)
            if m:
                out[m.group(1)] = (m.group(2).strip(), m.group(3), m.group(4))
    return out


def select_device(dbg, win, dev_id):
    """Click rows from the top until the app reports `dev_id` selected."""
    lay = layout(dbg)
    tx, ty, _, _ = lay["tree"]
    for row in range(lay.get("tree.rows", 0)):
        click(dbg, win, tx + 40, ty + row * lay["tree.row_h"] + lay["tree.row_h"] // 2)
        lay = wait_layout(dbg, lambda l: l.get("tree.selected") == row, 2.0)
        if lay.get("selected") == dev_id:
            return True
    return False


def ink(im, rect, pred):
    """Matching pixels in `rect`, and the span of columns they fall in."""
    x, y, w, h = rect
    crop = im.crop((x, y, x + w, y + h)).convert("RGB")
    n, cols = 0, set()
    for cy in range(h):
        for cx in range(w):
            if pred(crop.getpixel((cx, cy))):
                n += 1
                cols.add(cx)
    return n, (max(cols) - min(cols) + 1) if cols else 0


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "devmgr_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("device manager (lib/udevice.c, uui_tree icons, disable/enable)")

    # Single-instance: a window left open would take the spawn, and it
    # read the layout-log setting before enter_gui() turned it on.
    old = window(dbg)
    if old:
        dbg.send(f"gui close {old['z']}")
        dbg.settle(1.0)
    dbg.logs("devmgr:", clear=True)
    win = dbg.spawn(DEVMGR, TITLE)
    lay = wait_layout(dbg, lambda l: all(k in l for k in ("tree", "view", "toggle", "keep"))
                      and "selected" in l)
    if not check("Device Manager opens and reports its layout", win and "tree" in lay,
                 f"reported {sorted(k for k in lay if '.' not in k)}"):
        return finish()
    check("...with a device selected, in the by-type view",
          lay.get("selected", "-") != "-" and lay.get("view") == "type",
          f"selected={lay.get('selected')} view={lay.get('view')}")

    # DRAWN, not just reported.
    from PIL import Image
    shot = os.path.join(args.tmp, f"devmgr_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]
    tx, ty, tw, th = lay["tree"]
    tree_r = (c["x"] + tx, c["y"] + ty, tw, th)
    sx = lay["split"][0] + lay["split"][2]
    pane_r = (c["x"] + sx, c["y"] + ty, c["w"] - sx, th // 2)
    dark = lambda p: sum(p) < 300
    colour = lambda p: max(p) - min(p) > 60
    tree_ink, _ = ink(im, tree_r, dark)
    pane_ink, _ = ink(im, pane_r, dark)
    # Every row but the selected one, which is accent-filled on purpose.
    sel_y = c["y"] + ty + lay.get("tree.selected", 0) * lay["tree.row_h"]
    above = (tree_r[0], tree_r[1], tree_r[2], max(0, sel_y - tree_r[1]))
    below_y = sel_y + lay["tree.row_h"]
    below = (tree_r[0], below_y, tree_r[2], max(0, tree_r[1] + tree_r[3] - below_y))
    i1, s1 = ink(im, above, colour)
    i2, s2 = ink(im, below, colour)
    icons, span = i1 + i2, max(s1, s2)
    check("the tree draws its rows", tree_ink > 500, f"{tree_ink} dark px")
    check("the properties pane draws the selected device", pane_ink > 300, f"{pane_ink} dark px")
    # In a band no wider than two indents of icons: colour anywhere else
    # (a saturated selection wash) is not an icon.
    check("the tree draws its icons, and only in the icon gutter",
          icons > 200 and span <= 3 * lay["tree.row_h"],
          f"{icons} saturated px across {span} columns")

    # By connection, and back -- the SAME device stays selected.
    before = lay.get("selected")
    vx, vy, vw, vh = lay.get("view.slot1", (0, 0, 0, 0))
    click(dbg, win, vx + vw // 2, vy + vh // 2)
    lay = wait_layout(dbg, lambda l: l.get("view") == "connection")
    check("\"By connection\" switches the view and keeps the device",
          lay.get("view") == "connection" and lay.get("selected") == before,
          f"view={lay.get('view')} selected {before} -> {lay.get('selected')}")
    vx, vy, vw, vh = lay.get("view.slot0", (0, 0, 0, 0))
    click(dbg, win, vx + vw // 2, vy + vh // 2)
    wait_layout(dbg, lambda l: l.get("view") == "type")

    devs = devices(dbg)
    nic = next((d for d, v in devs.items() if v[0].startswith("Network") and v[1] != "-"), None)
    store = next((d for d, v in devs.items() if v[0].startswith("Storage") and v[1] != "-"), None)
    if not check("devctl lists a network card and a storage controller with drivers",
                 nic and store, f"nic={nic} storage={store}"):
        return finish()
    nic_driver = devs[nic][1]
    bx, by, bw, bh = lay["toggle"]

    # Cancel first: the card must stay bound.
    select_device(dbg, win, nic)
    click(dbg, win, bx + bw // 2, by + bh // 2)
    lay = wait_layout(dbg, lambda l: l.get("ask", (0, 0, 0, 0))[2] > 0)
    check("Disable asks first", lay.get("ask", (0, 0, 0, 0))[2] > 0, f"ask={lay.get('ask')}")
    # A CLOSE WHILE IT ASKS IS REFUSED: the Device Manager has no
    # on_close, and uapp's default holds the close while a question is
    # open IN the window (uapp_desc.on_close) -- the answer decides.
    dbg.logs("uapp: close refused", clear=True)
    me = next((w for w in dbg.windows() if w.get("title") == TITLE), None)
    if me:
        dbg.send(f"gui close {me['z']}")
    refused = None
    deadline = time.time() + 4
    while time.time() < deadline and not refused:
        refused = dbg.logs("uapp: close refused", clear=False)
        time.sleep(0.2)
    check("a close while Disable asks is refused, the window and question kept",
          bool(refused) and any(w.get("title") == TITLE for w in dbg.windows()) and
          layout(dbg).get("ask", (0, 0, 0, 0))[2] > 0,
          f"refused {refused}, windows {[w['title'] for w in dbg.windows()]}")
    dbg.send(f"gui key {K_ESC}")
    lay = wait_layout(dbg, lambda l: l.get("ask", (0, 0, 0, 0))[2] == 0)
    check("Escape closes the question", lay.get("ask", (0, 0, 0, 0))[2] == 0, f"ask={lay.get('ask')}")
    check("...and leaves the card bound", devices(dbg).get(nic, ("", "", ""))[1] == nic_driver,
          f"{nic}: {devices(dbg).get(nic)}")

    # Disable for real, through the dialog's own button.
    click(dbg, win, bx + bw // 2, by + bh // 2)
    lay = wait_layout(dbg, lambda l: l.get("ask", (0, 0, 0, 0))[2] > 0 and "ask.button0" in l)
    ax, ay, aw, ah = lay.get("ask.button0", (0, 0, 0, 0))
    click(dbg, win, ax + aw // 2, ay + ah // 2)
    time.sleep(1.0)
    after = devices(dbg).get(nic, ("", "", ""))
    check("the dialog's Disable unbinds the card (devctl agrees)",
          after[1] == "-" and after[2] == "disabled", f"{nic}: {after}")
    lay = wait_layout(dbg, lambda l: l.get("selected") == nic)
    check("...and the card stays selected", lay.get("selected") == nic, f"selected={lay.get('selected')}")

    # Enable: no question, and the driver comes back.
    click(dbg, win, bx + bw // 2, by + bh // 2)
    time.sleep(1.5)
    back = devices(dbg).get(nic, ("", "", ""))
    check("Enable binds the same driver again, without asking",
          back[1] == nic_driver and back[2] == "ok", f"{nic}: {back} (wanted {nic_driver})")

    # The storage controller: nothing to ask, nothing done.
    select_device(dbg, win, store)
    dbg.logs("devmgr:", clear=True)
    click(dbg, win, bx + bw // 2, by + bh // 2)
    lay = wait_layout(dbg, lambda l: False, 1.0)
    check("the storage controller's toggle opens no dialog",
          lay.get("ask", (0, 0, 0, 0))[2] == 0, f"ask={lay.get('ask')}")
    check("...and it keeps its driver", devices(dbg).get(store, ("", "", ""))[1] == devs[store][1],
          f"{store}: {devices(dbg).get(store)}")

    overlay_hover(dbg, qmp, win, lay, args.tmp)

    w = window(dbg)
    if w:
        dbg.send(f"gui close {w['z']}")
    return finish()


def overlay_hover(dbg, qmp, win, lay, tmp):
    """A desktop menu over the tree: the rows under it must not hover."""
    c = win["content"]
    if win["x"] < 40:
        check("a desktop menu over the tree takes the pointer", False,
              f"no desktop left of the window to right-click (x={win['x']})")
        return
    tx, ty, tw, th = lay["tree"]
    dbg.send(f"gui rclick {win['x'] - 30} {c['y'] + ty + 20}")
    dbg.settle(1.0)
    m = re.search(r"x=(-?\d+) y=(-?\d+) w=(\d+) item_h=(\d+) rows=(\d+)",
                  dbg.send("gui ctxmenu"))
    if not check("the desktop menu opened over the tree", m is not None, "no menu"):
        return
    mx, my, mw, ih, rows = (int(v) for v in m.groups())
    hover_y = my + ih * min(rows - 1, 5) + ih // 2
    right = c["x"] + tx + tw - 6
    band = (mx + mw + 12, c["y"] + ty, right, c["y"] + ty + th)
    if band[2] - band[0] < 20:
        check("a desktop menu over the tree takes the pointer", False,
              f"the menu covers the tree's width: menu {mx}..{mx + mw}, tree to {right}")
        return
    rest = (mx + 4, my - 8)
    _, on_tree = dbg.hover_frames(qmp, tmp, rest_at=rest,
                                  hover_at=(mx + mw + 12, hover_y), prefix="dm_ctl")
    base, _ = dbg.hover_frames(qmp, tmp, rest_at=rest, hover_at=rest, prefix="dm_base")
    ctl = changed_rows(base, on_tree, band)
    check("control: beside the menu, the tree row under the pointer lights up",
          bool(ctl["rows"]), f"band {ctl.get('band')}")
    _, on_menu = dbg.hover_frames(qmp, tmp, rest_at=rest,
                                  hover_at=(mx + mw - 10, hover_y), prefix="dm_menu")
    seen = changed_rows(base, on_menu, band)
    check("a desktop menu over the tree takes the pointer: no row lights under it",
          not seen["rows"], f"rows {seen['rows'][:4]} changed beside the menu")
    dbg.send("gui key 0x1b")
    dbg.settle(0.5)


def finish():
    return _res.finish("devmgr_test")


if __name__ == "__main__":
    sys.exit(main())
