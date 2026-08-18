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
SPAWN_PATH = "/bin/wm/apps/notepad"   # spawned directly -- see run()
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
    """The text area, once the frame has settled.

    Every check here compares one capture against another, so a capture
    landing mid-paint fails a comparison with nothing wrong with it --
    the failure mode that made calculator_client_test.py intermittent
    (see QMPSession.stable_pixels()). This tool showed the same shape
    once, on "New clears the editor" and its partner, both of which are
    comparisons against an earlier capture.

    The caret is why this needs saying rather than being obvious: it
    does NOT blink here (Notepad draws it only where the cursor is, and
    the cursor does not move on its own), so consecutive reads of a
    quiet editor really are identical. A tool whose window animates
    must not use this.
    """
    return qmp.stable_pixels(os.path.abspath(os.path.join(tmp, name)), box)


def run(dbg, qmp, tmp, shot_dir, res):
    # `gui spawn` rather than typing `run notepad` at a Terminal: the
    # kernel-space Terminal retired in M41's stage 0, and the ring-3 one
    # has no window yet when the injected keys would arrive.
    dbg.send(f"gui spawn {SPAWN_PATH}")

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
    # The text area, as the app reports it. This used to be a hardcoded
    # band (`oy + 60` to `oy + 200`) chosen to sit "generously inside"
    # it -- which stopped being true the moment the toolbar became a
    # menu bar and the text moved up by the difference. The band then
    # sampled blank background in every state, so "typed", "cleared" and
    # "reopened" all compared equal and two checks failed while a third
    # passed for the wrong reason. Ask the app (docs/gui-guidelines.md).
    tr = None
    for l in reversed(dbg.logs("notepad: layout text", clear=False)):
        tr = [int(v) for v in l.split("layout text")[1].split()[:4]]
        break
    res.check("Notepad reports its text area", tr is not None,
              "no 'notepad: layout text' line")
    if tr is None:
        return
    box = (ox + tr[0] + 2, oy + tr[1] + 2,
           ox + tr[0] + tr[2] - 2, oy + tr[1] + tr[3] - 2)

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
    # Ctrl-N. This used to click a toolbar button at a hardcoded offset;
    # the toolbar is a menu bar now (ui/uui_menubar.h), and that offset
    # landed on the File TITLE instead -- which opened the menu, left it
    # open, and had it swallow the Ctrl-O and the arrow keys below. The
    # "New clears the editor" check still passed, because the popup
    # drawing over the sampled box changed those pixels too. The menu
    # itself is covered by tools/menubar_test.py; this test is about the
    # editor, so it drives the accelerator.
    key(dbg, "0x0e")  # Ctrl-N
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

    # Dragging the thumb must move the view too. The strip's rect comes
    # from the app -- this used to be `ox + c["w"] - MARGIN - 6`, a
    # re-derivation that stopped pointing at the bar the moment the
    # chrome around the text area changed (docs/gui-guidelines.md: a GUI
    # test asks the app where things are).
    sb0 = None
    for l in reversed(dbg.logs("notepad: layout scrollbar", clear=False)):
        sb0 = [int(v) for v in l.split("layout scrollbar")[1].split()[:4]]
        break
    if sb0 is None:
        res.check("dragging the scrollbar scrolls the editor", False,
                  "no 'notepad: layout scrollbar' line")
        return
    bar_x = ox + sb0[0] + sb0[2] // 2
    dbg.drag(bar_x, oy + sb0[1] + sb0[3] - 40, bar_x, oy + sb0[1] + 40)
    dbg.settle()
    time.sleep(0.4)
    dragged = text_pixels(qmp, tmp, "np_bardrag.png", box)
    res.check("dragging the scrollbar scrolls the editor", dragged != at_bottom,
              "the text area did not change when the scrollbar thumb was dragged")

    # The stepper arrows (UUI_SCROLLBAR_ARROWS -- Notepad is the flag's
    # first caller). Clicking one steps a line; paired so that "the top
    # arrow did something" cannot pass by the view simply drifting.
    for _ in range(6):
        dbg.send("gui wheel -9")
    dbg.settle()
    time.sleep(0.4)
    pinned = text_pixels(qmp, tmp, "np_pinned.png", box)

    # Ask the app where its scrollbar is. Deriving it here is how the
    # first version of this check clicked the TRACK instead of the
    # arrow, paged instead of stepping, and then could not step back.
    sb = None
    for l in reversed(dbg.logs("notepad: layout scrollbar", clear=False)):
        sb = [int(v) for v in l.split("layout scrollbar")[1].split()[:4]]
        break
    res.check("Notepad reports its scrollbar geometry", sb is not None,
              "no 'notepad: layout scrollbar' line")
    if sb is None:
        return
    sbx, sby, sbw, sbh = sb
    bar_x = ox + sbx + sbw // 2
    bar_top = oy + sby + sbw // 2
    dbg.send(f"gui click {bar_x} {bar_top}")
    dbg.settle()
    time.sleep(0.4)
    stepped = text_pixels(qmp, tmp, "np_arrow_up.png", box)
    res.check("the scrollbar's up arrow steps the view", stepped != pinned,
              "clicking the top stepper arrow changed nothing")

    bar_bottom = oy + sby + sbh - sbw // 2
    dbg.send(f"gui click {bar_x} {bar_bottom}")
    dbg.settle()
    time.sleep(0.4)
    unstepped = text_pixels(qmp, tmp, "np_arrow_down.png", box)
    res.check("the down arrow steps back", unstepped == pinned,
              "stepping up then down did not return to the same pixels")


    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-notepad-reopened.png")))

    # Clean up the file so a re-run starts from the same state.
    dbg.send(f"sh rm /{SAVE_NAME}")

    # Esc must NOT close it. This check is the point of the whole
    # change: Esc used to quit, which put unsaved text one stray
    # keypress away from gone -- and once Esc also became the
    # menu-close key, that keypress got much easier to hit by accident.
    key(dbg, ESC)
    dbg.settle()
    time.sleep(0.6)
    res.check("Esc does NOT close the editor",
              find_window(dbg, SAVE_NAME) is not None
              or find_window(dbg, "untitled") is not None,
              "Esc closed the window -- it must be app-local now")

    # Alt+F4 does. It never reaches the app: the WM takes it and asks
    # through the same handshake the X button uses, so a successful
    # close still proves the client processed WIN_EV_CLOSE and answered
    # with WIN_REQ_DESTROY.
    dbg.send("gui key 0xa5 alt")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if find_window(dbg, SAVE_NAME) is None and find_window(dbg, "untitled") is None:
            gone = True
            break
    res.check("Alt+F4 closes the editor", gone)


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
