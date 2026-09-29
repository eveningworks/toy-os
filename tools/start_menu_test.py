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
  * A PIN SURVIVES A REBOOT. Pinning through the row's own context menu
    makes a Favourites folder appear and the app show in it; the store
    is then read back from `/etc/start-menu.conf` through an
    INDEPENDENT path (`sh cat`), and the guest is REBOOTED and asked
    again -- which is the only version of this check worth having,
    since a pin that lasted until the next boot would be the bug.
  * A LAUNCH PUTS AN APP IN Recent, newest first, and the count the WM
    reports goes up by exactly one per launch.
  * A FOLDER TALLER THAN THE PANE SCROLLS: the wheel moves the window
    over the list (the rows shown change, the list does not), End
    reaches the last row, and the selection is never off screen -- the
    bug this pair exists for is a highlight that walks past the bottom
    while Enter goes on launching something invisible.
  * RANKING: typing a prefix puts the app whose name STARTS with it
    first. "te" offers Terminal before Crash Test, which a substring
    match alone does not.
  * THE DESCRIPTION STRIP says what the hovered row is, from its
    `Comment=` -- and says nothing, rather than the last thing, when
    the pointer is on a row that has none.
  * A LINE TOO LONG FOR THE STRIP IS MARKED `..`, and the TOOLTIP
    carries the whole of it after a hover delay. Three things are
    asserted separately because they fail separately: it is NOT up
    immediately (a tooltip with no delay strobes as the pointer crosses
    a list), it IS up after the delay with the full text, and a CLICK
    still reaches the row underneath -- a tooltip that took the pointer
    would eat the click, which is the classic way to get this wrong.
    **The "not up yet" half is SKIPPED, not failed, when the probe
    itself took longer than the delay** -- every console command against
    the bare-metal machine is a telnet round trip of about half a
    second, which is the whole delay, so asserting it there would
    measure the transport. It reports as "not measurable here" and the
    run stays honest; the VM asks about 280ms in and measures it.

Positive controls, each run once when this was written (2026-09-17):
  * start_menu_key() returning 0 for printable characters reddens every
    search check and the Enter-launches check, and nothing else.
  * pane_row() ignoring the selected folder (always listing every app)
    reddens "shows exactly that folder's apps" while leaving the
    selection check green -- which is the pair of checks that
    distinguishes the two failures.
  * start_store_pin() not writing the file reddens the four persistence
    checks (the store on disk, and the three after the reboot) and
    NOTHING else -- "pinning makes a Favourites folder" stays green,
    which is exactly the failure a same-boot check cannot see.
  * wm_tooltip_update() without its delay reddens exactly one check,
    "no tooltip has appeared yet" -- the one that separates a hint from
    a box that strobes as the pointer crosses a list.

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
KEY_HOME = "0x97"
KEY_END = "0x98"
# UUI_TOOLTIP_DELAY_TICKS (ui/uui_toolbar.h) at the PIT's 100 Hz. Named
# here so the one check that races it says what it is racing.
TOOLTIP_DELAY_S = 0.5


class Result:
    def __init__(self):
        self.passes, self.fails, self.skips = [], [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
        return bool(ok)

    def skip(self, name, why):
        """A check this RUN could not measure -- not a pass.

        Counted and printed separately, because "we could not ask in
        time" and "the answer was right" are different statements and a
        tool that reported the first as the second would be lying in the
        direction that matters.
        """
        self.skips.append(name)
        print(f"  SKIP  {name}   {why}")
        return False


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


def reboot(dbg, timeout=90):
    """Reboot the guest and wait for its desktop to answer again.

    The only way to test that something PERSISTED. `reboot` is a /bin
    program, so the console's connection dies with the machine -- the
    wait is a fresh console per poll, and the first one that answers is
    the new boot.
    """
    dbg.send("sh sync")
    try:
        dbg.send("sh spawn /bin/reboot")
    except (OSError, EOFError):
        pass        # the machine going away mid-command is the point
    time.sleep(3)
    if not dbg.reconnect(timeout):
        return False
    deadline = time.time() + timeout
    while time.time() < deadline:
        if "screen " in (dbg.send("gui state") or ""):
            time.sleep(1.5)     # let the desktop finish its first frame
            return True
        time.sleep(2)
    return False


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
    #
    # ALL APPS, EXPLICITLY. The menu opens on Favourites or Recent when
    # either exists -- and check 5 above LAUNCHES something, so by here
    # Recent does, with one row in it. Two Down presses on a one-row
    # folder land on the same row, which read as the arrows being
    # broken: the test had assumed a default that its own earlier check
    # changes.
    open_menu(dbg)
    dbg.menu_select_folder("All Apps")
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

    # --- 8. the description strip ----------------------------------------
    open_menu(dbg)
    menu = dbg.menu()
    row = next((r for r in rows_of(menu, "app") if r["label"] == "Notepad"), None)
    if row is None:
        x, y = dbg.menu_app_row("Notepad")
        menu = dbg.menu()
        row = next(r for r in rows_of(menu, "app") if r["label"] == "Notepad")
    dbg.warp_cursor(qmp, row["cx"], row["cy"])
    said = dbg.menu().get("description") or ""
    res.check("the strip says what the hovered app is",
              len(said) > 8, f"{said!r}")
    # AND IT DOES NOT LATCH. A strip that kept showing the last app's
    # line while the pointer sat on something else would be worse than
    # an empty one -- it would be wrong. A sidebar action is the row to
    # prove it on: every shipped app entry has a `Comment=` now, so the
    # "nothing to say" case is a row that is not an app at all.
    act = rows_of(dbg.menu(), "action")[0]
    dbg.warp_cursor(qmp, act["cx"], act["cy"])
    res.check("...and says nothing on a row that is not an app",
              not (dbg.menu().get("description") or ""),
              f"{dbg.menu().get('description')!r}")

    # --- 8b. the tooltip --------------------------------------------------
    #
    # A LONG description is the case worth testing: Crash Test's runs
    # past the strip, so the strip must MARK the cut and the tooltip
    # must carry the rest.
    try:
        dbg.menu_app_row("Crash Test")
        row = next(r for r in rows_of(dbg.menu(), "app")
                   if r["label"] == "Crash Test")
    except (KeyError, StopIteration):
        row = None
    if row:
        dbg.warp_cursor(qmp, row["cx"], row["cy"])
        full = dbg.menu().get("description") or ""
        res.check("the strip reports the whole description",
                  len(full) > 40, f"{full!r}")
        # NOT YET: the delay is the thing being tested, and a tooltip
        # that appeared on arrival would pass every other check here.
        #
        # ...but only when the PROBE ITSELF got there in time. Every
        # console command is a round trip, and against the bare-metal
        # machine that is a telnet exchange of about half a second --
        # the whole delay. Asking late and then asserting "not up"
        # measures the transport, not the feature: on hardware this
        # failed while the tooltip was behaving perfectly.
        t0 = time.time()
        dbg.warp_cursor(qmp, row["cx"], row["cy"])
        asked = dbg.json("gui tooltip --json")
        elapsed = time.time() - t0
        if elapsed < TOOLTIP_DELAY_S * 0.8:
            res.check("...and no tooltip has appeared yet", not asked.get("open"),
                      f"asked {elapsed * 1000:.0f}ms in")
        else:
            res.skip("...and no tooltip has appeared yet",
                     f"the probe took {elapsed * 1000:.0f}ms of a "
                     f"{TOOLTIP_DELAY_S * 1000:.0f}ms delay -- this check needs a "
                     f"faster transport than this machine has")
        time.sleep(1.2)
        tip = dbg.json("gui tooltip --json")
        res.check("after a pause the tooltip carries the full text",
                  tip.get("open") and tip.get("text") == full,
                  f"{tip.get('open')} {tip.get('text')!r}")
        # THE PIXELS, because "the WM says it is open" is not "it is on
        # screen": the box is drawn over the desktop, so the rect it
        # claims must differ from the frame taken before it appeared.
        if tip.get("open"):
            box = (tip["x"], tip["y"], tip["x"] + tip["w"], tip["y"] + tip["h"])
            with_tip = qmp.stable_pixels(os.path.join(tmp, "tip-on.png"), box=box)
            dbg.warp_cursor(qmp, act["cx"], act["cy"])   # a row with no comment
            time.sleep(0.4)
            without = qmp.stable_pixels(os.path.join(tmp, "tip-off.png"), box=box)
            res.check("...and it is DRAWN, not just reported",
                      with_tip != without)
            res.check("...and moving off it takes it down",
                      not dbg.json("gui tooltip --json").get("open"))

    # --- 9. scrolling a folder taller than the pane ------------------------
    dbg.menu_select_folder("All Apps")
    menu = dbg.menu()
    listed, shown = menu.get("listed", 0), len(rows_of(menu, "app"))
    res.check("All Apps holds more than the pane shows",
              listed > shown, f"{listed} apps, {shown} rows")
    if listed > shown:
        first_rows = [r["label"] for r in rows_of(menu, "app")]
        dbg.wheel(-3)
        m2 = dbg.menu()
        res.check("the wheel scrolls the list, and the list does not change",
                  m2.get("scroll", 0) > 0 and m2.get("listed") == listed and
                  [r["label"] for r in rows_of(m2, "app")] != first_rows,
                  f"scroll={m2.get('scroll')} listed={m2.get('listed')}")
        dbg.key(KEY_END)
        m3 = dbg.menu()
        sel = [r["label"] for r in rows_of(m3, "app") if r["selected"]]
        res.check("End reaches the last row, and it is ON SCREEN",
                  m3.get("scroll", 0) == listed - shown and len(sel) == 1,
                  f"scroll={m3.get('scroll')} of {listed - shown}, selected={sel}")
        dbg.key(KEY_HOME)
        res.check("Home goes back to the top",
                  dbg.menu().get("scroll", 0) == 0)

    # --- 10. ranking -------------------------------------------------------
    type_text(dbg, "te")
    hits = [r["label"] for r in rows_of(dbg.menu(), "app")]
    res.check("a prefix match is offered before a substring one",
              bool(hits) and hits[0].lower().startswith("te"), f"{hits}")
    dbg.key(KEY_ESC)

    # --- 11. Recent, and a pin that survives a reboot ----------------------
    before = {a["label"]: a for a in dbg.menu().get("apps", [])}
    runs_before = before.get("Calculator", {}).get("runs", 0)
    open_menu(dbg, want=False)
    dbg.open_app("Calculator")
    deadline = time.time() + 20
    while time.time() < deadline and dbg.window("Calculator") is None:
        time.sleep(0.5)
    if dbg.window("Calculator") is not None:
        dbg.key(KEY_F4, mods="alt")
        time.sleep(0.8)
    open_menu(dbg)
    after = {a["label"]: a for a in dbg.menu().get("apps", [])}
    res.check("a launch is counted",
              after.get("Calculator", {}).get("runs", 0) == runs_before + 1,
              f"{runs_before} -> {after.get('Calculator', {}).get('runs')}")
    res.check("...and Recent is a folder now",
              any(c["label"] == "Recent"
                  for c in rows_of(dbg.menu(), "category")))
    dbg.menu_select_folder("Recent")
    recent = [r["label"] for r in rows_of(dbg.menu(), "app")]
    res.check("...with the app just launched at the top",
              bool(recent) and recent[0] == "Calculator", f"{recent}")

    # ESTABLISH THE PRECONDITION, do not assume it. On a machine that
    # has been used -- the bare-metal one, or a disk image a previous
    # run wrote to -- Notepad may already be pinned, and the row then
    # offers Unpin. The first version of this asserted "Pin to Start" is
    # present and failed on a working menu for that reason.
    def pin_menu(label_wanted):
        x, y = dbg.menu_app_row("Notepad")
        dbg.rclick(x, y)
        time.sleep(0.6)
        rows = [r["label"] for r in (dbg.ctxmenu() or {}).get("rows", [])]
        pos = dbg.ctxmenu_row(label_wanted)
        if pos:
            dbg.click(*pos)
            time.sleep(0.6)
        return rows

    labels = pin_menu("Unpin from Start")   # a no-op when it is not pinned
    res.check("an app row offers the pin toggle, by the name of what it does",
              "Pin to Start" in labels or "Unpin from Start" in labels, f"{labels}")
    labels = pin_menu("Pin to Start")
    res.check("...and once unpinned it offers Pin to Start",
              "Pin to Start" in labels, f"{labels}")
    res.check("pinning makes a Favourites folder",
              any(c["label"] == "Favourites"
                  for c in rows_of(dbg.menu(), "category")))
    # THROUGH AN INDEPENDENT PATH: the menu believing it is pinned and
    # the file saying so are different claims, and only the second
    # survives a boot.
    stored = dbg.send("sh cat /etc/start-menu.conf") or ""
    res.check("...and the store on disk says so",
              "pinned=" in stored and "notepad" in stored,
              stored.strip().splitlines()[-1] if stored.strip() else "empty")

    open_menu(dbg, want=False)
    if reboot(dbg):
        open_menu(dbg)
        menu = dbg.menu()
        res.check("AFTER A REBOOT the pin is still there",
                  any(c["label"] == "Favourites"
                      for c in rows_of(menu, "category")),
                  f"{[c['label'] for c in rows_of(menu, 'category')]}")
        res.check("...the menu opens on Favourites",
                  menu.get("category") == "Favourites", f"{menu.get('category')}")
        fav = [r["label"] for r in rows_of(menu, "app")]
        res.check("...and the pinned app is in it", fav == ["Notepad"], f"{fav}")
        res.check("...and the launch count survived too",
                  {a["label"]: a["runs"] for a in menu.get("apps", [])}
                      .get("Calculator", 0) >= 1)

    # PUT THE MACHINE BACK. A pin is user-visible state that outlives
    # the run -- on the bare-metal machine it outlives the DAY -- and
    # CLAUDE.md's rule for a test that changes the machine applies to
    # this as much as to a setting. The launch history is deliberately
    # NOT reset: it is a counter of real launches, and this tool made
    # one.
    pin_menu("Unpin from Start")
    res.check("the pin is removed again, leaving the machine as it was",
              not any(a["label"] == "Notepad" and a["pinned"]
                      for a in dbg.menu().get("apps", [])))

    # --- 12. Recent keeps its order while the menu is up -------------------
    # A launch records itself before the menu has gone, and a live order
    # moved the launched row to the top UNDER THE POINTER. The rows are
    # read in the instant after the click, while the menu still reports
    # itself open; a second app launched first makes the list two long,
    # so the bottom row is not already the top one.
    open_menu(dbg, want=False)
    for app in ("Minesweeper",):
        dbg.open_app(app)
        deadline = time.time() + 20
        while time.time() < deadline and dbg.window(app) is None:
            time.sleep(0.5)
        if dbg.window(app) is not None:
            dbg.key(KEY_F4, mods="alt")
            time.sleep(0.8)
    open_menu(dbg)
    dbg.menu_select_folder("Recent")
    before = [r["label"] for r in rows_of(dbg.menu(), "app")]
    during, was_open = before, False
    if len(before) >= 2:
        x, y = dbg.menu_app_row(before[-1])
        dbg.send(f"gui click {x} {y}")
        m = dbg.menu()
        during, was_open = [r["label"] for r in rows_of(m, "app")], bool(m.get("open"))
    res.check("clicking Recent's bottom row does not reorder it while the menu is up",
              len(before) >= 2 and was_open and during == before,
              f"{before} -> {during} (open={was_open})")
    deadline = time.time() + 20
    while len(before) >= 2 and time.time() < deadline and dbg.window(before[-1]) is None:
        time.sleep(0.5)
    if len(before) >= 2 and dbg.window(before[-1]) is not None:
        dbg.key(KEY_F4, mods="alt")
        time.sleep(0.8)
    open_menu(dbg)
    dbg.menu_select_folder("Recent")
    again = [r["label"] for r in rows_of(dbg.menu(), "app")]
    res.check("...and the next time it opens, that app is first",
              bool(again) and len(before) >= 2 and again[0] == before[-1],
              f"{again}")

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
    tail = f", {len(res.skips)} not measurable here" if res.skips else ""
    print(f"\nstart_menu_test: {n_ok} passed, {n_bad} failed{tail}")
    if res.fails:
        print("  failed: " + ", ".join(res.fails))
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
