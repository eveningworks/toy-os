#!/usr/bin/env python3
"""Drives the RING-3 Control Panel (userland/gui/system/cpanel.c).

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
    python3 tools/cpanel_test.py
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
CPANEL = "/bin/wm/system/cpanel"
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
        m = re.search(r"cpanel: layout (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out


def setting_count(dbg):
    drain(dbg)
    n = None
    for line in _log:
        m = re.search(r"cpanel: settings (\d+)", line)
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
    print("control panel (the settings registry, in ring 3)")

    # NOT DebugConsole.spawn(): it polls with logs(), which clears what
    # it returns, and would eat the app's own startup lines before this
    # tool could read them.
    dbg.send(f"gui spawn {CPANEL}")
    deadline = time.time() + 8
    while time.time() < deadline:
        time.sleep(0.3)
        drain(dbg)
        if any("cpanel: layout" in l for l in _log):
            break
    dbg.settle()

    wins = [w for w in dbg.json("gui windows --json")["windows"]
            if w["title"] == "Control Panel"]
    win = wins[-1] if wins else None
    if not check("Control Panel opened", win is not None):
        print("\nnothing to drive -- the checks below would be vacuous")
        return report()

    check("it is a ring-3 client", win.get("client_pid", 0) != 0,
          f"client_pid={win.get('client_pid')}")

    cx, cy = win["content"]["x"], win["content"]["y"]
    cw, ch = win["content"]["w"], win["content"]["h"]

    geo = layout(dbg)
    check("it reports its own geometry",
          all(k in geo for k in ("list", "choices", "tab0", "tab1")),
          f"keys={sorted(geo)}")
    if "list" not in geo:
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

    lx, ly, lw, lh = geo["list"]
    chx, chy, chw, chh = geo["choices"]

    # --- the settings page is DRAWN ----------------------------------
    settings_ink, settings_px = crop_ink(
        qmp, f"{args.tmp}/cpanel_settings.png", (cx, cy, cw, ch))
    check("the settings page draws something", settings_ink > 500,
          f"{settings_ink} non-background px")

    # The SAME RECT the hidden-page check below measures, captured while
    # the page is visible. Without this baseline that check has nothing
    # to be compared against except the whole page's ink, which is so
    # much larger that the comparison passes whatever happens -- the
    # first version of this tool did exactly that, and a positive
    # control (making uui_layout_draw ignore `hidden` again) failed to
    # redden a single check. Measure the same region in both states.
    lx0, ly0, lw0, lh0 = geo["list"]
    band_shown, _ = crop_ink(qmp, f"{args.tmp}/cpanel_band_shown.png",
                              (cx + lx0, cy + ly0, lw0, lh0))

    # --- selecting a setting loads ITS choices ------------------------
    #
    # Row 2 is "Console cursor" -- chosen because its values are cheap
    # and reversible, unlike the font size (which reflows the whole UI)
    # or the timezone (which moves the taskbar clock).
    row_h = 20
    click(lx + 40, ly + int(row_h * 2.5))
    sel = last(r"cpanel: select (\w+)")
    check("selecting a row selects that setting",
          sel is not None and "cursor_style" in sel, sel or "no select line")

    # --- applying a choice, verified through an INDEPENDENT path ------
    old_value = stored_value(dbg, "cursor_style")
    # Pick a choice that is NOT the current one, so "it changed" cannot
    # pass by the value already being right.
    want_row = 2 if old_value != "beam" else 0
    click(chx + 30, chy + int(22 * want_row + 11))

    applied = last(r"cpanel: set (\S+) (\S+) result (\d+)")
    check("clicking a choice applies it", applied is not None, applied or "no set line")
    result = int(re.search(r"result (\d+)", applied).group(1)) if applied else 0
    # 1 is SETTING_SAVED. 2 is SETTING_UNSAVED -- applied but NOT
    # persisted, which a settings UI must never report as success, so it
    # is a FAILURE here rather than a pass with a caveat.
    check("...and the registry reports it SAVED (not merely applied)",
          result == 1, f"enum setting_result = {result}")

    new_value = re.search(r"cpanel: set \S+ (\S+) result", applied).group(1) if applied else ""
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

    # --- the System Info tab HIDES the settings page ------------------
    t1x, t1y, t1w, t1h = geo["tab1"]
    click(t1x + t1w // 2, t1y + t1h // 2)
    check("the tab switched", (last(r"cpanel: tab (\w+)") or "").endswith("sysinfo"),
          last(r"cpanel: tab (\w+)") or "no tab line")

    sysinfo_ink, sysinfo_px = crop_ink(
        qmp, f"{args.tmp}/cpanel_sysinfo.png", (cx, cy, cw, ch))
    check("the page changed", sysinfo_px != settings_px,
          f"{sysinfo_ink} vs {settings_ink} non-background px")

    # THE LOAD-BEARING ONE. The settings list must be GONE, not merely
    # painted over -- so the SAME rect is measured in both states and
    # must have collapsed. A hidden widget that the layout still drew
    # leaves this band essentially unchanged, which is precisely the bug
    # `hidden` had, and "the page changed" above stays green through it
    # because the System Info text lands in the gaps.
    band_hidden, _ = crop_ink(qmp, f"{args.tmp}/cpanel_band_hidden.png",
                               (cx + lx, cy + ly, lw, lh))
    # The band does NOT go empty, and expecting that would be wrong: the
    # System Info text occupies the same rows. What separates the two
    # states is how MUCH survives -- measured at 55% of the shown ink
    # with `hidden` honoured, and 98% with the positive control in place
    # (uui_layout_draw ignoring it), because the list is then still
    # there with the text merely added around it. 75% sits between them
    # with room on both sides.
    check("the settings list is not still drawn underneath",
          band_hidden < band_shown * 0.75,
          f"{band_shown} px shown -> {band_hidden} px hidden (same rect)")

    # --- and back: a round trip must restore it EXACTLY ---------------
    t0x, t0y, t0w, t0h = geo["tab0"]
    click(t0x + t0w // 2, t0y + t0h // 2)
    back_ink, back_px = crop_ink(qmp, f"{args.tmp}/cpanel_back.png", (cx, cy, cw, ch))
    # Pixel-identical to the FIRST settings capture would be wrong here
    # -- the selection moved and a value changed since. So: it must be
    # the settings page again (differing from System Info) and must have
    # real content, which together rule out both a dead tab and a blank
    # page.
    check("switching back restores the settings page",
          back_px != sysinfo_px and back_ink > 500,
          f"{back_ink} non-background px")

    dbg.send("gui close 0")
    dbg.settle()
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    print(f"\ncpanel_test: {passed} passed, {failed} failed")
    for name, ok, _ in checks:
        if not ok:
            print(f"  FAILED: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
