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
# TASKBAR_H/ITEM_H/MENU_TOP_Y were previously calibrated for
# gfx_char_h()=18 (TASKBAR_H=32, ITEM_H=24) -- re-measured this session
# (pixel-scanned a live screenshot's border/divider rows, see the file
# picker QMP-testing session's notes) and found the ACTUAL running
# kernel's default font now measures gfx_char_h()=21, not 18: TASKBAR_H
# (WM_TITLEBAR_H = gfx_char_h()+8) = 29, ITEM_H (gfx_char_h()+6) = 27.
# Cause not fully tracked down -- no /etc/toyos.conf font_size override
# was present on the disk.img this was measured against, so this looks
# like the kernel's actual compiled-in default font metrics differ from
# what this file assumed, not a per-image runtime setting. If a future
# session finds the opposite (these constants read too big), re-measure
# the same way: open the menu, screenshot, pixel-scan for the top
# border row and the divider row between the app list and system
# actions (both draw in THEME_BORDER, a solid, distinctive color run
# unlike the surrounding text glyph rows) rather than trusting either
# this comment or the formula blindly.
TASKBAR_H = 29       # WM_TITLEBAR_H -- taskbar strip is the same height as a title bar
ITEM_H = 27           # Start menu row height (gfx_char_h() + 6 -- see above)
# Deriving the menu's top Y from SCREEN_H - TASKBAR_H - ITEM_H*total_items
# (start_menu.c's own geometry() formula) matched a live-measured
# screenshot exactly at these corrected constants (menu top border at
# y=502 for today's 7-row menu: 720-29-27*7=502) -- if APP_ORDER's
# length or ITEM_H ever changes again, re-measure directly rather than
# trusting the formula alone, same caution as above.
MENU_TOP_Y = 502
START_BTN = (50, 703)  # inside the taskbar's Start button, safely off any edge

# Keep in sync with apps/gui_apps.c's gui_app_registry[] order.
APP_ORDER = ["Notepad", "About", "Calculator", "Terminal", "Task Manager"]
# Keep in sync with apps/wm/start_menu.c's wm_system_actions[] order.
SYSTEM_ACTIONS = ["Exit to shell", "Shutdown"]


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
