#!/usr/bin/env python3
"""Drive userland/tests/screenclient.c -- Milestone 41 stage 4b's ring-3
compositor SURFACE: the back buffer, the clip rect, the damage box, the
blit, the publish path and the damage-verify diff.

WHAT IS ACTUALLY UNDER TEST, AND WHAT CANNOT BE
-----------------------------------------------
The ring-0 WM still owns the screen while this runs, so a block the
client presents survives only until the WM's next frame. A test that
looked for it in a screenshot would fail against a WORKING kernel. That
mistake cost real time on stage 4a and the checks here are shaped around
it, in two groups:

  * **Back-buffer logic** (damage, clip, blit, verify). Deterministic,
    entirely inside the client, and asserted to the pixel -- exact
    counts, exact bounding boxes, exact source offsets. These are the
    parts whose breakage would make a ring-3 WM draw subtly wrong pixels
    forever while every screenshot still looked plausible.
  * **The mapping is the real screen.** Proved by agreement between a
    pixel the client reads through the mapping and QEMU's screendump of
    the same point -- the technique compositor_test.py already uses for
    R1, and the reason the client's probe is READ-ONLY: reading the
    steady desktop races nothing, whereas reading back a write races the
    WM's repaint and would prove only that some writable page is there.

WHY THE EXPECTED VALUES ARE SPELLED OUT
---------------------------------------
Every geometric check here asserts an exact number rather than "it
changed", because this repo has shipped both failure modes that a
looser assertion passes: a damage box that covers only the LAST rect
(fine for one rect, wrong for two), and a clipped blit that draws the
right count of pixels in the right box with the wrong CONTENTS, because
it clipped the destination and kept reading from the source's origin.
The blit check reads back four specific source values for exactly that.

THE HEAP IS PART OF WHAT THIS PROVES. `screenclient: screen ...` only
appears if sbrk handed over a full screen of back buffer. Before this
stage the ring-3 heap was 1 MiB against a 3.5 MiB buffer at 1280x720, so
the first check is a real regression gate on kernel/uaddr.h, not a
formality -- see its comment there.

Usage (the VM must already be up and in GUI mode):

    python3 tools/vm.py start
    python3 tools/screen_surface_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole          # noqa: E402
from qmp_test import QMPSession             # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/tests/screenclient"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}"
          + (f"    [{detail}]" if detail else ""))


def client_lines(dbg, acc):
    """Drain the kernel log into `acc`.

    DebugConsole.logs() CLEARS what it returns, so a second parser over
    it finds nothing -- accumulate rather than re-reading. That trap has
    cost this repo a whole tool's worth of vanished values.
    """
    for line in dbg.logs():
        if "screenclient:" in line:
            acc.append(line.strip())
    return acc


# Set when the desktop had to be taken down for this run -- see main().
# Keys then travel as REAL keystrokes over QMP instead of `gui key`,
# which is dispatched from inside a window manager that no longer
# exists. screenclient reads them either way: as the registered
# compositor it receives WIN_EV_RAW_KEY, which the kernel pushes from
# the hardware when no ring-0 layer is in the way.
NO_WM = {"on": False, "qmp": None}


def send_key(dbg, key):
    if NO_WM["on"]:
        NO_WM["qmp"].send_key(key)
    else:
        dbg.key(key)


def one(dbg, acc, key, prefix, settle=0.4):
    """The reply line for one command.

    With a desktop up the command is a keystroke and the reply is
    whatever arrived after it. Without one the client has already run
    the whole sequence itself (`screenclient auto`), so the reply is
    looked up by its STEP MARKER instead -- `d` and `n` both log
    `damage`, and picking them apart by position in the log is precisely
    the fragility the markers exist to remove.
    """
    if NO_WM["on"]:
        client_lines(dbg, acc)
        want = f"screenclient: step {key}"
        # The LAST occurrence: `r` runs twice, and the second one is the
        # one a screendump taken after the run can be compared with.
        starts = [i for i, l in enumerate(acc) if want in l]
        if not starts:
            return ""
        section = acc[starts[-1] + 1:]
        for l in section:
            if "screenclient: step " in l:
                break
            if f"screenclient: {prefix}" in l:
                return l
        return ""

    client_lines(dbg, acc)
    before = len(acc)
    send_key(dbg, key)
    time.sleep(settle)
    client_lines(dbg, acc)
    hits = [l for l in acc[before:] if f"screenclient: {prefix}" in l]
    return hits[-1] if hits else ""


def fields(line, prefix):
    """The whitespace-separated fields after `screenclient: <prefix>`."""
    if not line or f"screenclient: {prefix}" not in line:
        return []
    return line.split(f"screenclient: {prefix}", 1)[1].split()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default=DEFAULT_SOCK)
    ap.add_argument("--qmp-port", type=int, default=4445)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()

    # Needed either way: the "is this the real screen" check reads pixels
    # off the display, not out of the client's own log.
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        # `vm.py start` lands at the PHYSICAL shell, so the desktop has to
        # be entered by typing. It cannot go over the debug console --
        # `gui` never returns, so it is on debug_console.c's blocked list.
        qmp.send_text("gui")
        qmp.send_key("ret")
        time.sleep(2.0)

    dbg = DebugConsole(args.sock)
    acc = []

    print("screen surface (M41 stage 4b):")

    # THE COMPOSITOR ROLE IS SINGLE, so a ring-3 desktop already holding
    # it is not a backdrop this tool can ignore -- claiming the role
    # would EVICT the desktop and then every `gui` command in here would
    # be answered by screenclient, which does not implement them. (That
    # is exactly how this tool failed under `gui3`.)
    #
    # So the desktop is taken down deliberately and the checks run with
    # the client as the only compositor, which is the configuration they
    # were written for. `kill` at the shell, not `gui kill`: the latter
    # runs inside the WM's own loop and scheduler_kill() refuses to kill
    # the current process. `spawn`, not `run`: the legacy loader is not a
    # scheduled process, so its win_request() is refused outright.
    owner = (dbg.json("gui compositor --json") or {}).get("pid", 0)
    NO_WM["qmp"] = qmp
    if owner > 0:
        print(f"  (the ring-3 desktop holds the role as pid {owner} -- "
              f"taking it down for this run)")
        # DELETE THE DESCRIPTOR FIRST, or the kill below is undone before
        # the client has finished. init supervises the desktop since
        # docs/init-design.md stage 2 and restarts it with a ZERO backoff,
        # and the restarted desktop CLAIMS THE ROLE BACK -- so this tool
        # took the role, lost it mid-run, and its client never reached
        # its own release/exit steps. Removing the descriptor makes init
        # stop restarting it without touching the running one, which is
        # the precondition every check below assumes: the client is the
        # only compositor.
        #
        # The image is gui_regress's per-tool throwaway copy, so deleting
        # a seeded file costs nothing beyond this run.
        dbg.send("sh rm /etc/services.d/toywm")
        time.sleep(0.4)
        dbg.send(f"sh kill {owner}")
        time.sleep(2.0)
        NO_WM["on"] = True

    dbg.logs()
    if NO_WM["on"]:
        # `auto`: it runs the whole sequence itself, because with no
        # desktop the physical shell owns the keyboard and injected
        # keystrokes never reach a compositor.
        dbg.send(f"sh spawn {SPAWN_PATH} auto")
    else:
        dbg.send(f"gui spawn {SPAWN_PATH}")
    time.sleep(2.5)
    client_lines(dbg, acc)

    check("the client registers as compositor",
          any("screenclient: registered" in l for l in acc),
          acc[-1] if acc else "no output")

    # --- the grant, and the heap that has to hold its back buffer -----
    screen = [l for l in acc if "screenclient: screen" in l]
    line = screen[-1] if screen else ""
    f = fields(line, "screen")
    w = h = pitch = bpp = None
    if len(f) == 4:
        try:
            w, h, pitch, bpp = (int(v) for v in f)
        except ValueError:
            pass
    check("the framebuffer grant is accepted and a back buffer allocated",
          w is not None, line or "no screen line")

    if NO_WM["on"]:
        # No WM to ask, so the reference is the DISPLAY itself -- which
        # is a stronger one anyway: the screendump is the real
        # framebuffer's geometry rather than the WM's belief about it.
        ref = os.path.join(tempfile.gettempdir(), "screenref.png")
        qmp.screenshot(ref)
        rw, rh = Image.open(ref).size
        kscreen = {"w": rw, "h": rh}
    else:
        state = dbg.state() or {}
        kscreen = state.get("screen", {})
    check("its geometry matches the kernel's own screen",
          w is not None and w == kscreen.get("w") and h == kscreen.get("h")
          and bpp in (24, 32) and pitch >= w * (bpp // 8),
          f"{w}x{h} pitch={pitch} bpp={bpp} vs kernel {kscreen}")

    if w is None:
        # Nothing below can mean anything without a surface, and reporting
        # a cascade of failures would bury the one that matters.
        return summarise()

    # --- the mapping is the real screen -------------------------------
    #
    # The load-bearing check of the group. Both observers read the same
    # steady desktop pixel, from opposite sides of the mapping.
    probe = one(dbg, acc, "r", "probe")
    pf = fields(probe, "probe")
    client_val = pf[0].lower().lstrip("0") if pf else None

    shot = os.path.join(tempfile.gettempdir(), "screenfb.png")
    qmp.screenshot(shot)
    seen = Image.open(shot).convert("RGB").getpixel((100, 100))
    seen_hex = f"{seen[0]:02x}{seen[1]:02x}{seen[2]:02x}".lstrip("0")
    check("the mapping reads the REAL screen (client vs screendump)",
          client_val is not None and client_val == seen_hex,
          f"client={client_val} screendump={seen_hex}")

    # --- the damage box is a UNION, not the last rect ------------------
    #
    # (100,100 64x64) and (300,200 10x10) -> x 100..310, y 100..210.
    # An implementation that tracks only the most recent rectangle
    # reports "300 200 10 10" and passes any did-it-change assertion.
    d = fields(one(dbg, acc, "d", "damage"), "damage")
    check("the damage box covers every rect drawn since the reset",
          d == ["100", "100", "210", "110"],
          " ".join(d) or "no damage line")

    # --- the clip bounds drawing AND damage ---------------------------
    #
    # Fields: drawn_outside x y w h. A fill wholly outside the clip must
    # change nothing (0), and a fill straddling it must damage exactly
    # the clip -- no more, or a compositor publishes a rect wider than
    # what changed; no less, and it leaves stale pixels on screen.
    c = fields(one(dbg, acc, "c", "clip"), "clip")
    check("a fill outside the clip changes nothing",
          c[:1] == ["0"], " ".join(c) or "no clip line")
    check("a fill straddling the clip damages exactly the clip",
          c[1:] == ["200", "200", "10", "10"],
          " ".join(c) or "no clip line")

    # --- a clipped blit reads from the right place --------------------
    #
    # 4x4 source of 0x100000+index, clipped to its bottom-right 2x2, so
    # the surviving pixels must be source indices 10, 11, 14, 15. A blit
    # that clips the destination but reads from the source's origin
    # yields 0, 1, 4, 5 -- same count, same box, wrong picture.
    b = fields(one(dbg, acc, "b", "blit"), "blit")
    check("a clipped blit offsets its SOURCE, not just its destination",
          [v.lower() for v in b] == ["10000a", "10000b", "10000e", "10000f"],
          " ".join(b) or "no blit line")

    # --- the verify diff is exact -------------------------------------
    #
    # A 3x2 rect at (400,400): 6 pixels, first difference at (400,400),
    # bounding box (400,400)-(403,402) half-open. R2 requires this
    # facility to keep reporting count, first and box, because
    # damage_sweep.py parses all three.
    v = fields(one(dbg, acc, "v", "verify"), "verify")
    check("the verify diff reports exact count, first pixel and box",
          v == ["6", "400", "400", "400", "400", "403", "402"],
          " ".join(v) or "no verify line")

    # --- present publishes the damaged box ----------------------------
    p = fields(one(dbg, acc, "p", "present"), "present")
    check("present publishes exactly the damaged box",
          p[:4] == ["100", "100", "64", "64"],
          " ".join(p) or "no present line")
    check("the presented pixels are in the framebuffer",
          len(p) >= 6 and p[5].lower().lstrip("0") == "ff00ff",
          " ".join(p) or "no present line")

    # --- an empty present is a no-op, not a corruption ----------------
    #
    # Presenting with nothing drawn must publish no rect AND leave the
    # box usable: the check is that the NEXT frame still reports its own
    # damage correctly. A present that reset state it should not have
    # touched shows up here rather than as a mystery a frame later.
    n = fields(one(dbg, acc, "n", "damage"), "damage")
    check("an empty present leaves the next frame's damage intact",
          n == ["50", "60", "8", "9"], " ".join(n) or "no damage line")

    if NO_WM["on"]:
        # It is holding the screen so the probe above could be compared
        # against a screendump; wait it out before asserting it released.
        deadline = time.time() + 9.0
        while time.time() < deadline:
            client_lines(dbg, acc)
            if any("screenclient: exit" in l for l in acc):
                break
            time.sleep(0.5)
    else:
        send_key(dbg, "q")
        time.sleep(0.6)
    client_lines(dbg, acc)
    check("the client releases the role and exits cleanly",
          any("screenclient: released" in l for l in acc)
          and any("screenclient: exit" in l for l in acc),
          "released/exit")

    # The desktop must be unharmed by a process that mapped the screen
    # and wrote to it -- the whole grant is pointless if it can wedge the
    # session. `gui state` answering at all is the liveness proof: it is
    # dispatched from inside wm_run(), so a frozen WM cannot reply.
    if NO_WM["on"]:
        # There is no desktop to survive; what must is the KERNEL, and
        # `sh` is served by the kernel context rather than by any
        # process, so an answer here is exactly that claim.
        # `rescue df`, not `df`: plain df is a ring-3 program now, and
        # this check is about the KERNEL being alive, not about it
        # still being able to load one. shell_rescue.c's copy runs
        # entirely in ring 0. See compositor_death_test.py.
        df = dbg.send("sh rescue df")
        check("the kernel survives a process that mapped the screen",
              "Filesystem:" in df,
              df.strip().splitlines()[0] if df.strip() else "no answer")
    else:
        check("the desktop survives", bool(dbg.state()), "gui state answered")

    return summarise()


def summarise():
    bad = [n for n, ok, _ in checks if not ok]
    print(f"\n{len(checks) - len(bad)}/{len(checks)} checks passed")
    if bad:
        print("FAILED: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
