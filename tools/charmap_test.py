#!/usr/bin/env python3
"""tools/charmap_test.py -- Character Map (userland/gui/apps/charmap.c).

What this asks, each against the app's own `charmap: layout state` line
(page, font, count, selected code point, text length, its last) and,
where "it is drawn" is the claim, against pixels:

  1. It opens on the INTERFACE's font with the names table loaded, and
     the grid is DRAWN: a cell holding "!" has ink, a cell past the last
     character has none (the Calculator-with-no-buttons failure).
  2. A NAME search finds RIGHTWARDS ARROW, a character the session font
     cannot draw -- and the detail tile shows it in ink.
  3. Copy, then PASTE INTO THE SEARCH BOX: the round trip through the
     clipboard finds the same one character. The copy's own log line is
     not trusted to say the bytes were right. Twice: the arrow goes as
     UTF-8, e-acute as its one Latin-1 byte (text here is Latin-1).
  4. A double-click adds it to the text line, which then has ink, and
     Backspace on the grid takes it off again.
  5. A letter typed on the focused Block list moves to Greek and Coptic
     without opening it, and the grid lists that block.
  6. The Fonts page: a card picks a different font, and the size ladder
     is drawn in it -- the waterfall before and after must differ.
  7. "Use for terminals" writes system.font_mono (put back afterwards:
     a setting outlives the tool, CLAUDE.md).

    python3 tools/vm.py start
    python3 tools/charmap_test.py
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
from harness import Results  # noqa: E402

TITLE = "Character Map"
ARROW = 0x2192
KEY_TAB, KEY_BS = "0x09", "0x08"   # hex: a bare "8" types the digit
LOG = []


def poll(dbg):
    LOG.extend(dbg.logs("charmap:", clear=True))
    return LOG


class Layout:
    """The last frame's self-reported rects; a frame starts at `rail`."""

    def __init__(self, lines, content):
        self.r, self.state = {}, None
        self.cx, self.cy = content["x"], content["y"]
        last = max([i for i, ln in enumerate(lines) if "charmap: layout rail " in ln] or [-1])
        for line in lines[max(last, 0):]:
            if "charmap: layout " not in line:
                continue
            parts = line.split("charmap: layout ", 1)[1].split()
            try:
                nums = [int(p) for p in parts[1:]]
            except ValueError:
                continue
            if parts[0] == "state":
                self.state = dict(zip(("page", "font", "count", "cp", "ntext", "last"), nums))
            elif len(nums) == 4:
                self.r[parts[0]] = tuple(nums)
            elif len(nums) == 1:
                self.r[parts[0]] = nums[0]

    def screen(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x, self.cy + y, w, h)

    def centre(self, key):
        x, y, w, h = self.screen(key)
        return (x + w // 2, y + h // 2)

    def cell(self, i):
        x, y, w, h = self.screen("grid.first_cell")
        c = self.r["grid.columns"]
        return (x + i % c * w, y + i // c * h, w, h)


def wait(dbg, content, want, timeout=15.0):
    deadline = time.time() + timeout
    while True:
        lay = Layout(poll(dbg), content)
        if (lay.state and want(lay)) or time.time() > deadline:
            return lay
        time.sleep(0.3)


def shot(qmp, tmp, name):
    from PIL import Image
    path = os.path.join(tmp, name)
    qmp.screenshot(path)
    return Image.open(path).convert("RGB")


def ink(im, rect, ground=None):
    """Pixels in `rect` that differ from its top-left corner's (or `ground`)."""
    x, y, w, h = rect
    px = im.load()
    g = ground or px[x, y]
    return sum(1 for j in range(y, y + h) for i in range(x, x + w)
               if sum(abs(a - b) for a, b in zip(px[i, j], g)) > 90)


def chk(res, ok, name):
    return res.check(name, ok)


def typed(dbg, text):
    for ch in text:
        dbg.key(ord(ch), settle=False)
    dbg.settle()


def run(dbg, qmp, tmp, res):
    face = dbg.send("sh config get system.font_face").strip().splitlines()[-1].strip()
    dbg.send("gui spawn /bin/wm/apps/charmap")
    win = None
    for _ in range(50):
        win = dbg.window(TITLE)
        if win:
            break
        time.sleep(0.3)
    if not chk(res, win is not None, "the window opens"):
        return
    content = win["content"]
    lay = wait(dbg, content, lambda lay: lay.state["count"] > 0)
    head = " ".join(LOG)
    names = [int(w) for w in head.split() if w.isdigit()]
    chk(res, "names" in head and any(n > 1000 for n in names), "the names table loaded (charmap: ... names)")
    fonts = [ln for ln in LOG if "charmap: font " in ln]
    chk(res, bool(fonts) and face.replace("-", " ").split()[0].lower() in fonts[0].lower(),
              f"it opens on the interface's font ({face}): {fonts[:1]}")

    dbg.settle()
    im = shot(qmp, tmp, "open.png")
    chk(res, ink(im, lay.cell(1)) > 6, "the cell holding '!' has ink")
    count = lay.state["count"]
    cols = lay.r["grid.columns"]
    empty = count + cols                     # a row below the last character
    gx, gy, gw, gh = lay.screen("grid")
    ex = lay.cell(empty)
    if ex[1] + ex[3] <= gy + gh:
        chk(res, ink(im, ex) == 0, "a cell past the last character has none")

    # 2. A name search.
    dbg.click(*lay.centre("search"))
    typed(dbg, "rightwards arrow")
    lay = wait(dbg, content, lambda lay: lay.state["cp"] == ARROW)
    chk(res, lay.state["cp"] == ARROW, f"'rightwards arrow' selects U+2192 ({lay.state})")
    dbg.settle()
    im = shot(qmp, tmp, "arrow.png")
    sx, sy, sw, sh = lay.screen("grid.selected_cell")
    # Inside the selection's accent fill, measured against that fill: the
    # glyph, not the selection, is the claim.
    chk(res, ink(im, (sx + 6, sy + 6, sw - 12, sh - 12), im.getpixel((sx + 5, sy + sh // 2))) > 6,
        "the grid draws the search's match (white on the selection)")
    tx, ty, tw, th = lay.screen("tile")
    chk(res, ink(im, (tx + 4, ty + 4, tw - 8, th - 8)) > 20, "the detail tile draws the arrow")

    # 3. Copy, then paste it back into the search box.
    dbg.click(*lay.centre("copy"))
    dbg.click(*lay.centre("search"))
    for _ in range(len("rightwards arrow")):
        dbg.key(KEY_BS, settle=False)
    dbg.key("0x16")                  # Ctrl+V
    lay = wait(dbg, content, lambda lay: lay.state["count"] == 1)
    chk(res, lay.state["count"] == 1 and lay.state["cp"] == ARROW,
        f"the copied character pasted into the search finds itself alone ({lay.state}, "
        f"{[ln for ln in LOG if 'charmap: search' in ln][-1:]})")

    # 3b. A Latin-1 character copies as its one byte, which a text field
    # here holds as itself -- pasted back, the search still finds it.
    for _ in range(3):
        dbg.key(KEY_BS, settle=False)
    typed(dbg, "U+00E9")
    lay = wait(dbg, content, lambda lay: lay.state["cp"] == 0xE9)
    dbg.click(*lay.centre("copy"))
    dbg.click(*lay.centre("search"))
    for _ in range(6):
        dbg.key(KEY_BS, settle=False)
    dbg.key("0x16")
    lay = wait(dbg, content, lambda lay: lay.state["count"] == 1 and lay.state["cp"] == 0xE9)
    copied = [ln for ln in LOG if "charmap: copied" in ln]
    chk(res, lay.state["cp"] == 0xE9 and copied and "1 bytes, Latin-1" in copied[-1],
        f"e-acute copies as one Latin-1 byte and pastes back ({lay.state}, {copied[-1:]})")
    for _ in range(2):
        dbg.key(KEY_BS, settle=False)
    typed(dbg, "U+2192")
    lay = wait(dbg, content, lambda lay: lay.state["cp"] == ARROW)

    # 4. Double-click adds it; Backspace on the grid takes it off.
    if "grid.first_cell" not in lay.r:
        return
    cx, cy, cw, ch = lay.cell(0)
    dbg.click(cx + cw // 2, cy + ch // 2, settle=False)
    dbg.click(cx + cw // 2, cy + ch // 2)
    lay = wait(dbg, content, lambda lay: lay.state["ntext"] == 1)
    chk(res, lay.state["ntext"] == 1 and lay.state["last"] == ARROW, f"a double-click adds it ({lay.state})")
    dbg.settle()
    im = shot(qmp, tmp, "text.png")
    bx, by, bw, bh = lay.screen("textline")
    chk(res, ink(im, (bx + 4, by + 3, bw // 4, bh - 6)) > 10, "the text line draws it")
    dbg.key(KEY_BS)
    lay = wait(dbg, content, lambda lay: lay.state["ntext"] == 0)
    chk(res, lay.state["ntext"] == 0, "Backspace on the grid removes it")

    # 5. Block by keyboard: search -> Tab -> the Block list, then "G".
    dbg.click(*lay.centre("search"))
    dbg.key(KEY_BS)
    dbg.key(KEY_TAB)
    dbg.key(ord("G"))
    lay = wait(dbg, content, lambda lay: 0x370 <= lay.state["cp"] <= 0x3FF)
    chk(res, 0x370 <= lay.state["cp"] <= 0x3FF and lay.state["count"] > 20,
              f"'G' on the Block list shows Greek and Coptic ({lay.state})")

    # 6. The Fonts page.
    rx, ry, rw, rh = lay.screen("rail")
    row = lay.r["rail.row_h"]
    dbg.click(rx + rw // 2, ry + lay.r["rail.top"] + row + row // 2)
    lay = wait(dbg, content, lambda lay: lay.state["page"] == 1 and "waterfall" in lay.r)
    if not chk(res, lay.state["page"] == 1, f"the rail opens Fonts ({lay.state})"):
        return
    dbg.settle()
    before = shot(qmp, tmp, "fonts-a.png")
    wf = lay.screen("waterfall")
    chk(res, ink(before, wf) > 500, "the size ladder is drawn")
    was = lay.state["font"]
    other = 0 if was else 1
    cx, cy, cw, ch = lay.screen("cards.first_cell")
    dbg.click(cx + cw // 2, cy + other * ch + ch // 2)
    lay = wait(dbg, content, lambda lay: lay.state["font"] == other)
    chk(res, lay.state["font"] == other, f"a card picks another font ({lay.state})")
    dbg.settle()
    after = shot(qmp, tmp, "fonts-b.png")
    diff = sum(1 for j in range(wf[1], wf[1] + wf[3], 2) for i in range(wf[0], wf[0] + wf[2], 2)
               if before.getpixel((i, j)) != after.getpixel((i, j)))
    chk(res, diff > 200, f"the ladder is redrawn in it ({diff} sampled pixels differ)")

    # 7. Use for terminals, then put the setting back.
    mono = dbg.send("sh config get system.font_mono").strip().splitlines()[-1].strip()
    try:
        dbg.click(*lay.centre("usemono"))
        time.sleep(0.5)
        now = dbg.send("sh config get system.font_mono").strip().splitlines()[-1].strip()
        chk(res, now and now != mono, f"Use for terminals writes system.font_mono ({mono} -> {now})")
    finally:
        dbg.send(f"sh config set system.font_mono {mono}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--shot", metavar="DIR", help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "charmap_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = args.shot or tempfile.mkdtemp(prefix="charmap-")
    os.makedirs(tmp, exist_ok=True)
    print("charmap_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        for i, w in reversed(list(enumerate(dbg.windows() or []))):
            if w.get("title") == TITLE:
                dbg.send(f"gui close {i}")
        dbg.close()
    print(f"\ncharmap_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
