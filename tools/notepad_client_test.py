#!/usr/bin/env python3
"""Drive the RING-3 Notepad (userland/gui/apps/notepad.c) and assert on it.

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
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402
from harness import Results, poll  # noqa: E402

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


Result = Results


def key(dbg, k):
    dbg.send(f"gui key {k}")


def type_text(dbg, text):
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()


def fileview_selected(dbg, default=-1):
    """The Open chooser's selected row, as it reports it.

    Under `filedialog:`, not `notepad:` -- the chooser is its own WINDOW
    now (ui/uui_filedialog.h) and reports under one prefix whichever app
    opened it, so a helper written here reads the Image Viewer's too.
    -1 when nothing has drawn it since the log was last read."""
    for l in reversed(dbg.logs("filedialog: layout view.selected", clear=False)):
        try:
            return int(l.split("view.selected")[1].split()[0])
        except (IndexError, ValueError):
            continue
    return default


def dialog_buttons(dbg):
    """The unsaved-changes dialog's button rects, content-relative.

    Read from the app's OWN layout report (ui/uui_describe.h), not
    guessed from a font size -- and an empty list is how "the dialog is
    not up" is told from "it is up somewhere else". Only the latest
    frame counts, so the log is cut at the frame boundary first: a
    dialog that has since been dismissed would otherwise still be
    reported open, the trap tools/menubar_test.py documents.
    """
    lines = dbg.logs("notepad: layout", clear=False)
    last = -1
    for i, l in enumerate(lines):
        if "notepad: layout scrollbar" in l:   # leads every frame
            last = i
    if last < 0:
        return []
    rects = {}
    for l in lines[last:]:
        if "notepad: layout ask-save.button " not in l:
            continue
        nums = [int(v) for v in l.split("ask-save.button")[1].split()[:5]]
        rects[nums[0]] = tuple(nums[1:5])
    return [rects[i] for i in sorted(rects)]


def last_rect(dbg, what):
    """One `notepad: layout <what> x y w h` rect from the latest frame."""
    lines = dbg.logs("notepad: layout", clear=False)
    last = -1
    for i, l in enumerate(lines):
        if "notepad: layout scrollbar" in l:   # leads every frame
            last = i
    if last < 0:
        return None
    key = "notepad: layout %s " % what
    for l in reversed(lines[last:]):
        if key in l:
            nums = l.split(key, 1)[1].split()[:4]
            try:
                return tuple(int(v) for v in nums)
            except ValueError:
                return None
    return None


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
    # A SHORTCUT IS NEVER TYPED. Ctrl+1 and Alt+x reach a window as the
    # key with its modifier bit (api/keyboard.h), and an editor that
    # inserted every character would type on every shortcut. Checked on
    # the saved file below.
    dbg.send("gui key 0x31 ctrl")
    dbg.send("gui key 0x78 alt")
    dbg.settle()
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
    listing = dbg.send("sh ls /")
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
    res.check("...and Ctrl+1 / Alt+x typed nothing into it",
              ("1" + body) not in catted and ("x" + body) not in catted
              and ("1x" + body) not in catted,
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
    # WAIT FOR THE APP TO SAY IT ACTED, not for the frame to go quiet.
    # text_pixels() settles (stable_pixels: two identical consecutive
    # reads), and settled is not the same as UPDATED -- two reads of a
    # frame that has not been repainted yet are identical too. So under
    # load, where Ctrl-N has not been handled by the time the capture
    # starts, this check sampled the OLD frame and reported that New had
    # not cleared anything. Measured: it failed here with four other
    # guests running and passed 19/19 alone.
    #
    # New clears g_path, so the title goes back to "untitled" -- an
    # observable the app owns, unlike a sleep.
    deadline = time.time() + 8.0
    while find_window(dbg, "untitled") is None and time.time() < deadline:
        time.sleep(0.2)
    res.check("New put the window back to `untitled`",
              find_window(dbg, "untitled") is not None,
              "the title never returned to untitled after Ctrl-N")

    # AND IT IS NOT DIRTY, which find_window() cannot tell you: it
    # strips the leading '*' so that a path matches whether or not the
    # document has been edited. So the raw title is read here. A fresh
    # document wrongly marked dirty would make the unsaved-changes
    # dialog fire on every close of an empty editor, and every check in
    # this file would still pass.
    raw = [w.get("title", "") for w in dbg.json("gui windows --json")["windows"]]
    res.check("...and a new document is not marked modified",
              "untitled" in raw and "*untitled" not in raw,
              f"window titles: {raw}")
    cleared = text_pixels(qmp, tmp, "np_cleared.png", box)
    res.check("New clears the editor", cleared != typed)
    blank_ref = cleared

    # The file is really there, asked of the filesystem rather than of
    # the app. Parse ONLY the entry lines: `ls`'s output is interleaved
    # with kernel log lines on this console, and a naive split()[-1]
    # picks those up too -- which is how an earlier version of this test
    # computed an index one row off and then blamed the app.
    import re as _re
    entry = _re.compile(r"^[A-Za-z0-9._-]+/?$")
    names = [l.strip().rstrip("/") for l in dbg.send("sh ls /").splitlines()
             if entry.match(l.strip())]
    res.check("the saved file appears in the directory listing",
              SAVE_NAME in names, f"{SAVE_NAME} not among {names}")

    # SELECT THE ROW BY TYPE-AHEAD, WITH ONE LETTER, and both halves of
    # that matter.
    #
    # Counting arrow presses is out: the dialog's body is a uui_fileview
    # now, which groups directories before files and sorts within each
    # group, so `ls`'s order is not the list's order and an index derived
    # from it lands on a directory. Re-deriving the list's order here
    # would be the same mistake in a longer form -- the app already knows
    # it (docs/conventions/gui.md).
    #
    # ONE letter, not the whole name: uui_seek's window is
    # UUI_SEEK_WINDOW_MS between keystrokes, so a multi-letter search
    # sent over this console restarts if any one key is slow, which under
    # a loaded suite it will be. The first keystroke starts a fresh
    # search and waits on nothing. `np` is the only entry at "/" starting
    # with that letter; if that ever stops being true the title check
    # below fails loudly rather than passing on the wrong file.
    found = False
    if SAVE_NAME in names:
        key(dbg, CTRL_O)
        dbg.settle()
        time.sleep(0.5)
        key(dbg, SAVE_NAME[0])
        dbg.settle()
        res.check("typing a letter moves the dialog's selection",
                  fileview_selected(dbg) > 0,
                  "the fileview never reported a selection past row 0")
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

    # --- closing a MODIFIED document asks first ----------------------
    #
    # Every action that would throw the document away goes through one
    # confirm (userland/gui/apps/notepad.c): the X, Alt+F4, File > Exit,
    # New, Open and a Recent entry. Alt+F4 is checked here because it is
    # the one that never reaches the app -- the WM takes it and asks
    # through the same handshake the X button uses -- so this also proves
    # the client can REFUSE that handshake, not merely answer it.
    #
    # The document is made dirty on purpose rather than assumed: whether
    # it is depends on what the checks above left behind, and a check
    # that only fires some runs is worse than none.
    type_text(dbg, "zz")
    dbg.settle()
    time.sleep(0.3)

    dbg.send("gui key 0xa5 alt")
    dbg.settle()
    time.sleep(0.8)
    res.check("Alt+F4 on a MODIFIED document does not close it",
              find_window(dbg, SAVE_NAME) is not None
              or find_window(dbg, "untitled") is not None,
              "the window went away with unsaved text in it")

    # And it is ASKING -- the dialog reports its own buttons, so this is
    # the app's geometry rather than a guess at where they landed.
    btns = dialog_buttons(dbg)
    res.check("...it puts up the unsaved-changes dialog, with three buttons",
              len(btns) == 3, f"button rects reported: {btns}")
    if len(btns) != 3:
        return

    win = find_window(dbg, SAVE_NAME) or find_window(dbg, "untitled")
    ox, oy = win["content"]["x"], win["content"]["y"]

    # --- the modal owns the CURSOR, not the editor underneath ---------
    #
    # The third axis of "modal": the click and the key were refused long
    # before the pointer's SHAPE was, so an I-beam belonging to a text
    # area nobody could reach sat over the dialog. The point sampled is
    # inside the document and OUTSIDE the dialog box, which is where the
    # old behaviour was visible and where the fix has to show.
    #
    # The I-beam is re-checked after Cancel as the CONTROL: without it,
    # an app that had simply stopped asking for the caret at all would
    # pass the first half and mean nothing.
    text_r = last_rect(dbg, "text")
    box_r = last_rect(dbg, "ask-save")
    cur_pt = None
    if text_r and box_r:
        tx, ty, tw, th = text_r
        bx, by, bw, bh = box_r
        # Above the box if there is room in the document, else below.
        py = ty + 8 if by - ty > 16 else by + bh + 8
        cur_pt = (ox + tx + tw // 2, oy + py)

    # These assert the behaviour, not the mechanism, which is why they
    # survived the toolkit fix being withdrawn: Notepad gates its own
    # I-beam on the modal now (docs/bugs.md). They go green again for
    # free if the universal version ever lands.
    res.check("Notepad reports the rects the cursor check needs",
              cur_pt is not None,
              f"text={text_r} ask-save={box_r}")
    if cur_pt:
        dbg.warp_cursor(qmp, *cur_pt)
        time.sleep(0.4)
        shape_modal = dbg.cursor_shape()
        res.check("with the modal up the cursor is the arrow, not the I-beam",
                  shape_modal == DebugConsole.CURSOR_NORMAL,
                  f"shape {shape_modal} at {cur_pt} (I-beam is "
                  f"{DebugConsole.CURSOR_TEXT})")

    def press(i):
        x, y, w, h = btns[i]
        dbg.send(f"gui click {ox + x + w // 2} {oy + y + h // 2}")
        dbg.settle()
        time.sleep(0.6)

    press(2)                                  # Cancel
    res.check("Cancel keeps the window and the text",
              find_window(dbg, SAVE_NAME) is not None
              or find_window(dbg, "untitled") is not None,
              "Cancel closed the window anyway")
    res.check("...and dismisses the dialog", not dialog_buttons(dbg),
              "the dialog is still reporting buttons after Cancel")

    # THE CONTROL for the cursor check above: the same point must go
    # back to the I-beam once the modal is gone. Without this, an app
    # that had stopped asking for the caret anywhere would pass.
    if cur_pt:
        dbg.warp_cursor(qmp, cur_pt[0], cur_pt[1] + 1)   # force a motion
        dbg.warp_cursor(qmp, *cur_pt)
        time.sleep(0.4)
        shape_after = dbg.cursor_shape()
        res.check("...and the I-beam comes back over the document",
                  shape_after == DebugConsole.CURSOR_TEXT,
                  f"shape {shape_after} at {cur_pt} (I-beam is "
                  f"{DebugConsole.CURSOR_TEXT})")

    # --- New opens a TAB; closing a tab asks, and only when it must --
    #
    # New used to replace the document, so it asked first. With tabs it
    # opens one beside it (Windows 11 Notepad, Kate) and throws nothing
    # away -- so it must NOT ask. Closing a tab is the discard now, and
    # it asks for a modified one and not for a clean one.
    def tabs():
        for l in reversed(dbg.logs("notepad: layout tabcount", clear=False)):
            n = l.split("tabcount")[1].split()
            return int(n[0]), int(n[2])
        return None

    n0, cur0 = tabs() or (1, 0)
    key(dbg, "0x0e")                          # Ctrl-N
    dbg.settle()
    time.sleep(0.8)
    res.check("New on a modified document opens a tab, and does not ask",
              not dialog_buttons(dbg) and tabs() == (n0 + 1, n0), f"tabs {tabs()}, had {n0}")
    res.check("...titled untitled", find_window(dbg, "untitled") is not None)

    key(dbg, "0x17")                          # Ctrl-W: the clean new tab
    dbg.settle()
    time.sleep(0.8)
    # ...and goes back to the tab it was opened FROM, as browsers do.
    res.check("Ctrl-W closes a CLEAN tab without asking, back to the one before",
              not dialog_buttons(dbg) and tabs() == (n0, cur0), f"tabs {tabs()}, was on {cur0}")

    key(dbg, "0x17")                          # Ctrl-W: the modified one
    dbg.settle()
    time.sleep(0.8)
    btns = dialog_buttons(dbg)
    res.check("Ctrl-W on a MODIFIED tab asks before discarding it",
              len(btns) == 3, f"button rects reported: {btns}")
    if len(btns) != 3:
        return
    press(1)                                  # Don't Save -> the tab goes
    res.check("...and Don't Save closes that tab", tabs() == (max(1, n0 - 1), max(0, n0 - 2))
              or (tabs() or (0,))[0] == max(1, n0 - 1), f"tabs {tabs()}")

    # On to an untitled document for what follows: a new tab.
    key(dbg, "0x0e")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    fresh = False
    while time.time() < deadline and not fresh:
        fresh = find_window(dbg, "untitled") is not None
        time.sleep(0.3)
    res.check("...and a new tab is untitled", fresh,
              "the title did not go back to `untitled`")

    # --- Save, on a document with no filename yet --------------------
    #
    # The intricate branch: Save on an untitled document has to open the
    # chooser and finish the close only once the bytes are down, which
    # is two waits stacked on each other. CANCELLING the chooser must
    # cancel the CLOSE too -- quitting there would throw away exactly
    # the text the dialog was put up to protect.
    type_text(dbg, "qq")
    dbg.settle()
    time.sleep(0.3)
    dbg.send("gui key 0xa5 alt")
    dbg.settle()
    time.sleep(0.8)
    btns = dialog_buttons(dbg)
    if len(btns) == 3:
        press(0)                              # Save
        chooser = None
        deadline = time.time() + SPAWN_TIMEOUT_S
        while time.time() < deadline and chooser is None:
            chooser = find_window(dbg, "Save As")
            time.sleep(0.3)
        res.check("Save on an untitled document opens the chooser",
                  chooser is not None,
                  "no `Save As` window -- Save did nothing, or saved to "
                  "a filename nobody chose")
        key(dbg, ESC)                         # cancel the chooser
        dbg.settle()
        time.sleep(0.8)
        res.check("...and cancelling the chooser cancels the close too",
                  find_window(dbg, "untitled") is not None,
                  "the editor quit with unsaved text after the chooser was "
                  "cancelled")

    # Ask again, and take the other answer.
    dbg.send("gui key 0xa5 alt")
    dbg.settle()
    time.sleep(0.8)
    btns = dialog_buttons(dbg)
    # Result.check() RETURNS NOTHING -- guard on the condition, never on
    # its return value. Writing `if not res.check(...)` here skipped the
    # rest of this function AND every markdown check below it, while the
    # tool reported a clean pass.
    asked_again = len(btns) == 3
    res.check("Alt+F4 asks again after a Cancel", asked_again,
              f"button rects reported: {btns}")
    if not asked_again:
        return
    press(1)                                  # Don't Save

    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if find_window(dbg, SAVE_NAME) is None and find_window(dbg, "untitled") is None:
            gone = True
            break
        time.sleep(0.3)
    res.check("Don't Save closes the editor", gone,
              "the window survived an explicit Don't Save")

    markdown_checks(dbg, qmp, tmp, res)
    history_and_find_checks(dbg, res)


# --- the edit history and find ------------------------------------------

def history_and_find_checks(dbg, res):
    """Undo walks back to the SAVED state, and find counts what it finds.

    The title's `*` is the observable for the history: dirty is a
    POSITION in it (ui/uui_undo.h), so undoing everything typed into a
    fresh document must make it clean again -- an undo that only
    restored the text, or a dirty flag set by every edit, both fail.
    """
    dbg.send(f"gui spawn {SPAWN_PATH}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline and find_window(dbg, "untitled") is None:
        time.sleep(0.3)

    def title():
        for w in dbg.json("gui windows --json")["windows"]:
            if w.get("title", "").lstrip("*") == "untitled":
                return w["title"]
        return None

    type_text(dbg, "alpha beta")
    time.sleep(0.3)
    res.check("typing marks the new document modified", title() == "*untitled", f"{title()}")
    key(dbg, "0x1a")                          # Ctrl-Z: "beta"
    key(dbg, "0x1a")                          # Ctrl-Z: "alpha "
    dbg.settle()
    time.sleep(0.3)
    res.check("undoing everything typed makes it CLEAN again", title() == "untitled", f"{title()}")
    key(dbg, "0x19")                          # Ctrl-Y
    dbg.settle()
    time.sleep(0.3)
    res.check("redo makes it modified again", title() == "*untitled", f"{title()}")

    key(dbg, "0x01")                          # Ctrl-A, then type over it
    type_text(dbg, "one two one three one")
    key(dbg, "0x06")                          # Ctrl-F
    type_text(dbg, "one")
    dbg.settle()
    time.sleep(0.4)
    matches = None
    for l in reversed(dbg.logs("notepad: layout find.matches", clear=False)):
        matches = int(l.split("find.matches")[1].split()[0])
        break
    res.check("find counts every match", matches == 3, f"matches {matches}")
    key(dbg, ESC)

    dbg.send("gui key 0xa5 alt")              # Alt+F4 -> ask -> Don't Save
    dbg.settle()
    time.sleep(0.8)
    btns = dialog_buttons(dbg)
    win = find_window(dbg, "untitled")
    if len(btns) == 3 and win:
        x, y, w, h = btns[1]   # content-relative, like every reported rect
        c = win["content"]
        dbg.send(f"gui click {c['x'] + x + w // 2} {c['y'] + y + h // 2}")


# --- the Markdown preview ---------------------------------------------

def markdown_checks(dbg, qmp, tmp, res):
    """A .md opens RENDERED, and Ctrl-E goes back to the source.

    The load-bearing check is that the two look DIFFERENT: a preview
    that silently fell back to drawing the raw text would satisfy "a
    window appeared", "the widget reported a layout" and "typing does
    nothing" all at once, and only the pixels can tell.
    """
    from PIL import Image

    # ONE FILE UNDER TWO EXTENSIONS, which is what makes the control at
    # the end airtight: the .md and the .txt differ in nothing but their
    # name, so a preview that appeared for both could only be ignoring
    # the extension. A real manual page is the fixture because it
    # contains one of everything the renderer draws -- headings, bold,
    # inline code, a code block, bullets and a table.
    src = "/usr/share/doc/cmd/ls.md"
    doc = "/var/tmp/np_md.md"
    txt = "/var/tmp/np_md.txt"
    dbg.send(f"sh rm {doc}")
    dbg.send(f"sh rm {txt}")
    dbg.send(f"sh cp {src} {doc}")
    dbg.send(f"sh cp {src} {txt}")

    dbg.send(f"gui spawn {SPAWN_PATH} {doc}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline and not win:
        win = find_window(dbg, "np_md.md")
    res.check("a .md opens in Notepad", win is not None)
    if not win:
        return
    c = win["content"]
    dbg.settle()

    # THE WIDGET IS DECLARED AND VISIBLE: a hidden widget reports no
    # layout line, so the line's presence is the app saying the preview
    # is up -- asked, not guessed (docs/gui-guidelines.md).
    lines = dbg.logs("notepad: layout markdown", clear=False)
    res.check("a .md opens with the Markdown preview ON",
              bool(lines), "no 'layout markdown' line")

    band = (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])
    rendered = Image.open(qmp.screenshot(os.path.join(tmp, "np_md_render.png"))
                          ).convert("RGB").crop(band)

    key(dbg, "0x05")          # Ctrl-E -- back to the source
    dbg.settle()
    time.sleep(0.5)
    source = Image.open(qmp.screenshot(os.path.join(tmp, "np_md_source.png"))
                        ).convert("RGB").crop(band)

    diff = sum(1 for a, b in zip(rendered.tobytes(), source.tobytes()) if a != b)
    res.check("the rendered document and the source do not look the same",
              diff > 5000, f"{diff} bytes differ")

    lines_after = dbg.logs("notepad: layout markdown", clear=False)
    res.check("Ctrl-E hides the preview and shows the source",
              len(lines_after) == len(lines), "the widget still reports a layout")

    # A CONTROL: an ordinary .txt must NOT open rendered. Without this,
    # a preview that was simply always on would pass everything above.
    dbg.logs("notepad: layout markdown", clear=True)
    dbg.send(f"gui spawn {SPAWN_PATH} {txt}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    win2 = None
    while time.time() < deadline and not win2:
        win2 = find_window(dbg, "np_md.txt")
    dbg.settle()
    res.check("a .txt opens as TEXT, not rendered (the control)",
              win2 is not None and not dbg.logs("notepad: layout markdown", clear=False))


SECOND_CLOSE_FILE = "/tmp/np_second_close.txt"
SECOND_CLOSE_B = "np_second_b.txt"   # tab 2, saved by name into the chooser's folder
NOTEPAD_SESSION = "/var/lib/notepad/session"


def tabs_now(dbg):
    for line in reversed(dbg.logs("notepad: layout tabcount", clear=False)):
        n = line.split("tabcount")[1].split()
        return int(n[0]), int(n[2])
    return None


def held_closes(dbg):
    return len(dbg.logs("notepad: close while asking", clear=False))


def windows_of(dbg, pid):
    return [w for w in dbg.windows() if w.get("client_pid") == pid]


def main_window(dbg, pid):
    """The pid's own top-level window -- never another Notepad's."""
    return next((w for w in windows_of(dbg, pid)
                 if not w.get("title", "").startswith(("Save As", "Open", "Notepad Options"))), None)


def spawn_notepad(dbg, arg=""):
    """Spawn one Notepad; its pid, once ITS window is up (the first window
    whose pid no earlier window had)."""
    before = {w.get("client_pid") for w in dbg.windows()}
    dbg.send(f"gui spawn {SPAWN_PATH} {arg}".rstrip())
    w = poll(lambda: next((w for w in dbg.windows() if w.get("client_pid") not in before
                           and w.get("client_pid")), None), SPAWN_TIMEOUT_S)
    dbg.settle()
    return w.get("client_pid") if w else None


def kill_pid(dbg, pid):
    if pid:
        dbg.send(f"sh kill {pid}")
        poll(lambda: not windows_of(dbg, pid), 6)


def second_close_checks(dbg, res):
    """A WINDOW close while Notepad is asking something.

    A tab's prompt -- THE ONE EXCEPTION to "ignored": two dirty tabs, a
    file then an untitled one, Ctrl-W on the second, then Alt+F4. It used
    to walk the close to the FIRST dirty tab (switch_to() before
    ask_discard()'s guard), so Don't Save threw away the file's tab; then,
    once guarded, the close was dropped. Now the close is held and the
    session written, Don't Save discards the tab asked about, and the
    close goes on: the FILE is asked next.

    The Save As chooser a tab's Save opened: the close must switch no
    tab and ask nothing new under it.

    Options open, the window minimized: a single close is ignored by the
    app and BRINGS THE QUESTION FORWARD -- unminimized, the dialog in
    front."""
    pids = []
    try:
        second_close_tab(dbg, res, pids)
        second_close_chooser(dbg, res, pids)
        second_close_brings_forward(dbg, res, pids)
        end_task_is_quiet(dbg, res, pids)
    finally:
        for pid in pids:
            kill_pid(dbg, pid)
        dbg.send(f"sh rm {SECOND_CLOSE_FILE}")
        dbg.send(f"sh rm /{SECOND_CLOSE_B}")


def _click_ask(dbg, pid, i):
    w = main_window(dbg, pid)
    btns = dialog_buttons(dbg)
    if w and len(btns) == 3:
        x, y, bw, bh = btns[i]
        c = w["content"]
        dbg.send(f"gui click {c['x'] + x + bw // 2} {c['y'] + y + bh // 2}")


def second_close_tab(dbg, res, pids):
    dbg.logs("notepad: close while asking", clear=True)
    dbg.send(f"sh write {SECOND_CLOSE_FILE} kept")
    dbg.send(f"sh rm {NOTEPAD_SESSION}")
    pid = spawn_notepad(dbg, SECOND_CLOSE_FILE)
    pids.append(pid)
    base = SECOND_CLOSE_FILE.rsplit("/", 1)[-1]

    def title():
        w = main_window(dbg, pid)
        return w.get("title", "") if w else ""
    if not poll(lambda: title().endswith(base), SPAWN_TIMEOUT_S):
        res.check("second close: Notepad opens the file", False, f"title {title()!r}")
        return
    type_text(dbg, "x")
    poll(lambda: title() == "*" + SECOND_CLOSE_FILE, 6)
    key(dbg, "0x0e")                          # Ctrl-N: tab 2
    poll(lambda: tabs_now(dbg) == (2, 1), 6)
    type_text(dbg, "y")
    # NAMED, so the session would list it if it were written too early.
    key(dbg, CTRL_S)
    poll(lambda: any(w.get("title") == "Save As" for w in windows_of(dbg, pid)), 8)
    type_text(dbg, SECOND_CLOSE_B)
    key(dbg, ENTER)
    poll(lambda: title().endswith(SECOND_CLOSE_B), 8)
    type_text(dbg, "z")                       # and dirty again, so closing it asks
    ready = poll(lambda: title().startswith("*") and title().endswith(SECOND_CLOSE_B) and
                 tabs_now(dbg) == (2, 1), 6)
    key(dbg, "0x17")                          # Ctrl-W: asks about tab 2
    btns = poll(lambda: len(dialog_buttons(dbg)) == 3 and dialog_buttons(dbg), 6)
    res.check("second close: two dirty tabs, and closing the second asks",
              bool(ready) and bool(btns),
              f"title {title()!r}, tabs {tabs_now(dbg)}, buttons {btns}")
    if not btns:
        return
    dbg.send("gui key 0xa5 alt")              # Alt+F4 while that prompt is up
    held = poll(lambda: held_closes(dbg) >= 1, 6)
    res.check("...a window close while it asks is held, not acted on",
              bool(held) and tabs_now(dbg) == (2, 1), f"held {held}, tabs {tabs_now(dbg)}")
    _click_ask(dbg, pid, 1)                   # Don't Save
    nxt = poll(lambda: tabs_now(dbg) == (1, 0) and len(dialog_buttons(dbg)) == 3 and
               title().lstrip("*") == SECOND_CLOSE_FILE, 6)
    res.check("...Don't Save discards the tab asked about, and the close goes on to the file",
              bool(nxt), f"tabs {tabs_now(dbg)}, title {title()!r}, "
              f"buttons {len(dialog_buttons(dbg))}")
    # Written on the answer, AFTER that tab went: the file, never tab 2.
    session = dbg.send(f"sh cat {NOTEPAD_SESSION}")
    res.check("...and the session then lists the file, not the tab just closed",
              SECOND_CLOSE_FILE in session and SECOND_CLOSE_B not in session,
              f"`cat` returned {session[:200]!r}")
    _click_ask(dbg, pid, 1)                   # Don't Save, for the file too
    gone = poll(lambda: not windows_of(dbg, pid), 6)
    res.check("...and answering that closes the window", bool(gone),
              f"windows {[w['title'] for w in windows_of(dbg, pid)]}")


def second_close_chooser(dbg, res, pids):
    dbg.logs("notepad: close while asking", clear=True)
    pid = spawn_notepad(dbg)
    pids.append(pid)

    def title():
        w = main_window(dbg, pid)
        return w.get("title", "") if w else ""
    if not poll(lambda: title() == "untitled", SPAWN_TIMEOUT_S):
        res.check("chooser close: Notepad opens", False, f"title {title()!r}")
        return
    type_text(dbg, "a")
    poll(lambda: title() == "*untitled", 6)
    key(dbg, "0x0e")
    poll(lambda: tabs_now(dbg) == (2, 1), 6)
    type_text(dbg, "b")
    key(dbg, "0x17")                          # Ctrl-W on tab 2: asks
    poll(lambda: len(dialog_buttons(dbg)) == 3, 6)
    _click_ask(dbg, pid, 0)                   # Save -> untitled -> the chooser
    chooser = poll(lambda: next((w for w in windows_of(dbg, pid)
                                 if w.get("title") == "Save As"), None), 8)
    main = main_window(dbg, pid)
    res.check("chooser close: a tab's Save opens the Save As chooser",
              chooser is not None and main is not None,
              f"windows {[w['title'] for w in windows_of(dbg, pid)]}")
    if not chooser or not main:
        return
    dbg.send(f"gui close {main['z']}")        # a window close under the chooser
    held = poll(lambda: held_closes(dbg) >= 1, 6)
    dbg.settle()
    res.check("...a window close while the chooser is up is held: no tab switch, no new ask",
              bool(held) and tabs_now(dbg) == (2, 1) and not dialog_buttons(dbg),
              f"held {held}, tabs {tabs_now(dbg)}, buttons {dialog_buttons(dbg)}")


def end_task_is_quiet(dbg, res, pids):
    """End Task is a BATCH: Notepad with Options open behind a Calculator,
    `gui endtask` asks both of its windows once each and raises nothing --
    the Calculator stays in front."""
    pid = spawn_notepad(dbg)
    pids.append(pid)
    for k in ("0xA4", "0x96", "o"):           # F10, Right, o: Edit > Options...
        dbg.send(f"gui key {k}")
        dbg.settle(0.3)
    poll(lambda: any("Notepad Options" in w.get("title", "") for w in windows_of(dbg, pid)), 12)
    before = {w.get("client_pid") for w in dbg.windows()}
    dbg.send("gui spawn /bin/wm/apps/calculator")
    calc = poll(lambda: next((w for w in dbg.windows() if w.get("client_pid") not in before
                              and w.get("client_pid")), None), SPAWN_TIMEOUT_S)
    if calc:
        pids.append(calc["client_pid"])
    dbg.settle()

    def front():
        ws = dbg.windows()
        return max(ws, key=lambda w: w["z"]) if ws else {}
    ready = calc is not None and front().get("client_pid") == calc["client_pid"] and \
        len(windows_of(dbg, pid)) == 2
    res.check("end task: Notepad and its Options behind a Calculator", ready,
              f"front {front().get('title')!r}, notepad windows "
              f"{[w['title'] for w in windows_of(dbg, pid)]}")
    if not ready:
        return
    said = dbg.send(f"gui endtask {pid}")
    dbg.settle()
    res.check("...End Task asks both of its windows, once each, and raises nothing",
              "asked 2 window(s)" in said and
              front().get("client_pid") == calc["client_pid"],
              f"said {said.strip()[:80]!r}, front {front().get('title')!r}")


def second_close_brings_forward(dbg, res, pids):
    pid = spawn_notepad(dbg)
    pids.append(pid)
    for k in ("0xA4", "0x96", "o"):           # F10, Right, o: Edit > Options...
        dbg.send(f"gui key {k}")
        dbg.settle(0.3)
    opts = poll(lambda: next((w for w in windows_of(dbg, pid)
                              if "Notepad Options" in w.get("title", "")), None), 12)
    main = main_window(dbg, pid)
    if not opts or not main:
        res.check("bring forward: Notepad with Options open", False,
                  f"windows {[w['title'] for w in windows_of(dbg, pid)]}")
        return
    # Minimized from its taskbar button's window menu, as a person would.
    b = next((b for b in dbg.json("gui taskbar --json")["buttons"]
              if b.get("index") == main["z"]), None)
    if b:
        dbg.rclick(b["cx"], b["cy"])
        row = dbg.ctxmenu_row("Minimize")
        if row:
            dbg.click(*row)
    mini = poll(lambda: (main_window(dbg, pid) or {}).get("state") == "minimized", 6)
    res.check("bring forward: Notepad, Options open, minimized",
              bool(mini), f"state {(main_window(dbg, pid) or {}).get('state')}")
    if not mini:
        return
    dbg.send(f"gui close {main_window(dbg, pid)['z']}")   # a SINGLE close

    def forward():
        ws = dbg.windows()
        top = max(ws, key=lambda w: w["z"]) if ws else {}
        m = main_window(dbg, pid) or {}
        return m.get("state") == "normal" and "Notepad Options" in top.get("title", "")
    ok = poll(forward, 6)
    ws = dbg.windows()
    res.check("...a single close is ignored and brings it forward: unminimized, Options in front",
              bool(ok) and main_window(dbg, pid) is not None,
              f"state {(main_window(dbg, pid) or {}).get('state')}, "
              f"front {max(ws, key=lambda w: w['z'])['title'] if ws else None!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--only", choices=("second-close",),
                    help="run only this section")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "notepad_client_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        if not args.only:
            run(dbg, qmp, args.tmp, args.shot, res)
        second_close_checks(dbg, res)
    finally:
        dbg.close()

    print(f"\nnotepad_client_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
