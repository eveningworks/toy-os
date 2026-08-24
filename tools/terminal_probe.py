#!/usr/bin/env python3
"""The GUI Terminal, asked what it actually does: keys, paging, scrolling.

WHY THIS EXISTS, and it is not "another GUI test". Answering three
ordinary questions about the Terminal -- does Ctrl-L clear, does the
wheel scroll, does `less` page -- took FIVE invalid runs before it took
one valid one, and every failure was in the harness rather than in the
OS. Each is encoded here so the sixth person does not pay for them
again:

  1. **A fixture smaller than one screen cannot test a pager.** The
     first attempt ran `less /etc/toyos.conf`, which fits on one screen,
     so less exited immediately and the space meant to page it landed at
     a shell prompt. The check could not tell that from a working pager.
     /tests/sample.txt exists for this, and every line names its own
     number so a capture can be placed exactly.

  2. **The taskbar clock ticks once a second**, so ANY two full-screen
     captures taken seconds apart differ no matter what happened. Crop
     to the window's CONTENT rect, which the WM will tell you
     (`gui windows --json` -> the `content` box). Do not compute it from
     the frame and a guessed title height.

  3. **`open_app` needs the desktop entry's exact name** -- "Terminal",
     not "terminal". A lowercase name opens nothing and returns
     normally, so the next thing you see is a window list with zero
     entries and a crop box of None.

  4. **`QMPSession.send_text()` silently drops uppercase and most
     punctuation.** `echo AB > /probe.txt` arrives as `echo  probe.txt`.
     Type through the debug console instead (`gui key <hex>`), which
     injects to the focused window with no layout in the way -- the
     mechanism uterm_test.py already used.

  5. **"The frame changed" is not a measurement.** The terminal caret
     BLINKS, so a dead screen differs between two captures. Report the
     FRACTION of the content area that moved: typing ten characters is
     ~0.2%, a page turn or a clear is >20%, and a caret is ~0.02%.
     Without the number, `less` not paging at all reads as "changed".

TWO KINDS OF PROBE, and the first is the one to reach for. A keystroke
that reached the shell and did the right thing leaves different BYTES ON
DISK -- so drive the Terminal with keys and assert through the
filesystem, which no redraw timing can fake. Pixels are for the
questions only pixels can answer: whether the screen cleared, whether it
scrolled.

Usage:
    python3 tools/terminal_probe.py              # every probe
    python3 tools/terminal_probe.py --keys       # the keymap only
    python3 tools/terminal_probe.py --pixels     # paging/scrolling only

Boots its own VM against a COPY of disk.img. Exits 0 if every probe
passed, 1 otherwise, 2 if it could not run at all.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
from gui_debug import DebugConsole, enter_gui                 # noqa: E402
from qmp_test import QMPSession                               # noqa: E402

# Characters `gui key` wants as a hex code rather than as themselves.
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f",
       ">": "0x3e", "<": "0x3c", "|": "0x7c"}

# Control codes, by the name of the key that produces them.
CTRL = {c: f"0x{ord(c) - ord('a') + 1:02x}" for c in "abcdefghijklmnopqrstuvwxyz"}
ESC, RET = "0x1b", "0x0d"
PAGE_UP, PAGE_DOWN = "0x93", "0x94"    # api/keyboard.h

FIXTURE = "/tests/sample.txt"          # 401 numbered lines -- trap 1
PROBE = "/probe.txt"

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"   -- {detail}" if detail else ""))


class Terminal:
    """A Terminal window, driven by injected keys."""

    def __init__(self, dbg, qmp):
        self.dbg, self.qmp = dbg, qmp
        dbg.open_app("Terminal")        # capital T -- trap 3
        time.sleep(3)
        w = dbg.window("Terminal")
        if not w:
            raise RuntimeError("no Terminal window after open_app")
        c = w["content"]
        # The CONTENT rect, from the WM -- trap 2.
        self.box = (c["x"], c["y"], c["x"] + c["w"], c["y"] + c["h"])

    def key(self, k):
        self.dbg.send(f"gui key {k}")

    def type(self, text):
        """Type literal text -- trap 4: through the debug console."""
        for ch in text:
            self.key(HEX.get(ch, ch))
        self.dbg.settle()

    def enter(self, settle=1.6):
        self.key(RET)
        self.dbg.settle()
        time.sleep(settle)

    # --- the filesystem probe ----------------------------------------

    def read_probe(self):
        """The probe file's first line, or a marker.

        The Terminal logs `uterm: layout ...` onto this SAME serial
        console, so the reply is the file's bytes mixed with an app's
        chatter -- reading the first line blindly returns a log line
        every time.
        """
        out = self.dbg.send(f"sh cat {PROBE}") or ""
        if "no such file" in out or "not found" in out:
            return "<MISSING>"
        noise = ("sh ", "elf_run", "syscall", "uterm:", "wm:", "cat:")
        body = [ln.strip() for ln in out.splitlines()
                if ln.strip() and not any(k in ln for k in noise)]
        return body[0] if body else "<empty>"

    def line(self, build):
        """Build a command line with editing keys in it, run it, read back."""
        self.dbg.send(f"sh rm {PROBE}")
        time.sleep(0.3)
        build()
        self.enter()
        return self.read_probe()

    # --- the pixel probe ---------------------------------------------

    def frame(self, tmp, tag):
        return self.qmp.stable_pixels(os.path.join(tmp, f"{tag}.png"), box=self.box)

    def moved(self, tmp, tag, act, settle=2.0):
        """Percent of the content area that changed -- trap 5.

        Sampled every 7th byte rather than every byte: the answer is a
        fraction, and a 640x400 window is a megabyte per capture.
        """
        a = self.frame(tmp, tag + "_a")
        act()
        time.sleep(settle)
        b = self.frame(tmp, tag + "_b")
        n = min(len(a), len(b))
        step = 7
        diff = sum(1 for i in range(0, n, step) if a[i] != b[i])
        return 100.0 * diff / max(1, n // step)


def probe_keys(t):
    """Editing keys, asserted through the FILESYSTEM."""
    # CONTROL FIRST. Everything below is void if plain typing does not
    # reach the shell -- which is exactly how four earlier attempts at
    # this went wrong without saying so.
    got = t.line(lambda: t.type(f"echo ab > {PROBE}"))
    check("CONTROL: plain typing reaches the shell", got == "ab", got)
    if got != "ab":
        print("  (control failed -- every result below would be meaningless)")
        return

    cases = [
        # Ctrl-A puts the caret at the start, so a 'z' typed there makes
        # the line begin "zecho ..." -- not a command, so NO file.
        ("Ctrl-A moves to the start of the line",
         lambda: (t.type(f"echo ab > {PROBE}"), t.key(CTRL["a"]), t.type("z")),
         "<MISSING>"),
        ("...and Ctrl-E moves back to the end",
         lambda: (t.type(f"echo ab > {PROBE}"), t.key(CTRL["a"]), t.key(CTRL["e"])),
         "ab"),
        ("Ctrl-W kills the word before the caret",
         lambda: (t.type("echo ab cd"), t.key(CTRL["w"]), t.type(f"> {PROBE}")),
         "ab"),
        ("Ctrl-U kills to the start of the line",
         lambda: (t.type("garbage"), t.key(CTRL["u"]), t.type(f"echo uu > {PROBE}")),
         "uu"),
        ("Ctrl-K kills to the end of the line",
         lambda: (t.type("echo bad"), t.key(CTRL["a"]), t.key(CTRL["k"]),
                  t.type(f"echo kk > {PROBE}")),
         "kk"),
        ("Ctrl-T swaps the two characters at the caret",
         lambda: (t.type("echo ba"), t.key(CTRL["t"]), t.type(f" > {PROBE}")),
         "ab"),
        ("Ctrl-Y yanks the last kill back",
         lambda: (t.type("echo yy"), t.key(CTRL["u"]), t.type("echo "),
                  t.key(CTRL["y"]), t.type(f" > {PROBE}")),
         "echo yy"),
        # Alt-<key> is ESC then the key -- TWO bytes. This is the one
        # case that tests the ESCAPE PREFIX rather than a control code.
        ("Alt-B moves back a word (ESC prefix)",
         lambda: (t.type(f"echo ab zz > {PROBE}"), t.key(ESC), t.key("b"),
                  t.key(ESC), t.key("b"), t.type("q")),
         "<MISSING>"),
    ]
    for name, build, want in cases:
        got = t.line(build)
        check(name, got == want, f"got {got!r}, wanted {want!r}")


def probe_pixels(t, tmp):
    """Paging, scrolling and clearing -- the questions only pixels answer."""
    # THE PIXEL CONTROL, and it doubles as the scale for everything
    # below: ten characters is what "a little changed" looks like.
    typing = t.moved(tmp, "ctl", lambda: t.type("echo hello"), settle=1.0)
    check("CONTROL: typing moves the content area", typing > 0.05,
          f"{typing:.2f}%")
    t.key(CTRL["u"])
    t.dbg.settle()

    # A pager on a 401-line fixture -- trap 1.
    t.type(f"less {FIXTURE}")
    t.enter(settle=2.5)
    paged = t.moved(tmp, "less", lambda: t.key("0x20"))
    check("space pages `less` forward", paged > 10.0,
          f"{paged:.2f}% moved (typing alone moves {typing:.2f}%)")
    t.key("q")
    t.dbg.settle()
    time.sleep(1.5)

    # ...and through a pipe, where fd 0 is NOT the terminal. A pager
    # that reads its keys from stdin cannot work here at all, which is
    # the difference this pair exists to show.
    t.type(f"cat {FIXTURE} | less")
    t.enter(settle=3.0)
    piped = t.moved(tmp, "pipe", lambda: t.key("0x20"))
    check("...and pages the same through a pipe", piped > 10.0,
          f"{piped:.2f}% moved")
    t.key(CTRL["c"])
    t.dbg.settle()
    time.sleep(1.5)

    # Scrollback, by key and by wheel -- the SAME state, so a terminal
    # that moved one and not the other has two notions of where the
    # reader is.
    t.type(f"cat {FIXTURE}")
    t.enter(settle=4.0)
    up = t.moved(tmp, "pgup", lambda: t.key(PAGE_UP))
    check("Page Up scrolls the scrollback", up > 10.0, f"{up:.2f}% moved")
    t.key(PAGE_DOWN)
    t.dbg.settle()
    time.sleep(1.0)

    wheel = t.moved(tmp, "wheel", lambda: t.dbg.send("gui wheel 3"))
    check("the mouse wheel scrolls it too", wheel > 5.0, f"{wheel:.2f}% moved")
    t.dbg.send("gui wheel -3")
    t.dbg.settle()
    time.sleep(1.0)

    # Ctrl-L must CLEAR, not scroll by a line. Both move a lot of
    # pixels, so the discriminator is what is left: a cleared screen is
    # nearly all background.
    cleared = t.moved(tmp, "ctrl_l", lambda: t.key(CTRL["l"]))
    check("Ctrl-L repaints the screen", cleared > 10.0, f"{cleared:.2f}% moved")
    after = t.frame(tmp, "after_clear")
    blank = sum(1 for i in range(0, len(after), 7) if after[i] == 0)
    frac = 100.0 * blank / max(1, len(after) // 7)
    # A cleared terminal is a prompt on black. A screen that merely
    # scrolled by one line still holds a screenful of text.
    check("...and what is left is a cleared screen, not a scrolled one",
          frac > 90.0, f"{frac:.1f}% of the area is background")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys", action="store_true", help="the keymap probes only")
    ap.add_argument("--pixels", action="store_true", help="the pixel probes only")
    ap.add_argument("--sock", default=".vm.serial")
    ap.add_argument("--qmp-port", type=int, default=4445)
    args = ap.parse_args()
    both = not (args.keys or args.pixels)

    r = subprocess.run([sys.executable, "tools/vm.py", "start"], cwd=REPO,
                       capture_output=True, text=True)
    if "started" not in (r.stdout + r.stderr) and r.returncode != 0:
        print("terminal_probe: could not start the VM\n" + r.stdout + r.stderr)
        return 2
    time.sleep(1)

    tmp = tempfile.mkdtemp(prefix="termprobe-")
    try:
        qmp = QMPSession(port=args.qmp_port)
        enter_gui(qmp, args.sock)
        dbg = DebugConsole(args.sock)
        t = Terminal(dbg, qmp)
        if args.keys or both:
            print("editing keys, through the filesystem")
            probe_keys(t)
        if args.pixels or both:
            print("paging, scrolling and clearing, through pixels")
            probe_pixels(t, tmp)
    finally:
        subprocess.run([sys.executable, "tools/vm.py", "stop"], cwd=REPO,
                       capture_output=True, text=True)

    failed = [n for n, ok in results if not ok]
    print(f"\nterminal_probe: {len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
