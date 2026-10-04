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

The combine and Close-all sections were seen red the same way
(`tools/mutate.py`, 2026-10-04): offering Close all at ONE window
reddens exactly "one Notepad: ... no Close all", and never placing the
overflow button reddens exactly "...behind an overflow button". A first
attempt at that control fired for the wrong reason -- `--in-gui` against
a guest whose desktop was not up yet, so every check errored -- which is
why a control's FAILING CHECK is read, not just its exit status.
The join, names and draghidden sections were seen red too: re-asking
already-asked entries logs "asking 3" for the second Close all; skipping
the title elision cuts the card's sentence at "The "; leaving hidden
windows un-ranked after a drop puts them at the FRONT of the row.

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
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
NOTEPAD = "/bin/wm/apps/notepad"
CALCULATOR = "/bin/wm/apps/calculator"

# Enough to pass the floor and force grouping at the default font on a
# 1280-wide screen, where the strip holds ~25 floor-width buttons. Not a
# calibrated constant: every check below reads the live numbers and the
# loop stops as soon as grouping engages, so a wider screen or a smaller
# font just means more iterations.
MAX_SPAWN = 34


Result = Results


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


# --- combining, the overflow button, Close all ----------------------------

# Every setting a section below changes; unset at the end whatever happens,
# since a setting outlives the tool (CLAUDE.md).
TOUCHED = ("desktop.taskbar_combine", "desktop.taskbar_buttons", "desktop.taskbar_height")
SECTIONS = ("height", "combine", "closeall", "join", "names", "draghidden", "overflow")


def close_everything(dbg):
    for _ in range(64):
        n = dbg.state()["windows"]
        if n <= 0:
            return
        dbg.send("gui close %d" % (n - 1))
        dbg.settle()
    # Something refused (an unsaved Notepad from an earlier tool):
    # kill the clients outright.
    for w in dbg.windows():
        if w.get("client_pid"):
            dbg.send(f"sh kill {w['client_pid']}")
    time.sleep(1.0)
    dbg.settle()


def set_and_wait(dbg, name, value, field=None, want=None, timeout=6.0):
    """Set (or, value None, unset) a setting and wait until the strip
    reports it -- the WM re-reads on a change counter, a frame later."""
    dbg.send(f"sh config unset {name}" if value is None else f"sh config set {name} {value}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        tb = strip(dbg)
        if field is None or tb.get(field) == want:
            return tb
        time.sleep(0.2)
    return strip(dbg)


def spawn_notepads(dbg, n):
    for _ in range(n):
        before = dbg.state()["windows"]
        dbg.send("gui spawn " + NOTEPAD)
        deadline = time.time() + 15
        while time.time() < deadline and dbg.state()["windows"] <= before:
            time.sleep(0.2)
        dbg.settle()


def menu_labels(dbg):
    m = dbg.ctxmenu() or {}
    return [r.get("label") for r in m.get("rows", [])] if m.get("open") else []


def dismiss_menu(dbg):
    if (dbg.ctxmenu() or {}).get("open"):
        dbg.key("0x1b")
        dbg.settle()


def check_combine(dbg, res):
    """`desktop.taskbar_combine`: always groups from the second window,
    when-full (the default) does not until the floor, never does not at
    all -- and then the overflow button holds the rest."""
    close_everything(dbg)
    tb = set_and_wait(dbg, "desktop.taskbar_combine", "always", "combine", "always")
    spawn_notepads(dbg, 2)
    tb = strip(dbg)
    counts = [b["count"] for b in tb["buttons"]]
    res.check("combine=always: two Notepads share one button",
              counts == [2] and "notepad" in tb["buttons"][0]["label"],
              f"combine={tb.get('combine')} counts={counts} "
              f"labels={[b['label'] for b in tb['buttons']]}")
    if counts == [2]:
        b = tb["buttons"][0]
        dbg.click(b["cx"], b["cy"])
        dbg.settle()
        labels = menu_labels(dbg)
        res.check("...and its list ends in \"Close all 2 windows\"",
                  bool(labels) and labels[-1] == "Close all 2 windows", f"rows {labels}")
        dismiss_menu(dbg)

    # THE CONTROL: the same two windows, the default mode -- two buttons.
    tb = set_and_wait(dbg, "desktop.taskbar_combine", None, "combine", "full")
    counts = [b["count"] for b in tb["buttons"]]
    res.check("combine=full (the default): the same two get a button each",
              counts == [1, 1], f"combine={tb.get('combine')} counts={counts}")

    # NEVER, PAST THE FLOOR. Icon buttons on the thickest strip are the
    # widest a button gets, so a dozen windows overflow it -- rather than
    # the thirty-odd floor-width labelled buttons a 1280 strip holds.
    set_and_wait(dbg, "desktop.taskbar_buttons", "icons", "buttons_kind", "icons")
    set_and_wait(dbg, "desktop.taskbar_height", "96")
    tb = set_and_wait(dbg, "desktop.taskbar_combine", "never", "combine", "never")
    opened = 2
    while opened < 16 and (tb.get("overflow") is None or tb["overflow"]["count"] < 2):
        spawn_notepads(dbg, 1)
        opened += 1
        tb = strip(dbg)
    ovf = tb.get("overflow")
    res.check("combine=never: no button stands for more than one window",
              all(b["count"] == 1 for b in tb["buttons"]),
              f"counts {[b['count'] for b in tb['buttons']]}")
    res.check("...and the windows that do not fit are behind an overflow button",
              ovf is not None and ovf["count"] == tb["hidden"] ==
              opened - len(tb["buttons"]) and tb["hidden"] >= 2,
              f"{opened} windows, {len(tb['buttons'])} buttons, hidden={tb['hidden']}, "
              f"overflow={ovf}")
    if ovf is None:
        return
    right = ovf["x"] + ovf["w"]
    last = max(b["x"] + b["w"] for b in tb["buttons"]) if tb["buttons"] else 0
    res.check("...placed after the last button and clear of the tray",
              last < ovf["x"] and right <= tb["tray_x"],
              f"last button ends {last}, overflow {ovf['x']}..{right}, tray {tb['tray_x']}")

    # Its list raises a hidden window: the front window afterwards is
    # one with no button of its own.
    dbg.click(ovf["cx"], ovf["cy"])
    dbg.settle()
    m = dbg.ctxmenu() or {}
    rows = m.get("rows", []) if m.get("open") else []
    res.check("clicking the overflow button lists the hidden windows",
              len(rows) == ovf["count"], f"{len(rows)} rows for {ovf['count']} hidden: {m}")
    if rows:
        # Row 0: the LAST row is the newest window, which is in front
        # already, and raising it would change nothing to see.
        before = dbg.state()["front_pid"]
        dbg.click(rows[0]["cx"], rows[0]["cy"])
        dbg.settle()
        st = dbg.state()
        tb = strip(dbg)
        front = st["windows"] - 1
        res.check("choosing one raises that hidden window",
                  st["front_pid"] != before and all(b["index"] != front for b in tb["buttons"]),
                  f"front pid {before} -> {st['front_pid']}, front index {front}, "
                  f"button indices {[b['index'] for b in tb['buttons']]}")


def window_menu_for_button(dbg, b):
    dbg.rclick(b["cx"], b["cy"])
    dbg.settle()
    return menu_labels(dbg)


def wait_log(dbg, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hit = [line for line in dbg.logs("closeall:", clear=False) if needle in line]
        if hit:
            return hit[-1]
        time.sleep(0.25)
    return None


def notepads(dbg):
    return [w for w in dbg.windows() if "untitled" in w.get("title", "")]


def check_close_all(dbg, qmp, res):
    """The window menu's "Close all N windows": offered only when the app
    has more than one window, asks each, and names the ones that stayed."""
    close_everything(dbg)
    spawn_notepads(dbg, 1)
    tb = strip(dbg)
    labels = window_menu_for_button(dbg, tb["buttons"][0]) if tb["buttons"] else []
    res.check("one Notepad: its window menu has Close and no Close all",
              "Close" in labels and not any(lb.startswith("Close all") for lb in labels),
              f"rows {labels}")
    dismiss_menu(dbg)

    spawn_notepads(dbg, 2)
    tb = strip(dbg)
    labels = window_menu_for_button(dbg, tb["buttons"][0]) if tb["buttons"] else []
    res.check("three Notepads: \"Close all 3 windows\" directly under Close",
              "Close" in labels and labels.index("Close") + 1 < len(labels) and
              labels[labels.index("Close") + 1] == "Close all 3 windows", f"rows {labels}")
    dbg.logs("closeall:", clear=True)
    row = dbg.ctxmenu_row("Close all 3 windows")
    if row:
        dbg.click(*row)
        dbg.settle()
    said = wait_log(dbg, "all 3 window(s) closed", 8)
    res.check("Close all closes all three", bool(said) and not notepads(dbg),
              f"log {said!r}, notepads left {len(notepads(dbg))}")

    # ONE REFUSES: the front Notepad has typed text and asks to save.
    spawn_notepads(dbg, 3)
    qmp.send_text("unsaved")
    dbg.settle()
    time.sleep(0.5)
    tb = strip(dbg)
    labels = window_menu_for_button(dbg, tb["buttons"][0]) if tb["buttons"] else []
    dbg.logs("closeall:", clear=True)
    t0 = time.time()
    row = dbg.ctxmenu_row("Close all 3 windows")
    if row:
        dbg.click(*row)
        dbg.settle()
    said = wait_log(dbg, "did not close", 12)
    waited = time.time() - t0
    res.check("with one refusing, the notice comes after the wait, not before",
              bool(said) and "1 of 3" in said and 4.0 <= waited <= 9.0,
              f"log {said!r} after {waited:.1f}s")
    time.sleep(0.3)
    st = dbg.state()
    nt = st.get("notice") or {}
    btns = {b["label"]: b for b in nt.get("buttons", [])}
    res.check("the notice says one Notepad window stayed open, and names it",
              nt.get("title") == "1 Notepad window stayed open" and
              "untitled" in nt.get("sub", "") and "The other 2 closed" in nt.get("sub", ""),
              f"notice {nt}")
    res.check("...offering Show it and Force Quit",
              {"Show it", "Force Quit"} <= set(btns), f"buttons {list(btns)}")
    res.check("the two that could close did",
              len(notepads(dbg)) == 1, f"notepads left {[w['title'] for w in notepads(dbg)]}")
    fq = btns.get("Force Quit")
    if fq:
        dbg.click(fq["x"] + fq["w"] // 2, fq["y"] + fq["h"] // 2)
        deadline = time.time() + 6
        while time.time() < deadline and notepads(dbg):
            time.sleep(0.3)
        res.check("Force Quit ends the one that stayed",
                  not notepads(dbg), f"left {[w['title'] for w in notepads(dbg)]}")


def check_drag_hidden(dbg, qmp, res):
    """A drag-reorder keeps the overflow's windows behind it: the drop
    re-ranks the row, and the hidden windows must rank AFTER it -- left on
    their older ranks they sorted first, and the row the user had just
    arranged went into the overflow list instead."""
    close_everything(dbg)
    set_and_wait(dbg, "desktop.taskbar_buttons", "icons", "buttons_kind", "icons")
    set_and_wait(dbg, "desktop.taskbar_height", "96")
    tb = set_and_wait(dbg, "desktop.taskbar_combine", "never", "combine", "never")
    opened = 0
    while opened < 20 and (tb.get("overflow") is None or tb["overflow"]["count"] < 3):
        spawn_notepads(dbg, 1)
        opened += 1
        tb = strip(dbg)
    before = [b["key"] for b in tb["buttons"]]
    hidden = tb["hidden"]
    if hidden < 3 or len(before) < 2:
        res.check("drag with windows hidden: three behind the overflow button", False,
                  f"{opened} windows, {len(before)} buttons, hidden {hidden}")
        return
    # A real drag, as taskbar_drag_test does it: button 0 past button 1.
    src, past = tb["buttons"][0], tb["buttons"][1]
    dbg.warp_cursor(qmp, src["cx"], src["cy"])
    time.sleep(0.2)
    qmp.mouse_down()
    time.sleep(0.2)
    end = past["cx"] + past["w"] // 4
    for x in range(src["cx"], end + 1, 16):
        dbg.warp_cursor(qmp, x, src["cy"])
        time.sleep(0.03)
    dbg.warp_cursor(qmp, end, src["cy"])
    time.sleep(0.4)
    qmp.mouse_up()
    time.sleep(0.4)
    dbg.settle()
    tb = strip(dbg)
    after = [b["key"] for b in tb["buttons"]]
    res.check("drag with windows hidden: the drop reordered the row",
              after[:2] == [before[1], before[0]], f"keys {before} -> {after}")
    res.check("...and the same windows are on the strip, the hidden ones still hidden",
              sorted(after) == sorted(before) and tb["hidden"] == hidden,
              f"keys {before} -> {after}, hidden {hidden} -> {tb['hidden']}")


def check_close_all_join(dbg, qmp, res):
    """A second Close all while one waits JOINS it and asks only the
    windows it adds: the window already asking to save is not sent a
    second close."""
    close_everything(dbg)
    spawn_notepads(dbg, 2)
    qmp.send_text("unsaved")   # the front Notepad, which will refuse
    dbg.settle()
    time.sleep(0.5)
    for _ in range(2):
        before = dbg.state()["windows"]
        dbg.send("gui spawn " + CALCULATOR)
        deadline = time.time() + 15
        while time.time() < deadline and dbg.state()["windows"] <= before:
            time.sleep(0.2)
        dbg.settle()
    tb = strip(dbg)
    note = next((b for b in tb["buttons"] if b["app_id"] == "notepad"), None)
    calc = next((b for b in tb["buttons"] if b["app_id"] == "calculator"), None)
    if not note or not calc:
        res.check("join: a Notepad and a Calculator button", False,
                  f"app ids {[b['app_id'] for b in tb['buttons']]}")
        return
    dbg.logs("closeall:", clear=True)
    window_menu_for_button(dbg, note)
    row = dbg.ctxmenu_row("Close all 2 windows")
    if row:
        dbg.click(*row)
        dbg.settle()
    wait_log(dbg, "asking", 4)
    time.sleep(0.5)   # the clean Notepad closes; the other asks to save
    window_menu_for_button(dbg, strip_button(dbg, "calculator") or calc)
    row = dbg.ctxmenu_row("Close all 2 windows")
    if row:
        dbg.click(*row)
        dbg.settle()
    time.sleep(0.5)
    asks = [line for line in dbg.logs("closeall:", clear=False) if "asking" in line]
    stopped = [line for line in dbg.logs("closeall:", clear=False) if "did not close" in line]
    res.check("a second Close all, while the first waits, asks only its own two",
              len(asks) == 2 and "asking 2 " in asks[0] and "asking 2 " in asks[1] and not stopped,
              f"asks {asks}, already ended {stopped}")
    said = wait_log(dbg, "did not close", 12)
    res.check("...and the joined batch ends as one: 1 of 4 stayed",
              bool(said) and "1 of 4" in said, f"log {said!r}")
    time.sleep(0.3)
    nt = dbg.state().get("notice") or {}
    res.check("...its notice counts the three that closed",
              "The other 3 closed" in nt.get("sub", ""), f"notice {nt}")
    btns = {b["label"]: b for b in nt.get("buttons", [])}
    fq = btns.get("Force Quit")
    if fq:
        dbg.click(fq["x"] + fq["w"] // 2, fq["y"] + fq["h"] // 2)
        deadline = time.time() + 6
        while time.time() < deadline and notepads(dbg):
            time.sleep(0.3)


LONG_NAMES = ["/tmp/close-all-notice-check-a-rather-long-name-%d.txt" % k for k in range(3)]


def check_stayed_sentence(dbg, qmp, res):
    """Three long-titled windows stay open: the card's sentence elides the
    TITLES, never its end -- "and 1 more did not close. The other 1
    closed." -- where a fixed buffer used to cut it mid-word."""
    close_everything(dbg)
    for path in LONG_NAMES:
        dbg.send(f"sh write {path} hello")
        before = dbg.state()["windows"]
        dbg.send(f"gui spawn {NOTEPAD} {path}")
        deadline = time.time() + 15
        while time.time() < deadline and dbg.state()["windows"] <= before:
            time.sleep(0.2)
        dbg.settle()
        time.sleep(0.5)
        qmp.send_text("x")   # dirty: it will ask to save
        dbg.settle()
    spawn_notepads(dbg, 1)   # the one that closes
    tb = strip(dbg)
    dbg.logs("closeall:", clear=True)
    if tb["buttons"]:
        window_menu_for_button(dbg, tb["buttons"][0])
    row = dbg.ctxmenu_row("Close all 4 windows")
    if row:
        dbg.click(*row)
        dbg.settle()
    said = wait_log(dbg, "did not close", 12)
    time.sleep(0.3)
    nt = dbg.state().get("notice") or {}
    sub = nt.get("sub", "")
    res.check("three long titles stayed: the sentence keeps its count and its end",
              bool(said) and "3 of 4" in said and
              sub.endswith("and 1 more did not close. The other 1 closed.") and ".." in sub,
              f"log {said!r}, notice {nt}")
    btns = {b["label"]: b for b in nt.get("buttons", [])}
    fq = btns.get("Force Quit")
    if fq:
        dbg.click(fq["x"] + fq["w"] // 2, fq["y"] + fq["h"] // 2)
        deadline = time.time() + 6
        while time.time() < deadline and dbg.state()["windows"]:
            time.sleep(0.3)
    for path in LONG_NAMES:
        dbg.send(f"sh rm {path}")


def strip_button(dbg, app_id):
    return next((b for b in strip(dbg)["buttons"] if b["app_id"] == app_id), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--pixels", action="store_true",
                    help="also check the last button is drawn where it says")
    ap.add_argument("--only", choices=SECTIONS,
                    action="append", help="run only these sections (repeatable)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "taskbar_test")
    only = set(args.only or SECTIONS)

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        try:
            # FIRST: the overflow checks at the end leave twenty Notepads
            # open, past which nothing more can spawn -- `config` included.
            if "height" in only:
                check_height(dbg, qmp, res)
            if "combine" in only:
                check_combine(dbg, res)
            if "closeall" in only:
                for name in TOUCHED:
                    dbg.send(f"sh config unset {name}")
                check_close_all(dbg, qmp, res)
            if "join" in only:
                for name in TOUCHED:
                    dbg.send(f"sh config unset {name}")
                check_close_all_join(dbg, qmp, res)
            if "names" in only:
                check_stayed_sentence(dbg, qmp, res)
            if "draghidden" in only:
                check_drag_hidden(dbg, qmp, res)
        finally:
            for name in TOUCHED:
                dbg.send(f"sh config unset {name}")
            dbg.settle()
        if "overflow" in only:
            run(dbg, qmp, res, args.pixels)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\ntaskbar_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
