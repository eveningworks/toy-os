#!/usr/bin/env python3
"""Refuse a GUI app that declares a key-taking widget and routes no keys.

WHY THIS EXISTS. Type-ahead was added to `uui_table` on 2026-08-27,
tested, and shipped doing nothing in Task Manager. The widget was
correct. `uapp.c` offers a key to `uapp_desc.focus` and then to
`uapp_desc.on_key`, and Task Manager declared NEITHER -- so no key had
ever reached `uui_table_key()`. Arrows, Home/End and paging had been
dead there since the app was written.

Nothing caught it because every check in `taskmgr_test.py` drives the
app by MOUSE, so the entire key handler could be absent and all twenty
checks passed. This is the sibling of tools/check_widget_ops.py: that
one catches an ops SLOT nobody filled, this one catches a filled slot
nobody can reach.

THE RULE. An app whose `struct uapp_desc` names a widget whose ops table
has a `.key` slot must declare `.focus` or `.on_key`. Those are the only
two doors in `uapp.c`; an app with neither is a closed building.

WHAT IT DELIBERATELY DOES NOT FLAG. A widget reached only through an
OPEN POPUP: `uui_router_overlay_key()` runs before both doors, so a
menu bar or an opened dropdown takes keys in an app with no routing at
all. That is why the key-capable set is derived from the `.key` slot in
`userland/ui/*.c` rather than listed here -- `uui_menubar_ops` has no
`.key` and correctly does not count.

WAIVE IN PLACE with a `key-routing-ok: <reason>` comment in the app,
the mechanism check_dispatch.py and check_widget_ops.py both use -- and
the reason is the MECHANISM, not the waiver.

Exit status is 0 when clean, 1 when an app cannot be typed at.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI_DIR = ROOT / "userland" / "ui"
GUI_DIR = ROOT / "userland" / "gui"

TABLE_RE = re.compile(
    r"const\s+struct\s+uui_widget_ops\s+(\w+)\s*=\s*\{(.*?)\n\};", re.S)
SLOT_RE = re.compile(r"\.(\w+)\s*=")
DESC_RE = re.compile(r"struct\s+uapp_desc\s+\w+\s*=\s*\{(.*?)\n\s*\};", re.S)
OPS_USE_RE = re.compile(r"&(uui_\w+_ops)\b")
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def strip_comments(src):
    """Comments are not code, and this check was blind to that.

    Its own positive control passed: commenting out `.on_key = on_key,`
    left the text in the file, the routing regex matched it, and the
    app read as routed. Anything asking "does this app do X" has to ask
    the code. Newlines are preserved so nothing below shifts lines.
    """
    return COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), src)


def key_capable_tables():
    """The ops tables that carry a `.key`, read from the widgets."""
    out = set()
    for path in sorted(UI_DIR.glob("*.c")):
        for m in TABLE_RE.finditer(strip_comments(path.read_text())):
            name, body = m.group(1), m.group(2)
            # A `_focus_ops` table exists to carry `key` FOR the ring, so
            # an app using one has a ring by construction.
            if name.endswith("_focus_ops"):
                continue
            if "key" in set(SLOT_RE.findall(body)):
                out.add(name)
    return out


def check_app(path, capable):
    raw = path.read_text()
    # The waiver is deliberately read from the RAW text -- it lives in a
    # comment. Everything else is read from the stripped source.
    if "key-routing-ok:" in raw:
        return []
    src = strip_comments(raw)
    if "uapp_desc" not in src:
        return []
    if not DESC_RE.search(src):
        return []

    # An app may build its desc in pieces, so ask the whole file whether
    # it ever names either door rather than only the literal.
    if re.search(r"\.focus\s*=", src) or re.search(r"\.on_key\s*=", src):
        return []

    used = sorted({t for t in OPS_USE_RE.findall(src) if t in capable})
    return [(path, used)] if used else []


def main():
    if not UI_DIR.is_dir() or not GUI_DIR.is_dir():
        print("check_key_routing: no userland/ui or userland/gui",
              file=sys.stderr)
        return 1

    capable = key_capable_tables()
    if not capable:
        print("check_key_routing: FAIL -- no ops table carries a .key slot;"
              " the pattern this reads must have changed", file=sys.stderr)
        return 1

    apps = sorted(GUI_DIR.rglob("*.c"))
    found = []
    for path in apps:
        found.extend(check_app(path, capable))

    if not found:
        print(f"check_key_routing: ok -- every app in {len(apps)} file(s) "
              f"that declares a key-taking widget routes keys to it "
              f"({len(capable)} key-capable ops tables)")
        return 0

    print(f"check_key_routing: FAIL -- {len(found)} app(s) cannot be "
          f"typed at\n")
    for path, used in found:
        rel = path.relative_to(ROOT)
        print(f"  {rel}: declares {', '.join(used)}")
        print("      but no .focus ring and no .on_key, so uapp.c has "
              "nowhere to send a key")
    print("\nRoute them. Forwarding from `on_key` to the one widget that")
    print("wants keys is what files.c and taskmgr.c do; a uapp_desc.focus")
    print("ring is the toolkit's idiom and adds a Tab stop that no widget")
    print("draws an indicator for yet. Or waive in that file with a")
    print("`key-routing-ok: <reason>` comment.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
