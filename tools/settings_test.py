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
               r"rows (\d+) kind (radio|combo)")


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
                            "rows": int(m.group(7))}
    return out


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
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)

    dbg = DebugConsole(args.sock)
    print("system settings (the settings registry, in ring 3)")

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

    def row_named(sub):
        for r in rows:
            if sub.lower() in r["label"].lower():
                return r
        return None

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
    check("...and a long list uses a DROPDOWN, not radio buttons",
          bool(tz) and tz[-1]["kind"] == "combo",
          f"kind={tz[-1]['kind'] if tz else '?'}")
    mouse_speed = [s for s in page_slots if "mouse_speed" in s["name"]]
    check("...while a short list stays radio buttons",
          bool(mouse_speed) and mouse_speed[-1]["kind"] == "radio",
          f"kind={mouse_speed[-1]['kind'] if mouse_speed else '?'}")

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
    if not check("the baseline is on disk to begin with", before == "normal",
                 f"stored={before!r}"):
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

    # Pick a row that is NOT the current one, so "it changed" cannot pass
    # by the value already being right.
    row_h = speed_ctl["h"] // max(speed_ctl["rows"], 1)
    target_row = 0 if before != "slow" else 2
    click_y = speed_ctl["y"] + row_h * target_row + row_h // 2
    click(speed_ctl["x"] + 20, click_y)
    # ASSERTED ON A LOGGED FACT, not on the status bar's pixels: the
    # first version looked for the bar's wording in the log, where it
    # never appears, so it failed while the app was working perfectly.
    staged = last(r"settings: staged (\S+) (\S+)")
    check("clicking a choice reaches the control",
          staged is not None and "mouse_speed" in staged,
          f"rect={speed_ctl}, clicked ({speed_ctl['x'] + 20}, {click_y}), "
          f"staged: {staged or 'nothing'}")

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

    # --- Cancel closes without writing --------------------------------
    mark = len(drain(dbg))
    click(tx + tw // 2, mouse_row["y"])
    speed_ctl = controls(dbg, mark).get("system.mouse_speed", speed_ctl)
    row_h = speed_ctl["h"] // max(speed_ctl["rows"], 1)
    on_disk = stored_value(dbg, "mouse_speed")
    click(speed_ctl["x"] + 20, speed_ctl["y"] + row_h * (target_row + 1) + row_h // 2)
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
