#!/usr/bin/env python3
"""Drive the SYSTEM TEXT CLIPBOARD across two apps and assert on it.

The clipboard used to hold FILES only (lib/uclip.h). It holds text now,
named by a `kind` in the message, and this is what proves the claim that
makes it a SYSTEM clipboard rather than a feature of one app: text
copied in Notepad pastes into the GUI Terminal, which shares no code
with it and reaches the clipboard through the same server.

FOUR CHECKS, and the order matters -- each leaves the state the next
one needs:

  1. **A paste with no text on the clipboard changes nothing.** Run
     FIRST, while the clipboard is empty or holds files, so it is a real
     assertion rather than a formality. This is what a `kind` field
     buys: without one, a paste would read a file path as a line of
     text.
  2. **A round trip through the DISK, not through pixels.** Type,
     select all, copy, collapse the selection to the end, paste, save,
     and read the file back with `sh cat` over the serial console -- an
     independent path that shares nothing with the editor that wrote
     it. The document must read as the text TWICE.
  3. **Cross-app.** The same clipboard pasted into the Terminal with
     Ctrl+Shift+V must put ink on the shell's prompt row. Asserted as a
     BAND -- the prompt row gains ink, a row above it does not -- rather
     than as a whole-screen difference, which anything at all would
     satisfy.
  4. **It survives the compositor.** The clipboard lived in the kernel
     because a clipboard the compositor owned would be emptied by a
     Force Quit, and this desktop kills its compositor on purpose. It
     is a supervised ring-3 service now, so that claim needs a check
     rather than a sentence: kill toywm, let init restart it, and the
     text must still be there.
  5. **An oversized copy is REFUSED, and does not disturb what is
     already there.** Opening pci.ids (1.6 MB) and pressing Ctrl-A
     Ctrl-C asks for far more than WIN_CLIP_BYTES. The clipboard must
     still hold what check 2 put there -- a truncating clipboard would
     replace it with the first 64 KB of pci.ids, which is exactly the
     failure the refusal exists to prevent.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/clipboard_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                      # noqa: E402

NOTEPAD = "/bin/wm/apps/notepad"
PCI_IDS = "/usr/share/hwdata/pci.ids"
SAVE_PATH = "/var/tmp/clip_test.txt"
PHRASE = "clipboard round trip"

CTRL_A, CTRL_C, CTRL_V, CTRL_S = "0x01", "0x03", "0x16", "0x13"
KEY_END = "0xf798"
ENTER = "0x0d"

# `gui key` takes a code; a space has no bare spelling on that line.
HEX = {" ": "0x20", "/": "0x2f", ".": "0x2e", "_": "0x5f", "-": "0x2d"}


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, ok, what):
        (self.passes if ok else self.fails).append(what)
        print(("  ok   " if ok else "  FAIL ") + what)


def key(dbg, k, *mods):
    dbg.send("gui key " + k + "".join(" " + m for m in mods))


def type_text(dbg, text):
    for ch in text:
        key(dbg, HEX.get(ch, ch))
    dbg.settle()


def ink(img, box):
    """Non-background pixels in a box, for a LIGHT-backgrounded app.

    Counting dark pixels is exactly wrong for the Terminal, whose grid
    is white on black -- there every pixel is "ink" and text REDUCES the
    count. That measure reported a paste that had plainly landed as
    nothing having happened. Anything over the Terminal uses changed()
    below instead, which does not care which way round the colours are.
    """
    px = img.crop(box).convert("L").tobytes()
    return sum(1 for v in px if v < 200)


def changed(a, b, box):
    """Pixels that differ between two shots, inside a box. Colour-blind
    by construction, so it is the right measure over a dark app."""
    from PIL import ImageChops
    d = ImageChops.difference(a.crop(box), b.crop(box)).convert("L")
    return sum(1 for v in d.tobytes() if v > 8)


def wm_pid(dbg):
    """The compositor's pid, from `ps` -- a REAL /bin program, so it
    needs the console's `sh` prefix. Without it the send returns
    nothing and every caller silently finds no process."""
    for line in (dbg.send("sh ps") or "").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[-1] == "toywm":
            return parts[0]
    return None


def wm_starts(dbg):
    """How many times init has brought the desktop up, counted in the
    log.

    **NOT THE PID.** Supervision restarts the compositor fast enough to
    reuse the dead process's slot, so the pid can be identical either
    side of a kill -- which reported a compositor that had genuinely
    died and come back as never having died at all. The log line init
    writes per start cannot be reused that way. dmesg is cumulative, so
    this is a count to COMPARE, never to read once."""
    return (dbg.send("sh dmesg") or "").count("toywm is ready")


def content_rect(dbg, title_starts):
    """The content x/y/w/h of the first window whose title starts with
    `title_starts`, straight out of `gui windows` -- never derived, so
    this cannot drift from where the compositor actually put it."""
    for line in (dbg.send("gui windows") or "").splitlines():
        if title_starts not in line or "|" not in line:
            continue
        right = line.split("|", 1)[1].split()
        if len(right) >= 4:
            return tuple(int(v) for v in right[:4])
    return None


def run(dbg, qmp, tmp, res):
    enter = dbg.send("gui state")
    del enter

    # --- check 1: a paste with no text on the clipboard --------------
    dbg.send("gui spawn " + NOTEPAD)
    time.sleep(2.0)
    dbg.settle()
    rect = content_rect(dbg, "untitled")
    if not rect:
        res.check(False, "Notepad opened")
        return
    cx, cy, cw, ch = rect
    qmp.click_at(cx + cw // 2, cy + ch // 2)
    dbg.settle()

    type_text(dbg, PHRASE)
    dbg.settle()
    before = qmp.screenshot(os.path.join(tmp, "clip_before.png"))
    key(dbg, CTRL_V)          # nothing on the clipboard yet
    dbg.settle()
    after = qmp.screenshot(os.path.join(tmp, "clip_noop.png"))
    from PIL import Image
    a, b = Image.open(before).convert("RGB"), Image.open(after).convert("RGB")
    body = (cx, cy, cx + cw, cy + ch // 2)
    res.check(ink(a, body) == ink(b, body),
              "a paste with no text on the clipboard leaves the document alone")

    # --- check 2: copy, paste, save, and read it back as TEXT --------
    key(dbg, CTRL_A)
    key(dbg, CTRL_C)
    dbg.settle()
    key(dbg, KEY_END)         # collapse the selection; a paste would replace it
    key(dbg, CTRL_V)
    dbg.settle()

    key(dbg, CTRL_S)          # no path yet -- this opens Save As
    time.sleep(1.0)
    dbg.settle()
    type_text(dbg, SAVE_PATH)
    key(dbg, ENTER)
    time.sleep(1.5)
    dbg.settle()

    out = dbg.send("sh cat " + SAVE_PATH) or ""
    res.check(out.count(PHRASE) == 2,
              "the saved file holds the phrase TWICE (%d)" % out.count(PHRASE))

    # --- check 3: the same clipboard, in another app -----------------
    dbg.open_app("Terminal")
    time.sleep(3.0)
    dbg.settle()
    trect = content_rect(dbg, "Terminal")
    if not trect:
        res.check(False, "Terminal opened")
    else:
        tx, ty, tw, th = trect
        pre = Image.open(qmp.screenshot(os.path.join(tmp, "term_pre.png"))).convert("RGB")
        key(dbg, CTRL_V, "shift")     # Ctrl+Shift+V
        dbg.settle()
        time.sleep(0.8)
        post = Image.open(qmp.screenshot(os.path.join(tmp, "term_post.png"))).convert("RGB")

        # ASSERT THE BAND, not the change (CLAUDE.md). A whole-window
        # difference would be satisfied by the taskbar clock ticking, so
        # measure inside the Terminal, and require the change to be a
        # CONTIGUOUS band near the top -- where a shell prompt is --
        # with everything below it untouched. A paste that scattered ink
        # down the grid would fail this while "something changed" passes.
        step = 16
        rows = [r for r in range(th // step)
                if changed(pre, post, (tx, ty + r * step, tx + tw, ty + (r + 1) * step))]
        res.check(bool(rows),
                  "Ctrl+Shift+V changed the Terminal")
        res.check(bool(rows) and max(rows) - min(rows) <= 1 and max(rows) < th // step // 3,
                  "and only in one band near the top -- the prompt (rows %s)" % rows)

    # --- check 4: the clipboard outlives the compositor ---------------
    #
    # THE INHERITED REQUIREMENT. This is the property that kept the
    # clipboard in ring 0, so a ring-3 service has to earn it rather
    # than assert it.
    #
    # **THE KILL IS ESTABLISHED, NOT ASSUMED.** The compositor's pid
    # must CHANGE across it: the first version of this check could not
    # find the pid, killed nothing, and reported the clipboard as having
    # survived an event that never happened.
    before_pid = wm_pid(dbg)
    before_starts = wm_starts(dbg)
    res.check(before_pid is not None, "found the compositor's pid")
    if before_pid:
        dbg.send("sh kill " + before_pid)
        starts = before_starts
        for _ in range(20):      # init restarts it; Restart=on-failure
            time.sleep(1.0)
            starts = wm_starts(dbg)
            if starts > before_starts:
                break
        res.check(starts > before_starts,
                  "the compositor really died and init restarted it "
                  "(%d -> %d starts logged)" % (before_starts, starts))
        enter_gui(qmp, dbg.sock_path)
        dbg.settle()

        dbg.send("gui spawn " + NOTEPAD)
        time.sleep(2.5)
        dbg.settle()
        srect = content_rect(dbg, "untitled")
        if not srect:
            res.check(False, "a Notepad opened after the compositor restarted")
        else:
            sx, sy, sw, sh = srect
            qmp.click_at(sx + sw // 2, sy + sh // 2)
            dbg.settle()
            key(dbg, CTRL_V)
            dbg.settle()
            key(dbg, CTRL_S)
            time.sleep(1.0)
            dbg.settle()
            type_text(dbg, SAVE_PATH + "3")
            key(dbg, ENTER)
            time.sleep(1.5)
            dbg.settle()
            out3 = dbg.send("sh cat " + SAVE_PATH + "3") or ""
            res.check(PHRASE in out3,
                      "the clipboard survived the compositor being killed")

    # --- check 5: an oversized copy is refused, and changes nothing ---
    dbg.send("gui spawn %s %s" % (NOTEPAD, PCI_IDS))
    time.sleep(3.0)
    dbg.settle()
    prect = content_rect(dbg, "pci.ids")
    if not prect:
        res.check(False, "Notepad opened pci.ids")
        return
    px, py, pw, ph = prect
    qmp.click_at(px + pw // 2, py + ph // 2)
    dbg.settle()
    key(dbg, CTRL_A)
    key(dbg, CTRL_C)          # 1.6 MB -- far past WIN_CLIP_BYTES
    dbg.settle()

    # The clipboard must be untouched: paste into a NEW document and
    # require the phrase back, not a slice of pci.ids.
    dbg.send("gui spawn " + NOTEPAD)
    time.sleep(2.0)
    dbg.settle()
    nrect = content_rect(dbg, "untitled")
    if not nrect:
        res.check(False, "a third Notepad opened")
        return
    nx, ny, nw, nh = nrect
    qmp.click_at(nx + nw // 2, ny + nh // 2)
    dbg.settle()
    key(dbg, CTRL_V)
    dbg.settle()
    key(dbg, CTRL_S)
    time.sleep(1.0)
    dbg.settle()
    type_text(dbg, SAVE_PATH + "2")
    key(dbg, ENTER)
    time.sleep(1.5)
    dbg.settle()
    out2 = dbg.send("sh cat " + SAVE_PATH + "2") or ""
    res.check(PHRASE in out2 and "PCI" not in out2,
              "an oversized copy was refused and left the clipboard alone")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "clipboard_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    res = Result()
    try:
        run(dbg, qmp, args.tmp, res)
    finally:
        dbg.close()

    print("\nclipboard_test: %d passed, %d failed" % (len(res.passes), len(res.fails)))
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
