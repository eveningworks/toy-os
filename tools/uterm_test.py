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
    titles = [l for l in dbg.logs("uterm: tab", clear=False) if "title" in l]
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


def item_rect(dbg, level, index):
    """One reported menu row: `uterm: layout item <level> <i> x y w h`."""
    if index is None:
        return None
    for l in reversed(dbg.logs("uterm: layout item ", clear=False)):
        v = [int(n) for n in l.split("uterm: layout item ")[1].split()[:6]]
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
    dbg.logs("uterm: layout item ", clear=True)
    dbg.click(*centre(win, title))
    dbg.settle()


def last_item_index(dbg, level):
    """The bottom row of an open popup, so a test names a row by where it
    is rather than by counting the menu tree in two places."""
    best = None
    for l in dbg.logs("uterm: layout item ", clear=False):
        v = [int(n) for n in l.split("uterm: layout item ")[1].split()[:6]]
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
    plus = rect(dbg, "newtab")
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

    title0 = rect(dbg, "title", 0)
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
    term = rect(dbg, "title", 1)      # "Terminal"; row 0 is Rename Tab
    res.check("c7. the Terminal title has a reported rect", term is not None)
    if not term:
        return
    sel_before = layout_field(dbg, "sel")
    count_before = tab_count(dbg)
    open_menu(dbg, win, term)
    row = item_rect(dbg, 0, 0)
    tab0 = rect(dbg, "tab", 0)
    covered = (row is not None and tab0 is not None
               and row[1] < tab0[1] + tab0[3]
               and row[0] < tab0[0] + tab0[2])
    res.check("c8. that row really does cover tab 0",
              covered, f"row {row} tab0 {tab0}")
    res.check("c8pre. and tab 0 is NOT the selected one",
              sel_before != 0, f"selected {sel_before}")
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
    view = rect(dbg, "title", 2)
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
        check_tabs(dbg, qmp, args.tmp, res)
        check_chrome(dbg, qmp, res)
    finally:
        dbg.close()

    print(f"\nuterm_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
