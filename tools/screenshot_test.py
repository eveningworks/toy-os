#!/usr/bin/env python3
"""Screen capture: /bin/screenshot, the Screenshot app, and the region band.

WHAT IS UNDER TEST
------------------
WIN_REQ_SCREENSHOT (userland/wm/wm_screenshot.c), the client half
(userland/lib/ushot.c), the command (userland/bin/screenshot.c) and the
app (userland/gui/apps/screenshot.c) -- and, through all of them, the two
encoders in userland/lib/uimg_qoi.c and uimg_png.c.

THE ASSERTION THAT MATTERS IS A ROUND TRIP, NOT "A FILE APPEARED"
-----------------------------------------------------------------
A capture that wrote a valid, well-formed image of the WRONG THING would
pass every "did it produce a file" check there is -- and the two ways it
could be wrong are both real: the compositor's back buffer can be a frame
old, and its alpha byte is zero, which every alpha-aware consumer reads
as fully transparent (that one shipped, as a black preview).

So the file is pulled OFF the guest's disk and compared PIXEL BY PIXEL
against a QMP screendump of the same screen, by Pillow -- a decoder that
shares no code with the one in this repo. The cursor box is excluded,
because that is the one place the two are SUPPOSED to differ: a
screendump photographs the framebuffer, cursor and all, and a capture
leaves the pointer out unless asked.

Four more things it asserts that a weaker version would not:

1. `--pointer` PUTS THE CURSOR BACK, and the no-pointer capture has it
   removed -- checked as a count of distinct colours in the cursor's own
   box, not as "the files differ".
2. `-w` REPORTS THE WINDOW'S SIZE, compared against what `gui windows`
   says that window is. A capture that silently fell back to the whole
   screen is the obvious failure and is the one this catches.
3. THE OVERLAY IS NOT IN ITS OWN PICTURE (WIN_SHOT_NO_SELF): a Screen
   capture taken through PrtSc equals the screendump from before it
   opened -- the pill, the dim and the app's taskbar button all absent.
4. A REGION CROPS TO WHAT WAS DRAGGED, and a corner handle resizes it,
   to the pixel, against that same screendump.
5. THE WINDOW TARGET FOLLOWS THE POINTER from one window to the next,
   checked against the geometry the compositor reports, and a click
   captures that window at its size.
6. The compositor's card names the file and offers Open, Copy and
   Folder; Esc saves nothing; the copy toggle is said on the card.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/screenshot_test.py
    python3 tools/vm.py stop

Needs Pillow. Skips with a clear message if it is missing rather than
failing, the way the other Pillow-dependent tools here do.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard                                       # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("screenshot_test: needs Pillow (pip install --user pillow) -- skipped")
    sys.exit(0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOT_DIR = "/var/tmp"        # the disk, not the ramfs: this survives to be read
checks = []


def check(name, ok, detail="", fail_detail=None):
    """`fail_detail` replaces `detail` when the check fails -- so a check
    whose only useful note is what went WRONG does not print an alarming
    sentence beside a PASS."""
    if not ok and fail_detail:
        detail = fail_detail
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def pixels(im):
    """Pillow 14 renames getdata(); both spellings have to work here."""
    if hasattr(im, "get_flattened_data"):
        return list(im.get_flattened_data())
    return list(im.getdata())


def pull(disk, guest_path, local_path):
    """Read a file off the guest's filesystem, from the HOST side.

    The guest has no way to hand a binary out through the debug console,
    and the point of this tool is to look at the bytes it wrote -- so the
    image is read straight out of the TFS3 volume. `volume_of()` FINDS
    the filesystem rather than assuming partition 1, which on a bootable
    image is GRUB's (tools/mkpart_test.py says why).
    """
    from mkpart_test import volume_of
    base, count = volume_of(disk)
    r = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "tfs3_writer.py"), "read",
         "--at-lba", str(base), "--sectors", str(count),
         disk, guest_path, local_path],
        capture_output=True)
    return r.returncode == 0 and os.path.exists(local_path)


def disk_from_qemu(qmp):
    """The path QEMU has open, off `info block`. One line, first entry."""
    for line in qmp.hmp("info block").splitlines():
        if "(raw)" in line and ": /" in line:
            path = line.split(": ", 1)[1].rsplit(" (", 1)[0]
            if os.path.exists(path):
                return path
    return None


def cursor_box(dbg):
    """The pointer's drawn extent, generously, for ANY shape: the arrow
    hangs down and right of its hotspot, but an I-beam or a move cross is
    drawn CENTRED on it -- the I-beam over a text field reached 10 px above
    the old arrow-only box and failed a comparison it had nothing to do with."""
    c = dbg.json("gui state --json")["cursor"]
    return (c["x"] - 14, c["y"] - 16, c["x"] + 18, c["y"] + 24)


def clock_box(dbg):
    """The taskbar clock, asked of the compositor: it shows SECONDS, so a
    screendump and a capture either side of a tick differ there -- which
    is a property of the clock, not of the capture. The suite's slower
    emulation crossed the tick often enough to fail a check now and then."""
    try:
        c = dbg.json("gui calendar --json").get("clock")
    except Exception:
        c = None
    if not c:
        return None
    return (c["x"] - 2, c["y"] - 2, c["x"] + c["w"] + 2, c["y"] + c["h"] + 2)


def in_any(x, y, boxes):
    return any(b and b[0] <= x < b[2] and b[1] <= y < b[3] for b in boxes)


def compare(a, b, skip=None):
    """Differing pixel count, ignoring anything inside `skip` -- one box,
    or a list of them."""
    boxes = skip if isinstance(skip, list) else [skip]
    pa, pb = pixels(a.convert("RGB")), pixels(b.convert("RGB"))
    if a.size != b.size:
        return -1
    w = a.size[0]
    n = 0
    for i, (x, y) in enumerate(((i % w, i // w) for i in range(len(pa)))):
        if in_any(x, y, boxes):
            continue
        if pa[i] != pb[i]:
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for the images")
    ap.add_argument("--disk", default=None,
                    help="the image the guest is booted from "
                         "(default: ask QEMU which one it has)")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "screenshot_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    out = lambda n: os.path.join(outdir, n)  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)

    # WHICH IMAGE THIS GUEST IS ACTUALLY ON, asked of QEMU rather than
    # assumed to be the repo's disk.img. gui_regress.py boots a per-tool
    # COPY, so a hardcoded path would have this tool reading a file the
    # guest never wrote -- and the monitor is the one oracle the guest
    # cannot fake (CLAUDE.md).
    disk = args.disk or disk_from_qemu(qmp)
    if not disk:
        print("screenshot_test: could not find the guest's disk image")
        return 1

    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print(f"screen capture (/bin/screenshot, the app, the region band) on {disk}")

    # --- 1. the command writes a file, and it is THE SCREEN ------------
    #
    # The screendump comes FIRST and the guest capture second, because a
    # settled screendump is a photograph of a screen that has stopped
    # moving -- taking it afterwards would race the app's own repaint.
    ref = out("qmp_ref.png")
    qmp.screenshot(ref)
    dbg.send(f"sh spawn /bin/screenshot {SHOT_DIR}/t.qoi")
    dbg.settle()
    dbg.send("sh sync")
    time.sleep(0.3)

    got = out("guest.qoi")
    if not check("the command wrote a file the host can read",
                 pull(disk, f"{SHOT_DIR}/t.qoi", got)):
        return report()

    shot = Image.open(got)
    refim = Image.open(ref)
    check("...at the screen's size", shot.size == refim.size,
          f"{shot.size} vs {refim.size}")

    cur = cursor_box(dbg)
    skip = [cur, clock_box(dbg)]
    diff = compare(shot, refim, skip=skip)
    total = shot.size[0] * shot.size[1]
    check("...and every pixel outside the cursor matches the screendump",
          diff == 0, f"{diff} of {total} differ")

    # THE CONTROL for that check: inside the cursor box the two MUST
    # differ, or the comparison above is passing because the capture is
    # a copy of the screendump rather than because it is correct.
    incur = compare(shot.crop(cur), refim.crop(cur))
    check("...while the cursor box differs, as it must", incur > 0,
          f"{incur} pixels differ inside the pointer")

    # --- 2. --pointer puts it back -------------------------------------
    dbg.send(f"sh spawn /bin/screenshot -p {SHOT_DIR}/p.qoi")
    dbg.settle()
    dbg.send("sh sync")
    time.sleep(0.3)
    pgot = out("guest_ptr.qoi")
    if check("--pointer wrote a file", pull(disk, f"{SHOT_DIR}/p.qoi", pgot)):
        pim = Image.open(pgot)
        # COLOUR COUNT, not "the files differ": a cursor is an
        # antialiased arrow, so its box goes from a couple of wallpaper
        # shades to a dozen or more. Two captures of a moving desktop
        # would "differ" with no cursor in either.
        with_p = len(set(pixels(pim.crop(cur).convert("RGB"))))
        without = len(set(pixels(shot.crop(cur).convert("RGB"))))
        check("the pointer is drawn with --pointer and not without",
              with_p > without + 3, f"{with_p} colours vs {without}")

    # --- 3. -w captures the window, at the window's size ---------------
    dbg.open_app("Calculator")
    dbg.settle()
    time.sleep(0.4)
    wins = [w for w in dbg.windows() if w["title"] == "Calculator"]
    if check("Calculator opened, to capture", len(wins) == 1):
        w = wins[0]
        dbg.send(f"sh spawn /bin/screenshot -w {SHOT_DIR}/w.qoi")
        dbg.settle()
        dbg.send("sh sync")
        time.sleep(0.3)
        wgot = out("guest_win.qoi")
        if check("-w wrote a file", pull(disk, f"{SHOT_DIR}/w.qoi", wgot)):
            wim = Image.open(wgot)
            check("-w is the WINDOW's size, not the screen's",
                  wim.size == (w["w"], w["h"]),
                  f"{wim.size} vs window {w['w']}x{w['h']}")

    # --- 4..8. THE OVERLAY (the app, mockup S3) -------------------------
    overlay_checks(dbg, disk, out, qmp, skip)
    return report()


# --- the overlay --------------------------------------------------------
#
# The app IS a fullscreen overlay over a frozen frame (userland/gui/apps/
# screenshot.c): PrtSc opens it, a pill picks Region / Screen / Window,
# the shutter saves to /home/screenshots and quits, and the compositor
# puts up a card with Open / Copy / Folder. Everything is asked of the
# app's layout log and the compositor's `gui state`, never derived here.

def layout(dbg):
    """The overlay's latest frame: {name: [ints or words]}."""
    lay = {}
    for line in dbg.logs("screenshot: layout ", clear=False):
        parts = line.split("screenshot: layout ", 1)[1].split()
        lay[parts[0]] = parts[1:]
    return lay


def rect_of(lay, name):
    v = lay.get(name)
    return [int(x) for x in v[:4]] if v and len(v) >= 4 else None


def open_overlay(dbg, timeout=10.0):
    """PrtSc, and wait for the fullscreen overlay. Its content rect."""
    dbg.logs("screenshot:", clear=True)
    dbg.send("gui key 0xb9")
    deadline = time.time() + timeout
    while time.time() < deadline:
        w = [x for x in dbg.windows() if x["title"] == "Screenshot"]
        if w and w[0]["state"] == "fullscreen" and layout(dbg).get("pill"):
            return w[0]["content"]
        time.sleep(0.3)
    return None


def press_ctl(dbg, c, lay, name):
    r = rect_of(lay, name)
    dbg.click(c["x"] + r[0] + r[2] // 2, c["y"] + r[1] + r[3] // 2)
    dbg.settle()


def gone(dbg, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not any(x["title"] == "Screenshot" for x in dbg.windows()):
            return True
        time.sleep(0.3)
    return False


def newest(dbg):
    names = sorted(n for n in dbg.send("sh ls /home/screenshots").split() if n.endswith(".qoi"))
    return names[-1] if names else None


def sampled_diff(a, b, box, skip, origin=(0, 0)):
    """Pixels of `a` (a crop at `origin`) that differ from `b`, on a grid;
    `skip` is a list of boxes left out."""
    n = 0
    ox, oy = origin
    for y in range(box[1], box[3], 4):
        for x in range(box[0], box[2], 4):
            if in_any(x, y, skip):
                continue
            if a.getpixel((x - ox, y - oy)) != b.getpixel((x, y)):
                n += 1
    return n


def overlay_checks(dbg, disk, out, qmp, skip):
    dbg.open_app("Calculator")
    dbg.settle()
    dbg.open_app("Notepad")
    dbg.settle()
    time.sleep(0.8)
    # THE BASELINE: the screen before the overlay exists. Every capture
    # below must equal it -- the frame is frozen, and the app is absent
    # from it, taskbar button included (WIN_SHOT_NO_SELF).
    base_path = out("qmp_before_overlay.png")
    qmp.screenshot(base_path)
    base = Image.open(base_path).convert("RGB")
    W, H = base.size
    # The screendump has the pointer and a capture does not; the clock ticks.
    skip = [cursor_box(dbg), clock_box(dbg)]

    # --- 4. PrtSc opens the overlay; Screen mode is the whole frozen frame
    c = open_overlay(dbg)
    if not check("PrtSc opens the overlay, fullscreen, with its pill", c is not None):
        return
    dbg.send("gui key s")
    dbg.settle()
    lay = layout(dbg)
    check("s picks Screen, and the selection is the whole screen",
          (lay.get("mode") or [""])[0] == "screen" and rect_of(lay, "selection") == [0, 0, W, H],
          str(lay.get("selection")))
    press_ctl(dbg, c, lay, "shutter")
    check("the shutter closes the overlay", gone(dbg))
    dbg.send("sh sync")
    time.sleep(0.3)
    name = newest(dbg)
    got = out("overlay_screen.qoi")
    if check("...and saves a capture", bool(name) and pull(disk, "/home/screenshots/" + name, got)):
        im = Image.open(got).convert("RGB")
        check("...at the screen's size", im.size == (W, H), str(im.size))
        if im.size == (W, H):
            n = sampled_diff(im, base, (0, 0, W, H), skip)
            check("...equal to the screen as it was: the overlay, the pill and its "
                  "taskbar button are not in it", n == 0, f"{n} sampled pixels differ")

    # --- 5. the card: the compositor's, with the file and three actions
    st = dbg.state()
    nt = st.get("notice") or {}
    labels = [b.get("label") for b in nt.get("buttons", [])]
    check("the compositor shows a card for the saved file",
          nt.get("title") == "Screenshot saved" and (nt.get("path") or "").endswith(name or "?"),
          str(nt))
    check("...with Open, Copy and Folder", labels == ["Open", "Copy", "Folder"], str(labels))

    # --- 6. Region: a drag, then a handle; the crop is what was chosen
    c = open_overlay(dbg)
    if not check("PrtSc again", c is not None):
        return
    dbg.send("gui key r")
    dbg.settle()
    dbg.drag(300, 200, 700, 500, steps=8)
    dbg.settle()
    time.sleep(0.5)
    sel = rect_of(layout(dbg), "selection")
    check("a drag selects exactly that rectangle", sel == [300, 200, 400, 300], str(sel))
    # The bottom-right handle, dragged: the region grows from that corner.
    dbg.drag(700, 500, 740, 520, steps=6)
    dbg.settle()
    time.sleep(0.5)
    lay = layout(dbg)
    sel = rect_of(lay, "selection")
    check("a corner handle resizes it from that corner", sel == [300, 200, 440, 320], str(sel))
    press_ctl(dbg, c, lay, "shutter")
    gone(dbg)
    dbg.send("sh sync")
    time.sleep(0.3)
    name = newest(dbg)
    got = out("overlay_region.qoi")
    if name and pull(disk, "/home/screenshots/" + name, got):
        im = Image.open(got).convert("RGB")
        check("the crop is exactly the chosen region", im.size == (440, 320), str(im.size))
        if im.size == (440, 320):
            n = sampled_diff(im, base, (300, 200, 740, 520), skip, origin=(300, 200))
            check("...and it is those pixels of the frozen frame", n == 0, f"{n} differ")

    # --- 7. Window: the target follows the pointer from window to window
    wins = {x["title"]: x for x in dbg.windows()}
    calc, note = wins.get("Calculator"), next((v for k, v in wins.items() if "untitled" in k), None)
    c = open_overlay(dbg)
    if check("PrtSc a third time", c is not None) and calc and note:
        dbg.send("gui key w")
        dbg.settle()
        # A point on Calculator that Notepad, opened over it, does not cover.
        cx, cy = calc["x"] + 8, calc["y"] + 8
        if note["x"] <= cx < note["x"] + note["w"] and note["y"] <= cy < note["y"] + note["h"]:
            cx, cy = calc["x"] + calc["w"] - 8, calc["y"] + calc["h"] - 8
        dbg.warp_cursor(qmp, cx, cy)
        time.sleep(0.6)
        a = rect_of(layout(dbg), "selection")
        check("Window: the window under the pointer is the target",
              a == [calc["x"], calc["y"], calc["w"], calc["h"]], f"{a} vs Calculator {calc}")
        # Notepad's bottom-right corner, clear of Calculator over it.
        px, py = note["x"] + note["w"] - 30, note["y"] + note["h"] - 30
        dbg.warp_cursor(qmp, px, py)
        time.sleep(0.6)
        b = rect_of(layout(dbg), "selection")
        check("...and it moves to the next window under it",
              b == [note["x"], note["y"], note["w"], note["h"]], f"{b} vs Notepad {note}")
        dbg.click(px, py)
        gone(dbg)
        dbg.send("sh sync")
        time.sleep(0.3)
        name = newest(dbg)
        got = out("overlay_window.qoi")
        if name and pull(disk, "/home/screenshots/" + name, got):
            im = Image.open(got)
            check("a click captures that window at its size",
                  im.size == (note["w"], note["h"]), f"{im.size}")

    # --- 8. Esc saves nothing; the copy toggle is kept and is said
    before = newest(dbg)
    c = open_overlay(dbg)
    if check("PrtSc a fourth time", c is not None):
        dbg.send("gui key 0x1b")
        check("Esc closes the overlay", gone(dbg))
        check("...and saves nothing", newest(dbg) == before)
    c = open_overlay(dbg)
    if check("PrtSc a fifth time", c is not None):
        dbg.send("gui key c")      # copy on
        dbg.send("gui key s")
        dbg.settle()
        lay = layout(dbg)
        check("c turns 'copy to the clipboard' on",
              (lay.get("options") or [])[2:4] == ["copy", "1"], str(lay.get("options")))
        press_ctl(dbg, c, lay, "shutter")
        gone(dbg)
        nt = dbg.state().get("notice") or {}
        check("...and the card says it is on the clipboard",
              "clipboard" in (nt.get("sub") or ""), str(nt.get("sub")))
    # Back off, for whoever runs next: the preference is kept.
    c = open_overlay(dbg)
    if c:
        dbg.send("gui key c")
        dbg.settle()
        for _ in range(3):
            dbg.send("gui key 0x1b")
            if gone(dbg, timeout=3.0):
                break

    # EVERY RUN WAS REAPED: the shortcut's launches are the compositor's
    # children, and an untracked one stayed a zombie for every PrtSc.
    time.sleep(1.0)
    zombies = [l for l in dbg.send("sh ps").splitlines() if "zombie" in l and "screenshot" in l]
    check("no capture is left a zombie", not zombies, f"{len(zombies)} zombies")


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nscreenshot_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
