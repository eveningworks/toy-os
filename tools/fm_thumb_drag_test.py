#!/usr/bin/env python3
"""The File Manager's icon view: dragging the scrollbar's thumb after
clicking a file SCROLLS, and does not drag that file.

The icons view answered a press on its scrollbar before forgetting the
LAST press's row, so the drag the router then started carried whatever
had been clicked before -- in /bin, click the `wm` folder, grab the thumb,
and the folder went with the pointer. With nothing clicked first the row
was -1 and the thumb worked, which is why it looked intermittent.

1. **A click selects the first icon after `..`** (the precondition: a row pressed).
2. **A thumb drag then scrolls the grid and starts no file drag.**
"""
import argparse
import sys
import time

import filemanager_test as fm
from gui_debug import enter_gui, DebugConsole          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

START = "/bin"   # far more programs than one screen of icons


def run(dbg, qmp, res):
    dbg.write_lines(fm.FILES_CONF, ["panes=1", "tree=0", "left_view=icons", "details_pane=0"])
    win = dbg.spawn(f"{fm.SPAWN_PATH} {START}", fm.TITLE)
    res.check("the File Manager opens", win is not None, "no window")
    if not win:
        return
    lay = (fm.wait_layout(dbg, win, lambda l: l.dir.get(0) == START and 0 in l.cellgrid)
           or fm.layout_now(dbg, win))
    if not lay or 0 not in lay.cellgrid:
        res.check("the icons view reports its grid", False, f"layout={lay and lay.cellgrid}")
        return

    # 1. click the SECOND cell: the first is "..", which no drag carries
    x0, y0, cw, ch, cols = lay.cellgrid[0]
    cx, cy = (x0 + cw, y0) if cols > 1 else (x0, y0 + ch)
    fm.sure_click(dbg, qmp, lay.ox + cx + cw // 2, lay.oy + cy + ch // 2)
    lay = (fm.wait_layout(dbg, win, lambda l: l.selected not in (None, "-"))
           or fm.layout_now(dbg, win) or fm.last_layout())
    picked = lay.selected if lay else None
    before = lay.scroll.get(0) if lay else None
    res.check("a click selects the first icon after ..", picked not in (None, "-"), f"selected={picked!r}")

    # 2. the thumb, at the top of the bar while the grid is at its top
    px, py, pw, ph = lay.pane[0]
    tx, ty = lay.ox + px + pw - 3, lay.oy + py + 6
    dbg.warp_cursor(qmp, tx, ty)
    qmp.mouse_down()
    time.sleep(0.2)
    mid = None
    for i in range(1, 7):
        dbg.warp_cursor(qmp, tx, ty + (ph // 2) * i // 6)
        if i == 6:
            time.sleep(0.3)
            mid = fm.layout_now(dbg, win)
    qmp.mouse_up()
    time.sleep(0.3)
    after = fm.wait_layout(dbg, win, lambda l: l.scroll.get(0) != before, timeout=8.0) \
        or fm.layout_now(dbg, win) or fm.last_layout()
    in_flight = bool(mid and mid.drag and mid.drag[0])
    res.check("a thumb drag after a click scrolls and carries no file",
              not in_flight and after is not None and after.scroll.get(0) != before
              and after.dir.get(0) == START,
              f"drag={mid and mid.drag} scroll {before} -> {after and after.scroll.get(0)} "
              f"dir={after and after.dir.get(0)!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "fm_thumb_drag_test")
    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, res)
    finally:
        dbg.close()
    print(f"\nfm_thumb_drag_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
