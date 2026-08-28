#!/usr/bin/env python3
"""tools/virtio_gpu_test.py -- the virtio GPU, end to end.

WHY THIS TOOL EXISTS AT ALL
---------------------------
Nothing else here boots with a virtio GPU. Every other GUI tool launches
`-vga std`, and `make test` does too, so the driver's own KTESTs would
skip on every run -- a file full of tests that never executes is worse
than no tests, because the suite reports green either way.

So this tool supplies the hardware and then checks BOTH halves:

  * inside the guest -- `ktest virtio-gpu`, which sees the driver's own
    state (is it the active display, is the surface the one it
    allocated, is a flush exactly two commands);
  * outside it -- the actual pixels, which is the only thing that can
    tell "the driver reported success" from "the frame reached the
    screen". On a device where nothing scans guest memory, those are
    genuinely different claims.

THE CONTROL, and it is the assertion to keep: the taskbar clock must
CHANGE between two captures. A driver whose flush silently stopped
working would still pass "the desktop is on screen" -- the first frame
got there before anything broke, and a still image cannot tell a live
display from a frozen one. The clock moving is what proves transfers
are still reaching the host.

The cursor plane is checked through `hwcursor demo`, a shell command
that exists precisely because the ring-3 compositor draws its own
software sprite and therefore never touches the plane (see
apps/shell_sys.c's cmd_hwcursor). The pointer moving to the device is a
roadmap item; this proves the queue works today.

USAGE
    python3 tools/virtio_gpu_test.py [--instance N] [--keep]
"""

import argparse
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole            # noqa: E402
from qmp_test import QMPSession               # noqa: E402

# For "the desktop is drawn": how much of the screen must be NON-BLACK.
# It used to be how much a single colour had to cover, which stopped
# being true of a working desktop the day wallpapers landed -- a
# photograph has no colour covering half the screen (`aurora`'s
# dominant one reaches 28%). Non-black measures the property the
# positive control actually breaks and does not care what is drawn.
DESKTOP_MIN_NONBLACK = 0.5
CURSOR_X, CURSOR_Y = 400, 300


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
        return ok


def dominant(im):
    from collections import Counter
    return Counter(im.getdata()).most_common(1)[0]


def run(dbg, qmp, tmp, res):
    # --- the driver's own account of itself ---------------------------
    #
    # `lsdev`, not `dmesg`: the kernel log is a ring buffer, and by the
    # time a desktop has been running for a few seconds the boot lines
    # this used to read have rolled out of it. A test whose oracle can
    # expire is a test that starts failing for reasons unrelated to the
    # code -- so lsdev names the active display driver now.
    dev = dbg.send("lsdev")
    res.check('the active display is "virtio-gpu"',
              "Display: virtio-gpu" in dev,
              [l for l in dev.splitlines() if l.startswith("Display:")][:1])
    res.check("it declares flush and a cursor plane",
              "flush" in dev.split("PCI devices")[0] and
              "cursor" in dev.split("PCI devices")[0])
    res.check("the GPU is on the bus as a VGA-compatible controller",
              "0x1af4:0x1050" in dev)

    # --- the kernel's own tests, on hardware that finally exists ------
    kt = dbg.send("ktest virtio-gpu")
    res.check("the in-kernel virtio-gpu tests pass", "PASSED" in kt,
              kt.splitlines()[-1] if kt else "no output")
    res.check("...and they did not all skip",
              "skipped" not in kt.split("PASSED")[-1] or ", 0 skipped" in kt,
              kt.splitlines()[-1] if kt else "")

    # --- the pixels ---------------------------------------------------
    from PIL import Image
    first = f"{tmp}/vgpu-1.png"
    qmp.screenshot(first)
    im = Image.open(first).convert("RGB")
    colour, count = dominant(im)
    total = im.size[0] * im.size[1]
    nonblack = sum(1 for p in im.getdata() if p != (0, 0, 0))
    # TWO CLAUSES, AND NEITHER IS DECORATION. The positive control that
    # removes DISPLAY_CAP_NEEDS_FLUSH leaves a screen that is 99.9%
    # black, which the first clause rejects; a display stuck on one
    # flat non-black frame is what the second one rejects. Whether that
    # frame is a wallpaper, a plain background or a window is the next
    # check's business, not this one's.
    res.check("the desktop is actually on screen",
              nonblack > total * DESKTOP_MIN_NONBLACK and count < total,
              f"{100.0 * nonblack / total:.1f}% non-black, "
              f"dominant {colour} covers {100.0 * count / total:.1f}%")



    # THE CONTROL. Without this, everything above passes on a display
    # that received exactly one frame and then froze.
    time.sleep(2.5)
    second = f"{tmp}/vgpu-2.png"
    qmp.screenshot(second)
    im2 = Image.open(second).convert("RGB")
    differing = sum(1 for a, b in zip(im.getdata(), im2.getdata()) if a != b)
    res.check("the screen KEEPS updating (the taskbar clock moves)",
              differing > 0, f"{differing} pixels changed in 2.5s")

    # --- the cursor plane ---------------------------------------------
    #
    # A HARDWARE CURSOR IS INVISIBLE TO screendump, AND THAT IS NOT A
    # BUG IN THIS TOOL. QEMU hands a device-composited cursor to the
    # display client out of band (the VNC cursor pseudo-encoding, an
    # SDL cursor), exactly as a real GPU hands it to the scanout
    # hardware -- it is never drawn into the surface `screendump`
    # captures. So there is no pixel to read, and a test asserting one
    # would be asserting the cursor had FAILED to be a cursor.
    #
    # What can be checked is: the commands complete (the in-kernel test
    # above, which counts control-queue round trips), and showing the
    # pointer costs NO framebuffer damage. The second is the property
    # the plane exists for -- a software sprite would repaint pixels
    # here, and a repaint is exactly what this rules out.
    quiet_a = f"{tmp}/vgpu-quiet-a.png"
    qmp.screenshot(quiet_a)
    box_before = Image.open(quiet_a).convert("RGB").crop(
        (CURSOR_X - 8, CURSOR_Y - 8, CURSOR_X + 48, CURSOR_Y + 48)).tobytes()

    out = dbg.send(f"sh hwcursor demo {CURSOR_X} {CURSOR_Y}")
    res.check("the adapter accepted a cursor image", "magenta square at" in out,
              out.strip().splitlines()[-1] if out.strip() else "no output")
    time.sleep(1.0)

    shown = f"{tmp}/vgpu-cursor.png"
    qmp.screenshot(shown)
    box_after = Image.open(shown).convert("RGB").crop(
        (CURSOR_X - 8, CURSOR_Y - 8, CURSOR_X + 48, CURSOR_Y + 48)).tobytes()
    res.check("showing it repaints NO framebuffer pixels (it is a plane, not a sprite)",
              box_after == box_before)

    out = dbg.send("sh hwcursor off")
    res.check("hiding it is accepted too", "hidden" in out,
              out.strip().splitlines()[-1] if out.strip() else "no output")

    # --- the COMPOSITOR rides the plane now (WIN_REQ_FB_CURSOR) -------
    #
    # The ring-3 WM defines its sprite over the plane and draws no
    # software cursor at all, and the kernel moves the plane from
    # win_input.c on every pointer event. The oracle is the same one as
    # above, applied to the real pointer: two frames around a pure
    # pointer MOVE must differ in NOTHING but the taskbar clock --
    # a software sprite would repaint both the old and new positions.
    # (`hwcursor demo` above stole the plane's sprite; the WM takes it
    # back on its next shape change, forced here by re-entering a
    # region -- simplest is one more warp, which re-syncs on draw.)
    st = dbg.json("gui state --json")
    res.check("the compositor reports the hardware cursor in use",
              st.get("hwcursor") is True, f"hwcursor={st.get('hwcursor')}")

    dbg.warp_cursor(qmp, 400, 300)
    time.sleep(0.6)
    mv_a = f"{tmp}/vgpu-move-a.png"
    qmp.stable_pixels(mv_a)
    dbg.warp_cursor(qmp, 700, 420)
    time.sleep(0.6)
    mv_b = f"{tmp}/vgpu-move-b.png"
    qmp.stable_pixels(mv_b)
    ia = Image.open(mv_a).convert("RGB")
    ib = Image.open(mv_b).convert("RGB")
    w, h = ia.size
    # Crop the taskbar off: its clock legitimately ticks between frames.
    ca = ia.crop((0, 0, w, h - 24)).tobytes()
    cb = ib.crop((0, 0, w, h - 24)).tobytes()
    res.check("a pointer move repaints NOTHING (the plane carries the cursor)",
              ca == cb,
              "framebuffer changed on pure motion -- software sprite still drawing?")


def compare_against_vga_std(n, tmp, res, reference_png):
    """Boot the SAME image on `-vga std` and require the same pixels.

    THIS IS THE PIXEL-FORMAT ORACLE, and it took a positive control to
    arrive at. Checking "is anything on screen" passes on a wrong format
    (a swapped desktop is still a desktop) and even on a black one;
    checking a channel order passes on a format that ROTATES channels
    rather than swapping two. What cannot be fooled is the same OS,
    drawing the same desktop, on an adapter whose framebuffer layout is
    already known good -- any disagreement about what a pixel word means
    shows up as a whole screen of differences.

    The tolerance covers the taskbar clock and nothing else: two boots
    seconds apart differ by ~124 pixels out of 921600 (0.013%).
    """
    from PIL import Image
    subprocess.run(["python3", "tools/vm.py", "--instance", str(n), "stop"],
                   stdout=subprocess.DEVNULL)
    if subprocess.run(["python3", "tools/vm.py", "--vga", "std",
                       "--instance", str(n), "start"]).returncode != 0:
        res.check("a -vga std reference boot came up", False)
        return
    try:
        dbg = wait_for_desktop(n, 60)
        if dbg is None:
            res.check("a -vga std reference boot came up", False)
            return
        # The other half of the plane check above: on an adapter with no
        # plane the compositor must be back on the software sprite.
        st = dbg.json("gui state --json")
        res.check("on -vga std the compositor is on the software sprite",
                  st.get("hwcursor") is False, f"hwcursor={st.get('hwcursor')}")
        dbg.close()
        ref = f"{tmp}/vgpu-std.png"
        qmp = QMPSession(port=4445 + n)
        try:
            qmp.screenshot(ref)
        finally:
            qmp.close()
        a = Image.open(reference_png).convert("RGB")
        b = Image.open(ref).convert("RGB")
        if a.size != b.size:
            res.check("virtio-gpu draws the same desktop as -vga std", False,
                      f"{a.size} vs {b.size}")
            return
        differ = sum(1 for p, q in zip(a.getdata(), b.getdata()) if p != q)
        total = a.size[0] * a.size[1]
        res.check("virtio-gpu draws the same desktop as -vga std "
                  "(so the pixel format is right)",
                  differ < total * 0.005,
                  f"{differ}/{total} pixels differ ({100.0 * differ / total:.3f}%)")
    finally:
        subprocess.run(["python3", "tools/vm.py", "--instance", str(n), "stop"],
                       stdout=subprocess.DEVNULL)


def wait_for_desktop(n, timeout_s):
    """Connect to the debug console and wait until the WM is answering.

    Polled rather than slept: on a NEEDS_FLUSH device the first frame is
    two virtqueue round trips per damage rect, so time-to-desktop is
    genuinely longer than on a scanned framebuffer and a fixed sleep
    would be either flaky or wasteful.
    """
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"
    deadline = time.time() + timeout_s
    dbg = None
    while time.time() < deadline and dbg is None:
        try:
            dbg = DebugConsole(sock)
        except OSError:
            time.sleep(0.5)
    if dbg is None:
        return None
    while time.time() < deadline and "windows" not in dbg.send("gui state"):
        time.sleep(0.5)
    return dbg


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--keep", action="store_true",
                    help="leave the guest running (for poking at it by hand)")
    args = ap.parse_args()

    n = args.instance
    port = 4445 + n

    # vm.py rather than a hand-rolled qemu line: it carries the stale-ISO
    # and QMP-port guards, and `--vga virtio` is the whole reason this
    # tool exists (see CLAUDE.md on there being ONE run path).
    launch = ["python3", "tools/vm.py", "--vga", "virtio",
              "--instance", str(n), "start"]
    if subprocess.run(launch).returncode != 0:
        print("virtio_gpu_test: could not start the guest")
        return 1

    dbg = wait_for_desktop(n, 60)
    if dbg is None:
        print("virtio_gpu_test: the guest never reached a desktop")
        return 1

    res = Result()
    try:
        qmp = QMPSession(port=port)
        try:
            run(dbg, qmp, args.tmp, res)
        finally:
            qmp.close()
    finally:
        dbg.close()

    if not args.keep:
        compare_against_vga_std(n, args.tmp, res, f"{args.tmp}/vgpu-1.png")

    print(f"\nvirtio_gpu_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
