#!/usr/bin/env python3
"""System Settings' Date & time page: setting the clock by hand, and its lock.

WHAT IS UNDER TEST
------------------
`userland/settings/set_clock.c`: the Change... dialog that steps the
clock (SYS_SETTIME), its refusal while network time is on, `/bin/time -s`
(the same action from the shell), and the Region & formats preview.

WHAT A BROKEN VERSION WOULD STILL PASS, AND WHY THIS DOES NOT
-------------------------------------------------------------
- "The dialog opened and Set closed it" passes with a Set that never
  calls the kernel. So the clock is read through an INDEPENDENT path --
  `config get clock.utc`, the kernel's own fact -- and must have moved by
  the hour the dialog was told to add, not merely have changed.
- A dialog that set the right fields as UTC would be an hour-or-three out
  on a non-UTC zone and pass on UTC. The test selects Helsinki (UTC+3 on
  any date it is likely to run in summer, +2 in winter) and checks the
  STEP, which is zone-independent, and then that `time -s` with the
  original LOCAL time puts UTC back -- which only holds if the local ->
  UTC conversion is right.
- "Change... is disabled under NTP" is a flag; the check clicks it and
  requires that NO dialog appears.

It touches `system.ntp`, `system.ntp_server` (pointed at 127.0.0.1, so
turning NTP on sends nothing off this machine), `system.timezone` and the
clock, and restores all four.

    python3 tools/vm.py start
    python3 tools/clock_settings_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DIALOG = "Change date and time"

_res = Results()
check = _res.check


def sh(dbg, cmd):
    return (dbg.send(f"sh {cmd}") or "").strip()


def last_line(text):
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    return lines[-1] if lines else ""


def utc(dbg):
    m = re.search(r"^(\d{6,})\s*$", sh(dbg, "config get clock.utc"), re.M)
    return int(m.group(1)) if m else 0


def titles(dbg):
    return [w["title"] for w in dbg.windows()]


def wait_title(dbg, title, present=True, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (title in titles(dbg)) == present:
            return True
        time.sleep(0.2)
    return False


def close_named(dbg, title):
    for i, w in enumerate(dbg.windows()):
        if w["title"] == title:
            dbg.send(f"gui close {i}")
            dbg.settle()
            return


def stepper(widget, up):
    """The screen point of a spinbox's up or down arrow (ui/uui_spinbox.c:
    a 14px column at the right edge, split at half height)."""
    s = widget["screen"]
    return s["x"] + widget["w"] - 7, s["y"] + (widget["h"] // 4 if up else widget["h"] * 3 // 4)


def dialog_widgets(dbg, timeout=4.0):
    """The dialog's named controls, as `widgets()` shapes them. A uapp
    DIALOG publishes no widget map, only its layout log
    (`settings.clock: layout <name> x y w h`, content-relative, behind
    desktop.layout_log -- which enter_gui() turns on), so the screen
    rect is that plus the window's content origin."""
    deadline = time.time() + timeout
    found = {}
    while time.time() < deadline and "clock_set" not in found:
        for ln in dbg.logs("settings.clock: layout", clear=True):
            m = re.search(r"layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)", ln)
            if m:
                found[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
        time.sleep(0.2)
    win = next((w for w in dbg.windows() if w["title"] == DIALOG), None)
    if not win:
        return {}
    ox, oy = win["content"]["x"], win["content"]["y"]
    return {n: {"w": r[2], "h": r[3], "screen": {"x": ox + r[0], "y": oy + r[1]}}
            for n, r in found.items()}


def centre(widget):
    s = widget["screen"]
    return s["x"] + widget["w"] // 2, s["y"] + widget["h"] // 2


def open_settings_on_date_page(dbg):
    close_named(dbg, "System Settings")
    dbg.logs("settings: page", clear=True)
    dbg.open_app("System Settings")
    dbg.settle()
    deadline = time.time() + 8
    page = []
    while time.time() < deadline and not any("Date & time" in p for p in page):
        time.sleep(0.3)
        page += dbg.logs("settings: page", clear=True)
    return any("Date & time" in p for p in page)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "clock_settings_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("Date & time: Change..., time -s, and the NTP lock")

    saved = {k: last_line(sh(dbg, f"config get {k}"))
             for k in ("system.ntp", "system.ntp_server", "system.timezone",
                       "desktop.clock_seconds")}
    sh(dbg, "config set system.ntp off")
    sh(dbg, "config set system.timezone helsinki")
    dbg.settle(); time.sleep(0.5)
    start_utc = utc(dbg)
    try:
        run(dbg, start_utc)
        clock_seconds(dbg, qmp)
    finally:
        for k, v in saved.items():
            if v:
                sh(dbg, f"config set {k} {v}")
        close_named(dbg, DIALOG)
        close_named(dbg, "System Settings")
    return report()


def run(dbg, start_utc):
    check("the kernel reports a clock", start_utc > 1_000_000_000, str(start_utc))

    # --- 1. the page, its clock and its button --------------------------
    if not check("System Settings opens on Date & time", open_settings_on_date_page(dbg)):
        return
    w = dbg.widgets("System Settings")
    check("the page shows the live clock", "clock_view" in w, f"{sorted(w)}")
    if not check("...and a Change... button", "clock_change" in w, f"{sorted(w)}"):
        return

    # --- 2. the dialog steps the clock by what it was told --------------
    dbg.click(*dbg.widget_center("clock_change", "System Settings"))
    if not check("Change... opens the dialog", wait_title(dbg, DIALOG)):
        return
    d = dialog_widgets(dbg)
    if not check("the dialog reports its fields",
                 all(n in d for n in ("clock_hour", "clock_set", "clock_day")), f"{sorted(d)}"):
        return
    # +1 hour -- or -1 at 23 o'clock, where the spinbox is at its top.
    # Either way a STEP of exactly an hour, which the kernel then shows.
    off = offset_seconds(dbg)
    before = utc(dbg)
    up = ((before + off) % 86400) // 3600 != 23
    want = 3600 if up else -3600
    dbg.logs("settings: clock", clear=True)
    dbg.click(*stepper(d["clock_hour"], up=up))
    dbg.settle()
    dbg.click(*centre(d["clock_set"]))
    dbg.settle(); time.sleep(0.5)
    set_lines = dbg.logs("settings: clock set", clear=True)
    after = utc(dbg)
    check("Set reported success", any(ln.rstrip().endswith("result 0") for ln in set_lines),
          f"{set_lines[-1:]}")
    check("the dialog closed", wait_title(dbg, DIALOG, present=False))
    check("the KERNEL's clock moved by the hour the dialog was told",
          abs((after - before) - want) <= 30, f"moved {after - before}s, wanted {want}")

    # --- 3. time -s puts it back, through the local -> UTC conversion ---
    # The original moment as Helsinki LOCAL time, with the offset the
    # guest itself reports -- `time` prints local and clock.utc is UTC.
    restore_utc = utc(dbg) - want
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(restore_utc + off))
    sh(dbg, f'time -s "{stamp}"')
    back = utc(dbg)
    check("`time -s` with the original LOCAL time restores UTC",
          abs(back - restore_utc) <= 30, f"{back} vs {restore_utc} ({stamp})")
    bad = sh(dbg, 'time -s "2026-02-30 10:00"')
    check("`time -s` refuses 30 February", "not a date" in bad, last_line(bad))

    # --- 4. network time locks both ways out ----------------------------
    sh(dbg, "config set system.ntp_server 127.0.0.1")   # nothing leaves the machine
    sh(dbg, "config set system.ntp on")
    dbg.settle(); time.sleep(1.0)
    refused = sh(dbg, 'time -s "2026-10-01 12:00"')
    check("`time -s` is refused while network time is on", "network time is on" in refused,
          last_line(refused))
    if open_settings_on_date_page(dbg):
        dbg.click(*dbg.widget_center("clock_change", "System Settings"))
        time.sleep(1.0)
        check("...and Change... opens nothing", DIALOG not in titles(dbg), f"{titles(dbg)}")
    sh(dbg, "config set system.ntp off")


def clock_changes(dbg, qmp, window_s):
    """Whether the taskbar clock's pixels change within `window_s`. The
    tray's other items are static, so its box from `tray_x` rightwards is
    the clock as far as a change is concerned."""
    from PIL import Image
    tb = dbg.taskbar()
    sw = dbg.state()["screen"]["w"]
    box = (tb["tray_x"], tb["y"], sw, tb["y"] + tb["h"])
    path = "/tmp/clock_seconds_%d.png"
    qmp.screenshot(path % 0, stable=False)
    first = Image.open(path % 0).convert("RGB").crop(box).tobytes()
    end = time.time() + window_s
    while time.time() < end:
        time.sleep(0.25)
        qmp.screenshot(path % 1, stable=False)
        if Image.open(path % 1).convert("RGB").crop(box).tobytes() != first:
            return True
    return False


def clock_seconds(dbg, qmp):
    """desktop.clock_seconds: on, the clock changes every second; off, it
    holds still for longer than a second. A minute rolling over inside the
    'off' window changes it too, so that half is tried twice."""
    sh(dbg, "config set desktop.clock_seconds on")
    dbg.settle(); time.sleep(1.2)
    check("with seconds ON the taskbar clock changes within a second",
          clock_changes(dbg, qmp, 1.6))
    sh(dbg, "config set desktop.clock_seconds off")
    dbg.settle(); time.sleep(1.2)
    still = not clock_changes(dbg, qmp, 2.5) or not clock_changes(dbg, qmp, 2.5)
    check("with seconds OFF it holds still for longer than a second", still)


def offset_seconds(dbg):
    """Helsinki's offset now, from the guest's own two clocks."""
    out = last_line(sh(dbg, "time"))
    u = utc(dbg)
    m = re.search(r"(\d+)[.:](\d{2})[.:](\d{2})(?:\s*(AM|PM))?", out)
    if not m:
        return 0
    h, mi, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if m.group(4):
        h = h % 12 + (12 if m.group(4) == "PM" else 0)
    local_sod = h * 3600 + mi * 60 + s
    diff = (local_sod - u % 86400) % 86400
    return diff - 86400 if diff > 43200 else diff


def report():
    rows = _res.rows
    passed = sum(1 for _, ok, _ in rows if ok)
    print(f"\nclock_settings_test: {passed} passed, {len(rows) - passed} failed")
    return 0 if rows and passed == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
