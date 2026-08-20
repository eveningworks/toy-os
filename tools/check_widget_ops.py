#!/usr/bin/env python3
"""Refuse a `struct uui_widget_ops` table with a slot it needs left NULL.

WHY THIS EXISTS. On 2026-08-19 four widgets shipped with short tables --
uui_dropdown, uui_checkbox, uui_textview and uui_textbox -- every
function they needed already written and only the table missing an
entry. Each failed SILENTLY AND AT A DISTANCE:

  * no `natural_size`/`set_geometry` -> the widget is never positioned or
    measured by a uui_layout. It sits at a zero rect, drawing nothing and
    hit-testing nothing, and the container looks like the broken thing.
    One of these presented as "uui_layout stops after four children" and
    cost most of a session; uui_layout_run() has no early exit at all.
  * no `release` -> uui_route.c NEVER NAMES THE WIDGET TO ITS APP, which
    reports a widget only when it has a release op. The control works
    perfectly on screen and the app hears nothing.

None of it was noticed because no app had put those widgets in a routed
layout yet. That is the shape this checks for: an ops slot nobody fills
is a capability nobody has tested, and the failure surfaces in a
different file from its cause.

THE RULES, and each names the failure it prevents:

  1. A table with `draw` must have `natural_size` AND `set_geometry`.
     Anything drawable can end up in a layout, and a layout cannot place
     what it cannot measure.
  2. A table with `press` must have `release`. Otherwise the app is
     never told the widget was used.
  3. A table with `key` must have `accepts_focus`. The focus ring SKIPS
     a widget that refuses focus (uui_focus.c), so a widget that takes
     keys but never says whether it wants them is relying on the
     default -- which is "yes" today and is not a statement. The
     inverse also holds and is not checkable here: `accepts_focus`
     without `key` is a tab stop that does nothing, which is exactly
     what uui_checkbox and uui_radio_list were before they gained one.

WAIVE IN PLACE with a `widget-ops-ok: <reason>` comment in the same file,
the same mechanism tools/check_dispatch.py uses -- and note the reason is
the MECHANISM, not the waiver. A `*_focus_ops` table is exempt by name:
it exists to carry `key` for the focus ring and is never a layout child.

Exit status is 0 when clean, 1 when something is short.
"""

import re
import sys
from pathlib import Path

UI_DIR = Path(__file__).resolve().parent.parent / "userland" / "ui"

TABLE_RE = re.compile(
    r"const\s+struct\s+uui_widget_ops\s+(\w+)\s*=\s*\{(.*?)\n\};", re.S)
SLOT_RE = re.compile(r"\.(\w+)\s*=")


def check_file(path):
    src = path.read_text()
    waived = "widget-ops-ok:" in src
    problems = []
    for m in TABLE_RE.finditer(src):
        name, body = m.group(1), m.group(2)
        # The focus ring's table is not a layout child -- see the module
        # docstring.
        if name.endswith("_focus_ops"):
            continue
        slots = set(SLOT_RE.findall(body))
        if "draw" in slots:
            for need in ("natural_size", "set_geometry"):
                if need not in slots:
                    problems.append(
                        (name, need,
                         "a layout cannot place what it cannot measure"))
        if "press" in slots and "release" not in slots:
            problems.append(
                (name, "release",
                 "uui_route.c names a widget to its app only when it has one"))
        if "key" in slots and "accepts_focus" not in slots:
            problems.append(
                (name, "accepts_focus",
                 "the focus ring skips a widget that refuses focus, and a "
                 "widget that takes keys must say whether it wants them"))
    if problems and waived:
        return []
    return [(path, *p) for p in problems]


def main():
    if not UI_DIR.is_dir():
        print(f"check_widget_ops: no {UI_DIR}", file=sys.stderr)
        return 1
    found = []
    files = sorted(UI_DIR.glob("*.c"))
    for path in files:
        found.extend(check_file(path))

    if not found:
        print(f"check_widget_ops: ok -- every ops table in {len(files)} "
              f"file(s) fills the slots it needs")
        return 0

    print(f"check_widget_ops: FAIL -- {len(found)} missing slot(s)\n")
    for path, table, slot, why in found:
        rel = path.relative_to(UI_DIR.parent.parent)
        print(f"  {rel}: {table} has no .{slot}")
        print(f"      {why}")
    print("\nFill it from the function the widget already has, or waive it")
    print("in that file with a `widget-ops-ok: <reason>` comment.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
