#!/usr/bin/env python3
"""tools/mixer_test.py -- the per-application volume, from the tray
flyout down to what soundd applies.

`volume_test.py` covers the flyout's master slider, mute and device
rows, and it cannot cover this: its guest has NO SOUND CARD, so soundd
exits with "no sound device", there is no beacon, no roster, and the
per-app section is correctly absent. This boots a guest with an
`ich9-intel-hda` and plays something through it.

WHAT A BROKEN VERSION WOULD STILL PASS. "A row appeared" is satisfied
by a panel that invented one, and "the file was written" by a UI wired
to nothing. So the assertions are the ROUND TRIP, and each step names
a different component:

  1. the row appears ONLY while a client is playing, and carries the
     name soundd is mixing it under -- not a pid, which is what the
     ring is actually called
  2. a click at a quarter of the track moves THAT row and no other
  3. /etc/sound.conf gains the key, so the value outlived the widget
  4. soundd reads it back and SAYS so on the next stream

Nothing but a working chain produces 4 from 2: the daemon and the
panel share no code, no memory the panel writes, and no protocol -- the
file is the only thing between them.

    python3 tools/mixer_test.py [--instance N] [--keep]

On demand, not in the gate: it boots its own guest with extra hardware.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import port_guard                      # noqa: E402
from gui_debug import DebugConsole      # noqa: E402
from qmp_test import QMPSession         # noqa: E402
from harness import copy_disk  # noqa: E402

CONFIG = "/etc/sound.conf"
FIXTURE = "/tests/sine1k.wav"


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'ok   ' if ok else 'FAIL '} {name}" +
              (f"  -- {detail}" if detail else ""))
        (self.passes if ok else self.fails).append(name)
        return ok

    def report(self):
        print(f"\nmixer_test: {len(self.passes)} passed, {len(self.fails)} failed")
        for f in self.fails:
            print(f"  FAILED: {f}")
        return 1 if self.fails else 0


def vol(dbg):
    out = dbg.send("gui volume --json") or ""
    m = re.search(r"\{.*\}", out, re.S)
    return json.loads(m.group(0)) if m else {}


def play_and_catch(dbg, tries=10):
    """Start a stream and return its roster row before it ends.

    THE FIXTURE IS 1.5 SECONDS, so the row exists only for that long --
    `spawn` returns as soon as the child exists, which is what makes the
    window usable at all. A poll rather than a sleep because under TCG
    the guest's second is not the host's.
    """
    dbg.send(f"sh spawn /bin/aplay {FIXTURE}")
    for _ in range(tries):
        g = vol(dbg)
        if g.get("apps"):
            return g
        time.sleep(0.25)
    return vol(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    args = ap.parse_args()
    n = args.instance
    res = Result()

    def vm(*argv):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                            "--instance", str(n), *argv],
                           cwd=REPO, capture_output=True, text=True, check=False)
        return r.stdout + r.stderr

    vm("stop")
    # ITS OWN GUEST, WITH A CARD -- and on a COPY, because the flyout
    # genuinely WRITES /etc/sound.conf. The run removes the key again,
    # but a crash between the write and the removal would leave `aplay`
    # quietened on the real image for every later tool to inherit, and
    # that exact contamination cost four rebuilds the day this was
    # written (audio_test's own aplay phases recorded near-silence).
    # A copy cannot do it however this tool exits.
    tmp = tempfile.mkdtemp(prefix="mixer_test_")
    img = os.path.join(tmp, "disk.img")
    copy_disk("disk.img", img, cwd=REPO)
    boot = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(n),
            "--disk", img, "--audio", "hda", "start"]
    if subprocess.run(boot, cwd=REPO).returncode != 0:
        res.check("the guest booted with a sound card", False)
        return 1

    try:
        dbg = DebugConsole(port_guard.instance_sock(n), timeout=15)
        dbg.settle()
        time.sleep(1.0)
        dbg.send(f"sh rm {CONFIG}")     # a previous run's value is not a fixture

        qmp = QMPSession(port=port_guard.instance_qmp(n))
        ref_png = os.path.join(tmp, "wallpaper.png")
        qmp.screenshot(ref_png)     # the desktop with nothing open over it

        g = vol(dbg)
        if not res.check("the volume flyout reports a tray item", g.get("tray", {}).get("w", 0) > 0,
                         str(g.get("tray"))):
            return res.report()

        # THE ABSENCE IS AN ASSERTION. A panel that always drew an
        # "Applications" box would pass every check below it.
        res.check("with nothing playing there are no application rows",
                  not g.get("apps"), f"apps={g.get('apps')}")
        closed_h = g.get("h", 0)

        dbg.send(f"gui click {g['tray']['cx']} {g['tray']['cy']}")
        dbg.settle(); time.sleep(0.4)
        g = vol(dbg)
        if not res.check("clicking the tray icon opens it", g.get("open")):
            return res.report()

        g = play_and_catch(dbg)
        apps = g.get("apps") or []
        if not res.check("a playing client appears as a row", len(apps) == 1,
                         json.dumps(apps)):
            return res.report()
        row = apps[0]
        # THE NAME, NOT THE PID. The ring is called `snd.<pid>`; a panel
        # showing that would mean the identity never reached it.
        res.check("...named by its application, not its ring", row["app"] == "aplay",
                  f'app="{row["app"]}"')
        res.check("...starting at full gain", row["gain"] == 100, f'gain={row["gain"]}')
        res.check("...and the panel grew to hold it", g["h"] > closed_h,
                  f'{closed_h} -> {g["h"]}')

        # A quarter along the track. The exact value is the scale's to
        # round, so the assertion is a BAND -- what is being judged is
        # that the click moved this row, not that it computed 25.
        x = row["x"] + row["w"] // 4
        dbg.send(f"gui click {x} {row['cy']}")
        dbg.settle(); time.sleep(0.6)
        after = (vol(dbg).get("apps") or [{}])[0]
        moved = after.get("gain", 100)
        res.check("clicking a quarter along the track moves that row",
                  15 <= moved <= 35, f"gain {row['gain']} -> {moved}")

        # THE VALUE OUTLIVED THE WIDGET. /etc/sound.conf is the only
        # thing between the panel and the daemon.
        conf = dbg.send(f"sh cat {CONFIG}") or ""
        res.check("...and is written to /etc/sound.conf",
                  re.search(rf"aplay\s*=\s*{moved}\b", conf) is not None,
                  conf.strip()[:80])

        # AND THE DAEMON READS IT BACK. Its own words, on a stream
        # started AFTER the write -- the half no amount of UI testing
        # can fake.
        #
        # COUNTED AS A DIFFERENCE from a baseline, never as "is the line
        # present": soundd reloads the moment the flyout writes, so the
        # line already exists from the client that was still playing
        # then. Matching the whole log passed this check without the
        # second stream ever being mixed -- the version of this test
        # that did so is why the baseline is here.
        want = f"aplay volume {moved}%"
        base = (dbg.send("sh dmesg") or "").count(want)
        dbg.send(f"sh spawn /bin/aplay {FIXTURE}")
        time.sleep(2.0)
        log = dbg.send("sh dmesg") or ""
        res.check("soundd applies it to a stream started after the write",
                  log.count(want) > base,
                  f"{base} -> {log.count(want)} occurrence(s) of \"{want}\"")

        # --- the panel SHRINKS BACK, and leaves no pixels behind -------
        #
        # A DAMAGE FAULT IS ONLY VISIBLE IN PIXELS. The panel is
        # anchored above the taskbar, so its height growing moves its
        # TOP UP; when the stream ends and it shrinks, the band it used
        # to occupy is wallpaper again -- and damaging only the NEW rect
        # left the old frame painted there. Reported from the ASUS by
        # eye, because every check here passed while it was happening.
        # A FRESH STREAM, caught while it is still playing: the earlier
        # checks take longer than the 1.5s fixture, so by now the panel
        # is already short and `tall` would be the shrunken one -- which
        # is a check that measures nothing, and did on its first run.
        tall = play_and_catch(dbg)
        tall_h = tall.get("h", 0)
        band = None
        if tall.get("apps"):
            for _ in range(20):                  # then let it end
                g = vol(dbg)
                if not g.get("apps"):
                    # The band between the tall panel's top and the short
                    # one's: exactly what a too-small damage rect fails
                    # to repaint.
                    band = (g["x"], tall["y"], g["x"] + g["w"], g["y"])
                    short_h = g.get("h", 0)
                    break
                time.sleep(0.25)
        if res.check("the panel shrinks again when the stream ends",
                     band is not None and short_h < tall_h,
                     f'{tall_h} -> {band and short_h}'):
            # AGAINST THE SAME BAND BEFORE ANYTHING OPENED, not against
            # "is it one colour": the wallpaper is a GRADIENT, so a
            # band of it is legitimately hundreds of colours and that
            # assertion could never pass. A self-comparison also
            # survives someone changing the wallpaper.
            from PIL import Image
            after_png = os.path.join(tmp, "after.png")
            qmp.screenshot(after_png)
            with Image.open(ref_png) as a, Image.open(after_png) as b:
                want = a.convert("RGB").crop(band).tobytes()
                got = b.convert("RGB").crop(band).tobytes()
            differing = sum(1 for x, y in zip(want, got) if x != y)
            res.check("...and leaves no residue in the band it vacated",
                      differing == 0,
                      f"{differing} of {len(want)} subpixels differ from the "
                      f"same band before the panel ever opened")

        # --- reopening after the streams changed ------------------------
        #
        # NOTHING REFRESHES THE ROSTER WHILE THE PANEL IS CLOSED, so a
        # panel reopened after a stream ended used to draw ONE frame
        # from the stale list -- a row too tall, with a dead
        # application in it, gone on the next frame. It does not leave
        # residue and it is not a damage fault: it FLASHES, which is
        # how it was reported from the ASUS.
        #
        # The assertion is the state on the FIRST look after opening,
        # with no frame in between to correct it.
        # THE STALE STATE HAS TO BE ESTABLISHED FIRST, or this passes
        # without testing anything -- which it did on its first run.
        # The panel must be CLOSED while a stream is still playing, so
        # its row count is left at 1 with nothing to correct it.
        toggle = f"gui click {g['tray']['cx']} {g['tray']['cy']}"
        staged = play_and_catch(dbg)
        if res.check("a row is showing when the panel is closed",
                     bool(staged.get("apps")), json.dumps(staged.get("apps"))):
            dbg.send(toggle)             # close WHILE it still plays
            dbg.settle()
            for _ in range(20):          # let it end, panel closed
                time.sleep(0.25)
                if not (dbg.send("sh dmesg") or "").rstrip().endswith("gone"):
                    pass
                break
            time.sleep(2.0)
            dbg.send(toggle)             # reopen
            first = vol(dbg)             # FIRST look, no settle
            # AND THIS CANNOT SEE THE FLASH ITSELF. The stale frame is
            # ONE frame; a serial round trip is slower than that, so a
            # poll has always corrected the state before this can ask
            # -- measured, by disabling the refresh in volume_open_now()
            # and watching this still pass. What it holds is the
            # invariant underneath (a reopened panel reports no dead
            # row); the one-frame artifact is the maintainer's eye, or
            # a photo of the panel.
            res.check("reopening reports no row for a stream that ended meanwhile",
                      not first.get("apps"),
                      f'apps={first.get("apps")} h={first.get("h")}')
            dbg.send(toggle)             # closed again

        # LEAVE THE IMAGE AS FOUND. A per-app volume left behind would
        # quieten aplay for every later tool on this disk, which is the
        # hazard CLAUDE.md records for settings_test's mouse values.
        qmp.close()
        dbg.send(f"sh rm {CONFIG}")
        gone = dbg.send(f"sh cat {CONFIG}") or ""
        res.check("the test's own config is removed again",
                  "no such file" in gone, gone.strip()[:60])
        return res.report()
    finally:
        if not args.keep:
            vm("stop")


if __name__ == "__main__":
    sys.exit(main())
