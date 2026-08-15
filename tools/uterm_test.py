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
TITLE = "Terminal (ring 3)"
SPAWN_CMD = "run uterm"
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
    dbg.send("gui open Terminal")
    dbg.settle()
    for ch in SPAWN_CMD:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()
    key(dbg, "0x0d")

    deadline = time.time() + SPAWN_TIMEOUT_S
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win:
            break
    res.check("the ring-3 Terminal opens its own window", win is not None)
    if not win:
        return

    c = win["content"]
    # The transcript area, clear of the prompt line at the bottom.
    box = (c["x"] + 4, c["y"] + 4, c["x"] + c["w"] - 4, c["y"] + c["h"] - 30)

    base = ink(qmp, tmp, "ut_base.png", box)
    res.check("it renders its banner", base > 100, f"only {base} ink pixels")

    # A BUILTIN: handled inside the shell, no spawn.
    type_line(dbg, "echo hi")
    after_echo = ink(qmp, tmp, "ut_echo.png", box)
    res.check("a builtin runs and its output appears", after_echo > base,
              f"{base} -> {after_echo}")

    if shot_dir:
        qmp.screenshot(os.path.abspath(os.path.join(shot_dir, "ring3-terminal.png")))

    # An EXTERNAL program: only reachable via spawn + pipe.
    type_line(dbg, "lscpu", settle=2.5)
    after_lscpu = ink(qmp, tmp, "ut_lscpu.png", box)
    res.check("an external /bin program is spawned and its output piped back",
              after_lscpu > after_echo * 2,
              f"echo left {after_echo} ink, lscpu left {after_lscpu} -- "
              "expected far more if the program's output really arrived")

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

    key(dbg, "0x1b")  # Esc
    dbg.settle()
    deadline = time.time() + SPAWN_TIMEOUT_S
    gone = False
    while time.time() < deadline:
        if dbg.window(TITLE) is None:
            gone = True
            break
    res.check("Esc closes it", gone)


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
