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
  4. A widget-array INDEX must not be derived from the array's own
     length (`(g_widget_count - 3)`) or from another such index. That
     shipped on 2026-08-30: appending one widget to the File Manager's
     array shifted three macros at once, so hiding the tree hid the tree
     SPLITTER and hiding the pane splitter hid the CONTEXT MENU -- a
     right-click that stopped working in single-pane view, and a tree
     drawn over the menu bar. It is the same "prefer facts that cannot
     go stale" rule CLAUDE.md states; look the widget up by its id.
  5. In a widget that DRAWS A SCROLLBAR, the `.hit` slot must not be the
     row hit converted with `>= 0`. `_hit()` there answers "which ROW",
     which excludes the bar column -- and uui_route.c gates press AND
     wheel on `.hit`, so routing on the row question leaves the
     scrollbar undraggable and the wheel dead over anything that is not
     a row. Five widgets shipped that conflation: uui_listbox,
     uui_table and uui_fileview each fixed it and left a comment,
     uui_sidebar and uui_tree still had it in 2026-09-03. Answer the
     WHOLE rect here (`uui_hit(x, y, w, h, ...)`) and keep `_hit()` for
     the row question. `>= 0` remains right for a widget with no bar.
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


HIT_SLOT_RE = re.compile(r"\.hit\s*=\s*(\w+)")


def row_hit_routed(src, body):
    """Is this table's `.hit` the row hit, converted with `>= 0`?

    That is the exact signature of the conflation rule 5 describes: a
    function whose whole answer is another `_hit()` tested against zero
    is answering "which row", and a widget with a scrollbar has a
    column that is not one.
    """
    m = HIT_SLOT_RE.search(body)
    if not m:
        return False
    fn = re.search(r"static\s+int\s+" + re.escape(m.group(1)) +
                   r"\s*\([^)]*\)\s*\{(.*?)\n\}", src, re.S)
    return bool(fn) and ">= 0" in fn.group(1)


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
                        (name, f"has no .{need}",
                         "a layout cannot place what it cannot measure"))
        if "set_geometry" in slots and "bounds" not in slots:
            problems.append(
                (name, "has no .bounds",
                 "uapp_log_layout() reports only what has one, so a widget "
                 "a layout can PLACE but not REPORT is invisible to every "
                 "test; eight shipped that way"))
        if "press" in slots and "release" not in slots:
            problems.append(
                (name, "has no .release",
                 "uui_route.c names a widget to its app only when it has one"))
        if "key" in slots and "accepts_focus" not in slots:
            problems.append(
                (name, "has no .accepts_focus",
                 "the focus ring skips a widget that refuses focus, and a "
                 "widget that takes keys must say whether it wants them"))
        if "uui_scrollbar_draw(" in src and row_hit_routed(src, body):
            problems.append(
                (name, "routes .hit through the ROW hit",
                 "`_hit(...) >= 0` excludes "
                 "the scrollbar column -- so uui_route.c refuses press and "
                 "wheel there and the bar cannot be dragged. Answer the "
                 "whole rect with uui_hit()"))
    if problems and waived:
        return []
    return [(path, *p) for p in problems]


# `#define NAME (<something> - <n>)` where <something> is a count or
# another index macro. A widget array's own length is the usual source;
# `WIDGET_TREE (WIDGET_TREE_SPLIT - 1)` is the chained form, which is
# just as fragile because its base is.
DERIVED_INDEX = re.compile(
    r"^\s*#define\s+(\w*WIDGET\w*|\w*_IDX\w*)\s*\(\s*"
    r"(\w*count\w*|\w*COUNT\w*|\w*WIDGET\w*|\w*_IDX\w*)\s*[-+]",
    re.IGNORECASE)


def check_derived_indices(root):
    """Widget-array indices derived from the array's LENGTH. See rule 4."""
    out = []
    for path in sorted(root.rglob("*.h")) + sorted(root.rglob("*.c")):
        try:
            text = path.read_text()
        except (OSError, UnicodeDecodeError):
            continue
        if "widget-ops-ok" in text:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            m = DERIVED_INDEX.match(line)
            if m:
                out.append((path, n, m.group(1), m.group(2)))
    return out


def main():
    if not UI_DIR.is_dir():
        print(f"check_widget_ops: no {UI_DIR}", file=sys.stderr)
        return 1
    found = []
    files = sorted(UI_DIR.glob("*.c"))
    for path in files:
        found.extend(check_file(path))

    derived = check_derived_indices(UI_DIR.parent)

    if not found and not derived:
        print(f"check_widget_ops: ok -- every ops table in {len(files)} "
              f"file(s) fills the slots it needs, and no widget index is "
              f"derived from an array length")
        return 0

    if derived:
        print(f"check_widget_ops: FAIL -- {len(derived)} widget index/indices "
              f"derived from a length\n")
        for path, line, name, base in derived:
            rel = path.relative_to(UI_DIR.parent.parent)
            print(f"  {rel}:{line}: {name} is derived from {base}")
            print("      Appending one widget to the array shifts it "
                  "silently.")
            print("      Look the widget up by its id instead, or waive it in")
            print("      this file with a `widget-ops-ok: <reason>` comment.")
        if not found:
            return 1
        print()

    print(f"check_widget_ops: FAIL -- {len(found)} bad slot(s)\n")
    for path, table, headline, why in found:
        rel = path.relative_to(UI_DIR.parent.parent)
        print(f"  {rel}: {table} {headline}")
        print(f"      {why}")
    print("\nFill or correct it from the function the widget already has,")
    print("or waive it in that file with a `widget-ops-ok: <reason>` comment.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
