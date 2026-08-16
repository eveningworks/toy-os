#!/usr/bin/env python3
"""tools/gui_flow.py -- named, composable QMP click-flows on top of
qmp_test.py's QMPSession, so GUI-testing sessions stop hand-deriving
the same Start-menu/app-launch pixel math from scratch each time.

Every prior GUI-testing session has independently worked out "where is
the Start button, where does row N of the Start menu land, how far
apart are the rows" by trial and error against screenshots. This module
bakes that math in once, from the same geometry the kernel itself uses
(see apps/wm/start_menu.c's `geometry()`): item_h = char cell height +
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

APP_ORDER must stay in sync with apps/gui_apps.c's `gui_app_registry[]`
order (Start menu shows apps in registry order, then SYSTEM_ACTIONS
below a 1px divider) -- if that registry changes, update this list to
match, same way a session already has to update any hardcoded
screenshot-comparison expectations.

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
# TASKBAR_H is WM_TITLEBAR_H = gfx_char_h()+8; ITEM_H is gfx_char_h()+6.
#
# The lesson those three re-measurements teach is not "keep them
# updated" -- it is don't depend on them. `gui menu --json` /
# `gui taskbar --json` report the live numbers, and
# DebugConsole.menu_row(label) turns a label straight into a click
# point. If you do need to re-measure by hand: open the menu, screenshot,
# and pixel-scan for the top border row and the divider row between the
# app list and the system actions -- both draw in THEME_BORDER, a solid
# distinctive colour run, unlike the surrounding text glyph rows.
TASKBAR_H = 24       # WM_TITLEBAR_H -- taskbar strip is the same height as a title bar
ITEM_H = 22           # Start menu row height (gfx_char_h() + 6 -- see above)
START_BTN = (50, 703)  # inside the taskbar's Start button, safely off any edge

# Keep in sync with apps/gui_apps.c's gui_app_registry[] order -- or
# don't, and ask the kernel instead: `gui apps` / `gui menu --json` list
# the registry live, in order (tools/gui_debug.py).
APP_ORDER = ["Task Manager", "Control Panel",
             # Launcher entries: these spawn a ring-3 program from /bin
             # rather than opening a kernel-space window, so open_app()
             # here returns before any window exists -- a caller that
             # wants the window must wait for the client to create it
             # (poll `gui windows`), not assume it is up on return.
             "About", "Shapes", "Calculator", "Notepad", "Terminal",
             "UI Demo"]
# Keep in sync with apps/wm/start_menu.c's wm_system_actions[] order.
SYSTEM_ACTIONS = ["Exit to shell", "Shutdown"]

# Deriving the menu's top Y from SCREEN_H - TASKBAR_H - ITEM_H*total_items
# (start_menu.c's own geometry() formula) matched a live-measured
# screenshot exactly at these corrected constants -- menu top border at
# y=475, back when the menu had 8 rows at the old 18pt default.
#
# It is COMPUTED from the lists above now rather than hardcoded, because
# the menu grows UPWARD from the taskbar: every app added to the registry
# moves this number, and a stale constant doesn't fail loudly -- it just
# clicks the wrong row. Adding the four ring-3 launchers moved it by 108
# pixels; dropping the default font to 14pt moved it again. Better
# still, don't rely on it at all: DebugConsole.menu_row
# (tools/gui_debug.py) asks the kernel where a row actually is, by label.
MENU_TOP_Y = SCREEN_H - TASKBAR_H - ITEM_H * (len(APP_ORDER) + len(SYSTEM_ACTIONS))


class GuiFlow:
    def __init__(self, qmp_port=4445, **session_kwargs):
        self.session = QMPSession(port=qmp_port, **session_kwargs)

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

    def open_start_menu(self, settle=0.4):
        self.session.click_at(*START_BTN)
        time.sleep(settle)
        self._menu_open_hint = True

    def _menu_row_center(self, row_index):
        return (80, MENU_TOP_Y + ITEM_H * row_index + ITEM_H // 2)

    def click_menu_row_by_index(self, row_index, settle=0.5, _menu_already_open=False):
        """0-based row index into APP_ORDER + SYSTEM_ACTIONS, top to
        bottom -- opens the Start menu first unless the caller already
        did (`_menu_already_open=True`, used by open_app()/
        run_system_action() so a single logical "open menu, click row"
        flow doesn't toggle the menu open-then-closed with two
        separate clicks on the Start button)."""
        if not _menu_already_open and not getattr(self, "_menu_open_hint", False):
            self.open_start_menu()
        x, y = self._menu_row_center(row_index)
        self.session.click_at(x, y)
        time.sleep(settle)
        self._menu_open_hint = False

    def open_app(self, name, settle=0.5):
        """Opens the Start menu and clicks the named app -- name must
        match APP_ORDER exactly (case-sensitive, matches the registry's
        `.name` string)."""
        if name not in APP_ORDER:
            raise ValueError(f"gui_flow: {name!r} not in APP_ORDER {APP_ORDER} -- "
                              "update APP_ORDER to match apps/gui_apps.c's registry")
        self.open_start_menu()
        self.click_menu_row_by_index(APP_ORDER.index(name), settle=settle, _menu_already_open=True)

    def run_system_action(self, label, settle=0.5):
        if label not in SYSTEM_ACTIONS:
            raise ValueError(f"gui_flow: {label!r} not in SYSTEM_ACTIONS {SYSTEM_ACTIONS}")
        self.open_start_menu()
        self.click_menu_row_by_index(len(APP_ORDER) + SYSTEM_ACTIONS.index(label),
                                      settle=settle, _menu_already_open=True)

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
