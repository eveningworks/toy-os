#!/usr/bin/env python3
"""Drive the text-rendering ring-3 client (userland/tests/uiclient.c).

Where winclient_test.py proves the windowing PROTOCOL with flat colour
fills, this proves the userland drawing RUNTIME: a ring-3 process
rendering real anti-aliased text with the desktop's own font, mapped
read-only via WIN_REQ_FONT, and repainting in response to input.

WHY THE ASSERTIONS LOOK LIKE THIS
---------------------------------
The client logs a line per state change (`uiclient: count 3`), but
those go to its stdout, which the window manager routes into the owning
Terminal's scrollback -- NOT to the serial console, so
DebugConsole.logs() cannot see them (a real trap: the same syscall
reaches a different sink depending on who spawned the process). So the
assertions here are pixel-based, which for a rendering feature is the
stronger check anyway.

"Text was rendered" is asserted as INK COVERAGE in a band: a run of
anti-aliased glyphs puts a countable number of non-background pixels in
its row range, while a failure to render leaves that band perfectly
uniform. That distinguishes real text from both a blank window and a
solid fill, which a single-pixel sample cannot. Every check also
compares against a region that must NOT change.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/uiclient_test.py --shot screenshots/YYYY-MM-DD
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
CLIENT_TITLE = "Counter (ring 3)"
# Spawned directly (DebugConsole.spawn -> `gui spawn`), not by typing at
# a Terminal: the kernel-space Terminal retired in M41's stage 0, and the
# ring-3 one has no window yet when the keys would arrive.
SPAWN_PATH = "/tests/uiclient"

# uiclient.c's own layout constants, window-relative. It reports these
# on startup (`uiclient: layout btn ...`) for the same reason
# apps/uidemo.c does -- but see the module docstring for why that line
# isn't readable from here, so these are mirrored and must be kept in
# step with the client.
BTN_X, BTN_Y, BTN_W, BTN_H = 20, 100, 120, 34
BG = (0xF5, 0xF6, 0xF7)

SPAWN_TIMEOUT_S = 15.0


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def grab(qmp, path):
    from PIL import Image
    qmp.screenshot(os.path.abspath(path))
    return Image.open(path).convert("RGB")


def ink_count(im, x, y, w, h, bg=BG, tol=12):
    """Pixels in the box that are not the client's background."""
    n = 0
    for j in range(y, y + h):
        for i in range(x, x + w):
            p = im.getpixel((i, j))
            if any(abs(a - b) > tol for a, b in zip(p, bg)):
                n += 1
    return n


def run(dbg, qmp, tmp, shot_dir, res):
    win = dbg.spawn(SPAWN_PATH, CLIENT_TITLE, SPAWN_TIMEOUT_S)
    res.check("the text-rendering client opened its window", win is not None,
              f"no window titled {CLIENT_TITLE!r} within {SPAWN_TIMEOUT_S}s")
    if not win:
        return

    c = win["content"]
    ox, oy = c["x"], c["y"]

    im = grab(qmp, os.path.join(tmp, "ui0.png"))

    # The two text bands the client draws into, in screen coordinates.
    # Generous heights so this doesn't depend on the exact font size.
    label_ink = ink_count(im, ox + 18, oy + 20, 260, 22)
    count_ink0 = ink_count(im, ox + 18, oy + 48, 200, 26)
    res.check("the client rendered text (label band has ink)",
              label_ink > 40, f"only {label_ink} non-background pixels")
    res.check("the counter line rendered too",
              count_ink0 > 20, f"only {count_ink0} non-background pixels")

    # The button is a filled rect, so it must be solidly NOT background.
    btn_ink = ink_count(im, ox + BTN_X + 4, oy + BTN_Y + 4, BTN_W - 8, BTN_H - 8)
    res.check("the button drew as a filled control",
              btn_ink > (BTN_W - 8) * (BTN_H - 8) * 0.6,
              f"{btn_ink} of {(BTN_W-8)*(BTN_H-8)}")

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-uiclient-text.png")))

    # Clicking the button must change the counter's rendered text. The
    # digit changes from 0 to 1, so the ink pattern in that band must
    # differ -- not merely be non-zero, which it already was.
    bx = ox + BTN_X + BTN_W // 2
    by = oy + BTN_Y + BTN_H // 2
    before = grab(qmp, os.path.join(tmp, "ui1.png")).crop(
        (ox + 18, oy + 48, ox + 218, oy + 74)).tobytes()
    dbg.send(f"gui click {bx} {by}")
    dbg.settle()
    time.sleep(0.7)
    after_im = grab(qmp, os.path.join(tmp, "ui2.png"))
    after = after_im.crop((ox + 18, oy + 48, ox + 218, oy + 74)).tobytes()
    res.check("clicking the button repaints the counter text", before != after)

    # ...and the label above it, which the client redraws identically,
    # must come back the same. This is the control: a client that
    # repainted the whole window differently, or a compositor that
    # smeared, would show up here.
    label_ink2 = ink_count(after_im, ox + 18, oy + 20, 260, 22)
    res.check("the unchanged label is still identical after the repaint",
              abs(label_ink2 - label_ink) <= 2,
              f"{label_ink} -> {label_ink2} ink pixels")

    # A key routed to the client does the same thing, through the other
    # input path.
    before2 = after
    dbg.send("gui key 0x20")  # space -- increments
    dbg.settle()
    time.sleep(0.7)
    after2 = grab(qmp, os.path.join(tmp, "ui3.png")).crop(
        (ox + 18, oy + 48, ox + 218, oy + 74)).tobytes()
    res.check("a key routed to the client repaints the counter text",
              before2 != after2)

    # Close it politely and confirm the handshake still works with this
    # client too.
    dbg.send(f"gui click {win['x'] + win['w'] - 14} {win['y'] + 14}")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if dbg.window(CLIENT_TITLE) is None:
            gone = True
            break
    res.check("the client closes on request", gone)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.5)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, args.shot, res)
    finally:
        dbg.close()

    print(f"\nuiclient_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
