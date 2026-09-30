#!/usr/bin/env python3
"""Drive userland/tests/compclient.c -- Milestone 41 stage 2's raw input
path to a registered ring-3 compositor.

WHAT IS ACTUALLY UNDER TEST
---------------------------
Two things that only mean something together:

  1. A ring-3 process can claim the compositor role (WIN_REQ_SET_
     COMPOSITOR) and receive the input stream the WM consumes, BEFORE
     focus and hit-testing -- WIN_EV_RAW_MOUSE/KEY/WHEEL.
  2. The real WM keeps routing that same input to the same windows while
     it does.

Check 2 is the load-bearing one and it is why UI Demo is opened here at
all. "The compositor received the click" is satisfied by an
implementation that stole the input stream outright, which would be a
regression dressed as a feature -- stage 2 explicitly runs both paths
alongside each other so stage 4 is a deletion rather than a cutover. So
every input this tool injects is asserted TWICE: once in compclient's
log and once in UI Demo's.

The other assertion worth naming is the idle one. The WM loop runs on
every timer tick and the event queue is 32 deep dropping the oldest, so
an implementation that pushed level state unconditionally would report
drops climbing while nobody touched anything. `gui compositor --json`
exposes that count precisely so it can be asserted at zero.

compclient logs to STDERR, which the kernel routes into the kernel log,
so DebugConsole.logs() can read it -- unlike a client's stdout, which
goes to its parent's pipe (see uiclient_test.py's docstring for the trap
in the other direction).

TWO WORLDS, PICKED BY ASKING WHO HOLDS THE ROLE
-----------------------------------------------
The design above is stage 2's, and stage 2's whole point was that BOTH
paths run at once so stage 4 is a deletion rather than a cutover. Once
the desktop itself is the ring-3 compositor that premise is gone: the
role is single, so spawning a stand-in EVICTS the desktop and every
`gui` command afterwards is answered by a client that does not implement
them. (That is exactly how this tool failed under `gui3`.)

So when a desktop already holds the role, this asserts the thing that
actually matters there and that nothing else covers: **real hardware
input reaching a window through the ring-3 desktop.** Every other GUI
tool injects with `gui click`, which enters the WM loop BELOW the PS/2
driver -- so the whole chain from a real interrupt through
`win_input.c`, `WIN_EV_RAW_*` and the desktop's hit-testing is exercised
by no test at all. Here it is driven with a real QMP click on a button
whose rect comes from UI Demo's own layout line.

Usage (the VM must already be up and in GUI mode):

    python3 tools/vm.py start
    python3 tools/compositor_test.py
    python3 tools/vm.py stop
"""

import argparse
import os
import sys
import tempfile
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession             # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
SPAWN_PATH = "/tests/compclient"
UIDEMO_TITLE = "UI Demo"



_res = Results()
check = _res.check
checks = _res.rows


def comp(dbg):
    """`gui compositor --json` -> dict. The only view of the second
    consumer -- every other `gui` subcommand reports the WM's own state,
    and a compositor is by definition another process."""
    return dbg.json("gui compositor --json")


def comp_lines(dbg, kind=None):
    """compclient's log lines since the last read, optionally one kind.

    logs() clears what it returns, so each call is 'since last time' --
    which is what makes 'this click produced these events' assertable
    rather than 'the log contains a mouse line somewhere'."""
    lines = [l for l in dbg.logs("compclient:") if "compclient:" in l]
    if kind:
        lines = [l for l in lines if f"compclient: {kind} " in l]
    return lines


def parse_mouse(line):
    """'compclient: mouse 300 400 1' -> (300, 400, 1)."""
    tail = line.split("compclient: mouse ", 1)[1].split()
    return int(tail[0]), int(tail[1]), int(tail[2])


def summarise():
    passed = sum(1 for _, ok, _ in checks if ok)
    failed = len(checks) - passed
    # gui_regress.py pulls the table's summary out of a line matching
    # "passed," and "failed" -- match the other tools' shape or the row
    # falls back to whatever the last line happened to be.
    print(f"\ncompositor_test: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


def uidemo_button(dbg):
    """Open UI Demo and return (window, btn1 rect) -- or (None, None).

    The rect comes from the app's own `uidemo: layout btn1 x y w h`
    line, never from a Python copy of a widget offset: those drift the
    moment a row is added to the app, which has bitten four tools here.
    """
    dbg.open_app(UIDEMO_TITLE)
    dbg.settle()
    win = dbg.window(UIDEMO_TITLE)
    if win is None:
        return None, None
    btn = None
    for line in dbg.logs("uidemo:"):
        if "uidemo: layout btn1 " in line:
            f = line.split("uidemo: layout btn1 ", 1)[1].split()
            btn = tuple(int(v) for v in f[:4])
    return win, btn


def run_ring3(dbg, qmp, owner):
    """The desktop IS the compositor: assert the REAL input chain.

    PS/2 interrupt -> win_input.c -> WIN_EV_RAW_* -> the ring-3 desktop
    -> hit-testing -> the client. Every other tool injects below the
    driver with `gui click`, so this is the only place the hardware half
    is exercised.
    """
    check("the desktop holds the compositor role", owner > 0, f"pid={owner}")

    win, btn = uidemo_button(dbg)
    if win is None or btn is None:
        check("UI Demo opened and reported btn1's layout", False,
              "no window" if win is None else "no layout line")
        return summarise()
    check("UI Demo opened and reported btn1's layout", True, f"btn1={btn}")

    # Idle must not flood the queue. The desktop's loop runs every tick,
    # and a compositor that pushed level state unconditionally would
    # overflow a 32-deep queue in a fraction of a second -- so a drop
    # count of zero after a second of nothing is the check that a
    # change-gate exists at all.
    time.sleep(1.0)
    idle = comp(dbg)
    check("nothing is dropped while idle", idle.get("dropped", -1) == 0,
          f"dropped={idle.get('dropped')}")

    content = win["content"]
    cx = content["x"] + btn[0] + btn[2] // 2
    cy = content["y"] + btn[1] + btn[3] // 2

    # A REAL click: warp the actual PS/2 cursor onto the button and
    # press. warp_cursor() confirms arrival through `gui state` rather
    # than trusting an open-loop move, which undershoots a large jump.
    dbg.logs()
    dbg.warp_cursor(qmp, cx, cy)
    # press/release WITHOUT re-positioning: QMPSession.click() does its
    # own goto(), which is open-loop and undershoots a large jump -- it
    # moved the cursor from the button to (0,129) here, and the click
    # then landed on the desktop while the press itself was delivered
    # perfectly. warp_cursor() is the one that confirms arrival.
    qmp.mouse_down()
    time.sleep(0.2)
    qmp.mouse_up()
    time.sleep(0.8)
    dbg.settle()
    real = [l for l in dbg.logs("uidemo:") if "uidemo: button 1" in l]
    check("a REAL hardware click reaches the window through the desktop",
          len(real) >= 1,
          f"{len(real)} button line(s) at ({cx},{cy})")

    # ...and the injected path still works, which is what every other
    # tool depends on. Asserting both is the same pairing the ring-0
    # scenario makes: either alone is satisfied by a broken half.
    dbg.logs()
    dbg.click(cx, cy)
    dbg.settle()
    injected = [l for l in dbg.logs("uidemo:") if "uidemo: button 1" in l]
    check("the injected `gui click` path still reaches it too",
          len(injected) >= 1, f"{len(injected)} button line(s)")

    # And the desktop was not evicted by any of it.
    end = comp(dbg)
    check("the desktop still holds the role afterwards",
          end.get("pid", 0) == owner, f"pid={end.get('pid')}")

    return summarise()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop; don't type `gui` first")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "compositor_test")

    # Needed either way now: the framebuffer-grant checks read pixels off
    # the real screen rather than believing the client's own log.
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)

    owner = comp(dbg).get("pid", 0)
    if owner > 0:
        print(f"compositor -- the RING-3 desktop holds the role (pid {owner}):")
        return run_ring3(dbg, qmp, owner)

    print("compositor (M41 stage 2)")

    # --- before anything: no compositor ------------------------------
    # Also the negative half of check 'registers': "pid is nonzero" means
    # nothing unless it was zero a moment earlier.
    before = comp(dbg)
    check("no compositor before spawn", before.get("pid", -1) == 0,
          f"pid={before.get('pid')}")

    # --- a window whose routing must survive -------------------------
    dbg.open_app(UIDEMO_TITLE)
    dbg.settle()
    demo = dbg.window(UIDEMO_TITLE)
    if demo is None:
        check("UI Demo opened", False, "no window -- cannot test coexistence")
        return 1
    check("UI Demo opened", True)

    # Take the button's rect from the app's own `uidemo: layout btn1 x y
    # w h` line rather than re-deriving it here -- a Python copy of a
    # widget offset drifts silently the moment a row is added to the app,
    # which has bitten four tools in this repo already.
    btn = None
    for line in dbg.logs("uidemo:"):     # also drops the startup noise
        if "uidemo: layout btn1 " in line:
            f = line.split("uidemo: layout btn1 ", 1)[1].split()
            btn = tuple(int(v) for v in f[:4])
    if btn is None:
        check("UI Demo reported btn1's layout", False, "no layout line")
        return 1
    check("UI Demo reported btn1's layout", True, f"btn1={btn}")

    # --- register ----------------------------------------------------
    dbg.spawn(SPAWN_PATH)          # no window to wait for -- it has none
    time.sleep(0.5)
    reg = comp_lines(dbg)
    # Kept for the framebuffer-grant group below: the pre-registration
    # refusal is logged here, and comp_lines() consumes what it returns.
    start_lines = list(reg)
    check("client reports registered",
          any("registered" in l for l in reg),
          f"{len(reg)} line(s)")

    after = comp(dbg)
    check("kernel reports a compositor pid", after.get("pid", 0) > 0,
          f"pid={after.get('pid')}")

    # --- idle must not flood -----------------------------------------
    # The WM loop runs every tick. Pushing level state unconditionally
    # would overflow a 32-deep queue in a fraction of a second, so this
    # is the check that a change-gate exists at all.
    dbg.logs()
    time.sleep(1.0)
    idle = comp_lines(dbg, "mouse")
    idle_state = comp(dbg)
    check("idle produces no mouse events", len(idle) == 0, f"{len(idle)} line(s)")
    check("nothing dropped while idle", idle_state.get("dropped", -1) == 0,
          f"dropped={idle_state.get('dropped')}")

    # --- a click reaches BOTH consumers -------------------------------
    content = demo["content"]
    cx = content["x"] + btn[0] + btn[2] // 2
    cy = content["y"] + btn[1] + btn[3] // 2
    dbg.logs()
    dbg.click(cx, cy)
    time.sleep(0.4)

    lines = comp_lines(dbg, "mouse")
    hit = [parse_mouse(l) for l in lines]
    check("compositor saw the click position",
          any(x == cx and y == cy for x, y, _ in hit),
          f"{len(hit)} mouse event(s)")
    check("compositor saw a button press",
          any(b & 1 for _, _, b in hit),
          f"buttons seen: {sorted({b for _, _, b in hit})}")
    # SCREEN coordinates, not window-relative -- that is the whole
    # difference between raw input and the routed WIN_EV_MOUSE_* a client
    # gets. If these came back window-relative they would be small.
    check("coordinates are screen-absolute",
          all(x >= content["x"] for x, _, _ in hit) if hit else False,
          f"content x={content['x']}")

    # --- ...and the WM still routed it -------------------------------
    demo_lines = [l for l in dbg.logs("uidemo:") if "uidemo:" in l]
    check("UI Demo still received the click (both paths live)",
          len(demo_lines) > 0,
          f"{len(demo_lines)} uidemo line(s)")

    # --- a key ---------------------------------------------------------
    dbg.logs()
    dbg.key("a")
    time.sleep(0.4)
    keys = comp_lines(dbg, "key")
    check("compositor saw a keypress", len(keys) > 0, f"{len(keys)} line(s)")

    # --- a wheel notch -------------------------------------------------
    dbg.logs()
    dbg.wheel(-1)
    time.sleep(0.4)
    wheels = comp_lines(dbg, "wheel")
    check("compositor saw a wheel notch", len(wheels) > 0, f"{len(wheels)} line(s)")

    # --- the framebuffer grant (M41 stage 4a) --------------------------
    #
    # WHAT THIS CAN AND CANNOT ASSERT, because getting it wrong wasted
    # real time here. The obvious check -- paint a block and find it in a
    # screenshot -- is WRONG in stage 4a, and it fails against a working
    # kernel. The ring-0 WM still owns the screen and repaints it, so a
    # ring-3 write survives until the WM's next frame and no longer;
    # measured directly, the framebuffer reads back as the written colour
    # immediately after the write and as the desktop again a moment
    # later. Both compositors writing one screen is the whole shape of
    # this stage. So the assertions below are about the MAPPING being the
    # real framebuffer, verified from two independent sides, and the
    # transience is asserted deliberately rather than fought.

    state = dbg.json("gui state --json") or {}

    # Refused to a non-compositor. Logged at startup, before registering.
    check("the grant is refused to a non-compositor",
          any("fb-unregistered refused" in l for l in start_lines),
          "granted!" if any("fb-unregistered GRANTED" in l for l in start_lines)
          else "refused")

    dbg.logs()
    dbg.key("f")
    time.sleep(0.5)
    fb = [l for l in comp_lines(dbg) if "compclient: fb " in l]
    check("the compositor is granted the framebuffer", len(fb) > 0,
          fb[0].strip() if fb else "no fb line")

    # Geometry has to match what the KERNEL says the screen is: "it
    # returned four numbers" is satisfied by four zeros.
    geom_ok = False
    probe_val = None
    detail = "no fb line"
    if fb:
        parts = fb[0].split("compclient: fb ")[1].split()
        try:
            w, h, pitch, bpp = (int(p) for p in parts[:4])
            probe_val = parts[5] if len(parts) > 5 else None
            screen = state.get("screen", {})
            geom_ok = (w == screen.get("w") and h == screen.get("h")
                       and pitch >= w * (bpp // 8) and bpp in (24, 32))
            detail = f"{w}x{h} pitch={pitch} bpp={bpp} vs kernel {screen}"
        except (ValueError, IndexError):
            detail = fb[0].strip()
    check("its geometry matches the kernel's own screen", geom_ok, detail)

    # THE LOAD-BEARING ONE, and the reason it is worth its length: the
    # client reports the pixel it reads at (100,100) THROUGH the new
    # mapping, and QEMU's screendump reports the same pixel from the
    # display's side. Two independent observers of one address. A
    # mapping pointed at any other memory -- zeroed frames, the wrong
    # physical base, someone else's buffer -- cannot produce agreement
    # here, and every other check in this group would still pass.
    shot = os.path.join(tempfile.gettempdir(), "compfb.png")
    qmp.screenshot(shot)
    px = Image.open(shot).convert("RGB")
    seen = px.getpixel((100, 100))
    seen_hex = f"{seen[0]:02x}{seen[1]:02x}{seen[2]:02x}"
    check("the mapping reads the REAL screen (client vs screendump)",
          probe_val is not None and probe_val.lstrip("0").lower() == seen_hex.lstrip("0").lower(),
          f"client={probe_val} screendump={seen_hex}")

    # And a write through it lands, read back through the same mapping.
    dbg.logs()
    dbg.key("p")
    time.sleep(0.5)
    painted = [l for l in comp_lines(dbg) if "painted" in l]
    check("present succeeds and the write is in the framebuffer",
          any("painted 1 readback ff00ff" in l for l in painted),
          painted[0].strip() if painted else "no painted line")

    # The transience, asserted on purpose: the ring-0 WM repaints over a
    # ring-3 write, which is correct while both own the screen. When the
    # WM becomes the ring-3 compositor in 4b this check is what should
    # change -- so it failing later is a signal, not a flake.
    dbg.key("r")
    time.sleep(0.5)
    reread = [l for l in comp_lines(dbg) if "reread" in l]
    check("the ring-0 WM still owns the screen (write is transient)",
          any("reread" in l and "ff00ff" not in l for l in reread),
          reread[0].strip() if reread else "no reread line")

    # --- release -------------------------------------------------------
    # 'q' quits compclient. It owns no window, so there is no close
    # button to click and no wm_request_close() to go through -- which is
    # itself the reason the client has a quit key.
    dbg.logs()
    dbg.key("q")
    time.sleep(0.8)
    out = comp_lines(dbg)
    check("client released the role", any("released" in l for l in out),
          f"{len(out)} line(s)")

    gone = comp(dbg)
    check("kernel reports no compositor after release",
          gone.get("pid", -1) == 0, f"pid={gone.get('pid')}")

    # --- and the desktop is unharmed ----------------------------------
    dbg.logs()
    dbg.click(cx, cy)
    time.sleep(0.3)
    still = [l for l in dbg.logs("uidemo:") if "uidemo:" in l]
    check("UI Demo still works after the compositor left", len(still) > 0,
          f"{len(still)} uidemo line(s)")

    return summarise()


if __name__ == "__main__":
    sys.exit(main())
