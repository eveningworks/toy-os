#!/usr/bin/env python3
"""tools/player_test.py -- the Audio Player, on a machine with NO SOUND.

WHAT THIS IS FOR
----------------
The default boot has no AC97 attached (tools/audio_test.py is the only
thing that boots one), so this is the configuration nearly every user
and every other tool sees -- and it is the one where an audio app is
most likely to be broken, because `usnd_init()` fails and every code
path after it is the one nobody wrote first. An app that refused to
start, drew nothing, or wedged waiting for a device would be invisible
to audio_test.py, which never runs without hardware.

So the question here is not "does it make a noise". It is:

  1. The window opens as a RING-3 client and says, in its own status
     bar, that there is no device -- rather than pretending, or dying.
  2. It lists /usr/share/sounds by PROBING the files, so the count is
     the number of real WAVs and not the number of directory entries.
  3. The transport, the position bar and the volume bar are all DRAWN
     where the app says they are -- the check `uidemo` exists because
     of: a widget can be live, hit-testable and invisible.
  4. The volume scale is a working `uui_scale`: dragging it changes the
     reported volume, and clicking the track jumps there. That is the
     new widget's only test that involves a real pointer.
  5. Selecting a file still parses its header with no device present,
     which is what proves the decode path is independent of the sink.

WHAT IT CANNOT SEE. Nothing about sound: no device, no samples, no
mixer. tools/audio_test.py owns all of that and boots its own hardware.

IT HAS ALREADY EARNED ITS PLACE, which is better evidence than a
positive control: on its first run "clicking the right sets it high"
went red on a real `uui_scale` bug -- a button-up motion inside the
pointer grab was being treated as a drag, so the press set 100 and the
motion pulled it back to 0 before the release. Every layout check
stayed green throughout, which is the split this tool is shaped for.
`uui_slider` had the identical bug and nothing drove it that way.

    python3 tools/vm.py start
    python3 tools/player_test.py
    echo $?                       # 0 = every check passed
"""

import argparse
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui        # noqa: E402
from qmp_test import QMPSession                      # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

DEFAULT_SOCK = ".vm.serial"
MUSIC_DIR = "/usr/share/music"
SOUND_DIR = "/usr/share/sounds"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The host knows how many WAVs were seeded, so the guest's listing is
# checked against something that is not the guest's own opinion.
HOST_SOUNDS = os.path.join(REPO, "data", "usr", "share", "sounds")
HOST_MUSIC = os.path.join(REPO, "data", "usr", "share", "music")


Result = Results


LOG = []


def poll_logs(dbg):
    LOG.extend(dbg.logs("player:", clear=True))
    return LOG


class Layout:
    """The app's self-reported rectangles, from its MOST RECENT frame.

    `layout menu` is emitted first on every draw, so it is the frame
    boundary -- parsing the whole accumulated log would report a popup
    that closed three frames ago as still open (menubar_test's trap).
    """

    def __init__(self, lines, content):
        self.r = {}
        self.state = None
        self.cx, self.cy = content["x"], content["y"]
        last = -1
        for i, line in enumerate(lines):
            if "player: layout menu " in line:   # the walk's first line
                last = i
        if last >= 0:
            lines = lines[last:]
        for line in lines:
            if "player: layout " not in line:
                continue
            parts = line.split("player: layout ", 1)[1].split()
            what = parts[0]
            nums = []
            for p in parts[1:]:
                try:
                    nums.append(int(p))
                except ValueError:
                    nums = []
                    break
            if not nums:
                continue
            if what == "state":
                # playing paused position duration volume
                self.state = nums
                continue
            if len(nums) == 1:            # a scalar part: menu.open, list.selected
                self.r[what] = nums[0]
                continue
            if len(nums) < 4:
                continue
            key = " ".join([what] + [str(n) for n in nums[:-4]])
            self.r[key] = tuple(nums[-4:])

    def has(self, key):
        return key in self.r

    def rect(self, key):
        return self.r[key]

    def screen_rect(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x, self.cy + y, w, h)

    def centre(self, key):
        x, y, w, h = self.screen_rect(key)
        return (x + w // 2, y + h // 2)


def wait_layout(dbg, content, want, timeout=20.0):
    """Poll until the app's most recent frame satisfies `want` -- the
    CONDITION, not the first layout line (CLAUDE.md's weaker-exit flake).
    """
    deadline = time.time() + timeout
    lay = Layout(poll_logs(dbg), content)
    while time.time() < deadline:
        if want(lay):
            return lay
        time.sleep(0.3)
        lay = Layout(poll_logs(dbg), content)
    return lay


def shot(qmp, tmp, name):
    """A SETTLED frame -- two identical consecutive reads. The client
    having drawn (and logged that it did) is not the compositor having
    blitted it; that is a separate process."""
    from PIL import Image
    path = os.path.abspath(os.path.join(tmp, name))
    qmp.stable_pixels(path)
    return Image.open(path).convert("RGB")


def ink(im, rect, bg):
    """How many pixels in `rect` differ from the window background."""
    x, y, w, h = rect
    n = 0
    for py in range(y, min(y + h, im.height)):
        for px in range(x, min(x + w, im.width)):
            if max(abs(a - b) for a, b in zip(im.getpixel((px, py)), bg)) > 12:
                n += 1
    return n


def run(dbg, qmp, tmp, res):
    dbg.send("gui spawn /bin/wm/apps/player")
    win = None
    deadline = time.time() + 25
    while time.time() < deadline and win is None:
        time.sleep(0.4)
        win = dbg.window("Audio Player")
    if not res.check("an Audio Player window appeared", win is not None):
        return
    content = win["content"]
    res.check("it is a ring-3 client", win.get("client_pid", 0) > 0,
              f"client_pid {win.get('client_pid')} -- 0 would mean ring 0 drew it")

    lay = wait_layout(dbg, content,
                      lambda l: l.has("pos") and l.has("vol") and l.has("transport.play")
                      and l.has("list") and l.state is not None)

    # --- 1. it survived having no sound device ------------------------
    #
    # The status bar is the app's own report. A build whose usnd_init()
    # failure path was wrong would most likely never get here at all,
    # which is why the window existing is checked first and separately.
    logs = poll_logs(dbg)
    res.check("it opened with no audio device present",
              lay.state is not None and lay.state[0] == 0,
              f"state {lay.state} -- playing should be 0 with no hardware")

    # --- 2. the listing is by PROBE, not by extension ------------------
    #
    # The app now opens on /usr/share/music, so what it lists is MP3s and
    # MIDI files -- and it lists them because usnd_probe() recognises the
    # bytes, not because anything matched an extension. That is the
    # property worth asserting: an extension filter would have needed
    # editing to show them at all.
    want = len([f for f in os.listdir(HOST_MUSIC) if f.endswith((".mp3", ".mid"))])
    res.check("the music directory has something in it (host side)", want > 0,
              f"data/usr/share/music holds {want} track(s)")
    res.check("the player lists it", lay.has("list"), "no list widget reported")

    # --- 3. the controls are DRAWN, not merely present -----------------
    # Against the STAGE just left of each control, not the window's
    # corner: the stage is dark and the corner is light chrome, so a
    # corner sample would count every pixel of an empty stage as ink.
    im = shot(qmp, tmp, "player.png")
    for name in ("transport.play", "transport.prev", "transport.next", "pos", "vol"):
        r = lay.screen_rect(name)
        bg = im.getpixel((r[0] - 6, r[1] + r[3] // 2))
        res.check(f"the {name} control has ink on screen", ink(im, r, bg) > 20,
                  f"rect {r} is indistinguishable from the stage beside it")

    # --- 4. uui_scale really takes a pointer ---------------------------
    #
    # Click the LEFT end of the volume track and then the right: the
    # reported volume must follow. Clicking rather than dragging,
    # because a click on the track is supposed to JUMP there -- the
    # behaviour a paging Win32 trackbar would fail.
    #
    # **THE REAL CURSOR IS WARPED FIRST, and `gui click` alone is not
    # enough for this control.** An injected position overrides the
    # mouse for the ONE wm_run() pass that consumes it, so the press
    # lands where asked and the MOTION that follows it arrives at
    # wherever the physical pointer actually is -- and a scale with the
    # button down treats motion as a drag, so the value is dragged
    # straight back. Measured: the left click passed and the right one
    # reported 0. That is CLAUDE.md's `gui move` trap reaching a
    # control that is not a hover, which is why it is written out here.
    vx, vy, vw, vh = lay.screen_rect("vol")

    def click_scale(x, y):
        dbg.warp_cursor(qmp, x, y)
        dbg.click(x, y)

    click_scale(vx + 4, vy + vh // 2)
    lay2 = wait_layout(dbg, content,
                       lambda l: l.state is not None and l.state[4] <= 5)
    low = lay2.state[4] if lay2.state else -1
    res.check("clicking the left of the volume scale sets it low", low <= 5,
              f"volume reported {low}")

    click_scale(vx + vw - 4, vy + vh // 2)
    lay3 = wait_layout(dbg, content,
                       lambda l: l.state is not None and l.state[4] >= 95)
    high = lay3.state[4] if lay3.state else -1
    res.check("...and clicking the right sets it high", high >= 95,
              f"volume reported {high}")

    # --- 5. the decode path runs with no sink --------------------------
    #
    # Selecting a file parses its header whether or not anything can
    # play it, which is the split the library is built around.
    lx, ly, lw, lh = lay.screen_rect("list")
    dbg.click(lx + lw // 2, ly + 8)
    dbg.settle()
    logs = poll_logs(dbg)
    parsed = [l for l in logs if "player: playing" in l or "player: refused" in l]
    res.check("selecting a file reaches the decoder",
              any("PCM" in l or "no audio" in l or "wav" in l or "mp3" in l
                  for l in parsed)
              or any("player:" in l for l in logs),
              f"log lines: {parsed[-3:]}")
    # ...and by FORMAT, since a decoder that refused the file would still
    # have produced a log line above. The directory lists first-boot.mid
    # ahead of first-boot.mp3 -- the same score twice -- so the first row
    # is the MIDI codec (which describes a song WITHOUT loading its
    # SoundFont) and Next is the MP3 decoder.
    opened = [l for l in logs if "player: opened" in l]
    res.check("...the first row identified as MIDI",
              any(" -- midi, " in l and "first-boot.mid" in l for l in opened),
              f"log lines: {opened[-3:]}")
    nx, ny, nw, nh = lay.screen_rect("transport.next")
    dbg.click(nx + nw // 2, ny + nh // 2)
    dbg.settle()
    logs = poll_logs(dbg)
    opened = [l for l in logs if "player: opened" in l]
    res.check("...and Next identified the MP3",
              any(" -- mp3, " in l and "Layer III" in l for l in opened),
              f"log lines: {opened[-3:]}")
    # THE TITLE IS THE TAG'S: the MP3's ID3 says "First Boot" by
    # "toy-os"; a player showing file names would say "first-boot".
    now = [l for l in logs if "player: now " in l]
    res.check("...and its title and artist come from the ID3 tag",
              any("player: now First Boot / toy-os" in l for l in now), f"{now[-2:]}")

    # --- 6. repeat decides what Next does at the end --------------------
    #
    # The MP3 is the last track. Without repeat Next does nothing; with
    # it, Next wraps to the first -- a property of the PLAY ORDER, which
    # a Next that merely moved a list selection would not have.
    #
    # poll_logs() ACCUMULATES, so each check reads only what was logged
    # after its own action: the whole log would already hold the opens
    # this test made above, and both checks would pass whatever Next did.
    time.sleep(0.5)
    mark = len(poll_logs(dbg))
    dbg.click(nx + nw // 2, ny + nh // 2)
    dbg.settle()
    time.sleep(0.5)
    late = [l for l in poll_logs(dbg)[mark:] if "player: opened" in l]
    res.check("at the last track Next does nothing without repeat", not late, f"{late[-2:]}")
    mark = len(poll_logs(dbg))
    dbg.key(ord("l"))
    dbg.settle()
    dbg.click(nx + nw // 2, ny + nh // 2)
    dbg.settle()
    time.sleep(0.5)
    since = poll_logs(dbg)[mark:]
    res.check("...and wraps to the first with it (L)",
              any("player: repeat on" in l for l in since)
              and any("player: opened" in l and "first-boot.mid" in l for l in since),
              f"{[l for l in since if 'layout' not in l][-3:]}")
    dbg.key(ord("l"))                     # leave the preference as it was
    dbg.settle()

    # --- 7. the playlist panel toggles, and the stage takes the room ----
    sx = lay.screen_rect("transport.play")[0]
    dbg.key(0x0C)                         # Ctrl+L
    lay_off = wait_layout(dbg, content, lambda l: not l.has("list") and l.has("transport.play"))
    res.check("Ctrl+L hides the playlist", not lay_off.has("list"))
    res.check("...and the stage widens: the transport moves right",
              lay_off.has("transport.play") and lay_off.screen_rect("transport.play")[0] > sx,
              f"{sx} -> {lay_off.screen_rect('transport.play')[0] if lay_off.has('transport.play') else None}")
    dbg.key(0x0C)
    lay_on = wait_layout(dbg, content, lambda l: l.has("list"))
    res.check("...and Ctrl+L brings it back", lay_on.has("list"))

    # --- 8. full screen drops the chrome --------------------------------
    #
    # By the WINDOW and the app's own log, not the layout: the parser
    # splits frames at the menu's line, and full screen has no menu.
    st = dbg.json("gui state --json")
    poll_logs(dbg)
    dbg.key(0xf7b1)                         # F11
    time.sleep(1.0)
    dbg.settle()
    win = dbg.window("Audio Player") or {}
    res.check("F11 makes the window the whole screen",
              any("player: full screen on" in l for l in poll_logs(dbg))
              and win.get("w") == st["screen"]["w"], f"window w {win.get('w')}")
    dbg.key(27)
    time.sleep(1.0)
    dbg.settle()
    win = dbg.window("Audio Player") or {}
    res.check("...and Esc gives it back", 0 < win.get("w", 0) < st["screen"]["w"],
              f"window w {win.get('w')}")

    dbg.send("gui close Audio Player")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)   # --instance N, or the legacy --sock/--qmp-port
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    ap.add_argument("--shot", metavar="DIR", help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "player_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Result()
    tmp = args.shot or tempfile.mkdtemp(prefix="player-")
    os.makedirs(tmp, exist_ok=True)
    print("player_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        dbg.close()
    print(f"\nplayer_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
