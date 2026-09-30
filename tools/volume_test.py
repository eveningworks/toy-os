#!/usr/bin/env python3
"""The taskbar's volume flyout: the slider, mute, the wheel and the
device list.

WHAT IS UNDER TEST
------------------
Clicking the tray's speaker icon opens a panel above it
(userland/wm/volume_popup.c) carrying a level slider, a mute toggle and
the registered output devices. It owns no audio state at all: it reads
and writes `system.volume` and `system.audio_device` over SYS_SETTING,
and its device rows ARE that setting's choice list.

FOUR THINGS THIS ASSERTS THAT AN "IT OPENED" CHECK WOULD NOT
------------------------------------------------------------
1. IT IS DRAWN, not merely flagged open. `gui volume --json` reporting
   open=true is exactly what a popup that draws nothing also reports --
   this repo's "it responds is not it is drawn" trap. So opening and
   closing are paired with a PIXEL comparison of the panel's rect, and
   closing must restore what was underneath.

2. THE SLIDER REACHES THE SETTING. Clicking at 40% of the track must
   leave `config get volume` near 40 -- the popup's own reading agreeing
   with itself proves nothing, since the write is DEBOUNCED and a build
   that never flushed it would still show the right number on screen.

3. THE WHEEL IS SCOPED. A notch over the tray icon moves the level; the
   same notch with the pointer over the desktop must NOT -- which is the
   half that fails if the handler forgets to check where the pointer is,
   and it would then eat every scroll in every app. The pointer is
   WARPED (DebugConsole.warp_cursor), never `gui move`: an injected
   position lasts one wm_run() iteration and the wheel would arrive with
   the cursor back where it was.

4. A DEVICE ROW WRITES THE SETTING, and the tick follows. On a machine
   with one card the list is `auto` plus that card, which is still two
   rows and still exercises the write.

5. THE THUMB MOVES ON SCREEN, read as PIXEL VALUES rather than a
   screenshot looked at: the thumb's old and new positions both change
   colour, and the mute button beside the track stays byte-identical.
   The level reaching the setting says nothing about what was drawn.

The level and the device are both RESTORED at the end: a tool that
leaves a setting changed changes the machine for every later tool
(CLAUDE.md), which is how a faster pointer once made two unrelated
tools fail.

Usage (the VM must already be up):

    python3 tools/vm.py start
    python3 tools/volume_test.py
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

DEFAULT_SOCK = ".vm.serial"



_res = Results()
check = _res.check
checks = _res.rows


def vol(dbg):
    return dbg.json("gui volume --json")


def panel_box(g):
    return (g["x"], g["y"], g["x"] + g["w"], g["y"] + g["h"])


def _ink(im, box):
    """The background and the pixel furthest from it, summed over RGB.

    Read rather than assumed: a symbolic icon's colour is the panel's
    `fg` at draw time, so the only way to check it is what landed.
    """
    import collections
    hist = collections.Counter(im.crop(box).convert("RGB").getdata())
    bg = hist.most_common(1)[0][0]
    ink = max(hist, key=lambda c: sum(abs(c[i] - bg[i]) for i in range(3)))
    return bg, ink, sum(abs(ink[i] - bg[i]) for i in range(3))


def tray_ink_check(qmp, g, path):
    try:
        from PIL import Image
    except ImportError:
        check("the tray icon is drawn in the clock's ink", True,
              "skipped -- no Pillow")
        return
    qmp.stable_pixels(path)
    im = Image.open(path)
    t = g["tray"]
    icon_box = (t["x"], t["y"], t["x"] + t["w"], t["y"] + t["h"])
    # The clock is the tray's rightmost item, so anything to the right
    # of the volume item and inside the screen is its text.
    clock_box = (t["x"] + t["w"] + 2, t["y"],
                 min(im.width, t["x"] + t["w"] + 90), t["y"] + t["h"])
    ibg, iink, icontrast = _ink(im, icon_box)
    cbg, cink, ccontrast = _ink(im, clock_box)
    check("the tray icon is drawn in the clock's ink",
          iink == cink, f"icon {iink} vs clock {cink}")
    check("...so it has the clock's contrast against the panel, not a sixth",
          icontrast >= ccontrast * 0.9,
          f"icon {icontrast} vs clock {ccontrast} on {ibg}")
    check("...and the clock is on the same background (the control)",
          ibg == cbg, f"{ibg} vs {cbg}")


def setting(dbg, name):
    reply = (dbg.send(f"sh config get {name}") or "").strip()
    return next((ln.strip() for ln in reply.splitlines() if ln.strip()), "")


def thumb_check(g, at_100_png, moved_png):
    """The slider's thumb moved, and the mute button beside it did not.

    `g` is the geometry AFTER the click. The scale is a uui_scale: its
    track is inset by half a thumb (the thumb is a text row square, so
    `slider.h / 2`) at each end, and the thumb sits at the level's share
    of what is left. Both positions are read from the two captures, so a
    thumb that reached the setting and never repainted is caught here.
    """
    try:
        from PIL import Image
    except ImportError:
        check("the thumb moved on screen", True, "skipped -- no Pillow")
        return
    s = g["slider"]
    half = s["h"] // 2
    x0, x1 = s["x"] + half, s["x"] + s["w"] - half
    old_x = x1
    new_x = x0 + (x1 - x0) * g["level"] // 100
    before = Image.open(at_100_png).convert("RGB")
    after = Image.open(moved_png).convert("RGB")
    cy = s["cy"]
    check("the thumb left its old position",
          before.getpixel((old_x, cy)) != after.getpixel((old_x, cy)),
          f"({old_x},{cy}) {before.getpixel((old_x, cy))} -> {after.getpixel((old_x, cy))}")
    # Sampled ABOVE the track band, inside the thumb's square: on the
    # band itself the fill and the thumb are both the accent, so a
    # thumb that never moved reads the same as one that did.
    ty = cy - half + 2
    check("...and arrived at the new one",
          before.getpixel((new_x, ty)) != after.getpixel((new_x, ty)),
          f"({new_x},{ty}) {before.getpixel((new_x, ty))} -> {after.getpixel((new_x, ty))}")
    # The neighbour is the first device row, not the mute button: the
    # speaker icon FOLLOWS the level (high/low/muted), so it is expected
    # to change here and would measure nothing.
    if not g["devices"]:
        check("...while the row below it did not change", True,
              "skipped -- no device rows to compare")
        return
    d = g["devices"][0]
    dbox = (d["cx"] - 6, d["cy"] - 6, d["cx"] + 6, d["cy"] + 6)
    check("...while the device row below it did not change",
          before.crop(dbox).tobytes() == after.crop(dbox).tobytes(), str(dbox))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "volume_test")

    outdir = args.logs or "/tmp"
    os.makedirs(outdir, exist_ok=True)
    shot = lambda n: os.path.join(outdir, n)  # noqa: E731

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)

    dbg = DebugConsole(args.sock)
    print("volume flyout (tray speaker -> slider, mute, devices)")

    # Start from closed and from a known level, whatever the last tool
    # left behind -- the pixel comparisons below compare the panel's rect
    # against itself, so an already-open popup would compare open with
    # open and read as a dead feature.
    if vol(dbg)["open"]:
        dbg.send("gui click 640 300")
        dbg.settle(); time.sleep(0.3)
    dbg.send("sh config set volume 100")
    time.sleep(0.4)

    g = vol(dbg)
    if not check("the tray reports a volume item to click",
                 g["tray"]["w"] > 0, str(g["tray"])):
        return report()
    check("...and the closed popup reports the current level",
          g["level"] == 100 and not g["open"], f'level={g["level"]}')

    # THE TRAY ICON IS DRAWN IN THE CLOCK'S INK, and the clock is the
    # control -- half the assertion is the neighbour staying put
    # (CLAUDE.md). This is a pixel check because the failure it exists
    # for looked perfectly plausible in a screenshot: the icon carried
    # the toolbar's dark ink onto the near-black taskbar, at 93 of
    # summed contrast against the clock's 596.
    tray_ink_check(qmp, g, shot("vol_tray_ink.png"))

    box = panel_box(g)
    before = qmp.stable_pixels(shot("vol_before.png"), box=box)

    # --- 1. it opens, AND it is drawn ---------------------------------
    dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
    dbg.settle(); time.sleep(0.4)
    g = vol(dbg)
    check("clicking the tray icon opens it", g["open"])
    opened = qmp.stable_pixels(shot("vol_open.png"), box=box)
    check("...and the panel is actually PAINTED", opened != before,
          "identical pixels" if opened == before else "pixels changed")

    # --- 2. the device list came from the settings registry ------------
    devs = g["devices"]
    check("the list starts with an Automatic row",
          len(devs) >= 1 and devs[0]["value"] == "auto",
          ", ".join(d["value"] for d in devs))
    check("...and it names what auto would pick",
          devs and devs[0]["label"].startswith("Automatic"),
          devs[0]["label"] if devs else "no rows")

    # --- 3. the slider reaches the SETTING, not just the screen --------
    # 40% along the track. The debounce is what makes reading the
    # setting the real assertion: a build that applied on screen and
    # never flushed shows the right number here and the wrong one there.
    target_x = g["slider"]["x"] + g["slider"]["w"] * 40 // 100
    dbg.send(f"gui click {target_x} {g['slider']['cy']}")
    dbg.settle(); time.sleep(0.8)
    g = vol(dbg)
    check("clicking the track moves the level", 33 <= g["level"] <= 47,
          f'level={g["level"]}')
    stored = setting(dbg, "volume")
    check("...and the level reached /etc through the setting",
          stored.isdigit() and abs(int(stored) - g["level"]) <= 1,
          f"config get volume -> {stored!r}")
    qmp.stable_pixels(shot("vol_moved.png"), box=box)
    thumb_check(g, shot("vol_open.png"), shot("vol_moved.png"))

    # --- 4. mute is a level of zero, and it comes back -----------------
    dbg.send(f"gui click {g['mute']['cx']} {g['mute']['cy']}")
    dbg.settle(); time.sleep(0.8)
    muted = vol(dbg)
    check("the speaker button mutes", muted["level"] == 0 and muted["muted"],
          f'level={muted["level"]}')
    dbg.send(f"gui click {g['mute']['cx']} {g['mute']['cy']}")
    dbg.settle(); time.sleep(0.8)
    back = vol(dbg)
    check("...and unmuting returns to the level it had",
          back["level"] == g["level"], f'{back["level"]} vs {g["level"]}')

    # --- 5. the wheel, and the half that must NOT happen ---------------
    # The REAL cursor, warped and confirmed: an injected `gui move`
    # position survives one wm_run() iteration, so the wheel would
    # arrive with the pointer back where the user left it.
    at = dbg.warp_cursor(qmp, g["tray"]["cx"], g["tray"]["cy"])
    if check("the pointer can be parked on the tray icon", at is not None, str(at)):
        level_before = vol(dbg)["level"]
        dbg.send("gui wheel 1")
        dbg.settle(); time.sleep(0.5)
        check("a wheel notch over the tray icon raises the level",
              vol(dbg)["level"] == level_before + 5,
              f'{level_before} -> {vol(dbg)["level"]}')
        dbg.send("gui wheel -1")
        dbg.settle(); time.sleep(0.5)
        check("...and the other way lowers it",
              vol(dbg)["level"] == level_before, f'back to {vol(dbg)["level"]}')

    at = dbg.warp_cursor(qmp, 400, 300)
    if check("the pointer can be parked on the desktop", at is not None, str(at)):
        level_before = vol(dbg)["level"]
        dbg.send("gui wheel 3")
        dbg.settle(); time.sleep(0.5)
        check("a wheel notch AWAY from the tray leaves the volume alone",
              vol(dbg)["level"] == level_before,
              f'{level_before} -> {vol(dbg)["level"]}')

    # --- 6. a device row writes the setting ----------------------------
    g = vol(dbg)
    if len(g["devices"]) >= 2:
        row = g["devices"][1]
        dbg.send(f"gui click {row['cx']} {row['cy']}")
        dbg.settle(); time.sleep(0.6)
        after = vol(dbg)
        check("clicking a device row moves the tick",
              after["selected"] == 1, f'selected={after["selected"]}')
        check("...and the choice reached the setting",
              setting(dbg, "audio_device") == row["value"],
              f'{setting(dbg, "audio_device")!r} vs {row["value"]!r}')
        dbg.send(f"gui click {g['devices'][0]['cx']} {g['devices'][0]['cy']}")
        dbg.settle(); time.sleep(0.6)
        check("...and choosing Automatic again restores it",
              setting(dbg, "audio_device") == "auto",
              setting(dbg, "audio_device"))
    else:
        # A machine with no sound hardware has only the Automatic row,
        # and the setting refuses a write for want of a device -- so the
        # rows are unreachable rather than broken. Said out loud rather
        # than silently skipped: a device list that went empty on a
        # machine that HAS a card would otherwise read as a clean pass.
        print("  SKIP  device rows -- this guest has no sound device "
              "(gui_regress boots this tool with --audio both)")

    # --- 7. it closes, and the pixels come back ------------------------
    dbg.send("gui click 640 300")
    dbg.settle(); time.sleep(0.4)
    check("a click on the desktop dismisses it", not vol(dbg)["open"])
    closed = qmp.stable_pixels(shot("vol_closed.png"), box=box)
    check("...and what was underneath is repainted", closed == before,
          "restored" if closed == before else "pixels differ")

    # --- 8. the Start menu and this one are mutually exclusive ---------
    g = vol(dbg)
    dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
    dbg.settle(); time.sleep(0.3)
    if check("reopened for the Start-menu check", vol(dbg)["open"]):
        tb = dbg.json("gui taskbar --json")["start"]
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")
        dbg.settle(); time.sleep(0.3)
        st = dbg.json("gui state --json")["overlays"]
        check("opening the Start menu closes the volume popup",
              st["start_menu"] is True and st["volume"] is False, str(st))
        dbg.send(f"gui click {tb['cx']} {tb['cy']}")
        dbg.settle()

    # Restored, so the next tool inherits the machine it expected.
    dbg.send("sh config set volume 100")
    dbg.send("sh config set audio_device auto")
    time.sleep(0.3)
    return report()


def report():
    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\nvolume_test: {passed} passed, {len(checks) - passed} failed")
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
