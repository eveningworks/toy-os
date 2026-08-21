#!/usr/bin/env python3
"""Runtime fonts: loading a TTF from disk, switching faces live, and
proportional advance widths actually reaching the screen.

The glyphs the desktop draws with come from one of two places: the
tables tools/genttf.py baked into the kernel image, or a .ttf under
/usr/share/fonts rasterized at runtime (kernel/lib/ttf.c). Switching
between them is a setting, and WIN_EV_FONT tells every client its cached
metrics went stale.

WHAT THIS TEST IS SHAPED AROUND. "Text is on screen" proves nothing --
the baked font is a complete, working fallback, so a rasterizer that
produced garbage, or a face switch that silently did nothing, leaves a
perfectly readable desktop behind. Every check here is therefore a
DIFFERENCE between two states, and the load-bearing one is that
liberation-sans (proportional) draws the same labels NARROWER than
dejavu-sans-mono (monospace) does. A build that ignored per-glyph
advances would still render both faces, still switch between them, and
still fail that one check -- which is the point.

The measurement is the rightmost ink in the desktop's icon-label column.
It is a width, not a pixel count: a face with heavier stems has more ink
at the same width, so counting ink would mostly measure boldness.

Usage (the VM must already be up):
    python3 tools/vm.py start
    python3 tools/font_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
MONO = "dejavu-sans-mono"
PROP = "liberation-sans"

# THE VERSION TEXT, bottom right: two lines of ~40 characters, drawn
# RIGHT-ALIGNED against the screen edge. That last part is what makes it
# the right ruler here -- a narrower font starts further right, so its
# LEFTMOST ink moves by the whole difference in string width, tens of
# pixels rather than the two or three the desktop icon captions would
# show (those are clipped to the icon cell, so a narrower font mostly
# just un-truncates them).
TEXT_X0, TEXT_X1 = 700, 1280
TEXT_Y0, TEXT_Y1 = 655, 690
BG = (24, 60, 90)

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def text_left(qmp, tag):
    """Leftmost inked column of the right-aligned version text, and ink.

    Uses a SETTLED frame: a face change is a client-side repaint two
    process hops away from the setting being written, and a capture
    landing mid-paint compares two half-drawn desktops.
    """
    path = os.path.join(tempfile.gettempdir(), f"font_{tag}.png")
    qmp.stable_pixels(path, box=(TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1))
    # stable_pixels() writes the WHOLE screen and only COMPARES the box,
    # so the crop has to happen here. Scanning the full frame instead
    # counts the taskbar and every icon as "ink" and the measurement
    # stops meaning anything -- it read 53,968 in a 20,300-pixel band.
    im = Image.open(path).convert("RGB").crop((TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1))
    w, h = im.size
    left, ink = w, 0
    for y in range(h):
        for x in range(w):
            if im.getpixel((x, y)) != BG:
                ink += 1
                if x < left:
                    left = x
    return TEXT_X0 + left, ink


def set_face(dbg, name):
    dbg.send(f"gui spawn /bin/config set system.font_face {name}")
    time.sleep(1.2)


def set_size(dbg, px):
    dbg.send(f"gui spawn /bin/config set system.font_size {px}")
    time.sleep(1.2)


def wait_log(dbg, needle, timeout=8.0):
    """The most recent COMPOSITOR log line containing `needle`, waited for.

    Note what this can and cannot see: DebugConsole.logs() is the
    compositor's own buffer, so `wm: font changed -- WxH cell` is here
    and the kernel's `font: <face> at <n>px` line (which goes to klog)
    is NOT. That turns out to be the better source anyway -- the cell the
    CLIENT ended up with is the thing under test, and reading the
    kernel's intention would prove one hop less.

    A POLL, not a sleep. The setting is written by a short-lived ring-3
    process, applied in the kernel and logged there, and the client
    repaint that follows is a third hop -- so "how long does that take"
    is a load-dependent question with no good fixed answer. Reads
    without clearing so several waits can look at the same buffer.
    """
    deadline = time.time() + timeout
    while True:
        lines = [l for l in dbg.logs(clear=False) if needle in l]
        if lines:
            return lines[-1].strip()
        if time.time() > deadline:
            return ""
        time.sleep(0.3)



def demo_report(dbg, timeout=10.0):
    """The Font Demo app's own measurements, as a dict of its log lines.

    WHY ASK THE APP RATHER THAN READ PIXELS. Bold, kerning and a private
    face are all things a screenshot shows and a screenshot cannot
    MEASURE: "that looks heavier" and "that looks tighter" are exactly
    the judgements that let a fallback-to-regular ship. The app measures
    with the same ugfx_text_width() every widget lays out with, and
    prints the numbers -- so what is asserted here is the number a real
    layout would have used, not a human reading of a PNG.

    Its lines go to stderr, which reaches the kernel log, so they arrive
    through logs() like the compositor's own.
    """
    out = {}
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in dbg.logs(clear=False):
            if "fontdemo:" not in line:
                continue
            body = line.split("fontdemo:", 1)[1].strip()
            words = body.split()
            if not words:
                continue
            # TWO words, not one: "session regular ..." and "session bold
            # ..." are different measurements that both start with
            # "session", and keying on the first word alone silently made
            # the second overwrite the first -- so the bold line was read
            # as the regular one and the bold check could never pass.
            key = " ".join(words[:2]) if words[0] == "session" else words[0]
            out[key] = body
        # `private` is logged last, so its presence means the whole
        # report landed -- waiting on the FIRST line and then requiring
        # all of them is the flake docs/testing.md warns about.
        if "private" in out:
            return out
        time.sleep(0.3)
    return out


def check_demo_pixels(dbg, qmp):
    """The demo's text is drawn on its own background, not onto a void.

    THE CHECK THAT WAS MISSING, and worth having for what it says about
    the rest of this file: every other assertion in the second half reads
    numbers the app measured out of the mapped atlas -- widths, advances,
    ink counts -- and all of them passed while the demo rendered every
    letter as a hollow outline. The atlas was perfect; the app drew onto
    a surface it had never cleared. "It measures" is not "it is drawn".

    WHY THE DOMINANT COLOUR AND NOT AN INK COUNT. ugfx_draw_char() skips
    fully-background pixels, so a `bg` argument fills nothing -- it is
    only the colour the antialiased rim is blended toward. On an uncleared
    (black) surface the glyph interiors are painted near-black on black
    and vanish, leaving the rim. The tempting probe is "mostly solid ink
    rather than mostly edge pixels", and it is WRONG IN BOTH DIRECTIONS:
    it was written here first, and the black background counted as 95,136
    'solid' pixels, so the broken build passed by a wide margin. Measured,
    not reasoned about -- see CLAUDE.md on thresholds picked without a
    control.

    What actually separates the two states is what MOST of the window is:
    the theme's panel background when the app cleared, near-black when it
    did not.
    """
    win = dbg.window("Font Demo")
    if not win:
        check("the Font Demo window is on screen for a pixel check", False)
        return
    c = win["content"]
    x, y, w, h = c["x"], c["y"], c["w"], c["h"]

    path = os.path.join(tempfile.gettempdir(), "font_demo.png")
    qmp.stable_pixels(path, box=(x, y, x + w, y + h))
    im = Image.open(path).convert("RGB").crop((x, y, x + w, y + h))

    counts = {}
    for py in range(im.height):
        for px in range(im.width):
            p = im.getpixel((px, py))
            counts[p] = counts.get(p, 0) + 1
    dominant, dom_n = max(counts.items(), key=lambda kv: kv[1])
    dom_lum = sum(dominant) // 3
    total = im.width * im.height

    check("the demo cleared its surface -- text sits on a background",
          dom_lum > 200, f"dominant={dominant} ({dom_n}/{total})")

    # ...and there IS text on it. Dark pixels, but a small minority: a
    # window that is mostly ink is a filled rect, not a page of labels.
    ink = sum(n for p, n in counts.items() if sum(p) // 3 < 60)
    check("...and there is text drawn on it",
          200 < ink < total // 4, f"ink={ink}/{total}")


def check_weights_and_kerning(dbg, qmp):
    """Bold beside regular, kerning applied, and a private face, in ring 3."""
    dbg.open_app("Font Demo")
    dbg.settle()
    check_demo_pixels(dbg, qmp)
    rep = demo_report(dbg)

    check("Font Demo reported its measurements",
          "session regular" in rep and "private" in rep, str(sorted(rep)))
    if not rep:
        return

    # --- bold ---------------------------------------------------------
    #
    # THE CHECK A FALLBACK CANNOT PASS. A bold mapping that failed, or
    # one the server answered with the regular atlas, measures the
    # string at exactly the regular width. Both a designed bold and a
    # smeared one are wider, because both widen advances to match their
    # thicker strokes.
    bold = rep.get("session bold", "")
    # MEASURED IN INK BY THE APP, not in width -- on a monospace face a
    # designed bold has exactly the regular advances, so a width probe
    # reports a working bold as a fallback. See fontdemo.c's glyph_ink().
    check("the bold weight is distinct from regular, not a fallback",
          bold.endswith("distinct 1"), bold)

    # --- kerning ------------------------------------------------------
    #
    # `plain` is ugfx_text_width(), which kerns; `unkerned` is the sum of
    # the same characters' advances, which cannot. On a MONOSPACE face
    # they are equal and this proves nothing, which is why the caller
    # switches to the proportional face first -- and why the difference
    # is required to be several pixels rather than merely non-zero.
    kern = rep.get("kern", "")
    parts = kern.split()
    if len(parts) >= 5:
        plain = int(parts[-3])
        unkerned = int(parts[-1])
        check("kerning tightens the sample rather than loosening it",
              plain < unkerned, f"plain={plain} unkerned={unkerned}")
        check("the kerning is a real amount, not a rounding artefact",
              unkerned - plain >= 4, f"delta={unkerned - plain}")
    else:
        check("kerning was measured", False, kern)

    # --- the private tier ---------------------------------------------
    #
    # A 24px face this app rasterized ITSELF, while the session sits at
    # 14px. Asserting it is TALLER than the session font is what proves
    # the private atlas is actually being drawn from: a private load
    # that silently failed, or a ugfx_set_font() that did nothing, would
    # report the session's own metrics.
    # --- descenders are not clipped -----------------------------------
    #
    # A 'g' with its tail cut flat is glaring in a screenshot and
    # INVISIBLE to every other check here: the glyph has plenty of ink
    # and the right advance, it is just missing its last rows. The app
    # reports the empty rows between its lowest ink and the bottom of
    # its cell, so 0 means the outline runs into the last row.
    #
    # This is only asserted for the PRIVATE font. The session font's
    # cell is deliberately squeezed (it is the layout grid -- see
    # docs/decisions.md), so it clips descenders on purpose and a check
    # here would be asserting the opposite of the design.
    desc = rep.get("descender", "")
    if desc:
        slack = int(desc.split("slack")[1].split()[0])
        check("the private font's descenders are not clipped",
              slack >= 1, desc)

    # The SESSION font's descenders too, now that its bitmap is taller
    # than its line. Only meaningful for a loaded face: `builtin`'s
    # bitmaps were rasterized squeezed at build time by genttf.py, so it
    # still clips and always will until those are regenerated.
    sdesc = rep.get("session-descender", "")
    if sdesc:
        reg = int(sdesc.split("regular")[1].split()[0])
        bold = int(sdesc.split("bold")[1].split()[0])
        check("the session font's descenders survive its line pitch",
              reg >= 1 and bold >= 1, sdesc)

    priv = rep.get("private", "")
    sess = rep.get("session regular", "")
    check("a face this app rasterized for itself loaded", "loaded" in priv, priv)
    if "loaded" in priv and sess:
        # Both lines end "<w>x<h>" but the private one has words after
        # it, so take the last WxH token rather than splitting the whole
        # line on "x" -- that read "13 distinct 1" as a number.
        priv_h = int([t for t in priv.split() if "x" in t][-1].split("x")[1])
        sess_h = int([t for t in sess.split() if "x" in t][-1].split("x")[1])
        check("the private font is at ITS size, not the session's",
              priv_h > sess_h, f"private {priv_h}px vs session {sess_h}px")


def check_boot_face_is_live(dbg):
    """A face selected at BOOT must actually be drawing.

    THE BUG THIS EXISTS FOR, and why it lives here rather than in a
    KTEST. font_face_select() only loads and validates a .ttf; the atlas
    that makes it drawable comes from gfx_set_font_px(). font_config_init()
    called the first and -- with no `font_size` key in /etc/toyos.conf --
    returned before the second. So `fontface` reported a face as active
    while every glyph came from the BAKED tables, and proportional
    advances, kerning and bold all silently fell back.

    It survived a full green suite because a missing size key is the
    state of a FRESHLY FORMATTED DISK and of no developer's image: the
    key persists once anything sets it, and every check in this file used
    to set a size before measuring anything. So this check runs FIRST,
    before set_face() or set_size() touch a thing.

    A KTEST cannot do this job: the font KTESTs that run before it build
    an atlas and restore it, so the invariant would already hold by the
    time one was asserted, and the test would pass on the broken build.
    """
    rep = demo_report(dbg)
    if not check("Font Demo reported before any setting was touched", bool(rep),
                 str(sorted(rep))):
        return
    # `distinct 1` needs a real face: the baked font has ONE weight, so a
    # machine that fell back to it reports bold and regular identical.
    # That makes this one line a test of the whole boot path.
    bold = rep.get("session bold", "")
    check("the face selected at boot is live, not just chosen",
          bold.endswith("distinct 1"), bold)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)

    dbg = DebugConsole(args.sock)
    print("runtime fonts")

    # --- FIRST, before anything is set ---------------------------------
    # This one is about what BOOT left behind, and every check below
    # repairs the state it looks for by setting a face and a size. It
    # only means anything on a fresh image, which is what
    # `make clean-disk && make iso` gives the suite.
    dbg.open_app("Font Demo")
    dbg.settle()
    check_boot_face_is_live(dbg)

    # --- the faces are on disk and one of them is in use ---------------
    # ESTABLISH the starting state rather than inherit it: font_face and
    # font_size both persist to /etc/toyos.conf, which survives a build,
    # so a previous run leaves its choice behind and every measurement
    # below is relative to a baseline that would otherwise be silently
    # wrong. Via PROP first so the switch to MONO is a real change --
    # setting a setting to the value it already holds does nothing, by
    # design, and would leave nothing to observe.
    set_size(dbg, 14)
    set_face(dbg, PROP)
    dbg.logs()  # drain, so every wait below sees only what IT caused
    set_face(dbg, MONO)
    mono_cell = wait_log(dbg, "wm: font changed")
    check("a face rasterizes from /usr/share/fonts and reaches the compositor",
          "cell" in mono_cell, mono_cell)

    mono_left, mono_ink = text_left(qmp, "mono")
    check("the desktop draws text with it", mono_ink > 200,
          f"left={mono_left} ink={mono_ink}")

    # --- a live face switch reaches the screen -------------------------
    # No restart: WIN_EV_FONT tells the compositor its metrics moved.
    dbg.logs()
    set_face(dbg, PROP)
    prop_cell = wait_log(dbg, "wm: font changed")
    check("switching face tells the compositor a new cell", "cell" in prop_cell,
          prop_cell)
    check("the two faces do not have the same cell", prop_cell != mono_cell,
          f"{mono_cell!r} -> {prop_cell!r}")

    prop_left, prop_ink = text_left(qmp, "prop")
    check("the switch reached the screen without a restart",
          prop_left != mono_left or abs(prop_ink - mono_ink) > 100,
          f"left {mono_left} -> {prop_left}, ink {mono_ink} -> {prop_ink}")

    # THE CHECK THIS TOOL EXISTS FOR. A proportional face must draw the
    # same captions in less width than a monospace one -- that is what
    # per-glyph advances DO. Ignore hmtx and every other check here still
    # passes.
    check("proportional text is narrower than monospace",
          prop_left > mono_left + 4,
          f"right-aligned text starts at {prop_left} vs {mono_left}")

    # --- an arbitrary size, which is only possible with a rasterizer ---
    dbg.logs()
    set_size(dbg, 13)
    size13 = wait_log(dbg, "wm: font changed")
    check("a size nobody baked is rasterized", "cell" in size13 and size13 != prop_cell,
          f"{prop_cell!r} -> {size13!r}")

    # --- and the baked font is still there -----------------------------
    set_size(dbg, 14)
    set_face(dbg, "builtin")
    # WAIT FOR THE COMPOSITOR TO SAY IT CHANGED, rather than sleeping.
    # This was a fixed 1.0s and it became a flake the moment a font
    # change got more expensive: every client now re-maps TWO atlases
    # (regular and bold) on WIN_EV_FONT instead of one, and the extra
    # round trip was enough to land the capture on the previous face --
    # which reads exactly like "switching to builtin did nothing".
    #
    # stable_pixels() below cannot save it: two identical reads of a
    # frame that has not started repainting are still identical. A
    # settled frame is not the same thing as the RIGHT frame.
    wait_log(dbg, "font changed")
    builtin_left, builtin_ink = text_left(qmp, "builtin")
    check("the baked font still draws when no face is selected",
          builtin_ink > 200, f"left={builtin_left} ink={builtin_ink}")
    check("the baked font is not the face we just left",
          builtin_left != prop_left or abs(builtin_ink - prop_ink) > 100,
          f"left {prop_left} -> {builtin_left}")

    # --- weights, kerning and a private face ---------------------------
    #
    # NOTE THE ORDER: check_boot_face_is_live() ran BEFORE any set_face()
    # or set_size() above, because it is about what BOOT left behind and
    # setting either would repair the state it is looking for.
    #
    # ON THE PROPORTIONAL FACE, deliberately: dejavu-sans-mono has no
    # `kern` table at all (it is monospace, so kerning it would be
    # wrong), and every kerning assertion would pass vacuously against
    # it -- equal is equal.
    set_face(dbg, PROP)
    set_size(dbg, 14)
    time.sleep(1.0)
    check_weights_and_kerning(dbg, qmp)

    # Put the machine back the way a fresh image boots.
    set_face(dbg, MONO)
    set_size(dbg, 14)

    passed = sum(1 for _, ok in checks if ok)
    failed = len(checks) - passed
    print(f"font_test: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
