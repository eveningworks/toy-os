#!/usr/bin/env python3
"""tools/start_menu_test.py -- the Start menu's folders, search and keyboard.

Drives the real menu through the debug console (`gui key`, `gui click`,
`gui menu --json`) and reads pixels for the parts JSON cannot answer.
What it proves, and what a broken version would still pass:

  * The menu is TWO COLUMNS: folder rows on the left, app rows starting
    to the right of them, and one search row across the foot. A menu
    that reported folders but drew the old single column passes a
    "there are category rows" check and fails the x-ordering one.
  * Clicking a folder shows EXACTLY that folder's apps -- compared
    against `gui menu --json`'s own app-to-folder list, so a pane that
    ignored the selection (or showed every app regardless) fails. The
    selection is asserted separately from the contents, because a
    highlight that moves while the list does not is the likelier bug.
  * TYPING FILTERS, with no click into the field first: the query the WM
    reports is what was typed, every row left matches it, and a name in
    another folder is found. A menu that took keys but never filtered
    would pass the query check alone, so the row contents are asserted
    too.
  * Enter LAUNCHES the highlighted result -- a real window appears. The
    weaker "the menu closed" version of this passes when Enter merely
    dismisses.
  * Esc CLEARS the query first and closes only when there is none, which
    is one level at a time; a menu that closed on the first Esc fails
    the first half.
  * Down/Up move the highlighted row and Right moves the folder.
  * PIXELS: the search field is DRAWN -- typing changes the pixels
    inside the field's own rect, while a sidebar action row (which
    nothing in that gesture touches) stays byte-identical. "It responds"
    is not "it is drawn", and the neighbour is half the assertion.

Positive controls, each run once when this was written (2026-09-17):
  * start_menu_key() returning 0 for printable characters reddens every
    search check and the Enter-launches check, and nothing else.
  * pane_row() ignoring the selected folder (always listing every app)
    reddens "shows exactly that folder's apps" while leaving the
    selection check green -- which is the pair of checks that
    distinguishes the two failures.

Usage (the VM must already be up):
    python3 tools/gui_regress.py --only start_menu_test
    python3 tools/start_menu_test.py --instance 0
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import port_guard  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
from gui_debug import DebugConsole, enter_gui  # noqa: E402

KEY_SUPER = "0xA6"
KEY_ESC = "0x1b"
KEY_DOWN = "0x92"
KEY_UP = "0x91"
KEY_RIGHT = "0x96"
KEY_ENTER = "0x0a"
KEY_F4 = "0xA5"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
        return bool(ok)


def shot(qmp, tmp, name):
    """A SETTLED frame -- a client that has drawn is not one composited."""
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, name))
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def rows_of(menu, kind):
    return [r for r in menu.get("rows", []) if r["kind"] == kind]


def open_menu(dbg, want=True, tries=6):
    """Toggle with Super until the WM says the menu is in the state asked.

    The Super GESTURE IS ON THE RELEASE (wm.c) -- pressing it only arms
    -- so both edges are sent. Toggling means a run that inherited an
    open menu would otherwise close the one it meant to open.
    """
    for _ in range(tries):
        if bool(dbg.menu().get("open")) == want:
            return True
        dbg.key(KEY_SUPER)
        dbg.key(KEY_SUPER, mods="up")
        time.sleep(0.3)
    return bool(dbg.menu().get("open")) == want


def type_text(dbg, text):
    for ch in text:
        dbg.key(ch)


def crop(im, r):
    return im.crop((r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"])).tobytes()


def run(dbg, qmp, tmp, res):
    # --- 1. the shape -------------------------------------------------
    if not res.check("Super opens the Start menu", open_menu(dbg),
                     "the menu never reported itself open"):
        return
    menu = dbg.menu()
    cats = rows_of(menu, "category")
    apps = rows_of(menu, "app")
    search = rows_of(menu, "search")
    res.check("the sidebar lists folders", len(cats) >= 2,
              f"{[c['label'] for c in cats]}")
    res.check("there is exactly one search row", len(search) == 1)
    side_right = max((c["x"] + c["w"]) for c in cats) if cats else 0
    res.check("app rows are in a second column, right of the folders",
              bool(apps) and min(a["x"] for a in apps) >= side_right,
              f"apps start at {min((a['x'] for a in apps), default=-1)}, "
              f"sidebar ends at {side_right}")
    res.check("the search row spans the menu",
              bool(search) and search[0]["w"] >= menu["w"] - 2,
              f"search w={search[0]['w'] if search else None} menu w={menu['w']}")
    res.check("a folder is selected to begin with",
              any(c["selected"] for c in cats),
              f"selected: {[c['label'] for c in cats if c['selected']]}")

    # --- 2. a folder shows its own apps --------------------------------
    by_app = dbg.menu_apps()          # {app label: folder label}
    res.check("the menu reports which folder each app is in",
              len(by_app) > 3, f"{len(by_app)} apps")
    # A folder other than the open one, so this cannot pass by accident.
    open_folder = next((c["label"] for c in cats if c["selected"]), None)
    target = next((c["label"] for c in cats
                   if c["label"] != open_folder
                   and any(v == c["label"] for v in by_app.values())), None)
    if target:
        dbg.menu_select_folder(target)
        menu = dbg.menu()
        shown = sorted(r["label"] for r in rows_of(menu, "app"))
        want = sorted(a for a, cat in by_app.items() if cat == target)
        res.check(f"clicking {target!r} shows exactly that folder's apps",
                  shown == want, f"shown {shown} want {want}")
        res.check("...and the clicked folder is the selected one",
                  any(c["selected"] and c["label"] == target
                      for c in rows_of(menu, "category")),
                  f"selected {[c['label'] for c in rows_of(menu, 'category') if c['selected']]}")
        res.check("...and the menu stayed open -- selecting is not committing",
                  menu.get("open"))

    # --- 3. the search field is DRAWN ----------------------------------
    menu = dbg.menu()
    field = rows_of(menu, "search")[0]
    keep = next((r for r in rows_of(menu, "action") if r["label"] == "Shutdown"),
                rows_of(menu, "action")[0])
    before = shot(qmp, tmp, "startmenu-empty.png")
    type_text(dbg, "cal")
    after = shot(qmp, tmp, "startmenu-typed.png")
    res.check("typing changes the pixels inside the search field",
              crop(before, field) != crop(after, field))
    res.check("...and leaves a sidebar action row untouched",
              crop(before, keep) == crop(after, keep),
              f"{keep['label']} row changed")

    # --- 4. what the query does ----------------------------------------
    menu = dbg.menu()
    res.check("the WM reports the typed query", menu.get("query") == "cal",
              f"query={menu.get('query')!r}")
    hits = [r["label"] for r in rows_of(menu, "app")]
    res.check("every remaining row matches the query",
              bool(hits) and all("cal" in h.lower() for h in hits), f"{hits}")
    res.check("search reaches an app outside the open folder",
              "Calculator" in hits, f"{hits}")
    res.check("the first result is highlighted, ready for Enter",
              any(r["selected"] for r in rows_of(menu, "app")))

    # --- 5. Enter launches it ------------------------------------------
    dbg.key(KEY_ENTER)
    deadline = time.time() + 20
    while time.time() < deadline and dbg.window("Calculator") is None:
        time.sleep(0.5)
    launched = dbg.window("Calculator") is not None
    res.check("Enter launches the highlighted result", launched,
              "" if launched else "no Calculator window appeared")
    if launched:
        dbg.key(KEY_F4, mods="alt")
        time.sleep(0.8)

    # --- 6. Esc, one level at a time ------------------------------------
    open_menu(dbg)
    type_text(dbg, "ca")
    dbg.key(KEY_ESC)
    menu = dbg.menu()
    res.check("Esc clears the query before it closes anything",
              menu.get("open") and menu.get("query") == "",
              f"open={menu.get('open')} query={menu.get('query')!r}")
    dbg.key(KEY_ESC)
    res.check("a second Esc closes the menu", not dbg.menu().get("open"))

    # --- 7. the arrows ---------------------------------------------------
    open_menu(dbg)
    dbg.key(KEY_DOWN)
    first = [r["selected"] for r in rows_of(dbg.menu(), "app")]
    dbg.key(KEY_DOWN)
    second = [r["selected"] for r in rows_of(dbg.menu(), "app")]
    res.check("Down highlights a result and moves it",
              any(first) and any(second) and first != second,
              f"{first} then {second}")
    dbg.key(KEY_UP)
    res.check("Up moves it back",
              [r["selected"] for r in rows_of(dbg.menu(), "app")] == first)

    before_folder = dbg.menu().get("category")
    dbg.key(KEY_RIGHT)
    res.check("Right moves to the next folder",
              dbg.menu().get("category") != before_folder,
              f"{before_folder} -> {dbg.menu().get('category')}")

    # Leave nothing open: a left-over menu eats the next tool's first
    # click, anywhere on screen.
    open_menu(dbg, want=False)
    res.check("the menu closes again", not dbg.menu().get("open"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "start_menu_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nstart_menu_test: {n_ok} passed, {n_bad} failed")
    if res.fails:
        print("  failed: " + ", ".join(res.fails))
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
