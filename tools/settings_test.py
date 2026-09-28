#!/usr/bin/env python3
"""Drives the RING-3 System Settings app (userland/gui/system/settings.c).

Run it after touching the settings registry (kernel/lib/setting.c,
api/setting.h, kernel/lib/setting_text.c), SYS_SETTING/SYS_SYSINFO, or
any widget the page is built from -- uui_sidebar, uui_setting_row,
uui_switch, uui_segmented, uui_radio_list, uui_dropdown, uui_label,
uui_checkbox, uui_dialog and uui_layout's `hidden` handling.

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
import port_guard  # noqa: E402

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
               r"rows (\d+) kind (radio|combo|slider|spin|text|keycap|switch|segmented)")


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


def row_y(rows, index, top, pitch=None):
    """Where sidebar row `index` sits with the list scrolled to `top`.

    The app dumps every row's y ONCE, at top 0 (settings.c), so a row
    below the fold is reported where it WOULD be unscrolled -- outside
    the sidebar, and for the last rows outside the window. This is the
    only geometry this tool derives, and it derives it from the app's own
    numbers: two reported rows give the pitch, the reported offset gives
    the rest.
    """
    if pitch is None:
        pitch = rows[1]["y"] - rows[0]["y"] if len(rows) > 1 else 20
    return rows[0]["y"] + pitch * (index - top)


def fully_inside(ctl, view_y, view_h, margin=4):
    """Whether ALL of a control's rect is inside a scroll view.

    The clip is at the VIEWPORT EDGE, so a control straddling it is half
    routable: the mouse-speed spinbox stepped up and never down while a
    tool scrolled it until merely its top was visible.
    """
    return (ctl is not None and ctl["y"] >= view_y
            and ctl["y"] + ctl["h"] <= view_y + view_h - margin)


def wait_page(dbg, mark, want, timeout=6.0):
    """Poll until the app says a page whose name contains `want` is open.

    Waiting on the app's own report rather than sleeping: a click that
    missed and a page that is merely slow look identical for the first
    few hundred milliseconds, and only one of them ever resolves.
    """
    deadline = time.time() + timeout
    while True:
        got = page_line(dbg, mark) or page_line(dbg, 0)
        if got and want.lower() in got["page"].lower():
            return got
        if time.time() >= deadline:
            return None
        time.sleep(0.15)


def sidebar_state(dbg):
    """Where the sidebar is scrolled to, as the app last reported it.

    Reported ON CHANGE (settings.c), so while the app stays quiet the
    newest line IS the current position -- and a fixed sleep after an
    input reads whatever was there before the frame it asked for.
    """
    drain(dbg)
    hits = _since(0, r"settings: sidebar top (-?\d+) visible (\d+) rows (\d+)")
    if not hits:
        return None
    m = hits[-1]
    return {"top": int(m.group(1)), "visible": int(m.group(2)),
             "rows": int(m.group(3))}


def wait_sidebar(dbg, pred, timeout=4.0):
    """Poll the reported scroll position until `pred` holds, or None."""
    deadline = time.time() + timeout
    while True:
        sb = sidebar_state(dbg)
        if sb and pred(sb):
            return sb
        if time.time() >= deadline:
            return None
        time.sleep(0.15)


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
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "settings_test")

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
          all(k in geo for k in ("search", "tree", "page", "reset", "apply")),
          f"keys={sorted(geo)}")
    if "tree" not in geo or "apply" not in geo:
        return report()

    tx, ty, tw, th = geo["tree"]
    px0, py0, pw0, ph0 = geo["page"]

    def press(button):
        """Click the footer's Reset or Apply where the app last said it is."""
        x, y, w, h = layout(dbg).get(button, geo[button])
        return click(x + w // 2, y + h // 2)

    n = setting_count(dbg)
    # From the REGISTRY, not a list in the app. >= rather than ==: the
    # registry is meant to grow.
    check("it listed the registered settings", (n or 0) >= 9, f"{n} settings")

    # --- the sidebar is a FLAT list of pages, from the registry -------
    rows = []
    for line in _log:
        m = re.search(r"settings: row (\d+) id (\d+) y (-?\d+) depth (\d+) (.+)", line)
        if m:
            rows.append({"row": int(m.group(1)), "id": int(m.group(2)),
                          "y": int(m.group(3)), "depth": int(m.group(4)),
                          "label": m.group(5).strip()})
    # **A HEADING PER CATEGORY, ITS PAGES UNDER IT.** Every heading
    # (depth 0) is an inert caption and every page (depth 1) a
    # destination, so the two can never be mistaken for each other --
    # which a flat list of pages and captions rendered alike once did.
    check("the sidebar is headings over pages",
          rows and {r["depth"] for r in rows} == {0, 1}
          and all(r["id"] == 0 for r in rows if r["depth"] == 0)
          and all(r["id"] > 0 for r in rows if r["depth"] == 1),
          f"{len(rows)} rows, depths={sorted({r['depth'] for r in rows})}")
    pages = [r for r in rows if r["depth"] == 1]
    check("its pages come from the registry's groups",
          len(pages) >= 10, f"{len(pages)} pages")

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
        """The first PAGE whose label contains `sub` -- never a heading."""
        for r in rows:
            if r["depth"] == 1 and sub.lower() in r["label"].lower():
                return r
        return None

    # THE ROW DUMP IS TAKEN ONCE, AT TOP 0. Every row's reported y is
    # therefore where it would be unscrolled, and rows past the fold are
    # reported outside the sidebar entirely -- so a row is aimed at
    # through its CURRENT position, derived from the one thing the app
    # keeps reporting: where it is scrolled to.
    def row_point(r, top):
        return tx + tw // 2, row_y(rows, r["row"], top)

    def item_point(sb):
        """A visible PAGE row, never a heading -- so anything restoring
        the scroll position does not depend on the inert-row case the
        checks below are testing."""
        for r in rows:
            if r["depth"] == 1 and sb["top"] <= r["row"] < sb["top"] + sb["visible"]:
                return row_point(r, sb["top"])
        return tx + tw // 2, ty + th // 2

    def scroll_into_view(r, tries=20):
        """Wheel until row `r` is on screen, one notch at a time against
        the app's own reported position. A notch count computed from the
        row pitch would be one assumption too many: the sidebar's step is
        the widget's, not this tool's."""
        sb = sidebar_state(dbg)
        for _ in range(tries):
            if sb is None:
                return None
            if sb["top"] <= r["row"] < sb["top"] + sb["visible"]:
                return sb
            down = r["row"] >= sb["top"] + sb["visible"]
            dbg.warp_cursor(qmp, cx + tx + tw // 2, cy + ty + th // 2)
            dbg.wheel(-1 if down else 1)
            was = sb["top"]
            sb = wait_sidebar(dbg, lambda s, was=was: s["top"] != was, timeout=2.0)
        return None

    def open_row(r):
        """Click sidebar row `r` where it IS, scrolling it into view
        first. Returns False with a failed check recorded if it cannot
        be reached."""
        if r is None:
            check("the sidebar row asked for exists", False, "no such row")
            return False
        sb = scroll_into_view(r)
        if sb is None:
            check(f"the {r['label']} row can be scrolled into view", False,
                  f"row {r['row']} of {len(rows)}, sidebar {sidebar_state(dbg)}")
            return False
        return click(*row_point(r, sb["top"]))

    def select_page(label, want=None, timeout=6.0):
        """Open a page by the name of its sidebar row and CONFIRM the app
        says it is open. Returns the page dict, or None with a failed
        check -- never the previous page, which is what a click that
        missed leaves on screen for the next assertion to read."""
        r = row_named(label)
        if r is None:
            check(f"the sidebar has a {label} row", False,
                  f"labels={[x['label'] for x in rows]}")
            return None
        mark = len(drain(dbg))
        if not open_row(r):
            return None
        got = wait_page(dbg, mark, want or label, timeout)
        if got is None:
            check(f"clicking {label} opens its page", False,
                  f"row {r['row']}, sidebar {sidebar_state(dbg)}; "
                  f"the app reports {page_line(dbg, 0)}")
        return got

    def reveal(name, ctl, tries=6):
        """Scroll the page until ALL of `name`'s control is inside the
        SCROLL VIEW, re-reading its rect from the app after each wheel.

        The viewport is the page rect, not the window: a control whose
        lower half hangs past it is drawn clipped, and a press there is
        clipped away too -- which is why the mouse-speed spinbox stepped
        UP and never DOWN. A control below the WINDOW is worse again: the
        click reaches the desktop.
        """
        for _ in range(tries):
            if fully_inside(ctl, py0, ph0):
                return ctl
            mark_sv = len(drain(dbg))
            dbg.warp_cursor(qmp, cx + (ctl["x"] if ctl else px0) + 10,
                            cy + py0 + ph0 // 2)
            for _ in range(3):
                dbg.send("gui wheel -1")
            dbg.settle()
            time.sleep(0.4)
            ctl = controls(dbg, mark_sv).get(name) or ctl
        return ctl

    # --- KERNEL TUNABLES APPEAR, AND UNDER THEIR OWN HEADING ----------
    #
    # A tunable is a setting with no config file (kernel/lib/tunables.c),
    # and the sidebar is generated from the registry -- so this asserts
    # that "no file" did not quietly mean "no row". It is filed under
    # "Kernel" deliberately, separate from Appearance and Input: these
    # are machine knobs, and a heap-debug toggle sitting beside the
    # wallpaper would be a worse app.
    # **EVERY ROW IS A PAGE, OR IT IS A RULE.** The sidebar is flat: no
    # captions at all, so there is no such thing as a row that looks
    # clickable and is not. That was the defect this replaced -- half
    # the top-level rows opened a page and half were inert captions,
    # rendered identically.
    #
    # The invariant is asserted over EVERY row rather than over a named
    # category, because the failure it guards against (a page-table
    # overflow) always cuts whatever is LAST, and a check naming a
    # category stops working the moment one is added. Storage and Kernel
    # both shipped stranded when MAX_GROUPS was a hand-picked 24 and 24
    # pages existed; with the caps derived from MAX_SETTINGS a dropped
    # page now shows up here as a missing row.
    # EVERY HEADING HAS A PAGE UNDER IT. The failure this guards against
    # (a page-table overflow) cuts whatever is LAST, which leaves a
    # heading with nothing beneath it -- asserted over every row rather
    # than a named category, so adding one cannot quietly retire it.
    orphans = [r["label"] for i, r in enumerate(rows)
               if r["depth"] == 0 and (i + 1 >= len(rows) or rows[i + 1]["depth"] != 1)]
    check("every heading has a page under it", not orphans, f"bare headings: {orphans}")

    def under(heading):
        """The page labels listed under `heading`."""
        out, inside = [], False
        for r in rows:
            if r["depth"] == 0:
                inside = r["label"] == heading
            elif inside:
                out.append(r["label"])
        return out

    # The kernel tunables are registered with no /etc file at all, so
    # this asserts that "no file" did not quietly mean "no row".
    check("the kernel tunables have pages of their own, under Kernel",
          "Memory" in under("Kernel") and "Diagnostics" in under("Kernel"),
          f"Kernel: {under('Kernel')}")
    # A name MAY repeat across categories now -- position says which --
    # but never within one, where nothing would tell the two apart.
    heads = [r["label"] for r in rows if r["depth"] == 0]
    dupes = [(h, l) for h in heads for l in set(under(h)) if under(h).count(l) > 1]
    check("no two pages in one category share a name", not dupes, f"{dupes}")
    # A category's only page keeps its own name, under the category's.
    check("a lone page sits under its category's heading",
          len(under("Sound")) == 1 and len(under("Storage")) == 1,
          f"Sound: {under('Sound')} Storage: {under('Storage')}")

    # Pages from three different categories, so the walk is exercised
    # across the list rather than at one end of it.
    for group in ("Shell", "Diagnostics", "Wallpaper"):
        check(f"the sidebar offers a {group} page",
              row_named(group) is not None,
              f"labels={[r['label'] for r in rows]}")

    cw, ch = win["content"]["w"], win["content"]["h"]

    def click(rel_x, rel_y):
        # REFUSED OUTSIDE THE CONTENT AREA, with a check rather than
        # silently. A sidebar row below the fold is still reported at its
        # unscrolled y, and clicking that landed on the TASKBAR, whose
        # button for this window MINIMIZES it -- after which the app
        # draws nothing, reports nothing, and every later check reads the
        # state it had before (see select_page()).
        if not (0 <= rel_x < cw and 0 <= rel_y < ch):
            check("a click stays inside the window", False,
                  f"({rel_x},{rel_y}) is outside the {cw}x{ch} content area")
            return False
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
        return True

    # --- an EFFECT's own options, in a window of their own ------------
    #
    # A minimize effect declares what it lets you change
    # (userland/lib/ueffect.h) and System Settings reaches those through
    # a Settings... button, not rows on the page. Three things matter
    # and none is visible in a screendump: the button appears ONLY where
    # there is something to configure, the options are in the DIALOG and
    # not on the page, and Cancel writes nothing.
    #
    # **EARLY, WHILE THE APP IS FRESH.** Run last, after the phases that
    # move the page scroll, stage a change and drag the splitter, the
    # sidebar click simply did not take -- and the failure said only
    # "page is None". A phase establishes its own preconditions; the
    # cheapest way here is not to inherit eighty checks of state.
    eff_page = select_page("Effects", "Appearance/Effects")
    if eff_page:
        # AN ON/OFF SETTING IS A SWITCH, and flipping it STAGES the other
        # value -- read from the app's report, then put back with Reset so
        # nothing below inherits it.
        sws = {n: c for n, c in controls(dbg, 0).items() if c["kind"] == "switch"}
        if check("effects: an on/off setting is a switch", bool(sws),
                 f"kinds={ {n: c['kind'] for n, c in controls(dbg, 0).items()} }"):
            name, c = sorted(sws.items())[0]
            was = choice_shown(dbg, 0, name)
            mk = len(drain(dbg))
            click(c["x"] + c["w"] // 4, c["y"] + c["h"] // 2)
            got = staged_for(dbg, mk, name)
            check("...and clicking it stages the other value",
                  got in ("on", "off") and was is not None and got != was[0],
                  f"{name}: {was} -> {got!r}")
            press("reset")
            time.sleep(0.4)
        dbg.send("sh config set desktop.minimize_effect shatter")
        dbg.settle(1.2)
        mark_eff = len(drain(dbg))

        def opts_button():
            drain(dbg)
            for line in reversed(_log):
                m = re.search(r"settings: opts_button (-?\d+) (-?\d+) (-?\d+) "
                              r"(-?\d+) opts (\d+)", line)
                if m:
                    return [int(v) for v in m.groups()]
            return None

        ob = opts_button()
        check("effects: a Settings... button, because shatter declares options",
              ob is not None and ob[4] > 0 and ob[2] > 0, f"opts_button={ob}")

        # **ASKED BEFORE THE DIALOG EXISTS.** The app reports every slot
        # it holds, whichever surface drew it -- so once the dialog is
        # up its controls have real rects and "not on the page" cannot
        # be read from the report any more. On the page alone they are
        # unplaced, which is the thing worth asserting.
        placed = [n for n, c in controls(dbg, mark_eff).items()
                  if n.startswith("shatter.") and c["w"] > 0]
        check("effects: the options are not drawn on the page",
              not placed, f"placed on the page: {placed}")

        # **THE CONTROL PATH, as a person takes it.** The check above
        # arrives by `config set` from outside, which is the page
        # following a change made elsewhere. This one STAGES the choice
        # in the effect's own control -- a radio group, five choices
        # being under CHOICES_DROPDOWN_MIN -- scrolled into view first,
        # and aimed at through the app's reported rect and row count, in
        # the declared order (scale, genie, squash, glide, shatter).
        # Genie has no options and Shatter has two, so the button must
        # follow the STAGED choice both ways. Read only from lines
        # emitted after each click: an unchanged report is not an answer.
        def opts_since(mark):
            drain(dbg)
            hits = _since(mark, r"settings: opts_button (-?\d+) (-?\d+) (-?\d+) "
                                r"(-?\d+) opts (\d+)")
            return [int(v) for v in hits[-1].groups()] if hits else None

        def pick(row):
            mm = reveal("desktop.minimize_effect",
                        controls(dbg, 0).get("desktop.minimize_effect"))
            if not mm or mm["kind"] != "radio" or mm["rows"] < 5:
                return None, mm
            mark = len(drain(dbg))
            click(mm["x"] + 12, mm["y"] + (2 * row + 1) * mm["h"] // (2 * mm["rows"]))
            dbg.settle(1.0)
            return opts_since(mark), mm

        genie, mm = pick(1)
        back, _ = pick(4)
        check("effects: staging an effect with NO options takes the "
              "Settings... button away", genie is not None and genie[4] == 0,
              f"after Genie: {genie}; control {mm}")
        check("...and staging shatter again brings it back",
              back is not None and back[4] > 0, f"after Shatter: {back}")

        if ob and ob[4] > 0:
            # **SCROLL TO THE BOTTOM FIRST, THEN READ ONCE.** Scrolling
            # until the button is "inside the viewport" and clicking the
            # position read during that loop races the relayout the
            # scroll causes: the click landed elsewhere and the dialog
            # never opened, with nothing in the log because nothing had
            # been pressed. At the bottom the page cannot move again, so
            # the position read after it is the position clicked.
            geo_eff = layout(dbg)
            pv = geo_eff.get("page", (px0, py0, pw0, ph0))
            dbg.warp_cursor(qmp, cx + pv[0] + pv[2] // 2,
                            cy + pv[1] + pv[3] // 2)
            for _ in range(12):
                dbg.send("gui wheel -1")
            dbg.settle(1.0)
            time.sleep(0.5)
            ob = opts_button() or ob
            check("effects: the button is reachable on the page",
                  pv[1] <= ob[1] and ob[1] + ob[3] <= pv[1] + pv[3],
                  f"button {ob[:4]} against page {pv}")
            # WAIT FOR THE WINDOW, don't sleep at it -- and click again
            # once if it has not come. A fixed sleep passed twice and
            # failed the third time: the app relayouts after the scroll,
            # and a click landing in that window does nothing.
            dlg = None
            for attempt in range(2):
                click(ob[0] + ob[2] // 2, ob[1] + ob[3] // 2)
                deadline = time.time() + 6
                while time.time() < deadline:
                    dlg = dbg.window("Shatter options")
                    if dlg:
                        break
                    time.sleep(0.25)
                if dlg:
                    break
                dbg.settle(1.0)
                ob = opts_button() or ob
            check("effects: the button opens the effect's options window",
                  dlg is not None,
                  f"windows={[w['title'] for w in dbg.windows()]}")
            if dlg:
                # **AND IT IS STILL THERE A MOMENT LATER.** The dialog
                # opens CENTRED UNDER THE CURSOR, so an event reaching
                # Cancel dismisses it -- which it did, ten milliseconds
                # after appearing, on a machine where the pointer landed
                # there. "It opened" is satisfied either way; this is
                # the check that is not.
                dbg.settle(1.0)
                time.sleep(0.8)
                check("effects: the dialog STAYS open",
                      dbg.window("Shatter options") is not None,
                      "it closed on its own after opening")

                # NAMED, not `shatter options`: the descriptor's token is
                # the author's word and a title bar shows a name.
                # `owner` is a window INDEX and -1 is the sentinel
                # (wm_debug.c) -- 0 is a real owner, System Settings
                # itself, which an `owner != 0` test called no owner.
                check("effects: it is a MODAL dialog owned by the page",
                      bool(dlg.get("dialog")) and bool(dlg.get("modal")) and
                      dlg.get("owner", -1) >= 0,
                      f"dialog={dlg.get('dialog')} modal={dlg.get('modal')} "
                      f"owner={dlg.get('owner')}")

                before = dbg.send("sh cat /etc/effects/shatter.conf") or ""
                dc = dlg["content"]
                # Cancel writes NOTHING -- it and the X are one answer.
                dbg.send(f"gui click {dc['x'] + 92} {dc['y'] + dc['h'] - 22}")
                time.sleep(1.5)
                dbg.settle(1.0)
                after = dbg.send("sh cat /etc/effects/shatter.conf") or ""
                check("effects: Cancel closes it and writes nothing",
                      dbg.window("Shatter options") is None and after == before,
                      f"before={before.strip()!r} after={after.strip()!r}")

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
    open_row(mouse_row)
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
    open_row(tz_row)
    tz_slots = slots(dbg, mark)
    tz = [s for s in tz_slots if "timezone" in s["name"]]
    check("the timezone page loaded every city", tz and tz[-1]["choices"] >= 50,
          f"{tz[-1]['choices'] if tz else 0} choices")
    # A ONE-SETTING PAGE IS ONE CARD, and the card is titled even when
    # the page's name is the same word: a card is read on its own, as it
    # is in Windows 11's Settings.
    tz_page = page_line(dbg, mark)
    check("a one-setting page is one titled card",
          tz_page is not None and tz_page["slots"] == 1 and tz_page["captions"] == 1,
          f"{tz_page}" if tz_page else "no page line")

    check("...and a long list uses a DROPDOWN, not radio buttons",
          bool(tz) and tz[-1]["kind"] == "combo",
          f"kind={tz[-1]['kind'] if tz else '?'}")
    # NOT mouse_speed any more -- it became SETTING_TYPE_INT and has no
    # choices to be short. cursor_size is the short ENUM on this page,
    # and it is what this check was always about.
    # Three short names fit side by side: a segmented control.
    mouse_speed = [s for s in page_slots if "cursor_size" in s["name"]]
    check("...while a few short names sit side by side, segmented",
          bool(mouse_speed) and mouse_speed[-1]["kind"] == "segmented",
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
    open_row(mouse_row)
    mark_tz = len(drain(dbg))
    open_row(tz_row)
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
    # AND DISCARD WHAT THE KEYS STAGED: leaving a page with a change asks
    # now, and the next section navigates away.
    press("reset")
    time.sleep(0.4)
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
    open_row(mouse_row)
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
    speed_ctl = reveal("system.mouse_speed", speed_ctl)

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
    press("apply")
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
    # FROM THE TOP OF THE PAGE, because the sections above leave it part
    # scrolled and a wheel at the bottom moves nothing -- which reads as
    # an app that never re-reports, rather than as a page with nowhere
    # left to go.
    mark_scroll = len(drain(dbg))
    dbg.warp_cursor(qmp, cx + px0 + pw0 // 2, cy + py0 + ph0 // 2)
    for _ in range(20):
        dbg.send("gui wheel 1")
    dbg.settle()
    time.sleep(0.4)
    at_top = (controls(dbg, mark_scroll).get("system.mouse_accel")
              or ctls.get("system.mouse_accel"))
    accel_ctl = reveal("system.mouse_accel", at_top)
    qmp.screenshot(f"{args.tmp}/settings_slider.png")
    # WHOLLY INSIDE THE VIEWPORT, scrolled there if it was not: a press
    # outside the view is clipped away. (The Mouse page fits the default
    # window since the cards, so there may be nothing to scroll; the
    # Effects phase above is the one that has to.)
    check("the slider is inside the page, scrolled to if it was not",
          accel_ctl is not None and fully_inside(accel_ctl, py0, ph0),
          f"accel y {at_top and at_top['y']} -> {accel_ctl and accel_ctl['y']}")
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
        # Discarded, or leaving the page would stop at the question.
        press("reset")
        time.sleep(0.4)

    # --- Advanced= keeps a setting off the page until asked ----------
    #
    # system.cursor_style is marked Advanced=1 in /etc/settings.d, so it
    # must NOT be on its page by default and MUST appear once the toggle
    # is checked. Both halves matter: the first alone would pass if the
    # setting had simply vanished from the registry.
    # The Console page carries it (cursor_config.c's group). This looked
    # for "Console cursor" or "Appearance" until the sidebar grew
    # headings -- neither was a page, so the check was silently skipped.
    appearance = row_named("Console")
    check("the sidebar offers the Console page", appearance is not None)
    if appearance:
        mark = len(drain(dbg))
        open_row(appearance)
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
            # TEXT INK -- dark pixels -- not "anything but the background":
            # a page of white cards is all non-background and would
            # swamp a page of plain text lines.
            return sum(1 for p in crop.getdata() if sum(p) < 300)

        # BOTH PAGES CONFIRMED OPEN before their pixels are compared.
        # System Information is the LAST row of thirty-one and the
        # sidebar shows eighteen, so this is the check that reads the
        # wrong screen when a row is aimed at where it is not.
        mouse_page = select_page("Mouse")
        control_ink = page_ink("mouse") if mouse_page else None
        info_page = select_page("System Information") if mouse_page else None
        if info_page:
            info_ink = page_ink("sysinfo")
            check("the System Information page draws its text",
                  info_ink > control_ink // 4,
                  f"sysinfo={info_ink} px vs a settings page={control_ink} px")
        else:
            check("the System Information page draws its text", False,
                  "it, or the settings page it is measured against, never opened -- "
                  "the pixels would be some other page's")

    # --- A STRING SETTING IS EDITABLE, not a dead empty control -------
    #
    # Until CTRL_TEXT existed, SETTING_TYPE_STRING drew an EMPTY radio
    # list: a row that looks broken and can only be changed with `config
    # set`. The Network Time page is the fixture because it carries one
    # of each kind -- an enum, a string and an int -- so it also proves
    # a page can mix them.
    ntp_row = row_named("Network Time")
    if check("the sidebar offers a Network Time page", ntp_row is not None,
             f"labels={[r['label'] for r in rows]}"):
        # THE PAGE FIRST, THEN ITS CONTROLS. An empty control list means
        # two very different things -- a page with no controls, or a page
        # that never opened -- and only the page report tells them apart.
        mark = len(drain(dbg))
        ntp_page = select_page("Network Time")
        ntp_ctls = controls(dbg, mark) if ntp_page else {}
        server_ctl = ntp_ctls.get("system.ntp_server")
        # BY NAME, not by count: three controls from the page that was
        # already open satisfies a count just as well as the right page.
        want_ntp = ("system.ntp", "system.ntp_server", "system.ntp_interval")
        check("the page carries all three network-time settings",
              ntp_page is not None and all(n in ntp_ctls for n in want_ntp),
              f"page={ntp_page and ntp_page['page']!r} controls={sorted(ntp_ctls)}")
        if check("the app reported the server control's rect",
                 server_ctl is not None,
                 f"page={ntp_page and ntp_page['page']!r} controls={sorted(ntp_ctls)}"):
            # THE CHECK THIS PHASE EXISTS FOR. A string setting used to
            # report kind `radio` with zero rows -- which is exactly what
            # "an empty control" looks like in this log.
            check("a string setting gets a TEXT FIELD, not an empty radio",
                  server_ctl["kind"] == "text", f"kind={server_ctl['kind']}")
            check("...and the field has a real width to type into",
                  server_ctl["w"] > 40, f"w={server_ctl['w']}")

            before_server = stored_value(dbg, "ntp_server")
            # CLICK TO FOCUS, THEN TYPE. The click both places the caret
            # and moves the keyboard focus ring (uui_focus_click), which
            # is what makes the field the widget keys go to -- and it
            # lands past the end of the value, so the letter appends.
            #
            # dbg.key(), not QMP's send_text(): the debug console's key
            # injection is what every other keyboard check here uses,
            # and it is the one that reliably reaches the focused widget.
            click(server_ctl["x"] + server_ctl["w"] // 2,
                  server_ctl["y"] + server_ctl["h"] // 2)
            mark_key = len(drain(dbg))
            dbg.key(ord("z"))
            time.sleep(0.3)
            staged_server = staged_for(dbg, mark_key, "system.ntp_server")
            check("typing into the field stages a value",
                  staged_server is not None,
                  f"staged: {staged_server or 'nothing'}")
            if staged_server:
                # THE TEXT, not an index. Every other control on this
                # page stages a number, and a field that reported one
                # would write "1" as the server name.
                check("...and what it staged is the TEXT, not an index",
                      staged_server.endswith("z") and staged_server != before_server,
                      f"staged {staged_server!r}, was {before_server!r}")
            # The same load-bearing rule as every other control: staging
            # must not write.
            check("typing does NOT write it yet",
                  stored_value(dbg, "ntp_server") == before_server,
                  f"{before_server!r} -> {stored_value(dbg, 'ntp_server')!r}")

            press("apply")
            time.sleep(0.5)
            drain(dbg)
            after_server = stored_value(dbg, "ntp_server")
            check("Apply writes the typed value to /etc",
                  after_server is not None and after_server.endswith("z")
                  and after_server != before_server,
                  f"{before_server!r} -> {after_server!r}")

    # --- THE SIDEBAR SCROLLS, AND ITS SCROLLBAR CAN BE DRAGGED --------
    #
    # ASSERTED ON THE APP'S OWN `sidebar top`, not on pixels: a
    # screendump of a scrolled list would answer the same question with
    # the cursor sprite inside the crop, and the row dump is taken once
    # at the top so it cannot answer it at all.
    #
    # The three points are three separate refusals uui_sidebar_ops.hit
    # used to make -- it answered "which ITEM row", and uui_route.c
    # gates press AND wheel on that slot, so the wheel was dead over a
    # heading and over the bar, and the thumb could not be pressed.
    #
    # EVERY STEP WAITS ON THE REPORT, never on a sleep: the position is
    # reported on a CHANGE, so a read taken too early hands back the
    # value from before the input and reads exactly like input that was
    # never delivered.
    def to_top():
        """Back to row 0, CONFIRMED. Wheeled from an ITEM row, which is
        the one place the sidebar answered the wheel even when it was
        broken -- so the restore cannot itself depend on the fix."""
        sb = sidebar_state(dbg)
        if sb is None:
            return None
        if sb["top"] == 0:
            return sb
        ix, iy = item_point(sb)
        dbg.warp_cursor(qmp, cx + ix, cy + iy)
        dbg.wheel(len(rows))
        return wait_sidebar(dbg, lambda s: s["top"] == 0, timeout=4.0)

    to_top()
    sb = sidebar_state(dbg)
    if check("the sidebar reports where it is scrolled to", sb is not None,
             f"{sb}"):
        check("the sidebar is long enough to scroll",
              sb["rows"] > sb["visible"] > 3,
              f"{sb['rows']} rows, {sb['visible']} visible")
        max_top = sb["rows"] - sb["visible"]
        step = 3   # rows per wheel notch -- uui_sidebar_wheel()
        want = min(step, max_top)

        def wheels_from(where, rel_x, rel_y):
            name = f"the wheel scrolls the sidebar over {where}"
            if to_top() is None:
                check(name, False, f"could not get back to the top: {sidebar_state(dbg)}")
                return
            dbg.warp_cursor(qmp, cx + rel_x, cy + rel_y)
            dbg.wheel(-1)
            got = wait_sidebar(dbg, lambda s: s["top"] == want, timeout=3.0)
            check(name, got is not None,
                  f"top {sidebar_state(dbg) and sidebar_state(dbg)['top']}, wanted {want}")

        # Over an INERT row -- a heading. Not a row the sidebar can
        # select, and therefore not one it would scroll under either.
        head = next((r for r in rows[:sb["visible"]] if r["depth"] == 0), None)
        if head:
            wheels_from("a heading row", *row_point(head, 0))
        # Over the SCROLLBAR strip itself, where every desktop scrolls.
        wheels_from("the scrollbar", tx + tw - 2, ty + th // 2)

        # THE THUMB, dragged the length of the track. Landing at the END
        # is what a row-pitch drag would also fail, not just a dead one:
        # the thumb has to map the track's pixels onto the row range.
        #
        # drag_real(), never dbg.drag(): an injected drag is consumed one
        # position per WM iteration with the real mouse read in between,
        # so a ring-3 CLIENT is told the pointer left and never sees a
        # held motion at all (gui_debug.py's drag_real docstring).
        name = "dragging the thumb to the bottom of the track reaches the end"
        thumb = th // sb["visible"]
        if to_top() is None:
            check(name, False, f"could not get back to the top: {sidebar_state(dbg)}")
            check("dragging the thumb does not re-open the page", False,
                  "no drag was made")
        else:
            mark = len(drain(dbg))
            dbg.drag_real(qmp, cx + tx + tw - 2, cy + ty + thumb // 2,
                          cx + tx + tw - 2, cy + ty + th - 1)
            got = wait_sidebar(dbg, lambda s: s["top"] == max_top, timeout=4.0)
            check(name, got is not None,
                  f"top {sidebar_state(dbg) and sidebar_state(dbg)['top']}, wanted {max_top}")
            # AND THE DRAG MUST NOT NAVIGATE. uui_route.c reports the
            # sidebar's id on the release whatever the press was for, so
            # the app has to tell a scroll from a click -- a page
            # re-opened here would discard whatever the user had staged.
            opened = page_line(dbg, mark)
            check("dragging the thumb does not re-open the page",
                  opened is None,
                  f"the drag opened {opened and opened['page']!r}")
        # A CLICK ON THE TRACK PAGES, the other half of a scrollbar.
        name = "clicking the track below the thumb pages down"
        if to_top() is None:
            check(name, False, f"could not get back to the top: {sidebar_state(dbg)}")
        else:
            dbg.warp_cursor(qmp, cx + tx + tw - 2, cy + ty + th - thumb)
            qmp.click()
            got = wait_sidebar(dbg, lambda s: 0 < s["top"] <= max_top, timeout=3.0)
            check(name, got is not None,
                  f"top {sidebar_state(dbg) and sidebar_state(dbg)['top']}, wanted 1..{max_top}")
        to_top()

    # --- A SCREENSAVER'S OWN OPTIONS, ON THE PAGE THAT SELECTS IT -----
    #
    # These rows are NOT registry settings. They are declared by a data
    # file beside each saver (/usr/wm/savers/<name>.saver) and written to
    # a file of its own (/etc/savers/<name>.conf), because a saver's
    # options belong to a program that is not running and depend on which
    # saver is chosen -- see userland/lib/usaver.h. So everything below
    # is asking whether synthesised rows behave like real ones.
    #
    # WHAT A BROKEN VERSION WOULD STILL PASS is the question each check
    # is picked against: "there are some controls" is satisfied by the
    # two registry settings alone, so every check here names the SAVER
    # the rows claim to belong to.
    saver_row = row_named("Screensaver")
    if check("the sidebar offers a Screensaver page", saver_row is not None):
        mark_sv = len(drain(dbg))
        open_row(saver_row)
        sv_ctl = controls(dbg, mark_sv).get("desktop.screensaver")

        def option_slots(mark):
            """The page's rows that belong to a saver, not the registry.

            Told apart by the namespace: a registry setting is
            `desktop.screensaver`, an option is `<saver>.<key>`.
            """
            return [s for s in slots(dbg, mark)
                    if "." in s["name"] and not s["name"].startswith("desktop.")]

        def chosen(mark, fallback=None):
            return staged_for(dbg, mark, "desktop.screensaver") or fallback

        started_on = choice_shown(dbg, mark_sv, "desktop.screensaver")
        started_on = started_on[0] if started_on else None
        opts = option_slots(mark_sv)
        check("the page carries the chosen saver's own options",
              bool(opts) and started_on is not None
              and all(o["name"].startswith(started_on + ".") for o in opts),
              f"saver={started_on!r} rows={[o['name'] for o in opts]}")
        # THE DECLARATION DECIDES THE CONTROL, which is the half a row
        # count cannot see: `Option.stars=int:50..2000:420` has to become
        # a spinbox and `Option.colour=enum:...` a choice control, or the
        # descriptor's types are being ignored and every option is a
        # string box.
        kinds = {o["kind"] for o in opts}
        check("...and each option's control comes from its declared type",
              "spin" in kinds and kinds & {"radio", "segmented", "switch", "combo"},
              f"kinds={sorted(kinds)} from {[o['name'] for o in opts]}")

        def pick_saver(want=None, avoid=None, tries=14):
            """Open the saver dropdown and choose a row, by NAME where one
            is asked for. Returns (saver, option rows), or (None, []).

            IT RETURNS THE ROWS IT SAW, rather than leaving the caller to
            ask afterwards. The app's layout report is DEDUPED per frame,
            so the rebuilt page is described exactly once -- in the window
            this function already drained. A caller that nudged another
            frame and read again got an empty list and a check that
            failed with the feature working.

            IT SCANS RATHER THAN ASSUMING A ROW PITCH. The popup is a
            uui_listbox with no `describe`, so nothing reports its row
            height -- and a pitch worked out once from a font size is the
            constant this repo has had to re-measure three times. Walking
            down the popup until the APP says the wanted saver is staged
            asks the app instead of doing arithmetic, and needs no
            knowledge of the order the savers directory lists them in.
            """
            if sv_ctl is None:
                return None
            popup_top = cy + sv_ctl["y"] + sv_ctl["h"]
            for step in range(tries):
                mk = len(drain(dbg))
                dbg.send(f"gui click {cx + sv_ctl['x'] + sv_ctl['w'] // 2} "
                         f"{cy + sv_ctl['y'] + sv_ctl['h'] // 2}")
                dbg.settle()
                time.sleep(0.35)
                y = popup_top + 6 + step * 8
                dbg.send(f"gui click {cx + sv_ctl['x'] + 20} {y}")
                dbg.settle()
                time.sleep(0.45)
                # A SECOND FRAME, for the reason click() gives: the
                # control report comes from on_draw, which runs before
                # the rebuilt page has been laid out.
                dbg.send(f"gui move {cx + sv_ctl['x'] + 20} {y + 1}")
                dbg.settle()
                time.sleep(0.3)
                got = chosen(mk)
                if got is None:
                    continue
                if (want is not None and got == want) or \
                   (want is None and got != avoid):
                    return got, option_slots(mk)
            return None, []

        # BY NAME, and a name that HAS options: "any saver but this one"
        # picked `blank` -- which correctly shows none, so the swap check
        # below failed on the one saver that cannot demonstrate it.
        want_other = "plasma" if started_on == "matrix" else "matrix"
        picked, new_opts = pick_saver(want=want_other)
        if check(f"the saver dropdown can select {want_other}",
                 picked is not None, f"stayed on {started_on!r}"):
            # BOTH HALVES: the rows now shown belong to the saver just
            # chosen, AND none of the previous saver's rows survived. The
            # first alone passes on a page that merely appended.
            check("choosing a saver swaps its options in",
                  bool(new_opts)
                  and all(o["name"].startswith(picked + ".") for o in new_opts)
                  and not any(o["name"].startswith(str(started_on) + ".")
                              for o in new_opts),
                  f"{picked}: {[o['name'] for o in new_opts]}")

        # A SAVER MAY DECLARE NO OPTIONS, and `blank` ships without a
        # descriptor at all -- an empty page is a supported state here
        # rather than a failed read, and it is the case that proves the
        # rows come from the descriptor and not from the app.
        blank, blank_opts = pick_saver(want="blank")
        if blank == "blank":
            check("a saver with no descriptor contributes no options",
                  not blank_opts, f"{[o['name'] for o in blank_opts]}")
        else:
            check("the blank saver is selectable", False, "never staged")

        # --- STAGING AND WRITING AN OPTION ----------------------------
        #
        # Back to a saver that HAS options, then step one of its
        # spinboxes. The value is checked on DISK, read by `cat` through
        # the serial console -- a different process and a different code
        # path from the one that wrote it, which is the only way to tell
        # "the app thinks it saved" from "it saved" (the reasoning every
        # other check in this file uses for /etc/toyos.conf).
        target, target_opts = pick_saver(want="starfield")
        conf = f"/etc/savers/{target}.conf"
        spins = [o for o in target_opts if o["kind"] == "spin"]
        if check("starfield offers a numeric option to change", bool(spins),
                 f"target={target!r}"):
            name = spins[0]["name"]
            key = name.split(".", 1)[1]
            # FROM THE EARLIEST MARK, deliberately. controls() keeps the
            # LAST rect reported for each name, and the deduped report
            # for this page came and went inside pick_saver's window --
            # a fresh mark here would find nothing at all.
            ctl = reveal(name, controls(dbg, mark_sv).get(name))
            if check(f"{name} reported a rect", ctl is not None):
                mark_stage = len(drain(dbg))
                click(ctl["x"] + ctl["w"] - 7, ctl["y"] + ctl["h"] // 4)
                staged_opt = staged_for(dbg, mark_stage, name)
                check("stepping an option stages it",
                      staged_opt is not None, f"staged {staged_opt!r}")
                # NOTHING ON DISK YET. This app was instant-apply once,
                # and an option that quietly wrote would look identical
                # on screen and differ only here.
                check("...and writes nothing until Apply",
                      key + "=" not in dbg.send(f"sh cat {conf}"),
                      f"{conf} holds {key} already")
                press("apply")
                time.sleep(1.2)
                on_disk = None
                for line in dbg.send(f"sh cat {conf}").splitlines():
                    line = line.strip()
                    if line.startswith(key + "="):
                        on_disk = line.split("=", 1)[1].strip()
                check("Apply writes the option to the saver's own file",
                      on_disk is not None and on_disk == staged_opt,
                      f"{conf}: {key}={on_disk!r}, staged {staged_opt!r}")
            # PUT THE MACHINE BACK. A setting this tool applies is the
            # machine every later tool runs on -- an applied mouse_accel
            # once made two unrelated tools fail as "hover does nothing".
            # The saver's file goes too: it changes what the screen draws.
            dbg.send(f"sh rm {conf}")
        if started_on:
            dbg.send(f"sh config set desktop.screensaver {started_on}")
        dbg.settle()

    # --- Reset discards; leaving a changed page asks ------------------
    mark = len(drain(dbg))
    open_row(mouse_row)
    speed_ctl = reveal("system.mouse_speed",
                        controls(dbg, mark).get("system.mouse_speed", speed_ctl))
    on_disk = stored_value(dbg, "mouse_speed")

    def stage_down():
        """Step the speed DOWN -- a value different from what Apply left,
        since staging the stored value is no change at all. Asserted
        before anything is discarded: an unchanged disk is satisfied just
        as well by a click that never reached the control."""
        mk = len(drain(dbg))
        click(speed_ctl["x"] + speed_ctl["w"] - 7, speed_ctl["y"] + speed_ctl["h"] * 3 // 4)
        return staged_for(dbg, mk, "system.mouse_speed")

    staged_down = stage_down()
    check("the step down stages a change for Reset to discard",
          staged_down is not None and staged_down != on_disk,
          f"staged {staged_down!r}, on disk {on_disk!r}")
    mark = len(drain(dbg))
    press("reset")
    time.sleep(0.6)
    drain(dbg)
    check("Reset discards the staged change and writes nothing",
          bool(_since(mark, r"settings: reset$")) and
          stored_value(dbg, "mouse_speed") == on_disk,
          f"{on_disk!r} still on disk, staged was {staged_down!r}")

    # LEAVING A PAGE WITH A CHANGE ASKS, and Discard goes on without
    # writing. The question is asserted on the app's own line, the
    # answer on the disk and on which page then opened.
    speed_ctl = reveal("system.mouse_speed",
                        controls(dbg, 0).get("system.mouse_speed", speed_ctl))
    staged_again = stage_down()
    mark = len(drain(dbg))
    open_row(tz_row)
    asked = _since(mark, r"settings: ask leave page changes (\d+)")
    check("leaving a page with a change asks first",
          staged_again is not None and bool(asked) and asked[-1].group(1) == "1",
          f"staged {staged_again!r}, asked {[m.group(0) for m in asked]}")
    buttons = {}
    for m in _since(mark, r"settings: layout ask\.button (\d) (-?\d+) (-?\d+) (\d+) (\d+)"):
        buttons[int(m.group(1))] = tuple(int(v) for v in m.groups()[1:])
    if check("...with Apply, Discard and Cancel", len(buttons) == 3, f"{buttons}"):
        x, y, w, h = buttons[1]                       # Discard
        mark = len(drain(dbg))
        click(x + w // 2, y + h // 2)
        went = wait_page(dbg, mark, "Time zone")
        check("Discard goes on to the page asked for, writing nothing",
              went is not None and stored_value(dbg, "mouse_speed") == on_disk,
              f"page {went and went['page']!r}, on disk "
              f"{stored_value(dbg, 'mouse_speed')!r} (was {on_disk!r})")

    # --- THE SEARCH BOX FILTERS THE SIDEBAR -----------------------------
    #
    # By the words a person uses: "cursor" is in no page's NAME, only in
    # its settings' labels, so a filter that matched page names alone
    # would find nothing and fail here.
    sx, sy, sw, sh = layout(dbg)["search"]
    click(sx + sw // 2, sy + sh // 2)
    mark = len(drain(dbg))
    for ch in "cursor":
        dbg.key(ord(ch), settle=False)
    dbg.settle()
    time.sleep(0.6)
    drain(dbg)
    filt = _since(mark, r'settings: filter "([^"]*)" rows (\d+)')
    shown = [m.group(1) for m in _since(mark, r"settings: row \d+ id \d+ y -?\d+ depth 1 (.+)")]
    check("typing in the search box filters the sidebar by setting",
          bool(filt) and filt[-1].group(1) == "cursor"
          and 0 < int(filt[-1].group(2)) < len(rows) and "Mouse" in shown,
          f"filter {[m.group(0) for m in filt][-1:]} pages {shown[-8:]}")
    mark = len(drain(dbg))
    for _ in range(len("cursor")):
        dbg.key("0x08", settle=False)   # hex: a bare "8" is typed as the digit
    dbg.settle()
    time.sleep(0.6)
    drain(dbg)
    filt = _since(mark, r'settings: filter "([^"]*)" rows (\d+)')
    check("...and clearing it brings every page back",
          bool(filt) and filt[-1].group(1) == "" and int(filt[-1].group(2)) == len(rows),
          f"{[m.group(0) for m in filt][-1:]} against {len(rows)} rows")

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
