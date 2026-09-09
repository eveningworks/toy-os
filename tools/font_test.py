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
import re
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402

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
# The rows just above the taskbar, where desktop.c draws the version
# text. TEXT_Y1 is the taskbar's top, set from the guest in main().
#
# **THE BAND IS ONE LINE, AND THAT IS THE WHOLE POINT.** It was 35 rows,
# which at any of these font sizes is TWO AND A HALF lines -- so it
# swallowed the version text's FIRST line, which is longer than the
# second and made of different characters. "Leftmost ink" then measured
# that line rather than the one being compared, and a proportional face
# that is visibly NARROWER on screen read as wider. Confirmed by eye on
# real hardware before the band was believed.
TEXT_Y0, TEXT_Y1 = 655, 690
LINE_H = 16          # replaced per measurement -- see set_band()
BG = (24, 60, 90)

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))


def set_band(line_h):
    """The last line of the version text, and nothing else.

    A line above it is LONGER and made of different characters, so
    including it measures the wrong string -- see TEXT_Y0's comment. Two
    rows of margin below, to stay off the taskbar's top edge.
    """
    global TEXT_Y0, LINE_H
    LINE_H = max(8, int(line_h))
    TEXT_Y0 = TEXT_Y1 - 2 - LINE_H


def cell_h_from(log_line):
    """The line pitch out of `wm: font changed -- WxH cell`, or None."""
    m = re.search(r"--\s*(\d+)x(\d+)\s*cell", log_line or "")
    return int(m.group(2)) if m else None


def _text_left_once(qmp, tag):
    """One measurement of the right-aligned version text.

    Uses a SETTLED frame: a face change is a client-side repaint several
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


def text_left(qmp, tag, differs_from=None, timeout=8.0):
    """...and WAIT for it to actually change when it is supposed to.

    **SETTLED IS NOT CAUGHT UP.** The font now arrives from /bin/fontd,
    which notices the setting on its own poll and republishes; the
    compositor and every client then re-map on the next frame. So the
    desktop can be perfectly stable while still showing the PREVIOUS
    face, and two identical reads of a screen that has not repainted yet
    are as identical as any other.

    Measured without this, each capture returned the face before the one
    it was labelled with -- so a narrower proportional font read as
    WIDER, which is the opposite of what is on screen. Waiting on the
    observable is the fix; the frame being stable is not the observable.
    """
    deadline = time.time() + timeout
    last = _text_left_once(qmp, tag)
    if differs_from is None:
        return last
    while time.time() < deadline and last[0] == differs_from:
        time.sleep(0.2)
        last = _text_left_once(qmp, tag)
    return last


def set_font_setting(dbg, key, value, timeout=8.0):
    """Change a font setting, and WAIT for the compositor to say so.

    It slept 1.2s instead, and that is a race rather than a slow path:
    the setting is written by a short-lived ring-3 process, applied in
    the kernel, and only then does the client repaint and log its new
    cell -- three hops whose total is load-dependent. When the
    ESTABLISHING switch's line landed after the 1.2s, it survived the
    drain that follows and the next wait_log() returned the PREVIOUS
    face's cell, so two genuinely different faces compared equal. Seen
    3 runs in 5 once the desktop had a little more repainting to do.

    Waits for a font-changed line that is NEW since the spawn, and
    returns it. Setting the face it already holds does nothing, by
    design, so nothing is logged and this falls back to the timeout --
    which is why callers establish a state they know differs.
    """
    needle = "wm: font changed"
    before = len([l for l in dbg.logs(clear=False) if needle in l])
    dbg.send(f"gui spawn /bin/config set {key} {value}")
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = [l for l in dbg.logs(clear=False) if needle in l]
        if len(lines) > before:
            return lines[-1].strip()
        time.sleep(0.2)
    return ""


def set_face(dbg, name, timeout=8.0):
    return set_font_setting(dbg, "system.font_face", name, timeout)


def set_size(dbg, px, timeout=8.0):
    # SAME WAIT AS set_face, and for a sharper reason than tidiness: a
    # size change still in flight is a font-changed line that lands
    # inside the NEXT call's wait, which then returns having observed
    # somebody else's change. `set_size(14); set_face("builtin")` failed
    # exactly that way -- the capture measured the previous face and
    # read as "switching to builtin did nothing".
    return set_font_setting(dbg, "system.font_size", px, timeout)


def set_setting(dbg, key, value):
    dbg.send(f"gui spawn /bin/config set {key} {value}")
    time.sleep(0.8)


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
            # TWO words, not one, for "session ..." and "preview ...":
            # "session regular"/"session bold" and "preview face"/"preview
            # size" are different measurements sharing a first word, and
            # keying on it alone silently made the second overwrite the
            # first -- so the bold line was read as the regular one and
            # the bold check could never pass.
            key = " ".join(words[:2]) if words[0] in ("session", "preview") else words[0]
            out[key] = body
        # `preview face` is logged last (after the per-size loads), so its
        # presence means the whole report landed -- waiting on the FIRST
        # line and then requiring all of them is the flake docs/testing.md
        # warns about.
        if "preview face" in out:
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


def _preview_text_w(face_line):
    """The `text-w <n>` width from a `preview face ...` line, or 0."""
    if "text-w" not in face_line:
        return 0
    try:
        return int(face_line.split("text-w", 1)[1].split()[0])
    except (IndexError, ValueError):
        return 0


def _family_rect(dbg):
    """The Font Demo's family-dropdown rect (content-relative), or None --
    logged by uapp_log_layout() as `fontdemo: layout family x y w h`, so
    the click is a lookup not a pixel guess."""
    for line in reversed(dbg.logs("fontdemo: layout ", clear=False)):
        parts = line.split("fontdemo: layout ", 1)[1].split()
        if len(parts) >= 5 and parts[0] == "family":
            return tuple(int(v) for v in parts[1:5])
    return None


def check_weights_and_kerning(dbg, qmp):
    """Bold vs regular and kerning (session font), plus the previewer:
    the sample loads on open and picking a different family changes it."""
    dbg.open_app("Font Demo")
    dbg.settle()
    check_demo_pixels(dbg, qmp)
    rep = demo_report(dbg)

    check("Font Demo reported its measurements",
          "session regular" in rep and "preview face" in rep, str(sorted(rep)))
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

    # The SESSION font's descenders survive its line pitch, now that its
    # bitmap is taller than its line. Only meaningful for a loaded face:
    # `builtin`'s bitmaps were rasterized squeezed at build time by
    # genttf.py, so it still clips and always will until those are
    # regenerated.
    sdesc = rep.get("session-descender", "")
    if sdesc:
        reg = int(sdesc.split("regular")[1].split()[0])
        bold = int(sdesc.split("bold")[1].split()[0])
        check("the session font's descenders survive its line pitch",
              reg >= 1 and bold >= 1, sdesc)

    # --- the previewer, and the interactive check that earns its name -
    #
    # The ladder loads on open, and picking a DIFFERENT family actually
    # rasterises a different face -- proven by the pangram's WIDTH
    # changing, a metric no relabelled control could move. This is the
    # half a static demo cannot have.
    face0 = rep.get("preview face", "")
    tw0 = _preview_text_w(face0)
    check("the previewer loaded a face and measured the sample",
          '"Liberation Sans"' in face0 and tw0 > 0, face0)

    loaded = [l for l in dbg.logs("fontdemo: preview size", clear=False)
              if l.rstrip().endswith("loaded")]
    check("the size ladder rasterised (all rungs loaded)",
          len(loaded) >= 3, f"{len(loaded)} rungs loaded")

    fam = _family_rect(dbg)
    win = dbg.window("Font Demo")
    if fam and win:
        cx, cy = win["content"]["x"], win["content"]["y"]
        fx, fy, fw, fh = fam
        dbg.logs("fontdemo: preview face")   # clear: demo_report waits for the NEW one
        dbg.send("gui click %d %d" % (cx + fx + fw // 2, cy + fy + fh // 2))   # open the popup
        dbg.settle()
        # The popup lists items below the box, item N centred at
        # box_bottom + N*box_h + box_h/2. Item 1 is DejaVu Sans Mono.
        dbg.send("gui click %d %d" % (cx + fx + fw // 2, cy + fy + 2 * fh + fh // 2))
        dbg.settle()
        rep2 = demo_report(dbg)
        face1 = rep2.get("preview face", "")
        tw1 = _preview_text_w(face1)
        # A DIFFERENT family with a DIFFERENT width -- which specific one
        # the popup row landed on does not matter; that the face changed
        # and the metrics moved with it is the whole point.
        check("selecting a different family renders a genuinely different face",
              '"Liberation Sans"' not in face1 and tw1 > 0 and tw1 != tw0,
              f"was {tw0}px ({face0}), now {tw1}px ({face1})")
    else:
        check("the Font Demo reported its dropdown geometry", False, str(fam))


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



_probe_seq = [0]


def font_glyph(dbg, arg, flags=""):
    """`/bin/font glyph <arg>` output, as one string.

    THROUGH A FILE, and not because that is tidy. A spawned process's
    stdout goes to the console it inherited, so `gui spawn` hands the
    debug console back its own "spawned as pid N" line and nothing
    else -- a probe reading that would assert against the spawn message
    rather than the program, and would pass on any output at all.
    Running it under `tosh -c` with a redirect puts the output somewhere
    that can be read back deterministically.

    It also has to be a real SCHEDULED process, which is the client
    half's requirement: SYS_WIN_REQUEST refuses a caller with no
    scheduler slot, so the same command typed at the kernel `#` prompt
    (the legacy `run` loader) can only ever show the ring-0 view.

    The path is unique per call. A shared one read back after a spawn
    that failed would return the PREVIOUS call's output, which is the
    quietest way a probe like this can lie.
    """
    _probe_seq[0] += 1
    path = f"/fontprobe{_probe_seq[0]}.txt"
    dbg.send(f"gui spawn /bin/tosh -c font glyph {arg} {flags} > {path}".strip())
    # The spawn is asynchronous and the file does not exist until the
    # child has run. Polled on the ARTIFACT rather than slept on, so a
    # slow guest costs time instead of a flake.
    deadline = time.time() + 8
    out = ""
    while time.time() < deadline:
        time.sleep(0.4)
        out = dbg.send(f"sh cat {path}") or ""
        if "slot" in out or "font:" in out:
            break
    dbg.send(f"sh rm {path}")
    return out


def check_glyph_probe(dbg):
    """/bin/font: the two views, and the blank-glyph case it exists for."""
    g = font_glyph(dbg, "g")

    # 'g' is slot 71 -- ASCII 32..126 is contiguous from 0, so
    # 0x67 - 32 = 71. Pinned as a NUMBER because the bug that produced
    # this command printed the codepoint here instead (a %X that
    # consumed no argument, so the following %d read the wrong slot).
    check("`font glyph g` names the right atlas slot",
          "slot 71" in g, g.splitlines()[0][:60] if g else "no output")

    # BOTH VIEWS PRESENT, which is the thing that makes the hash line
    # mean anything -- a run where the client half quietly failed would
    # still print a kernel block and look healthy.
    check("...and reports the kernel view", "kernel " in g)
    check("...and the client's own mapping of the same atlas",
          "client   cell" in g,
          "client half missing -- no compositor?" if g else "no output")
    check("...and the two agree byte for byte",
          "agree" in g and "DISAGREE" not in g,
          next((ln.strip() for ln in g.splitlines() if "hash" in ln), "no hash line"))

    # 'g' has a DESCENDER, so its ink reaches below the line box. That
    # is ordinary rather than a defect (font_face.h), and it is the one
    # glyph property here that a wrong cell_h/line_h split would hide.
    check("...and notices that 'g' paints below its line",
          "paints below its line" in g)

    # THE DISCRIMINATING PAIR. A command that always said "ink yes"
    # passes every check above; a command that always said "blank"
    # passes this one. Only a probe that really reads the coverage
    # bytes passes both -- and telling a blank glyph from a drawn one
    # is the entire reason this exists (docs/bugs.md's session-font
    # cell that read as empty while 101/101 glyphs had been built).
    check("...and 'g' has ink in it", "ink  yes" in g)
    blank = font_glyph(dbg, "0x20", "--kernel")
    check("a SPACE is reported as entirely blank",
          "ink  NONE" in blank,
          next((ln.strip() for ln in blank.splitlines() if "ink" in ln), "no ink line"))
    # ...and by codepoint, because a space cannot be passed as an
    # argument through any shell here -- which is why the numeric form
    # exists at all.
    check("...reached by codepoint, since a space cannot be typed",
          "U+0020" in blank, blank.splitlines()[0][:60] if blank else "no output")

    # The ring-0 view must stand alone: on a `text` boot there is no
    # compositor and the client half is unreachable, so --kernel has to
    # carry a picture of its own.
    #
    # ASKED OF A GLYPH THAT HAS INK. The first version of this check
    # demanded a '#' in the SPACE's map, which is the one glyph that
    # can never have one -- a test asserting the opposite of what it had
    # just established two lines above.
    check("--kernel prints a map for a blank glyph too",
          "ink map (kernel" in blank)
    inked = font_glyph(dbg, "A", "--kernel")
    check("...and it has ink in it for a glyph that does",
          "ink map (kernel" in inked and "#" in inked,
          next((ln.strip() for ln in inked.splitlines() if "ink " in ln), "no ink line"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "font_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    global TEXT_Y0, TEXT_Y1
    st = dbg.state()
    TEXT_Y1 = st["screen"]["h"] - st["taskbar_h"]
    set_band(LINE_H)
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
    # AND A FLAT DESKTOP: text_left() finds the leftmost inked column over
    # a patch of desktop, so a wallpaper puts "ink" in every column and
    # every measurement collapses to the patch's own edge. Same class as
    # the face and size below -- establish the state, do not inherit it.
    set_setting(dbg, "desktop.wallpaper", "none")
    set_size(dbg, 14)
    set_face(dbg, PROP)
    dbg.logs()  # drain, so every wait below sees only what IT caused
    set_face(dbg, MONO)
    mono_cell = wait_log(dbg, "wm: font changed")
    check("a face rasterizes from /usr/share/fonts and reaches the compositor",
          "cell" in mono_cell, mono_cell)

    set_band(cell_h_from(mono_cell) or LINE_H)
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

    set_band(cell_h_from(prop_cell) or LINE_H)
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
    builtin_left, builtin_ink = text_left(qmp, "builtin", differs_from=prop_left)
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
    time.sleep(1.0)

    # LAST, and on the restored default face on purpose: every
    # assertion below names a slot, a cell size or an ink flag of the
    # font a fresh image boots with, so running it mid-sequence would
    # pin whatever the previous case happened to leave selected.
    check_glyph_probe(dbg)

    passed = sum(1 for _, ok in checks if ok)
    failed = len(checks) - passed
    print(f"font_test: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
