#!/usr/bin/env python3
"""tools/settings_gallery_test.py -- System Settings' cursor theme gallery.

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
    dbg.key("0xf796")                       # Right: the next card
    time.sleep(0.5)
    right = staged(dbg.logs(clear=False))
    dbg.key("0xf795")                       # Left: back
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
    finally:
        dbg.close()
    print(f"\nsettings_gallery_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
