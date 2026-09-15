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
3. THE APP IS NOT IN ITS OWN SCREENSHOT (WIN_SHOT_NO_SELF), checked by
   comparing the app window's rect in the capture against the desktop
   behind it -- not by looking at the picture.
4. THE REGION BAND CROPS TO WHAT WAS DRAGGED, to the pixel, against the
   full-screen capture taken moments earlier.
5. THE WINDOW PICKER CROPS TO THE WINDOW UNDER THE POINTER, checked
   against the geometry the compositor independently reports for that
   window -- and the outline has to MOVE when the pointer crosses from
   one window to another, which is the half a "it produced a file" check
   cannot see.

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
    """The pointer's drawn extent, generously. wm_render.c anchors the
    built-in arrow at the hotspot and draws 13x19 down and right of it."""
    c = dbg.json("gui state --json")["cursor"]
    return (c["x"] - 3, c["y"] - 3, c["x"] + 16, c["y"] + 22)


def compare(a, b, skip=None):
    """Differing pixel count, ignoring anything inside `skip`."""
    pa, pb = pixels(a.convert("RGB")), pixels(b.convert("RGB"))
    if a.size != b.size:
        return -1
    w = a.size[0]
    n = 0
    for i, (x, y) in enumerate(((i % w, i // w) for i in range(len(pa)))):
        if skip and skip[0] <= x < skip[2] and skip[1] <= y < skip[3]:
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

    skip = cursor_box(dbg)
    diff = compare(shot, refim, skip=skip)
    total = shot.size[0] * shot.size[1]
    check("...and every pixel outside the cursor matches the screendump",
          diff == 0, f"{diff} of {total} differ")

    # THE CONTROL for that check: inside the cursor box the two MUST
    # differ, or the comparison above is passing because the capture is
    # a copy of the screendump rather than because it is correct.
    incur = compare(shot.crop(skip), refim.crop(skip))
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
        with_p = len(set(pixels(pim.crop(skip).convert("RGB"))))
        without = len(set(pixels(shot.crop(skip).convert("RGB"))))
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

    # --- 4. the app is not in its own screenshot -----------------------
    #
    # THE BASELINE IS TAKEN NOW, with Calculator up and the Screenshot
    # app not yet open. Comparing against step 1's instead compared a
    # screen with Calculator on it to one without, and reported the
    # feature broken over a window this tool had opened itself.
    base_path = out("qmp_before_app.png")
    qmp.screenshot(base_path)
    base = Image.open(base_path).convert("RGB")

    dbg.open_app("Screenshot")
    dbg.settle()
    time.sleep(0.5)
    app = [x for x in dbg.windows() if x["title"] == "Screenshot"]
    if check("the Screenshot app opened", len(app) == 1):
        a = app[0]
        rect = (a["x"], a["y"], a["x"] + a["w"], a["y"] + a["h"])
        take = None
        dbg.send("sh config set desktop.layout_log on")
        dbg.settle()
        time.sleep(0.8)
        for line in dbg.logs():
            if "screenshot: layout take " in line:
                take = [int(v) for v in line.split()[-4:]]
        if check("the app reports where its Take button is", take is not None):
            c = a["content"]
            dbg.click(c["x"] + take[0] + take[2] // 2,
                      c["y"] + take[1] + take[3] // 2)
            dbg.settle()
            time.sleep(0.8)
            check("the app survived taking one",
                  any(x["title"] == "Screenshot" for x in dbg.windows()),
                  fail_detail="its window is gone -- it crashed")

            # Its own capture is auto-saved; read the newest one.
            names = dbg.send("sh ls /home/screenshots").split()
            names = sorted(n for n in names if n.endswith(".qoi"))
            if check("the app saved a capture", bool(names), str(names)):
                dbg.send("sh sync")
                time.sleep(0.3)
                agot = out("app.qoi")
                if pull(disk, "/home/screenshots/" + names[-1], agot):
                    aim = Image.open(agot).convert("RGB")
                    # WIN_SHOT_NO_SELF: inside the app's own rect its
                    # capture must show what was THERE BEFORE it opened,
                    # not the app. The cursor box is skipped for the
                    # usual reason -- a screendump has the pointer and a
                    # capture does not.
                    if aim.size == base.size:
                        n = 0
                        for y in range(rect[1], min(rect[3], aim.size[1]), 4):
                            for x in range(rect[0], min(rect[2], aim.size[0]), 4):
                                if skip[0] <= x < skip[2] and skip[1] <= y < skip[3]:
                                    continue
                                if aim.getpixel((x, y)) != base.getpixel((x, y)):
                                    n += 1
                        check("the app is absent from its own screenshot",
                              n == 0, f"{n} sampled pixels differ inside its rect")

    # --- 5. the region band crops to what was dragged ------------------
    app = [x for x in dbg.windows() if x["title"] == "Screenshot"]
    mode = None
    if app and take:
        a = app[0]
        c = a["content"]
        for line in dbg.logs():
            if "screenshot: layout mode " in line:
                mode = [int(v) for v in line.split()[-4:]]
        if check("the app reports where its mode list is", mode is not None):
            row = mode[3] // 3
            dbg.click(c["x"] + mode[0] + 40, c["y"] + mode[1] + row * 2 + row // 2)
            dbg.settle()
            dbg.click(c["x"] + take[0] + take[2] // 2,
                      c["y"] + take[1] + take[3] // 2)
            dbg.settle()
            time.sleep(0.8)
            fs = [x for x in dbg.windows() if x["title"] == "Screenshot"]
            check("the app goes fullscreen to choose a region",
                  bool(fs) and fs[0]["state"] == "fullscreen",
                  str(fs[0]["state"]) if fs else "no window")

            dbg.drag(300, 200, 700, 500, steps=8)
            dbg.settle()
            time.sleep(1.0)
            back = [x for x in dbg.windows() if x["title"] == "Screenshot"]
            check("...and comes back out of fullscreen on the release",
                  bool(back) and back[0]["state"] == "normal",
                  str(back[0]["state"]) if back else "no window")

            dbg.send("sh sync")
            time.sleep(0.3)
            names = sorted(n for n in dbg.send("sh ls /home/screenshots").split()
                           if n.endswith(".qoi"))
            rgot = out("region.qoi")
            if names and pull(disk, "/home/screenshots/" + names[-1], rgot):
                rim = Image.open(rgot)
                check("the crop is exactly the dragged rectangle",
                      rim.size == (400, 300), f"{rim.size}, wanted 400x300")

    # --- 6. the window picker -----------------------------------------
    #
    # Two windows are needed, or "the outline follows the pointer" cannot
    # be told from "the outline is stuck on the only window there is".
    dbg.open_app("Calculator")
    dbg.settle()
    dbg.open_app("Notepad")
    dbg.settle()
    # RAISED AGAIN, and its geometry RE-READ: the two windows just opened
    # are above it, so a click at its old content coordinates lands on
    # one of them -- which reads exactly like the picker not working.
    # It is single-instance, so opening it raises the one that is up.
    dbg.open_app("Screenshot")
    dbg.settle()
    time.sleep(0.6)
    # BY WINDOW TITLE, which is not the app's name: Notepad titles its
    # window after the document, so it is "untitled" until saved. Keying
    # on "Notepad" skipped this whole section silently.
    wins = {w["title"]: w for w in dbg.windows()}
    second = "untitled"
    app = wins.get("Screenshot")
    if app and take and mode and "Calculator" in wins and second in wins:
        c = app["content"]
        # "Active window" is the SECOND row.
        row = mode[3] // 3
        dbg.click(c["x"] + mode[0] + 40, c["y"] + mode[1] + row + row // 2)
        dbg.settle()
        dbg.click(c["x"] + take[0] + take[2] // 2,
                  c["y"] + take[1] + take[3] // 2)
        dbg.settle()
        time.sleep(0.8)
        fs = [x for x in dbg.windows() if x["title"] == "Screenshot"]
        check("the picker goes fullscreen",
              bool(fs) and fs[0]["state"] == "fullscreen",
              str(fs[0]["state"]) if fs else "no window")

        # THE OUTLINE IS READ OFF THE SCREEN, not out of the app's log.
        #
        # It is the stronger claim: a rect the app computed and did not
        # draw passes a log check and leaves the person with nothing to
        # aim at, which is this repo's "it responds is not it is drawn"
        # trap. The outline is two rings, the inner one UTHEME_ACCENT,
        # and its bounding box IS the rect being offered.
        accent = (70, 110, 160)

        # LONG RUNS, not a bounding box of every accent pixel. The accent
        # is a THEME colour -- the app's own taskbar icon is painted in
        # it -- so a plain bbox is pulled wide by whatever else is on
        # screen. Only the outline puts a hundred of them in one row or
        # column.
        RUN = 100

        def outlined():
            """The outlined window's rect as [x, y, w, h], or None."""
            path = out("pick-probe.png")
            qmp.screenshot(path)
            im = Image.open(path).convert("RGB")
            W, H = im.size
            data = pixels(im)
            rows = [0] * H
            cols = [0] * W
            for i, p in enumerate(data):
                if p == accent:
                    rows[i // W] += 1
                    cols[i % W] += 1
            ys = [y for y in range(H) if rows[y] >= RUN]
            xs = [x for x in range(W) if cols[x] >= RUN]
            if not ys or not xs:
                return None
            # The two accent rings sit at rect-1 and rect (the app's
            # outline()), so the outermost accent is one pixel outside
            # the window on every side.
            return [xs[0] + 1, ys[0] + 1,
                    xs[-1] - xs[0] - 1, ys[-1] - ys[0] - 1]

        # THE OUTLINE MUST MOVE. Point at each window in turn and require
        # the app's reported rect to be that window's -- compared against
        # what `gui windows` says, which the app never saw.
        #
        # TWO THINGS THIS HAS TO GET RIGHT, both of which it got wrong
        # first. The pointer must land where ONLY the intended window is:
        # aiming at a window's centre picks whichever is on top there,
        # and the app outlining the other one is then correct behaviour
        # reported as a bug. And it must WARP THE REAL CURSOR -- an
        # injected `gui move` overrides the pointer for one wm_run()
        # iteration and the next one snaps it back, so a capture taken
        # afterwards photographs the screen with nothing pointed at
        # (CLAUDE.md).
        cal, note = wins["Calculator"], wins[second]
        aim = {
            # Left of where Notepad starts, so only the Calculator is there.
            "Calculator": (cal["x"] + (note["x"] - cal["x"]) // 2,
                           cal["y"] + cal["h"] // 2),
            # Right of where the Calculator ends, so only Notepad is.
            second: (cal["x"] + cal["w"] + (note["x"] + note["w"]
                                            - cal["x"] - cal["w"]) // 2,
                     note["y"] + note["h"] // 2),
        }
        seen = {}
        for name in ("Calculator", second):
            w = wins[name]
            ax, ay = aim[name]
            if not (w["x"] <= ax < w["x"] + w["w"]
                    and w["y"] <= ay < w["y"] + w["h"]):
                check(f"an unambiguous point exists inside {name}", False,
                      f"({ax}, {ay}) is not in {name}")
                continue
            want = [w["x"], w["y"], w["w"], w["h"]]
            # The APP's own report is the confirmation, not the cursor's
            # position: this asserts that what it is pointing AT is the
            # window meant, which is the claim being made.
            dbg.warp_confirmed(qmp, ax, ay, lambda: outlined() == want)
            seen[name] = outlined()
            check(f"pointing at {name} outlines {name}",
                  seen[name] == want,
                  f"app says {seen[name]}, window is {want} "
                  f"(pointed at {ax}, {ay})")
        check("...and the outline MOVED between the two",
              seen.get("Calculator") != seen.get(second),
              "the same rect for both -- it is not following the pointer")

        # Click where the pointer already IS -- the second window, which
        # the loop above left it over.
        target = wins[second]
        dbg.click(*aim[second])
        dbg.settle()
        time.sleep(1.0)
        back = [x for x in dbg.windows() if x["title"] == "Screenshot"]
        check("clicking a window leaves the picker",
              bool(back) and back[0]["state"] == "normal",
              str(back[0]["state"]) if back else "no window")

        dbg.send("sh sync")
        time.sleep(0.3)
        names = sorted(n for n in dbg.send("sh ls /home/screenshots").split()
                       if n.endswith(".qoi"))
        pgot = out("picked.qoi")
        if names and pull(disk, "/home/screenshots/" + names[-1], pgot):
            pim = Image.open(pgot)
            check("the capture is that window's size",
                  pim.size == (target["w"], target["h"]),
                  f"{pim.size}, wanted {target['w']}x{target['h']}")

    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nscreenshot_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
