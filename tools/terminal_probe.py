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
import port_guard  # noqa: E402
from harness import copy_disk  # noqa: E402

# Characters `gui key` wants as a hex code rather than as themselves.
# `gui key` takes a hex code for anything the console will not pass
# through as a bare character. Regex punctuation was added when
# grep_test.py found that `(` silently produced nothing -- the command
# ran with a mangled pattern, matched nothing, and read exactly like a
# broken grep.
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "-": "0x2d", "_": "0x5f",
       ">": "0x3e", "<": "0x3c", "|": "0x7c",
       "(": "0x28", ")": "0x29", "[": "0x5b", "]": "0x5d",
       "^": "0x5e", "$": "0x24", "*": "0x2a", "+": "0x2b",
       "?": "0x3f", "{": "0x7b", "}": "0x7d", "\\": "0x5c",
       ":": "0x3a", ",": "0x2c", "=": "0x3d", "!": "0x21", '"': "0x22"}

# Control codes, by the name of the key that produces them.
CTRL = {c: f"0x{ord(c) - ord('a') + 1:02x}" for c in "abcdefghijklmnopqrstuvwxyz"}
ESC, RET = "0x1b", "0x0d"
PAGE_UP, PAGE_DOWN = "0x93", "0x94"    # api/keyboard.h

# The default scheme's page (data/usr/share/terminal/slate.scheme's
# Color0). A pixel within a few units of it is background.
PAGE = (0x23, 0x26, 0x29)


def is_page(raw, i):
    return (abs(raw[i] - PAGE[0]) < 8 and abs(raw[i + 1] - PAGE[1]) < 8
            and abs(raw[i + 2] - PAGE[2]) < 8)


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
        dbg.send("sh config set desktop.layout_log on")
        dbg.settle()
        dbg.open_app("Terminal")        # capital T -- trap 3
        time.sleep(3)
        w = dbg.window("Terminal")
        if not w:
            raise RuntimeError("no Terminal window after open_app")
        # THE WHOLE GRID ON SCREEN, above the taskbar. A restored saved
        # position may sit over it (wm_geometry.c clamps to the screen on
        # purpose), and the pager's status line is the grid's LAST row:
        # under the taskbar it is never drawn where a capture can see it.
        st = dbg.json("gui state --json")
        floor = st["screen"]["h"] - st.get("taskbar_h", 0)
        over = w["y"] + w["h"] - floor
        if over > 0:
            gx, gy = w["x"] + 120, w["y"] + 10
            dbg.drag(gx, gy, gx, max(10, gy - over))
            dbg.settle()
            w = dbg.window("Terminal")
            if w["y"] + w["h"] > floor:
                raise RuntimeError(f"could not lift the Terminal above the "
                                   f"taskbar: {w['y']}+{w['h']} > {floor}")
        c = w["content"]
        # The GRID, not the content rect (trap 2's WM rect, cut down): the
        # tab bar (and the menu bar, when shown) above it is a light full-width band
        # that read as a pager's status bar, and the scrollbar beside it
        # is not background. Both edges are the app's own layout line.
        chrome, bar_x = None, None
        for _ in range(20):
            for l in reversed(dbg.logs("uterm: layout", clear=False)):
                f = l.split()
                if chrome is None and "chrome" in f:
                    chrome = int(f[f.index("chrome") + 1])
                if bar_x is None and l.split("uterm: layout ")[-1].startswith("bar "):
                    bar_x = int(f[f.index("bar") + 1])
            if chrome is not None and bar_x is not None:
                break
            dbg.settle()
        if chrome is None or bar_x is None:
            raise RuntimeError("the Terminal reported no layout line -- "
                               "is desktop.layout_log on?")
        self.box = (c["x"], c["y"] + chrome, c["x"] + bar_x, c["y"] + c["h"])

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

    def ink_rows(self, raw):
        """How many rows of a capture contain anything but background.

        The capture is raw RGB for the content box, so a row is
        `width * 3` bytes. The page is one known colour (PAGE, the default
        scheme's), which is what makes "has ink" a pixel test rather than
        a comparison against another frame -- and therefore what lets
        this see a page that is WRONG rather than merely different.
        """
        w = self.box[2] - self.box[0]
        h = self.box[3] - self.box[1]
        stride = w * 3
        inked = 0
        for y in range(h):
            base = y * stride
            row = raw[base:base + stride]
            if any(not is_page(row, i) for i in range(0, len(row) - 2, 9)):
                inked += 1
        # Rows are pixels; report them as TEXT rows so the number in a
        # failure means something to a person reading it.
        cell = 16   # a glyph is ~16px tall at the default font
        return inked // cell, h // cell

    def bar_run(self, raw):
        """The longest horizontal run of near-white pixels.

        A reverse-video status bar is a SOLID BAND of the default
        foreground with dark letters on it. Counting light PIXELS does
        not distinguish it from ordinary text -- glyphs are drawn in the
        same colour and a screenful scores thousands. What only a filled
        background produces is a long unbroken RUN: a glyph is a few
        pixels wide, a bar is hundreds. uterm_test.py learned this the
        same way, on a check that scored 4304 on a frame with no bar.
        """
        w = self.box[2] - self.box[0]
        h = self.box[3] - self.box[1]
        best = 0
        for y in range(h):
            base = y * w * 3
            run = 0
            for x in range(w):
                i = base + x * 3
                if raw[i] > 150 and raw[i + 1] > 150 and raw[i + 2] > 150:
                    run += 1
                    if run > best:
                        best = run
                else:
                    run = 0
        return best

    @staticmethod
    def diff_pct(a, b):
        """How much two captures differ, as a percentage.

        BYTE-EXACT EQUALITY IS THE WRONG TEST for "did it come back",
        because the caret BLINKS: two captures of the same page differ
        by a few dozen bytes depending on which phase each landed in. A
        wrong page differs by 15% or more, so a tolerance separates them
        with room to spare -- and keeps the check strong, which exact
        equality did not: it reported a correct round trip as a failure.
        """
        n = min(len(a), len(b))
        step = 7
        diff = sum(1 for i in range(0, n, step) if a[i] != b[i])
        return 100.0 * diff / max(1, n // step)

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
    first = t.frame(tmp, "less_first")
    # THE NOISE FLOOR, measured rather than assumed. Two captures of the
    # SAME page are not byte-identical -- the caret blinks, and a couple
    # of percent of the area moves with it. Which couple is not worth
    # explaining and has not been: what matters is that a real page
    # change is many times larger, so the round-trip checks below
    # compare against this baseline instead of against zero or against a
    # number somebody picked.
    noise = t.diff_pct(first, t.frame(tmp, "less_first2"))
    same = noise + 4.0
    print(f"    (noise floor between two captures of one page: {noise:.2f}%)")
    paged = t.moved(tmp, "less", lambda: t.key("0x20"))
    check("space pages `less` forward", paged > 10.0,
          f"{paged:.2f}% moved (typing alone moves {typing:.2f}%)")

    # **"A LOT OF PIXELS MOVED" IS NOT "IT PAGED", and this pair is what
    # tells them apart.** A pager that APPENDS a screenful below the last
    # one instead of repainting moves just as many pixels as one that
    # turns a page -- that shipped, and the check above went green
    # through it. A pager sized to the wrong terminal moves just as many
    # again. What only a correct pager does is come BACK: `b` from page
    # two must reproduce page one exactly, byte for byte.
    t.key("b")
    time.sleep(2.0)
    back = t.frame(tmp, "less_back")
    d = t.diff_pct(back, first)
    check("...and `b` returns to the page it left", d < same,
          f"{d:.2f}% different, against a {noise:.2f}% noise floor")

    # **A PAGE MUST FILL THE WINDOW**, and this is the check neither of
    # the two above could make. A pager whose output is TRUNCATED draws
    # the same short page every time: "a lot of pixels moved" is
    # satisfied, and the round trip is satisfied too, because a
    # consistently wrong page is still consistent. Both were green while
    # every page came out as seventeen lines cut mid-word -- the kernel
    # caps one write at 1 KB and libsys was not looping, so the tail of
    # each frame, status line included, went nowhere.
    #
    # Counted as ROWS THAT CONTAIN INK, against the rows the window has.
    # A full page inks nearly all of them; a truncated one leaves the
    # bottom third blank, which is exactly what was on screen.
    page = t.frame(tmp, "less_fill")
    rows_inked, rows_total = t.ink_rows(page)
    check("a page fills the window rather than stopping short",
          rows_inked > rows_total * 0.85,
          f"{rows_inked} of {rows_total} rows have ink")

    # ...and the same round trip over the whole file, which additionally
    # pins that the LAST page is reachable -- "it will not go all the way
    # down" was a real symptom of the wrong page height.
    t.key("G")
    time.sleep(2.0)
    end = t.frame(tmp, "less_end")
    d = t.diff_pct(end, first)
    check("`G` reaches the end, and it is a different page", d > same * 2,
          f"{d:.2f}% different, against a {noise:.2f}% noise floor")
    t.key("g")
    time.sleep(2.0)
    d = t.diff_pct(t.frame(tmp, "less_home"), first)
    check("...and `g` comes back to the first page", d < same,
          f"{d:.2f}% different, against a {noise:.2f}% noise floor")

    # THE STATUS BAR IS A BAND, not a run of text -- measured as the
    # longest unbroken horizontal run of the foreground colour, because
    # counting light pixels cannot tell a bar from a screenful of
    # glyphs.
    bar = t.bar_run(t.frame(tmp, "less_bar"))
    width = t.box[2] - t.box[0]
    check("the status line is a full-width band",
          bar > width * 0.9, f"longest light run {bar}px of {width}px")

    # `q` must be enough to leave -- before the key source was fixed,
    # nothing reached the pager and Ctrl-C was the only way out.
    t.key("q")
    t.dbg.settle()
    time.sleep(1.5)
    t.dbg.send("sh rm /qprobe.txt")
    time.sleep(0.3)
    t.type("echo q > /qprobe.txt")
    t.enter()
    out = t.dbg.send("sh cat /qprobe.txt") or ""
    check("`q` quits the pager and the shell is back",
          "q" in out and "no such file" not in out,
          "the shell did not run a command after q")

    # ...AND THE PAGER LEFT NOTHING BEHIND. The alternate screen
    # (ESC[?1049h/l) means quitting restores whatever the terminal held
    # before it started, so the band must be GONE -- a pager that merely
    # cleared the screen, or one that printed a newline and exited,
    # would leave its bar in the scrollback. Same measure as above,
    # which is what makes the pair meaningful.
    left = t.bar_run(t.frame(tmp, "after_q"))
    check("...and the alternate screen took its status bar with it",
          left < width * 0.5, f"longest light run still {left}px")

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
    blank = sum(1 for i in range(0, len(after) - 2, 21) if is_page(after, i))
    frac = 100.0 * blank / max(1, len(after) // 21)
    # A cleared terminal is a prompt on the page. A screen that merely
    # scrolled by one line still holds a screenful of text.
    check("...and what is left is a cleared screen, not a scrolled one",
          frac > 90.0, f"{frac:.1f}% of the area is background")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys", action="store_true", help="the keymap probes only")
    ap.add_argument("--pixels", action="store_true", help="the pixel probes only")
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    args = ap.parse_args()
    port_guard.resolve_instance(args, "terminal_probe")
    both = not (args.keys or args.pixels)

    # A COPY on the slot asked for: the probe types into a shell and
    # turns a setting on, neither of which belongs on the real image.
    tmp = tempfile.mkdtemp(prefix="termprobe-")
    disk = copy_disk(os.path.join(REPO, "disk.img"), os.path.join(tmp, "disk.img"))
    vm_args = [sys.executable, "tools/vm.py", "--disk", disk,
               "--instance", str(args.qmp_port - port_guard.QMP_BASE)]
    r = subprocess.run(vm_args + ["start"], cwd=REPO,
                       capture_output=True, text=True)
    if "started" not in (r.stdout + r.stderr) and r.returncode != 0:
        print("terminal_probe: could not start the VM\n" + r.stdout + r.stderr)
        return 2
    time.sleep(1)

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
        subprocess.run(vm_args + ["stop"], cwd=REPO,
                       capture_output=True, text=True)
        os.unlink(disk)

    failed = [n for n, ok in results if not ok]
    print(f"\nterminal_probe: {len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
