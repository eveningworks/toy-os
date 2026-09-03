#!/usr/bin/env python3
"""tools/uidemo_test.py -- drive UI Demo's widgets and assert on its log.

WHAT THIS IS
------------
UI Demo (`apps/uidemo.c`) exists to be a known target: one of every
`apps/ui/` widget, and every interaction reported as a single parseable
line. This is the other half of that -- the thing that drives it and
checks the lines came out right. A widget regression shows up here as a
named failing check rather than as something subtly wrong in a
screenshot nobody looks at closely.

    python3 tools/vm.py start          # or --disk a copy
    python3 tools/uidemo_test.py       # enters GUI mode itself
    echo $?                            # 0 = every check passed

WHY IT IS A TOOL AND NOT A SCRIPT
---------------------------------
It encodes three things that cost real time to rediscover:

  1. **Geometry comes from the app**, via its `uidemo: layout <widget>
     <x> <y> <w> <h>` lines, not from re-deriving row offsets from font
     metrics in Python. The Python copy drifts silently the moment a row
     is added to the app -- which is exactly what happened when the
     dropdown and listbox rows landed between the textbox and the
     scrollback.
  2. **Coordinates must be inside the window.** A click outside the
     content rect never reaches the app at all, so it proves nothing --
     and reads as a failing widget rather than a bad test. The
     dismiss-the-popup check hit this for real: at `x + 400` it was
     past the right edge of a ~406px-wide content area.
  3. **Key codes go in as hex** (`gui key 0x92`), matching
     `api/keyboard.h`. That only works as of the `parse_int()` fix in
     the same change as this file; against an older kernel every key
     command comes back "bad or dropped key".

POSITIVE CONTROL
----------------
Recorded when the listbox scrollbar checks were added: make
`uui_listbox_press()` return 0 immediately (the state the bar shipped
in) and rebuild. The four scrollbar checks go red, plus the popup one
and four cascades from the diverged state. **"the drag changed no
selection" stays GREEN** -- an absence check cannot catch a control that
does nothing, which is the useful half of running the control at all.

CAVEAT
------
Injected input enters below the PS/2 driver (see tools/gui_debug.py), so
a clean run says nothing about the real mouse or keyboard path, and
nothing here looks at pixels -- use tools/pixel_probe.py for anything
whose answer is a colour.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402

# utheme.c's `accent`, which uui_focus_ring() draws every focus indicator
# in. Duplicated here rather than derived, deliberately: a test that read
# the colour out of the guest would agree with a broken guest.
UTHEME_ACCENT_RGB = (70, 110, 160)
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/bin/wm/demos/uidemo"   # a ring-3 process since M41's stage 0
SPAWN_TIMEOUT_S = 15.0

# api/keyboard.h. Sent as hex, which is how that header writes them.
K_UP, K_DOWN = "0x91", "0x92"
K_PGUP, K_PGDN = "0x93", "0x94"
K_HOME, K_END = "0x97", "0x98"
K_LEFT, K_RIGHT = "0x95", "0x96"
K_ESC = "0x1b"
# Tab is 0x09 with OR without Shift -- the modifier word is the only
# thing that distinguishes them. See api/keyboard.h's "Modifier bits".
K_TAB = "0x09"
# Space has to go in as hex: `gui key` splits its arguments on
# whitespace, so a literal " " arrives as no argument at all.
K_SPACE = "0x20"


class Demo:
    """UI Demo, plus the geometry it reported about itself."""

    def __init__(self, dbg, verbose=False):
        self.dbg = dbg
        self.verbose = verbose
        self.fails, self.passes = [], []
        self.layout = {}
        self.row_h = 0
        self.win = None

    # -- plumbing ------------------------------------------------------

    def events(self):
        return self.dbg.logs("uidemo:", clear=True)

    def _abs(self, cx, cy):
        return self.win["content"]["x"] + cx, self.win["content"]["y"] + cy

    def click(self, cx, cy):
        x, y = self._abs(cx, cy)
        self.dbg.send(f"gui click {x} {y}")
        self.dbg.settle()
        return self.events()

    def drag(self, cx0, cy0, cx1, cy1):
        x0, y0 = self._abs(cx0, cy0)
        x1, y1 = self._abs(cx1, cy1)
        self.dbg.send(f"gui drag {x0} {y0} {x1} {y1}")
        self.dbg.settle()
        return self.events()

    def key(self, k, mods=""):
        """`mods` is a space-separated list of shift/ctrl/alt/altgr."""
        self.dbg.send(f"gui key {k} {mods}".rstrip())
        self.dbg.settle()
        return self.events()

    def park(self, qmp, cx, cy):
        """Park the REAL cursor over a widget, and wait for it to land.

        Needed since the toolkit routes the wheel to the widget UNDER
        THE CURSOR (ui/uui_route.h) rather than down a fixed chain in
        the app -- the wheel scrolls what you are pointing at, so a
        wheel test has to point at something first.

        `gui move` is NOT enough here: an injected move holds for one WM
        iteration and then the real PS/2 cursor takes over, so the
        client's last-known pointer ends up wherever the real cursor
        was. warp_cursor() drives the real one and confirms it arrived.
        """
        x, y = self._abs(cx, cy)
        self.dbg.warp_cursor(qmp, x, y)
        self.dbg.settle()
        return self.events()

    def wheel(self, notches):
        self.dbg.send(f"gui wheel {notches}")
        self.dbg.settle()
        return self.events()

    # -- assertions ----------------------------------------------------

    def check(self, name, got, want):
        self._record(name, any(want in l for l in got), got, f"wanted {want!r}")

    def check_absent(self, name, got, unwanted):
        self._record(name, not any(unwanted in l for l in got), got,
                     f"did NOT want {unwanted!r}")

    def _record(self, name, ok, got, why):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if ok:
            self.passes.append(name)
            if self.verbose:
                print(f"        {got}")
        else:
            print(f"        {why}")
            print(f"        got    {got}")
            self.fails.append(name)

    # -- setup ---------------------------------------------------------

    def open(self):
        """Spawn a FRESH UI Demo and read its self-reported layout.

        UI Demo is a RING-3 PROCESS since Milestone 41's stage 0
        (userland/gui/uidemo.c), so this spawns it rather than opening a
        kernel-space window, and waits for the app's own layout lines --
        a window in the WM's list does not yet mean the client has run
        its on_open. Everything else is unchanged: close whatever is on
        screen first, or a stale window's coordinates are read instead.
        """
        while True:
            ws = self.dbg.json("gui windows --json")["windows"]
            if not ws:
                break
            self.dbg.send(f"gui close {len(ws) - 1}")
            self.dbg.settle()
        self.dbg.logs("", clear=True)

        self.dbg.spawn(SPAWN_PATH, "UI Demo")
        deadline = time.time() + SPAWN_TIMEOUT_S
        lines = []
        while time.time() < deadline:
            # logs(), NOT events(). events() filters the output of the
            # ONE command it issues, so a line printed while some other
            # command was in flight is lost to it -- and spawn() polls
            # `gui windows --json` while waiting for the window, which
            # reads the wire (and therefore the app's own startup lines)
            # to the prompt. logs() reads send()'s accumulated buffer, so
            # it sees them wherever they landed.
            #
            # That race made this tool ERROR at startup roughly one run
            # in six ("UI Demo reported no layout"), never failing a
            # check -- a run that measured nothing. CLAUDE.md records the
            # trap; this is it biting.
            lines += self.dbg.logs("uidemo:")
            if any("layout listbox.row_h" in l for l in lines):
                break
            time.sleep(0.2)
        for line in lines:
            p = line.split()
            if len(p) >= 7 and p[1] == "layout":
                self.layout[p[2]] = tuple(int(v) for v in p[3:7])
            elif len(p) == 4 and p[2] == "listbox.row_h":
                self.row_h = int(p[3])

        self.win = [w for w in self.dbg.json("gui windows --json")["windows"]
                    if w["title"] == "UI Demo"][-1]
        if not self.layout or not self.row_h:
            raise RuntimeError("UI Demo reported no layout -- is this an older kernel?")

    # -- geometry helpers ---------------------------------------------

    def list_row(self, n):
        """Centre of VISIBLE row n of the listbox (0 = topmost shown)."""
        x, y, w, h = self.layout["listbox"]
        return x + 10, y + n * self.row_h + self.row_h // 2

    def textbox_center(self):
        x, y, w, h = self.layout["textbox"]
        return x + w // 2, y + h // 2

    def list_bar_x(self):
        """A column inside the listbox's SCROLLBAR strip.

        The strip is uui_listbox's `bar_w` (8px) at the control's right
        edge; -4 lands in the middle of it. Taken from the listbox's own
        reported rect rather than re-derived, per this file's rule.
        """
        x, y, w, h = self.layout["listbox"]
        return x + w - 4

    def dropdown_center(self):
        x, y, w, h = self.layout["dropdown"]
        return x + w // 2, y + h // 2

    def popup_row(self, n):
        """Centre of row n of the OPEN popup (it hangs below the box)."""
        x, y, w, h = self.layout["dropdown"]
        return x + 10, y + h + 1 + n * self.row_h + self.row_h // 2


def run(d, qmp):
    lx, ly, lw, lh = d.layout["listbox"]
    ddc = d.dropdown_center()

    print("\n== listbox: mouse ==")
    d.check("click row 1 selects it", d.click(*d.list_row(1)), "list 1 bravo")
    d.check("click row 3 selects it", d.click(*d.list_row(3)), "list 3 delta")
    # A ROW SELECTS ON CONTACT here, unlike the kernel widget this
    # replaced, which armed on press and committed on release. That is
    # the ring-3 listbox's existing contract (uui_listbox.h) and what
    # Windows and GTK do -- the commit-on-release rule
    # docs/gui-guidelines.md states is about BUTTONS, whose action is not
    # already visible. So the assertion is that the press itself selects:
    # a drag beginning on row 0 selects row 0 and dragging away does not
    # un-select it.
    d.check("a press selects on contact and dragging off keeps it",
            d.drag(lx + 10, ly + d.row_h // 2, lx - 60, ly - 60),
            "list 0 alpha")

    print("\n== listbox: keyboard ==")
    # Selection is row 0 after the drag above, not row 3.
    d.check("down arrow moves selection", d.key(K_DOWN), "list 1 bravo")
    d.check("up arrow moves selection", d.key(K_UP), "list 0 alpha")
    d.check("End jumps to last", d.key(K_END), "list 11 lima")
    d.check("Home jumps to first", d.key(K_HOME), "list 0 alpha")
    d.check("PageDown pages", d.key(K_PGDN), "list 4 echo")

    print("\n== listbox: wheel scrolls the view, not the selection ==")
    d.key(K_HOME)
    # Point at the listbox: the wheel goes to the widget under the
    # cursor now, not to whichever widget the app happened to try first.
    d.park(qmp, *d.list_row(1))
    d.events()
    # 3 notches x wheel_rows(3) = 9 rows; with 12 items and 4 visible,
    # max_top is 8, so the top visible row becomes item 8.
    d.check_absent("wheel changes no selection", d.wheel(-3), "list ")
    d.check("view actually scrolled", d.click(*d.list_row(0)), "list 8 india")

    print("\n== listbox: the SCROLLBAR, which a user found inert ==")
    # This bar DREW and handled nothing -- no drag, no paging, and a
    # click on it did not even reach the widget. Found by dragging it on
    # the desktop, after a 28-check suite passed. The checks below are
    # the ones that were missing; see docs/gui-guidelines.md's
    # "Scrollbars: what a real one does".
    d.key(K_HOME)          # selection to row 0, view to the top
    d.events()
    bx = d.list_bar_x()
    lby = ly

    # 1. The thumb DRAGS, and it tracks the cursor rather than paging.
    got = d.drag(bx, lby + 8, bx, lby + lh - 12)
    d.check("dragging the listbox thumb scrolls the view", got, "list_scroll")
    d.check("...and it is a thumb drag, not a track page", got, "thumb")
    # 2. Reversible: dragging back returns it to the top. A bar that
    #    only ever moved one way would pass the check above.
    got = d.drag(bx, lby + lh - 12, bx, lby + 8)
    d.check("dragging back returns it to the top", got, "list_scroll 0")
    # 3. Scrolling the view must NOT change the selection -- the same
    #    rule the wheel follows.
    d.check_absent("the drag changed no selection", got, "list ")
    # 4. A click on the TRACK pages toward it.
    d.check("clicking the track pages", d.click(bx, lby + lh - 6),
            "list_scroll 3 page")

    print("\n== dropdown ==")
    d.check("click opens popup", d.click(*ddc), "dropdown open")
    d.check("click popup row commits value", d.click(*d.popup_row(2)),
            "dropdown 2 Capybara")
    d.check("click reopens", d.click(*ddc), "dropdown open")
    # The POPUP's scrollbar is the same widget, and clicking it used to
    # DISMISS the popup -- the most annoying possible answer to "I tried
    # to scroll". It must scroll and stay open.
    px, py, pw, ph = d.layout["dropdown"]
    got = d.click(px + pw - 4, py + ph + 40)
    d.check_absent("clicking the popup's scrollbar does not dismiss it", got,
                   "dropdown close")
    d.check_absent("...and does not commit a value either", got, "dropdown 0")
    # Dismiss from INSIDE the window but outside the popup. A point past
    # the content rect never reaches the app -- see the module docstring.
    bx, by, bw, bh = d.layout["btn1"]
    got = d.click(bx + bw // 2, by + bh // 2)
    d.check_absent("outside click changes no value", got, "dropdown 0")
    # ...and it is SWALLOWED: the dismissing click must not also press
    # the button it landed on, which is the half of this that a popup
    # falling through would break.
    d.check("outside click is swallowed by the popup", got, "dropdown close")
    d.check_absent("...and did not press the button underneath", got, "button 1")

    print("\n== keyboard focus follows the click ==")
    # Focus somewhere else FIRST, so "clicking the listbox focuses it"
    # can actually be a change -- a check that passes only because focus
    # was already there proves nothing.
    d.click(*d.textbox_center())
    d.events()
    d.check("clicking the listbox focuses it", d.click(*d.list_row(0)), "focus listbox")
    d.check("...and arrows now reach the listbox", d.key(K_DOWN), "list ")
    d.check("clicking the dropdown focuses it back", d.click(*ddc), "focus dropdown")
    d.key(K_ESC)
    d.events()

    print("\n== dropdown: keyboard while closed ==")
    # A CLOSED ring-3 dropdown takes only the keys that OPEN it (Down,
    # Enter, Space) and ignores the rest -- it does not cycle its value
    # with the popup shut, which the kernel widget did. Both are real
    # toolkit behaviours; this one means an arrow key can never change a
    # setting the user cannot see.
    # "unconsumed", not "(no focus)": the app used to print the latter
    # for a key the FOCUSED widget declined, which is what this case is
    # -- the dropdown has focus and simply does not want Home.
    d.check("Home while closed does nothing", d.key(K_HOME), "unconsumed")
    d.check("down arrow opens it instead of cycling", d.key(K_DOWN), "dropdown open")
    d.key(K_ESC)
    d.events()

    print("\n== Tab / Shift-Tab move focus ==")
    # Tab order is the app's array order: textbox, dropdown, listbox,
    # checkbox0, checkbox1, radio. The checkbox and radio list JOINED
    # the ring when they gained keyboard behaviour (docs/bugs.md, fixed)
    # -- before that they were deliberately left out, on the grounds
    # that a stop with no keyboard behaviour is a stop that does
    # nothing, which was true and was also why a keyboard-only user
    # could not toggle a checkbox at all. The button group is still not
    # a stop, for that original reason.
    # Focus is on the dropdown (index 1) here.
    d.check("Tab moves forward", d.key(K_TAB), "focus listbox")
    d.check("Tab reaches the checkbox", d.key(K_TAB), "focus checkbox0")
    d.check("Tab reaches the second checkbox", d.key(K_TAB), "focus checkbox1")
    d.check("Tab reaches the radio list", d.key(K_TAB), "focus radio")
    d.check("Tab wraps", d.key(K_TAB), "focus textbox")
    # Shift-Tab is the case modifier bits exist for at all: Tab has no
    # shifted character, so without them this is indistinguishable from
    # plain Tab and a ring can only ever cycle one way.
    d.check("Shift-Tab moves backward", d.key(K_TAB, "shift"), "focus radio")

    print("\n== the keyboard reaches act-on-contact controls ==")
    # Focus is on the radio list. ARROWING IS CHOOSING on a radio group
    # -- Win32, GTK and Qt all commit on the arrow rather than
    # previewing -- so one Down is a whole selection change, reported in
    # the SAME grammar a mouse release produces.
    d.check("Down moves the radio selection", d.key(K_DOWN), "radio ")
    d.check("Up moves it back", d.key(K_UP), "radio ")

    # Back to the checkboxes: Shift-Tab twice from the radio list.
    d.key(K_TAB, "shift")
    d.key(K_TAB, "shift")
    # SPACE TOGGLES, and the assertion is on the VALUE, not merely that
    # something was logged: a checkbox that reported "check alpha off"
    # every time would pass a "did it respond" check while never
    # toggling. Two presses, and they must report different states.
    on = d.key(K_SPACE)
    off = d.key(K_SPACE)
    d.check("Space checks the box", on, "check alpha on")
    d.check("Space unchecks it again", off, "check alpha off")

    print("\n== buttons still commit on RELEASE, not on press ==")
    # The one control where commit-on-release matters, and the reason
    # the rule exists: a button's action is not already visible, so a
    # press the user drags away from must do nothing.
    bx, by, bw, bh = d.layout["btn1"]
    d.check("clicking a button commits it",
            d.click(bx + bw // 2, by + bh // 2), "button 1")
    d.check_absent("a press dragged off the button commits nothing",
                   d.drag(bx + bw // 2, by + bh // 2, bx + bw // 2, by - 80),
                   "button 1")


def check_containment(d, qmp, tmp):
    """The window manager must clip an app to its own window.

    UI Demo deliberately paints two magenta squares outside its own
    content rect on every frame (see apps/uidemo.c). They must never
    reach the screen: wm_render.c's clip_to_window_content() narrows the
    clip to the window's content area around on_draw(), which is a
    containment boundary rather than an optimisation -- without it, an
    app with a layout bug paints over the desktop and over whatever
    window sits behind it.

    Asserted as "this colour is nowhere on screen" rather than by
    sampling the two spots, because a broken boundary does not
    necessarily fail THERE: it lets everything the app draws escape, and
    the markers are only the part guaranteed to be outside.
    """
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, "uidemo_containment.png"))
    qmp.screenshot(p)
    with Image.open(p) as im:
        colours = im.convert("RGB").getcolors(maxcolors=1 << 24) or []
        leaked = sum(n for n, c in colours if c == (255, 0, 255))
    d._record("an app cannot draw outside its own window", leaked == 0,
              [], f"{leaked} magenta pixel(s) escaped the window")


def check_widgets_drawn(d, qmp, tmp):
    """The widgets are actually ON SCREEN.

    **This is the check the suite did not have, and its absence let UI
    Demo ship completely blank.** Every other check here asserts on the
    app's LOG: clicking a coordinate produced the right line, which it
    did -- the widgets were live, hit-testable and reporting correctly,
    and simply not painted. "It responds" is not "it is drawn"
    (docs/gui-guidelines.md), and this is the third time that exact gap
    has hidden a real defect in this project.

    What broke: the toolkit draws declared widgets before calling
    on_draw, and UI Demo's on_draw still began by clearing the surface,
    so it painted over all of them. uapp.c documents that trap in its
    own comment; the test suite could not see it.

    Asserted per ROW rather than over the whole window, because a single
    total is satisfied by one widget drawing and the rest not -- which
    is a more likely bug than everything vanishing at once.
    """
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, "uidemo_drawn.png"))
    qmp.screenshot(path)
    im = Image.open(path).convert("RGB")

    c = d.win["content"]
    bg = im.getpixel((c["x"] + c["w"] - 3, c["y"] + c["h"] - 3))  # a corner the
                                                                  # widgets leave alone

    def ink(name):
        x, y, w, h = d.layout[name]
        n = 0
        for j in range(c["y"] + y, c["y"] + y + h):
            for i in range(c["x"] + x, c["x"] + x + w):
                if im.getpixel((i, j)) != bg:
                    n += 1
        return n

    for name in ("btn1", "chk_alpha", "radio", "textbox", "dropdown",
                 "listbox", "scrollback"):
        n = ink(name)
        d._record(f"{name} is actually drawn", n > 20, [],
                  f"only {n} non-background pixels in its own rect")


def check_focus_is_visible(d, qmp, tmp):
    """A focused widget SAYS SO on screen -- the pixel half of the Tab checks.

    Every focus check above asserts on the app's log: Tab moved focus to
    the listbox, and it did. For a long time that was the whole story
    and the listbox drew nothing to say it -- Tab moved an invisible
    cursor, and the first thing typed went somewhere the user did not
    choose (docs/roadmap.md's focus-indicator item). "It responds" is
    not "it is drawn", for the fourth time in this project.

    Asserted as ACCENT pixels inside the widget's own rect, BOTH WAYS:
    present with the listbox focused and absent with focus moved away.
    A one-sided check passes on a widget that rings itself
    unconditionally, which is a control lying about holding the keyboard
    rather than one saying nothing.

    The counterpart at the widget level is /tests/focusring_test, which
    covers all eleven of them off-screen; what this adds is that the
    ring survives the compositor and reaches the glass.

    POSITIVE CONTROL: make uui_focus_ring() return immediately. The
    first check goes red; **the second stays GREEN**, which is the
    asymmetry worth knowing -- an absence check cannot catch a ring that
    never draws, exactly as this file's scrollbar control recorded.
    """
    from PIL import Image
    c = d.win["content"]
    x, y, w, h = d.layout["listbox"]

    def accent():
        path = os.path.abspath(os.path.join(tmp, "uidemo_focus.png"))
        qmp.screenshot(path)
        im = Image.open(path).convert("RGB")
        n = 0
        for j in range(c["y"] + y, c["y"] + y + h):
            for i in range(c["x"] + x, c["x"] + x + w):
                if im.getpixel((i, j)) == UTHEME_ACCENT_RGB:
                    n += 1
        return n

    # Driven by CLICKING, the way the focus checks above are: there is
    # no command that sets focus, and a check should reach the widget by
    # the path a person would.
    d.click(*d.list_row(0))
    lit = accent()
    d.click(*d.textbox_center())
    dark = accent()

    d._record("a focused listbox is ringed in the accent", lit > 0, [],
              "no accent pixels in the listbox's rect while it had focus")
    d._record("...and an unfocused one is not", dark == 0, [],
              f"{dark} accent pixel(s) still there after focus moved away")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    ap.add_argument("--shot", metavar="DIR",
                    help="also write uidemo-widgets.png / uidemo-dropdown-open.png here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "uidemo_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    d = Demo(dbg, verbose=args.verbose)
    d.open()
    print(f"uidemo_test: layout {d.layout}, row_h {d.row_h}")
    run(d, qmp)

    print("\n== containment ==")
    check_widgets_drawn(d, qmp, "/tmp")
    check_focus_is_visible(d, qmp, "/tmp")
    check_containment(d, qmp, "/tmp")

    if args.shot:
        if qmp is None:
            qmp = QMPSession(port=args.qmp_port)
        d.click(*d.dropdown_center())  # leave the popup open for the shot
        d.events()
        qmp.screenshot(os.path.abspath(os.path.join(args.shot, "uidemo-dropdown-open.png")))
        d.key(K_ESC)
        d.events()
        qmp.screenshot(os.path.abspath(os.path.join(args.shot, "uidemo-widgets.png")))

    print(f"\nuidemo_test: {len(d.passes)} passed, {len(d.fails)} failed")
    for f in d.fails:
        print("  FAILED:", f)
    dbg.close()
    return 1 if d.fails else 0


if __name__ == "__main__":
    sys.exit(main())
