#!/usr/bin/env python3
"""filemanager_test.py's own harness, checked on the host with no guest.

Three things that once made that tool abort or lie, each driven with a
scripted debug console and a QMP stand-in that records every click:

  - a MISSING toolbar item records a failed check and clicks nothing,
    instead of raising KeyError out of the suite;
  - a report SPLIT across two serial sweeps is parsed whole, because
    partial reads accumulate within one wait;
  - a wait whose predicate never holds returns None -- never the layout
    it rejected -- and the last frame seen stays available as evidence;
    and a state the app already reported answers a later wait after the
    grace period, since the app repeats nothing it already said.

Deterministic and a few seconds long. Named by ondemand_sweep.py.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import filemanager_test as fm  # noqa: E402

WIN = {"content": {"x": 100, "y": 50, "w": 600, "h": 400}}

FRAME_HEAD = [
    "files: layout pane 0 0 40 300 340",
    "files: layout dir 0 /fmtest",
]
FRAME_TAIL = [
    "files: layout pane 1 300 40 300 340",
    "files: layout active 0",
    "files: layout view 1 1 single 0 tree 0 0",
]
def tbitems(n):
    return [f"files: layout tbitem {i} {i * 24} 4 22 22" for i in range(n)]


class FakeDbg:
    """Answers `logs()` from a script of batches, one batch per sweep."""
    def __init__(self, batches):
        self.batches = list(batches)
        self.keys = []
    def logs(self, match="", clear=True):
        return self.batches.pop(0) if self.batches else []
    def warp_cursor(self, qmp, x, y):
        qmp.warps.append((x, y))
    def key(self, code):
        self.keys.append(code)


class FakeQmp:
    def __init__(self):
        self.clicks, self.warps = [], []
    def click(self, button="left"):
        self.clicks.append(button)


passed = failed = 0
def check(name, ok, detail=""):
    global passed, failed
    print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    if not ok:
        failed += 1
        if detail:
            print(f"        {detail}")
    else:
        passed += 1


def reset():
    fm._LAST_LAYOUT = None
    fm._ALL_BUF[:] = []


# --- 1. a missing item: a failed check, no click, no traceback ---------
print("  (the one FAIL line below is the harness under test recording the missing item -- expected)")
reset()
dbg = FakeDbg([FRAME_HEAD + tbitems(13) + FRAME_TAIL])
qmp = FakeQmp()
res = fm.Result()
lay = fm.layout_now(dbg, WIN, tries=2)
check("the fixture frame parses", lay is not None and 12 in lay.tbitems and 13 not in lay.tbitems)
t0 = time.time()
got = fm.toolbar_click(dbg, qmp, WIN, lay, 13, res, "folder tree on")
check("a missing toolbar item returns None instead of raising", got is None)
check("...and records exactly one FAILED check", len(res.fails) == 1 and not res.passes,
      f"fails={res.fails} passes={res.passes}")
check("...and clicks nothing", qmp.clicks == [] and qmp.warps == [], f"clicks={qmp.clicks}")
check("...within its own bounded wait", time.time() - t0 < 12, f"{time.time() - t0:.1f}s")
ev = fm._toolbar_evidence(13, fm.last_layout())
check("the evidence names the id asked for and the ids reported",
      "requested tbitem 13" in ev and "reported [0, 1," in ev and "12]" in ev, ev)
check("...and the view state", "view=[1, 1, 0, 0, 0]" in ev, ev)

# --- 2. a present item clicks at its reported centre --------------------
qmp = FakeQmp()
res = fm.Result()
got = fm.toolbar_click(dbg, qmp, WIN, lay, 12, res, "second pane off")
check("a reported item is clicked once, at its centre",
      got is lay and qmp.clicks == ["left"] and qmp.warps == [(100 + 12 * 24 + 11, 50 + 4 + 11)],
      f"clicks={qmp.clicks} warps={qmp.warps}")
check("...with no check recorded", not res.fails and not res.passes)

# --- 3. a split report is parsed whole ---------------------------------
reset()
dbg = FakeDbg([FRAME_HEAD + tbitems(14)[:7], tbitems(14)[7:] + FRAME_TAIL])
got = fm.wait_layout(dbg, WIN, lambda l: 13 in l.tbitems, timeout=3.0)
check("a report split across two sweeps yields one complete layout",
      got is not None and 13 in got.tbitems and 0 in got.pane and 1 in got.pane,
      f"got={got and sorted(got.tbitems)}")

# --- 4. a wait that expires with its predicate false -------------------
reset()
dbg = FakeDbg([FRAME_HEAD + tbitems(14) + FRAME_TAIL])
t0 = time.time()
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 1, timeout=1.5)
check("a wait whose predicate never holds returns None", got is None, f"got={got}")
check("...after its timeout", 1.4 <= time.time() - t0 < 4.0, f"{time.time() - t0:.1f}s")
check("...and the frame it saw is still available as evidence",
      fm.last_layout() is not None and fm.last_layout().view == [1, 1, 0, 0, 0])

# --- 5. an already-reported state answers after the grace period --------
dbg = FakeDbg([])                      # the app stays silent: nothing changed
t0 = time.time()
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 0, timeout=6.0, grace=0.6)
check("a state the app already reported answers a later wait",
      got is fm.last_layout(), f"got={got}")
check("...after the grace period, not the whole timeout",
      0.5 <= time.time() - t0 < 3.0, f"{time.time() - t0:.1f}s")
dbg = FakeDbg([])
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 1, timeout=1.2, grace=0.3)
check("...but never one that does not satisfy the predicate", got is None)

# --- 6. a torn line inside a report does not take the parser down -------
reset()
dbg = FakeDbg([FRAME_HEAD + ["files: layout tbitem 13 3", "files: layout tbitem 5 12 4 dbg> 22"] + FRAME_TAIL])
got = fm.layout_now(dbg, WIN, tries=2)
check("a torn report line is skipped, not fatal", got is not None and 13 not in got.tbitems)

print(f"\nfilemanager_harness_hostcheck: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
