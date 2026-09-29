#!/usr/bin/env python3
"""tools/taskbar_test.py -- open enough windows to overflow the taskbar,
and assert it never runs off the screen.

WHAT THIS IS
------------
The reported bug was a screenshot of thirteen windows with the last
taskbar button clipped by the right edge and running under the clock
(docs/bugs.md, fixed). The strip's layout is one function now --
`taskbar_layout()` in userland/wm/wm_taskbar.c -- and this drives it past
every threshold it has: natural width, shrink-to-fit, the floor, and
grouping by application.

    python3 tools/vm.py --disk <copy> start
    python3 tools/taskbar_test.py
    echo $?                            # 0 = every check passed

WHY IT ASSERTS ON JSON AND NOT ON PIXELS
----------------------------------------
`gui taskbar --json` comes from the SAME taskbar_layout() that draws the
buttons and that hit-tests them -- that is the whole point of the file it
lives in. Before this change the debug console was a fourth, independent
walk of the window list, and it was already wrong: it placed button 0 at
`sw + 4` where the real one sat at `4 + sw + 8`, so every centre it
reported, and every test click aimed at one, was eight pixels left of
the button. A report a test trusts has to come from the code under test.

The one thing that report cannot answer is whether a button is DRAWN
where it says, which is why `--pixels` exists: it warps the real cursor
onto the last button and checks the hover tint moves. Off by default
because it needs QMP and the layout claim is the one under test.

POSITIVE CONTROL
----------------
Run twice when this was written, and the FIRST run is the lesson.

Replacing `taskbar_layout()`'s shrink-and-group block with a flat
`int w = natural;` reddened three checks -- shrink, grouping, per-app
buttons -- and left "no button crosses into the tray" GREEN. Not because
the check is weak, but because the emit loop's `x + w > x1` guard was
still there, so the extra buttons became HIDDEN instead of being drawn
off the edge. The control never reached the code the check is about.
That is this repo's recurring shape: when a control fires less than
expected, ask what other path satisfied the assertion.

So the control has to remove BOTH -- the width policy and the guard --
and then the overflow check goes red as intended. The first run was
still worth having: it showed there was no unconditional assertion on
`hidden`, which is the second way to get this wrong (stop the overflow
by silently forgetting windows). "no window is left off the strip"
exists because of that run.

CAVEAT
------
Spawning thirty-odd Notepads is the point, and it is slow -- each one is
a real process with a real window buffer. Budget a couple of minutes.
Not in gui_regress.py for that reason; run it when the taskbar or the
window list changes.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
NOTEPAD = "/bin/wm/apps/notepad"
CALCULATOR = "/bin/wm/apps/calculator"

# Enough to pass the floor and force grouping at the default font on a
# 1280-wide screen, where the strip holds ~25 floor-width buttons. Not a
# calibrated constant: every check below reads the live numbers and the
# loop stops as soon as grouping engages, so a wider screen or a smaller
# font just means more iterations.
MAX_SPAWN = 34


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print(f"        {detail}")


def strip(dbg):
    """The taskbar as the WM reports it, plus the derived numbers."""
    tb = dbg.json("gui taskbar --json")
    btns = tb["buttons"]
    tb["right"] = max((b["x"] + b["w"]) for b in btns) if btns else 0
    tb["represented"] = sum(b["count"] for b in btns)
    return tb


def screen_w(dbg):
    """The live screen width -- `gui state` knows it.

    NOT a hardcoded 1280. Two tools in this suite assume that resolution
    and are the reason the default cannot be changed (docs/roadmap.md);
    there is no need for a third when the WM will just say.
    """
    return dbg.state()["screen"]["w"]


def run(dbg, qmp, res, pixels):
    # A clean start matters: this counts windows, so anything left open
    # by an earlier tool would shift every threshold. Close whatever is
    # up rather than assuming a fresh desktop.
    while dbg.state()["windows"] > 0:
        dbg.send("gui close %d" % (dbg.state()["windows"] - 1))
        dbg.settle()

    base = strip(dbg)
    res.check("a desktop with no windows has no window buttons",
              len(base["buttons"]) == 0 and base["hidden"] == 0,
              f"got {len(base['buttons'])} buttons, hidden={base['hidden']}")

    grouped_at = None
    widths = []
    n = 0
    while n < MAX_SPAWN:
        dbg.send("gui spawn " + NOTEPAD)
        dbg.settle()
        n += 1
        tb = strip(dbg)
        widths.append(tb["buttons"][0]["w"] if tb["buttons"] else 0)

        # THE REPORTED BUG, and the assertion is against the TRAY's
        # left edge, not the screen's: the reported screenshot had
        # buttons running under the clock, which is well inside the
        # screen. `tray_x` is reported by the same tray_left() the
        # layout divides the strip by. Checked at EVERY count rather
        # than only at the end, because the failure is a threshold and
        # the end is past it.
        if tb["right"] > tb["tray_x"]:
            res.check(f"no button crosses into the tray ({n} windows)", False,
                      f"rightmost button ends at {tb['right']}, "
                      f"tray starts at {tb['tray_x']}")
            break

        # The other way to "fix" an overflow is to forget windows. Every
        # window is either on a button of its own or counted into a
        # group; `hidden` is what is left, and it must be zero until the
        # strip is genuinely full.
        if tb["represented"] + tb["hidden"] != n:
            res.check(f"every window is accounted for ({n} windows)", False,
                      f"{tb['represented']} on buttons + {tb['hidden']} hidden != {n}")
            break

        if any(b["count"] > 1 for b in tb["buttons"]):
            grouped_at = n
            break

    final = strip(dbg)
    res.check("no button crosses into the tray, at any window count",
              final["right"] <= final["tray_x"] and final["right"] < screen_w(dbg),
              f"rightmost button ends at {final['right']}; tray starts at "
              f"{final['tray_x']}, screen is {screen_w(dbg)} wide")

    # UNCONDITIONAL, and it belongs outside the grouped branch: the
    # second way to get this wrong is to stop the overflow by quietly
    # dropping the windows that do not fit, which leaves them with no
    # handle at all. The positive control below reaches exactly that
    # state, and this is the check that catches it.
    res.check("no window is left off the strip",
              final["hidden"] == 0,
              f"{final['hidden']} of {n} windows have no button")

    # Numbered, not identical: a strip of equal-width buttons is
    # pixel-identical whether or not shrinking works, so the claim is
    # that the width MOVED and moved DOWN.
    shrank = len(set(widths)) > 1 and min(widths) < max(widths)
    res.check("buttons shrink as windows are added",
              shrank, f"widths seen: {sorted(set(widths))}")

    res.check("windows of one app collapse into one button",
              grouped_at is not None and
              len([b for b in final["buttons"] if b["count"] > 1]) == 1,
              f"grouped at {grouped_at} windows; counts "
              f"{[b['count'] for b in final['buttons']]}")

    if grouped_at is not None:
        group = [b for b in final["buttons"] if b["count"] > 1][0]
        res.check("the group button counts every window it stands for",
                  group["count"] == grouped_at,
                  f"button says {group['count']}, {grouped_at} were opened")
        # Named after the APP, not after whichever window opened first --
        # "notepad (28)", not "unt (28)". The count alone would pass with
        # the window title, which is why the app id is checked too.
        res.check("the group button is named after the application",
                  "notepad" in group["label"] and "(" in group["label"],
                  f"label is {group['label']!r}")
        res.check("the group accounts for every window opened",
                  final["represented"] == grouped_at,
                  f"buttons cover {final['represented']} of {grouped_at}")

        # Clicking a collapsed button lists its windows rather than
        # picking one arbitrarily -- Windows' jump list.
        dbg.click(group["x"] + group["w"] // 2, group["cy"])
        dbg.settle()
        menu = dbg.ctxmenu()
        rows = menu.get("rows", []) if menu else []
        res.check("clicking a grouped button lists its windows",
                  bool(menu and menu.get("open") and len(rows) > 1),
                  f"context menu reports {menu}")
        if rows:
            # The row's own reported centre, never a derived index --
            # docs/gui-guidelines.md. Raising a window changes which pid
            # is frontmost, which is the fact to assert on: `z` is the
            # position in the list and every row is a window, so the pid
            # is what distinguishes "it raised the one I picked" from
            # "it raised something".
            # `gui state`, not `gui windows`: with this many windows open
            # the full list exceeds WIN_DEBUG_REPLY_MAX and comes back
            # truncated (docs/bugs.md). `front_pid` is the one fact
            # needed and it is one number.
            before = dbg.state()["front_pid"]
            dbg.click(menu["x"] + menu["w"] // 2, rows[0]["cy"])
            dbg.settle()
            after = dbg.state()["front_pid"]
            res.check("choosing a window from the group raises it",
                      after != before,
                      f"frontmost pid stayed {before} after picking row 0")

    # A second application must not join the first's group -- the check
    # that grouping is by app and not merely by "everything".
    dbg.send("gui spawn " + CALCULATOR)
    dbg.settle()
    mixed = strip(dbg)
    ids = [b["app_id"] for b in mixed["buttons"]]
    res.check("a different application gets its own button",
              "calculator" in ids and "notepad" in ids,
              f"app ids on the strip: {ids}")

    if pixels:
        check_pixels(dbg, qmp, mixed, res)


def check_pixels(dbg, qmp, tb, res):
    """The one claim the JSON cannot make: the button is DRAWN there.

    Hovers the last button and requires its own pixels to change while a
    control point elsewhere on the strip does not -- half the assertion
    is the neighbour staying put (CLAUDE.md).
    """
    from PIL import Image
    import tempfile

    if len(tb["buttons"]) < 2:
        res.check("hovering the last button tints it", False,
                  "need at least two buttons for a control point")
        return

    last, first = tb["buttons"][-1], tb["buttons"][0]
    tmp = tempfile.mkdtemp(prefix="taskbar_")

    def shot(name):
        p = os.path.join(tmp, name)
        qmp.screenshot(p)
        return Image.open(p).convert("RGB")

    dbg.warp_cursor(qmp, 4, 4)
    time.sleep(0.4)
    a = shot("tb_off.png")
    dbg.warp_cursor(qmp, last["cx"], last["cy"])
    time.sleep(0.4)
    b = shot("tb_on.png")

    moved = a.getpixel((last["cx"], last["cy"])) != b.getpixel((last["cx"], last["cy"]))
    still = a.getpixel((first["cx"], first["cy"])) == b.getpixel((first["cx"], first["cy"]))
    res.check("hovering the last button tints it, and only it",
              moved and still,
              f"hovered pixel moved={moved}, control point unchanged={still}")


def check_height(dbg, qmp, res):
    """`desktop.taskbar_height` moves the strip, live, and `unset` puts
    the font-derived default back. Checked against the WM's own report
    AND a pixel: the strip's top row is its background colour at the new
    y and the row above is not, which a stale report could not fake."""
    base = strip(dbg)
    sh = dbg.state()["screen"]["h"]
    want = 60 if base["h"] != 60 else 64
    dbg.send(f"sh config set desktop.taskbar_height {want}")
    dbg.settle()
    time.sleep(0.5)
    tb = strip(dbg)
    res.check("desktop.taskbar_height re-lays the strip live",
              tb["h"] == want and tb["y"] == sh - want,
              f"asked {want}, got h={tb['h']} y={tb['y']} (screen h {sh})")
    from PIL import Image
    import tempfile
    tmp = tempfile.mkdtemp(prefix="taskbar_h_")
    p = os.path.join(tmp, "thick.png")
    qmp.screenshot(p)
    img = Image.open(p).convert("RGB")
    x = tb["tray_x"] - 8 if tb["tray_x"] > 8 else img.width // 2
    top = img.getpixel((x, sh - want))
    above = img.getpixel((x, sh - want - 1))
    e = tb.get("edge", 0x1E1E22)
    edge = ((e >> 16) & 255, (e >> 8) & 255, e & 255)
    res.check("the strip's top edge is drawn where it is reported",
              top == edge and above != edge,
              f"row {sh - want} = {top} (edge {edge}), row above = {above}")
    dbg.send("sh config unset desktop.taskbar_height")
    dbg.settle()
    time.sleep(0.5)
    back = strip(dbg)
    res.check("unset returns the strip to its default height",
              back["h"] == base["h"],
              f"default {base['h']}, got {back['h']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--pixels", action="store_true",
                    help="also check the last button is drawn where it says")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        # FIRST: the overflow checks below leave twenty Notepads open,
        # past which nothing more can spawn -- `config` included.
        check_height(dbg, qmp, res)
        run(dbg, qmp, res, args.pixels)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ntaskbar_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
