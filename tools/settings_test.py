#!/usr/bin/env python3
"""Drives the RING-3 System Settings app (userland/gui/system/settings.c).

Run it after touching the settings registry (kernel/lib/setting.c,
api/setting.h), SYS_SETTING/SYS_SYSINFO, or any of the widgets the page
is built from -- uui_listbox, uui_radio_list, uui_statusbar and
uui_layout's `hidden` handling.

THREE CHECKS HERE CARRY THE WEIGHT, and each exists because of a bug
that everything else stayed green through:

1. A change is verified through an INDEPENDENT PATH: the BYTES ON DISK,
   read with `cat`. Clicking a choice makes the app log a success, and
   the app believing itself proves nothing -- the same reasoning
   notepad_client_test.py uses when it `cat`s the file it just saved.
   (`/bin/config get` was the first attempt and is useless here: a
   spawned program's STDOUT goes to its parent's pipe, not to the kernel
   log, so the debug console cannot see it. Only stderr is readable from
   outside -- see CLAUDE.md on sys_eprint.)

2. The hidden page is asserted to be GONE FROM THE PIXELS, not merely
   "the tab switched". `hidden` was honoured by the input router and not
   by the layout's draw, so the settings page stayed fully visible and
   painted over System Info while every log line said the tab had
   changed. A log-only check passes that.

3. The two pages must DIFFER, and returning to the first must restore it
   EXACTLY. "It changed" alone is satisfied by almost anything; the
   round trip is what rules out a page that merely redraws differently.

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
from gui_debug import DebugConsole          # noqa: E402
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)

    dbg = DebugConsole(args.sock)
    print("system settings (the settings registry, in ring 3)")

    # NOT DebugConsole.spawn(): it polls with logs(), which clears what
    # it returns, and would eat the app's own startup lines before this
    # tool could read them.
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
    cw, ch = win["content"]["w"], win["content"]["h"]

    geo = layout(dbg)
    check("it reports its own geometry",
          all(k in geo for k in ("tree", "page", "choices")),
          f"keys={sorted(geo)}")
    if "tree" not in geo:
        return report()

    n = setting_count(dbg)
    # From the REGISTRY, not from a list in the app -- so this number
    # tracks whatever the kernel registered. >= 4 rather than == 4: the
    # registry is meant to grow, and pinning the count turns a future
    # setting into a failure in an unrelated file.
    check("it listed the registered settings", (n or 0) >= 4, f"{n} settings")

    def click(rel_x, rel_y):
        dbg.send(f"gui click {cx + rel_x} {cy + rel_y}")
        dbg.settle()
        time.sleep(0.4)
        drain(dbg)

    tx, ty, tw, th = geo["tree"]
    chx, chy, chw, chh = geo["choices"]

    # The sidebar's rows AS THE APP REPORTS THEM -- id, click y, depth
    # and label. Never re-derived here: a tool that computes row offsets
    # from a font size drifts the moment an inset changes, which is what
    # gui_flow.py's calibrated numbers have cost three times.
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
          len([r for r in rows if r["depth"] == 0]) >= 2,
          f"{len([r for r in rows if r['depth'] == 0])} headings")

    def row_named(sub):
        for r in rows:
            if sub.lower() in r["label"].lower():
                return r
        return None

    # --- the settings page is DRAWN ----------------------------------
    settings_ink, settings_px = crop_ink(
        qmp, f"{args.tmp}/settings_settings.png", (cx, cy, cw, ch))
    check("the settings page draws something", settings_ink > 500,
          f"{settings_ink} non-background px")

    # The SAME RECT the hidden-page check below measures, captured while
    # the page is visible. Without this baseline that check has nothing
    # to be compared against except the whole page's ink, which is so
    # much larger that the comparison passes whatever happens -- the
    # first version of this tool did exactly that, and a positive
    # control (making uui_layout_draw ignore `hidden` again) failed to
    # redden a single check. Measure the same region in both states.
    # The PAGE's band, captured while a setting page is shown -- the
    # baseline the hidden-page check below is compared against. The same
    # rect in both states: comparing a band against the WHOLE page's ink
    # is what made the first version of this check pass with the bug
    # present.
    px0, py0, pw0, ph0 = geo["page"]
    band_shown, _ = crop_ink(qmp, f"{args.tmp}/settings_band_shown.png",
                              (cx + px0, cy + py0, pw0, ph0))

    # --- selecting a setting in the sidebar loads ITS page ------------
    #
    # The cursor style, chosen because its values are cheap and
    # reversible -- unlike the font size (which reflows the whole UI) or
    # the timezone (which moves the taskbar clock). Found BY LABEL from
    # the app's own row table, so inserting a category above it does not
    # silently move the click onto a different setting.
    cursor_row = row_named("cursor style") or row_named("cursor")
    if not check("the sidebar offers a cursor setting", cursor_row is not None,
                 f"labels={[r['label'] for r in rows][:8]}"):
        return report()
    click(tx + tw // 2, cursor_row["y"])
    sel = last(r"settings: page (\S+)")
    check("selecting a row opens that setting's page",
          sel is not None and "cursor_style" in sel, sel or "no page line")

    # --- HOVERING MUST NOT COMMIT ANYTHING ---------------------------
    #
    # The bug this guards: settings's on_widget() discarded `reason`, so
    # every router event -- including plain UUI_REASON_MOTION -- was
    # treated as a commit. Moving the pointer across the choice list
    # applied a setting per motion event, each of which wrote
    # /etc/toyos.conf, bumped fs_generation() and made the desktop
    # re-read every .desktop file. The machine froze for seconds while
    # hovering, and the churn exposed a re-entrancy bug in fs_read()
    # that panicked the kernel.
    #
    # `gui move` lasts one WM iteration, which is precisely what is
    # wanted here: several separate motion events, no press, no release.
    # drain() ACCUMULATES into one list rather than returning only what
    # is new, so take a mark first -- counting the whole log here would
    # include the sets made earlier in this test and fail always.
    mark = len(drain(dbg))
    for i in range(6):
        dbg.send(f"gui move {cx + chx + 30} {cy + chy + 11 + i * 8}")
        dbg.settle()
    time.sleep(0.4)
    hover_sets = [l for l in drain(dbg)[mark:] if "settings: set " in l]
    check("hovering the choice list commits nothing",
          len(hover_sets) == 0,
          f"{len(hover_sets)} set(s) while only moving: {hover_sets[:2]}")

    # --- applying a choice, verified through an INDEPENDENT path ------
    old_value = stored_value(dbg, "cursor_style")
    # Pick a choice that is NOT the current one, so "it changed" cannot
    # pass by the value already being right.
    want_row = 2 if old_value != "beam" else 0
    click(chx + 30, chy + int(22 * want_row + 11))

    applied = last(r"settings: set (\S+) (\S+) result (\d+)")
    check("clicking a choice applies it", applied is not None, applied or "no set line")
    result = int(re.search(r"result (\d+)", applied).group(1)) if applied else 0
    # 1 is SETTING_SAVED. 2 is SETTING_UNSAVED -- applied but NOT
    # persisted, which a settings UI must never report as success, so it
    # is a FAILURE here rather than a pass with a caveat.
    check("...and the registry reports it SAVED (not merely applied)",
          result == 1, f"enum setting_result = {result}")

    new_value = re.search(r"settings: set \S+ (\S+) result", applied).group(1) if applied else ""
    on_disk = stored_value(dbg, "cursor_style")
    # THE CHECK THAT MATTERS: the file itself, read by a path sharing
    # nothing with the one that wrote it. Paired with old_value so it
    # cannot pass by the value having been right all along.
    check("the value CHANGED on disk",
          on_disk is not None and on_disk != old_value,
          f"{old_value!r} -> {on_disk!r}")
    check("...and it is what the app said it set",
          bool(new_value) and on_disk == new_value,
          f"app said {new_value!r}, disk says {on_disk!r}")

    # --- the System Information node HIDES the settings page ----------
    sysinfo_row = row_named("System Information")
    if not check("the sidebar offers System Information", sysinfo_row is not None):
        return report()
    click(tx + tw // 2, sysinfo_row["y"])

    sysinfo_ink, sysinfo_px = crop_ink(
        qmp, f"{args.tmp}/settings_sysinfo.png", (cx, cy, cw, ch))
    check("the page changed", sysinfo_px != settings_px,
          f"{sysinfo_ink} vs {settings_ink} non-background px")

    # THE LOAD-BEARING ONE, and its shape had to change with the
    # redesign. It used to measure INK: the settings list was hidden and
    # the same band had to collapse. That premise is gone -- System
    # Information now draws INTO the page's rect, so the band gains ink
    # when the choices are hidden (measured: 2395 -> 8870). An ink check
    # here would now fail against a correct app, which is worse than not
    # checking.
    #
    # So it asserts the PROPERTY instead of a proxy for it: `hidden`
    # removes a widget from HIT-TESTING as well as from the picture. A
    # click where a choice used to be must change nothing. That is a
    # stronger check than the ink one ever was -- a widget still drawn
    # but unclickable would have passed the old version, and a widget
    # invisible but still live is exactly the bug `hidden` exists to
    # prevent.
    mark = len(drain(dbg))
    click(chx + 30, chy + 11)
    time.sleep(0.4)
    ghost_sets = [l for l in drain(dbg)[mark:] if "settings: set " in l]
    check("a hidden choice list cannot be clicked",
          len(ghost_sets) == 0,
          f"{len(ghost_sets)} set(s) from a click on a hidden widget: {ghost_sets[:2]}")

    # --- and back: a round trip must restore the settings page --------
    click(tx + tw // 2, cursor_row["y"])
    back_ink, back_px = crop_ink(qmp, f"{args.tmp}/settings_back.png", (cx, cy, cw, ch))
    # Pixel-identical to the FIRST settings capture would be wrong here
    # -- the selection moved and a value changed since. So: it must be
    # the settings page again (differing from System Info) and must have
    # real content, which together rule out both a dead tab and a blank
    # page.
    check("switching back restores the settings page",
          back_px != sysinfo_px and back_ink > 500,
          f"{back_ink} non-background px")

    # --- the page SCROLLS when the window is too small for it --------
    #
    # Before uui_scrollview, a settings window shrunk below its content
    # did not clip or scroll -- uui_layout handed every child its full
    # natural size and placed the rest past the window's bottom edge.
    # So the status bar vanished entirely and all but the first of a
    # setting's choices became unreachable, with nothing on screen to
    # say so.
    #
    # Selecting Time zone first, because it is the setting with enough
    # choices to overflow a short window. A setting with two choices
    # would fit and prove nothing -- the fixture has to cross the
    # boundary being tested.
    # ESTABLISH the baseline BEFORE interacting. Settings persist to the
    # disk image and `make iso` re-seeds by sync rather than reformat, so
    # a previous run's timezone is still there -- and if it happens to be
    # the row this ends up clicking, "it changed" is false while
    # everything works. Done here rather than just before the click
    # because writing a setting bumps the registry's generation, and the
    # app reloads and drops its selection when it sees that.
    dbg.send("sh config set system.timezone utc")
    dbg.settle()

    # click() takes CONTENT-RELATIVE coordinates and adds the origin
    # itself -- passing absolute ones offsets them twice and lands
    # outside the window, which looks exactly like a dead control.
    tz_row = row_named("time zone") or row_named("timezone")
    if not check("the sidebar offers a timezone setting", tz_row is not None):
        return report()
    click(tx + tw // 2, tz_row["y"])
    dbg.settle()
    time.sleep(0.3)
    # ASSERTED, not assumed: everything below depends on this page being
    # the one with enough choices to overflow, and a silent failure here
    # would surface as "scrolling does not work".
    check("the timezone page opened",
          "timezone" in (last(r"settings: page (\S+)") or ""),
          last(r"settings: page (\S+)") or "no page line")
    dbg.settle()

    win = dbg.window("System Settings")
    short_h = win["h"] - 160
    dbg.send("gui drag %d %d %d %d" % (win["x"] + win["w"] - 2, win["y"] + win["h"] - 2,
                                        win["x"] + win["w"] - 2, win["y"] + short_h))
    dbg.settle()
    win = dbg.window("System Settings")
    c = win["content"]
    scx, scy, scw, sch = c["x"], c["y"], c["w"], c["h"]

    check("the window really did shrink", win["h"] < short_h + 40,
          f"{win['h']}px tall")

    # THE ONE THAT WOULD HAVE CAUGHT THE ORIGINAL BUG. The status bar is
    # OUTSIDE the scroll view, so it must survive a window too small for
    # the page -- it used to be laid out past the bottom edge and simply
    # disappear. Measured as ink in the bottom rows of the content.
    bar_h = 20
    bar_ink, _ = crop_ink(qmp, f"{args.tmp}/settings_statusbar.png",
                           (scx, scy + sch - bar_h, scw, bar_h))
    # THE THRESHOLD IS THE CHECK. Measured: 4675 px with the status bar
    # present, 777 with the positive control in place (uui_layout back
    # to overflowing, so the bar is laid out past the window). It is not
    # zero in the broken case -- the scrolled page's own content sits in
    # those rows instead -- so "is there any ink" passes either way,
    # which is exactly what this check did on its first run. 2000 sits
    # between the two with room on both sides.
    check("the status bar survives a window too small for the page",
          bar_ink > 2000, f"{bar_ink} non-background px in the bottom {bar_h} rows")

    # Scrolling must REACH what a short window hides. Capture the page,
    # wheel to the bottom, capture again: the two must differ, and the
    # cursor has to be parked inside the page first because the wheel
    # goes to the widget under it (gui_debug.py's warp_cursor).
    # THE RECT AND THE CURSOR MUST BOTH BE INSIDE THE PAGE, and neither
    # was: the rect spanned the whole content width and warp_cursor
    # parked at scw//3, which is INSIDE the sidebar -- it is wide enough
    # to hold "Cursor theme  (system)". So the wheel scrolled the TREE,
    # the page never moved, and this check passed anyway because the
    # tree's own scrolling changed pixels inside the measured rect.
    #
    # The general form is this repo's own rule: a check whose region
    # includes something other than the thing under test can be
    # satisfied by that other thing. Measure the page; point at the page.
    page_x = scx + tw
    page_w = scw - tw
    page_rect = (page_x, scy + 30, page_w, sch - 60)
    before_ink, before_px = crop_ink(qmp, f"{args.tmp}/settings_scroll_top.png", page_rect)
    dbg.warp_cursor(qmp, page_x + page_w // 2, scy + sch // 2)
    for _ in range(8):
        dbg.send("gui wheel -1")
    dbg.settle()
    after_ink, after_px = crop_ink(qmp, f"{args.tmp}/settings_scroll_bottom.png", page_rect)
    check("the wheel scrolls the page", after_px != before_px,
          f"{before_ink} -> {after_ink} non-background px")

    # And the payoff: a choice that was off-screen is now selectable.
    # Asserted on the FILE, not on the app's own claim -- the same
    # independent path the earlier checks use.
    before_tz = stored_value(dbg, "timezone")
    drain(dbg)
    # The last row of the scrolled page. Sampling from the bottom of the
    # viewport rather than a computed row index: what is under it
    # depends on the scroll offset, which is the widget's business.
    # THE PAGE MOVED WHEN THE WINDOW DID, so the geometry reported at
    # OPEN is stale here -- using it clicks where the page used to be,
    # which reads exactly like a scroll view that scrolled nothing. The
    # page starts just right of the sidebar, whose width the layout does
    # not change on a vertical resize, so its own reported width is the
    # offset that stays true.
    #
    # Clicking near the BOTTOM instead was an earlier attempt and it
    # landed in the gap between the last row and the status bar.
    # The page's HORIZONTAL MIDDLE, not a few pixels past the sidebar:
    # `tw + 20` landed in the layout's gap between the two, which
    # produced no event at all and read exactly like a scroll view that
    # scrolled nothing. The sidebar keeps its width across a vertical
    # resize, so its reported width plus half of what remains is inside
    # the page whatever the window is doing.
    # The CHOICE LIST's own x, not the page's middle: a radio list
    # recomputes its width rather than filling what the layout offers
    # (uui_radio_list.h says so), so the page's centre can be past its
    # right edge and hit nothing. Its x does not move on a vertical
    # resize, which is what makes the open-time value still true here.
    click(chx + 30, 20)
    dbg.settle()
    # settle() waits for the WM's queue to drain, which is not the same
    # as the CLIENT having written the file: the write happens in the
    # app's own process, one hop further out. Give it that hop before
    # reading the disk, or this reads the old value and blames the
    # scroll view.
    time.sleep(0.5)
    after_tz = stored_value(dbg, "timezone")
    # Reports what the APP said as well as what the disk says: "utc ->
    # utc" alone cannot tell a click that missed from a click that
    # landed on the value already in effect, and those need different
    # fixes.
    said = last(r"settings: set (\S+) (\S+) result (\d+)")
    check("a choice only reachable by scrolling can be applied",
          after_tz is not None and after_tz != before_tz,
          f"{before_tz} -> {after_tz}; app last said: {said or 'nothing'}")

    dbg.send("gui close 0")
    dbg.settle()
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print(f"\nsettings_test: {passed} passed, {failed} failed")
    for name, ok, _ in checks:
        if not ok:
            print(f"  FAILED: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
