#!/usr/bin/env python3
"""tools/imgview_test.py -- the JPEG decoder, all the way to the screen.

WHAT THIS IS
------------
`userland/lib/uimg_jpeg.c` is checked for CORRECTNESS in two other
places: tools/uimg_hostcheck.py compares it against libjpeg over a few
hundred generated images on the host, and /tests/uimg_test runs the same
comparison in ring 3. Neither of them can see a pixel on a screen.

So this tool asks the questions only a running desktop can answer, and
its oracle is the same one throughout: **the host decodes the very same
file with libjpeg and the guest's framebuffer has to agree with it.**

  1. The wallpaper on the desktop IS the file, pixel for pixel --
     aurora.jpg is 1280x720 and so is the screen, so no resampling
     stands between the decoder and the check -- and with no wallpaper
     the desktop is one flat colour, which is that check's control.
  2. Image Viewer is a ring-3 client that lists a directory by PROBING
     its files, decodes the selected one, and draws it where it says it
     did.
  3. The fit modes are real: "Fit to window" letterboxes (the picture
     rect is smaller than its widget and the bars are the window
     background), "Actual size" fills and crops.
  4. "Set as wallpaper" reaches the DESKTOP -- a different process --
     through /etc/desktop.conf and the filesystem generation counter,
     and the background becomes the other image.

WHY THE ORACLE MATTERS. "Something colourful appeared" is the weak check
this replaces: a decoder with its colour transform wrong, its chroma
planes swapped, or its image upside down would pass it. Comparing
against libjpeg's own output for the same file cannot be satisfied by
any of those.

    python3 tools/vm.py start
    python3 tools/imgview_test.py
    echo $?                       # 0 = every check passed

POSITIVE CONTROLS, both run against this tool rather than merely
proposed:
  * swap Cb and Cr in uimg_jpeg.c's colour transform -> the four pixel
    comparisons go red (default wallpaper, drawn picture, second
    picture, the desktop picking it up) and every layout check stays
    green. That split is the point: the oracle checks measure the
    decoder and nothing else.
  * make uui_image always use UIMG_FIT_COVER -> both letterbox checks go
    red and nothing else does.

A control that did NOT work, recorded because it says what this tool
cannot see: removing the wallpaper change's damage request in
`userland/wm/desktop.c` leaves every check green. The desktop repaints
on its own cadence within about a second, so "the setting reached the
picture" is measurable here and "the change was declared as damage" is
not.

CAVEAT
------
Injected input enters below the PS/2 driver (see tools/gui_debug.py), so
a clean run says nothing about the real mouse or keyboard path.
"""

import argparse
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui        # noqa: E402
from qmp_test import QMPSession                      # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
WALLPAPER_DIR = "/usr/share/wallpapers"
HOST_WALLPAPERS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "wallpapers")

# How far a guest pixel may sit from libjpeg's value for the same file.
# Two conforming IDCTs are allowed to differ (the JPEG spec fixes the
# transform, not the arithmetic); measured worst case against libjpeg is
# 3, and the framebuffer adds nothing of its own at 32bpp.
TOL = 5

# The viewer's window title, as the WM reports it.
TITLE_VIEWER = "Image Viewer"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print("        " + detail)


def shot(qmp, tmp, name):
    """A SETTLED frame, never a raw screenshot.

    A client that has drawn into its buffer -- and even logged that it
    did, which this tool reads -- has not necessarily been composited
    yet; the desktop is a separate process, so that hop's timing varies
    with load. The first version of this tool sampled the window between
    the app's draw and the compositor's blit and reported a perfectly
    working viewer as an entirely black one. See CLAUDE.md.
    """
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, name))
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def host_image(name):
    from PIL import Image
    return Image.open(os.path.join(HOST_WALLPAPERS, name)).convert("RGB")


def compare_points(got, want, points, tol=TOL):
    """Worst per-channel difference over `points`, and where it was."""
    worst, at = 0, None
    for (gx, gy), (wx, wy) in points:
        a = got.getpixel((gx, gy))
        b = want.getpixel((wx, wy))
        d = max(abs(a[i] - b[i]) for i in range(3))
        if d > worst:
            worst, at = d, ((gx, gy), a, b)
    return worst <= tol, worst, at


def wait_points(qmp, tmp, name, want, points, tol=TOL, timeout=6.0):
    """Settled frames until `points` match `want`, or the deadline.

    The app's "shown" log line precedes the frame on screen by the
    paint plus the compositor hop -- measured at ~0.2 s under TCG, which
    is LONGER than stable_pixels()'s 0.15 s window, so a settled shot
    taken on the log line photographs the previous picture and two
    consecutive dumps agree on it. SETTLED IS NOT CAUGHT UP (CLAUDE.md):
    wait for the observable itself. Returns the last frame and the last
    comparison, so a timeout still reports the worst point.
    """
    deadline = time.time() + timeout
    while True:
        im = shot(qmp, tmp, name)
        ok, worst, at = compare_points(im, want, points, tol)
        if ok or time.time() >= deadline:
            return im, ok, worst, at
        time.sleep(0.1)


class Layout:
    """Image Viewer's self-reported rectangles, from its most recent draw.

    Only the LAST frame is parsed: the log accumulates, so a popup that
    has since closed would otherwise still be reported as open -- the
    trap tools/menubar_test.py documents. `layout menu` is the frame
    boundary, emitted first on every draw.
    """

    def __init__(self, lines, content):
        self.r = {}
        self.cx, self.cy = content["x"], content["y"]
        last = -1
        for i, line in enumerate(lines):
            if "imgview: layout menu " in line:   # the walk's first line
                last = i
        if last >= 0:
            lines = lines[last:]
        for line in lines:
            if "imgview: layout " not in line:
                continue
            parts = line.split("imgview: layout ", 1)[1].split()
            what, nums = parts[0], []
            ok = True
            for p in parts[1:]:
                try:
                    nums.append(int(p))
                except ValueError:
                    ok = False
                    break
            if not ok or not nums:
                continue
            if len(nums) == 1:            # a scalar part: list.selected, menu.open
                self.r[what] = nums[0]
                continue
            if len(nums) < 4:
                continue
            key = " ".join([what] + [str(n) for n in nums[:-4]])
            self.r[key] = tuple(nums[-4:])

    def has(self, key):
        return key in self.r

    def rect(self, key):
        return self.r[key]

    def screen_rect(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x, self.cy + y, w, h)

    def centre(self, key):
        x, y, w, h = self.screen_rect(key)
        return (x + w // 2, y + h // 2)

    def popups(self):
        return sorted(int(k.split()[1]) for k in self.r if k.startswith("menu.popup "))


# EVERY LINE THE APP HAS EVER LOGGED, kept for the whole run.
#
# dbg.logs() CLEARS as it reads, and this app repaints on demand rather
# than on a tick -- so a read taken during a quiet moment returns nothing
# and a Layout built from it knows nothing, even though the app's state
# is perfectly well defined. Keeping the history is what makes "where is
# the File menu" answerable at any point instead of only just after a
# redraw.
LOG = []


def poll_logs(dbg):
    LOG.extend(dbg.logs("imgview:", clear=True))
    return LOG


def wait_layout(dbg, content, want, timeout=15.0):
    """Poll until the app's most recent frame satisfies `want`.

    Waits for the CONDITION rather than for the first layout line: a
    poll whose exit condition is weaker than what follows it is a flake
    by construction (CLAUDE.md).
    """
    deadline = time.time() + timeout
    lay = Layout(poll_logs(dbg), content)
    while time.time() < deadline:
        if want(lay):
            return lay, LOG
        time.sleep(0.3)
        lay = Layout(poll_logs(dbg), content)
    return lay, LOG


def open_menu_item(dbg, content, title_index, item_index):
    """Open menu `title_index` and commit item `item_index`.

    A menu OPENS on press and COMMITS on release -- both go through
    `gui click`, which is a press and a release at one point, so this is
    two clicks and not a drag.
    """
    lay, _ = wait_layout(dbg, content, lambda l: l.has("menu.title %d" % title_index))
    dbg.click(*lay.centre("menu.title %d" % title_index))
    lay2, _ = wait_layout(dbg, content, lambda l: l.popups() == [0])
    key = "menu.item 0 %d" % item_index
    if not lay2.has(key):
        return None
    dbg.click(*lay2.centre(key))
    dbg.settle()
    return lay2


def set_setting(dbg, key, value):
    """Change a setting WITHOUT going near the window manager.

    `gui spawn /bin/config ...` would be the obvious lever and it is the
    wrong one here: a `gui` command makes the WM repaint, so the idle
    check below would be watching a screen something else just dirtied.
    That is not hypothetical -- with `gui spawn`, removing the wallpaper
    change's damage request entirely left the check GREEN. The kernel
    shell touches nothing the compositor knows about.
    """
    dbg.send(f"sh config set {key} {value}")
    time.sleep(0.8)


def run(dbg, qmp, tmp, res):
    # ESTABLISH the wallpaper rather than inherit it. It persists to
    # /etc/desktop.conf on the disk image, which a build syncs onto but
    # never reformats, so a previous run of this tool -- or of
    # font_test.py, which turns the wallpaper OFF because it needs a flat
    # desktop to measure against -- would otherwise decide what check 1
    # is looking at. Set through the registry, which is also the path a
    # person or System Settings takes.
    set_setting(dbg, "desktop.wallpaper_mode", "fill")
    dbg.warp_cursor(qmp, 1180, 40)       # the sprite is composited on top
    dbg.settle()

    # --- 1. the wallpaper, with nothing else touching the screen ------
    #
    # NOTHING BUT THE SETTING HAPPENS BETWEEN THESE TWO CAPTURES: no
    # click, no window, no keystroke, and the setting is changed through
    # the kernel shell rather than through `gui`, so the compositor is
    # not poked into repainting.
    #
    # WHAT THIS DOES NOT PROVE, stated because the check looks like it
    # does: that the wallpaper change DAMAGES the screen. Removing
    # desktop.c's damage request leaves both checks green, because the
    # desktop repaints on its own cadence within about a second either
    # way. This measures that the setting reaches the picture; the
    # damage request is about promptness and is not under test.
    set_setting(dbg, "desktop.wallpaper", "none")
    time.sleep(1.5)
    plain = shot(qmp, tmp, "desk-plain.png")
    flat = {plain.getpixel((x, y)) for x in (300, 700, 1100) for y in (120, 400)}
    res.check("with no wallpaper the desktop is one flat colour", len(flat) == 1,
              f"sampled {flat}")

    set_setting(dbg, "desktop.wallpaper", "aurora")
    time.sleep(2.5)   # the desktop polls, decodes and repaints

    # aurora.jpg is 1280x720 and so is the screen, so UIMG_FIT_COVER
    # scales by exactly 1 and the framebuffer should hold the decoder's
    # output unaltered. That is what makes this the tightest check here:
    # nothing but the decoder sits between the file and these pixels.
    desk = shot(qmp, tmp, "desk-default.png")
    aurora = host_image("aurora.jpg")
    pts = [((x, y), (x, y)) for x in (300, 500, 700, 900, 1100)
           for y in (120, 260, 400, 520)]
    ok, worst, at = compare_points(desk, aurora, pts)
    res.check("...and setting one paints it, pixel for pixel", ok,
              f"worst channel difference {worst} at {at}")

    # A control for that check: the same points must NOT match the OTHER
    # wallpaper, or "the comparison passes" would say nothing.
    dusk = host_image("dusk.jpg")
    ok2, worst2, _ = compare_points(desk, dusk, pts)
    res.check("and is not the other wallpaper", not ok2,
              f"aurora and dusk agree to within {worst2} at every sample -- "
              "this comparison cannot tell them apart")

    # --- 2. the viewer opens, lists by probing, and decodes -----------
    dbg.send("gui spawn /bin/wm/apps/imgview")
    win = None
    deadline = time.time() + 20
    while time.time() < deadline and win is None:
        time.sleep(0.4)
        win = dbg.window(TITLE_VIEWER)
    if win is None:
        print("imgview_test: no Image Viewer window appeared")
        return
    content = win["content"]
    res.check("Image Viewer is a ring-3 client", win.get("client_pid", 0) > 0,
              f"client_pid {win.get('client_pid')} -- 0 would mean ring 0 drew it")

    lay, lines = wait_layout(dbg, content,
                             lambda l: l.has("image.picture") and l.has("list"))
    listed = [l for l in lines if "imgview: listing" in l]
    # The COUNT is not asserted as a literal. It used to be "2 image(s)",
    # which silently went stale the day three more wallpapers were seeded
    # -- a test that fails because the data grew is a test nobody trusts.
    # What matters is that it listed THIS directory and found something.
    def listed_some(line):
        if WALLPAPER_DIR not in line:
            return False
        m = re.search(r"(\d+) image\(s\)", line)
        return bool(m) and int(m.group(1)) > 0
    res.check("it listed the wallpapers directory",
              any(listed_some(l) for l in listed),
              f"listing lines: {listed}")
    shown = [l for l in lines if "imgview: shown" in l]
    res.check("it decoded the first image",
              any("aurora.jpg 1280x720" in l for l in shown),
              f"shown lines: {shown}")
    res.check("it reports where the picture landed",
              lay.has("image.picture") and lay.has("image"),
              "no `layout picture` line")
    if not lay.has("image.picture"):
        return

    # --- 3. the picture on screen is that file ------------------------
    #
    # Sampled through the SAME fit maths the app used: the picture rect
    # the app reported maps back to the source by a simple ratio, since
    # both scaling paths preserve the aspect. Tolerance is wider than the
    # wallpaper's because this one IS resampled -- box-averaged down by
    # about 2.5x, where libjpeg's own pixels are the reference for a
    # different filter.
    im = shot(qmp, tmp, "view-aurora.png")
    px, py, pw, ph = lay.screen_rect("image.picture")
    pts = []
    for fx in (0.25, 0.5, 0.75):
        for fy in (0.3, 0.6):
            pts.append(((px + int(pw * fx), py + int(ph * fy)),
                        (int(aurora.width * fx), int(aurora.height * fy))))
    ok, worst, at = compare_points(im, aurora, pts, tol=40)
    res.check("the drawn picture is the decoded file, scaled", ok,
              f"worst channel difference {worst} at {at}")

    # --- 4. the fit modes are real ------------------------------------
    ix, iy, iw, ih = lay.rect("image")
    pxr, pyr, pwr, phr = lay.rect("image.picture")
    res.check("Fit to window letterboxes rather than cropping",
              pwr <= iw and phr < ih and pwr > 0,
              f"picture {pwr}x{phr} in a {iw}x{ih} box -- a fitted 16:9 "
              "picture in a taller box must leave bars")
    # THE TWO BARS MUST BE THE SAME COLOUR AS EACH OTHER, which is the
    # assertion a cropped picture cannot satisfy: this image is a
    # gradient, so its top and its bottom are far apart. "The bar differs
    # from the picture centre" was the first version of this check and
    # it passed with letterboxing disabled entirely -- the top of the
    # picture differs from its middle too.
    cx0, cy0 = content["x"], content["y"]
    top = im.getpixel((cx0 + ix + iw // 2, cy0 + iy + (ih - phr) // 4))
    bot = im.getpixel((cx0 + ix + iw // 2, cy0 + iy + ih - (ih - phr) // 4))
    mid = im.getpixel((cx0 + pxr + pwr // 2, cy0 + pyr + phr // 2))
    res.check("the letterbox bars are one flat background colour",
              top == bot and max(abs(top[i] - mid[i]) for i in range(3)) > 20,
              f"top bar {top}, bottom bar {bot}, picture centre {mid}")

    open_menu_item(dbg, content, 1, 1)    # View > Actual size
    lay2, _ = wait_layout(dbg, content,
                          lambda l: l.has("image.picture") and l.rect("image.picture")[3] >= ih)
    res.check("Actual size fills the box and crops",
              lay2.has("image.picture") and lay2.rect("image.picture")[2] == iw
              and lay2.rect("image.picture")[3] == ih,
              f"picture {lay2.rect('image.picture') if lay2.has('image.picture') else None} "
              f"in a {iw}x{ih} box -- a 1280x720 image at 1:1 must fill it")

    open_menu_item(dbg, content, 1, 0)    # View > Fit to window
    wait_layout(dbg, content, lambda l: l.has("image.picture") and l.rect("image.picture")[3] < ih)

    # --- 5. selecting the other image ---------------------------------
    lx, ly, lw, lh = lay.screen_rect("list")
    row_h = 0
    # Row 1's centre, derived from the listbox's own row height: ask the
    # app rather than assuming a font size (docs/gui-guidelines.md).
    # The listbox draws rows of uui_listbox_row_h(); one row down from
    # the top is a safe click for a two-item list.
    row_h = max(12, int(lh / 20))
    dbg.click(lx + lw // 2, ly + row_h + row_h // 2)
    lay3, lines3 = wait_layout(dbg, content,
                               lambda l: l.r.get("list.selected") == 1, timeout=20)
    res.check("clicking the second row selects it", lay3.r.get("list.selected") == 1,
              f"selected {lay3.r.get('selected')}")
    res.check("and it decodes the other file",
              any("imgview: shown dusk.jpg" in l for l in lines3),
              f"shown lines: {[l for l in lines3 if 'shown' in l]}")

    px, py, pw, ph = lay3.screen_rect("image.picture")
    pts = [((px + int(pw * fx), py + int(ph * fy)),
            (int(dusk.width * fx), int(dusk.height * fy)))
           for fx in (0.3, 0.6) for fy in (0.3, 0.7)]
    im2, ok, worst, at = wait_points(qmp, tmp, "view-dusk.png", dusk, pts, tol=40)
    res.check("the second picture is dusk.jpg", ok,
              f"worst channel difference {worst} at {at}")

    check_resize_never_blanks(dbg, qmp, tmp, res, win)
    check_auto_resize_falls_back(dbg, res)

    # --- 6. set as wallpaper, and the DESKTOP picks it up -------------
    open_menu_item(dbg, content, 2, 0)    # Desktop > Set as wallpaper (fill)
    conf = ""
    deadline = time.time() + 10
    while time.time() < deadline:
        conf = dbg.send("sh cat /etc/desktop.conf") or ""
        if "dusk" in conf:
            break
        time.sleep(0.5)
    res.check("it set the wallpaper SETTING, not a private key of its own",
              "wallpaper=dusk" in conf.replace(" ", ""),
              f"desktop.conf: {conf.strip()[:200]}")

    # The desktop is a different process and hears about this through the
    # filesystem generation counter, so poll rather than assume.
    #
    # THE WINDOW'S RECT IS RE-READ, not the one captured when it opened:
    # everything above resizes it, and a stale rect let four sample
    # points drift INSIDE the window -- which compares the viewer's own
    # pixels against the wallpaper and fails looking like a desktop that
    # never noticed. An empty spot list is a harness failure, not a pass.
    cur = dbg.window(TITLE_VIEWER) or win
    wx, wy = cur["x"], cur["y"]
    ww, wh = cur["w"], cur["h"]
    spots = [(x, y) for x in (1150, 1200) for y in (100, 300)
             if not (wx <= x <= wx + ww and wy <= y <= wy + wh)]
    if len(spots) < 2:
        res.check("the desktop picked the new wallpaper up", False,
                  f"only {len(spots)} sample points are outside the window "
                  f"at {wx},{wy} {ww}x{wh} -- nothing was measured")
        return
    ok = False
    deadline = time.time() + 20
    while time.time() < deadline and not ok:
        time.sleep(1.0)
        desk2 = shot(qmp, tmp, "desk-dusk.png")
        pts = [((x, y), (x, y)) for (x, y) in spots]
        ok, worst, at = compare_points(desk2, dusk, pts)
    res.check("the desktop picked the new wallpaper up", ok,
              f"worst channel difference {worst} at {at} against dusk.jpg")

    # PUT IT BACK. This tool sets its own starting state (above), so this
    # is a courtesy to whatever runs next rather than something it
    # depends on -- which is the right way round: a tool that needs the
    # previous tool's cleanup to have happened is a tool that fails
    # depending on the order the suite ran in.
    set_setting(dbg, "desktop.wallpaper", "aurora")


def check_resize_never_blanks(dbg, qmp, tmp, res, win):
    """Resizing the viewer never shows a black window.

    THE SLOW CLIENT IS THE POINT. A resize rebuilds the client's buffer,
    and if the buffer the compositor is reading is one of the rebuilt
    ones it composites freshly zeroed memory until the client repaints
    -- which for this app means decoding scale plus a full redraw. That
    gap measured 100-240 ms under TCG here, two whole frames of black
    window, and it is why the front buffer keeps its last frame while
    only the back one is rebuilt (abi/win_proto.h's configure/ack).

    Checked HERE rather than in tools/uapp_test.py, which owns the rest
    of the handshake, because winclient repaints instantly: with that
    client the gap is a few milliseconds and no sampling rate catches it
    -- the check passes with the bug present, which is the shape
    CLAUDE.md calls a test that measures nothing.

    RAW frames, never settled ones: settling waits for two identical
    dumps, which is precisely a way of skipping the frames in question.

    Positive control, run when this was written: rebuild BOTH buffers in
    win_role.c's resize_window() (pass `cw->front` as well as `back`)
    and this goes red with several all-black samples.
    """
    from PIL import Image

    w = dbg.window(TITLE_VIEWER) or win
    c = w["content"]
    # Inside the content's top-left, which stays inside the window at
    # every size a grow passes through -- the grip is the far corner.
    box = (c["x"] + 4, c["y"] + 4, c["x"] + 64, c["y"] + 64)

    blanks, samples = 0, 0
    for _ in range(4):
        cur = dbg.window(TITLE_VIEWER)
        if not cur:
            break
        gx, gy = cur["x"] + cur["w"] - 2, cur["y"] + cur["h"] - 2
        dbg.drag(gx, gy, gx + 50, gy + 34, settle=False)
        until = time.time() + 0.8
        while time.time() < until:
            path = os.path.abspath(os.path.join(tmp, "resize-raw.png"))
            qmp.screenshot(path, stable=False, settle=0.0)
            with Image.open(path) as im:
                px = list(im.convert("RGB").crop(box).getdata())
            samples += 1
            if all(p[0] < 24 and p[1] < 24 and p[2] < 24 for p in px):
                blanks += 1
    dbg.settle()
    restore_window_size(dbg, w)

    res.check("resizing the viewer never shows a blank window",
              blanks == 0 and samples >= 8,
              f"{blanks} all-black samples of {samples}")


def restore_window_size(dbg, was):
    """Drag the grip back to the size the window was.

    NOT a courtesy: the compositor SAVES a window's geometry when it
    closes and restores it at the next launch (`/etc/windows.conf`), so
    a check that leaves the viewer covering the screen hands the next
    RUN of this tool a window with no desktop beside it -- which is how
    a wallpaper check that samples the desktop starts failing a day
    later, in a tool nobody changed. See CLAUDE.md on persisted state.
    """
    if not was:
        return
    now = dbg.window(TITLE_VIEWER)
    if not now:
        return
    dbg.drag(now["x"] + now["w"] - 2, now["y"] + now["h"] - 2,
             now["x"] + now["w"] - 2 + (was["w"] - now["w"]),
             now["y"] + now["h"] - 2 + (was["h"] - now["h"]))
    dbg.settle()
    time.sleep(1.0)


def check_auto_resize_falls_back(dbg, res):
    """`desktop.resize_mode = auto` on a client that CANNOT keep up.

    The other half of the pair tools/uapp_test.py holds. `auto` exists
    to tell a fast client from a slow one, so one client can only ever
    show one half: winclient repaints in a couple of frames and must
    stay live, and this viewer rescales a JPEG per proposed size and
    must not. Measured here: ~200-300 ms to become a size it was asked
    for, against winclient's 20-30 ms, and a threshold of 100 ms.

    THE FIRST DRAG IS LIVE AND THAT IS CORRECT -- a window nobody has
    dragged has no measurement, and `auto` refuses to guess from
    anything but one. It learns from that drag, so the SECOND is the
    one that must be an outline. Asserting on the second is also what
    makes this independent of how fast the host is: whatever the lag
    turns out to be, the check is that the WM acted on the number it
    measured.

    Positive control, run when this was written: drop the remembered
    `windows[resizing].resize_lag_ms` test from wm_input.c's drag start
    and this goes red -- the second drag stays live, because an injected
    drag is over before the client's slow frame arrives, which is
    exactly why the WM remembers rather than only reacting.
    """
    start = dbg.window(TITLE_VIEWER)

    def drag_grip(dx, dy):
        w = dbg.window(TITLE_VIEWER)
        if not w:
            return None
        dbg.drag(w["x"] + w["w"] - 2, w["y"] + w["h"] - 2,
                 w["x"] + w["w"] - 2 + dx, w["y"] + w["h"] - 2 + dy)
        dbg.settle()
        time.sleep(1.2)      # the rescale, which is the whole point
        st = dbg.state()
        return {"paint": st["resize_paint"], "lag": st["resize_lag_ms"]}

    set_setting(dbg, "desktop.resize_mode", "auto")
    first = drag_grip(90, 60)
    second = drag_grip(90, 60)
    res.check("auto: a client too slow to follow is outlined on the next drag",
              first and second and first["lag"] > 100 and second["paint"] == "outline",
              f"first drag lag {first['lag']}ms paint {first['paint']}, "
              f"second paint {second['paint']}"
              if first and second else "no window")
    set_setting(dbg, "desktop.resize_mode", "auto")
    restore_window_size(dbg, start)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    ap.add_argument("--shot", metavar="DIR", help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "imgview_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Result()
    tmp = args.shot or tempfile.mkdtemp(prefix="imgview-")
    os.makedirs(tmp, exist_ok=True)
    print("imgview_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\nimgview_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
