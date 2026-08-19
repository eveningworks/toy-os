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
from gui_debug import DebugConsole          # noqa: E402
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
        if dbg.logs("uterm: layout prompt", clear=False):
            break
        time.sleep(0.2)
    dbg.settle()

    c = win["content"]
    # The transcript area, clear of the prompt line at the bottom.
    box = (c["x"] + 4, c["y"] + 4, c["x"] + c["w"] - 4, c["y"] + c["h"] - 30)

    base = ink(qmp, tmp, "ut_base.png", box)
    res.check("it renders its banner", base > 100, f"only {base} ink pixels")

    # --- the prompt follows the transcript ----------------------------
    #
    # It used to be pinned to the bottom of the window unconditionally,
    # so a fresh Terminal showed two banner lines at the top and a
    # prompt stranded at the foot with a band of empty black between.
    # A real terminal puts the prompt after the last line and only
    # reaches the bottom once the screen has filled.
    #
    # Paired on purpose: "near the top with a short transcript" alone
    # would also pass if the prompt were simply nailed to the top, and
    # "near the bottom after output" alone is what the bug did. Both
    # together say it FOLLOWS.
    def prompt_y():
        for l in reversed(dbg.logs("uterm: layout prompt", clear=False)):
            return int(l.split("layout prompt")[1].split()[0])
        return None

    short_y = prompt_y()
    res.check("the prompt follows a short transcript instead of sitting at the bottom",
              short_y is not None and short_y < c["h"] // 2,
              f"prompt at y={short_y} in a {c['h']}px content area")

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

    # The other half of the pair: lscpu fills the window, so the prompt
    # must now be at the bottom rather than wherever it started.
    long_y = prompt_y()
    res.check("a full transcript pushes the prompt to the bottom",
              long_y is not None and short_y is not None and long_y > short_y,
              f"prompt was at y={short_y}, now y={long_y} after lscpu filled the window")

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-terminal-spawn.png")))

    # A command that does not exist must be reported, not silently
    # ignored -- otherwise "spawn failed" and "program printed nothing"
    # look identical to a user.
    before = after_lscpu
    type_line(dbg, "definitelynotacommand")
    after_bad = ink(qmp, tmp, "ut_bad.png", box)
    res.check("an unknown command reports itself", after_bad != before)

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

    print(f"\nuterm_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
