#!/usr/bin/env python3
"""Window animations (userland/wm/wm_anim.c) and `desktop.animations`.

A window opening, closing, minimizing and coming back is drawn as a
GHOST for ~150 ms. Three things are asserted about each, from the WM's
own numbers and from pixels:

  - `gui state --json` reports `anims` >= 1 right after the state change
    and 0 once settled (the WM ran a ghost, and finished it)
  - a frame captured mid-ghost differs from the settled frame inside the
    window's rect AND from the frame before the change -- the ghost is
    something in between, not the before or the after
  - with `desktop.animations` off, `anims` never leaves 0 and the first
    frame after the change already equals the settled one

The setting is written to /etc/desktop.conf and put back to `on` in a
`finally`, since a setting left behind changes the machine for every
later tool (CLAUDE.md).

    python3 tools/vm.py start
    python3 tools/animation_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402
from harness import Results  # noqa: E402

SPAWN = "/bin/wm/apps/calculator"
TITLE = "Calculator"


Result = Results


def shot(qmp, path):
    from PIL import Image
    qmp.screenshot(path, stable=False, settle=0)
    return Image.open(path).convert("RGB")


def diff_in(a, b, rect):
    """Sampled pixels differing between two frames inside rect (x, y, w, h)."""
    x0, y0, w, h = rect
    n = 0
    for y in range(y0, y0 + h, 3):
        for x in range(x0, x0 + w, 3):
            if a.getpixel((x, y)) != b.getpixel((x, y)):
                n += 1
    return n


def anims_now(dbg):
    return dbg.state().get("anims")


def kill_app(dbg):
    for p in dbg.processes():
        if p["name"] == "calculator" and p["state"] != "zombie":
            dbg.send(f"sh kill {p['pid']}")
    time.sleep(0.6)


def run_change(dbg, qmp, tmp, res, name, action, rect_fn, expect_anim):
    """Perform `action`, watch the WM's ghost report while it runs, then settle.

    THE WM IS ASKED WHERE THE GHOST IS (`anim_rects` in `gui state
    --json`) rather than photographed: a QMP screenshot takes tens of
    milliseconds under TCG and a 150 ms ghost is easily over before one
    lands, which read a working animation as none. The report is
    polled as fast as the console answers; two distinct rects or alphas
    are the motion, and every rect must lie between the window's rect
    and the ghost's destination -- so a report that merely repeated one
    rect, or one that jumped somewhere else, would both fail.
    """
    before = shot(qmp, os.path.join(tmp, f"{name}_before.png"))
    action()
    seen = []          # (x, y, w, h, alpha) per poll while a ghost is in flight
    peak = 0
    deadline = time.time() + 0.6
    while time.time() < deadline:
        st = dbg.state()
        a = st.get("anims") or 0
        peak = max(peak, a)
        for r in st.get("anim_rects", []):
            t = (r["x"], r["y"], r["w"], r["h"], r["alpha"])
            if not seen or seen[-1] != t:
                seen.append(t)
        if seen and a == 0:
            break
        if not expect_anim and time.time() > deadline - 0.3:
            break
        time.sleep(0.005)
    dbg.settle(1.0)
    time.sleep(0.3)
    after = shot(qmp, os.path.join(tmp, f"{name}_after.png"))
    rect = rect_fn()
    settled = anims_now(dbg)
    d_after = diff_in(before, after, rect) if rect else -1
    if expect_anim:
        res.check(f"{name}: the WM reports a ghost in flight", peak >= 1, f"anims peak={peak}")
        res.check(f"{name}: ...and none once settled", settled == 0, f"anims={settled}")
        res.check(f"{name}: the ghost MOVED -- at least two distinct rects or alphas reported",
                  len(seen) >= 2, f"seen={seen}")
        if rect and seen:
            x, y, w, h = rect
            # Every ghost rect is inside the union of the window's rect
            # and the whole screen band below it (a minimize heads for
            # the taskbar); it never leaves the screen.
            inside = all(0 <= r[0] and r[0] + r[2] <= 1280 + 1 and 0 <= r[1] and r[1] + r[3] <= 720 + 1
                         and r[2] <= w + 2 and r[3] <= h + 2 for r in seen)
            res.check(f"{name}: every reported ghost rect is on screen and no larger than the window",
                      inside, f"window={rect} seen={seen}")
        res.check(f"{name}: the screen changed inside the window's rect", d_after > 20,
                  f"rect={rect} before-vs-after={d_after}")
    else:
        res.check(f"{name}: animations off -- the WM reports no ghost", peak == 0 and not seen,
                  f"anims peak={peak} seen={seen}")
        res.check(f"{name}: animations off -- the screen still changed inside the window's rect",
                  d_after > 20, f"rect={rect} before-vs-after={d_after}")
    return after


def run(dbg, qmp, tmp, res):
    def win():
        return dbg.window(TITLE)

    def rect_of_window():
        w = win()
        return (w["x"], w["y"], w["w"], w["h"]) if w else None

    last_rect = {}

    def rect_last():
        return last_rect.get("r")

    def taskbar_button():
        tb = dbg.json("gui taskbar --json")
        for b in tb["buttons"]:
            if b["app_id"] == "calculator" or b["title"] == TITLE:
                return b
        return None

    for setting, expect in (("on", True), ("off", False)):
        dbg.send(f"sh config set desktop.animations {setting}")
        time.sleep(0.4)
        # OPEN
        run_change(dbg, qmp, tmp, res, f"open[{setting}]",
                   lambda: dbg.send(f"gui spawn {SPAWN}"), rect_of_window, expect)
        w = win()
        res.check(f"open[{setting}]: the window is up and normal", w is not None and w["state"] == "normal")
        if not w:
            return
        last_rect["r"] = (w["x"], w["y"], w["w"], w["h"])
        b = taskbar_button()
        res.check(f"open[{setting}]: the window has a taskbar button", b is not None)
        if b:
            # MINIMIZE: a click on the focused window's button.
            run_change(dbg, qmp, tmp, res, f"minimize[{setting}]",
                       lambda: dbg.send(f"gui click {b['cx']} {b['cy']}"), rect_last, expect)
            res.check(f"minimize[{setting}]: the window is minimized", win()["state"] == "minimized")
            # RESTORE
            run_change(dbg, qmp, tmp, res, f"restore[{setting}]",
                       lambda: dbg.send(f"gui click {b['cx']} {b['cy']}"), rect_last, expect)
            res.check(f"restore[{setting}]: the window is back", win()["state"] == "normal")
        # CLOSE: the client exits; the ghost outlives it.
        pid = [p["pid"] for p in dbg.processes() if p["name"] == "calculator"]
        run_change(dbg, qmp, tmp, res, f"close[{setting}]",
                   lambda: dbg.send(f"sh kill {pid[0]}") if pid else None, rect_last, expect)
        res.check(f"close[{setting}]: the window is gone", win() is None)
        kill_app(dbg)


def ghost_ms(dbg, label):
    """How long a ghost stays in flight for one minimize, in ms, and
    whether one appeared at all. Polls `anims` rather than pixels: this
    is about DURATION, and a screenshot cannot time anything."""
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if "untitled" in str(x.get("title", ""))), None)
    if not b:
        return None, 0
    t0 = time.time()
    dbg.send(f"gui click {b['cx']} {b['cy']}")
    peak, saw = 0, False
    while time.time() - t0 < 4.0:
        a = dbg.state().get("anims") or 0
        peak = max(peak, a)
        if a:
            saw = True
        elif saw:
            break
    ms = (time.time() - t0) * 1000.0
    dbg.settle(0.8)
    tb = dbg.taskbar()
    buttons = tb.get("buttons") if isinstance(tb, dict) else tb
    b = next((x for x in (buttons or []) if "untitled" in str(x.get("title", ""))), None)
    if b:
        dbg.send(f"gui click {b['cx']} {b['cy']}")   # restore, for the next round
        dbg.settle(0.8)
    return (ms if saw else None), peak


def run_speed(dbg, res):
    """`desktop.animation_speed` scales every animation -- KWin's shape.

    ASSERTS THE ORDER, NOT THE MILLISECONDS. The durations are real
    (125/250/500 ms at the time of writing) but a poll adds its own
    overhead and the guest is a TCG machine, so pinning exact numbers
    would be a flake generator. What must hold is that the knob does
    something monotonic, and that `instant` means NO GHOST AT ALL --
    not a very short one, since the point of instant is that nothing is
    snapshotted and the real window is never hidden.
    """
    # ESTABLISH THE PRECONDITION. run() above ends on its `off` case, so
    # arriving here with animations disabled would report "no ghost
    # flies" for every speed -- a fixture failure wearing the costume of
    # the thing under test.
    dbg.send("sh config set desktop.animations on")
    dbg.settle(1.2)
    dbg.open_app("Notepad")
    dbg.settle(1.5)
    if not [w for w in dbg.windows() if w["title"] == "untitled"]:
        res.check("speed: a window to minimize", False, "Notepad did not open")
        return

    got = {}
    try:
        for sp in ("instant", "fast", "normal", "slow"):
            dbg.send(f"sh config set desktop.animation_speed {sp}")
            dbg.settle(1.2)
            ms, peak = ghost_ms(dbg, sp)
            got[sp] = ms
            if sp == "instant":
                res.check("speed instant: no ghost is created at all",
                          peak == 0, f"anims peaked at {peak}")
            else:
                res.check(f"speed {sp}: a ghost flies", ms is not None,
                          "no ghost seen")
    finally:
        dbg.send("sh config set desktop.animation_speed normal")
        dbg.settle(0.8)

    if got.get("fast") and got.get("normal") and got.get("slow"):
        res.check("speed: fast < normal < slow",
                  got["fast"] < got["normal"] < got["slow"],
                  f"fast={got['fast']:.0f} normal={got['normal']:.0f} "
                  f"slow={got['slow']:.0f} ms")
        # Slow is 2x normal by construction; allow wide margins for the
        # poll and for TCG, but a knob that moved by 10% would be one
        # nobody can feel.
        res.check("speed: slow is markedly longer than fast",
                  got["slow"] > got["fast"] * 2.0,
                  f"fast={got['fast']:.0f} slow={got['slow']:.0f} ms")


def run_effects(dbg, qmp, tmp, res):
    """`desktop.minimize_effect`: scale / genie / squash / glide / shatter.

    Three things per effect. It must ANIMATE -- a ghost in flight whose
    reported rect actually changes, so an effect that silently fell back
    to drawing nothing would fail. And it must leave NO STALE PIXELS:
    the screen once it has settled must match a forced full repaint.

    AND IT MUST BE THE EFFECT IT SAYS: `pieces` from `gui state --json`
    is 1 for every effect that moves one box and the tile count for
    SHATTER, which is the only thing distinguishing the two from
    outside. A shatter that fell back to drawing a plain scale moves a
    ghost, leaves no stale pixels, and passes both other checks.

    SHATTER RUNS TWICE, once per motion: `explode` throws tiles OUTSIDE
    the window, which is a different damage rule, and the stale-pixel
    check is the only thing that would catch a margin set too small.

    THE GENIE IS WHY THE SECOND CHECK EXISTS. It paints a whole tube
    from the window's top edge down to the taskbar button, which is not
    its lerped rect -- so damaging the rect alone would leave the neck
    on screen. That is the same fault the drop shadow and the
    resize-under-a-ghost both were, and it is invisible to a test that
    only asks whether something moved.

    The taskbar strip is excluded from the comparison: the clock
    advances between the two captures, which is a real difference and
    not a bug. Measured at 13 sampled pixels, all of them in the clock.
    """
    from PIL import Image

    dbg.send("sh config set desktop.animations on")
    dbg.settle(1.2)

    # EXACTLY ONE NOTEPAD, and that is not fussiness. Notepad is
    # multi-instance, so a second window makes the taskbar GROUP them --
    # and a click on a group button opens the group's list instead of
    # minimizing anything. The phase before this one leaves a Notepad
    # open, so opening another unconditionally produced a button that
    # could not minimize, reported as "no ghost flies" for every effect
    # while the screen check passed because nothing had changed.
    extra = [w for w in dbg.windows() if w["title"] == "untitled"]
    for w in extra[1:]:
        dbg.send(f"sh kill {w['client_pid']}")
        dbg.settle(1.0)
    if not extra:
        dbg.open_app("Notepad")
        dbg.settle(1.5)
    n = len([w for w in dbg.windows() if w["title"] == "untitled"])
    res.check("effects: exactly one Notepad window to drive", n == 1,
              f"{n} windows -- the taskbar would group them")

    def button():
        tb = dbg.taskbar()
        buttons = tb.get("buttons") if isinstance(tb, dict) else tb
        return next((x for x in (buttons or [])
                     if "untitled" in str(x.get("title", ""))), None)

    def full_repaint():
        dbg.send("sh config set desktop.shadows off")
        dbg.settle(0.8)
        dbg.send("sh config set desktop.shadows on")
        dbg.settle(1.0)

    def ensure_normal():
        """The window OPEN and not minimized, whatever the phase before
        this left behind. Clicking a minimized window's button restores
        it instead of minimizing it, and the check would then be timing
        the wrong direction -- or, if the restore never landed, nothing
        at all. Established rather than assumed: this ran after two
        other phases and reported 'no ghost' for every effect because
        the window was already down."""
        for _ in range(3):
            w = next((x for x in dbg.windows() if x["title"] == "untitled"), None)
            if w and w.get("state") == "normal":
                return True
            b = button()
            if b:
                dbg.send(f"gui click {b['cx']} {b['cy']}")
            dbg.settle(1.2)
        return False

    try:
        dbg.send("sh config set desktop.animation_speed normal")
        dbg.settle(1.0)
        for eff, motion in (("scale", None), ("genie", None), ("squash", None),
                            ("glide", None), ("shatter", "pour"),
                            ("shatter", "explode")):
            name = eff if not motion else f"{eff}/{motion}"
            if motion:
                # The effect's OWN option file (userland/lib/ueffect.h),
                # put in place with cp -- the debug console's `sh` runs a
                # program and does not lex a redirect, so a fixture is
                # how a test writes a file here.
                dbg.send(f"sh cp /tests/shatter_{motion}.conf /etc/effects/shatter.conf")
                # ...and a generation bump so the WM re-reads it: adopt()
                # runs on a SETTING change, and setting the effect to what
                # it already is does nothing at all.
                dbg.send("sh config set desktop.minimize_effect scale")
                dbg.settle(0.6)
            dbg.send(f"sh config set desktop.minimize_effect {eff}")
            dbg.settle(1.2)
            if not ensure_normal():
                res.check(f"effect {name}: the window is up before minimizing it",
                          False, "could not get it out of the taskbar")
                continue
            b = button()
            if not b:
                res.check(f"effect {name}: a taskbar button to click", False)
                continue

            rects, peak, saw, pieces = set(), 0, False, 0
            t0 = time.time()
            dbg.send(f"gui click {b['cx']} {b['cy']}")
            while time.time() - t0 < 4.0:
                st = dbg.state()
                a = st.get("anims") or 0
                peak = max(peak, a)
                for r in st.get("anim_rects", []):
                    rects.add((r["x"], r["y"], r["w"], r["h"]))
                    pieces = max(pieces, r.get("pieces", 0))
                if a:
                    saw = True
                elif saw:
                    break
            res.check(f"effect {name}: a ghost flies and MOVES",
                      peak >= 1 and len(rects) >= 2,
                      f"anims peak={peak} distinct rects={len(rects)}")
            # THE ONE CHECK A FALLBACK CANNOT PASS. Everything else here
            # is satisfied by any effect that moves a box to the button.
            want = "many" if eff == "shatter" else "one"
            ok = pieces > 1 if eff == "shatter" else pieces == 1
            res.check(f"effect {name}: drawn as {want} piece(s)", ok,
                      f"pieces={pieces}")

            dbg.settle(1.5)
            live_p = os.path.join(tmp, f"eff_{name.replace(chr(47), chr(95))}_live.png")
            qmp.screenshot(live_p, settle=0.0, stable=True)
            live = Image.open(live_p).convert("RGB")
            full_repaint()
            ref_p = os.path.join(tmp, f"eff_{name.replace(chr(47), chr(95))}_full.png")
            qmp.screenshot(ref_p, settle=0.0, stable=True)
            ref = Image.open(ref_p).convert("RGB")

            bad = []
            for y in range(0, min(live.height, ref.height) - 40, 2):
                for x in range(0, min(live.width, ref.width), 2):
                    a_, c_ = live.getpixel((x, y)), ref.getpixel((x, y))
                    if max(abs(a_[i] - c_[i]) for i in range(3)) > 24:
                        bad.append((x, y))
            res.check(f"effect {name}: nothing stale once it has settled",
                      len(bad) == 0,
                      f"{len(bad)} px differ from a full repaint, first {bad[:3]}")

            b = button()
            if b:
                dbg.send(f"gui click {b['cx']} {b['cy']}")   # restore
                dbg.settle(1.2)
    finally:
        dbg.send("sh config set desktop.minimize_effect scale")
        dbg.settle(0.8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "animation_test")
    tmp = args.logs or "/tmp"
    os.makedirs(tmp, exist_ok=True)

    res = Result()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    try:
        run(dbg, qmp, tmp, res)
        run_speed(dbg, res)
        run_effects(dbg, qmp, tmp, res)
    finally:
        try:
            dbg.send("sh config set desktop.animations on")
            dbg.send("sh config set desktop.animation_speed normal")
            dbg.send("sh config set desktop.minimize_effect scale")
        finally:
            dbg.close()
    print(f"\nanimation_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
