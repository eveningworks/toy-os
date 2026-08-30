#!/usr/bin/env python3
"""Drives the RING-3 System Settings app (userland/gui/system/settings.c).

Run it after touching the settings registry (kernel/lib/setting.c,
api/setting.h, kernel/lib/setting_text.c), SYS_SETTING/SYS_SYSINFO, or
any widget the page is built from -- uui_tree, uui_label,
uui_radio_list, uui_dropdown, uui_checkbox, uui_statusbar and
uui_layout's `hidden` handling.

FOUR CHECKS CARRY THE WEIGHT, and each exists because of a bug
everything else stayed green through:

1. Selecting a choice must STAGE it and NOT write anything. The app was
   instant-apply until 2026-08-19; a staged model that quietly still
   applied would look identical on screen and differ only on disk.
   Asserted as: after clicking a choice, /etc is UNCHANGED; after
   clicking Apply, it changed.

2. A change is verified through an INDEPENDENT PATH: the BYTES ON DISK,
   read with `cat`. The app believing itself proves nothing -- the same
   reasoning notepad_client_test.py uses. (`/bin/config get` is useless
   here: a spawned program's STDOUT goes to its parent's pipe, not the
   kernel log, so the debug console cannot see it. Only stderr is
   readable from outside -- CLAUDE.md on sys_eprint.)

3. A GROUP PAGE must carry every setting in its group. The Mouse page is
   the fixture precisely because it holds four, from TWO different
   kernel files -- a page that showed only its own file's settings would
   pass any single-setting check.

4. The sidebar and the page must both come from the KERNEL. The counts
   are asserted as >=, never ==: the registry is meant to grow, and
   pinning a count turns a future setting into a failure in an unrelated
   file.

The app REPORTS its own geometry and its own rows (`settings: row ...`,
`settings: slot ...`), and this tool looks things up BY LABEL from that.
Never re-derive a row position from a font size here: that is what
gui_flow.py's calibrated constants have cost three re-measurements.

Typical use, against a VM someone else started:

    python3 tools/vm.py start
    python3 tools/settings_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, changed_rows, enter_gui   # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SETTINGS = "/bin/wm/system/settings"
CONF = "/etc/toyos.conf"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


# DebugConsole.logs() CLEARS what it returns, so every reader has to
# accumulate rather than re-query -- a value reported once at startup
# vanishes from the second call otherwise. One collector, many readers.
_log = []


def drain(dbg):
    _log.extend(l.strip() for l in dbg.logs())
    return _log


def layout(dbg):
    """The app's own reported geometry -- never re-derived in Python.

    A tool that computes widget offsets itself drifts the moment a row
    is added to the app, which has bitten four tools in this repo.
    """
    drain(dbg)
    out = {}
    for line in _log:
        m = re.search(r"settings: layout (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out


def setting_count(dbg):
    drain(dbg)
    n = None
    for line in _log:
        m = re.search(r"settings: settings (\d+)", line)
        if m:
            n = int(m.group(1))
    return n


def last(pattern):
    hits = [l for l in _log if re.search(pattern, l)]
    return hits[-1] if hits else None


def stored_value(dbg, key):
    """What /etc/toyos.conf actually holds for `key`, or None.

    Straight off the disk through the serial console's own `sh`, which
    shares nothing with the path the GUI used to write it -- different
    process, different code, no registry involved on the way out.
    """
    out = dbg.send(f"sh cat {CONF}")
    for line in out.splitlines():
        line = line.strip()
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    return None


def crop_ink(qmp, path, rect):
    """Non-background pixel count in a rect -- 'is anything drawn here?'

    Counting ink rather than sampling a point: a point moves when the
    layout does, and a blank page and a filled one are distinguishable
    by volume without knowing what the content is.
    """
    from PIL import Image
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")
    x, y, w, h = rect
    px = list(im.crop((x, y, x + w, y + h)).getdata())
    bg = max(set(px), key=px.count)          # the page's own fill
    return sum(1 for p in px if p != bg), px


def _since(mark, pattern):
    """Matches of `pattern` logged AFTER `mark`.

    drain() ACCUMULATES into one list rather than returning only what is
    new, so anything read without a mark sees every page ever opened --
    which is how the first version of this tool asked about the Mouse
    page and got the timezone page's control back.
    """
    out = []
    for line in _log[mark:]:
        m = re.search(pattern, line)
        if m:
            out.append(m)
    return out


CONTROL_RE = (r"settings: control (\d+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) "
               r"rows (\d+) kind (radio|combo|slider|spin)")


def slots(dbg, mark):
    """The controls on the page opened since `mark`.

    Read from the app's per-page `control` lines, NOT from a dump at
    startup: on_open runs once, so a startup dump describes the first
    page forever and a tool reading it while looking at another page
    gets a confident wrong answer.
    """
    drain(dbg)
    return [{"slot": int(m.group(1)), "name": m.group(2),
              "kind": m.group(8), "choices": int(m.group(7))}
            for m in _since(mark, CONTROL_RE)]


def controls(dbg, mark):
    """Each control's rect on the page opened since `mark`."""
    drain(dbg)
    out = {}
    for m in _since(mark, CONTROL_RE):
        out[m.group(2)] = {"x": int(m.group(3)), "y": int(m.group(4)),
                            "w": int(m.group(5)), "h": int(m.group(6)),
                            "rows": int(m.group(7)), "kind": m.group(8)}
    return out


CHOICE_RE = r"settings: choice (\d+) (\S+) raw (\S+) shown (.+)"


def choice_shown(dbg, mark, name):
    """(raw, shown) for `name`'s current choice, as the app reports it.

    The two are DIFFERENT STRINGS for a setting whose choices carry
    display names, and a screendump cannot tell "no display name" from
    "a value that happens to read like one".
    """
    drain(dbg)
    hits = [m for m in _since(mark, CHOICE_RE) if m.group(2) == name]
    if not hits:
        return None
    return hits[-1].group(3), hits[-1].group(4).strip()


def staged_for(dbg, mark, name):
    """The last value staged for `name` since `mark`, or None."""
    drain(dbg)
    hits = [m for m in _since(mark, r"settings: staged (\S+) (\S+)")
            if m.group(1) == name]
    return hits[-1].group(2) if hits else None


def page_line(dbg, mark):
    """The app's own `settings: page` summary for the page opened since `mark`.

    Carries `captions` and `disabled` because neither is visible in a
    screendump: a caption suppressed as a duplicate of the page title
    looks exactly like a setting that never had one, and a greyed
    control differs from a live one by a few units of colour.
    """
    drain(dbg)
    # `(.+?)` rather than `\S+`: a page name is "<category>/<group>"
    # and BOTH halves are human strings with spaces in them ("Time &
    # Locale/Time zone").
    hits = _since(mark, r"settings: page (.+?) slots (\d+) advanced (\d+) "
                        r"captions (\d+) disabled (\d+)")
    if not hits:
        return None
    m = hits[-1]
    return {"page": m.group(1), "slots": int(m.group(2)),
            "advanced": int(m.group(3)), "captions": int(m.group(4)),
            "disabled": int(m.group(5))}


def advanced_toggle(dbg, mark):
    drain(dbg)
    hits = _since(mark, r"settings: advanced_toggle (-?\d+) (-?\d+) (-?\d+) (-?\d+) shown (\d+)")
    if not hits:
        return None
    m = hits[-1]
    return {"x": int(m.group(1)), "y": int(m.group(2)), "w": int(m.group(3)),
             "h": int(m.group(4)), "shown": int(m.group(5))}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("system settings (the settings registry, in ring 3)")

    # THE TIMEZONE FIXTURE, set BEFORE the app starts. The app reads a
    # setting's value when it builds a page and caches it, so a value
    # changed from outside mid-run is not necessarily what the next page
    # shows -- which failed as "no display name" and was a stale read.
    #
    # Los Angeles specifically: its stored token ("losangeles") and its
    # display name differ by more than case, which "utc"/"UTC" does not
    # -- and it is far enough down a 92-city list that it is never the
    # popup's FIRST row, which the hover check below depends on.
    dbg.send("sh config set system.timezone losangeles")
    dbg.settle()
    for _ in range(20):
        if stored_value(dbg, "timezone") == "losangeles":
            break
        time.sleep(0.3)

    # NOT DebugConsole.spawn(): it polls with logs(), which CLEARS what
    # it returns, and would eat the app's own startup lines.
    dbg.send(f"gui spawn {SETTINGS}")
    deadline = time.time() + 8
    while time.time() < deadline:
        time.sleep(0.3)
        drain(dbg)
        if any("settings: layout" in l for l in _log):
            break
    dbg.settle()

    wins = [w for w in dbg.json("gui windows --json")["windows"]
            if w["title"] == "System Settings"]
    win = wins[-1] if wins else None
    if not check("System Settings opened", win is not None):
        print("\nnothing to drive -- the checks below would be vacuous")
        return report()

    check("it is a ring-3 client", win.get("client_pid", 0) != 0,
          f"client_pid={win.get('client_pid')}")

    cx, cy = win["content"]["x"], win["content"]["y"]
    geo = layout(dbg)
    check("it reports its own geometry",
          all(k in geo for k in ("tree", "page", "buttons")),
          f"keys={sorted(geo)}")
    if "tree" not in geo:
        return report()

    tx, ty, tw, th = geo["tree"]
    px0, py0, pw0, ph0 = geo["page"]
    bx, by, bw, bh = geo["buttons"]

    n = setting_count(dbg)
    # From the REGISTRY, not a list in the app. >= rather than ==: the
    # registry is meant to grow.
    check("it listed the registered settings", (n or 0) >= 9, f"{n} settings")

    # --- the sidebar is a tree, built from the kernel's grouping ------
    rows = []
    for line in _log:
        m = re.search(r"settings: row (\d+) id (\d+) y (-?\d+) depth (\d+) (.+)", line)
        if m:
            rows.append({"row": int(m.group(1)), "id": int(m.group(2)),
                          "y": int(m.group(3)), "depth": int(m.group(4)),
                          "label": m.group(5).strip()})
    check("the sidebar is a TREE, not a flat list",
          any(r["depth"] == 0 for r in rows) and any(r["depth"] == 1 for r in rows),
          f"{len(rows)} rows, depths={sorted({r['depth'] for r in rows})}")
    check("its headings come from the registry's categories",
          len([r for r in rows if r["depth"] == 0]) >= 3,
          f"{len([r for r in rows if r['depth'] == 0])} headings")

    # --- THE SIDEBAR'S WIDTH IS DRAGGABLE ----------------------------
    #
    # The same uui_splitter the File Manager's dividers are, placed in a
    # uui_layout row -- so this is the check that the LAYOUT-PLACED case
    # works, where files.c exercises the hand-computed one. The page is
    # the neighbour that must move with it (CLAUDE.md: half the
    # assertion is what did NOT stay put).
    check("the body has a divider between the sidebar and the page",
          "split" in geo and geo["split"][0] >= tx + tw,
          f"split={geo.get('split')} tree={geo.get('tree')}")
    if "split" in geo:
        sx, sy, sw, sh = geo["split"]
        dbg.warp_cursor(qmp, cx + sx + sw // 2, cy + sy + sh // 2)
        qmp.mouse_down()
        time.sleep(0.2)
        for i in (1, 2, 3, 4):
            dbg.warp_cursor(qmp, cx + sx + sw // 2 + 15 * i, cy + sy + sh // 2)
        qmp.mouse_up()
        time.sleep(0.6)
        geo2 = layout(dbg)
        t2 = geo2.get("tree", (0, 0, 0, 0))
        p2 = geo2.get("page", (0, 0, 0, 0))
        check("dragging it widens the sidebar and narrows the page",
              t2[2] > tw + 20 and p2[2] < pw0 - 20,
              f"tree w {tw} -> {t2[2]}, page w {pw0} -> {p2[2]}")
        # SAMPLED WHILE THE POINTER IS ON THE BAND, with the sidebar as
        # the control -- the first version of this check read the shape
        # after the drag had already parked the cursor 60px away, and a
        # correct resize cursor reported as absent.
        # OFF THE BAND FIRST, then onto it. A drag ends with the pointer
        # where the band now IS, so warping "to the band" can be a move
        # of zero pixels -- no motion event, no cursor query, and the
        # client's last shape stands whatever it was. Coming from
        # elsewhere guarantees the event this is measuring.
        s2 = geo2.get("split", (sx, sy, sw, sh))
        dbg.warp_cursor(qmp, cx + t2[0] + t2[2] // 2, cy + t2[1] + t2[3] // 2)
        time.sleep(0.4)
        off_band = dbg.cursor_shape()
        dbg.warp_cursor(qmp, cx + s2[0] + s2[2] // 2, cy + s2[1] + s2[3] // 2)
        time.sleep(0.4)
        on_band = dbg.cursor_shape()
        check("the pointer over it is the resize cursor, and not beside it",
              on_band == DebugConsole.CURSOR_H and
              off_band == DebugConsole.CURSOR_NORMAL,
              f"on band={on_band} off band={off_band}")
        # Put it back, or every later check in this tool -- and every
        # tool after it, since this app WRITES -- sees a narrowed page.
        dbg.warp_cursor(qmp, cx + s2[0] + s2[2] // 2, cy + s2[1] + s2[3] // 2)
        qmp.click()
        time.sleep(0.15)
        qmp.click()
        time.sleep(0.6)
        geo = layout(dbg)
        tx, ty, tw, th = geo.get("tree", (tx, ty, tw, th))
        px0, py0, pw0, ph0 = geo.get("page", (px0, py0, pw0, ph0))

    def row_named(sub):
        for r in rows:
            if sub.lower() in r["label"].lower():
                return r
        return None

    # --- KERNEL TUNABLES APPEAR, AND UNDER THEIR OWN HEADING ----------
    #
    # A tunable is a setting with no config file (kernel/lib/tunables.c),
    # and the sidebar is generated from the registry -- so this asserts
    # that "no file" did not quietly mean "no row". It is filed under
    # "Kernel" deliberately, separate from Appearance and Input: these
    # are machine knobs, and a heap-debug toggle sitting beside the
    # wallpaper would be a worse app.
    kernel_head = None
    for r in rows:
        if r["depth"] == 0 and r["label"].strip().lower() == "kernel":
            kernel_head = r
            break
    check("the sidebar has a Kernel heading for the tunables",
          kernel_head is not None,
          f"headings={[r['label'] for r in rows if r['depth'] == 0]}")
    # Its three groups, which is what proves the ROWS came through and
    # not just the heading -- a category with no settings under it would
    # not be drawn at all, so the heading alone is weaker than it looks.
    for group in ("Memory", "Storage", "Diagnostics"):
        check(f"the Kernel section offers a {group} page",
              row_named(group) is not None,
              f"labels={[r['label'] for r in rows]}")

    def click(rel_x, rel_y):
        dbg.send(f"gui click {cx + rel_x} {cy + rel_y}")
        dbg.settle()
        # A SECOND FRAME, forced. The app reports its control geometry
        # from on_draw, which runs BEFORE the layout has placed a newly
        # opened page -- so the first frame after a page change still
        # describes the previous page. A redraw requested from inside
        # on_draw coalesces into the frame already in progress, so the
        # nudge has to come from out here.
        dbg.send(f"gui move {cx + rel_x} {cy + rel_y + 1}")
        dbg.settle()
        time.sleep(0.4)
        drain(dbg)

    # --- A GROUP PAGE CARRIES SEVERAL SETTINGS ------------------------
    #
    # The Mouse page is the fixture because its four settings come from
    # TWO kernel files (cursor_theme_config.c and mouse_config.c), so a
    # page that only gathered its own file's settings would fail here
    # and pass any single-setting check.
    mouse_row = row_named("Mouse")
    if not check("the sidebar offers a Mouse page", mouse_row is not None,
                 f"labels={[r['label'] for r in rows]}"):
        return report()
    mark = len(drain(dbg))
    click(tx + tw // 2, mouse_row["y"])
    page_slots = slots(dbg, mark)
    qmp.screenshot(f"{args.tmp}/settings_mousepage.png")
    check("a group page carries SEVERAL settings", len(page_slots) >= 4,
          f"{len(page_slots)} controls: {[s['name'] for s in page_slots]}")
    # EVERY control positioned, not just present. This is the direct
    # guard for the bug that cost this page most of a session: a scroll
    # view lays its content out only when its own rect or offset moves,
    # so swapping the item list left every NEW widget at a zero rect --
    # invisible and unclickable, and reading exactly like a layout that
    # stops after four children. uui_scrollview notices for itself now;
    # this is what would catch it coming back.
    zero = [k for k, v in controls(dbg, mark).items() if v["w"] <= 0 or v["h"] <= 0]
    check("every control on the page was positioned", not zero,
          f"zero-sized: {zero}" if zero else "all have a real rect")

    # A MULTI-SETTING PAGE LABELS EVERY CONTROL. The counterpart of the
    # timezone check below -- without this one, "captions == 0" would
    # pass by the app simply never drawing a caption at all.
    mouse_page = page_line(dbg, mark)
    check("a multi-setting page captions every control",
          mouse_page is not None and mouse_page["captions"] == mouse_page["slots"],
          f"{mouse_page}" if mouse_page else "no page line")

    check("...gathered from more than one kernel file",
          any("mouse_" in s["name"] for s in page_slots) and
          any("cursor_" in s["name"] for s in page_slots),
          f"{[s['name'] for s in page_slots]}")

    # --- the control TYPE follows the choice count / the text file ----
    tz_row = row_named("Time zone") or row_named("Time")
    if not check("the sidebar offers a timezone page", tz_row is not None):
        return report()
    mark = len(drain(dbg))
    click(tx + tw // 2, tz_row["y"])
    tz_slots = slots(dbg, mark)
    tz = [s for s in tz_slots if "timezone" in s["name"]]
    check("the timezone page loaded every city", tz and tz[-1]["choices"] >= 50,
          f"{tz[-1]['choices'] if tz else 0} choices")
    # A ONE-CONTROL PAGE DOES NOT PRINT ITS OWN NAME TWICE. A setting
    # with no `group` gets a page named by its LABEL, so the heading and
    # the sole caption were the same string -- "Time zone" over "Time
    # zone", and the same on every other single-setting page.
    tz_page = page_line(dbg, mark)
    check("a one-control page does not repeat its title as a caption",
          tz_page is not None and tz_page["slots"] == 1 and tz_page["captions"] == 0,
          f"{tz_page}" if tz_page else "no page line")

    check("...and a long list uses a DROPDOWN, not radio buttons",
          bool(tz) and tz[-1]["kind"] == "combo",
          f"kind={tz[-1]['kind'] if tz else '?'}")
    # NOT mouse_speed any more -- it became SETTING_TYPE_INT and has no
    # choices to be short. cursor_size is the short ENUM on this page,
    # and it is what this check was always about.
    mouse_speed = [s for s in page_slots if "cursor_size" in s["name"]]
    check("...while a short list stays radio buttons",
          bool(mouse_speed) and mouse_speed[-1]["kind"] == "radio",
          f"kind={mouse_speed[-1]['kind'] if mouse_speed else '?'}")

    # --- THE TIMEZONE DROPDOWN: names, hover, and type-ahead ----------
    #
    # All three on the one page with a long list, which is the only
    # place any of them matters.
    #
    # NAVIGATE AWAY AND BACK, not straight back to the page already
    # open: the app's per-frame layout block is DEDUPED, so re-opening
    # the page it is already showing logs nothing at all and every
    # reader below gets None. Going via the Mouse page makes the next
    # block genuinely different.
    #
    # The value itself was set before the app started -- see the fixture
    # at the top, and why it has to be there rather than here.
    click(tx + tw // 2, mouse_row["y"])
    mark_tz = len(drain(dbg))
    click(tx + tw // 2, tz_row["y"])
    shown = choice_shown(dbg, mark_tz, "system.timezone")
    check("the timezone's stored value is still a token",
          shown is not None and shown[0] == "losangeles",
          f"{shown}")
    check("...and what the dropdown shows is a real name",
          shown is not None and shown[1].startswith("Los Angeles"),
          f"shown={shown[1]!r}" if shown else "no choice line")

    tz_ctl = controls(dbg, mark_tz).get("system.timezone")
    if not check("the timezone control reported a rect", tz_ctl is not None):
        return report()
    tz_cx = cx + tz_ctl["x"] + tz_ctl["w"] // 2
    tz_cy = cy + tz_ctl["y"] + tz_ctl["h"] // 2

    # --- HOVER, MEASURED AS PIXELS ------------------------------------
    #
    # The bug this exists for: uui_router_motion() delivered a move
    # event ONE level of containers deep while press and wheel recursed
    # to any depth, so nothing below this page's scroll view ever heard
    # the pointer and no hover state in the app could light up. It read
    # as a missing feature and was a routing bug.
    #
    # THE REAL CURSOR AND TWO SETTLED FRAMES, via
    # DebugConsole.hover_frames(): `gui move` is one WM iteration and
    # the pointer then snaps back to where the mouse really is, so a
    # capture taken after it shows no hover at all. See gui_debug.py,
    # which carries the rest of the reasoning.
    dbg.send(f"gui click {tz_cx} {tz_cy}")          # opens the popup
    dbg.settle()
    time.sleep(0.4)

    popup_x = cx + tz_ctl["x"]
    popup_top = cy + tz_ctl["y"] + tz_ctl["h"]
    # Parked at the popup's LEFT EDGE both times, and sampled well to
    # the right of it, so the pointer's own pixels are never in the
    # measurement -- otherwise "the row got darker" would be satisfied
    # by the cursor being drawn on it.
    #
    # ROW 0 for the hover, and it must not be the SELECTED row: a
    # selected row draws with the selection colour and hover correctly
    # does not override it, so hovering the current city would measure
    # nothing and read as a dead hover. The fixture is what guarantees
    # that (see the top).
    rest_png, hover_png = dbg.hover_frames(
        qmp, args.tmp,
        rest_at=(popup_x + 4, popup_top + 130),   # below the rows
        hover_at=(popup_x + 4, popup_top + 6),    # row 0
        prefix="settings-popup")
    seen = changed_rows(rest_png, hover_png,
                        (popup_x + 40, popup_top + 1,
                         popup_x + tz_ctl["w"] - 8, popup_top + 120))

    check("hovering a dropdown row highlights it", bool(seen["rows"]),
          f"{len(seen['rows'])} rows changed below y={popup_top}")
    if seen["rows"]:
        first = seen["rows"][0]
        # DARKER, not merely different: on this near-white theme
        # uui_state_bg() has to darken, and a hover that brightened
        # would be the bug docs/gui-guidelines.md warns about.
        check("...and it gets darker, as a light theme must",
              seen["hover"][first] < seen["rest"][first],
              f"y={first}: {seen['rest'][first]:.1f} -> {seen['hover'][first]:.1f}")
        # THE CONTROL POINT, and the half that carries the weight: every
        # OTHER row must be untouched. Without it, a repaint, a scroll,
        # or the whole list lighting up at once would all pass.
        lo, hi = seen["band"]
        check("...and only that row",
              lo <= popup_top + 6 <= hi and (hi - lo) < 40,
              f"changed band y={seen['band']}, pointer at {popup_top + 6}")

    # --- TYPE-AHEAD ---------------------------------------------------
    #
    # The popup is open and owns the keyboard (ui/uui_route.h's overlay
    # rule), so these keys reach the list with no focus ring involved.
    # Asserted through the app's STAGED value -- a fact, and the same
    # channel a click commits through -- not through pixels.
    mark_key = len(drain(dbg))
    dbg.key(ord("h"))
    first = staged_for(dbg, mark_key, "system.timezone")
    check("typing a letter jumps to that letter", bool(first) and first[0] == "h",
          f"staged={first!r}")

    mark_key = len(drain(dbg))
    dbg.key(ord("h"))
    second = staged_for(dbg, mark_key, "system.timezone")
    check("...and pressing it again cycles to the next one",
          bool(second) and second[0] == "h" and second != first,
          f"{first!r} -> {second!r}")

    # TWO KEYS WITHOUT WAITING: a prefix, not two independent jumps.
    # Sent back to back on purpose -- the window is a second, and this
    # is what separates "ho" from "an h, then an o".
    mark_key = len(drain(dbg))
    dbg.key(ord("h"), settle=False)
    dbg.key(ord("o"), settle=False)
    dbg.settle()
    time.sleep(0.4)
    prefix = staged_for(dbg, mark_key, "system.timezone")
    check("...and two quick keys build a prefix",
          bool(prefix) and prefix.startswith("ho"),
          f"staged={prefix!r} (an 'o' city would mean the prefix expired)")

    # NOTHING WAS WRITTEN. Type-ahead stages like every other control
    # here; a keyboard path that applied directly would be the bug the
    # whole staged model exists to prevent.
    check("...and none of it reached the disk",
          stored_value(dbg, "timezone") == "losangeles",
          f"stored={stored_value(dbg, 'timezone')!r}")

    dbg.key(0x1B)   # Esc: put the popup away before the next section
    dbg.settle()
    # And put the pointer back where the WM starts it, so the sections
    # below photograph the same screen they always did.
    dbg.warp_cursor(qmp, 640, 360)

    # --- Widget= in /etc overrides the count rule ---------------------
    #
    # Pointer acceleration has four choices -- fewer than the dropdown
    # threshold -- and asks for a SLIDER, because its values are ordered
    # levels. So this checks the FILE's word beating the count, which is
    # the whole point of the hint.
    accel = [s for s in page_slots if "mouse_accel" in s["name"]]
    check("a setting can ask for a slider, and /etc's word wins",
          bool(accel) and accel[-1]["kind"] == "slider",
          f"kind={accel[-1]['kind'] if accel else '?'}, choices="
          f"{accel[-1]['choices'] if accel else 0}")

    # --- SELECTING STAGES; APPLY WRITES -------------------------------
    #
    # The check that would catch a staged model that quietly still
    # applied: /etc must be UNCHANGED after the click and CHANGED after
    # Apply. Cursor style is not used -- it is marked Advanced and so is
    # not on a page by default, which is itself asserted below.
    # ESTABLISH the baseline rather than inherit it. The key may never
    # have been written, in which case stored_value() is None -- and
    # "None -> None" passes the unchanged check vacuously, which is
    # exactly the shape of a test that measures nothing.
    dbg.send("sh config set system.mouse_speed normal")
    dbg.settle()
    time.sleep(0.4)
    mark = len(drain(dbg))
    click(tx + tw // 2, mouse_row["y"])
    page_slots = slots(dbg, mark)
    before = stored_value(dbg, "mouse_speed")
    # A NUMBER NOW, not a named level. mouse_speed became
    # SETTING_TYPE_INT -- a percentage with a registry-enforced range --
    # so the baseline is whatever digits are on disk, or None when the
    # key has never been written (a fresh image, which is the normal
    # case for this suite).
    if not check("the baseline is on disk or absent, and numeric if present",
                 before is None or before.isdigit(), f"stored={before!r}"):
        return report()
    speed = [s for s in page_slots if "mouse_speed" in s["name"]]
    if not check("the Mouse page has the speed control", bool(speed)):
        return report()

    # The CONTROL's own rect, reported by the app after the layout ran.
    ctls = controls(dbg, mark)
    speed_ctl = ctls.get("system.mouse_speed")
    if not check("the app reported the speed control's rect", speed_ctl is not None,
                 f"controls={sorted(ctls)}"):
        return report()

    # A SPINBOX NOW: click its UP stepper, which is the rightmost
    # ~14px, top half. That is a real change whatever the current value
    # is -- unless it is already at the maximum, which the range makes
    # impossible for a default machine.
    #
    # The rect comes from the app, so this does not hardcode where the
    # control sits; only the stepper's width is a widget constant, and
    # it is checked by the staged value moving rather than assumed.
    check("the speed control is a spinbox", speed_ctl["kind"] == "spin",
          f"kind={speed_ctl['kind']}")

    # SCROLLED INTO VIEW FIRST, like the slider flow below: the Mouse
    # group grew the two scroll-wheel settings (mouse.scroll_*), which
    # pushed this control below the fold at the default window size --
    # where a click at its reported rect lands on whatever the scroll
    # view left there instead (it staged the TIMEZONE, one page up).
    page_h = win["content"]["h"]
    for _ in range(4):
        if speed_ctl["y"] + speed_ctl["h"] < page_h - 30:
            break
        mark_sv = len(drain(dbg))
        dbg.warp_cursor(qmp, cx + speed_ctl["x"] + 10, cy + page_h // 2)
        for _ in range(3):
            dbg.send("gui wheel -1")
        dbg.settle()
        time.sleep(0.4)
        fresh = controls(dbg, mark_sv)
        speed_ctl = fresh.get("system.mouse_speed") or speed_ctl

    click_y = speed_ctl["y"] + speed_ctl["h"] // 4
    click(speed_ctl["x"] + speed_ctl["w"] - 7, click_y)
    # ASSERTED ON A LOGGED FACT, not on the status bar's pixels: the
    # first version looked for the bar's wording in the log, where it
    # never appears, so it failed while the app was working perfectly.
    staged = last(r"settings: staged (\S+) (\S+)")
    check("stepping the spinbox reaches the control",
          staged is not None and "mouse_speed" in staged,
          f"rect={speed_ctl}, clicked ({speed_ctl['x'] + speed_ctl['w'] - 7}, "
          f"{click_y}), staged: {staged or 'nothing'}")
    # ...and it staged a NUMBER IN RANGE. A spinbox that reported an
    # index (which is what every other control on this page stages)
    # would still "reach the control" and then write 1 as the pointer
    # speed -- which the registry would refuse, so the failure would
    # surface as a mysterious Apply error rather than here.
    if staged:
        val = staged.split()[-1]
        check("...and it staged a number, not a choice index",
              val.isdigit() and speed_ctl["imin"] <= int(val) <= speed_ctl["imax"]
              if "imin" in speed_ctl else val.isdigit(),
              f"staged {val!r}")

    # THE LOAD-BEARING CHECK. Selecting must STAGE, not write. A staged
    # model that quietly still applied would look identical on screen.
    mid = stored_value(dbg, "mouse_speed")
    check("selecting a choice does NOT write it yet",
          mid == before, f"{before!r} -> {mid!r} (should be unchanged)")
    check("...and the app staged it rather than applying it",
          staged is not None and last(r"settings: set \S+") is None,
          f"staged={staged!r}, any set line={last(r'settings: set .*')!r}")

    # Now Apply.
    click(bx + bw + 6 + bw // 2, by + bh // 2)   # the middle button
    time.sleep(0.5)
    drain(dbg)
    after = stored_value(dbg, "mouse_speed")
    check("Apply writes the staged change",
          after is not None and after != before, f"{before!r} -> {after!r}")
    said = last(r"settings: set (\S+) (\S+) result (\d+)")
    check("...and the registry reported it SAVED (not merely applied)",
          bool(said) and said.rstrip().endswith("result 1"), said or "no set line")

    # --- the slider stages a change like any other control -----------
    #
    # Dragged, not clicked, because a slider's whole job is the drag --
    # and the pointer GRAB is what makes a drag keep reaching the widget
    # once the cursor leaves it. A click-only check would pass on a
    # slider whose motion handling was missing entirely.
    # SCROLL IT INTO VIEW FIRST. The Mouse page is taller than the
    # window, and a scroll view correctly refuses to route a press to a
    # child outside its viewport -- so a control below the fold is not
    # merely hard to hit, it is unreachable, and a test clicking at its
    # unscrolled coordinates gets silence. The app re-reports its rects
    # whenever they move, so the post-scroll geometry is what to use.
    mark_scroll = len(drain(dbg))
    dbg.warp_cursor(qmp, cx + px0 + pw0 // 2, cy + py0 + ph0 // 2)
    for _ in range(6):
        dbg.send("gui wheel -1")
    dbg.settle()
    time.sleep(0.4)
    scrolled = controls(dbg, mark_scroll)
    accel_ctl = scrolled.get("system.mouse_accel") or ctls.get("system.mouse_accel")
    qmp.screenshot(f"{args.tmp}/settings_slider.png")
    check("scrolling moved the page's controls",
          bool(scrolled), f"{len(scrolled)} control(s) re-reported after the wheel")
    if accel_ctl:
        mark3 = len(drain(dbg))
        y_mid = accel_ctl["y"] + 5
        dbg.send(f"gui drag {cx + accel_ctl['x'] + 4} {cy + y_mid} "
                 f"{cx + accel_ctl['x'] + accel_ctl['w'] - 4} {cy + y_mid}")
        dbg.settle()
        time.sleep(0.4)
        drain(dbg)
        dragged = None
        for m in _since(mark3, r"settings: staged (\S+) (\S+)"):
            if "mouse_accel" in m.group(1):
                dragged = m.group(2)
        check("dragging the slider stages a new value", dragged is not None,
              f"staged {dragged!r} from a drag across the track")
        # To the far END of the track, so the value is the LAST option --
        # a drag that moved one stop would pass a weaker check.
        check("...and a drag to the end selects the last option",
              dragged == "high", f"got {dragged!r}, wanted 'high'")

    # --- Advanced= keeps a setting off the page until asked ----------
    #
    # system.cursor_style is marked Advanced=1 in /etc/settings.d, so it
    # must NOT be on its page by default and MUST appear once the toggle
    # is checked. Both halves matter: the first alone would pass if the
    # setting had simply vanished from the registry.
    appearance = row_named("Console cursor") or row_named("Appearance")
    if appearance:
        mark = len(drain(dbg))
        click(tx + tw // 2, appearance["y"])
        page = slots(dbg, mark)
        check("an Advanced setting is NOT on the page by default",
              not any("cursor_style" in s["name"] for s in page),
              f"{[s['name'] for s in page]}")
        adv = advanced_toggle(dbg, mark)
        if adv and adv["shown"]:
            mark2 = len(drain(dbg))
            click(adv["x"] + 6, adv["y"] + adv["h"] // 2)
            page = slots(dbg, mark2)
            check("...and the toggle reveals it",
                  any("cursor_style" in s["name"] for s in page),
                  f"{[s['name'] for s in page]}")

    # --- A LONG DESCRIPTION WRAPS; A SHORT ONE DOES NOT ---------------
    #
    # Both halves, and the second is the one that keeps the first
    # honest. Reserving two rows for EVERY description also makes every
    # long one fit -- and it was the first implementation, which pushed
    # the last control of this page below the scroll fold, where a
    # control is unreachable rather than merely awkward. So this asserts
    # that a description wraps ONLY when it has to.
    #
    # Self-calibrating: the app reports the text's width and the label's
    # width alongside the row count, so the expected answer is computed
    # from the same numbers the app used rather than from a threshold
    # someone picked and nobody re-checks when the font changes.
    prose = []
    for line in drain(dbg) + _log:
        m = re.search(r"settings: prose \d+ (\S+) rows (\d+) width (\d+) text (\d+)", line)
        if m:
            prose.append({"name": m.group(1), "rows": int(m.group(2)),
                          "width": int(m.group(3)), "text": int(m.group(4))})
    if check("the app reports its description metrics", len(prose) > 0,
             f"{len(prose)} prose lines"):
        wrapped = [p for p in prose if p["text"] > p["width"] > 0]
        plain = [p for p in prose if 0 < p["text"] <= p["width"]]
        check("a description too wide for its label wraps to two rows",
              all(p["rows"] >= 2 for p in wrapped),
              f"{[(p['name'], p['rows'], p['text'], p['width']) for p in wrapped if p['rows'] < 2]}")
        check("...and one that fits does NOT spend a second row",
              all(p["rows"] == 1 for p in plain),
              f"{[(p['name'], p['rows'], p['text'], p['width']) for p in plain if p['rows'] != 1]}")

    # --- THE SYSTEM INFORMATION PAGE ACTUALLY DRAWS -------------------
    #
    # PIXELS, not the app's log, and that is the whole point of this
    # check. The page shipped EMPTY: it painted its text from on_draw,
    # which uapp.c runs BEFORE the widgets, so the scroll view's
    # background covered every line the moment it was drawn. Nothing
    # else could see it -- the title, the status bar and the sidebar
    # selection were all correct, and the app logged that it had drawn.
    # "It responds" is not "it is drawn" (CLAUDE.md).
    #
    # It compares against the SAME REGION on a settings page, so the
    # threshold is not a number picked out of the air: a page that draws
    # controls has ink there, and a blank one does not.
    sysinfo_row = row_named("System Information")
    if check("the sidebar offers a System Information page",
             sysinfo_row is not None,
             f"labels={[r['label'] for r in rows]}"):
        from PIL import Image

        def page_ink(tag):
            path = os.path.abspath(os.path.join(args.tmp, f"settings-{tag}.png"))
            qmp.screenshot(path)
            im = Image.open(path).convert("RGB")
            # The page BODY: below the title/description rows, so the
            # title -- which drew correctly even when the body did not --
            # cannot satisfy this on its own.
            bx = cx + px0 + 8
            by = cy + py0 + 3 * 22
            crop = im.crop((bx, by, bx + pw0 - 16, cy + py0 + ph0 - 8))
            px = list(crop.getdata())
            bg = max(set(px), key=px.count)     # the panel colour, whatever it is
            return sum(1 for p in px if p != bg)

        click(tx + tw // 2, mouse_row["y"])
        control_ink = page_ink("mouse")
        click(tx + tw // 2, sysinfo_row["y"])
        info_ink = page_ink("sysinfo")
        check("the System Information page draws its text",
              info_ink > control_ink // 4,
              f"sysinfo={info_ink} px vs a settings page={control_ink} px")

    # --- Cancel closes without writing --------------------------------
    mark = len(drain(dbg))
    click(tx + tw // 2, mouse_row["y"])
    speed_ctl = controls(dbg, mark).get("system.mouse_speed", speed_ctl)
    on_disk = stored_value(dbg, "mouse_speed")
    # Step it DOWN this time, so the staged value differs from the one
    # the Apply test above left behind -- staging the value that is
    # already stored does nothing at all (setting_set() short-circuits
    # it), and a Cancel test whose change was a no-op proves nothing.
    click(speed_ctl["x"] + speed_ctl["w"] - 7,
          speed_ctl["y"] + speed_ctl["h"] * 3 // 4)
    click(bx + 2 * (bw + 6) + bw // 2, by + bh // 2)   # Cancel
    time.sleep(0.6)
    check("Cancel discards the staged change",
          stored_value(dbg, "mouse_speed") == on_disk,
          f"{on_disk!r} still on disk")

    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    if failed:
        # THE APP'S OWN LINES, on failure only. This tool drains them
        # into _log to make its assertions and used to print none of
        # them, so a failing check reported a coordinate and nothing
        # about what the app thought it drew -- and the log file this
        # writes contained no trace of the app at all. Same rule as
        # naming the phase in a truncated ktest summary.
        print("\n--- what the app reported ---")
        for line in _log:
            if "settings:" in line:
                print("   ", line)
    print(f"\nsettings_test: {passed} passed, {failed} failed")
    for name, ok, _ in checks:
        if not ok:
            print(f"  FAILED: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
