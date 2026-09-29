#!/usr/bin/env python3
"""tools/gui_flow.py -- named, composable QMP click-flows on top of
qmp_test.py's QMPSession, so GUI-testing sessions stop hand-deriving
the same Start-menu/app-launch pixel math from scratch each time.

Every prior GUI-testing session has independently worked out "where is
the Start button, where does row N of the Start menu land, how far
apart are the rows" by trial and error against screenshots. This module
bakes that math in once, from the same geometry the kernel itself uses
(see userland/wm/start_menu.c's `geometry()`): item_h = char cell height +
6px, menu_y = (screen_h - taskbar_h) - item_h * total_items, rows
stacked top to bottom, apps first then system actions below a divider.

**These constants are now a fallback, not the source of truth.** The
kernel will tell you the real numbers: `gui menu --json` over the serial
debug console returns the menu rect, item height and every row's centre
exactly as the WM computes them, and `gui windows --json` does the same
for window rects. See tools/gui_debug.py -- DebugConsole.menu_row(label)
replaces all of the arithmetic below. Prefer that for new tests; this
module stays for flows that only have a QMP connection, and because a
hardcoded number that has been checked against the kernel is still
useful as a cross-check. (Re-verified 2026-08-14 after the default font
dropped from FONT_SIZE_18 to FONT_SIZE_14: the kernel reports
menu_y=410, item_h=22 and taskbar h=24, matching the values below.
Note how many of these numbers moved for a one-line font change --
that is the argument for asking the kernel rather than hardcoding.)

Known-good constants below (SCREEN_W/H, TASKBAR_H, ITEM_H) match the
project's fixed 1280x720 QEMU boot resolution and default font -- if a
future session changes the default font size or boot resolution, these
will need updating to match (there's no runtime query for them over
QMP, this mirrors the kernel's own math with numbers instead of live
gfx_char_w()/gfx_char_h() calls).

Menu rows are found BY LABEL, from the kernel's own geometry -- there is
no list of apps in this file to keep in step with /usr/wm/applications/. See
the note above `class GuiFlow` for the bug that came of having one.

Usage:
    import sys; sys.path.insert(0, "tools")
    from gui_flow import GuiFlow

    flow = GuiFlow(qmp_port=4444)
    flow.enter_gui()
    flow.open_app("Calculator")
    flow.session.screenshot("/tmp/calc.png")
    flow.open_start_menu()
    flow.open_app("Calculator")   # a 2nd instance, if that app is multi_instance
"""

import sys
import time

sys.path.insert(0, "tools")
from qmp_test import QMPSession  # noqa: E402

SCREEN_W = 1280
SCREEN_H = 720
# These track the DEFAULT FONT and have been re-measured three times
# now, once per change to it: gfx_char_h()=18 (TASKBAR_H 32, ITEM_H 24),
# then 21 (29, 27), and now 16 at the FONT_SIZE_14 default (24, 22).
# TASKBAR_H is TASKBAR_H_DEFAULT unless `desktop.taskbar_height` says
# otherwise; ITEM_H is gfx_char_h()+6.
#
# The lesson those three re-measurements teach is not "keep them
# updated" -- it is don't depend on them. `gui menu --json` /
# `gui taskbar --json` report the live numbers, and
# DebugConsole.menu_row(label) turns a label straight into a click
# point. If you do need to re-measure by hand: open the menu, screenshot,
# and pixel-scan for the top border row and the divider row between the
# app list and the system actions -- both draw in THEME_BORDER, a solid
# distinctive colour run, unlike the surrounding text glyph rows.
TASKBAR_H = 48       # desktop.taskbar_height's default (wm_taskbar.h's TASKBAR_H_DEFAULT)
ITEM_H = 22           # Start menu row height (gfx_char_h() + 6 -- see above)
START_BTN = (20, 706)  # inside the taskbar's Start button (icon-only by default), off any edge

# THERE IS NO MIRRORED LIST OF APPS HERE ANY MORE, and there was one
# until 2026-08-20. `APP_ORDER` copied what the WM builds from
# /usr/wm/applications/, and the Start menu's top edge was DERIVED from its
# length (the menu grows upward from the taskbar, so every app added
# moves it). "Crash Test" was added to the desktop entries and not to
# the list, which did two things at once: the index of every app after
# it was wrong, AND the computed origin was one row too low -- so
# open_app("System Settings") clicked Task Manager. Both symptoms, one
# cause, and neither failed loudly.
#
# Rows are asked for BY LABEL now, from the kernel's own geometry
# (`gui menu --json`, via DebugConsole.menu_row). A name cannot go
# stale the way an index can.

class GuiFlow:
    def __init__(self, qmp_port=4445, console=None, serial=None, **session_kwargs):
        """Menu rows are found by LABEL, which needs the debug console.

        **PASS YOUR OWN `console` IF YOU HAVE ONE.** Two DebugConsole
        objects are two connections to the SAME serial socket, and the
        guest's reply goes to whichever happens to be reading -- so a
        caller holding its own console while this opened a second saw
        queries answered with somebody else's output, or with nothing.
        That presented as "the Start menu did not open" on a menu that
        was demonstrably open, which is a long way from the cause.

        `serial` (a path) is the fallback for a caller that has no
        console of its own; it is opened lazily, so a flow that never
        touches the menu needs no socket at all.
        """
        self.session = QMPSession(port=qmp_port, **session_kwargs)
        self._serial = serial or ".vm.serial"
        self._dbg = console

    def _console(self):
        if self._dbg is None:
            from gui_debug import DebugConsole
            self._dbg = DebugConsole(self._serial)
        return self._dbg

    # -- entry / menu -----------------------------------------------------

    def enter_gui(self, settle=1.5):
        """Types 'gui' + Enter at the shell prompt and waits for wm_run()
        to take over. Call this once per fresh QEMU boot; for a script
        reusing an already-open GUI session from a previous process,
        skip this and call self.session.recalibrate() instead (see
        qmp_test.py's cursor-drift gotcha)."""
        self.session.send_text("gui")
        self.session.send_key("ret")
        time.sleep(settle)

    def open_start_menu(self, settle=0.4, timeout=4.0):
        """Clicks Start and WAITS UNTIL THE MENU EXISTS.

        Not a fixed sleep: `gui menu --json` answers with nothing at all
        while the menu is closed, so a caller that slept and then asked
        got an empty reply and a confusing parse error rather than "the
        menu did not open". Waiting on the artifact is this repo's own
        rule for exactly this shape.
        """
        # IDEMPOTENT, because the Start button TOGGLES. Clicking it when
        # the menu is already open closes it, and the poll below then
        # waits out its whole timeout on a menu the caller had already
        # opened -- which is how this presented: an "open" that closed.
        # `open`, NOT `rows`. `gui menu --json` reports the menu's
        # GEOMETRY whether it is showing or not -- a full row list with
        # "open": false -- so a rows-are-present test is true always,
        # and this returned without ever clicking. The one field that
        # answers the question is the one named after it.
        try:
            if self._console().menu().get("open"):
                self._menu_open_hint = True
                return
        except (ValueError, KeyError):
            pass
        # RECALIBRATE FIRST. QMP moves the mouse in RELATIVE steps, so a
        # process that did not itself put the pointer somewhere known has
        # no idea where it is -- and the click lands wherever it happens
        # to be. It worked within one script and failed in the next,
        # which is the signature of exactly this (see qmp_test.py's
        # cursor-drift note). Cheap, and it makes the helper safe to call
        # as the first thing a script does.
        self.session.recalibrate()
        self.session.click_at(*START_BTN)
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(0.2)
            try:
                if self._console().menu().get("open"):
                    self._menu_open_hint = True
                    time.sleep(settle)
                    return
            except (ValueError, KeyError):
                continue
        raise RuntimeError(
            "gui_flow: the Start menu did not open within "
            f"{timeout}s -- is a desktop running, and is the Start "
            "button where START_BTN says? `gui taskbar` reports its real "
            "position.")

    def click_menu_row(self, label, settle=0.5, _menu_already_open=False):
        """Clicks the Start menu row with this LABEL.

        The position comes from the kernel (`gui menu --json`), so this
        cannot drift when an app is added -- which is exactly what the
        row-index version did. A label that is not in the menu raises,
        rather than clicking whatever is at that height.
        """
        if not _menu_already_open and not getattr(self, "_menu_open_hint", False):
            self.open_start_menu()
        # menu_app_row(), not menu_row(): the menu shows ONE FOLDER at a
        # time, so an app in another folder has no geometry until its
        # folder is clicked. The helper does that and falls straight
        # through for a row already on screen (a folder, a system
        # action, an app in the open folder).
        x, y = self._console().menu_app_row(label)
        self.session.click_at(x, y)
        time.sleep(settle)
        self._menu_open_hint = False

    def open_app(self, name, settle=0.5):
        """Opens the Start menu and clicks the named app.

        For most purposes prefer `DebugConsole.open_app(name)`, which
        sends `gui open <name>` and lets the WM resolve it -- no menu, no
        pixels, nothing to drift. This exists for a test that wants the
        real menu EXERCISED rather than bypassed.
        """
        self.open_start_menu()
        self.click_menu_row(name, settle=settle, _menu_already_open=True)

    def run_system_action(self, label, settle=0.5):
        """`Exit to shell` / `Restart` / `Shutdown` -- the rows below the divider.
        Same mechanism as open_app(); they are menu rows like any other,
        and were only ever a separate list because the index arithmetic
        needed to know how many apps came first.
        """
        self.open_start_menu()
        self.click_menu_row(label, settle=settle, _menu_already_open=True)

    # -- misc ---------------------------------------------------------------

    def screenshot_named(self, name, subdir=None):
        """Screenshots to screenshots/<subdir or today's date>/<name>.png
        -- pass an explicit subdir (e.g. "2026-08-11") since this module
        can't call the real clock (matches the project's date-folder
        screenshot convention without hardcoding a date at import time)."""
        import os
        if subdir is None:
            raise ValueError("gui_flow: pass subdir explicitly (e.g. today's date) -- "
                              "this module doesn't read the clock itself")
        out_dir = os.path.join("screenshots", subdir)
        os.makedirs(out_dir, exist_ok=True)
        return self.session.screenshot(os.path.join(out_dir, f"{name}.png"))


if __name__ == "__main__":
    # Minimal self-check: connect, enter GUI, open the Start menu, screenshot.
    # Requires a QEMU instance already running with QMP on the given port
    # (see qmp_test.py's launch_qemu_cmd()).
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4445)
    ap.add_argument("--out", default="/tmp/gui_flow_selfcheck.png")
    args = ap.parse_args()

    flow = GuiFlow(qmp_port=args.port)
    flow.enter_gui()
    flow.open_start_menu()
    flow.session.screenshot(args.out)
    print(f"gui_flow self-check: screenshot written to {args.out}")
