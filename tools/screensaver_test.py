#!/usr/bin/env python3
"""The screensaver: the idle clock, the two settings, and the savers.

WHAT THIS ASSERTS, and why each is shaped the way it is:

  - A SAVER IS A PROCESS. `gui idle` reports its pid, so "is it up?" is
    a fact the compositor answers rather than a guess from pixels -- and
    the window it owns is a real entry in `gui windows`, fullscreen.
  - INPUT KILLS IT. A key, and separately a pointer move, must end it.
    Both are checked because they arrive on different paths in the frame
    loop, and a clock reset wired to only one of them would look fine
    from whichever the test happened to use.
  - OFF MEANS OFF. With the timeout at zero the saver must not start,
    even through the test lever -- which is why that lever refuses
    rather than skipping the setting along with the wait.
  - THE BLANK SAVER IS ACTUALLY BLACK, read as PIXELS. "A window opened"
    is not "the screen is dark", which is exactly the distinction
    CLAUDE.md's "it responds is not it is drawn" rule exists for.

THE TEST LEVER, stated plainly: the shortest timeout a person can
configure is ONE MINUTE, so a test that waited for the real clock would
cost a minute per check. `gui idle start` skips the WAIT and nothing
else -- the clock it skips is still what `gui idle` reports, and the
settings are still read the same way.

IT PUTS BOTH SETTINGS BACK, including on a failed check: a test that
leaves a screensaver configured changes the machine for every later
tool, and this one turns it off and on again.
"""
import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import port_guard                                      # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
from gui_debug import DebugConsole, enter_gui          # noqa: E402

SAVER_DIR = "/bin/wm/savers"
checks = []


def check(name, ok, detail=""):
    # THE DETAIL ONLY ON A FAILURE. A passing line that carries "no saver
    # window" reads as a contradiction, and this tool printed one.
    checks.append((name, bool(ok)))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if not ok and detail else ""))
    return bool(ok)


def idle(dbg):
    """The compositor's own report: seconds quiet, the saver's pid, the
    configured name and the timeout."""
    for _ in range(10):
        out = dbg.send("gui idle") or ""
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("{") and "seconds" in line:
                try:
                    return json.loads(line)
                except ValueError:
                    pass
        time.sleep(0.2)
    return None


def wait_pid(dbg, want_running, timeout=6.0):
    """Poll until the saver is running (or not). Waiting on the report
    rather than sleeping: a spawn is a process, and how long it takes to
    get far enough to own a window varies with load."""
    deadline = time.time() + timeout
    while True:
        st = idle(dbg)
        if st and bool(st["pid"]) == want_running:
            return st
        if time.time() >= deadline:
            return st
        time.sleep(0.25)


def saver_window(dbg):
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] in ("Starfield", "Blank"):
            return w
    return None


def wait_window(dbg, full=True, timeout=8.0):
    """Poll until the saver's WINDOW exists and, by default, has reached
    the screen's size.

    A pid is not a window, and a window is not yet a FULLSCREEN window.
    The process is spawned, asks for a surface at the size its descriptor
    named, asks to go fullscreen, and is resized a frame or more later.
    Stopping at any earlier step and then asserting about the next one is
    the flake CLAUDE.md calls a poll whose exit condition is weaker than
    what follows it -- and it fails as "the saver is not fullscreen",
    which reads like a bug in the saver.
    """
    scr = dbg.json("gui state --json").get("screen", {})
    deadline = time.time() + timeout
    w = None
    while True:
        w = saver_window(dbg)
        if w and (not full or (w["w"] >= scr.get("w", 1) and w["h"] >= scr.get("h", 1))):
            return w
        if time.time() >= deadline:
            return w
        time.sleep(0.25)


def set_setting(dbg, key, value):
    dbg.send(f"gui spawn /bin/config set {key} {value}")
    time.sleep(0.8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "screensaver_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("screensaver (the idle clock, in the compositor)")

    st = idle(dbg)
    if not check("the compositor reports its idle clock", st is not None, f"{st}"):
        return report()
    check("...with a saver configured and a timeout in minutes",
          st["saver"] and st["minutes"] > 0, f"{st}")
    check("nothing is running while the machine is being driven",
          st["pid"] == 0, f"pid={st['pid']}")

    # --- it starts, and it owns a real fullscreen window ---------------
    dbg.send("gui idle start")
    st = wait_pid(dbg, True)
    started = check("the saver starts as its own process",
                    st is not None and st["pid"] > 0, f"{st}")
    if started:
        win = wait_window(dbg)
        check("...and owns a window", win is not None,
              f"windows={[w['title'] for w in dbg.json('gui windows --json')['windows']]}")
        if win:
            scr = dbg.json("gui state --json").get("screen", {})
            check("...which is FULLSCREEN, not a framed window",
                  win["w"] >= scr.get("w", 0) and win["h"] >= scr.get("h", 0),
                  f"{win['w']}x{win['h']} vs screen {scr.get('w')}x{scr.get('h')}")
            check("...and the window is the configured saver",
                  win["title"].lower().startswith(st["saver"][:4]),
                  f"title={win['title']!r} saver={st['saver']!r}")

    # --- a KEY ends it --------------------------------------------------
    dbg.key(ord("a"))
    st = wait_pid(dbg, False)
    check("a keypress ends it", st is not None and st["pid"] == 0, f"{st}")
    check("...and the idle clock restarted", st is not None and st["seconds"] <= 3,
          f"seconds={st and st['seconds']}")

    # --- a POINTER MOVE ends it too, which is a different code path -----
    dbg.send("gui idle start")
    st = wait_pid(dbg, True)
    if check("it starts again", st is not None and st["pid"] > 0, f"{st}"):
        cur = dbg.cursor()
        dbg.warp_cursor(qmp, cur[0] + 60, cur[1] + 40)
        st = wait_pid(dbg, False)
        check("moving the pointer ends it", st is not None and st["pid"] == 0, f"{st}")

    # --- the blank saver is actually black ------------------------------
    set_setting(dbg, "desktop.screensaver", "blank")
    dbg.send("gui idle start")
    st = wait_pid(dbg, True)
    if check("the other saver starts", st is not None and st["pid"] > 0, f"{st}"):
        check("...and it is the one that was selected",
              st["saver"] == "blank", f"saver={st and st['saver']}")
        check("...and its window is up before the screen is read",
              wait_window(dbg) is not None, "no saver window")
        path = f"{args.logs}/screensaver_blank.png"
        from PIL import Image
        # A SETTLED frame: the client having painted is not the
        # compositor having composited it, and that hop varies with load.
        qmp.screenshot(path)
        im = Image.open(path).convert("RGB")
        w, h = im.size
        pts = [(w // 4, h // 4), (w // 2, h // 2), (3 * w // 4, 3 * h // 4)]
        vals = [im.getpixel(p) for p in pts]
        # PIXELS, not "a window opened". A saver that drew nothing at all
        # would leave the desktop on screen and pass every check above.
        check("the blank saver actually blanks the screen",
              all(sum(v) < 40 for v in vals), f"{vals}")
        dbg.key(ord("a"))
        wait_pid(dbg, False)

    # --- off means off --------------------------------------------------
    set_setting(dbg, "desktop.screensaver_idle", "0")
    st = idle(dbg)
    check("a zero timeout is what turns it off",
          st is not None and st["minutes"] == 0, f"{st}")
    dbg.send("gui idle start")
    time.sleep(1.0)
    st = idle(dbg)
    check("...and nothing starts while it is off",
          st is not None and st["pid"] == 0, f"{st}")

    set_setting(dbg, "desktop.screensaver_idle", "10")
    set_setting(dbg, "desktop.screensaver", "starfield")
    dbg.close()
    return report()


def report():
    passed = sum(1 for _, ok in checks if ok)
    failed = len(checks) - passed
    print(f"\nscreensaver_test: {passed} passed, {failed} failed")
    for name, ok in checks:
        if not ok:
            print(f"  FAILED: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
