#!/usr/bin/env python3
"""Drag a client window's resize grip repeatedly, printing BOTH views of
the window after each drag.

**WHY BOTH.** A window-protocol bug is usually the compositor and the
window server disagreeing rather than either being wrong alone -- so
this prints `guictl windows` (the compositor's list) beside `lswin` (the
kernel's, per buffer). Reading one of them was how three wrong
hypotheses about a resize bug got written in an afternoon.

**WHY A DRAG RATHER THAN `guictl resize`.** They are not the same path:
`guictl resize` proposes a size directly, while a drag proposes one the
WM computes from the pointer. A resize bug that reproduces only under
dragging is invisible to the simpler call, which is exactly the case
this was written for.

    python3 tools/window_resize_probe.py --instance auto
    python3 tools/window_resize_probe.py --drags 4 --dx 60 --dy 40
"""
import argparse
import sys
import time

import port_guard
import qmp_test
from gui_debug import DebugConsole, enter_gui


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--app", default="/tests/winclient",
                    help="binary to spawn (default: the TWP test client)")
    ap.add_argument("--title", default="Ring 3 Client")
    ap.add_argument("--drags", type=int, default=3)
    ap.add_argument("--dx", type=int, default=40)
    ap.add_argument("--dy", type=int, default=28)
    # THE MODE IS AN AXIS, not a detail: `desktop.resize_mode` decides
    # whether the WM repaints the window live during a drag or draws an
    # outline and proposes once on release, and a resize bug that only
    # appears in one of them is invisible to the other.
    ap.add_argument("--steps", type=int, default=12,
                    help="pointer steps per drag; more means more proposals")
    ap.add_argument("--mode", default=None,
                    choices=["live", "outline", "auto"],
                    help="desktop.resize_mode to set before dragging")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "window_resize_probe")

    qmp = qmp_test.QMPSession(port=args.qmp_port)
    enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    if args.mode:
        dbg.send(f"sh config set desktop.resize_mode {args.mode}")
        time.sleep(0.5)
    dbg.spawn(args.app)
    time.sleep(1.5)

    # **REFUSED, NOT GUESSED, WHEN A TITLE IS AMBIGUOUS.** Two clients
    # with one title -- a previous run's still open -- made this compare
    # one window's compositor view against another's kernel row and
    # report a disagreement that was the probe's own. Twice. A tool
    # whose failure mode looks exactly like the bug it hunts has to say
    # so instead of picking.
    matches = [w for w in dbg.windows() if w["title"] == args.title]
    if len(matches) != 1:
        print(f"window_resize_probe: {len(matches)} windows titled "
              f"{args.title!r} -- need exactly one. Restart the guest, or "
              f"pass --title.")
        return 1
    win = matches[0]
    main.prev_asked = dbg.state().get("resizes_asked", 0)

    print(f"{'drag':>4}  {'asked':>11}  {'compositor':>11}  {'kernel':>11}  "
          f"{'comp buf':>9}  {'lag':>5} {'paint':>7} {'props':>5}  kernel buffers")
    for n in range(args.drags + 1):
        if n:
            win = dbg.window(args.title)
            if not win:
                print(f"{n:>4}  window gone")
                return 1
            # The GRIP is the bottom-right corner of the FRAME, which is
            # what a person drags. Taken from the WM's own geometry
            # rather than computed here -- a hardcoded offset is the
            # thing that rots when the chrome changes.
            # **INJECTED INPUT, NOT THE REAL MOUSE.** dbg.drag() pushes
            # events the WM drains as fast as it iterates, so a live
            # drag proposes many times; qmp.drag() moves the physical
            # pointer, which the WM samples once a frame and which
            # therefore proposes ONCE however many steps it is given.
            # Using the wrong one made live and outline mode look
            # identical and hid the difference this tool exists to show.
            gx = win["x"] + win["w"] - 2
            gy = win["y"] + win["h"] - 2
            dbg.drag(gx, gy, gx + args.dx, gy + args.dy, steps=args.steps)
            dbg.settle()
            time.sleep(0.6)

        w = dbg.window(args.title)
        comp = f'{w["content"]["w"]}x{w["content"]["h"]}' if w else "gone"
        # WHICH OBJECT EACH SIDE IS ON. A window's buffer is a named shm
        # object whose contents are replaced on every resize, and the
        # GENERATION is which replacement -- so the compositor sitting a
        # generation behind the kernel is a re-open that did not happen,
        # which the sizes alone need not show.
        cbuf = w.get("buf") if w else None
        comp_gen = f'b{cbuf["front"]} g{cbuf["gen"]}' if cbuf else "?"
        # MATCHED BY PID, never "the first row". Two windows with the
        # same title -- a previous run's client still open -- would
        # otherwise compare one window's compositor view against
        # another's kernel row and report a disagreement that is the
        # PROBE's, which is exactly the class of bug this tool exists to
        # avoid producing.
        # `client_pid`, which is what the WM calls it. Reading a key
        # that is not there silently disabled this match once already.
        want_pid = w.get("client_pid") if w else None
        if w and want_pid is None:
            print("window_resize_probe: no client_pid in `gui windows` -- "
                  "cannot correlate the two views")
            return 1
        kern, bufs = "?", ""
        for l in dbg.send("sh lswin").splitlines():
            f = l.split()
            if len(f) < 6 or not f[0].isdigit():
                continue
            if want_pid is not None and int(f[0]) != want_pid:
                continue
            kern = f"{f[2]}x{f[3]}"
            bufs = " ".join(f[5:])
            break
        # THE WM'S OWN LAG MEASUREMENT, which is what `auto` mode
        # decides on: it watches how long a client takes to answer a
        # resize and falls back to an outline when that grows. A change
        # to the resize path that costs the client time shows up HERE
        # before it shows up as a failing check.
        st = dbg.state()
        lag = st.get("resize_lag_ms", "?")
        paint = st.get("resize_paint", "?")
        # PROPOSALS PER DRAG, as a delta. `resizes_asked` is cumulative,
        # so the interesting number is how many this drag caused: live
        # mode proposes continuously while the pointer moves and outline
        # proposes ONCE on release, so a live drag reporting one
        # proposal is behaving like an outline drag whatever the mode
        # says.
        asked_now = st.get("resizes_asked", 0)
        props = asked_now - main.prev_asked
        main.prev_asked = asked_now

        asked = "-" if not n else f"+{args.dx},+{args.dy}"
        flag = "" if comp == kern else "   <-- DISAGREE"
        print(f"{n:>4}  {asked:>11}  {comp:>11}  {kern:>11}  "
              f"{comp_gen:>9}  {lag:>5} {paint:>7} {props:>5}  {bufs}{flag}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
