#!/usr/bin/env python3
"""The taskbar's brightness flyout: the slider, the wheel, and the
honest answer on a machine with no backlight.

WHAT IS UNDER TEST
------------------
Clicking the tray's sun icon opens a panel above it
(userland/wm/brightness_popup.c) carrying one slider. It owns no
display state: it reads and writes `system.brightness` over SYS_SETTING,
and the registered setting answers `unavailable` with a sentence on a
display whose driver has no DISPLAY_CAP_BACKLIGHT -- which is every
QEMU adapter. So this tool exercises the DEGRADED path, the one
player_test.py takes for sound, and says so.

WHAT IT ASSERTS
---------------
1. IT IS DRAWN, not merely flagged open: opening and closing are paired
   with a pixel comparison of the panel's rect, and closing restores
   what was underneath.
2. THE SENTENCE IS SHOWN, and it is the kernel's: `gui brightness`
   reports what `config` refuses with, and they must agree -- the panel
   is not allowed to invent its own reason.
3. THE SLIDER AND THE WHEEL DO NOT WRITE when unavailable. A click at
   40% of the track must leave `config get brightness` where it was,
   and so must a notch over the tray icon -- a build that skipped the
   availability check would push a value at a registry that refuses it
   and report a level the hardware never took.
4. THE WHEEL IS STILL SCOPED: a notch over the desktop reaches the
   focused window, not this panel (the pointer is WARPED, never
   `gui move`).
5. THE START MENU CLOSES IT, as with every popup.

The positive half -- the slider dimming a real panel -- is reachable
only on hardware with a backlight (the bare-metal laptop), and is
verified there by `config get brightness` reading the PWM back.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/brightness_test.py
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

DEFAULT_SOCK = ".vm.serial"

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return bool(ok)


def bri(dbg):
    return dbg.json("gui brightness --json")


def panel_box(g):
    return (g["x"], g["y"], g["x"] + g["w"], g["y"] + g["h"])


def setting(dbg, name):
    reply = (dbg.send(f"sh config get {name}") or "").strip()
    return next((ln.strip() for ln in reply.splitlines() if ln.strip()), "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "brightness_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.join(outdir, n)  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("brightness flyout (tray sun -> slider; unavailable in QEMU)")

    if bri(dbg)["open"]:
        dbg.send("gui click 640 300")
        dbg.settle(); time.sleep(0.3)

    g = bri(dbg)
    if not check("the tray reports a brightness item to click",
                 g["tray"]["w"] > 0, str(g["tray"])):
        return report()
    stored = setting(dbg, "brightness")
    check("the closed popup reports the setting's level",
          stored.isdigit() and int(stored) == g["level"] and not g["open"],
          f'level={g["level"]} config={stored!r}')

    # --- the honest answer on a machine with no backlight -------------
    # The sentence is the KERNEL's: `config set` must refuse with the
    # same words the panel shows, or the panel is inventing a reason.
    refusal = (dbg.send("sh config set brightness 50") or "")
    check("this guest has no backlight, and the panel says so",
          not g["available"] and g["unavailable"], g["unavailable"])
    check("...with the registry's own sentence",
          g["unavailable"] and g["unavailable"] in refusal,
          refusal.strip().splitlines()[-1] if refusal.strip() else "no reply")

    box = panel_box(g)
    before = qmp.stable_pixels(shot("bri_before.png"), box=box)

    # --- 1. it opens, AND it is drawn ---------------------------------
    dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    g = bri(dbg)
    check("clicking the tray icon opens it", g["open"])
    opened = qmp.stable_pixels(shot("bri_open.png"), box=box)
    check("...and the panel is actually PAINTED", opened != before,
          "identical pixels" if opened == before else "pixels changed")

    # --- 2. the slider does not write when unavailable ----------------
    target_x = g["slider"]["x"] + g["slider"]["w"] * 40 // 100
    dbg.send(f"gui click {target_x} {g['slider']['cy']}")
    dbg.settle(); time.sleep(0.8)
    after = bri(dbg)
    check("a click on the track leaves the level alone when unavailable",
          after["level"] == g["level"] and setting(dbg, "brightness") == stored,
          f'level {g["level"]} -> {after["level"]}, config {setting(dbg, "brightness")!r}')

    # --- 3. the wheel: scoped, and inert here -------------------------
    at = dbg.warp_cursor(qmp, g["tray"]["cx"], g["tray"]["cy"])
    if check("the pointer can be parked on the tray icon", at is not None, str(at)):
        dbg.send("gui wheel 1")
        dbg.settle(); time.sleep(0.5)
        check("a wheel notch over the tray icon leaves the level alone when unavailable",
              bri(dbg)["level"] == g["level"] and setting(dbg, "brightness") == stored,
              f'level={bri(dbg)["level"]}')
    at = dbg.warp_cursor(qmp, 400, 300)
    if check("the pointer can be parked on the desktop", at is not None, str(at)):
        dbg.send("gui wheel 3")
        dbg.settle(); time.sleep(0.5)
        check("a wheel notch AWAY from the tray leaves it alone too",
              bri(dbg)["level"] == g["level"], f'level={bri(dbg)["level"]}')

    # --- 4. it closes, and the pixels come back -----------------------
    dbg.send("gui click 640 300")
    dbg.settle(); time.sleep(0.4)
    check("a click on the desktop dismisses it", not bri(dbg)["open"])
    closed = qmp.stable_pixels(shot("bri_closed.png"), box=box)
    check("...and what was underneath is repainted", closed == before,
          "restored" if closed == before else "pixels differ")

    # --- 5. the Start menu and this one are mutually exclusive --------
    g = bri(dbg)
    dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
    dbg.settle(); time.sleep(0.3)
    if check("reopened for the Start-menu check", bri(dbg)["open"]):
        tb = dbg.json("gui taskbar --json")["start"]
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")
        dbg.settle(); time.sleep(0.3)
        st = dbg.json("gui state --json")["overlays"]
        check("opening the Start menu closes the brightness popup",
              st["start_menu"] is True and st["brightness"] is False, str(st))
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")
        dbg.settle()

    # --- 6. the two flyouts are mutually exclusive --------------------
    v = dbg.json("gui volume --json")
    dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
    dbg.settle(); time.sleep(0.3)
    dbg.send(f"gui click {v['tray']['cx']} {v['tray']['cy']}")
    dbg.settle(); time.sleep(0.3)
    st = dbg.json("gui state --json")["overlays"]
    check("opening the volume flyout closes the brightness one",
          st["volume"] is True and st["brightness"] is False, str(st))
    dbg.send("gui click 640 300")
    dbg.settle()
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nbrightness_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
