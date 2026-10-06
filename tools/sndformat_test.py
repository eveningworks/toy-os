#!/usr/bin/env python3
"""tools/sndformat_test.py -- a sound card's Format panel (ui/uui_sndformat.h)
in System Settings and Device Manager, on QEMU's HDA.

Judged by what the apps REPORT (the layout log names every control),
what they DRAW (ink inside the reported rects), and what they SAVE --
/etc/sound-cards.conf read back through the shell, the independent
reader, since the panel's own state is what is under test:

    Settings, Sound > Output: the card's rates as Allowed checkboxes and
      both lists are shown and drawn; choosing 48 kHz in Sample rate saves
      `rate=48000` and takes the Allowed row away; Match brings it back;
      unticking 44.1 saves `allowed=48000`; unticking the last one left is
      refused; Bit depth 16-bit saves `bits=16`.
    Device Manager: the panel is under a sound card's properties and not
      under any other device's.

Needs a sound card: gui_regress.py gives this tool `--audio hda`.
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from gui_debug import DebugConsole, enter_gui  # noqa: E402
from qmp_test import QMPSession  # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402
import devmgr_test as dm  # noqa: E402

SETTINGS = "/bin/wm/system/settings"
CONF = "/etc/sound-cards.conf"
K_DOWN, K_UP, K_ENTER = "0xf781", "0xf780", "0x0a"   # api/keyboard.h


# EVERY POSITION EVER REPORTED, for clicking -- dbg.logs() hands out only
# the lines since the last call, and an unchanged layout is not
# re-reported. What is SHOWN NOW is judged from the reports made after
# an action (wait_for), never from this.
_seen = {}


def layout(dbg, prefix):
    """The positions reported since the last call."""
    out = {}
    for line in dbg.logs(f"{prefix}:"):
        m = re.search(rf"{prefix}: layout (\w+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    _seen.update(out)
    return out


def wait_for(dbg, prefix, ok, timeout=8.0):
    """The reports made from now until `ok` holds of them."""
    deadline = time.time() + timeout
    got = layout(dbg, prefix)
    while not ok(got) and time.time() < deadline:
        time.sleep(0.2)
        got.update(layout(dbg, prefix))
    return got


def conf(dbg):
    return dbg.send(f"sh cat {CONF}") or ""


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sndformat_test")

    res = Results()
    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("sound format panel (Settings > Sound > Output, Device Manager)")
    dbg.send(f"sh rm {CONF}")
    dbg.send("sh config set desktop.layout_log on")
    # SINGLE-INSTANCE APPS: one left open would take the spawn and report
    # nothing new.
    for w in dbg.windows():
        if w["title"] in ("System Settings", dm.TITLE):
            dbg.send(f"gui close {w['z']}")
            dbg.settle(1.0)

    # --- System Settings ---------------------------------------------------
    dbg.logs("settings:", clear=True)
    win = dbg.spawn(f"{SETTINGS} system.audio_device", "System Settings")
    lay = wait_for(dbg, "settings", lambda l: "sndfmt_rate" in l and "sndfmt_now" in l)
    if not res.check("Settings shows the Format card on Sound > Output",
                     win is not None and "sndfmt_rate" in lay,
                     f"{sorted(k for k in lay if k.startswith('sndfmt'))}"):
        return 1
    allow = sorted(k for k in lay if k.startswith("sndfmt_allow_"))
    res.check("...with QEMU's seven rates as Allowed checkboxes",
              allow == sorted(f"sndfmt_allow_{hz}" for hz in
                              (16000, 22050, 32000, 44100, 48000, 88200, 96000)), f"{allow}")
    res.check("...and a Bit depth list", "sndfmt_bits" in lay)

    from PIL import Image
    shot = os.path.join(args.tmp, f"sndformat_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]

    def drawn(name, fresh=False):
        """Dark pixels inside where `name` was reported -- on a NEW
        screenshot when `fresh`, which is how "it went away" is seen."""
        nonlocal im
        if fresh:
            qmp.screenshot(shot)
            im = Image.open(shot).convert("RGB")
        x, y, w, h = _seen[name]
        n, _ = dm.ink(im, (c["x"] + x, c["y"] + y, w, h), lambda p: sum(p) < 300)
        return n

    res.check("the Sample rate list is DRAWN", drawn("sndfmt_rate") > 60,
              f"{drawn('sndfmt_rate')} dark px")
    res.check("the 44.1 kHz checkbox is DRAWN", drawn("sndfmt_allow_44100") > 30,
              f"{drawn('sndfmt_allow_44100')} dark px")

    def click(name):
        x, y, w, h = _seen[name]
        dbg.send(f"gui click {c['x'] + x + w // 2} {c['y'] + y + h // 2}")
        dbg.settle(0.6)

    def choose(name, steps):
        """Open the list, move `steps` rows (negative: up), take it."""
        click(name)
        for _ in range(abs(steps)):
            dbg.key(K_DOWN if steps > 0 else K_UP)
        dbg.key(K_ENTER)
        dbg.settle(0.8)

    # Rows: Match, 16, 22.05, 32, 44.1, 48, 88.2, 96 -- 48 kHz is five down.
    choose("sndfmt_rate", 5)
    text = conf(dbg)
    res.check("choosing 48 kHz saves a fixed rate", "rate=48000" in text, text.strip()[-160:])
    gone = drawn("sndfmt_allow_44100", fresh=True)
    res.check("...and takes the Allowed rates row away", gone < 5, f"{gone} dark px where it was")
    choose("sndfmt_rate", -5)
    res.check("Match what plays is saved back", "rate=match" in conf(dbg), conf(dbg).strip()[-160:])
    back = drawn("sndfmt_allow_44100", fresh=True)
    res.check("...and the Allowed rates row returns", back > 30, f"{back} dark px")

    click("sndfmt_allow_44100")
    text = conf(dbg)
    res.check("unticking 44.1 kHz saves the rest", "allowed=48000" in text, text.strip()[-160:])
    click("sndfmt_allow_48000")
    text = conf(dbg)
    res.check("...and the last allowed rate cannot be unticked", "allowed=48000" in text,
              text.strip()[-160:])

    # Rows: Automatic (16-bit), 16-bit.
    choose("sndfmt_bits", 1)
    res.check("Bit depth 16-bit is saved", "bits=16" in conf(dbg), conf(dbg).strip()[-160:])
    w = [x for x in dbg.windows() if x["title"] == "System Settings"]
    if w:
        dbg.send(f"gui close {w[-1]['z']}")
        dbg.settle(1.0)

    # --- Device Manager ----------------------------------------------------
    dbg.logs("devmgr:", clear=True)
    win = dbg.spawn(dm.DEVMGR, dm.TITLE)
    dm.wait_layout(dbg, lambda l: "tree" in l and "selected" in l)
    # The app's own word on whether the panel is up: `format` on its
    # "selected" line, reported with every selection.
    hda = dm.select_device(dbg, win, "pci:00:03.0")
    shown = dm._lay.get("format")
    res.check("Device Manager shows the Format panel for the sound card",
              hda and shown == 1, f"selected hda {hda}, format {shown}")
    nic = dm.select_device(dbg, win, "pci:00:02.0")
    shown = dm._lay.get("format")
    res.check("...and not for a network card", nic and shown == 0,
              f"selected nic {nic}, format {shown}")

    dbg.send(f"sh rm {CONF}")
    print(f"\nsndformat_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
