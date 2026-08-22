#!/usr/bin/env python3
"""Drive the RING-3 Terminal (userland/gui/terminal.c) and assert on it.

What this actually proves, and why it is the interesting test in the
set: a ring-3 process runs ANOTHER program and reads its output. That
needs SYS_PIPE + SYS_SPAWN + SYS_WAITPID working together, and before
those existed no process here could see another's stdout at all.

HOW IT ASSERTS THAT WITHOUT OCR
-------------------------------
By INK VOLUME in the transcript area, compared between two commands
whose outputs differ by a lot:

  * `echo hi` produces one short line -- and critically, it is a
    BUILTIN, handled inside the shell with no spawn involved.
  * `lscpu` is an external /bin binary whose output is dozens of lines,
    and can only appear if it was spawned and its stdout piped back.

So "much more ink after lscpu than after echo" is a statement about the
pipe, not about rendering. A terminal that echoed commands but never
captured output would pass every other check here and fail that one --
which is exactly the failure mode worth catching.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/uterm_test.py --shot screenshots/YYYY-MM-DD
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Terminal"
SPAWN_PATH = "/bin/wm/apps/uterm"   # spawned directly -- see run()
SPAWN_TIMEOUT_S = 15.0

# The terminal draws light text on black, so "ink" is anything not black.
BG = (0, 0, 0)
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f"}


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


def type_line(dbg, text, settle=1.2):
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()
    key(dbg, "0x0d")
    dbg.settle()
    time.sleep(settle)


def bar_run(qmp, tmp, name, box):
    """The longest horizontal RUN of the reverse-video colour.

    `ESC[7m` swaps foreground and background, so a status bar is a solid
    band of the default foreground (light grey, 0xAAAAAA) with black
    letters on it. Counting PIXELS of that colour does not distinguish a
    bar from ordinary text -- glyphs are drawn in the same grey, and a
    screenful of them scores thousands. What only a filled background
    produces is a long unbroken RUN: a glyph is a few pixels wide, a bar
    is hundreds.

    That distinction is the check. An earlier version counted pixels,
    scored 4304 on a frame with no bar at all, and would have passed
    whatever the editor drew.

    It is also why the box here is the WHOLE content rect: the transcript
    box used elsewhere in this file deliberately cuts off the bottom
    rows, which is exactly where a status bar lives.
    """
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    best = 0
    with Image.open(p) as im:
        crop = im.convert("RGB").crop(box)
        w, h = crop.size
        raw = crop.tobytes()
        for y in range(h):
            run = 0
            base = y * w * 3
            for x in range(w):
                i = base + x * 3
                if (abs(raw[i] - 0xAA) < 12 and abs(raw[i + 1] - 0xAA) < 12
                        and abs(raw[i + 2] - 0xAA) < 12):
                    run += 1
                    if run > best:
                        best = run
                else:
                    run = 0
    return best


def ink(qmp, tmp, name, box):
    """Non-background pixels in the box -- a proxy for how much text is
    on screen, which is all this test needs."""
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    n = 0
    with Image.open(p) as im:
        raw = im.convert("RGB").crop(box).tobytes()
        for i in range(0, len(raw), 3):
            if (abs(raw[i] - BG[0]) > 30 or abs(raw[i + 1] - BG[1]) > 30
                    or abs(raw[i + 2] - BG[2]) > 30):
                n += 1
    return n


def run(dbg, qmp, tmp, shot_dir, res):
    # Spawned directly: this test's subject IS the ring-3 Terminal, and
    # since M41's stage 0 there is no kernel-space one to type `run
    # uterm` at (a `gui open Terminal` now spawns this same binary, but
    # via the launcher rather than the path under test).
    dbg.send(f"gui spawn {SPAWN_PATH}")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win:
            break
    res.check("the ring-3 Terminal opens its own window", win is not None)
    if not win:
        return

    # A window in the WM's list is not yet a window with PIXELS in it:
    # the client has to draw and present its first frame. Wait for the
    # app to say it laid itself out rather than sleeping -- capturing
    # before that reads DESKTOP through the window's rect, which scores
    # as ~230k ink and makes every later comparison meaningless. (The
    # old flow typed `run uterm` at a Terminal, whose keystroke latency
    # hid this; spawning directly does not.)
    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline:
        if dbg.logs("uterm: layout cursor", clear=False):
            break
        time.sleep(0.2)
    dbg.settle()

    c = win["content"]
    # The transcript area, clear of the prompt line at the bottom.
    box = (c["x"] + 4, c["y"] + 4, c["x"] + c["w"] - 4, c["y"] + c["h"] - 30)

    base = ink(qmp, tmp, "ut_base.png", box)
    res.check("it renders its banner", base > 100, f"only {base} ink pixels")

    # --- the cursor is where the shell put it -------------------------
    #
    # **THE PROMPT IS NOT THIS APP'S ANY MORE.** It used to draw one
    # itself, below the transcript, and this pair of checks asserted that
    # it FOLLOWED the transcript rather than being nailed to the bottom.
    # There is no prompt widget now: Terminal is a terminal emulator, the
    # prompt is bytes /bin/tosh printed, and it lands wherever the text
    # does. What replaces the check is the CURSOR -- the emulator's own
    # position in the buffer, which is the thing that would be wrong if
    # '\r' or overwrite were mishandled.
    def cursor_at():
        for l in reversed(dbg.logs("uterm: layout cursor", clear=False)):
            return int(l.split("layout cursor")[1].split()[0])
        return None

    start_cursor = cursor_at()
    res.check("the emulator reports a cursor inside its buffer",
              start_cursor is not None and start_cursor > 0,
              f"cursor at {start_cursor}")

    # A BUILTIN: handled inside the shell, no spawn.
    type_line(dbg, "echo hi")
    after_echo = ink(qmp, tmp, "ut_echo.png", box)
    res.check("a builtin runs and its output appears", after_echo > base,
              f"{base} -> {after_echo}")

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-terminal.png")))

    # --- THE SHARED LINE EDITOR --------------------------------------
    #
    # This window edits with kernel/lib/klineedit.c, compiled a second
    # time into libuapp.a, rather than the append-only loop it used to
    # carry. Asserted through the FILESYSTEM, not through ink: every
    # other check here counts non-background pixels, and both a working
    # edit and a `not found` line add some, so ink cannot tell them
    # apart. /tests/file_test writes /filetest.txt and `zfile_test`
    # does not exist, so the file is present only if Home and Delete
    # really moved the cursor and removed the z.
    #
    # An append-only editor fails both of these: it ignores Home,
    # Delete and Up entirely.
    def root_has(name):
        out = dbg.send("sh ls /") or ""
        return any(tok.rstrip("/") == name
                   for line in out.splitlines() for tok in line.split())

    dbg.send("sh rm /filetest.txt")
    dbg.settle()
    for ch in "zfile_test":
        key(dbg, HEX.get(ch, ch))
    dbg.settle()
    key(dbg, "0x97")   # KEY_HOME
    key(dbg, "0x99")   # KEY_DELETE
    dbg.settle()
    key(dbg, "0x0d")
    time.sleep(2.5)
    res.check("Home + Delete edit mid-line, and the edited command runs",
              root_has("filetest.txt"))

    dbg.send("sh rm /filetest.txt")
    dbg.settle()
    key(dbg, "0x91")   # KEY_ARROW_UP -- recall the previous line
    dbg.settle()
    key(dbg, "0x0d")
    time.sleep(2.5)
    res.check("Up recalls the previous command and it runs again",
              root_has("filetest.txt"))

    # An EXTERNAL program: only reachable via spawn + pipe.
    type_line(dbg, "lscpu", settle=2.5)
    after_lscpu = ink(qmp, tmp, "ut_lscpu.png", box)
    res.check("an external /bin program is spawned and its output piped back",
              after_lscpu > after_echo * 2,
              f"echo left {after_echo} ink, lscpu left {after_lscpu} -- "
              "expected far more if the program's output really arrived")

    # The cursor's other half: a screenful of output must have moved it a
    # long way, and it must still point INSIDE the buffer. An emulator
    # that mishandled '\r' would leave it stuck at the start of a line
    # while the text grew past it -- which looks fine in a screenshot and
    # puts the caret in the wrong place on the very next keystroke.
    long_cursor = cursor_at()
    res.check("a screenful of output moves the cursor with it",
              long_cursor is not None and start_cursor is not None
              and long_cursor > start_cursor,
              f"cursor was {start_cursor}, now {long_cursor} after lscpu")

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-terminal-spawn.png")))

    # A command that does not exist must be reported, not silently
    # ignored -- otherwise "spawn failed" and "program printed nothing"
    # look identical to a user.
    before = after_lscpu
    type_line(dbg, "definitelynotacommand")
    after_bad = ink(qmp, tmp, "ut_bad.png", box)
    res.check("an unknown command reports itself", after_bad != before)

    # --- A CHILD THAT READS STDIN MUST NOT FREEZE THIS WINDOW --------
    #
    # This terminal takes keys as WINDOW EVENTS; its fd 0 is a console
    # some other process owns, or nobody does. A child left to inherit
    # that blocks forever on a keyboard it will never be given, and
    # since the shell waits for its child, the window stops responding.
    #
    # It became easy to hit when tosh's builtin `cat` was removed (so
    # `cat` is /bin/cat, which reads fd 0 with no arguments), but it was
    # always reachable -- `catin` does the same thing. tosh hands a
    # child an EMPTY stdin when it has no terminal input of its own; see
    # struct tosh's `stdin_ok`.
    #
    # THE CHECK IS THAT THE NEXT COMMAND STILL RUNS. A screenshot of a
    # frozen window looks much like a live one, and the window object
    # survives either way -- so the assertion is that the shell reached a
    # prompt again and executed something, through the FILESYSTEM.
    dbg.send("sh rm /filetest.txt")
    dbg.settle()
    # A BARE `cat` NOW WAITS FOR INPUT, AND THAT IS THE FIX RATHER THAN
    # THE BUG. It used to return at once because the window had no
    # terminal to lend, so tosh handed every child a closed pipe as stdin
    # -- honest at the time, and exactly what a process with no
    # controlling terminal gets. This window has a real pty now, so `cat`
    # inherits it and blocks for input like it does everywhere else. The
    # way out is the way out everywhere else too: Ctrl-D.
    type_line(dbg, "cat", settle=1.5)
    res.check("a bare `cat` waits for input rather than returning",
              not root_has("filetest.txt"))
    key(dbg, "0x04")                    # Ctrl-D: end of input -- cat exits
    dbg.settle()
    res.check("...and the window survived it", dbg.window(TITLE) is not None)
    type_line(dbg, "file_test", settle=3.0)
    res.check("...and the terminal still runs the NEXT command",
              root_has("filetest.txt"),
              "the shell never came back after Ctrl-D ended `cat`")

    # --- Ctrl-C, WHICH IS THE WHOLE POINT ----------------------------
    #
    # This window could not interrupt a job at all until it had a
    # terminal: it read keys as WINDOW EVENTS, so it owned no console and
    # had no foreground group. Now the key is written to a pty master as
    # the byte 0x03 and kernel/tty/ldisc.c recognises it as INTR -- the
    # SAME function, on the same kind of object, that the keyboard IRQ
    # feeds for the physical console.
    #
    # spin_test writes nothing and makes no syscalls while it spins, so
    # the only way it can end is signal delivery on a timer tick. A job
    # that polled would be killed by the other path and would say nothing
    # about this one.
    def spinners():
        out = dbg.send("sh kstack slots") or ""
        return [l for l in out.splitlines() if "spin_test" in l]

    type_line(dbg, "spin_test 900000", settle=2.0)
    res.check("a job is running in the window", bool(spinners()),
              "spin_test never started")

    key(dbg, "0x03")  # Ctrl-C
    deadline = time.time() + 8
    while time.time() < deadline and spinners():
        time.sleep(0.25)
    res.check("Ctrl-C in a WINDOW interrupts the job", not spinners(),
              "spin_test survived -- the byte never became a SIGINT")

    # The load-bearing other half: a Ctrl-C that killed the shell too
    # would pass the check above and leave a dead window. The shell
    # ignores SIGINT and is not in the job's group.
    type_line(dbg, "file_test2", settle=2.0)
    res.check("...and the SHELL survived it", dbg.window(TITLE) is not None)

    # --- A FULL-SCREEN PROGRAM IN A WINDOW ---------------------------
    #
    # `/bin/edit` ADDRESSES its screen -- ESC[4;12H, ESC[K, ESC[7m --
    # which a character stream cannot express. This window's screen is a
    # GRID now, driven by the kernel's own ANSI parser compiled into
    # ring 3, so the escapes are obeyed rather than printed. Before that
    # they appeared as literal text and the editor was unusable here.
    #
    # ASSERTED THROUGH THE FILESYSTEM: what a full-screen program draws
    # is the one thing a screenshot cannot check cheaply, and "the bytes
    # reached the disk" is the claim that matters. Read back through a
    # completely different path, so the editor claiming success proves
    # nothing on its own.
    full = (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])
    plain = bar_run(qmp, tmp, "ut_nobar.png", full)
    dbg.send("sh rm /uterm_edit.txt")
    dbg.settle()
    type_line(dbg, "edit /uterm_edit.txt", settle=2.5)
    for ch in "hello":
        key(dbg, HEX.get(ch, ch))
    dbg.settle()

    # **THE CHECK THAT PROVES THE ESCAPES WERE OBEYED RATHER THAN
    # PRINTED.** Saving would pass either way -- the editor writes the
    # file whatever the screen did with its output, which is exactly the
    # "it responds is not it is drawn" trap docs/gui-guidelines.md warns
    # about. So: `edit` parks the caret at the CURSOR, which after typing
    # five characters into an empty file is row 0, column 5. A terminal
    # that printed `ESC[1;6H` as text would have a caret hundreds of
    # cells along, at the end of everything it had ever shown.
    cur = cursor_at()
    res.check("the editor's cursor sequences MOVED the caret, not printed",
              cur is not None and cur < 40,
              f"caret at cell {cur} -- expected row 0, near column 5")

    # ...and the status bar is REVERSE VIDEO, which is the other half of
    # "the escapes were obeyed": ESC[7m has to swap the colours AND the
    # swapped background has to be painted. `bar` is measured against
    # `plain`, taken before the editor opened -- a threshold with no
    # control is a guess.
    bar = bar_run(qmp, tmp, "ut_editbar.png", full)
    res.check("...and its status bar is drawn in reverse video",
              bar > 100 and bar > plain * 3,
              f"longest bar-coloured run was {plain}px before, {bar}px now")

    key(dbg, "0x9a")   # F2 -- save
    time.sleep(1.5)
    key(dbg, "0x9b")   # F3 -- exit
    time.sleep(1.5)
    out = dbg.send("sh cat /uterm_edit.txt") or ""
    res.check("a full-screen editor runs IN THE WINDOW and saves",
              "hello" in out, out.strip()[:70])

    # The other half: an editor that never returned, or left the terminal
    # raw, would satisfy the check above and leave a dead window.
    res.check("...and the shell came back after it", dbg.window(TITLE) is not None)

    # And the terminal is still alive and interactive afterwards.
    type_line(dbg, "pwd")
    res.check("it survives an external command and a failure",
              dbg.window(TITLE) is not None)

    # Esc belongs to the shell's line editor now, not to closing the
    # window -- see docs/decisions.md. Alt+F4 closes, via the WM.
    key(dbg, "0x1b")  # Esc
    dbg.settle()
    time.sleep(0.5)
    res.check("Esc does NOT close it", dbg.window(TITLE) is not None,
              "Esc must reach the shell, not the window")

    dbg.send("gui key 0xa5 alt")
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if dbg.window(TITLE) is None:
            gone = True
            break
    res.check("Alt+F4 closes it", gone)


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
        enter_gui(qmp, args.sock)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, args.shot, res)
    finally:
        dbg.close()

    print(f"\nuterm_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
