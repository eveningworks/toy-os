"""tools/fullscreen_test.py -- the fullscreen state and the display lease.

Boots with a virtio GPU (the one QEMU display with a cursor plane, which
the lease policy requires), spawns /tests/fsclient -- a write-only
client that asks for fullscreen at once and paints a colour nothing
else on the desktop uses -- and asserts, in order:

  * the window's reported state is "fullscreen", its rect and content
    rect are the whole screen, and the taskbar's pixels are the client's
    colour (no chrome, no taskbar under a fullscreen window);
  * `gui fb --json` names the client as the LESSEE, and frames keep
    flowing while it holds the lease (two captures differ in the
    client's counter bar) -- the compositor is not presenting, so those
    frames can only be the client's own flips;
  * the Start menu (an overlay) takes the lease away and returns it;
  * F11 leaves fullscreen: state normal, lease 0, taskbar back;
  * Alt+F4 with the lease held ends it and the desktop is repainted.

What a broken version would still pass, and the check that stops it: a
"fullscreen" that kept the chrome would fail the taskbar pixel; a lease
that never flipped would fail the two-capture difference; a compositor
that kept presenting under a lease would overwrite the client's colour
with its own frame.

    python3 tools/fullscreen_test.py [--instance N] [--keep]
"""
import argparse
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from gui_debug import DebugConsole            # noqa: E402
from qmp_test import QMPSession               # noqa: E402

TITLE = "Fullscreen Client"
FILL = (0x30, 0x60, 0xC0)   # fsclient.c's FILL, as RGB
KEY_F11 = 0xB1
KEY_F4 = 0xA5
KEY_SUPER = 0xA6


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
        return ok


def near(p, q, tol=12):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


def grab(qmp, tmp, name):
    from PIL import Image
    path = f"{tmp}/{name}.png"
    qmp.screenshot(path, stable=False)   # the client animates on purpose
    return Image.open(path).convert("RGB")


def wait_state(dbg, state, timeout=10.0):
    deadline = time.time() + timeout
    win = None
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win and win.get("state") == state:
            return win
        time.sleep(0.25)
    return win


def wait_lease(dbg, pid, timeout=6.0):
    deadline = time.time() + timeout
    fb = {}
    while time.time() < deadline:
        fb = dbg.json("gui fb --json")
        if fb.get("lease") == pid:
            return fb
        time.sleep(0.25)
    return fb


def wait_pixel(qmp, tmp, name, x, y, want, timeout=6.0):
    """The client's first frame lands after its resize is adopted, and
    under a loaded host that is not instant -- so a capture is retried
    until the pixel matches (or the budget is spent)."""
    deadline = time.time() + timeout
    p = None
    while time.time() < deadline:
        p = grab(qmp, tmp, name).getpixel((x, y))
        if near(p, want):
            return p
        time.sleep(0.3)
    return p


def run(dbg, qmp, tmp, res):
    st = dbg.state()
    sw, sh = st["screen"]["w"], st["screen"]["h"]
    # The plane is claimed by the compositor's first cursor sync, which
    # under a loaded host can land after the desktop reports ready.
    deadline = time.time() + 10
    hwc = bool(st.get("hwcursor"))
    while not hwc and time.time() < deadline:
        time.sleep(0.4)
        hwc = bool(dbg.state().get("hwcursor"))
    res.check("the hardware cursor plane is active (the lease's precondition)", hwc)

    win = dbg.spawn("/tests/fsclient", TITLE)
    res.check("fsclient opened a window", win is not None)
    if not win:
        return
    pid = win["client_pid"]

    win = wait_state(dbg, "fullscreen")
    res.check("the window reports the fullscreen state", win and win.get("state") == "fullscreen",
              win and win.get("state"))
    # The proposal is adopted with the client's next present.
    deadline = time.time() + 8
    while time.time() < deadline:
        win = dbg.window(TITLE)
        if win and win["w"] == sw and win["h"] == sh:
            break
        time.sleep(0.25)
    res.check("its rect is the whole screen", win and (win["x"], win["y"], win["w"], win["h"]) == (0, 0, sw, sh),
              win and (win["x"], win["y"], win["w"], win["h"]))
    c = win and win["content"]
    res.check("...and so is its content rect (no chrome)",
              c and (c["x"], c["y"], c["w"], c["h"]) == (0, 0, sw, sh), c)

    if hwc:
        fb = wait_lease(dbg, pid)
        res.check("the compositor leased the display to the client", fb.get("lease") == pid, fb)
    else:
        fb = dbg.json("gui fb --json")
        res.check("no lease without a cursor plane (composed instead)", fb.get("lease") == 0, fb)

    tb = wait_pixel(qmp, tmp, "fs-1", sw // 2, sh - 8, FILL)
    res.check("the taskbar's pixels are the client's colour", near(tb, FILL), tb)
    mid = wait_pixel(qmp, tmp, "fs-1b", sw // 2, sh // 2, FILL)
    res.check("...and so is the middle of the screen", near(mid, FILL), mid)
    im = grab(qmp, tmp, "fs-1c")

    # Frames flow: the counter bar is longer in a later capture.
    def bar_len(img):
        n = 0
        for x in range(8, min(sw - 8, 8 + 220)):
            if near(img.getpixel((x, 11)), (255, 255, 255)):
                n += 1
        return n
    l1 = bar_len(im)
    time.sleep(0.5)
    im2 = grab(qmp, tmp, "fs-2")
    l2 = bar_len(im2)
    res.check("frames keep arriving on screen (the counter bar moved)", l1 != l2, f"{l1} -> {l2}")

    if hwc:
        # An overlay above the client takes the lease away (a right
        # click is the CLIENT's inside its content, so the Start menu is
        # the overlay to open)...
        dbg.key(KEY_SUPER)
        time.sleep(0.4)
        fb = dbg.json("gui fb --json")
        res.check("the Start menu over it ends the lease", fb.get("lease") == 0, fb)
        # ...and closing it hands the lease back.
        dbg.key(KEY_SUPER)
        fb = wait_lease(dbg, pid)
        res.check("closing the menu leases again", fb.get("lease") == pid, fb)

    dbg.key(KEY_F11)
    win = wait_state(dbg, "normal")
    res.check("F11 leaves fullscreen", win and win.get("state") == "normal", win and win.get("state"))
    # THE CRASH THIS TOOL EXISTS FOR: the client draws continuously, so
    # the moment the lease ends it is mid-frame into the leased buffer.
    # Unmapping it then killed DOOM with a page fault at WIN_FB_VADDR;
    # the pages must stay until the client's own next present.
    time.sleep(1.0)
    alive = dbg.window(TITLE) is not None
    # Repeated, because the hazard is a race between the client's
    # frame and the lease's end: one toggle can be lucky.
    for _ in range(4):
        if not alive:
            break
        dbg.key(KEY_F11)
        wait_state(dbg, "fullscreen")
        if hwc:
            wait_lease(dbg, pid)
        time.sleep(0.4)
        dbg.key(KEY_F11)
        wait_state(dbg, "normal")
        time.sleep(0.6)
        alive = dbg.window(TITLE) is not None
    res.check("...and a continuously drawing client SURVIVES it, five times (no fault at WIN_FB_VADDR)",
              alive, [l for l in dbg.send("sh dmesg").splitlines() if "CRASH" in l][-1:])
    fb = dbg.json("gui fb --json")
    res.check("...and the lease is gone", fb.get("lease") == 0, fb)
    time.sleep(0.8)
    im3 = grab(qmp, tmp, "fs-3")
    tb = im3.getpixel((sw // 2, sh - 8))
    res.check("the taskbar is painted again", not near(tb, FILL), tb)

    dbg.key(KEY_F11)
    win = wait_state(dbg, "fullscreen")
    res.check("F11 enters fullscreen again", win and win.get("state") == "fullscreen")
    if hwc:
        fb = wait_lease(dbg, pid)
        res.check("...with the lease", fb.get("lease") == pid, fb)

    dbg.key(KEY_F4, mods="alt")
    deadline = time.time() + 6
    while time.time() < deadline and dbg.window(TITLE):
        time.sleep(0.25)
    res.check("Alt+F4 closes the fullscreen client", dbg.window(TITLE) is None)
    fb = dbg.json("gui fb --json")
    res.check("the lease ended with it", fb.get("lease") == 0, fb)
    time.sleep(0.8)
    im4 = grab(qmp, tmp, "fs-4")
    tb = im4.getpixel((sw // 2, sh - 8))
    res.check("the desktop is repainted", not near(tb, FILL), tb)


def wait_for_desktop(n, timeout_s):
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
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    n = args.instance
    launch = ["python3", "tools/vm.py", "--vga", "virtio", "--instance", str(n), "start"]
    if subprocess.run(launch).returncode != 0:
        print("fullscreen_test: could not start the guest")
        return 1
    dbg = wait_for_desktop(n, 60)
    if dbg is None:
        print("fullscreen_test: the guest never reached a desktop")
        return 1
    res = Result()
    try:
        qmp = QMPSession(port=4445 + n)
        try:
            run(dbg, qmp, args.tmp, res)
        finally:
            qmp.close()
    finally:
        dbg.close()
        if not args.keep:
            subprocess.run(["python3", "tools/vm.py", "--instance", str(n), "stop"],
                           stdout=subprocess.DEVNULL)
    print(f"fullscreen_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
