#!/usr/bin/env python3
"""tools/damage_sweep.py -- exercise the WM against its damage invariant.

WHAT THIS IS FOR
----------------
The compositor only repaints the region declared as damage, so it is
correct only if everything that changes on screen is inside that region
(docs/gui-guidelines.md, "The damage invariant"). `gui damage verify on`
turns a violation into a loud report, but somebody still has to DO the
interactions -- and the interesting ones are the boring ones: raise a
window, drag it over another, minimize and restore it, resize it, close
the frontmost one. Every damage bug this project has had came from a
state change that moved no geometry: a title bar losing focus, a taskbar
button changing tint, a window inheriting focus when the one above it
closed.

So this walks a fixed sequence of those interactions with verification
on, and reports every distinct violation with the interaction that
produced it. Run it after touching anything that draws, damages,
focuses, or changes window chrome:

    python3 tools/vm.py start                    # or --disk a copy
    python3 tools/damage_sweep.py                # enters GUI mode itself
    echo $?                                      # 0 = invariant held

WHY IT IS A TOOL AND NOT A SCRIPT
---------------------------------
It was a scratch script first, and rewriting it cost real time twice.
It also encodes three things that are easy to get wrong and expensive to
rediscover:

  1. `DebugConsole.click()/drag()` return events() filtered to the
     `uidemo:` prefix -- a `wm: DAMAGE BUG` line does not match and was
     silently dropped, so the first version of this reported a clean run
     against a kernel that was actively failing. It asserts on
     damage_bugs() instead.
  2. Coordinates are re-read from `gui windows` between steps, never
     carried over. Windows move; a cached rect drags the wrong thing and
     the failure looks like a WM bug.
  3. It VALIDATES ITSELF. --positive-control INJECTS a miss -- `gui
     damage shrink 4` insets every WINDOW damage rect by 32 px for the
     next few rendered frames, so a drag's frames are limited to a box
     smaller than the area the window vacated -- and then expects the
     verifier to report it, so "0 bugs" can be distinguished from "the
     harness is not actually checking anything" -- which is exactly the
     mistake this project's testing notes warn about most often. It used
     to merely EXPECT a violation, which passed for as long as the WM
     had a real one; the day the last was fixed the control reported the
     harness as broken. (Dropping the damage outright does not work: a
     frame with none is a full repaint, correct by construction.)

CAVEAT WORTH KNOWING
--------------------
Injected input enters BELOW the PS/2 driver (see tools/gui_debug.py), so
a clean run says nothing about the real mouse path. It also cannot prove
the ABSENCE of damage bugs -- only that this sequence found none. When
you change drawing code, add the interaction you changed to SEQUENCE
rather than trusting the existing walk to cover it.
"""

import argparse
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
APPS = ("Terminal", "Calculator", "Notepad")


class Sweep:
    def __init__(self, dbg, verbose=False):
        self.dbg = dbg
        self.verbose = verbose
        self.hits = []
        self.voided = []
        self.skipped = []   # fixtures that were ABSENT, reported not swallowed
        self.steps = 0

    def step(self, label, command):
        """Run one interaction, then collect any violation it produced."""
        self.steps += 1
        if self.verbose:
            print(f"  {label}: {command}")
        self.dbg.send(command)
        self.dbg.settle()
        # Void verdicts first, so they stay VISIBLE rather than being
        # silently dropped -- a report the WM could not conclude
        # anything from is still worth seeing when hunting a real one.
        # They do not count toward the exit status.
        for line in self.dbg.damage_bugs_void(clear=False):
            if line not in self.voided:
                self.voided.append(line)
                if self.verbose:
                    print(f"  (void) [{label}] {line}")
        for line in self.dbg.damage_bugs():
            print(f"  DAMAGE BUG [{label}] {line}")
            self.hits.append((label, line))

    # -- geometry, always re-read (see the module docstring's point 2) --

    def windows(self):
        return self.dbg.json("gui windows --json")["windows"]

    def visible(self):
        return [w for w in self.windows() if w["state"] != "minimized"]

    def by_title(self, title):
        for w in self.windows():
            if w["title"] == title:
                return w
        return None


def run_sequence(sw, inject=False):
    dbg = sw.dbg

    # Open several overlapping windows -- overlap is what makes a missed
    # declaration visible at all.
    for app in APPS:
        sw.step(f"open {app}", f"gui open {app}")
    if inject:
        # THE POSITIVE CONTROL: window damage is shrunk for a few frames,
        # so the drag below moves a window whose vacated border the
        # frame's damage box does not include. A verifier that cannot see
        # this sees nothing. A few frames, not one: the drag's first
        # rendered frame is a cursor-only move.
        w = sw.visible()[-1]
        dbg.send("gui damage shrink 4")
        sw.step(f"injected miss: drag {w['title']}",
                f"gui drag {w['x'] + w['w'] // 2} {w['y'] + 12} "
                f"{w['x'] + w['w'] // 2 + 60} {w['y'] + 12 + 40}")

    # Raise each in turn: z-order churn with no geometry change, which
    # compute_window_damage() cannot see on its own.
    for title in [w["title"] for w in sw.visible()]:
        w = sw.by_title(title)
        sw.step(f"raise {title}", f"gui click {w['x'] + w['w'] // 2} {w['y'] + 12}")

    # Drag each window away and back, so windows are revealed as well as
    # covered.
    for title in [w["title"] for w in sw.visible()]:
        w = sw.by_title(title)
        sw.step(f"drag-out {title}",
                f"gui drag {w['x'] + w['w'] // 2} {w['y'] + 12} "
                f"{w['x'] + w['w'] // 2 + 170} {w['y'] + 12 + 80}")
        w = sw.by_title(title)
        sw.step(f"drag-back {title}",
                f"gui drag {w['x'] + w['w'] // 2} {w['y'] + 12} "
                f"{w['x'] + w['w'] // 2 - 170} {w['y'] + 12 - 80}")

    # Minimize and restore via the taskbar: visibility flips, and the
    # frontmost button's tint changes with it.
    for b in dbg.json("gui taskbar --json")["buttons"]:
        sw.step(f"minimize {b['title']}", f"gui click {b['cx']} {b['cy']}")
        sw.step(f"restore {b['title']}", f"gui click {b['cx']} {b['cy']}")

    # Overlays: the Start menu paints outside any window's rect.
    start = dbg.json("gui taskbar --json")["start"]
    sw.step("start-menu open", f"gui click {start['cx']} {start['cy']}")
    sw.step("start-menu dismiss", "gui click 940 300")

    # THE TRAY'S VOLUME PANEL, WHICH RESIZES WHILE IT IS OPEN -- the one
    # overlay whose geometry changes with no input at all. Its rows come
    # from two live sources: the sound devices (a DAC plugged in or out)
    # and the per-application roster (anything that opens the stream), so
    # it grows and shrinks under an open panel and has to damage what the
    # LARGER one covered. The maintainer photographed a sliver left
    # behind by exactly this on 2026-09-22 (docs/bugs.md); nothing in
    # this sweep touched the tray until then.
    vol = dbg.json("gui volume --json")
    sw.step("volume-panel open", f"gui click {vol['tray']['cx']} {vol['tray']['cy']}")

    # An application row APPEARS: the Audio Player registers as a sound
    # client when it starts, so the open panel gains a row and grows
    # upward. Opened while the panel is up on purpose -- the resize with
    # nobody touching the panel is the case that was never covered.
    sw.step("volume-panel app row appears", "gui open Audio Player")

    # **AND THE FIXTURE IS CHECKED, because a machine with no sound
    # hardware grows NO ROW and the steps below would pass without
    # testing anything.** `vm.py` attaches no card unless asked
    # (--audio-wav/--audio), and the first version of this reported a
    # clean sweep against a panel that never resized at all.
    after = dbg.json("gui volume --json")
    if not after.get("apps"):
        sw.skipped.append(
            "volume-panel resize: no application row appeared -- this guest has "
            "no sound card, so the panel never changed size. Start the VM with "
            "`vm.py --audio-wav <path> --audio both start` to cover it")
    else:
        # ...and GOES AWAY again, which is the direction that leaves a
        # residue: the panel shrinks and something has to cover the rest.
        # Closed BY TITLE -- the topmost window is not reliably the one
        # just opened, and closing the wrong one removes no row.
        ws = sw.windows()
        idx = next((i for i, w in enumerate(ws)
                    if w["title"] == "Audio Player"), None)
        if idx is not None:
            sw.step("volume-panel app row goes", f"gui close {idx}")

    # A device row click: it rewrites "Automatic (hda0)" to name the new
    # pick, so the panel's WIDTH moves under the pointer.
    vol = dbg.json("gui volume --json")
    if vol.get("open") and len(vol.get("devices", [])) > 1:
        d = vol["devices"][-1]
        sw.step(f"volume-panel pick {d['label']}", f"gui click {d['cx']} {d['cy']}")
    else:
        sw.skipped.append(
            "volume-panel device pick: fewer than two device rows -- nothing to "
            "switch between, so the label-width change was not exercised")

    sw.step("volume-panel dismiss", "gui click 940 300")

    # Resize via the grip, both directions.
    for title in [w["title"] for w in sw.visible() if w["resizable"]]:
        w = sw.by_title(title)
        gx, gy = w["x"] + w["w"] - 4, w["y"] + w["h"] - 4
        sw.step(f"resize-grow {title}", f"gui drag {gx} {gy} {gx + 110} {gy + 70}")
        w = sw.by_title(title)
        gx, gy = w["x"] + w["w"] - 4, w["y"] + w["h"] - 4
        sw.step(f"resize-shrink {title}", f"gui drag {gx} {gy} {gx - 110} {gy - 70}")

    # Close from the top of the z-order down: each close hands focus to
    # the window underneath, whose title bar changes without moving.
    while True:
        ws = sw.windows()
        if not ws:
            break
        sw.step(f"close {ws[-1]['title']}", f"gui close {len(ws) - 1}")


def run_random(sw, count, seed):
    """A seeded random walk over the same interactions.

    The fixed sequence above covers the interactions somebody thought to
    list. This covers the ORDERS nobody thought to list, which is where
    the remaining bugs of this family live -- a missed declaration
    usually needs one specific window to be in one specific place
    relative to another, and a scripted walk visits very few such
    arrangements. Seeded so a failure is replayable: the seed is printed
    on every run and `--seed N` reproduces it exactly.
    """
    rng = random.Random(seed)
    print(f"damage_sweep: random walk, {count} interactions, seed {seed}")

    for n in range(count):
        ws = sw.windows()
        vis = [w for w in ws if w["state"] != "minimized"]
        choices = ["open", "raise", "drag", "minimize", "resize", "menu", "close"]
        if len(ws) >= 5:
            choices.remove("open")
        if not vis:
            choices = ["open"]
        what = rng.choice(choices)

        if what == "open":
            sw.step(f"[{n}] open", f"gui open {rng.choice(APPS)}")
        elif what == "raise":
            w = rng.choice(vis)
            sw.step(f"[{n}] raise {w['title']}",
                    f"gui click {w['x'] + w['w'] // 2} {w['y'] + 12}")
        elif what == "drag":
            w = rng.choice(vis)
            dx, dy = rng.randint(-260, 260), rng.randint(-120, 200)
            sw.step(f"[{n}] drag {w['title']} by ({dx},{dy})",
                    f"gui drag {w['x'] + w['w'] // 2} {w['y'] + 12} "
                    f"{w['x'] + w['w'] // 2 + dx} {w['y'] + 12 + dy}")
        elif what == "minimize":
            bs = sw.dbg.json("gui taskbar --json")["buttons"]
            if not bs:
                continue
            b = rng.choice(bs)
            sw.step(f"[{n}] taskbar {b['title']}", f"gui click {b['cx']} {b['cy']}")
        elif what == "resize":
            rs = [w for w in vis if w["resizable"]]
            if not rs:
                continue
            w = rng.choice(rs)
            gx, gy = w["x"] + w["w"] - 4, w["y"] + w["h"] - 4
            dx, dy = rng.randint(-160, 200), rng.randint(-140, 160)
            sw.step(f"[{n}] resize {w['title']} by ({dx},{dy})",
                    f"gui drag {gx} {gy} {gx + dx} {gy + dy}")
        elif what == "menu":
            start = sw.dbg.json("gui taskbar --json")["start"]
            sw.step(f"[{n}] start-menu", f"gui click {start['cx']} {start['cy']}")
            sw.step(f"[{n}] start-dismiss",
                    f"gui click {rng.randint(700, 1200)} {rng.randint(200, 600)}")
        elif what == "close":
            i = rng.randrange(len(ws))
            sw.step(f"[{n}] close {ws[i]['title']}", f"gui close {i}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM is already showing the desktop; don't type `gui` first")
    ap.add_argument("--positive-control", action="store_true",
                    help="INVERT the exit code: expect at least one violation. "
                         "Run against a kernel with a damage declaration deliberately "
                         "removed to prove this harness actually detects one.")
    ap.add_argument("--random", type=int, metavar="N", default=0,
                    help="after the fixed sequence, run N randomised interactions "
                         "(covers the ORDERS the fixed walk doesn't)")
    ap.add_argument("--seed", type=int, default=None,
                    help="seed for --random; printed on every run so a failure replays")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "damage_sweep")

    if not args.in_gui:
        qmp = QMPSession(port=args.qmp_port)
        enter_gui(qmp, sock=args.sock)   # types nothing if the desktop is up

    dbg = DebugConsole(args.sock)
    dbg.damage_verify(True)
    dbg.damage_bugs()  # discard anything from before we started

    sw = Sweep(dbg, verbose=args.verbose)
    try:
        run_sequence(sw, inject=args.positive_control)
        if args.random:
            seed = args.seed if args.seed is not None else random.randrange(1 << 30)
            run_random(sw, args.random, seed)
    finally:
        dbg.damage_verify(False)

    print(f"\ndamage_sweep: {sw.steps} interactions, {len(sw.hits)} distinct "
          f"violation(s), {len(sw.voided)} report(s) the WM declared void")
    # NOT SILENT: a step whose fixture was missing tested nothing, and a
    # clean run that skipped it is weaker evidence than it looks.
    for why in sw.skipped:
        print(f"  NOT COVERED: {why}")
    for label, line in sw.hits:
        print(f"  {label}: {line}")

    if args.positive_control:
        if sw.hits:
            print("positive control PASSED -- the sweep detects a real violation")
            return 0
        print("positive control FAILED -- the sweep reported nothing against a "
              "kernel that should be failing; it is not checking anything")
        return 1
    return 1 if sw.hits else 0


if __name__ == "__main__":
    sys.exit(main())
