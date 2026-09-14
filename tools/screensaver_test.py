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
    """The running saver's window, found by the PID the compositor
    reports -- not by title.

    Matching a title meant a list of them in here, which went stale the
    moment a saver was added: four of six were reported as never
    reaching fullscreen when the truth was that this could not see their
    windows at all. The pid is exact and cannot drift."""
    st = idle(dbg)
    if not st or not st["pid"]:
        return None
    for w in dbg.json("gui windows --json")["windows"]:
        if w.get("client_pid") == st["pid"]:
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
    at_size = None
    while True:
        w = saver_window(dbg)
        big = w and (not full or (w["w"] >= scr.get("w", 1) and w["h"] >= scr.get("h", 1)))
        if big:
            # AND IT HAS PRESENTED AT THAT SIZE. The window reaching the
            # screen's dimensions is the COMPOSITOR's record; the pixels
            # arrive when the client next presents, which is another
            # process hop. A settled screendump cannot tell the two
            # apart -- two identical reads of a screen that has not
            # repainted yet are as identical as any other -- so this
            # waits for the buffer GENERATION to advance once more.
            gen = w.get("buf", {}).get("gen")
            if at_size is None:
                at_size = gen
            elif gen != at_size:
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
    _QMP[0] = qmp
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
    # PARK THE POINTER FIRST, so the check below knows where to look.
    # Warping after the saver started would dismiss it, which is the
    # behaviour two checks above already rely on.
    scr = dbg.json("gui state --json").get("screen", {})
    park = (scr.get("w", 640) // 2, scr.get("h", 480) // 2)
    dbg.warp_cursor(qmp, park[0], park[1])
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
        # AND NO POINTER ON TOP OF IT. The arrow is the one thing on
        # screen that says the machine is awake, and on a machine with a
        # hardware cursor plane the sprite is not part of the composited
        # image at all -- so "we stopped drawing it" is not the same
        # claim as "it is gone", and only pixels can tell them apart.
        box = [im.getpixel((park[0] + dx, park[1] + dy))
               for dx in range(-2, 12) for dy in range(-2, 18)
               if 0 <= park[0] + dx < w and 0 <= park[1] + dy < h]
        check("...with no pointer drawn on top of it",
              all(sum(v) < 40 for v in box),
              f"brightest {max(box, key=sum)} at the parked pointer")
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

    # --- EVERY saver in the directory actually draws -------------------
    #
    # The list comes from the DIRECTORY, not from a list in here: the
    # setting's choices are that directory, so a saver added later is
    # covered the day it lands rather than the day somebody remembers to
    # add it. A saver that opened a window and painted nothing would pass
    # every check above, which is CLAUDE.md's "it responds is not it is
    # drawn" -- so this reads pixels, and asks for more than one colour
    # so a saver stuck on a flat fill cannot pass either.
    every_saver(dbg, qmp, args)

    # --- AND ITS OPTIONS REACH THE PIXELS -------------------------------
    #
    # A saver declares what it lets you change and reads the chosen
    # values at startup (userland/lib/usaver.h). That the FILE parses is
    # /tests/usaver_test's job and that System Settings WRITES it is
    # settings_test's; what neither can see is whether the saver's
    # drawing actually uses what it read -- which is the half a wired-up
    # descriptor and a `#define` still in place look identical from.
    options_reach_the_pixels(dbg, qmp, args)

    # --- System Settings' Test button -----------------------------------
    #
    # Windows' Preview by another name. What makes it safe is that it
    # spawns the saver and nothing else: the compositor recognises a
    # client out of the savers directory by its SPAWN PATH and adopts it,
    # so a previewed saver is dismissed by input exactly like one the
    # idle clock started. A preview that could not be dismissed would be
    # a way to lose the machine, so that adoption is the check that
    # matters here -- not that a window appeared.
    test_button(dbg, qmp, args)

    dbg.close()
    return report()


SAVER_CONF = "/etc/savers"


def write_conf(dbg, saver, values):
    """Put `values` in /etc/savers/<saver>.conf, replacing what is there.

    Through `tosh -c` and its redirection, which is the only writer a
    test outside the guest has: the kernel shell has no `>` and these
    are not registry settings, so `config set` cannot reach them.
    """
    path = f"{SAVER_CONF}/{saver}.conf"
    dbg.send(f"sh rm {path}")
    for i, (k, v) in enumerate(values):
        # UNQUOTED, deliberately. The kernel shell's `spawn` passes a
        # quoted word through WITH its quotes, so tosh re-lexes it as one
        # word and runs a command called "echo colour=amber". `tosh -c`
        # joins everything after the flag with spaces, so the plain form
        # is the one that works.
        dbg.send(f"sh spawn /bin/tosh -c echo {k}={v} "
                 f"{'>' if i == 0 else '>>'} {path}")
    # READ IT BACK, and retry the lines that did not land. Without this
    # the fixture can be incomplete and the saver then draws its DEFAULT
    # for the missing option -- which is indistinguishable from the
    # saver ignoring the option, and was reported as exactly that.
    for _ in range(4):
        got = dbg.send(f"sh cat {path}") or ""
        missing = [(k, v) for k, v in values if f"{k}={v}" not in got]
        if not missing:
            return path
        for k, v in missing:
            dbg.send(f"sh spawn /bin/tosh -c echo {k}={v} >> {path}")
    check(f"the {saver} fixture reached the disk", False,
          f"{path} is {dbg.send(f'sh cat {path}')!r}, wanted {values}")
    return path


def saver_pixels(dbg, qmp, args, tag):
    """Start the configured saver, photograph it, and take it down.

    Returns (ink, mean red, mean blue) over the non-background pixels,
    or None if it never came up.
    """
    from PIL import Image
    dbg.send("gui idle start")
    if wait_pid(dbg, True) is None or wait_window(dbg) is None:
        return None
    path = f"{args.logs}/saver_opt_{tag}.png"
    # WAIT FOR THE SAVER'S OWN BACKDROP, not for a fixed delay. A window
    # the compositor has accepted is not yet a window it has PAINTED,
    # and a frame taken too early photographs the DESKTOP -- whose ink
    # is grey, where red and blue are equal, which is exactly what a
    # saver ignoring its colour option would look like. That read as a
    # real failure once.
    #
    # Settled-frame comparison is no use here: this saver animates on
    # purpose, so two identical dumps never arrive. The condition is
    # what the saver paints -- a black screen with points on it.
    px, bg, ink = [], None, []
    for _ in range(20):
        qmp.screenshot(path, stable=False)
        im = Image.open(path).convert("RGB")
        px = list(im.getdata())
        bg = max(set(px), key=px.count)
        ink = [p for p in px if p != bg]
        if bg == (0, 0, 0) and ink:
            break
        time.sleep(0.3)
    dbg.key(ord("a"))
    wait_pid(dbg, False)
    if not ink:
        return 0, 0, 0
    return (len(ink), sum(p[0] for p in ink) / len(ink),
            sum(p[2] for p in ink) / len(ink))


def options_reach_the_pixels(dbg, qmp, args):
    saver = "starfield"
    set_setting(dbg, "desktop.screensaver", saver)
    conf = f"{SAVER_CONF}/{saver}.conf"

    # THE TINT, AS A COMPARISON BETWEEN TWO CHANNELS. An absolute value
    # would need a threshold picked from one machine's rendering; "amber
    # is redder than it is blue" is true of the colour and of nothing
    # else. A saver ignoring the option draws WHITE, where the two
    # channels are equal -- so neither half can pass by accident.
    write_conf(dbg, saver, [("stars", 1200), ("colour", "amber")])
    amber = saver_pixels(dbg, qmp, args, "amber")
    write_conf(dbg, saver, [("stars", 1200), ("colour", "ice")])
    ice = saver_pixels(dbg, qmp, args, "ice")
    if check("the tinted saver came up twice", amber is not None and ice is not None,
             f"amber={amber} ice={ice}"):
        check("colour=amber draws warm stars", amber[1] > amber[2] + 8,
              f"r={amber[1]:.1f} b={amber[2]:.1f}")
        check("colour=ice draws cold ones", ice[2] > ice[1] + 8,
              f"r={ice[1]:.1f} b={ice[2]:.1f}")

    # THE INT OPTION, as a count rather than a colour. The two ends of
    # the declared range differ by twenty times, which no frame-to-frame
    # variation in a moving field can cover.
    write_conf(dbg, saver, [("stars", 100)])
    few = saver_pixels(dbg, qmp, args, "few")
    write_conf(dbg, saver, [("stars", 2000)])
    many = saver_pixels(dbg, qmp, args, "many")
    if check("the counted saver came up twice", few is not None and many is not None,
             f"few={few} many={many}"):
        check("stars=2000 paints far more than stars=100",
              many[0] > few[0] * 4, f"{few[0]} px -> {many[0]} px")

    # LEAVE NOTHING BEHIND: a conf file here changes what every later
    # boot of this saver draws.
    dbg.send(f"sh rm {conf}")


def every_saver(dbg, qmp, args):
    from PIL import Image
    out = (dbg.send(f"sh ls {SAVER_DIR}") or "")
    names = [ln.strip() for ln in out.splitlines()
             if ln.strip() and ":" not in ln and "/" not in ln]
    if not check(f"the savers directory lists programs ({len(names)})",
                 len(names) >= 2, f"{names}"):
        return
    for name in sorted(names):
        set_setting(dbg, "desktop.screensaver", name)
        dbg.send("gui idle start")
        st = wait_pid(dbg, True)
        if not check(f"{name}: starts", st is not None and st["pid"] > 0, f"{st}"):
            continue
        if not check(f"{name}: reaches the whole screen",
                     wait_window(dbg) is not None, "never went fullscreen"):
            dbg.key(ord("a"))
            wait_pid(dbg, False)
            continue
        path = f"{args.logs}/saver_{name}.png"
        qmp.screenshot(path)
        im = Image.open(path).convert("RGB")
        px = list(im.crop((0, 0, im.size[0], im.size[1])).getdata())
        distinct = len(set(px))
        # NOT A COLOUR COUNT. Two of these are flat-shaded by design --
        # a tinted icon on black is two colours, a lit octahedron is
        # four -- so a threshold picked from the starfield's thirty-odd
        # would fail them for being exactly what they are. What every
        # saver but `blank` must do is cover some of the screen with
        # something other than its background, and `blank` must do the
        # opposite.
        bg = max(set(px), key=px.count)
        ink = sum(1 for p in px if p != bg)
        # AN ABSOLUTE PIXEL COUNT, not a percentage: a starfield is a few
        # hundred points on nine hundred thousand pixels, which rounds to
        # nothing as a fraction and is plainly a drawn screen.
        if name == "blank":
            check(f"{name}: paints a flat screen, which is its job",
                  distinct == 1, f"{distinct} distinct colours")
        else:
            check(f"{name}: paints something ({distinct} colours, {ink} px)",
                  distinct >= 2 and ink >= 200,
                  f"{distinct} colours and {ink} non-background pixels")
        dbg.key(ord("a"))
        wait_pid(dbg, False)
    set_setting(dbg, "desktop.screensaver", "starfield")


def test_button(dbg, qmp, args):
    # A FRESH WINDOW, because the app dedupes its whole report block: a
    # System Settings left open by an earlier run of this tool is
    # already on the page and logs nothing, so the button's rect never
    # arrives and this reads as a missing button.
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] == "System Settings":
            dbg.send(f"gui close {w['z']}")
            time.sleep(0.5)
    _applog[:] = []
    dbg.send("gui spawn /bin/wm/system/settings")
    # **LONG ENOUGH FOR THE WALK ITSELF.** find_test_button() arrows down
    # the sidebar one row at a time and reads the log after each, so one
    # attempt costs tens of seconds -- a 10s deadline expired PART WAY
    # ALONG and the retry started over from wherever it had got to.
    deadline = time.time() + 90
    rect = None
    while time.time() < deadline and not rect:
        time.sleep(0.3)
        rect = find_test_button(dbg)
    if not check("System Settings reports a Test button on the Screensaver page",
                 rect is not None, "no `settings: test_button` line with a rect"):
        return
    win = None
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] == "System Settings":
            win = w
    if not check("...and the window it is in", win is not None):
        return
    x, y, bw, bh = rect
    cx, cy = win["content"]["x"], win["content"]["y"]
    # INSIDE THE WINDOW, or the click goes somewhere else entirely --
    # the page is a scroll view, and a control below its fold is
    # unreachable rather than merely hard to hit (docs/conventions/gui.md).
    ch = win["content"]["h"]
    if not check("...and it is inside the window, not below the fold",
                 0 <= y and y + bh <= ch, f"button at y={y}+{bh}, content h={ch}"):
        return
    # THE REAL POINTER, warped and confirmed, then a real click. A
    # button arms on press and commits on RELEASE, and an injected
    # position overrides the mouse for the one WM iteration that
    # consumes it -- so the release can land where the real pointer
    # snapped back to, leaving the button armed and never activated.
    dbg.warp_cursor(qmp, cx + x + bw // 2, cy + y + bh // 2)
    qmp.click()
    dbg.settle()
    st = wait_pid(dbg, True)
    if check("pressing Test starts the saver", st is not None and st["pid"] > 0, f"{st}"):
        # THE ADOPTION, which is the whole point: System Settings spawned
        # it, and the compositor is the one reporting it as the saver.
        check("...and the COMPOSITOR has adopted it, so input can end it",
              wait_window(dbg) is not None, "no fullscreen saver window")
        dbg.key(ord("a"))
        st = wait_pid(dbg, False)
        check("...and a keypress ends a PREVIEWED saver too",
              st is not None and st["pid"] == 0, f"{st}")
        titles = [w["title"] for w in dbg.json("gui windows --json")["windows"]]
        # The preview must not take its own window with it.
        check("...leaving System Settings open behind it",
              "System Settings" in titles, f"{titles}")
    # PUT THE DESKTOP BACK. A window left open changes what every later
    # tool sees, the same rule as the settings this tool restores.
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] == "System Settings":
            dbg.send(f"gui close {w['z']}")
            time.sleep(0.4)


# THE APP'S LINES ACCUMULATE HERE. `DebugConsole.logs()` CLEARS what it
# returns, so a helper that re-reads it on every poll loses the sidebar
# rows it needs to navigate with -- which is how this looked for a Test
# button on a page it had never managed to open.
_applog = []
# The QMP session, so find_test_button() can use the REAL pointer. A
# module slot rather than a parameter because the polling loop that
# calls it is three frames away from where the session is made.
_QMP = [None]


def _drain(dbg):
    _applog.extend(ln.strip() for ln in dbg.logs())
    return _applog


def _last(pattern):
    import re
    hit = None
    for line in _applog:
        m = re.search(pattern, line)
        if m:
            hit = m
    return hit


def find_test_button(dbg):
    """The Test button's rect, from the app's own report.

    The Screensaver page has to be OPEN for the button to be shown, so
    this navigates there first -- by the sidebar row's LABEL, never by a
    row index, and through the tree rect the app reports rather than
    arithmetic of its own.
    """
    import re
    _drain(dbg)
    btn = _last(r"settings: test_button (-?\d+) (-?\d+) (-?\d+) (-?\d+) shown (\d+)")
    if btn:
        g = [int(v) for v in btn.groups()]
        if g[4] and g[2] > 0:
            return (g[0], g[1], g[2], g[3])
    row_y = None
    for line in _applog:
        m = re.search(r"settings: row \d+ id \d+ y (-?\d+) depth \d+ (.+)", line)
        if m and m.group(2).strip() == "Screensaver":
            row_y = int(m.group(1))
    tree = _last(r"settings: layout tree (-?\d+) (-?\d+) (-?\d+) (-?\d+)")
    if row_y is None or not tree:
        return None
    win = None
    for w in dbg.json("gui windows --json")["windows"]:
        if w["title"] == "System Settings":
            win = w
    if not win:
        return None
    tx, ty, tw, _th = (int(v) for v in tree.groups())
    # **ARROWED TO, NOT TYPED AND NOT CLICKED.** The row report gives an
    # UNSCROLLED y, so clicking a row below the fold lands on whatever is
    # at that point -- and Screensaver moved from the eighth row to the
    # twenty-eighth. And `uui_sidebar` has no type-ahead (arrows only),
    # so spelling the name at it does nothing. The click that starts the
    # walk must land on a SELECTABLE row: row 0 is a heading.
    first_item_y = None
    for line in _applog:
        m2 = re.search(r"settings: row \d+ id \d+ y (-?\d+) depth (\d+) ", line)
        if m2 and m2.group(2) == "1" and first_item_y is None:
            first_item_y = int(m2.group(1))
    dbg.warp_cursor(_QMP[0], win["content"]["x"] + tx + tw // 2,
                    win["content"]["y"] + (first_item_y if first_item_y
                                            is not None else ty + 8))
    _QMP[0].click()
    dbg.settle()
    # Stops on the BUTTON, never on a count -- a fixed number of Downs
    # goes stale the next time a setting lands above it. One key per
    # look, too: batching five presses between checks overshoots the
    # wanted row, and `settle()` after each costs a round trip that took
    # the tool past its 600s budget.
    for _ in range(40):
        dbg.send("gui key 0x92")          # KEY_ARROW_DOWN
        time.sleep(0.18)
        _drain(dbg)
        btn = _last(r"settings: test_button (-?\d+) (-?\d+) (-?\d+) (-?\d+) shown (\d+)")
        if btn:
            g = [int(v) for v in btn.groups()]
            if g[4] and g[2] > 0:
                return (g[0], g[1], g[2], g[3])
    return None


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
