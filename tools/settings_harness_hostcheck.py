#!/usr/bin/env python3
"""settings_test.py's own wait and geometry logic, on the host, no guest.

Four things that once made that tool measure the wrong screen, each
driven with a scripted debug console:

  - a sidebar row is aimed at where it IS, not where it would be
    unscrolled -- the row dump is taken once, at the top, and the last
    rows are reported below the window, where a click hits the taskbar
    and MINIMIZES the app;
  - the scroll position is read from the newest report and waited for,
    since it is logged only on a CHANGE and a read taken too early hands
    back the value from before the input;
  - a page is confirmed open by its own report before anything reads its
    controls, because an empty control list means either a page with no
    controls or a page that never opened;
  - and a control list is scoped to the page that was just opened, so
    the previous page's controls cannot answer for it.

Deterministic and a few seconds long. Named by ondemand_sweep.py.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import settings_test as st  # noqa: E402

# The real sidebar, as the app reports it: 31 rows of pitch 20 from y=26,
# 18 of them visible in a 361px tree.
ROWS = [{"row": i, "y": 26 + 20 * i, "depth": 0 if i in (0, 3, 5) else 1,
         "label": f"row{i}"} for i in range(31)]
VISIBLE, TREE_BOTTOM = 18, 377


class FakeDbg:
    """Answers logs() from a script of batches, one batch per sweep."""
    def __init__(self, batches):
        self.batches = list(batches)

    def logs(self, match="", clear=True):
        return self.batches.pop(0) if self.batches else []


passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if ok:
        passed += 1
    else:
        failed += 1
        if detail:
            print(f"        {detail}")


def reset():
    st._log[:] = []


# --- 1. a row is aimed at where it IS ----------------------------------
last = ROWS[-1]
check("the last row's reported y is outside the sidebar",
      st.row_y(ROWS, last["row"], 0) > TREE_BOTTOM,
      f"y={st.row_y(ROWS, last['row'], 0)} vs tree bottom {TREE_BOTTOM}")
max_top = len(ROWS) - VISIBLE
y_scrolled = st.row_y(ROWS, last["row"], max_top)
check("...and inside it once the list is scrolled to the end",
      ROWS[0]["y"] <= y_scrolled <= TREE_BOTTOM, f"y={y_scrolled}")
check("a row above the fold is reported ABOVE the sidebar, not inside it",
      st.row_y(ROWS, 2, max_top) < ROWS[0]["y"],
      f"y={st.row_y(ROWS, 2, max_top)}")
check("row 0 at top 0 is where the app said it was",
      st.row_y(ROWS, 0, 0) == ROWS[0]["y"])
check("the pitch comes from the app's own rows, not a constant",
      st.row_y([{"row": 0, "y": 10}, {"row": 1, "y": 40}], 2, 0) == 70)

# --- 2. the scroll position, read and waited for -----------------------
reset()
check("no report yet means no position", st.sidebar_state(FakeDbg([])) is None)
dbg = FakeDbg([["settings: sidebar top 0 visible 18 rows 31"]])
check("the report parses", st.sidebar_state(dbg) == {"top": 0, "visible": 18, "rows": 31},
      f"{st.sidebar_state(dbg)}")
dbg = FakeDbg([["settings: sidebar top 3 visible 18 rows 31"]])
got = st.wait_sidebar(dbg, lambda s: s["top"] == 3, timeout=2.0)
check("a wait is satisfied by the newer report", got is not None and got["top"] == 3,
      f"{got}")
dbg = FakeDbg([])
t0 = time.time()
got = st.wait_sidebar(dbg, lambda s: s["top"] == 9, timeout=1.2)
check("a position the app never reports times out rather than answering",
      got is None, f"{got}")
check("...after its deadline", 1.1 <= time.time() - t0 < 3.0,
      f"{time.time() - t0:.1f}s")
check("...and the newest report is still readable as evidence",
      st.sidebar_state(dbg)["top"] == 3)

# --- 3. a page is confirmed before its controls are read ---------------
reset()
OPEN_MOUSE = ["settings: page Input/Mouse slots 6 advanced 0 captions 6 disabled 0",
              "settings: control 4 system.mouse_speed 187 492 66 24 rows 0 kind spin"]
OPEN_NTP = ["settings: page Time & Locale/Network Time slots 3 advanced 0 captions 2 disabled 0",
            "settings: control 0 system.ntp 187 84 100 24 rows 2 kind radio",
            "settings: control 1 system.ntp_server 187 150 445 24 rows 0 kind text",
            "settings: control 2 system.ntp_interval 187 200 66 24 rows 0 kind spin"]
dbg = FakeDbg([OPEN_MOUSE])
st.drain(dbg)
mark = len(st._log)
dbg = FakeDbg([[]])                    # the click missed: the app says nothing
t0 = time.time()
check("a page that never opens is not confirmed",
      st.wait_page(dbg, mark, "Network Time", timeout=1.2) is None)
check("...even though another page IS open",
      st.page_line(dbg, 0)["page"] == "Input/Mouse")
check("...and the previous page's controls do not answer for it",
      st.controls(dbg, mark) == {}, f"{st.controls(dbg, mark)}")
dbg = FakeDbg([OPEN_NTP])
got = st.wait_page(dbg, mark, "Network Time", timeout=2.0)
check("the page that did open is confirmed by name",
      got is not None and got["page"] == "Time & Locale/Network Time", f"{got}")
ctls = st.controls(dbg, mark)
check("...and its controls are the ones read",
      sorted(ctls) == ["system.ntp", "system.ntp_interval", "system.ntp_server"],
      f"{sorted(ctls)}")
check("...with the rect the app reported", ctls["system.ntp_server"]["w"] == 445)

# --- 4. a control straddling the viewport is not 'in view' -------------
#
# The rule reveal() applies: the whole rect, not its top edge. The
# spinbox at y=366 has its down stepper below a 377px viewport, and a
# press there is clipped away -- it stepped up and never down.
PAGE_TOP, PAGE_H = 16, 361
for y, want in ((492, False), (366, False), (324, True), (8, False)):
    ctl = {"x": 187, "y": y, "w": 66, "h": 24}
    check(f"a 24px control at y={y} is {'inside' if want else 'NOT inside'} the viewport",
          st.fully_inside(ctl, PAGE_TOP, PAGE_H) == want,
          f"viewport {PAGE_TOP}..{PAGE_TOP + PAGE_H}")

print(f"\nsettings_harness_hostcheck: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
