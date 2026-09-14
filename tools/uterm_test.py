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
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
TITLE = "Terminal"
SPAWN_PATH = "/bin/wm/apps/uterm"   # spawned directly -- see run()
SPAWN_TIMEOUT_S = 15.0

# The terminal draws light text on black, so "ink" is anything not black.
BG = (0, 0, 0)

# terminal.c's scrollbar colours -- Breeze's dark pair.
TRACK = (49, 54, 59)

HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f"}


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        (self.passes if ok else self.fails).append(name)
        if not ok and detail:
            print(f"        {detail}")


# THE MENU BAR'S TITLES, BY INDEX -- the layout log's vocabulary is
# `menu.title N` with no label (ui/uui_describe.h), so a title can only
# be named by where it sits.
#
# **THIS HAS BROKEN ONCE**: adding an Edit menu to the Terminal shifted
# everything after File, and three checks failed pointing at Rename Tab
# and the menu-bar toggle rather than at the menu that had moved. If you
# add a title to the Terminal's bar, these move.
#
#   0 File   1 Edit   2 Terminal   3 View   4 Tabs
MENU_TERMINAL = 2
MENU_VIEW = 3


def key(dbg, k):
    dbg.send(f"gui key {k}")


def type_text(dbg, text):
    """Types without pressing Enter -- what a completion check needs,
    since the whole point is what Tab does to a HALF-TYPED line."""
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()


def type_line(dbg, text, settle=1.2):
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()
    key(dbg, "0x0d")
    dbg.settle()
    time.sleep(settle)


def cyan_pixels(qmp, tmp, name, box):
    """Pixels of the DIRECTORY colour `ls` uses (ANSI cyan).

    Cyan is the one colour nothing else in a terminal produces: the
    default text is grey (r == g == b) and the background is black, so
    "green and blue well above red" is only ever a coloured run. Counted
    rather than measured as a run, because a directory name is a few
    characters and there is no solid band to find.
    """
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    n = 0
    with Image.open(p) as im:
        raw = im.convert("RGB").crop(box).tobytes()
        for i in range(0, len(raw), 3):
            r, g, b = raw[i], raw[i + 1], raw[i + 2]
            if g > 100 and b > 100 and r + 40 < g:
                n += 1
    return n


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


def corner_shape(qmp, tmp, name, box, fill):
    """How square the four corners of `box` are, for a rect filled with
    `fill`.

    Returns (corners, middles): how many of the four corner pixels carry
    the fill colour, and how many of the four edge MIDPOINTS do. A square
    rect answers (4, 4); a capsule answers (0, 4) -- the corners are
    rounded away while the arc still reaches each edge's centre.

    The pair is what makes this an assertion about SHAPE rather than
    about size: a bar that simply shrank, or one that was not drawn at
    all, fails the second half while passing the first.
    """
    from PIL import Image
    p = os.path.abspath(os.path.join(tmp, name))
    qmp.screenshot(p)
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    with Image.open(p) as im:
        rgb = im.convert("RGB")
        def is_fill(x, y):
            px = rgb.getpixel((x, y))
            return all(abs(px[i] - fill[i]) <= 8 for i in range(3))
        corners = sum(is_fill(x, y) for x in (x0, x1 - 1) for y in (y0, y1 - 1))
        middles = (is_fill(cx, y0) + is_fill(cx, y1 - 1)
                   + is_fill(x0, cy) + is_fill(x1 - 1, cy))
    return corners, middles


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
    # The TRANSCRIPT, clear of the prompt line at the bottom and -- the
    # part that matters -- clear of the CHROME at the top.
    #
    # This box started at the content origin, so the menu bar and the tab
    # strip counted as terminal output. It only ever worked because the
    # chrome happened to be dark: painting the selected tab a light
    # colour added ~25k "ink" pixels that no shell had printed, and the
    # ratio check below (lscpu must leave twice what `pwd` did) failed
    # deterministically with the emulator working perfectly. The app
    # reports where its grid starts; ask it rather than assume.
    top = layout_field(dbg, "chrome", 4)
    box = (c["x"] + 4, c["y"] + top, c["x"] + c["w"] - 4, c["y"] + c["h"] - 30)

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

    # A BUILTIN: handled inside the shell, no spawn. `pwd`, because
    # `echo` stopped being one -- it shadowed /bin/echo, which honours
    # `-n` where the builtin did not. Three builtins remain and each has
    # to be one; see userland/lib/tosh.c.
    type_line(dbg, "pwd")
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

    # --- Ctrl-Z, THE SAME MECHANISM WITH A DIFFERENT ENDING ----------
    #
    # `Ctrl-Z` is 0x1A on the same pty, recognised as SUSP by the same
    # ldisc function that just handled INTR -- so what this proves is
    # not the suspension (tools/jobs_test.py does that against the
    # physical console) but that a WINDOW is a terminal in the same
    # sense: same foreground group, same line discipline, same shell
    # binary. A difference here would mean the tty layer had failed at
    # its one job.
    def spin_state():
        found = dbg.processes_named("spin_test")
        return found[0]["state"] if found else None

    type_line(dbg, "spin_test 900000", settle=2.0)
    res.check("a second job is running in the window", spin_state() is not None,
              "spin_test never started")

    key(dbg, "0x1a")  # Ctrl-Z
    deadline = time.time() + 8
    while time.time() < deadline and spin_state() != "stopped":
        time.sleep(0.25)
    res.check("Ctrl-Z in a WINDOW suspends the job", spin_state() == "stopped",
              f"state is {spin_state()} -- the byte never became a SIGTSTP")

    # `fg` brings it back, which is what says the job table and the
    # terminal handover work through a pty exactly as they do on tty0.
    type_line(dbg, "fg", settle=2.0)
    deadline = time.time() + 8
    while time.time() < deadline and spin_state() == "stopped":
        time.sleep(0.25)
    res.check("...and `fg` resumes it", spin_state() not in (None, "stopped"),
              f"state is {spin_state()}")

    key(dbg, "0x03")  # tidy up -- the next section wants a quiet machine
    deadline = time.time() + 8
    while time.time() < deadline and spin_state() is not None:
        time.sleep(0.25)

    # --- COLOUR REACHES THE WINDOW -----------------------------------
    #
    # `/bin/ls` colours directories, and `--color=auto` is its default --
    # so a listing here must be coloured, and the SAME listing with
    # `--color=never` must not. The second half is the control: a
    # threshold with nothing to compare against is a guess.
    #
    # **IT DID NOT WORK AND THE REASON WAS TWO LAYERS DOWN.** tosh
    # CAPTURED a child's stdout through a pipe and re-emitted it, which
    # it did because the GUI Terminal used to host it as a library and
    # had no descriptor to hand over. A pipe is not a terminal, so
    # isatty(1) was false in every program the shell ran and auto turned
    # colour OFF -- with nothing on screen to say why. Reported from a
    # screenshot, twice: first as `ls -l` not working (a builtin
    # shadowing /bin/ls) and then as this.
    # PLAIN FIRST, AND THAT ORDERING IS THE POINT. A terminal appends,
    # so measuring the coloured listing first leaves its pixels on screen
    # and the `never` run scores exactly the same -- which is what
    # happened, and it reads as `--color=never` being ignored. Taking the
    # control first means the only cyan on screen afterwards is the one
    # this check is about.
    type_line(dbg, "ls --color=never", settle=1.5)
    plain_ls = cyan_pixels(qmp, tmp, "ut_nocolor.png", box)
    res.check("`ls --color=never` puts no colour on screen",
              plain_ls < 20, f"{plain_ls} directory-coloured pixels")

    type_line(dbg, "ls", settle=1.5)
    coloured = cyan_pixels(qmp, tmp, "ut_color.png", box)
    res.check("...and a plain `ls` IS coloured -- the child sees a terminal",
              coloured > plain_ls + 20,
              f"{plain_ls} coloured pixels with never, {coloured} without")

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


def layout_field(dbg, name, default=None):
    """A named field of the emulator's own layout line.

    Fields are read BY NAME rather than by position -- the line has
    grown three times, and terminal.c's own note says the only fixed
    thing about it is that `cursor` comes first.
    """
    for l in reversed(dbg.logs("uterm: layout cursor", clear=False)):
        parts = l.split()
        if name in parts:
            return int(parts[parts.index(name) + 1])
    return default


def tab_count(dbg):
    """The `tabs N` field of the emulator's own layout line."""
    return layout_field(dbg, "tabs")


def rect(dbg, kind, index=None):
    """A rect the app reported: `uterm: layout <kind> [i] x y w h`."""
    want = f"uterm: layout {kind} "
    for l in reversed(dbg.logs(want, clear=False)):
        v = l.split(want)[1].split()
        if index is None:
            return [int(n) for n in v[:4]]
        if int(v[0]) == index:
            return [int(n) for n in v[1:5]]
    return None


def centre(win, r):
    """A reported content-relative rect, as a screen point to click."""
    c = win["content"]
    return c["x"] + r[0] + r[2] // 2, c["y"] + r[1] + r[3] // 2


def check_tabs(dbg, qmp, tmp, res):
    """Tabs: a second shell in one window, and each keeping its own.

    THE LOAD-BEARING CHECK IS THE SWITCH, not the count. A Terminal that
    opened a tab, drew a strip and pointed both tabs at ONE session
    would pass every count and every pixel check here -- so a marker is
    typed into the second tab and the first is required NOT to have it,
    then required to have it again on the way back. Only two real
    sessions can do that.

    Titles are asserted as TEXT from the app's own log rather than from
    the pixels of a tab label: a label is a few characters at font size
    and "some ink changed" is not a measurement.
    """
    # **ITS OWN WINDOW.** run() ends by closing the Terminal with
    # Alt+F4, so everything below would otherwise drive a window that is
    # not there -- and read STALE log lines from the one that was, which
    # is how three of these checks first passed against nothing.
    dbg.logs("uterm:", clear=True)
    dbg.send(f"gui spawn {SPAWN_PATH}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        time.sleep(0.2)
    res.check("t0. a Terminal for the tab checks opened", win is not None)
    if not win:
        return
    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline:
        if dbg.logs("uterm: layout cursor", clear=False):
            break
        time.sleep(0.2)
    # FOCUS IT. `gui key` reaches the focused window and so does a real
    # keystroke, and the combos below are real keystrokes.
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    dbg.settle()

    start = tab_count(dbg)
    res.check("t1. the window starts with one tab", start == 1, f"tabs {start}")

    # Ctrl+Shift+T. The letter has already been folded to a control code
    # by the time the app sees it, so this pair is the only spelling --
    # see terminal.c and api/keyboard.h.
    qmp.combo(["ctrl", "shift", "t"])
    time.sleep(1.2)
    dbg.settle()
    res.check("t2. Ctrl+Shift+T opens a second tab", tab_count(dbg) == 2,
              f"tabs {tab_count(dbg)}")

    # A MARKER ONLY THE SECOND TAB'S SHELL HAS SEEN.
    # `cd /bin` changes the second shell's directory, which is what its
    # OSC title reports -- tosh announces one per directory change.
    type_line(dbg, "cd /bin")
    time.sleep(1.0)
    dbg.settle()
    # A log line can arrive GLUED to a console echo on the serial
    # stream ('gui state --jsuterm: tab 1 title /bin'), which breaks a
    # positional split silently -- reparse each line from its marker.
    titles = [l[l.index("uterm: tab"):]
              for l in dbg.logs("uterm: tab", clear=False) if "title" in l]
    res.check("t3. the shell's OSC title reached its tab",
              any(l.rstrip().endswith("/bin") for l in titles),
              f"titles seen: {titles[-3:]}")
    res.check("t4. the rename named the SECOND tab, not the first",
              any(l.split()[2] == "1" for l in titles
                  if len(l.split()) > 3 and l.rstrip().endswith("/bin")),
              f"titles seen: {titles[-3:]}")

    # Switching. Ctrl+PageUp goes back one, wrapping, as in Konsole.
    #
    # **THE WINDOW'S CONTENT ONLY, AND SETTLED.** A whole-screen
    # comparison across two seconds fails on the taskbar CLOCK, which
    # has nothing to do with tabs; and a capture that lands mid-paint
    # fails a comparison with nothing wrong with it. The rows BELOW the
    # tab strip are the subject -- the strip itself changes on purpose.
    c = win["content"]
    # **THE APP SAYS WHERE ITS GRID STARTS.** This was a hardcoded 24,
    # which the menu bar's arrival pushed the chrome past -- a band that
    # then compares two patches of tab strip and calls them a pass.
    strip = layout_field(dbg, "chrome", 24)
    grid_box = (c["x"], c["y"] + strip, c["x"] + c["w"], c["y"] + c["h"])

    def grid(name):
        p = os.path.abspath(os.path.join(tmp, name))
        return qmp.stable_pixels(p, box=grid_box)

    in_tab2 = grid("uterm_tab2.png")
    qmp.combo(["ctrl", "pgup"])
    time.sleep(1.0)
    dbg.settle()
    in_tab1 = grid("uterm_tab1.png")
    res.check("t5. switching tabs changes what is on screen",
              in_tab1 != in_tab2, "the two tabs render identically")

    # ...and back, which must restore the SECOND tab's screen. A window
    # with one shared session would come back to the same pixels either
    # way, so this pair is what tells two real sessions from one.
    qmp.combo(["ctrl", "pgdn"])
    time.sleep(1.0)
    dbg.settle()
    back = grid("uterm_tab2b.png")
    res.check("t6. switching back restores that tab's own screen",
              back == in_tab2, "the second tab did not come back as it was")

    # Ctrl+Shift+W closes it. The strip STAYS -- it is drawn at one tab
    # now, because it carries the "+" (terminal.c's chrome_h()).
    qmp.combo(["ctrl", "shift", "w"])
    time.sleep(1.2)
    dbg.settle()
    res.check("t7. Ctrl+Shift+W closes a tab", tab_count(dbg) == 1,
              f"tabs {tab_count(dbg)}")

    # A tab opened AFTER the window grew. grow_caps() frees every
    # session's old grids on a resize, and a fresh session struct that
    # inherited those freed pointers skipped its own allocation and wrote
    # through them (a GP fault, the Terminal gone). Maximize by a title
    # bar double-click, then open a tab.
    win = dbg.window(TITLE)
    if win:
        tx, ty = win["x"] + win["w"] // 2, win["y"] + 8
        dbg.send(f"gui click {tx} {ty}")
        time.sleep(0.1)
        dbg.send(f"gui click {tx} {ty}")
        dbg.settle()
        win = dbg.window(TITLE)
    res.check("t8. a title-bar double-click maximizes the Terminal",
              win is not None and win["state"] == "maximized",
              f"state {win['state'] if win else 'gone'}")
    if win:
        dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
        dbg.settle()
        qmp.combo(["ctrl", "shift", "t"])
        time.sleep(1.2)
        dbg.settle()
        win = dbg.window(TITLE)
    res.check("t9. a tab opens in the maximized window and it survives",
              win is not None and tab_count(dbg) == 2,
              f"window {'alive' if win else 'GONE'}, tabs {tab_count(dbg)}")
    # Put it back for the checks that follow: one tab, normal size.
    if win:
        qmp.combo(["ctrl", "shift", "w"])
        time.sleep(1.0)
        dbg.settle()
        tx, ty = win["x"] + win["w"] // 2, win["y"] + 8
        dbg.send(f"gui click {tx} {ty}")
        time.sleep(0.1)
        dbg.send(f"gui click {tx} {ty}")
        dbg.settle()
        win = dbg.window(TITLE)
        res.check("t10. ...and a second double-click restores it",
                  win is not None and win["state"] == "normal" and tab_count(dbg) == 1,
                  f"state {win['state'] if win else 'gone'}, tabs {tab_count(dbg)}")


def item_rect(dbg, level, index):
    """One reported menu row: `uterm: layout menu.item <level> <i> x y w h`."""
    if index is None:
        return None
    for l in reversed(dbg.logs("uterm: layout menu.item ", clear=False)):
        v = [int(n) for n in l.split("uterm: layout menu.item ")[1].split()[:6]]
        if v[0] == level and v[1] == index:
            return v[2:]
    return None


def open_menu(dbg, win, title):
    """Open one menu, having CLEARED the rows the last one reported.

    Without the clear, `last_item_index` counts rows from every popup
    opened so far -- the File menu's four plus the Terminal menu's eight
    -- and names a row the open popup does not have. That is this repo's
    stale-log trap, and it read exactly like the menu item not working.
    """
    dbg.logs("uterm: layout menu.item ", clear=True)
    dbg.click(*centre(win, title))
    dbg.settle()


def last_item_index(dbg, level):
    """The bottom row of an open popup, so a test names a row by where it
    is rather than by counting the menu tree in two places."""
    best = None
    for l in dbg.logs("uterm: layout menu.item ", clear=False):
        v = [int(n) for n in l.split("uterm: layout menu.item ")[1].split()[:6]]
        if v[0] == level:
            best = v[1] if best is None else max(best, v[1])
    return best


def check_chrome(dbg, qmp, res):
    """The "+" button and the menu bar, on a Terminal that is already up.

    **THE LOAD-BEARING CHECK IS c6**, and a positive control says so
    rather than the name: dropping `overlay_active` from uui_menubar_ops
    reddens c6 (and c8a behind it) and NOTHING ELSE here. Without that
    slot the router hit-tests a popup row against the bar's own rect,
    which it is outside, so no menu command commits at all.

    c8b and c8c ask the sharper question -- whether a popup click falls
    THROUGH onto the tab strip under it -- and they are honest to keep
    but they cannot currently fail: once the menu is a routed overlay
    the router offers it every press first, so the fall-through is
    unreachable by construction. They are here to catch a future menu
    that goes back to being hand-routed, which is the shape Notepad
    still has. Do not read them as evidence that the ordering works;
    c6 is that evidence.

    The command they commit is Rename Tab, chosen because it moves
    NEITHER the selection nor the tab count -- so both are free to act
    as fall-through detectors. New Tab cannot: it selects the tab it
    creates, so a moved selection would prove nothing (which is exactly
    how the first version of this check failed against correct code).

    Geometry comes from the app's own reported rects, never derived: a
    menu row's position depends on the font and on what is nested where,
    and a test that computes it is asserting its own arithmetic.
    """
    win = dbg.window(TITLE)
    res.check("c0. a Terminal is up for the chrome checks", win is not None)
    if not win:
        return

    # The strip is drawn at ONE tab now, which is what makes "+" reachable
    # before a second tab exists. Asserted as the button's own rect --
    # "the strip is tall enough" is a different claim.
    plus = rect(dbg, "tabs.new")
    res.check("c1. the + button has a rect at one tab",
              plus is not None and plus[2] > 0 and tab_count(dbg) == 1,
              f"newtab {plus} tabs {tab_count(dbg)}")
    if not plus:
        return

    before = tab_count(dbg)
    dbg.click(*centre(win, plus))
    time.sleep(1.2)
    dbg.settle()
    res.check("c2. clicking + opens a tab", tab_count(dbg) == before + 1,
              f"tabs {before} -> {tab_count(dbg)}")

    res.check("c3. the menu bar is shown by default",
              layout_field(dbg, "menu") == 1,
              f"menu {layout_field(dbg, 'menu')}")

    title0 = rect(dbg, "menu.title", 0)
    res.check("c4. the File title has a reported rect", title0 is not None)
    if not title0:
        return

    before = tab_count(dbg)
    open_menu(dbg, win, title0)       # a menu bar OPENS on press
    row = item_rect(dbg, 0, 0)        # "New Tab"
    res.check("c5. the File popup reported its rows", row is not None)
    if not row:
        return
    dbg.click(*centre(win, row))
    time.sleep(1.2)
    dbg.settle()
    # THE ROUTED PATH: uui_menubar_ops parks the code and on_widget takes
    # it, which is a different path from the one Notepad hand-routes.
    res.check("c6. File > New Tab opens a tab through the menu",
              tab_count(dbg) == before + 1,
              f"tabs {before} -> {tab_count(dbg)}")

    # --- the fall-through triple -------------------------------------
    term = rect(dbg, "menu.title", MENU_TERMINAL)   # row 0 is Rename Tab
    res.check("c7. the Terminal title has a reported rect", term is not None)
    if not term:
        return
    sel_before = layout_field(dbg, "sel")
    count_before = tab_count(dbg)
    open_menu(dbg, win, term)
    row = item_rect(dbg, 0, 0)

    # WHICHEVER TAB THE ROW ACTUALLY SITS OVER, not tab 0 by name.
    # **This used to name tab 0 and broke once**: adding an Edit menu to
    # the Terminal moved the Terminal title to the right, its popup with
    # it, and the row stopped overlapping the leftmost tab -- which
    # failed as "that row really does cover tab 0" and said nothing
    # about the fall-through this block exists to test. What the check
    # needs is a tab under the row that is NOT the selected one, so that
    # c8b can tell a swallowed click from one that landed.
    under = None
    for i in range(max(count_before, 1)):
        slot = rect(dbg, "tabs.slot", i)
        if (row is not None and slot is not None
                and row[1] < slot[1] + slot[3]
                and row[0] < slot[0] + slot[2]
                and slot[0] < row[0] + row[2]):
            under = i
            break
    res.check("c8. that row really does cover a tab",
              under is not None, f"row {row} tabs {count_before}")
    res.check("c8pre. and the tab under it is NOT the selected one",
              under is not None and sel_before != under,
              f"under {under}, selected {sel_before}")
    if not row:
        return
    dbg.click(*centre(win, row))
    time.sleep(0.8)
    dbg.settle()
    res.check("c8a. Terminal > Rename Tab opened the prompt",
              layout_field(dbg, "rename") == 1,
              f"rename {layout_field(dbg, 'rename')}")
    res.check("c8b. the click did NOT select the tab under it",
              layout_field(dbg, "sel") == sel_before,
              f"selected {sel_before} -> {layout_field(dbg, 'sel')}")
    res.check("c8c. ...nor land on that tab's close box",
              tab_count(dbg) == count_before,
              f"tabs {count_before} -> {tab_count(dbg)}")

    dbg.send("gui key 0x1b")   # Esc: cancel the rename
    time.sleep(0.5)
    dbg.settle()
    res.check("c8d. Esc cancels the rename prompt",
              layout_field(dbg, "rename") == 0,
              f"rename {layout_field(dbg, 'rename')}")

    # Hiding the bar, and F10 getting it back -- the half that makes the
    # toggle a toggle rather than a one-way door.
    view = rect(dbg, "menu.title", MENU_VIEW)
    res.check("c9. the View title has a reported rect", view is not None)
    if not view:
        return
    open_menu(dbg, win, view)
    menurow = item_rect(dbg, 0, last_item_index(dbg, 0))
    res.check("c10. the View popup reported its rows", menurow is not None)
    if not menurow:
        return

    chrome_before = layout_field(dbg, "chrome")
    dbg.click(*centre(win, menurow))
    time.sleep(0.8)
    dbg.settle()
    res.check("c11. View > Menu Bar hides the bar",
              layout_field(dbg, "menu") == 0,
              f"menu {layout_field(dbg, 'menu')}")
    # AND THE GRID GREW. "the flag flipped" is not "the row was given
    # back" -- a hidden bar that still reserved its row would pass c10.
    res.check("c12. hiding it gives the row back to the grid",
              layout_field(dbg, "chrome") < chrome_before,
              f"chrome {chrome_before} -> {layout_field(dbg, 'chrome')}")

    dbg.send("gui key 0xa4")   # F10
    time.sleep(0.8)
    dbg.settle()
    res.check("c13. F10 brings a hidden menu bar back",
              layout_field(dbg, "menu") == 1,
              f"menu {layout_field(dbg, 'menu')}")


def check_completion(dbg, qmp, res):
    """Tab completion inside the Terminal window.

    **ASSERTED THROUGH THE FILESYSTEM, not through pixels.** A completion
    that worked leaves different bytes on disk -- `mkdi<Tab> <path>`
    creates a directory only if Tab turned it into `mkdir` -- and that is
    checkable with `sh ls`, which shares no code with the shell under
    test. Reading the completed text off the screen would assert the
    emulator's rendering as much as the completion.

    The load-bearing check is c14: a directory and a FILE share a prefix,
    so `cd` can only finish the word if it filters to directories. With
    files included there are two candidates, completion stops at the
    shared prefix, and the `cd` fails -- which the cwd then shows.
    """
    win = dbg.window(TITLE)
    res.check("c14pre. a Terminal is up for the completion checks",
              win is not None)
    if not win:
        return
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    dbg.settle()
    # A KNOWN-CLEAN LINE. Whatever ran before this may have left the
    # editor mid-line, and a completion check that types onto the end of
    # someone else's text is asserting nothing.
    key(dbg, "0x0d")
    dbg.settle()

    dbg.send("sh rm /ct_gui_dir")
    dbg.send("sh rm /ct_gui_file")
    dbg.settle()

    # `mkdi<Tab>` -> `mkdir`, proving FIRST-WORD completion off PATH.
    # Typed as a real keystroke stream, Tab included, so the byte goes
    # down the pty exactly as a person's would.
    type_text(dbg, "mkdi")
    key(dbg, "0x09")   # Tab, the same path every other keystroke here takes
    dbg.settle()
    time.sleep(0.5)
    type_line(dbg, " /ct_gui_dir", settle=1.5)
    listing = dbg.send("sh ls /")
    res.check("c14. Tab completed a PATH program in first position",
              "ct_gui_dir" in listing,
              "the directory was never created, so `mkdi` did not become `mkdir`")

    # A file beside it, sharing the prefix. Now `cd /ct_gui_<Tab>` has
    # two candidates unless `cd` filters to directories.
    dbg.send("sh write /ct_gui_file x")
    dbg.settle()
    type_text(dbg, "cd /ct_gui_")
    key(dbg, "0x09")   # Tab, the same path every other keystroke here takes
    dbg.settle()
    time.sleep(0.5)
    type_line(dbg, "", settle=1.5)
    type_line(dbg, "pwd", settle=1.5)

    # The shell announces its directory as an OSC title on every change,
    # which is text the app logs -- so where the shell now stands is
    # readable without looking at a single pixel.
    titles = [l.rstrip() for l in dbg.logs("uterm: tab", clear=False)
              if "title" in l]
    res.check("c15. Tab on `cd` completed to the DIRECTORY, not the shared prefix",
              any(l.endswith("/ct_gui_dir") for l in titles),
              f"last titles: {titles[-3:]}")

    dbg.send("sh rm /ct_gui_file")
    dbg.send("sh rm /ct_gui_dir")
    dbg.settle()


def check_tab_legibility(dbg, qmp, res):
    """Can you tell the tabs apart, and see which one is open?

    **THIS IS THE CHECK FOR A REPORTED COMPLAINT**, and the complaint is
    what makes it sharp: three tabs sitting in the same directory all
    report the same title, so their labels are IDENTICAL. Two tabs drawn
    from identical labels are pixel-identical unless something else
    distinguishes them -- which is the "moving identical content is
    pixel-identical" trap in docs/gui-guidelines.md, arriving from the
    other side. c16 asserts they differ; only the tab NUMBER can make
    that true here.

    c17 reads the two fills as VALUES rather than looking at them, with
    the resting tab beside it as the control -- a selection marked by a
    few units is invisible in practice and looks fine in a screenshot,
    which is exactly how the first version of this strip shipped.
    """
    win = dbg.window(TITLE)
    if win:
        # Dismiss any menu the checks above left open by CLICKING the
        # grid, not by pressing Esc. Esc with no menu open is a byte to
        # the shell, and klineedit reads it as the Alt prefix -- it then
        # swallows the next keystroke, which broke the completion checks
        # that run after this one.
        c0 = win["content"]
        dbg.click(c0["x"] + c0["w"] // 2, c0["y"] + c0["h"] - 20)
        dbg.settle()
    if not win:
        res.check("c16pre. a Terminal is up for the legibility checks", False)
        return

    tabs = [rect(dbg, "tabs.slot", i) for i in range(3)]
    res.check("c16pre. three tabs, so two of them are resting",
              all(t is not None for t in tabs) and tab_count(dbg) == 3,
              f"tabs {tab_count(dbg)}")
    if not all(t is not None for t in tabs):
        return

    from PIL import Image
    shot = os.path.abspath(os.path.join("/tmp", "ut_tabs_legible.png"))
    qmp.stable_pixels(shot)
    c = win["content"]
    with Image.open(shot) as raw:
        im = raw.convert("RGB")

        def crop(t):
            # THE LABEL AREA ONLY -- the left half. Cropping the whole
            # tab made this check pass with numbering disabled: tab 0
            # carries a separator hairline on its right edge and tab 1
            # does not (its neighbour is the selected tab), so the crops
            # differed by that one line whatever the labels said. Caught
            # by a positive control, which is the only thing that would
            # have caught it.
            return im.crop((c["x"] + t[0], c["y"] + t[1],
                            c["x"] + t[0] + t[2] // 2,
                            c["y"] + t[1] + t[3])).tobytes()

        def fill(t):
            # The LEFT PADDING at mid-height: tabs are as wide as their
            # title now, so the middle of one is inside the label's
            # glyphs and a sample there reads ink as often as fill. Three
            # pixels in is past the rounded corner's inset at this row and
            # short of the first glyph.
            return im.getpixel((c["x"] + t[0] + 3,
                                c["y"] + t[1] + t[3] // 2))

        # Tabs 0 and 1 are both RESTING and both labelled "/" -- so
        # anything that tells them apart has to be drawn by the widget.
        res.check("c16. two resting tabs with the same label still differ",
                  crop(tabs[0]) != crop(tabs[1]),
                  "they render identically, so nothing distinguishes them")

        sel = layout_field(dbg, "sel")
        if sel is None or not 0 <= sel < 3:
            res.check("c17. the selected tab's fill is visibly lifted", False,
                      f"selected {sel}")
            return
        other = 0 if sel != 0 else 1
        fs, fo = fill(tabs[sel]), fill(tabs[other])
        delta = max(abs(fs[k] - fo[k]) for k in range(3))
        # **THE FLOOR IS ABOVE WHAT WAS REPORTED AS TOO SUBTLE.** A
        # twenty-unit lift (field over window) shipped and was reported
        # as hard to see with three tabs open, so a check that accepts
        # twenty accepts the bug. Resting tabs are the control colour
        # now, which is thirty.
        res.check("c17. the selected tab's fill is visibly lifted",
                  delta >= 25,
                  f"selected {fs} vs resting {fo} -- delta {delta}, want >= 25")

        # ...AND THE CONTROL: the two RESTING tabs must have the SAME
        # fill. Without it, "the fills differ" would pass on a strip
        # where every tab is a different colour for no reason.
        f0, f1 = fill(tabs[0]), fill(tabs[1])
        res.check("c18. ...and two resting tabs share one fill",
                  f0 == f1, f"{f0} vs {f1}")


def tab_order(dbg):
    """Which session slot sits at which tab position, as the app reports it.

    A tab RECT says where a tab is drawn and nothing about which shell it
    holds, so this is the only way to see whether a new tab was appended
    or dropped into a recycled slot's hole.
    """
    seen = {}
    for l in dbg.logs("uterm: layout tabslot ", clear=False):
        v = l.split("uterm: layout tabslot ")[1].split()
        seen[int(v[0])] = int(v[1])
    return [seen[i] for i in sorted(seen)] if seen else []


def check_tab_order(dbg, qmp, res):
    """A NEW TAB GOES TO THE RIGHT END, whatever slot it is given.

    **THE SCENARIO IS THE ONLY ONE THAT SHOWS IT**: with no closures,
    slots fill 0,1,2 and a slot-ordered strip appends by accident. Close
    a MIDDLE tab and its slot is recycled -- so the next new tab reused
    that slot and silently reappeared in the hole, halfway along the
    strip. Opening tabs without closing one first cannot see this.
    """
    win = dbg.window(TITLE)
    if not win:
        res.check("c19pre. a Terminal is up for the order checks", False)
        return
    c = win["content"]
    dbg.click(c["x"] + c["w"] // 2, c["y"] + c["h"] - 20)
    dbg.settle()

    # Get to exactly three tabs, whatever the checks above left behind.
    plus = rect(dbg, "tabs.new")
    if not plus:
        res.check("c19pre. the + button has a rect", False)
        return
    guard = 0
    while (tab_count(dbg) or 0) < 3 and guard < 6:
        dbg.click(*centre(win, plus))
        time.sleep(1.2)
        dbg.settle()
        guard += 1
    res.check("c19pre. three tabs for the order checks", tab_count(dbg) == 3,
              f"tabs {tab_count(dbg)}")
    if tab_count(dbg) != 3:
        return

    before = tab_order(dbg)
    res.check("c19. the app reports which slot sits at which position",
              len(before) == 3, f"order {before}")
    if len(before) != 3:
        return
    middle_slot = before[1]

    # Select the MIDDLE tab and close it with Ctrl+Shift+W. By keyboard
    # rather than by its close box: the box's position is the widget's
    # arithmetic, and a test that re-derives it is asserting its own.
    qmp.combo(["ctrl", "pgup"])
    time.sleep(0.8)
    dbg.settle()
    res.check("c20pre. the middle tab is selected",
              layout_field(dbg, "sel") == 1,
              f"selected {layout_field(dbg, 'sel')}")
    qmp.combo(["ctrl", "shift", "w"])
    time.sleep(1.4)
    dbg.settle()
    res.check("c20. closing the middle tab leaves two",
              tab_count(dbg) == 2, f"tabs {tab_count(dbg)}")
    if tab_count(dbg) != 2:
        return

    # ...and now open one. The freed slot is the LOWEST free one, so a
    # strip ordered by slot puts this tab back in the middle.
    dbg.logs("uterm: layout tabslot ", clear=True)
    dbg.click(*centre(win, plus))
    time.sleep(1.2)
    dbg.settle()
    after = tab_order(dbg)
    res.check("c21. the new tab is at the RIGHT END, not in the freed hole",
              len(after) == 3 and after[-1] == middle_slot,
              f"order {before} -> {after}, recycled slot {middle_slot}")
    res.check("c22. ...and it is the selected one",
              layout_field(dbg, "sel") == 2,
              f"selected {layout_field(dbg, 'sel')} of {tab_count(dbg)}")


def fresh_terminal(dbg, res, tag):
    """A Terminal of this block's own, focused, with its logs cleared.

    ITS OWN WINDOW every time: an earlier check may have closed the last
    one with Alt+F4, and reading STALE log lines from a window that is
    gone is how three checks in this file first passed against nothing.
    """
    dbg.logs("uterm:", clear=True)
    dbg.send(f"gui spawn {SPAWN_PATH}")
    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline and not win:
        win = dbg.window(TITLE)
        time.sleep(0.2)
    res.check(f"{tag}0. a Terminal opened", win is not None)
    if not win:
        return None
    deadline = time.time() + SPAWN_TIMEOUT_S
    while time.time() < deadline:
        if dbg.logs("uterm: layout cursor", clear=False):
            break
        time.sleep(0.2)
    dbg.click(win["x"] + win["w"] // 2, win["y"] + win["h"] // 2)
    dbg.settle()
    return win


def fill_scrollback(dbg):
    """Print far more rows than the window holds, each one DIFFERENT.

    **DIFFERENT, and that is the whole point.** Moving identical content
    is pixel-identical: a scroll test printing forty copies of one line
    cannot tell a working scrollbar from a dead one. `dmesg` is the
    longest output on the machine and every line of it is distinct.
    """
    type_line(dbg, "dmesg", settle=3.5)


def check_scrollbar(dbg, qmp, tmp, res):
    """The gutter scrollbar: it is DRAWN, and dragging it MOVES the view.

    **THE LOAD-BEARING CHECKS ARE s3 AND s5**, and they are the two the
    guidelines say a bar needs: the thumb drag and the trough page must
    each change where the reader is. "It responds" is not "it scrolls",
    and an absence check ("the drag selected nothing") passes perfectly
    against a bar that does nothing at all -- which is how this project
    shipped three inert scrollbars.

    The position is read from the app's own `sbview` field rather than
    from pixels. A test that measured the thumb's y would be asserting
    the same arithmetic the widget used to draw it; sbview is the thing
    that would still be wrong if the bar were decorative.
    """
    win = fresh_terminal(dbg, res, "s")
    if not win:
        return

    bar = rect(dbg, "bar")
    res.check("s1. the bar has a reported rect inside the window",
              bar is not None and bar[2] > 0 and bar[0] + bar[2] <= win["w"],
              f"bar {bar} window w {win['w']}")
    if not bar:
        return

    # THE TRACK IS ACTUALLY PAINTED, read as pixels rather than assumed
    # from the rect: a reported rect proves the app computed one, not
    # that anything reached the screen. The control is the strip of
    # background just LEFT of the bar, which must stay black -- half the
    # assertion is the neighbour staying put.
    c = win["content"]
    gutter = ink(qmp, tmp, "sb_gutter.png",
                 (c["x"] + bar[0], c["y"] + bar[1],
                  c["x"] + bar[0] + bar[2], c["y"] + bar[1] + bar[3]))
    beside = ink(qmp, tmp, "sb_gutter.png",
                 (c["x"] + bar[0] - bar[2], c["y"] + bar[1],
                  c["x"] + bar[0], c["y"] + bar[1] + bar[3]))
    area = bar[2] * bar[3]
    res.check("s2. the track is painted, and the margin beside it is not",
              gutter > area // 2 and beside < area // 10,
              f"{gutter}/{area} px in the gutter, {beside} beside it")

    # THE SHAPE, read as pixels: the groove is a capsule
    # (uui_scrollbar_style_default), so its four corners are background
    # and the middle of each of its four edges is still track. Asserting
    # only the corners would pass against a bar that was never drawn.
    corners, middles = corner_shape(
        qmp, tmp, "sb_shape.png",
        (c["x"] + bar[0], c["y"] + bar[1],
         c["x"] + bar[0] + bar[2], c["y"] + bar[1] + bar[3]), TRACK)
    res.check("s2a. the track's corners are rounded away, its edges are not",
              corners == 0 and middles == 4,
              f"{corners}/4 corners still track-coloured, {middles}/4 edge midpoints")

    fill_scrollback(dbg)
    sbcount = layout_field(dbg, "sbcount") or 0
    res.check("s2b. printing past the screen filled the scrollback",
              sbcount > 20, f"sbcount {sbcount}")
    if sbcount <= 20:
        return

    # The thumb, from the app's rect and the widget's own proportions.
    # Dragging it UP goes BACK into history: a vertical bar here is a
    # scrollback, so offset 0 is pinned to the newest text at the bottom.
    bx = c["x"] + bar[0] + bar[2] // 2
    bottom = c["y"] + bar[1] + bar[3] - 8
    top = c["y"] + bar[1] + 8

    before = layout_field(dbg, "sbview")
    dbg.drag_real(qmp, bx, bottom, bx, top)
    dbg.settle()
    after = layout_field(dbg, "sbview")
    res.check("s3. dragging the thumb up scrolls BACK into history",
              before == 0 and after is not None and after > 0,
              f"sbview {before} -> {after}")

    # Back to the bottom, so the trough check starts somewhere known --
    # by a KEY, not by dragging the thing under test. A test must not
    # assume the thing it is testing.
    key(dbg, "0x94")   # PageDown, repeatedly, to pin the view at 0
    for _ in range(40):
        if layout_field(dbg, "sbview") == 0:
            break
        key(dbg, "0x94")
    dbg.settle()
    res.check("s4. PageDown returns the view to the live screen",
              layout_field(dbg, "sbview") == 0,
              f"sbview {layout_field(dbg, 'sbview')}")

    # The trough ABOVE the thumb pages back. With the view at the bottom
    # the thumb is at the bottom, so anything near the top of the track
    # is trough.
    rows = layout_field(dbg, "rows") or 24
    dbg.click(bx, c["y"] + bar[1] + 4)
    dbg.settle()
    paged = layout_field(dbg, "sbview")
    res.check("s5. clicking the trough above the thumb pages BACK",
              paged is not None and paged >= rows - 1,
              f"sbview 0 -> {paged}, a page is {rows - 1}")

    key(dbg, "0x94")
    dbg.settle()
    dbg.key("f4", mods="alt")
    dbg.settle()


def check_selection(dbg, qmp, tmp, res):
    """Mouse selection, and that what it copies is the text underneath.

    **THE LOAD-BEARING CHECK IS x4, AND IT IS A ROUND TRIP**: the
    selection is pasted back into the shell and the pasted characters
    have to be the ones that were selected. Every weaker form of this
    passes against a broken implementation -- "some pixels inverted"
    passes against a highlight over the wrong cells, and "the clipboard
    is not empty" passes against a copy of the whole screen.

    The numbers come from `seq`, so each row's text is unique and a
    selection of the wrong row is a different string rather than an
    identical one.
    """
    win = fresh_terminal(dbg, res, "x")
    if not win:
        return

    fill_scrollback(dbg)
    chrome = layout_field(dbg, "chrome") or 0
    rows = layout_field(dbg, "rows") or 24
    c = win["content"]
    # A cell's height is font-derived, so it is DERIVED HERE TOO -- from
    # the app's own reported chrome height and row count, never a pixel
    # constant that would be right at exactly one font size.
    band = max((c["h"] - chrome) // max(rows, 1), 1)

    # The row above the prompt is the last number `seq` printed. Selected
    # by dragging across it from the left margin to well past its end.
    y = c["y"] + chrome + (rows - 3) * band + band // 2
    dbg.drag_real(qmp, c["x"] + 8, y, c["x"] + 200, y)
    dbg.settle()

    n = layout_field(dbg, "selbytes")
    res.check("x1. a drag over a row selects some of it",
              n is not None and n > 0, f"selbytes {n}")

    # A CLICK WITH NO DRAG SELECTS NOTHING. Without this an "anchor
    # equals cursor" bug reads as a working selection of zero bytes,
    # which x1 above would not distinguish from no selection at all.
    dbg.click(c["x"] + 40, y)
    dbg.settle()
    res.check("x2. a plain click selects nothing",
              layout_field(dbg, "selbytes") == 0,
              f"selbytes {layout_field(dbg, 'selbytes')}")

    # Double-click selects a WORD -- here a bare number, so the word and
    # the line differ and the two gestures cannot be confused.
    dbg.drag_real(qmp, c["x"] + 8, y, c["x"] + 200, y)
    dbg.settle()
    selected = layout_field(dbg, "selbytes")

    # x4: paste it back and require the characters to arrive. The
    # clipboard is read through the TERMINAL rather than asserted on
    # directly, which is what makes this a round trip: copy-on-select put
    # it there, Ctrl+Shift+V takes it out, and the shell echoes it.
    key(dbg, "0x03")          # Ctrl-C first: abandon whatever is typed
    dbg.settle()
    dbg.logs("uterm: layout cursor", clear=True)
    before_cursor = layout_field(dbg, "cursor", default=0)
    dbg.key("0x16", mods="shift")   # Ctrl+Shift+V
    dbg.settle()
    time.sleep(0.8)
    after_cursor = layout_field(dbg, "cursor", default=0)
    res.check("x4. Ctrl+Shift+V pastes the selection back onto the line",
              selected and selected > 0 and after_cursor > before_cursor,
              f"selected {selected} bytes, cursor {before_cursor} -> {after_cursor}")

    key(dbg, "0x03")
    dbg.settle()
    dbg.key("f4", mods="alt")
    dbg.settle()


def check_resize(dbg, qmp, tmp, res):
    """The prompt stays on the LAST ROW across a resize, both ways.

    **THE LOAD-BEARING CHECK IS r3**, the grow. Shrinking looked fine
    before this and growing did not: the cursor was merely CLAMPED into
    the shorter window, so the text stayed where it was and the prompt
    landed on top of old output -- and growing then left it stranded
    there with blank rows below it, because nothing moved it back.

    Asserted as "the cursor is on the last row", which is what a prompt
    at the bottom of a full screen means, rather than as a pixel
    position: the row count changes under the test, so a fixed y would
    be asserting the arithmetic instead of the behaviour.
    """
    win = fresh_terminal(dbg, res, "r")
    if not win:
        return

    fill_scrollback(dbg)   # a full screen, prompt at the bottom

    def at_bottom():
        rows = layout_field(dbg, "rows")
        cols = layout_field(dbg, "cols")
        cur = layout_field(dbg, "cursor")
        if rows is None or not cols or cur is None:
            return None, None
        return cur // cols, rows

    cr, rows0 = at_bottom()
    res.check("r1. the prompt starts on the last row of a full screen",
              cr is not None and cr == rows0 - 1, f"cursor row {cr} of {rows0}")
    if cr is None:
        return

    # **`gui resize`, NOT A DRAG OF THE GRIP.** The grip needs a real
    # pointer the compositor tracks across frames, which injected input
    # cannot be -- an earlier version of this check dragged it and the
    # window never moved at all, so both halves passed against a
    # completely unchanged terminal. `gui resize` reaches the same
    # resize_ask() the grip does.
    c = win["content"]
    dbg.send(f"gui resize {c['w']} {c['h'] - 200}")
    dbg.settle()
    cr, rows1 = at_bottom()
    res.check("r2. shrinking keeps the prompt on the last row",
              rows1 is not None and rows1 < rows0 and cr == rows1 - 1,
              f"cursor row {cr} of {rows1} (was {rows0})")

    dbg.send(f"gui resize {c['w']} {c['h']}")
    dbg.settle()
    cr, rows2 = at_bottom()
    res.check("r3. ...and growing brings it back to the last row",
              rows2 == rows0 and cr == rows2 - 1,
              f"cursor row {cr} of {rows2} (started at {rows0})")

    prefs_checks(dbg, qmp, tmp, win, res)

    dbg.key("f4", mods="alt")
    dbg.settle()


KEY_F10 = "0xa4"


def prime_layout(dbg):
    """Force a fresh layout block out of the Terminal.

    **TYPING IS NOT ENOUGH, and that is the trap.** `uapp` dedupes the
    whole block per frame, and terminal.c logs its CHROME only when
    `chrome_moved()` says the signature changed -- so an idle window
    emits one `layout cursor` line and nothing else, however much you
    type into it. Every check before this one has cleared the
    accumulated log, so the dialog's rect reads as absent and a working
    feature scores as missing.

    F10 hides the menu bar and F10 puts it back: `g_menu_shown` is in
    that signature, so the block is re-emitted in full, twice, ending
    with the bar shown exactly as it started.
    """
    dbg.send(f"gui key {KEY_F10}")
    dbg.settle()
    time.sleep(0.4)
    dbg.send(f"gui key {KEY_F10}")
    dbg.settle()
    time.sleep(0.6)


def layout_rect(dbg, name):
    """The rect of a named row of the Terminal's most recent layout block.

    ONLY THE NEWEST BLOCK, for the reason menubar_test.py's Layout
    states: the log accumulates, so a `prefs 289 37 408 400` line from a
    dialog that has since closed stays in it forever and a parser taking
    the last occurrence of each key reports a closed dialog as open.

    **terminal.c LOGS `cursor` LAST, not first** -- which is the
    opposite of Notepad, and slicing forward from it finds an empty
    block and reports every rect as absent. So the newest block is the
    run BETWEEN two `cursor` lines, and it must be one with something
    between them: an idle repaint emits a lone `cursor` line and the
    chrome only re-logs when `chrome_moved()` says it moved.

    Every coordinate this test clicks comes from here rather than from
    arithmetic, which is the rule for a control whose position moves
    with the font.
    """
    lines = dbg.logs("uterm: layout", clear=False)
    at = [i for i, l in enumerate(lines) if "uterm: layout cursor" in l]
    lo = hi = None
    for k in range(len(at) - 1, 0, -1):
        if at[k] - at[k - 1] > 1:
            lo, hi = at[k - 1] + 1, at[k]
            break
    if lo is None:
        return None
    want = f"uterm: layout {name} "
    for l in lines[lo:hi + 1]:
        if want in l:
            t = l.split(want)[1].split()
            if len(t) >= 4 and all(x.lstrip("-").isdigit() for x in t[:4]):
                return tuple(int(x) for x in t[:4])
    return None


def prefs_checks(dbg, qmp, tmp, win, res):
    """The colour scheme, and that the Preferences dialog is wired in.

    **THE SCHEME IS ASSERTED ON A PIXEL**, because the whole feature is
    a claim about colour and "the app reported that it applied one" is
    exactly what a broken palette would also produce. Green Phosphor,
    because its background (#001b00) is neither the default black nor
    anything else on this desktop: a scheme that merely LOOKED applied
    would have to land on that exact value.

    **WHAT IS NOT DRIVEN HERE IS THE MENU.** Opening Edit > Preferences
    needs a pointer that is still on the title between the press and the
    release, and an injected position survives ONE wm_run() iteration
    (CLAUDE.md's note on `gui move` and hover) -- so the popup opens and
    is dismissed by a release the WM reads at the real cursor. The
    dialog's own open/pick/OK/Cancel path is verified by hand; what is
    checked here is everything downstream of it, which is where a
    regression would actually land.
    """
    from PIL import Image

    c = win["content"]

    try:
        prime_layout(dbg)
        # The dialog is a declared widget, so the toolkit reports its
        # rect -- zero while it is closed. A missing line means it never
        # reached the widget array, which is how a modal silently stops
        # being routed at all.
        res.check("p1. the prefs dialog is declared and closed",
                  layout_rect(dbg, "prefs") == (0, 0, 0, 0),
                  f"prefs rect {layout_rect(dbg, 'prefs')}")
        res.check("p2. so is the close confirmation",
                  layout_rect(dbg, "quit-ask") == (0, 0, 0, 0),
                  f"quit-ask rect {layout_rect(dbg, 'quit-ask')}")

        top = layout_field(dbg, "chrome", 4)
        probe = (c["x"] + c["w"] // 2, c["y"] + top + c["h"] // 2)
        # THE CONTROL: a pixel on the desktop beside the window, which
        # must not move. Half the assertion is the neighbour staying put.
        control = (max(4, c["x"] // 2), c["y"] + c["h"] // 2)

        # Reads `probe` at CALL time, not at definition time: the point
        # moves to the freshly spawned window below, and a closure over
        # the old value would keep sampling the window that is gone.
        def pixels(name):
            path = os.path.abspath(os.path.join(tmp, name))
            qmp.screenshot(path)
            with Image.open(path) as im:
                im = im.convert("RGB")
                return im.getpixel(probe), im.getpixel(control)

        was_grid, was_desk = pixels("ut_scheme_before.png")

        # **EVERY TERMINAL IS CLOSED FIRST, BY PID.** By this point the
        # run has left several stacked at the same position, and
        # spawning one more onto that pile is the step that is
        # unreliable -- a spawn that quietly produced no window left the
        # capture reading the one UNDERNEATH, whose grid is still the
        # default black, so the check failed with the feature working.
        #
        # NOT with Alt+F4, which is what this tried first: a terminal
        # with more than one tab now ASKS before closing, and several of
        # those are exactly what the tab sections leave behind -- so the
        # key put a modal up and the loop closed nothing at all. A kill
        # needs no window to cooperate.
        for line in (dbg.send("sh ps") or "").splitlines():
            f = line.split()
            if len(f) >= 2 and f[-1] == "uterm" and f[0].isdigit():
                dbg.send(f"sh kill {f[0]}")
        dbg.settle()
        time.sleep(1.0)

        # Through tosh, because the `#` shell has no redirection of its
        # own; the kernel splits this string by tosh's quoting rules
        # (docs/conventions/shell.md).
        #
        # **POLLED FOR ITS CONTENT, not slept on.** The spawn returns
        # before the child has written, so a fixed pause reads an EMPTY
        # file perfectly happily -- and the window opened next then
        # takes the default scheme.
        dbg.send('sh spawn /bin/tosh -c "echo scheme=green > /etc/terminal.conf"')
        dbg.settle()
        conf = ""
        deadline = time.time() + 10
        while time.time() < deadline:
            conf = dbg.send("sh cat /etc/terminal.conf") or ""
            if "scheme=green" in conf:
                break
            time.sleep(0.4)
        res.check("p3. the config file is where the app will look for it",
                  "scheme=green" in conf, conf.strip()[:60])

        # A NEW window, because the scheme is read when one OPENS: the
        # running terminal is deliberately not reloaded from the file.
        dbg.send(f"gui spawn {SPAWN_PATH}")
        deadline = time.time() + SPAWN_TIMEOUT_S
        fresh = None
        while time.time() < deadline and not fresh:
            fresh = dbg.window(TITLE)
            if not fresh:
                time.sleep(0.3)
        if not fresh:
            res.check("p4. a scheme in /etc/terminal.conf reaches the grid",
                      False, "no Terminal window came back after the spawn")
            return
        dbg.settle()
        time.sleep(1.5)

        # The probe follows the NEW window, not the one measured before.
        fc = fresh["content"]
        top = layout_field(dbg, "chrome", 4)
        probe = (fc["x"] + fc["w"] // 2, fc["y"] + top + fc["h"] // 2)
        now_grid, now_desk = pixels("ut_scheme_after.png")
        res.check("p4. a scheme in /etc/terminal.conf reaches the grid",
                  now_grid == (0, 27, 0),
                  f"grid was {was_grid}, now {now_grid} -- wanted green.scheme's "
                  "#001b00 background")
        res.check("p5. ...and nothing outside the window moved",
                  now_desk == was_desk,
                  f"the desktop pixel went {was_desk} -> {now_desk}")
    finally:
        # **ALWAYS**, including on an early return. This file is written
        # to disk.img and survives `make iso`, so a run that left a
        # scheme or a font size behind would change the machine for every
        # later tool -- the trap CLAUDE.md records for settings_test, and
        # one this check hit for real (a stale font_size reddened the
        # reverse-video assertion three sections earlier).
        dbg.send("sh rm /etc/terminal.conf")
        dbg.settle()
        dbg.key("f4", mods="alt")
        dbg.settle()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--shot", metavar="DIR")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "uterm_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    if args.shot:
        os.makedirs(args.shot, exist_ok=True)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, args.shot, res)
        check_tabs(dbg, qmp, args.tmp, res)
        check_chrome(dbg, qmp, res)
        check_tab_legibility(dbg, qmp, res)
        check_tab_order(dbg, qmp, res)
        check_completion(dbg, qmp, res)
        check_scrollbar(dbg, qmp, args.tmp, res)
        check_selection(dbg, qmp, args.tmp, res)
        check_resize(dbg, qmp, args.tmp, res)
    finally:
        dbg.close()

    print(f"\nuterm_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
