#!/usr/bin/env python3
"""Live wallpapers: the background client, what it draws, when it stops.

WHAT IS UNDER TEST
------------------
`userland/wm/wm_background.c` spawns the program `desktop.wallpaper_type`
and `desktop.wallpaper_live` name, maps its frames, and draws them where
the picture would go (desktop.c); it withholds the client's timer while
the desktop is hidden. `gui background` reports its state as JSON.

THE CHECKS, and what a broken version would still pass:
  1. a picture background runs NO client -- the control for 2.
  2. Live starts the named program and its frames reach the screen: the
     sample box MOVES between two raw captures, while the taskbar strip
     (the control) does not.
  3. ...and what is shown is not the picture: a live client that never
     presented would leave check 2's box static, but one presenting a
     blank frame would not, so the picture is compared too.
  4. a screensaver PAUSES it -- the frame counter stops -- and it resumes
     after. The resume half is the positive control: a counter that never
     moved would pass "stopped".
  5. a crashed client falls back to the picture and is restarted.
  6. an animated GIF picture runs the player; a still one does not.
  7. Plain colour paints the chosen colour, read back from pixels.
  8. the same program started by someone else is an ORDINARY window, and
     takes nothing from the background (the role is the spawned pid's).

Every setting it touches is restored. Usage (the VM must be up):

    python3 tools/vm.py start
    python3 tools/live_wallpaper_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

_res = Results()
check = _res.check

SETTINGS = ("desktop.wallpaper_type", "desktop.wallpaper_live", "desktop.wallpaper",
            "desktop.background_colour", "desktop.screensaver_idle")
PLUM = (0x4a, 0x2d, 0x4f)   # lib/ulivewall.c's PLAIN table


def setting(dbg, key):
    out = dbg.send(f"sh config get {key}") or ""
    for line in out.splitlines():
        line = line.strip()
        if line and "=" not in line and ":" not in line and " " not in line:
            return line
    return ""


def set_setting(dbg, key, value):
    dbg.send(f"sh config set {key} {value}")
    time.sleep(0.8)


def bg(dbg):
    return dbg.json("gui background") or {}


def wait_bg(dbg, pred, timeout=20.0):
    """`gui background` once `pred` holds, or the last answer."""
    deadline = time.time() + timeout
    st = bg(dbg)
    while not pred(st) and time.time() < deadline:
        time.sleep(0.3)
        st = bg(dbg)
    return st


def boxes(dbg):
    s = dbg.state()["screen"]
    w, h = s["w"], s["h"]
    # Right of the icon column, above the taskbar, where no window is.
    sample = (w * 55 // 100, h // 10, w * 95 // 100, h * 6 // 10)
    # The taskbar's empty middle: no clock, no Start button.
    strip = (w // 3, h - 20, w * 2 // 3, h - 10)
    return sample, strip


def raw(qmp, shot, name, box):
    """One capture, NOT settled: the thing under test never settles."""
    from PIL import Image
    path = shot(name)
    qmp.screenshot(path, stable=False, settle=0.05)
    return Image.open(path).convert("RGB").crop(box).tobytes()


def run(dbg, qmp, shot):
    sample, strip = boxes(dbg)
    set_setting(dbg, "desktop.wallpaper_type", "picture")
    set_setting(dbg, "desktop.wallpaper", "aurora")
    st = wait_bg(dbg, lambda s: s.get("pid") == 0)
    check("a picture background runs no client", st.get("pid") == 0, f"{st}")
    picture = qmp.stable_pixels(shot("lw_picture.png"), box=sample)

    # --- 2/3. live: started, drawn, moving -----------------------------
    set_setting(dbg, "desktop.wallpaper_live", "aurora")
    set_setting(dbg, "desktop.wallpaper_type", "live")
    st = wait_bg(dbg, lambda s: s.get("shown") == 1)
    check("Live starts /bin/wm/wallpapers/aurora and shows its frames",
          st.get("shown") == 1 and st.get("program") == "/bin/wm/wallpapers/aurora", f"{st}")
    a, ta = raw(qmp, shot, "lw_live_a.png", sample), raw(qmp, shot, "lw_live_a.png", strip)
    time.sleep(2.0)
    b, tb = raw(qmp, shot, "lw_live_b.png", sample), raw(qmp, shot, "lw_live_b.png", strip)
    check("the background moves between two captures", a != b)
    check("...and the taskbar strip does not (the control)", ta == tb)
    check("...and it is not the picture", a != picture and b != picture)

    # --- 4. paused under a screensaver, resumed after -----------------
    set_setting(dbg, "desktop.screensaver_idle", "5")
    f0 = bg(dbg).get("frames", 0)
    dbg.send("gui idle start")
    st = wait_bg(dbg, lambda s: s.get("paused") == 1, timeout=8)
    f1 = st.get("frames", 0)
    time.sleep(2.0)
    f2 = bg(dbg).get("frames", 0)
    check("a screensaver pauses it: no frames while it runs",
          st.get("paused") == 1 and f2 == f1, f"paused={st.get('paused')} frames {f0}->{f1}->{f2}")
    dbg.send("gui idle stop")
    st = wait_bg(dbg, lambda s: s.get("paused") == 0, timeout=8)
    time.sleep(2.0)
    f3 = bg(dbg).get("frames", 0)
    check("...and it resumes after (the counter moves again)",
          st.get("paused") == 0 and f3 > f2, f"frames {f2}->{f3}")

    # --- 5. a crash falls back to the picture, then restarts ----------
    pid = bg(dbg).get("pid", 0)
    dbg.send(f"sh kill {pid}")
    st = wait_bg(dbg, lambda s: s.get("pid") != pid and s.get("shown") == 0, timeout=6)
    shown_picture = qmp.stable_pixels(shot("lw_crashed.png"), box=sample)
    check("a killed client leaves the picture showing",
          st.get("shown") == 0 and shown_picture == picture, f"{st}")
    st = wait_bg(dbg, lambda s: s.get("shown") == 1 and s.get("pid") not in (0, pid))
    check("...and is restarted", st.get("shown") == 1 and st.get("pid") not in (0, pid), f"{st}")

    # --- 8. the program started by hand is an ordinary window ---------
    pid = bg(dbg).get("pid", 0)
    win = dbg.spawn("/bin/wm/wallpapers/drift", title="Live wallpaper")
    st = bg(dbg)
    check("the same program run by hand opens a window and leaves the background alone",
          win is not None and st.get("pid") == pid, f"window={bool(win)} {st}")
    for p in dbg.processes_named("drift"):
        dbg.send(f"sh kill {p['pid']}")

    # --- 6. an animated picture runs the player -------------------------
    set_setting(dbg, "desktop.wallpaper_type", "picture")
    set_setting(dbg, "desktop.wallpaper", "rain")
    st = wait_bg(dbg, lambda s: s.get("shown") == 1)
    check("an animated GIF picture plays through the player",
          st.get("program") == "/bin/wm/wallpapers/players/gif" and st.get("shown") == 1, f"{st}")
    set_setting(dbg, "desktop.wallpaper", "aurora")
    st = wait_bg(dbg, lambda s: s.get("pid") == 0)
    check("...and a still picture stops it", st.get("pid") == 0, f"{st}")

    # --- 7. plain colour ----------------------------------------------
    # THE TYPE FIRST: a colour written while the type is not colour is
    # refused (its Requires= is unmet), as Settings' Apply order avoids.
    set_setting(dbg, "desktop.wallpaper_type", "colour")
    set_setting(dbg, "desktop.background_colour", "plum")
    px = qmp.stable_pixels(shot("lw_colour.png"), box=sample)
    want = bytes(PLUM)
    check("Plain colour paints the chosen colour",
          px[:3] == want and px[-3:] == want, f"first {tuple(px[:3])} last {tuple(px[-3:])}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "live_wallpaper_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.abspath(os.path.join(outdir, n))  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("live wallpapers (the background client)")
    orig = {k: setting(dbg, k) for k in SETTINGS}
    try:
        run(dbg, qmp, shot)
    finally:
        # RESTORED WHATEVER HAPPENED: every one persists to /etc.
        dbg.send("gui idle stop")
        # Each value while its own type is selected, then the type.
        for t, k in (("colour", "desktop.background_colour"), ("live", "desktop.wallpaper_live"),
                     ("picture", "desktop.wallpaper")):
            if orig.get(k):
                set_setting(dbg, "desktop.wallpaper_type", t)
                set_setting(dbg, k, orig[k])
        for k in ("desktop.screensaver_idle", "desktop.wallpaper_type"):
            if orig.get(k):
                set_setting(dbg, k, orig[k])
    passed = sum(1 for _, ok, _ in _res.rows if ok)
    print(f"\nlive_wallpaper_test: {passed} passed, {len(_res.rows) - passed} failed")
    return 0 if passed == len(_res.rows) else 1


if __name__ == "__main__":
    sys.exit(main())
