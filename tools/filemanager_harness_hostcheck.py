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
    grace period, since the app repeats nothing it already said;
  - but that cached answer is OFF once a newer report has begun, so a
    frame whose tail has not arrived cannot let the previous one answer
    for a state the app has already left -- and it STAYS off across the
    waits that follow, until a block parses complete or the window is
    replaced.

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
    fm.reset_layout()
    fm._ALL_BUF[:] = []


# --- 1. a missing item: a failed check, no click, no traceback ---------
print("  (the one FAIL line below is the harness under test recording the missing item -- expected)")
reset()
# The command bar's items 0..10: "dpane" (item 11) is the one missing.
dbg = FakeDbg([FRAME_HEAD + tbitems(11) + FRAME_TAIL])
qmp = FakeQmp()
res = fm.Result()
lay = fm.layout_now(dbg, WIN, tries=2)
check("the fixture frame parses", lay is not None and 10 in lay.tbitems and 11 not in lay.tbitems)
t0 = time.time()
got = fm.toolbar_click(dbg, qmp, WIN, lay, "dpane", res, "details pane")
check("a missing toolbar item returns None instead of raising", got is None)
check("...and records exactly one FAILED check", len(res.fails) == 1 and not res.passes,
      f"fails={res.fails} passes={res.passes}")
check("...and clicks nothing", qmp.clicks == [] and qmp.warps == [], f"clicks={qmp.clicks}")
check("...within its own bounded wait", time.time() - t0 < 12, f"{time.time() - t0:.1f}s")
ev = fm._toolbar_evidence("dpane", fm.last_layout())
check("the evidence names the button asked for and the items reported",
      "requested button dpane" in ev and "'tb': [0, 1," in ev and "10]" in ev, ev)
check("...and the view state", "view=[1, 1, 0, 0, 0]" in ev, ev)

# --- 2. a present item clicks at its reported centre --------------------
qmp = FakeQmp()
res = fm.Result()
got = fm.toolbar_click(dbg, qmp, WIN, lay, "more", res, "see more")   # item 10
check("a reported item is clicked once, at its centre",
      got is lay and qmp.clicks == ["left"] and qmp.warps == [(100 + 10 * 24 + 11, 50 + 4 + 11)],
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

# --- 7. a newer report in flight retires the cached frame ---------------
#
# The frame the app is CURRENTLY sending is the one a wait cares about,
# and until its tail arrives the newest COMPLETE frame is the one before
# it. Answering from that after the grace period is how a wait for one
# pane succeeded against the two-pane layout it replaced.
reset()
OLD = FRAME_HEAD + tbitems(14) + FRAME_TAIL          # two panes, tree off
NEW_HEAD = ["files: layout pane 0 0 40 600 340", "files: layout dir 0 /fmtest"]
NEW_TAIL = ["files: layout pane 1 0 0 0 0", "files: layout active 0",
            "files: layout view 1 1 single 1 tree 0 0"]

dbg = FakeDbg([OLD])
base = fm.layout_now(dbg, WIN, tries=2)
check("the two-pane frame is the cached one", base is not None and base.view[2] == 0)

dbg = FakeDbg([NEW_HEAD])              # a report begins, and its tail never lands
t0 = time.time()
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 0, timeout=1.5, grace=0.3)
check("a partial newer report stops the cached frame answering", got is None,
      f"got view={got and got.view}")
check("...and the wait runs to its deadline rather than the grace period",
      1.4 <= time.time() - t0 < 4.0, f"{time.time() - t0:.1f}s")

# The discriminating half: a predicate true of BOTH frames. Answering it
# from the cache is indistinguishable from answering it correctly unless
# the layout that comes back is the one still arriving.
reset()
dbg = FakeDbg([OLD])
fm.layout_now(dbg, WIN, tries=2)
dbg = FakeDbg([NEW_HEAD, [], [], NEW_TAIL])
got = fm.wait_layout(dbg, WIN, lambda l: bool(l.view), timeout=6.0, grace=0.3)
check("a wait satisfied by either frame answers with the one arriving",
      got is not None and got.view[2] == 1, f"got view={got and got.view}")

# --- 8. the invalidation OUTLIVES the wait that saw the partial --------
#
# A wait timing out changes nothing about the app, so the frame it
# refused to answer with is no more current afterwards than it was
# during. Held per wait, the rule lapsed at the next call: the app went
# quiet, the grace period expired, and the retired frame answered.
reset()
dbg = FakeDbg([OLD])
fm.layout_now(dbg, WIN, tries=2)
check("a complete frame is cached and current",
      fm.last_layout() is not None and fm.layout_is_current())

dbg = FakeDbg([NEW_HEAD])
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 0, timeout=1.2, grace=0.3)
check("the wait that sees the partial report times out", got is None)
check("...leaving the cached frame NOT current", not fm.layout_is_current())
check("...but still available as evidence",
      fm.last_layout() is not None and fm.last_layout().view[2] == 0)

dbg = FakeDbg([])                      # the app says nothing for this whole wait
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 0, timeout=1.2, grace=0.3)
check("a LATER silent wait does not answer from the retired frame", got is None,
      f"got view={got and got.view}")

dbg = FakeDbg([NEW_TAIL])              # the tail lands at last
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 1, timeout=3.0, grace=0.3)
check("the tail completes the block the earlier wait began",
      got is not None and got.view[2] == 1, f"got view={got and got.view}")
check("...and that frame is current", fm.layout_is_current())
dbg = FakeDbg([])
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 1, timeout=3.0, grace=0.3)
check("...so silence answers from it again, the rule having re-armed",
      got is fm.last_layout(), f"got={got}")

# --- 9. layout_now consumes a partial report on the same terms ---------
reset()
dbg = FakeDbg([OLD])
fm.layout_now(dbg, WIN, tries=2)
dbg = FakeDbg([NEW_HEAD])
check("layout_now answers None while the report is partial",
      fm.layout_now(dbg, WIN, tries=2) is None)
check("...and retires the cached frame for the waits after it",
      not fm.layout_is_current())
dbg = FakeDbg([])
check("...so a silent wait for the old state times out",
      fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 0, timeout=1.2, grace=0.3) is None)
dbg = FakeDbg([NEW_TAIL])
got = fm.wait_layout(dbg, WIN, lambda l: l.view[2] == 1, timeout=3.0, grace=0.3)
check("...and the lines it consumed still complete the block later",
      got is not None and got.view[2] == 1, f"got view={got and got.view}")

# --- 10. a respawned window inherits nothing from the old one ----------
fm.reset_layout()
check("reset_layout forgets the frame and its currency",
      fm.last_layout() is None and not fm.layout_is_current())
dbg = FakeDbg([])
check("...so nothing answers a wait until the new window reports",
      fm.wait_layout(dbg, WIN, lambda l: True, timeout=0.9, grace=0.3) is None)

print(f"\nfilemanager_harness_hostcheck: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
