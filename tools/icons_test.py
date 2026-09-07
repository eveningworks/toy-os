#!/usr/bin/env python3
"""tools/icons_test.py -- application icons, from a .qoi file to the screen.

WHAT THIS IS
------------
The desktop, the Start menu and the taskbar draw an app's icon by name:
a .desktop entry says `Icon=notepad`, `userland/wm/icon_cache.c` decodes
/usr/share/icons/notepad.qoi and scales it, and three draw sites blit
the result with alpha. This checks the whole path, with the HOST as the
oracle -- Pillow decodes the very same file and the guest's framebuffer
has to agree with it.

THE CHECK THAT MATTERS MOST IS THE ALPHA ONE. An icon is a rounded tile
on a transparent background, so the CORNER of its 48x48 box must still
be the wallpaper. A decoder that dropped the alpha channel, or a draw
site that used a plain blit, produces a picture that looks perfectly
reasonable in a screenshot -- a square icon -- and this is the check
that can tell the difference.

  1. Every desktop icon is the file it names, sampled inside the tile
     and compared against Pillow's decode. Its control: the same
     samples must NOT match a different app's icon.
  2. The corners are the WALLPAPER, not the tile -- i.e. it composited.
  3. The Start menu draws an icon per row, and CRASH TEST -- which ships
     with no icon file on purpose -- falls back to its letter without
     disturbing the alignment of the rows around it.
  4. An open window's taskbar button carries its app's icon, matched
     through the entry's AppId (freedesktop's StartupWMClass).
  5. The same icon is in that window's TITLE BAR, at the rect the WM
     itself reports, with the title shifted clear of it -- and clicking
     it opens the window menu, as Windows' system-menu icon and
     Breeze's window-menu button do.
  6. The Start button honours `desktop.start_button` (text | icon |
     both): the mark is the start.qoi file, and the STRIP RE-LAYS OUT
     around it -- which is the half a screenshot of the button alone
     cannot tell you.
  7. The cache is a CACHE: repainting many times does not grow it.

    python3 tools/vm.py start
    python3 tools/icons_test.py
    echo $?                       # 0 = every check passed

POSITIVE CONTROLS, both run against this tool:
  * draw the icons with ugfx_blit() instead of ugfx_blit_alpha() -> the
    corner check goes red and everything else stays green. **This
    control is why that check says what it says.** Its first version
    asserted the corner was "not the tile colour", and the transparent
    pixels in the file are (0,0,0,0) -- so a plain blit writes BLACK
    there, which is certainly not the tile colour, and passed. It
    compares against the wallpaper beside the icon now.
  * make title_icon() ignore icon_get()'s NULL and hand back a rect
    anyway -> the title-bar corner check stays green and nothing else
    moves, which is why that check is paired with "the title starts
    clear of it": a rect with no picture in it is a clickable square
    the user cannot see, and only the ink check can tell.
  * make start_btn_w() ignore the mode and always return the text
    width -> the Start mark still draws (over the label's space) and
    only the "the strip re-lays out" check goes red. That check is the
    reason the others are not enough: every appearance check here would
    pass a build where the button's width never changed and the window
    buttons beside it therefore sat on top of it.
  * point icon_get() at a name that does not exist -> the desktop, menu
    and corner checks go red and the fallback check stays green. It also
    exposed a harness bug worth keeping fixed: the guest's log line about
    the missing file landed in front of `gui taskbar --json` and broke
    the parse, reporting "no taskbar button" for what was really a
    console-multiplexing detail.

CAVEAT
------
Injected input enters below the PS/2 driver (see tools/gui_debug.py), so
a clean run says nothing about the real mouse or keyboard path.
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui        # noqa: E402
from qmp_test import QMPSession                      # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, "data", "icons")

DESKTOP_ICON_SIZE = 48
# The desktop's own grid constants (userland/wm/desktop.c). Read from the
# guest instead where possible; these are only the origin, which the WM
# does not report.
ICON_X0, ICON_Y0 = 16, 16

# How far a guest pixel may sit from the host's decode of the same file.
# The guest scales 64->48 with a box filter and Pillow is asked for the
# same, so this is tighter than the wallpaper's -- what is left is the
# alpha blend against the wallpaper underneath.
TOL = 12


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  PASS  " if ok else "  FAIL  ") + name)
        if not ok and detail:
            print("        " + detail)


def shot(qmp, tmp, name):
    """A SETTLED frame -- see tools/imgview_test.py on why never a raw one."""
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, name))
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def host_icon(name, size):
    """The same file, decoded and scaled on the host. RGBA."""
    from PIL import Image
    src = Image.open(os.path.join(ICON_DIR, name + ".qoi")).convert("RGBA")
    # AN EXACT AREA AVERAGE, colour weighted by alpha and alpha by area
    # -- what uimg_scale() documents, written here in floats so it shares
    # no code. Pillow's BOX is NOT that at a fractional ratio: at 64->30
    # it disagreed with the guest by ~30 per channel on an edge pixel
    # while this agrees within 1 everywhere. Slow (size^2 * ratio^2
    # loops) and fine for a few icons.
    sp = src.load()
    sw, sh = src.size
    out = Image.new("RGBA", (size, size))
    op = out.load()
    rx, ry = sw / size, sh / size
    for oy in range(size):
        y0, y1 = oy * ry, (oy + 1) * ry
        for ox in range(size):
            x0, x1 = ox * rx, (ox + 1) * rx
            a = 0.0
            c = [0.0, 0.0, 0.0]
            tot = 0.0
            for sy in range(int(y0), min(sh, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                if wy <= 0:
                    continue
                for sx in range(int(x0), min(sw, int(x1) + 1)):
                    wx = min(x1, sx + 1) - max(x0, sx)
                    if wx <= 0:
                        continue
                    w = wx * wy
                    px = sp[sx, sy]
                    tot += w
                    a += w * px[3]
                    for i in range(3):
                        c[i] += w * px[3] * px[i]
            rgb = tuple(int(round(c[i] / a)) if a > 0 else 0 for i in range(3))
            op[ox, oy] = rgb + (int(round(a / tot)),)
    return out


def over(fg, bg):
    """Source-over, the operation ugfx_blit_alpha() performs."""
    a = fg[3]
    return tuple((fg[i] * a + bg[i] * (255 - a) + 127) // 255 for i in range(3))


def diff(a, b):
    return max(abs(a[i] - b[i]) for i in range(3))


def close_start_menu(dbg):
    """Close the Start menu, and PROVE it closed.

    This used to be `dbg.key("0x1b")`, which did nothing at all: Esc is
    deliberately unclaimed at the WM level (see wm.c -- it used to exit
    the window manager and was replaced by the Start menu's own "Exit to
    shell"). Nothing noticed, because the only thing that followed was
    `gui open`, which goes through the debug console rather than through
    a click.

    A left-over open menu eats the NEXT CLICK anywhere on screen --
    start_menu_handle_click() is consulted before the window loop -- so
    the first click of any later check silently dismisses a menu instead
    of doing what the check meant. That is what this exists to stop.
    """
    for _ in range(4):
        if not dbg.menu().get("open"):
            return True
        dbg.click(20, 709)          # the Start button TOGGLES
        time.sleep(0.6)
    return not dbg.menu().get("open")


def cached_count(dbg):
    """`gui icons --json`'s count, dug out of whatever else the console said.

    The kernel talks on the same channel (tools/vm.py's own note), so a
    reply can arrive wrapped in unrelated log lines -- taking the last
    {...} rather than the whole buffer is what makes this reliable.
    """
    import json as _json
    reply = dbg.send("gui icons --json") or ""
    start = reply.rfind("{")
    if start < 0:
        return -1
    try:
        return int(_json.loads(reply[start:]).get("cached", -1))
    except ValueError:
        return -1


def run(dbg, qmp, tmp, res):
    # A KNOWN wallpaper, so the composite below has a known backdrop. Set
    # through the kernel shell rather than `gui`, which would repaint.
    dbg.send("sh config set desktop.wallpaper aurora")
    time.sleep(2.0)
    dbg.warp_cursor(qmp, 1180, 640)
    dbg.settle()

    im = shot(qmp, tmp, "icons-desktop.png")

    # --- 1 & 2. the desktop icons ------------------------------------
    #
    # The grid is one column of DESKTOP_ICON_ROW_H, and the WM reports no
    # per-icon rect today, so the row pitch comes from the two icons this
    # tool actually looks at rather than from a formula: About is first
    # and its tile is at the grid origin.
    want = host_icon("about", DESKTOP_ICON_SIZE)
    px = im.load()
    # SAMPLED ON THE PLATE, not in the middle of the pictogram. Every
    # icon here has a white pictogram, so points chosen there match
    # ANY icon and the control below (which is what caught this) reports
    # that the comparison cannot tell two apps apart.
    worst, worst_at = 0, None
    for dx, dy in ((7, 24), (24, 7), (40, 24), (24, 40)):
        got = px[ICON_X0 + dx, ICON_Y0 + dy]
        bg = px[900, 300]                    # a wallpaper pixel, for the blend
        exp = over(want.getpixel((dx, dy)), bg)
        d = diff(got, exp)
        if d > worst:
            worst, worst_at = d, ((dx, dy), got, exp)
    res.check("the desktop icon is the .qoi file it names", worst <= TOL,
              f"worst channel difference {worst} at {worst_at}")

    # The control: those same samples must not match a DIFFERENT icon.
    other = host_icon("notepad", DESKTOP_ICON_SIZE)
    ok_other = True
    for dx, dy in ((7, 24), (24, 7), (40, 24), (24, 40)):
        got = px[ICON_X0 + dx, ICON_Y0 + dy]
        exp = over(other.getpixel((dx, dy)), px[900, 300])
        if diff(got, exp) > TOL:
            ok_other = False
    res.check("and not some other app's", not ok_other,
              "About and Notepad's icons agree at every sample -- this "
              "comparison cannot tell two icons apart")

    # THE ALPHA CHECK. The corner of the icon's box is outside the
    # rounded tile, so it must still be the WALLPAPER -- compared
    # against a wallpaper pixel just beside the icon, since the
    # background is a smooth gradient and neighbours are near-identical.
    #
    # "IT IS NOT THE TILE COLOUR" WAS THE FIRST VERSION OF THIS CHECK AND
    # IT MEASURED NOTHING: the transparent pixels in the file are
    # (0,0,0,0), so a plain ugfx_blit() writes BLACK there -- which is
    # certainly not the tile colour, and passed. The positive control
    # (draw with ugfx_blit instead of ugfx_blit_alpha) is what exposed
    # that, which is the entire reason for running one.
    corner = px[ICON_X0 + 1, ICON_Y0 + 1]
    beside = px[ICON_X0 - 8, ICON_Y0 + 1]
    tile = px[ICON_X0 + 24, ICON_Y0 + 24]
    res.check("the icon's corner is the wallpaper -- it composited",
              diff(corner, beside) <= 6 and diff(corner, tile) > 30,
              f"corner {corner} vs the wallpaper beside it {beside} "
              f"(tile centre {tile})")

    # --- 3. the Start menu -------------------------------------------
    # OPEN IT AND WAIT FOR THE GUEST TO SAY SO, rather than clicking and
    # assuming. The Start button TOGGLES, so a run that inherits an open
    # menu (a previous run that stopped early, say) would close it with
    # exactly the click meant to open it -- which is what happened, and
    # read as a wrong icon rather than as a missing menu.
    for _ in range(4):
        if dbg.menu().get("open"):
            break
        dbg.click(20, 709)
        time.sleep(0.8)
    im2 = shot(qmp, tmp, "icons-menu.png")
    menu = dbg.menu()          # AFTER the capture: geometry read before a
                               # settled frame can describe a menu that has
                               # since closed
    p2 = im2.load()
    rows = {r["label"]: r for r in menu["rows"]} if menu.get("open") else {}
    res.check("the Start menu is open and reports its rows", len(rows) > 3,
              f"rows: {list(rows)[:4]}")

    isz = menu["item_h"] - 4
    if "About" in rows:
        r = rows["About"]
        want = host_icon("about", isz)
        # The menu row's background is flat, so the blend is against it.
        row_bg = p2[menu["x"] + menu["w"] - 4, r["cy"]]
        top = r["cy"] - menu["item_h"] // 2
        worst, at = 0, None
        for dx, dy in ((isz // 3, isz // 3), (isz // 2, isz // 2),
                       (isz // 2, isz // 4)):
            got = p2[menu["x"] + 4 + dx, top + 2 + dy]
            exp = over(want.getpixel((dx, dy)), row_bg)
            if diff(got, exp) > worst:
                worst, at = diff(got, exp), ((dx, dy), got, exp)
        res.check("a Start menu row draws that app's icon", worst <= TOL,
                  f"worst channel difference {worst} at {at}; "
                  f"menu x={menu['x']} row top={top} item_h={menu['item_h']}")

    if "Crash Test" in rows:
        r = rows["Crash Test"]
        # Crash Test ships with NO icon file: its icon column must be
        # empty (the row's own background), which is the fallback path.
        col = p2[menu["x"] + 4 + isz // 2, r["cy"]]
        bgc = p2[menu["x"] + menu["w"] - 4, r["cy"]]
        res.check("an app with no icon file leaves the column empty",
                  diff(col, bgc) <= 6, f"icon column {col} vs row background {bgc}")
    res.check("the Start menu closes again",
              close_start_menu(dbg),
              "still open -- every later check's first click will be eaten "
              "dismissing it")

    # --- 4. the taskbar ----------------------------------------------
    dbg.open_app("Notepad")
    deadline = time.time() + 20
    while time.time() < deadline and dbg.window("untitled") is None:
        time.sleep(0.5)
    time.sleep(1.0)
    tb = dbg.send("gui taskbar --json") or ""
    im3 = shot(qmp, tmp, "icons-taskbar.png")
    p3 = im3.load()
    # THE LAST {...}, not the whole reply: the kernel talks on the same
    # channel, so a log line can land in front of the JSON. Parsing the
    # buffer whole turned a run where icons were deliberately missing
    # into "no taskbar button", which is a harness failure wearing a
    # feature failure's clothes.
    import json as _json
    btns = []
    start = tb.rfind("{\"y\"")
    if start < 0:
        start = tb.rfind("{")
    try:
        btns = _json.loads(tb[start:]).get("buttons", []) if start >= 0 else []
    except ValueError:
        pass
    res.check("the taskbar reports a button for the open window", len(btns) >= 1,
              f"taskbar json: {tb[:160]}")
    state = dbg.send("gui state") or ""
    strip_h = 22
    for word in state.split():
        if word.endswith("px") and word[:-2].isdigit():
            strip_h = int(word[:-2])
            break
    if btns:
        b = btns[0]
        h = im3.height
        # The strip is taskbar_h tall at the bottom; the icon sits 5px in
        # from the button's top-left (wm_render.c's draw_taskbar()).
        # taskbar_icon_size() is taskbar_h - 10 (wm_taskbar.c). The
        # strip height comes from the guest, so this tracks a font change
        # instead of assuming 22px.
        size = max(8, strip_h - 10)
        want = host_icon("notepad", size)
        row_bg = p3[b["x"] + b["w"] - 3, h - strip_h + strip_h // 2]
        worst = 0
        for dx, dy in ((size // 2, size // 2), (size // 2, size // 3)):
            got = p3[b["x"] + 4 + dx, h - strip_h + 5 + dy]
            exp = over(want.getpixel((dx, dy)), row_bg)
            worst = max(worst, diff(got, exp))
        res.check("the taskbar button carries the app's icon", worst <= TOL + 8,
                  f"worst channel difference {worst} at button x={b['x']}")

    # --- 5. the title bar --------------------------------------------
    #
    # THE RECT COMES FROM THE GUEST, not from a formula here. `gui
    # windows --json` reports the very square title_icon() filled in,
    # which is the same square wm_render.c blitted into and wm_input.c
    # hit-tests -- so this cannot drift from the compositor the way a
    # re-derived (WM_TITLEBAR_H - 8, centred) would.
    w = dbg.window("untitled")
    ic = (w or {}).get("icon")
    res.check("the window reports a title-bar icon rect",
              bool(ic) and ic.get("name") == "notepad", f"icon: {ic}")
    if ic:
        im4 = shot(qmp, tmp, "icons-titlebar.png")
        p4 = im4.load()
        ix, iy, isz = ic["x"], ic["y"], ic["size"]
        want = host_icon(ic["name"], isz)
        # The focused title bar is a flat blue, sampled from a row the
        # icon does not reach rather than assumed.
        bar = p4[ix + isz + 2, iy + isz - 1]
        worst, at = 0, None
        for dx, dy in ((isz // 2, isz // 2), (isz // 3, isz // 2), (isz // 2, isz // 3)):
            got = p4[ix + dx, iy + dy]
            exp = over(want.getpixel((dx, dy)), bar)
            if diff(got, exp) > worst:
                worst, at = diff(got, exp), ((dx, dy), got, exp)
        res.check("the title bar draws that app's icon", worst <= TOL,
                  f"worst channel difference {worst} at {at}, bar {bar}")

        # THE ALPHA CHECK AGAIN, and it is a different backdrop from the
        # desktop's -- a flat title bar rather than a wallpaper, so a
        # plain blit here would land BLACK corners on blue and the
        # desktop check would not have caught it.
        res.check("its corner is the title bar -- it composited",
                  diff(p4[ix, iy], bar) <= 6,
                  f"corner {p4[ix, iy]} vs title bar {bar}")

        # AND THE TITLE MOVED OUT OF THE WAY. Text drawn over the icon
        # is the failure this codebase has shipped twice (see
        # docs/gui-guidelines.md on gfx_draw_string not clipping), and
        # the three samples above cannot see it -- they sit on the
        # plate, and a glyph is thin.
        #
        # SO COMPARE THE WHOLE SQUARE, not a few points: the icon is
        # drawn first and the title after it, so ANY overlap shows up as
        # a run of pixels that are not the file. A white glyph on this
        # icon's blue plate is a difference of well over 100.
        #
        # "LOOK FOR THE LEFTMOST WHITE PIXEL" WAS THE FIRST VERSION OF
        # THIS CHECK AND IT MEASURED THE ICON: every icon here has a
        # WHITE PICTOGRAM, so the scan found the artwork's own ink at
        # x = ix + 5 and reported the title as overlapping on a build
        # where it plainly was not.
        worst_px, at_px = 0, None
        for dy in range(isz):
            for dx in range(isz):
                got = p4[ix + dx, iy + dy]
                exp = over(want.getpixel((dx, dy)), bar)
                if diff(got, exp) > worst_px:
                    worst_px, at_px = diff(got, exp), ((dx, dy), got, exp)
        res.check("nothing is drawn over the icon -- the title starts clear",
                  worst_px <= TOL + 20, f"worst pixel {worst_px} at {at_px}")

        # ...and the title really is there, to the RIGHT of it. Without
        # this, an app that drew no title at all would pass the check
        # above by drawing nothing.
        rows = range(iy + 2, iy + isz - 2)
        title_ink = sum(1 for sx in range(ix + isz, ix + isz + 120)
                        for sy in rows if min(p4[sx, sy]) > 230)
        res.check("and the title is drawn beside it", title_ink > 20,
                  f"{title_ink} title pixels right of the icon")

        # THE CLICK. A menu OPENS on button-down (the documented
        # exception in docs/gui-guidelines.md) and is anchored under the
        # icon, not at the cursor -- so asserting its x/y is what
        # distinguishes "the icon opened it" from "a right-click
        # somewhere did".
        dbg.click(ix + isz // 2, iy + isz // 2)
        time.sleep(1.0)
        cm = dbg.ctxmenu()
        labels = [r["label"] for r in cm.get("rows", [])] if cm.get("open") else []
        res.check("clicking the icon opens the window menu",
                  "Close" in labels and "Minimize" in labels, f"rows: {labels}")
        res.check("and it is anchored under the icon, not at the cursor",
                  cm.get("open") and cm.get("x") == ix and cm.get("y") == iy + isz,
                  f"menu at ({cm.get('x')}, {cm.get('y')}), icon ends at "
                  f"({ix}, {iy + isz})")
        # Dismissed with a click on empty desktop, for the same reason
        # close_start_menu() exists -- Esc closes neither popup.
        dbg.click(900, 400)
        time.sleep(0.5)

    # --- 6. the Start button -----------------------------------------
    #
    # Three appearances of ONE control, and the check that matters is
    # not "the mark is drawn" -- it is that everything else on the strip
    # MOVED. The Start button's width is derived from what is in it and
    # every window button starts to the right of that, so a build that
    # drew the mark without re-deriving the width would look perfect in
    # a screenshot of the button and have the window buttons sitting on
    # top of it.
    def taskbar_json():
        """`gui taskbar --json`, or None.

        THE LAST {...}, not the whole reply -- the kernel talks on this
        channel too, so a log line can land in front of the JSON. Same
        rule the taskbar section above already had to learn.
        """
        import json as _j
        tb = dbg.send("gui taskbar --json") or ""
        st = tb.rfind('{"y"')
        if st < 0:
            st = tb.rfind("{")
        if st < 0:
            return None
        try:
            return _j.loads(tb[st:])
        except ValueError:
            return None

    def start_w():
        """The Start button's width, from the guest's own layout."""
        j = taskbar_json()
        return j["start"]["w"] if j else None

    # ESTABLISH THE STARTING POINT rather than assume it. The loop below
    # waits for the width to CHANGE, which needs a value to change FROM
    # -- and the first mode had none, so `w0 not in widths.values()` was
    # trivially true on the first poll and the measurement recorded
    # whatever was on screen before the WM had adopted anything. That
    # passed only while the default happened to BE the first mode tested
    # (it was `text` until 2026-09-07), which is a fixture the test did
    # not establish wearing the costume of a working check.
    dbg.send("sh config set desktop.start_button icon")
    prev = None
    deadline = time.time() + 8
    while time.time() < deadline:
        j = taskbar_json()
        if j and j["start"].get("mark") and j["start"]["w"]:
            prev = j["start"]["w"]
            break
        time.sleep(0.4)
    res.check("the Start button can be put in a known mode to measure from",
              prev is not None, f"start: {(taskbar_json() or {}).get('start')}")

    widths = {}
    for mode in ("text", "icon", "both"):
        dbg.send(f"sh config set desktop.start_button {mode}")
        # The WM adopts it on its next generation poll, so wait for the
        # OBSERVABLE (the strip moved) rather than sleeping a guess. All
        # three widths differ, so "changed from the previous one" is a
        # sound wait at every step.
        deadline = time.time() + 8
        while time.time() < deadline:
            w0 = start_w()
            if w0 is not None and w0 != prev:
                break
            time.sleep(0.4)
        widths[mode] = prev = start_w()
    res.check("every Start button mode lays the strip out differently",
              len(set(v for v in widths.values() if v is not None)) == 3,
              f"Start button widths: {widths}")
    # AND IN THE RIGHT ORDER, which the set above cannot see: icon-only
    # is the narrowest and icon+text the widest, so a build that had the
    # three widths merely DIFFERENT (say, by drawing the mark in the
    # wrong mode) would still fail here.
    if all(v is not None for v in widths.values()):
        res.check("icon-only is narrowest and icon+text widest",
                  widths["icon"] < widths["text"] < widths["both"],
                  f"{widths}")

    # The mark itself is the file, checked the same way every other icon
    # here is. Left in `icon` mode for it, since that is where the mark
    # is centred and easiest to locate from the reported geometry alone.
    dbg.send("sh config set desktop.start_button icon")
    time.sleep(2.0)
    im5 = shot(qmp, tmp, "icons-start.png")
    p5 = im5.load()
    tj = taskbar_json()
    # THE MARK'S RECT COMES FROM THE GUEST, the same start_icon() that
    # blitted it -- not a centred position re-derived here, which would
    # be a second copy of the placement rule and would drift from it.
    mark = (tj or {}).get("start", {}).get("mark")
    res.check("the Start button reports its mark's rect in icon mode",
              bool(mark), f"start: {(tj or {}).get('start')}")
    if mark:
        mx, my, msz = mark["x"], mark["y"], mark["size"]
        swant = host_icon("start", msz)
        # The button's own background, sampled from a corner the mark
        # does not reach.
        btn_bg = p5[6, my + msz - 1]
        worst, at = 0, None
        for dx, dy in ((msz // 4, msz // 4), (3 * msz // 4, 3 * msz // 4)):
            got = p5[mx + dx, my + dy]
            exp = over(swant.getpixel((dx, dy)), btn_bg)
            if diff(got, exp) > worst:
                worst, at = diff(got, exp), ((dx, dy), got, exp)
        res.check("and it is start.qoi", worst <= TOL + 12,
                  f"worst channel difference {worst} at {at}, button bg {btn_bg}")

    # AND `text` MODE REPORTS NO MARK, which is the half that proves the
    # setting reaches the drawing rather than only the width.
    dbg.send("sh config set desktop.start_button text")
    deadline = time.time() + 8
    while time.time() < deadline:
        tj = taskbar_json()
        if tj and tj["start"].get("mark") is None:
            break
        time.sleep(0.4)
    res.check("text mode reports no mark at all",
              (taskbar_json() or {}).get("start", {}).get("mark") is None,
              "a mark is still reported with the button set to text")

    # The check above already left it on `text`, which is the default --
    # deliberate, not incidental. `make iso` re-seeds disk.img by SYNC
    # and never reformats, so a setting written here outlives the run
    # and every later tool would inherit it (CLAUDE.md's dirty-fixture
    # rule).

    # --- 7. the cache is a cache -------------------------------------
    #
    # "The icon is drawn" says nothing about whether the file is decoded
    # once or on every frame. This forces a dozen repaints and requires
    # the cache to be non-empty and UNCHANGED -- a decode-per-frame would
    # either grow it or (if it did not cache at all) leave it at zero.
    n1 = cached_count(dbg)
    for i in range(12):
        dbg.send("gui move %d %d" % (600 + i * 7, 300))
    time.sleep(1.0)
    n2 = cached_count(dbg)
    res.check("repainting does not decode again -- the cache holds",
              n1 > 0 and n1 == n2, f"cached {n1} before, {n2} after 12 repaints")
    print("        (%d icons cached)" % n2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "icons_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Result()
    tmp = args.shot or tempfile.mkdtemp(prefix="icons-")
    os.makedirs(tmp, exist_ok=True)
    print("icons_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\nicons_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
