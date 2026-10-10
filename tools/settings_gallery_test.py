#!/usr/bin/env python3
"""tools/settings_gallery_test.py -- System Settings' galleries and previews.

WHAT THIS IS
------------
`system.cursor_theme` is a `Widget=gallery` setting with `Preview=cursor`
(data/etc/settings.d/system.cursor_theme): a uui_gallery of cards, one
per theme directory, each tile painted by set_preview.c from the
theme's own shape files through lib/ucursor. This drives it from the
app's own report (`settings: gallery ... card i x y w h tile 1 shown
<label>`), never derived pixels:

  1. the Mouse page shows the setting as a gallery with a card per
     theme, every one with a painter and the theme's Choice. label;
  2. the PICTURE is the theme's: the Amber card's tile carries amber
     ink and the Classic card's carries none -- the control, so a tile
     painted the wrong theme (or the same one everywhere) is red;
  3. a click on a card STAGES it (the page's rule -- nothing is written
     yet, /etc unchanged), and the arrow keys move it like a radio group;
  4. Apply writes it, read back off the disk with `cat`, and the
     compositor loads the theme (its own `cursor: theme "amber"` line).

    python3 tools/vm.py start
    python3 tools/settings_gallery_test.py --instance 0

THE FONT GALLERIES (Appearance > Fonts, `Preview=font` / `fontmono`,
drawn by ui/uui_fontsample in each face from its own .ttf):
  5. both settings are galleries, a card per face, named by the FAMILY
     the file gives ("DejaVu Sans Mono"), never the stem;
  6. the pictures are the faces: Liberation Sans's and DejaVu Sans
     Mono's Interface tiles both carry ink and differ -- one painter
     drawing the same face everywhere is red;
  7. the proportional face's Monospace card says "Not fixed-width" in
     the warning colour, and DejaVu's (the control) does not.

THE WINDOW GALLERIES (`Preview=winmove`, `winresize`, `shadow`,
`seethrough`: the desktop in miniature, set_preview.c) and the SCREEN
MONITOR (Display > Screen):
  8. move_mode and resize_mode are galleries; each Outline card carries
     the dashed white outline and each Window card does not (the
     control); a click on Outline stages `outline`;
  9. the shadows On card darkens the desktop under its windows, Off
     (the control) not at all;
 10. See-through windows: None's bodies are solid, All's are not, and
     Inactive is between (transparency is turned on for it, then off);
 11. the Screen page shows its monitor, captioned with the mode on
     screen, with the desktop in miniature inside it.

POSITIVE CONTROL, run when this was written: the cursor painter drawing
the backdrop and no shapes (`if (!c->loaded) continue;` made
unconditional) -> check 2's "amber ink" goes red and nothing else does.
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
from harness import Results  # noqa: E402

KEY = "system.cursor_theme"
CONF = "/etc/toyos.conf"
WANT = {"Classic", "Classic bold", "Graphite", "Accent", "Amber", "Aurora"}
CARD_RE = re.compile(r"settings: gallery \d+ " + re.escape(KEY) +
                     r" card (\d+) (-?\d+) (-?\d+) (\d+) (\d+) tile (\d) shown (.+)")


def cards(dbg, logs):
    out = {}
    for line in logs:
        m = CARD_RE.search(line)
        if m:
            out[m.group(7).strip()] = {"i": int(m.group(1)), "x": int(m.group(2)),
                                       "y": int(m.group(3)), "w": int(m.group(4)),
                                       "h": int(m.group(5)), "tile": m.group(6) == "1"}
    return out


def stored(dbg):
    for line in (dbg.send(f"sh cat {CONF}") or "").splitlines():
        if line.strip().startswith("cursor_theme="):
            return line.split("=", 1)[1].strip()
    return None


def staged(logs):
    hits = [l for l in logs if f"settings: staged {KEY} " in l]
    return hits[-1].split()[-1] if hits else None


def amber_ink(img, c):
    """Pixels in the card's TILE (its upper part) that read as amber."""
    n = 0
    for y in range(c["y"] + 4, c["y"] + c["h"] * 2 // 3):
        for x in range(c["x"] + 4, c["x"] + c["w"] - 4):
            r, g, b = img.getpixel((x, y))
            if r > 200 and 110 < g < 215 and b < 90:
                n += 1
    return n


def run(dbg, qmp, tmp, res):
    from PIL import Image
    dbg.logs()   # sweep: what came before is not this run's
    dbg.send("gui spawn /bin/wm/system/settings")
    win = None
    for _ in range(30):
        win = dbg.window("System Settings")
        if win:
            break
        time.sleep(0.3)
    if not win:
        res.check("System Settings opened", False)
        return
    search = dbg.widgets("System Settings").get("search") or {}
    scr = search.get("screen") or {"x": win["content"]["x"] + 8, "y": win["content"]["y"] + 8}
    dbg.click(scr["x"] + 20, scr["y"] + 8)
    for ch in "mouse":
        dbg.key(ch)
    got = {}
    logs = []
    for _ in range(30):
        logs = dbg.logs(clear=False)
        got = cards(dbg, logs)
        if WANT <= set(got):
            break
        time.sleep(0.3)
    res.check("the Mouse page shows the theme as a gallery, a card per theme",
              WANT <= set(got) and all(c["tile"] for c in got.values()),
              f"cards={sorted(got)}")
    if "Amber" not in got or "Classic" not in got:
        return
    # The app reports CONTENT-relative rects; the screen is offset by the
    # window's content origin.
    ox, oy = win["content"]["x"], win["content"]["y"]
    for c in got.values():
        c["x"] += ox
        c["y"] += oy

    dbg.warp_cursor(qmp, win["x"] + 20, win["y"] + win["h"] - 20)
    dbg.settle()
    path = os.path.join(tmp, "gallery.png")
    qmp.stable_pixels(path)
    img = Image.open(path).convert("RGB")
    a, c = amber_ink(img, got["Amber"]), amber_ink(img, got["Classic"])
    res.check("the Amber card's tile carries amber ink, Classic's none (the control)",
              a > 40 and c == 0, f"amber={a} classic={c}")

    before = stored(dbg)
    am = got["Amber"]
    dbg.click(am["x"] + am["w"] // 2, am["y"] + am["h"] // 2)
    time.sleep(0.8)
    res.check("a click on a card STAGES it, and nothing is written yet",
              staged(dbg.logs(clear=False)) == "amber" and stored(dbg) == before,
              f"staged={staged(dbg.logs(clear=False))} stored={stored(dbg)} before={before}")
    dbg.key("0xf785")                       # Right: the next card
    time.sleep(0.5)
    right = staged(dbg.logs(clear=False))
    dbg.key("0xf784")                       # Left: back
    time.sleep(0.5)
    res.check("the arrow keys move it like a radio group",
              right == "aurora" and staged(dbg.logs(clear=False)) == "amber", f"right={right}")

    apply = (dbg.widgets("System Settings").get("apply") or {}).get("screen")
    if apply:
        dbg.click(apply["x"] + 10, apply["y"] + 8)
    loaded = False
    for _ in range(20):
        time.sleep(0.4)
        if any('cursor: theme "amber"' in l for l in dbg.logs(clear=False)):
            loaded = True
            break
    res.check("Apply writes it, and the compositor loads the theme",
              stored(dbg) == "amber" and loaded, f"stored={stored(dbg)} loaded={loaded}")


FONT_WANT = {"Built-in", "Liberation Sans", "DejaVu Sans Mono"}


def font_cards(logs, key):
    rx = re.compile(r"settings: gallery \d+ " + re.escape(key) +
                    r" card (\d+) (-?\d+) (-?\d+) (\d+) (\d+) tile (\d) shown (.+)")
    out = {}
    for line in logs:
        m = rx.search(line)
        if m:
            out[m.group(7).strip()] = {"x": int(m.group(2)), "y": int(m.group(3)),
                                       "w": int(m.group(4)), "h": int(m.group(5)),
                                       "tile": m.group(6) == "1"}
    return out


def on_image(img, c):
    """Is the whole card inside the screenshot? A card below the fold is
    reported with coordinates past the screen's edge."""
    return c["x"] >= 0 and c["y"] >= 0 and c["x"] + c["w"] <= img.width and c["y"] + c["h"] <= img.height


def ink_map(img, c):
    """The dark pixels of a card's TILE (above its label), as a set."""
    ink = set()
    for y in range(c["y"] + 4, c["y"] + c["h"] * 2 // 3):
        for x in range(c["x"] + 4, c["x"] + c["w"] - 4):
            r, g, b = img.getpixel((x, y))
            if r + g + b < 300:
                ink.add((x - c["x"], y - c["y"]))
    return ink


def warning_ink(img, c):
    n = 0
    for y in range(c["y"] + 4, c["y"] + c["h"] - 4):
        for x in range(c["x"] + 4, c["x"] + c["w"] - 4):
            r, g, b = img.getpixel((x, y))
            if r > 130 and 60 < g < 130 and b < 40:   # the theme's warning, (160, 92, 0)
                n += 1
    return n


def run_fonts(dbg, qmp, tmp, res):
    from PIL import Image
    dbg.logs()
    dbg.send("gui spawn /bin/wm/system/settings system.font_face")
    win = None
    for _ in range(30):
        win = dbg.window("System Settings")
        if win:
            break
        time.sleep(0.3)
    if not win:
        res.check("System Settings opened on the Fonts page", False)
        return
    ui, mono, logs = {}, {}, []
    for _ in range(30):
        logs = dbg.logs(clear=False)
        ui, mono = font_cards(logs, "system.font_face"), font_cards(logs, "system.font_mono")
        if FONT_WANT <= set(ui) and FONT_WANT <= set(mono):
            break
        time.sleep(0.3)
    stems = [n for n in list(ui) + list(mono) if "-" in n and n == n.lower()]
    res.check("both font settings are galleries, a card per face, named by family",
              FONT_WANT <= set(ui) and FONT_WANT <= set(mono) and not stems and
              all(c["tile"] for c in list(ui.values()) + list(mono.values())),
              f"interface={sorted(ui)} monospace={sorted(mono)}")
    if not (FONT_WANT <= set(ui)):
        return
    ox, oy = win["content"]["x"], win["content"]["y"]
    dbg.warp_cursor(qmp, win["x"] + 20, win["y"] + win["h"] - 20)
    dbg.settle()
    path = os.path.join(tmp, "fonts.png")
    qmp.stable_pixels(path)
    img = Image.open(path).convert("RGB")
    def on_screen(c):
        return dict(c, x=c["x"] + ox, y=c["y"] + oy)
    lib, dvu = ink_map(img, on_screen(ui["Liberation Sans"])), ink_map(img, on_screen(ui["DejaVu Sans Mono"]))
    differ = len(lib ^ dvu)
    res.check("each Interface card is drawn in its own face: both inked, and different",
              len(lib) > 60 and len(dvu) > 60 and differ > 60,
              f"liberation={len(lib)} dejavu={len(dvu)} differing={differ}")

    # The Monospace row with the proportional face sits below the fold:
    # scroll, then read the cards from the lines the page logs AFTER it --
    # the earlier ones still carry the old positions.
    # The REAL pointer, over the page: an injected `gui move` lasts one
    # compositor iteration, and the wheel goes where the pointer is.
    dbg.warp_cursor(qmp, win["x"] + win["w"] * 2 // 3, win["y"] + win["h"] // 2)
    dbg.settle()
    dbg.logs()
    dbg.wheel(-6)
    mono = {}
    for _ in range(20):
        time.sleep(0.3)
        mono = font_cards(dbg.logs(clear=False), "system.font_mono")
        if "Liberation Sans" in mono and "DejaVu Sans Mono" in mono:
            break
    dbg.warp_cursor(qmp, win["x"] + 20, win["y"] + win["h"] - 20)
    dbg.settle()
    qmp.stable_pixels(path)
    img = Image.open(path).convert("RGB")
    if ("Liberation Sans" not in mono or "DejaVu Sans Mono" not in mono or
            not on_image(img, on_screen(mono["Liberation Sans"])) or
            not on_image(img, on_screen(mono["DejaVu Sans Mono"]))):
        res.check("the proportional face's Monospace card says it is not fixed-width", False,
                  f"cards={sorted(mono)}")
        return
    w_lib = warning_ink(img, on_screen(mono["Liberation Sans"]))
    w_dvu = warning_ink(img, on_screen(mono["DejaVu Sans Mono"]))
    res.check("the proportional face's Monospace card says it is not fixed-width; DejaVu's does not",
              w_lib > 20 and w_dvu == 0, f"liberation={w_lib} dejavu={w_dvu}")


# --- the window galleries (Appearance > Windows, Effects, Transparency) ---

def gallery_cards(logs, key):
    """A gallery's cards by label, from the LAST report of each."""
    return font_cards(logs, key)


DESKTOP = (24, 60, 90)   # the miniature's desktop: set_preview.c's PREVIEW_DARK


def tile_box(img, c):
    """The miniature inside a card: the bounding box of its desktop
    colour, so the card's own white margin is never counted."""
    xs, ys = [], []
    for y in range(c["y"], c["y"] + c["h"]):
        for x in range(c["x"], c["x"] + c["w"]):
            if img.getpixel((x, y)) == DESKTOP:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return {"x": min(xs) + 1, "y": min(ys) + 1, "w": max(xs) - min(xs) - 1, "h": max(ys) - min(ys) - 1}


def tile_count(img, c, pred, box=True):
    """Pixels of a card's miniature (or of the rect `c` itself) that `pred` accepts."""
    b = tile_box(img, c) if box else c
    if not b:
        return -1
    n = 0
    for y in range(b["y"], b["y"] + b["h"]):
        for x in range(b["x"], b["x"] + b["w"]):
            if pred(img.getpixel((x, y))):
                n += 1
    return n


def open_page(dbg, key, want, keys):
    """Settings opened on `key`'s page; the cards of each of `keys` once
    every label in `want` has been reported."""
    for w in dbg.windows():
        if w["title"] == "System Settings":
            dbg.send(f"gui close {w['z']}")
            dbg.settle(1.0)
    dbg.logs()
    win = dbg.spawn(f"/bin/wm/system/settings {key}", "System Settings")
    got = {}
    for _ in range(30):
        logs = dbg.logs(clear=False)
        got = {k: gallery_cards(logs, k) for k in keys}
        if all(want[k] <= set(got[k]) for k in keys):
            break
        time.sleep(0.3)
    return win, got


def scroll_to(dbg, qmp, win, key, label):
    """Wheels the page until `key`'s `label` card is on screen; its rect."""
    dbg.warp_cursor(qmp, win["x"] + win["w"] * 2 // 3, win["y"] + win["h"] // 2)
    dbg.settle()
    dbg.logs()
    dbg.wheel(-12)
    cards = {}
    for _ in range(20):
        time.sleep(0.3)
        cards = gallery_cards(dbg.logs(clear=False), key)
        if label in cards:
            break
    return cards


def shot(dbg, qmp, win, tmp, name):
    from PIL import Image
    # The pointer parked on the window's own footer: off every card, and
    # off the taskbar, whose hover opens a preview over the page.
    dbg.warp_cursor(qmp, win["x"] + 20, win["y"] + win["h"] - 12)
    dbg.settle()
    path = os.path.join(tmp, name)
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def is_white(p):
    return p == (255, 255, 255)


def is_shadow(p):
    # The desktop (24, 60, 90) darkened; the taskbar (31, 37, 46) is not.
    return p[0] < 20 and p[1] < 50 and p[2] < 75


def is_solid_body(p):
    # An opaque window body; a see-through one is blended toward the desktop.
    return min(p) > 200 and max(p) - min(p) < 12


def run_windows(dbg, qmp, tmp, res):
    """move_mode and resize_mode are galleries of the desktop in miniature:
    an Outline card carries the dashed white rubber band, a Window card
    does not (the control)."""
    want = {"desktop.move_mode": {"Window", "Outline"},
            "desktop.resize_mode": {"Automatic", "Window", "Outline"}}
    win, got = open_page(dbg, "desktop.move_mode", want, list(want))
    ok = win is not None and all(want[k] <= set(got[k]) for k in want) and \
        all(c["tile"] for k in want for c in got[k].values())
    res.check("Windows: both drag settings are galleries with a painted card per choice", ok,
              f"{ {k: sorted(v) for k, v in got.items()} }")
    if not ok:
        return
    ox, oy = win["content"]["x"], win["content"]["y"]
    img = shot(dbg, qmp, win, tmp, "windows.png")
    for key in want:
        cs = {n: dict(c, x=c["x"] + ox, y=c["y"] + oy) for n, c in got[key].items()}
        band, live = tile_count(img, cs["Outline"], is_white), tile_count(img, cs["Window"], is_white)
        res.check(f"{key}: the Outline card draws the dashed outline, the Window card none",
                  band > 150 and live < 60, f"outline={band} window={live}")
    mv = dict(got["desktop.move_mode"]["Outline"])
    dbg.click(ox + mv["x"] + mv["w"] // 2, oy + mv["y"] + mv["h"] // 2)
    time.sleep(0.8)
    st = [l for l in dbg.logs(clear=False) if "settings: staged desktop.move_mode " in l]
    res.check("a click on the Outline card stages `outline`",
              bool(st) and st[-1].split()[-1] == "outline", st[-1] if st else "nothing staged")
    # Reset, or closing the window asks whether to discard it.
    reset = (dbg.widgets("System Settings").get("reset") or {}).get("screen")
    if reset:
        dbg.click(reset["x"] + 10, reset["y"] + 8)
        dbg.settle(0.6)


def run_effects(dbg, qmp, tmp, res):
    """desktop.shadows: the On card darkens the desktop under its windows,
    Off (the control) has no darkened desktop at all. See-through windows:
    None keeps both window bodies opaque, All lets the desktop through."""
    key = "desktop.shadows"
    win, got = open_page(dbg, key, {key: {"On", "Off"}}, [key])
    cards = scroll_to(dbg, qmp, win, key, "Off") if win else {}
    if not res.check("Effects: Window shadows is a gallery, On and Off", {"On", "Off"} <= set(cards),
                     f"{sorted(cards)}"):
        return
    ox, oy = win["content"]["x"], win["content"]["y"]
    img = shot(dbg, qmp, win, tmp, "effects.png")
    on = tile_count(img, dict(cards["On"], x=cards["On"]["x"] + ox, y=cards["On"]["y"] + oy), is_shadow)
    off = tile_count(img, dict(cards["Off"], x=cards["Off"]["x"] + ox, y=cards["Off"]["y"] + oy), is_shadow)
    res.check("the On card draws a shadow, the Off card none", on > 40 and off == 0,
              f"on={on} off={off}")

    dbg.send("sh config set desktop.transparency on")   # the gallery Requires= it
    try:
        key = "desktop.transparency_windows"
        names = {"None", "Title bars", "Inactive", "All"}
        win, got = open_page(dbg, key, {key: names}, [key])
        cards = scroll_to(dbg, qmp, win, key, "All") if win else {}
        if not res.check("Transparency: See-through windows is a gallery of four",
                         names <= set(cards), f"{sorted(cards)}"):
            return
        ox, oy = win["content"]["x"], win["content"]["y"]
        img = shot(dbg, qmp, win, tmp, "seethrough.png")
        n = {k: tile_count(img, dict(c, x=c["x"] + ox, y=c["y"] + oy), is_solid_body)
             for k, c in cards.items() if k in names}
        res.check("None keeps both window bodies solid, Inactive one, All neither",
                  n["None"] > 400 and n["All"] < n["None"] // 10 and
                  n["All"] < n["Inactive"] < n["None"], f"{n}")
    finally:
        dbg.send("sh config set desktop.transparency off")


def run_screen(dbg, qmp, tmp, res):
    """Display > Screen's monitor: the mode on screen, drawn as a desktop on
    a monitor at the top of the page, and the caption names the mode."""
    dbg.send("sh config set desktop.layout_log on")
    try:
        for w in dbg.windows():
            if w["title"] == "System Settings":
                dbg.send(f"gui close {w['z']}")
                dbg.settle(1.0)
        dbg.logs()
        win = dbg.spawn("/bin/wm/system/settings system.resolution", "System Settings")
        rect, caption = None, None
        for _ in range(30):
            for line in dbg.logs(clear=False):
                m = re.search(r"settings: layout screen_monitor (-?\d+) (-?\d+) (\d+) (\d+)$", line)
                if m:
                    rect = tuple(int(v) for v in m.groups())
                m = re.search(r"settings: screen (.+)$", line)
                if m:
                    caption = m.group(1).strip()
            if rect and caption:
                break
            time.sleep(0.3)
        if not res.check("Screen: the monitor is on the page", win is not None and rect is not None,
                         f"rect={rect}"):
            return
        img = shot(dbg, qmp, win, tmp, "screen.png")
        mode = f"{img.width}x{img.height}"
        res.check("the caption names the mode on screen", bool(caption) and caption.startswith(mode),
                  f"caption={caption!r} mode={mode}")
        x, y, w, h = rect
        box = {"x": win["content"]["x"] + x, "y": win["content"]["y"] + y, "w": w, "h": h * 3 // 2}
        desk = tile_count(img, box, lambda p: p == DESKTOP, box=False)
        res.check("the monitor shows the desktop in miniature", desk > 2000, f"{desk} desktop px")
    finally:
        dbg.send("sh config set desktop.layout_log off")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "settings_gallery_test")
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = args.shot or tempfile.mkdtemp(prefix="gallery-")
    os.makedirs(tmp, exist_ok=True)
    print("settings_gallery_test: checks")
    try:
        run(dbg, qmp, tmp, res)
        dbg.close_window("System Settings")
        run_fonts(dbg, qmp, tmp, res)
        run_windows(dbg, qmp, tmp, res)
        run_effects(dbg, qmp, tmp, res)
        run_screen(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\nsettings_gallery_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
