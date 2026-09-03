#!/usr/bin/env python3
"""The taskbar clock's calendar popup: opening, the grid, and week_start.

WHAT IS UNDER TEST
------------------
Clicking the tray clock opens a panel above it (userland/wm/calendar_popup.c)
showing the current month with today highlighted, `<`/`>` to page months
and a title that snaps back to today. The week's first column comes from
`desktop.week_start` (kernel/lib/week_start_config.c).

THREE THINGS THIS ASSERTS THAT AN "IT OPENED" CHECK WOULD NOT
-------------------------------------------------------------
1. IT IS DRAWN, not merely flagged open. `gui calendar --json` reporting
   open=true is exactly what a popup that draws nothing also reports --
   this repo's "it responds is not it is drawn" trap, which shipped a
   Calculator with no visible buttons. So every open/close check is
   paired with a PIXEL comparison of the panel's own rect, and closing
   must restore the pixels that were there before it opened.

2. THE GRID IS RIGHT, checked against an INDEPENDENT oracle. The guest
   computes the 1st's column from cal_days_from_civil(); the host checks
   it with datetime.date().weekday(), which shares no code with it. A
   test that re-derived the column the same way would agree with a wrong
   implementation. The year-boundary case (paging back past January) is
   included because "month - 1" without a wrap is the obvious bug.

3. `desktop.week_start` ACTUALLY MOVES THE COLUMNS. Setting it to sunday
   must shift the reported first column by exactly one and repaint the
   header row; a popup that read the setting and ignored it passes every
   other check here. The setting is restored to monday at the end --
   a tool that leaves a setting changed changes the machine for every
   later tool (CLAUDE.md), which is how a faster pointer once made two
   unrelated tools fail.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/calendar_test.py
    python3 tools/vm.py stop
"""

import argparse
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def cal(dbg):
    return dbg.json("gui calendar --json")


def panel_box(g):
    """The popup's own rect, EXCLUDING nothing -- its bottom edge is the
    taskbar's top edge, so the once-a-second clock tick is outside it.
    A comparison box containing the clock could never settle."""
    return (g["x"], g["y"], g["x"] + g["w"], g["y"] + g["h"])


def expected_first_col(year, month, week_start):
    """The 1st's column, computed on the HOST. datetime.weekday() is
    Monday=0, which is the monday-first answer directly."""
    mon0 = datetime.date(year, month, 1).weekday()
    return mon0 if week_start == "monday" else (mon0 + 1) % 7


def cell_probe(g, col, row):
    """A point inside a day cell that is NOT on the digit -- the top-left
    corner of the cell's interior. Sampling the centre would read the
    glyph as often as the background."""
    x = g["grid_x"] + col * g["cell_w"] + 3
    y = g["grid_y"] + (row + 1) * g["cell_h"] + 3
    return x, y


def pixel_at(qmp, png, x, y):
    from PIL import Image
    qmp.screenshot(png)
    return Image.open(png).convert("RGB").getpixel((x, y))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "calendar_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.join(outdir, n)  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("calendar popup (tray clock -> month grid)")

    # --- 0. the report is live before anything is clicked -------------
    # Start from closed whatever the last tool left behind: this asserts
    # against the pixels the panel covers, so a popup already up would
    # make check 1's "the pixels changed" compare an open panel with an
    # open panel and read as a dead feature.
    if cal(dbg)["open"]:
        dbg.send("gui click 640 300")
        dbg.settle(); time.sleep(0.3)

    g = cal(dbg)
    today = g["today"]
    if not check("closed popup reports today's month", not g["open"] and
                 g["view"]["year"] == today["year"] and
                 g["view"]["month"] == today["month"],
                 f'{g["title"]["text"]} vs today {today["year"]}-{today["month"]}'):
        return report()
    check("the clock has a rect to click", g["clock"] is not None)

    box = panel_box(g)
    before = qmp.stable_pixels(shot("cal_before.png"), box=box)

    # --- 1. clicking the clock opens it, AND draws it -----------------
    dbg.send(f"gui click {g['clock']['cx']} {g['clock']['cy']}")
    dbg.settle()
    time.sleep(0.4)
    g = cal(dbg)
    if not check("clicking the clock opened the popup", g["open"]):
        return report()
    after = qmp.stable_pixels(shot("cal_open.png"), box=box)
    check("...and the panel's pixels CHANGED (it is drawn, not just flagged)",
          after != before)
    check("the WM agrees it is an overlay",
          dbg.json("gui state --json")["overlays"]["calendar"] is True)

    # --- 2. today is highlighted, and its neighbour is not ------------
    tcol, trow = g["today"]["col"], g["today"]["row"]
    if check("today's cell is on the grid", tcol >= 0 and trow >= 0,
             f"col={tcol} row={trow}"):
        tx, ty = cell_probe(g, tcol, trow)
        # The CONTROL point: the cell one column over (wrapping left at
        # the last column). Half the assertion is the neighbour staying
        # the panel's own background -- without it, a bug that filled the
        # whole grid with the accent would pass.
        ncol = tcol - 1 if tcol > 0 else tcol + 1
        nx, ny = cell_probe(g, ncol, trow)
        px = pixel_at(qmp, shot("cal_today.png"), tx, ty)
        pn = pixel_at(qmp, shot("cal_today.png"), nx, ny)
        # The panel background, sampled from its own padding.
        pbg = pixel_at(qmp, shot("cal_today.png"), g["x"] + 3, g["grid_y"] + 3)
        check("today's cell is filled with something else", px != pn, f"{px} vs {pn}")
        check("...and the neighbouring cell is the panel background",
              pn == pbg, f"{pn} vs {pbg}")

    # --- 3. the grid agrees with an independent calendar --------------
    want = expected_first_col(g["view"]["year"], g["view"]["month"], g["week_start"])
    check("the 1st lands in the column datetime says it does",
          g["view"]["first_col"] == want,
          f'reported {g["view"]["first_col"]}, host says {want}')
    check("the month is as long as datetime says",
          g["view"]["days"] ==
          (datetime.date(g["view"]["year"] + (g["view"]["month"] == 12),
                         g["view"]["month"] % 12 + 1, 1) -
           datetime.date(g["view"]["year"], g["view"]["month"], 1)).days,
          f'{g["view"]["days"]} days')

    # --- 4. paging, including across the year boundary ----------------
    start_year, start_month = g["view"]["year"], g["view"]["month"]
    dbg.send(f"gui click {g['next']['cx']} {g['next']['cy']}")
    dbg.settle(); time.sleep(0.3)
    g2 = cal(dbg)
    nxt = (start_month % 12) + 1
    nxt_year = start_year + (1 if start_month == 12 else 0)
    check("`>` pages forward one month",
          (g2["view"]["year"], g2["view"]["month"]) == (nxt_year, nxt),
          g2["title"]["text"])
    check("...and it stayed open", g2["open"])

    # Back past January: start_month clicks on `<` from next month lands
    # in the previous year whenever the year has not much left in it.
    for _ in range(start_month + 1):
        dbg.send(f"gui click {g2['prev']['cx']} {g2['prev']['cy']}")
        dbg.settle()
    time.sleep(0.3)
    g3 = cal(dbg)
    check("`<` wraps back across the year boundary",
          (g3["view"]["year"], g3["view"]["month"]) == (start_year - 1, 12),
          g3["title"]["text"])
    check("...and December still starts where datetime says",
          g3["view"]["first_col"] == expected_first_col(start_year - 1, 12, g3["week_start"]),
          f'first_col={g3["view"]["first_col"]}')

    # --- 5. the title snaps back to today -----------------------------
    dbg.send(f"gui click {g3['title']['cx']} {g3['title']['cy']}")
    dbg.settle(); time.sleep(0.3)
    g4 = cal(dbg)
    check("clicking the title returns to today's month",
          (g4["view"]["year"], g4["view"]["month"]) == (start_year, start_month),
          g4["title"]["text"])

    # --- 6. week_start moves the columns ------------------------------
    mon_first = g4["view"]["first_col"]
    header_box = (g4["grid_x"], g4["grid_y"],
                  g4["grid_x"] + 7 * g4["cell_w"], g4["grid_y"] + g4["cell_h"])
    header_mon = qmp.stable_pixels(shot("cal_hdr_mon.png"), box=header_box)
    dbg.send("sh config set desktop.week_start sunday")
    dbg.settle(); time.sleep(1.2)
    g5 = cal(dbg)
    check("desktop.week_start=sunday is adopted", g5["week_start"] == "sunday",
          g5["week_start"])
    check("...and the 1st moves one column right",
          g5["view"]["first_col"] == (mon_first + 1) % 7,
          f'{mon_first} -> {g5["view"]["first_col"]}')
    header_sun = qmp.stable_pixels(shot("cal_hdr_sun.png"), box=header_box)
    check("...and the weekday header was actually repainted",
          header_sun != header_mon)
    dbg.send("sh config set desktop.week_start monday")   # leave the machine as found
    dbg.settle(); time.sleep(1.0)
    check("restored to monday", cal(dbg)["week_start"] == "monday")

    # --- 7. a second click on the clock CLOSES it ---------------------
    g6 = cal(dbg)
    dbg.send(f"gui click {g6['clock']['cx']} {g6['clock']['cy']}")
    dbg.settle(); time.sleep(0.4)
    check("a second click on the clock closes it", not cal(dbg)["open"])
    closed = qmp.stable_pixels(shot("cal_closed.png"), box=box)
    check("...and the pixels it covered are back", closed == before)

    # --- 8. it is mutually exclusive with the Start menu --------------
    dbg.send(f"gui click {g6['clock']['cx']} {g6['clock']['cy']}")
    dbg.settle(); time.sleep(0.3)
    if check("reopened for the Start-menu check", cal(dbg)["open"]):
        tb = dbg.json("gui taskbar --json")["start"]
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")
        dbg.settle(); time.sleep(0.3)
        st = dbg.json("gui state --json")["overlays"]
        check("opening the Start menu closes the calendar",
              st["start_menu"] is True and st["calendar"] is False, str(st))
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")   # close the Start menu again
        dbg.settle()

    # --- 9. a click on the desktop dismisses it -----------------------
    dbg.send(f"gui click {g6['clock']['cx']} {g6['clock']['cy']}")
    dbg.settle(); time.sleep(0.3)
    if cal(dbg)["open"]:
        dbg.send("gui click 640 300")
        dbg.settle(); time.sleep(0.3)
        check("a click on the desktop dismisses it", not cal(dbg)["open"])

    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\ncalendar_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
