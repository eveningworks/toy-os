#!/usr/bin/env python3
"""tools/scrollbar_test.py -- scrollbar BEHAVIOUR, against the ring-3 Notepad.

WHAT THIS COVERS
----------------
`docs/gui-guidelines.md`'s "Scrollbars: what a real one does" section,
which is the specification every real toolkit (Windows, KDE, macOS, GTK)
implements identically. Points 1, 2, 3 and 6 are asserted here:

  1. The thumb does not JUMP when grabbed. Pressing anywhere on it and
     moving N pixels moves the thumb N pixels -- which requires the
     press's offset within the thumb to be recorded and subtracted.
  2. Dragging is reversible: back by the same amount returns to the same
     place, with no accumulated drift.
  3. The trough PAGES and an arrow STEPS -- different zones, very
     different amounts.
  6. The strip is wide enough to hit with a mouse.

Point 1 is the reason this file exists. The ring-3 Notepad passed
`grab_offset_in_thumb = 0`, so the thumb leapt to put its TOP under the
cursor on the first motion event, and the bar was usable only by
catching its top edge exactly. Every other check in the suite passed the
whole time: the scrollbar DID scroll, it just scrolled to the wrong
place, which is `docs/gui-guidelines.md`'s "it responds is not it is
correct" in a new costume.

HOW IT MEASURES
---------------
By reading the THUMB'S PIXELS, not the text. Notepad's track and thumb
are known flat colours, so a column scan down the strip returns the
thumb's exact top and height -- a real number to assert on, with no OCR
and nothing re-derived in Python. The strip's rectangle comes from
Notepad's own `notepad: layout scrollbar x y w h` line, per this repo's
ask-the-app rule.

Scroll position is quantised to whole LINES, so every comparison carries
a tolerance of one line's worth of track plus a pixel. The tolerance is
computed, not guessed, and the two predictions a broken grab offset
chooses between are far further apart than it.

POSITIVE CONTROL
----------------
Pass 0 for `g_scrollbar_grab` in userland/gui/apps/notepad.c's on_motion().
Run when this was written: check 1 goes red ("thumb moved 16px, dragged
60px") and **everything else stays green**, including the
drag-back-and-return check -- the jump drives the thumb into the end of
the track, and coming back from a clamped position looks correct. So
check 1 is the only thing standing between this bug and a green suite;
don't weaken it. Do the control again before trusting a green run after
any scrollbar change.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui
from qmp_test import QMPSession
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/bin/wm/apps/notepad"   # spawned directly -- see run()
SPAWN_TIMEOUT_S = 20.0
ENTER = "0x0d"

# THE OVERLAY BAR (ui/uui_scrollbar.h's uui_scrollbar_draw_overlay):
# nothing but a thin thumb at rest, a groove and a full-width thumb
# under the pointer. The thumb is a blend toward the text ink, the
# groove only a faint one, the ground the document's white -- so "a
# thumb pixel" is anything darker than THUMB_MAX.
THUMB_MAX = 190

# Enough lines to give the thumb real travel: ~20 rows are visible, so
# this leaves a thumb about a fifth of the track with the rest to move
# through. Blank lines are fine -- what is measured is the thumb.
LINES = 100

# The ergonomic floor from the guidelines. char_w + 6 is 14 at the
# default 14pt font; anything at or below the old hardcoded 8 fails.
MIN_STRIP_W = 12
MIN_THUMB_W = 8


Result = Results


def near(a, b, tol):
    return abs(a - b) <= tol


class Bar:
    """The scrollbar's screen rectangle, as Notepad reports it."""

    def __init__(self, content, layout):
        self.x = content["x"] + layout[0]
        self.y = content["y"] + layout[1]
        self.w = layout[2]
        self.h = layout[3]
        # No arrows: the whole strip is the track.
        self.track_y = self.y
        self.track_h = self.h

    @property
    def cx(self):
        # The overlay thumb hugs the strip's far edge, so its middle is
        # there rather than at the strip's centre.
        return self.x + self.w - 6


def shot(qmp, tmp, name):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def is_thumb(px):
    return max(px) <= THUMB_MAX


def thumb_extent(im, bar, x=None):
    """(top, height) of the thumb, by scanning the strip's centre column.

    Returns (None, 0) if no thumb is drawn -- which is itself a real
    answer: it means the content fits, or nothing was painted.
    """
    top, run, best = None, 0, (None, 0)
    for y in range(bar.track_y, bar.track_y + bar.track_h):
        px = im.getpixel((bar.cx if x is None else x, y))
        if is_thumb(px):
            if top is None:
                top = y
            run += 1
        else:
            if run > best[1]:
                best = (top, run)
            top, run = None, 0
    if run > best[1]:
        best = (top, run)
    return best


def thumb_width(im, bar, thumb_y):
    """How many pixels wide the thumb actually is, on its own row."""
    n = 0
    for x in range(bar.x, bar.x + bar.w):
        if is_thumb(im.getpixel((x, thumb_y))):
            n += 1
    return n


def find_notepad(dbg):
    raw = dbg.json("gui windows --json")
    for w in raw.get("windows", []):
        t = w.get("title", "").lstrip("*")
        if t.rsplit("/", 1)[-1] in ("untitled", "notepad"):
            return w
    return None


def read_layout(dbg):
    """The last `notepad: layout scrollbar x y w h` line."""
    for line in reversed(dbg.logs("notepad: layout scrollbar", clear=False)):
        parts = line.split("notepad: layout scrollbar", 1)[1].split()
        if len(parts) >= 4:
            return [int(v) for v in parts[:4]]
    return None


def run(dbg, qmp, tmp, res):
    # `gui spawn`, not a Terminal typing `run notepad`: the kernel-space
    # Terminal retired in M41's stage 0, and the ring-3 one has no window
    # yet when the injected keys would arrive.
    dbg.send(f"gui spawn {SPAWN_PATH}")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = find_notepad(dbg)
        if win:
            break
        time.sleep(0.3)
    res.check("Notepad runs as a ring-3 client with its own window", win is not None)
    if not win:
        return

    # Make it scrollable. Newlines only: what is being measured is the
    # thumb, and 100 keystrokes beats 400.
    for i in range(LINES):
        dbg.send(f"gui key {ENTER}")
        if i % 20 == 19:
            dbg.settle()
    dbg.settle()
    time.sleep(0.5)

    win = find_notepad(dbg)
    layout = read_layout(dbg)
    if not win or not layout:
        res.check("Notepad reports its scrollbar's rectangle", False,
                  f"layout line: {layout}")
        return
    bar = Bar(win["content"], layout)

    # AT REST it is an OVERLAY: a thin thumb on the strip's right edge and
    # nothing at its centre. The pointer is parked off the strip first.
    dbg.warp_cursor(qmp, bar.x - 60, bar.y + bar.h // 2)
    time.sleep(0.4)
    im = shot(qmp, tmp, "sb_rest.png")
    rest_edge, _ = thumb_extent(im, bar, x=bar.x + bar.w - 2)
    rest_mid, _ = thumb_extent(im, bar, x=bar.x + bar.w // 2)
    res.check("at rest the bar is a thin thumb on its edge (an overlay bar)",
              rest_edge is not None and rest_mid is None,
              f"edge run at {rest_edge}, centre run at {rest_mid}")

    # UNDER THE POINTER it widens. The pointer sits on the strip at the
    # end AWAY from the thumb, so its sprite cannot split the thumb's run.
    def hover_away(top, h):
        far = bar.y + 2 if (top or 0) + h // 2 > bar.y + bar.h // 2 else bar.y + bar.h - 2
        dbg.warp_cursor(qmp, bar.x + 1, far)
        time.sleep(0.4)

    hover_away(rest_edge, 20)
    im = shot(qmp, tmp, "sb_start.png")
    t0, th0 = thumb_extent(im, bar)
    if t0 is None:
        res.check("the scrollbar shows a thumb once the text overflows", False,
                  "no thumb-coloured run in the track")
        return

    # 6. Wide enough to hit. Both the reported strip and the thumb's own
    #    painted pixels, because a wide strip with a hairline thumb is
    #    the same problem.
    tw = thumb_width(im, bar, t0 + th0 // 2)
    res.check("the scrollbar is wide enough to hit with a mouse",
              bar.w >= MIN_STRIP_W and tw >= MIN_THUMB_W,
              f"strip {bar.w}px (want >= {MIN_STRIP_W}), "
              f"painted thumb {tw}px (want >= {MIN_THUMB_W})")

    # One line's worth of track, which is how coarsely the thumb can be
    # positioned at all. Derived from the geometry, not guessed.
    travel = bar.track_h - th0
    max_scroll = max(LINES - 1, 1)
    tol = max(2, travel // max_scroll + 2)

    # 1. THE CHECK THIS FILE EXISTS FOR. Grab the thumb near its BOTTOM
    #    and drag up by a known amount. A correct bar moves the thumb by
    #    the drag distance. One that ignores the grab offset moves it by
    #    the drag distance PLUS the grab offset, putting the thumb's top
    #    under the cursor -- the two predictions are ~grab apart, which
    #    is far outside `tol`.
    grab = max(4, th0 - 6)          # near the bottom of the thumb
    dist = min(60, travel // 2)     # up, so there is room in both directions
    press_y = t0 + grab
    dbg.drag(bar.cx, press_y, bar.cx, press_y - dist)
    dbg.settle()
    time.sleep(0.4)

    hover_away(press_y - dist, th0)
    im = shot(qmp, tmp, "sb_dragged.png")
    t1, th1 = thumb_extent(im, bar)
    moved = (t0 - t1) if t1 is not None else None
    res.check("grabbing the thumb anywhere moves it by the drag distance, not to the cursor",
              t1 is not None and near(moved, dist, tol),
              f"thumb moved {moved}px, dragged {dist}px (tol {tol}); "
              f"a jump-to-cursor bar would move {dist + grab}px")

    # 2. Reversible: drag back the same distance, land back where it
    #    started. Drift shows up here and nowhere else.
    if t1 is not None:
        press_y = t1 + grab
        dbg.drag(bar.cx, press_y, bar.cx, press_y + dist)
        dbg.settle()
        time.sleep(0.4)
        hover_away(t1 + dist, th0)
        im = shot(qmp, tmp, "sb_back.png")
        t2, _ = thumb_extent(im, bar)
        res.check("dragging back the same distance returns the thumb to where it was",
                  t2 is not None and near(t2, t0, tol),
                  f"thumb at {t2}, started at {t0} (tol {tol})")

    # 3. The trough PAGES: a click beside the thumb moves the view by a
    #    screenful, many lines -- not one, and not to the click.
    hover_away(t0, th0)
    im = shot(qmp, tmp, "sb_zones_a.png")
    base, base_h = thumb_extent(im, bar)
    if base is None:
        res.check("a trough click pages", False, "no thumb to measure from")
        return
    room_below = base + base_h + 8 < bar.track_y + bar.track_h
    if room_below:
        trough_y = base + base_h + 6
    elif base - 8 > bar.track_y:
        trough_y = base - 6
    else:
        res.check("a trough click pages", False,
                  f"thumb fills the track (top {base}, h {base_h})")
        return
    dbg.click(bar.cx, trough_y)
    dbg.settle()
    time.sleep(0.4)
    hover_away(base, base_h)
    im = shot(qmp, tmp, "sb_paged.png")
    paged, _ = thumb_extent(im, bar)
    page_delta = abs(paged - base) if paged is not None else 0
    line_px = max(1, travel / max_scroll)
    res.check("a trough click pages (several lines at once)",
              page_delta > 4 * line_px,
              f"trough moved {page_delta}px, one line is {line_px:.1f}px")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "scrollbar_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    res = Result()
    with DebugConsole(args.sock) as dbg:
        dbg.settle()
        run(dbg, qmp, args.tmp, res)

    n_ok, n_bad = len(res.passes), len(res.fails)
    print(f"\nscrollbar_test: {n_ok} passed, {n_bad} failed")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
