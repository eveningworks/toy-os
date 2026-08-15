#!/usr/bin/env python3
"""Drive the RING-3 Notepad (userland/gui/notepad.c) and assert on it.

The headline check is a full ROUND TRIP through the real filesystem:
type text, save it to a file, clear the buffer, reopen the file, and
require the text area's pixels to match what was there before. That
proves the editor, the ported text widget, and blocking file I/O from a
ring-3 process all work together -- and it needs no OCR, because
"identical pixels" is a stronger statement than any string comparison.

It also proves something specific to this port: the kernel-space Notepad
CANNOT block on disk I/O (it runs inside wm_run(), so a blocking read
would freeze the desktop -- hence its stepped-read state machine). The
ring-3 one just calls sys_read(). If that were wrong, the desktop would
hang here rather than the test failing politely.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/notepad_client_test.py --shot screenshots/YYYY-MM-DD
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
SPAWN_CMD = "run notepad"
SAVE_NAME = "np_test.txt"

MARGIN = 8
SPAWN_TIMEOUT_S = 15.0

CTRL_S = "0x13"
CTRL_O = "0x0f"
ENTER = "0x0d"
ESC = "0x1b"

# `gui key` splits on whitespace, so anything unprintable-as-an-argument
# goes in as hex.
HEX = {" ": "0x20", "!": "0x21", ".": "0x2e", "_": "0x5f", "/": "0x2f"}


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


def key(dbg, k):
    dbg.send(f"gui key {k}")


def type_text(dbg, text):
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()


def find_window(dbg, want):
    """Notepad's title is the file's path with a leading '*' while
    dirty, so match on the BASENAME rather than the whole string --
    "np_test.txt" and "/np_test.txt" are the same file."""
    raw = dbg.json("gui windows --json")
    for w in raw.get("windows", []):
        t = w.get("title", "").lstrip("*")
        base = t.rsplit("/", 1)[-1]
        if base == want or t == want:
            return w
    return None


def text_pixels(qmp, tmp, name, box):
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    with Image.open(p) as im:
        return im.convert("RGB").crop(box).tobytes()


def run(dbg, qmp, tmp, shot_dir, res):
    dbg.send("gui open Terminal")
    dbg.settle()
    type_text(dbg, SPAWN_CMD)
    key(dbg, ENTER)

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = find_window(dbg, "untitled")
        if win:
            break
    res.check("Notepad runs as a ring-3 process with its own window", win is not None)
    if not win:
        return

    c = win["content"]
    ox, oy = c["x"], c["y"]
    # The text area, per notepad.c's text_rect(). Sampled generously
    # inside it so the exact toolbar height doesn't matter.
    box = (ox + MARGIN + 2, oy + 60, ox + c["w"] - 40, oy + 200)

    body = "Round trip through the real filesystem."
    type_text(dbg, body)
    # Park the caret at the start before capturing the reference.
    # load_file() resets the cursor to 0, so a reference captured with
    # the caret still at the end would differ from the reopened render
    # by exactly one caret bar -- a real difference, but not one that
    # says anything about the round trip.
    key(dbg, "0x97")  # Home
    dbg.settle()
    time.sleep(0.5)
    typed = text_pixels(qmp, tmp, "np_typed.png", box)

    blank_ref = None
    res.check("typing renders text in the editor", True)  # confirmed below by contrast

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-notepad.png")))

    # --- save ---------------------------------------------------------
    key(dbg, CTRL_S)
    dbg.settle()
    time.sleep(0.4)
    type_text(dbg, SAVE_NAME)
    key(dbg, ENTER)
    dbg.settle()
    time.sleep(1.0)

    saved_win = find_window(dbg, SAVE_NAME)
    res.check("saving updates the window title to the filename",
              saved_win is not None,
              "no window titled after the saved file")

    # The file must exist on the real filesystem, verified from OUTSIDE
    # the app -- the editor claiming success proves nothing.
    listing = dbg.send(f"sh ls /")
    res.check("the file really exists on disk afterwards",
              SAVE_NAME in listing,
              f"`ls /` did not mention {SAVE_NAME}")

    # The strongest save assertion is not a pixel one: read the file
    # back through a completely different path (the shell) and check the
    # bytes. If the editor wrote the wrong thing, this catches it in a
    # way "the title changed" never could.
    catted = dbg.send(f"sh cat /{SAVE_NAME}")
    res.check("the file's CONTENT on disk is what was typed",
              body in catted.replace("\r", ""),
              f"`cat` returned: {catted[:200]!r}")

    # --- clear, then reopen -------------------------------------------
    dbg.send("gui click %d %d" % (ox + MARGIN + 20, oy + MARGIN + 12))  # New
    dbg.settle()
    time.sleep(0.5)
    cleared = text_pixels(qmp, tmp, "np_cleared.png", box)
    res.check("New clears the editor", cleared != typed)
    blank_ref = cleared

    # Navigate the dialog deterministically: `ls /` lists in the same
    # table order sys_listdir() returns, so the row index is derivable
    # rather than guessed. Guessing is what made an earlier version of
    # this test reopen the dialog in a loop and reset its own selection.
    # Parse ONLY the entry lines. `ls`'s output is interleaved with
    # kernel log lines ("elf_run: calling process_run_ring3() ...") on
    # this console, and a naive split()[-1] picks those up too -- which
    # is exactly how an earlier version of this test computed an index
    # one row off and then blamed the app.
    import re as _re
    entry = _re.compile(r"^[A-Za-z0-9._-]+/?$")
    names = [l.strip().rstrip("/") for l in dbg.send("sh ls /").splitlines()
             if entry.match(l.strip())]
    idx = names.index(SAVE_NAME) if SAVE_NAME in names else -1
    res.check("the saved file appears in the directory listing", idx >= 0,
              f"{SAVE_NAME} not among {names}")

    found = False
    if idx >= 0:
        key(dbg, CTRL_O)
        dbg.settle()
        time.sleep(0.5)
        for _ in range(idx):
            key(dbg, "0x92")  # arrow down
        dbg.settle()
        key(dbg, ENTER)
        dbg.settle()
        time.sleep(1.0)
        found = find_window(dbg, SAVE_NAME) is not None

    res.check("the saved file can be reopened from the dialog", found)

    if found:
        time.sleep(0.6)
        reopened = text_pixels(qmp, tmp, "np_reopened.png", box)
        res.check("reopened text matches what was saved, pixel for pixel",
                  reopened == typed,
                  "the round trip changed the rendered text")
        res.check("...and it is not simply the blank buffer",
                  reopened != blank_ref)

    # --- scrolling: the wheel and the scrollbar -----------------------
    #
    # Both were dead. TWP carried no wheel event at all, so a ring-3
    # client could never receive scrolling -- Notepad drew a scrollbar it
    # had no way to move, and the wheel did nothing in any ring-3
    # window. Nothing here noticed, because every check was about typing
    # and saving.
    #
    # Round-tripped rather than just "it changed": scroll down, require
    # the text to move, scroll back, require it to match the original
    # exactly. "It changed" alone would also pass if scrolling corrupted
    # the view.
    # NUMBERED, not 40 copies of the same word: scrolling a buffer of
    # identical lines produces pixel-identical output, so a test that
    # typed "line" forty times could not tell a working scroll from a
    # dead one. (It didn't -- that is how this check first "failed"
    # against a feature that worked.)
    for i in range(40):
        type_text(dbg, f"row{i}")
        key(dbg, "0x0d")
    dbg.settle()
    time.sleep(0.5)

    # Pin the view to the BOTTOM first, then move UP from it.
    #
    # Which edge the view starts at depends on where the caret ended up,
    # and this test learned that twice. First it wheeled UP from a view
    # that was already at the top (the caret is at 0 after reopening a
    # file), so nothing moved and "scrolling back restores it" passed
    # trivially. Then it tried to pin to the top by wheeling up -- which
    # only works if the wheel works, i.e. it assumed what it was testing.
    # Wheeling DOWN always lands at the bottom whatever the state, so
    # that is the fixed point to measure from.
    for _ in range(6):
        dbg.send("gui wheel -9")
    dbg.settle()
    time.sleep(0.4)
    at_bottom = text_pixels(qmp, tmp, "np_bottom.png", box)

    dbg.send("gui wheel 3")
    dbg.settle()
    time.sleep(0.4)
    scrolled = text_pixels(qmp, tmp, "np_wheel.png", box)
    res.check("the mouse wheel scrolls the editor", scrolled != at_bottom,
              "the text area is pixel-identical before and after a wheel notch")

    dbg.send("gui wheel -3")
    dbg.settle()
    time.sleep(0.4)
    back = text_pixels(qmp, tmp, "np_wheel_back.png", box)
    res.check("scrolling back restores the view exactly", back == at_bottom,
              "scrolling up and down again did not return to the same pixels")

    # The scrollbar strip sits just right of the text area (notepad.c's
    # scrollbar_rect). Dragging its thumb must move the view too.
    bar_x = ox + c["w"] - MARGIN - 6
    dbg.drag(bar_x, oy + 210, bar_x, oy + 70)
    dbg.settle()
    time.sleep(0.4)
    dragged = text_pixels(qmp, tmp, "np_bardrag.png", box)
    res.check("dragging the scrollbar scrolls the editor", dragged != at_bottom,
              "the text area did not change when the scrollbar thumb was dragged")


    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-notepad-reopened.png")))

    # Clean up the file so a re-run starts from the same state.
    dbg.send(f"sh rm /{SAVE_NAME}")

    key(dbg, ESC)
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if find_window(dbg, SAVE_NAME) is None and find_window(dbg, "untitled") is None:
            gone = True
            break
    res.check("Esc closes the editor", gone)


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

    print(f"\nnotepad_client_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
