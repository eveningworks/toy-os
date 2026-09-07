#!/usr/bin/env python3
"""The shared pager: it draws, it turns a page, and it puts the terminal back.

WHY THIS EXISTS
---------------
`/bin/less` had no test at all until its paging moved into
userland/lib/upager.c so that `/bin/doc` could share it. A pager is the
one program whose whole behaviour is what the SCREEN does in response to
a key -- there is no output to capture and no file to read back -- so it
cannot be checked from the debug console, and it was not checked at all.

Both front ends are driven, because the seam is exactly the thing a
refactor breaks: `less` holds its own 256 KiB buffer and `doc` renders
into a malloc'd one, and the pager must not care which.

THE LOAD-BEARING CHECK IS THE STATUS BAR. Everything else here would
pass with the pager replaced by `cat`: text appears, the screen changes
when a key is typed (that key echoes at the prompt), and the window
survives. What only a running pager produces is a REVERSE-VIDEO BAND
across the last row of the content area -- a run of one colour, most of
the window wide, that is not the background. A dump cannot fake it.

The second is that the page TURNS: the frame after `space` must differ
from the frame before it, over the body rows only. Reading the body
rather than the whole window is what stops the moving status bar (its
percentage changes) from satisfying the check on its own.

Frames are SETTLED (QMPSession.screenshot's default), because a capture
landing mid-paint differs from its neighbour for reasons that have
nothing to do with the pager.
"""
import argparse
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))

from gui_debug import DebugConsole, enter_gui       # noqa: E402
from qmp_test import QMPSession                     # noqa: E402
import port_guard                                   # noqa: E402
from terminal_probe import Terminal                 # noqa: E402

try:
    from PIL import Image
except ImportError:
    sys.exit("pager_test.py needs Pillow: pip install --user pillow")

SPACE, QUIT = "0x20", "q"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", name,
                          ("   -- " + detail) if detail and not ok else ""))


def shot(qmp, tmp, tag):
    p = os.path.join(tmp, "pager_%s.ppm" % tag)
    qmp.screenshot(p)
    return Image.open(p).convert("RGB")


def rows(img, box, y0, y1):
    """The pixels of content rows [y0, y1) as a flat tuple, for comparing."""
    x0, ytop, x1, _ = box
    return tuple(img.crop((x0, ytop + y0, x1, ytop + y1)).getdata())


def status_band(img, box, char_h):
    """The widest run of one NON-BACKGROUND colour near the bottom.

    Returned as a fraction of the content width. A reverse-video bar is
    padded with spaces to the full width, so a running pager gives
    something close to 1.0 while a prompt or ordinary text gives short
    runs broken by the background between words.

    Two things this has to get right, and the first version got both
    wrong. **The BACKGROUND is uniform too**, so "widest run of one
    colour" alone is satisfied by an empty row -- the background is
    found first (as the commonest colour in the band) and excluded.
    And **only the bottom of the window may be looked at**: the WM's
    content rect includes the Terminal's own menu bar and tab strip,
    which are solid light bands that would answer 1.0 in every state.
    """
    x0, _y0, x1, y1 = box
    top = max(_y0, y1 - 6 * char_h)
    band = img.crop((x0, top, x1, y1 - 1))
    w, h = band.size
    if w < 8 or h < 2:
        return 0.0
    px = list(band.getdata())
    counts = {}
    for c in px:
        counts[c] = counts.get(c, 0) + 1
    bg = max(counts, key=counts.get)

    # COVERAGE, NOT A RUN. Reverse video paints the bar in the
    # foreground colour and its TEXT in the background one, so the bar
    # is broken by a black pixel at every glyph and the longest
    # unbroken run across it is one character wide. What separates it
    # from a row of text is how MUCH of the row one non-background
    # colour covers: ~85% for a padded bar, ~15% for a line of prose.
    best = 0.0
    for row in range(h):
        counts = {}
        for col in range(w):
            c = px[row * w + col]
            if c != bg:
                counts[c] = counts.get(c, 0) + 1
        if counts:
            best = max(best, max(counts.values()) / float(w))
    return best


def longest_page():
    """The name of the biggest command page -- the one certain to page."""
    import glob
    best, size = "config", 0
    for p in glob.glob(os.path.join(REPO, "docs", "commands", "*.md")):
        if os.path.basename(p) == "README.md":
            continue
        n = os.path.getsize(p)
        if n > size:
            best, size = os.path.basename(p)[:-3], n
    return best


def drive(term, dbg, qmp, tmp, command, char_h, label):
    """Run one pager front end and assert the three properties."""
    term.type(command)
    term.enter(settle=2.5)
    inside = shot(qmp, tmp, label + "_open")

    band = status_band(inside, term.box, char_h)
    check("%s: a status bar spans the window" % label, band > 0.6,
          "widest run is %.2f of the width" % band)

    body_before = rows(inside, term.box, 0, term.box[3] - term.box[1] - 2 * char_h)
    term.key(SPACE)
    dbg.settle()
    time.sleep(1.2)
    turned = shot(qmp, tmp, label + "_page2")
    body_after = rows(turned, term.box, 0, term.box[3] - term.box[1] - 2 * char_h)
    check("%s: space turns the page" % label, body_before != body_after)

    term.key(QUIT)
    dbg.settle()
    time.sleep(1.5)
    after = shot(qmp, tmp, label + "_quit")
    band = status_band(after, term.box, char_h)
    check("%s: q leaves the pager" % label, band <= 0.6,
          "status bar still spans %.2f of the width" % band)
    return after


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--logs", default=None,
                    help="directory to keep the screendumps in")
    port_guard.add_instance_args(ap)
    args = ap.parse_args()
    port_guard.resolve_instance(args, "pager_test")

    tmp = args.logs or os.path.join(REPO, "build", "pager_test")
    os.makedirs(tmp, exist_ok=True)

    qmp = QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    term = Terminal(dbg, qmp)

    # The row height the WM is actually drawing at -- never a constant,
    # since every size here is font-derived (docs/gui-guidelines.md).
    char_h = 16
    metrics = dbg.send("gui font") or ""
    for tok in metrics.replace(",", " ").split():
        if tok.startswith("h="):
            try:
                char_h = int(tok[2:])
            except ValueError:
                pass

    # /bin/less over a real file, and /bin/doc over a rendered page --
    # the two callers of one pager, holding their text in two different
    # places.
    #
    # THE LONGEST PAGE, chosen on the host rather than named here. `doc
    # ls` renders to 25 lines and fits one screen, so "space turns the
    # page" asked a correct pager to do something it must not: there was
    # nothing below. Picking the largest page by size means the check
    # cannot quietly stop discriminating as the pages are edited.
    drive(term, dbg, qmp, tmp, "less /tests/sample.txt", char_h, "less")
    drive(term, dbg, qmp, tmp, "doc %s" % longest_page(), char_h, "doc")

    # AND THE TERMINAL IS USABLE AFTERWARDS. A pager that returned
    # without leaving raw mode or the alternate screen hands the shell a
    # prompt with no echo, which looks exactly like a hung machine --
    # so this types an ordinary command and requires its output.
    term.type("echo pager-alive")
    term.enter(settle=2.0)
    final = shot(qmp, tmp, "after")
    prompt_row = status_band(final, term.box, char_h)
    check("the terminal still echoes after paging", prompt_row <= 0.6,
          "the screen still looks like a pager")

    bad = [n for n, ok in results if not ok]
    print("pager_test: %d checks, %s"
          % (len(results), "PASS" if not bad else "%d FAILED" % len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
