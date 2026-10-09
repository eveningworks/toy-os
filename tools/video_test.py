#!/usr/bin/env python3
"""tools/video_test.py -- the Video Player, on a machine with NO SOUND.

The default boot has no sound device, so the pictures run on the
monotonic clock (lib/uvid_play.h) -- the configuration every other GUI
tool sees, and the one where a player most easily wedges waiting for a
sound clock that never moves. What this asks:

  1. The window opens as a RING-3 client, lists /usr/share/videos by
     PROBING the files (the count is the host's), and plays the file it
     was handed.
  2. The picture is DRAWN AND MOVING: two photographs a second apart
     differ inside the picture's rect, which a player whose clock had
     stalled, or whose widget drew only the ground, would fail. The
     clips carry their frame number burnt in, so moving content cannot
     be pixel-identical by accident.
  3. A click on the picture PAUSES: the app says so, its clock stops,
     and two photographs a second apart are IDENTICAL inside the picture
     -- the inverse check, so (2) is not passing on noise.
  4. Right seeks ten seconds; the clock says where it landed.
  5. A playlist row plays its file -- the AVI, so both containers and
     both codecs are opened by one run.
  6. Ctrl+S saves the frame showing, and says where.
  7. Hovering the seek bar shows a still from that time.
  8. F11 is full screen with the floating bar; Esc gives the window back.

WHAT IT CANNOT SEE: sound, and therefore A/V sync -- `vplay --sync`
measures where the container put each, and a sound-clock run needs a
device (tools/audio_test.py's territory).

    python3 tools/vm.py start
    python3 tools/video_test.py
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

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# What the image was SEEDED from, not data/: an EXTRAS=1 build adds a
# film, which moves every row after it.
HOST_VIDEOS = os.path.join(REPO, "seed", "sync", "usr", "share", "videos")
if not os.path.isdir(HOST_VIDEOS):
    HOST_VIDEOS = os.path.join(REPO, "data", "usr", "share", "videos")
FIRST = "/usr/share/videos/first-boot.mpg"   # long enough to seek in

LOG = []


def poll_logs(dbg):
    LOG.extend(dbg.logs("video:", clear=True))
    return LOG


class Layout:
    """The app's self-reported rectangles from its most recent frame. A
    frame's block starts at the first item the walk reports, which is
    the menu in a window and the video in full screen."""

    def __init__(self, lines, content):
        self.r, self.state, self.preview = {}, None, None
        self.cx, self.cy = content["x"], content["y"]
        last = -1
        for i, line in enumerate(lines):
            if "video: layout menu " in line or "video: layout video " in line:
                last = i
        if last >= 0:
            lines = lines[last:]
        for line in lines:
            if "video: layout " not in line:
                continue
            parts = line.split("video: layout ", 1)[1].split()
            what, nums = parts[0], []
            for p in parts[1:]:
                try:
                    nums.append(int(p))
                except ValueError:
                    nums = []
                    break
            if what == "state" and nums:
                self.state = nums            # paused, position s, sound, ended
            elif what == "preview" and nums:
                self.preview = nums[0]
            elif len(nums) == 1:
                self.r[what] = nums[0]
            elif len(nums) >= 4:
                self.r[" ".join([what] + [str(n) for n in nums[:-4]])] = tuple(nums[-4:])

    def has(self, key):
        return key in self.r

    def screen_rect(self, key):
        x, y, w, h = self.r[key]
        return (self.cx + x, self.cy + y, w, h)

    def centre(self, key):
        x, y, w, h = self.screen_rect(key)
        return (x + w // 2, y + h // 2)


def wait_layout(dbg, content, want, timeout=20.0):
    deadline = time.time() + timeout
    lay = Layout(poll_logs(dbg), content)
    while time.time() < deadline:
        if want(lay):
            return lay
        time.sleep(0.3)
        lay = Layout(poll_logs(dbg), content)
    return lay


def photo(qmp, tmp, name):
    """A photograph of MOTION: not a settled frame, which a playing video
    never gives."""
    from PIL import Image
    path = os.path.join(tmp, name)
    qmp.screenshot(path, stable=False)
    return Image.open(path).convert("RGB")


def differing(a, b, rect):
    x, y, w, h = rect
    n = 0
    for py in range(y, y + h, 2):
        for px in range(x, x + w, 2):
            if max(abs(p - q) for p, q in zip(a.getpixel((px, py)), b.getpixel((px, py)))) > 24:
                n += 1
    return n


def colours(im, rect):
    x, y, w, h = rect
    return len({im.getpixel((px, py)) for py in range(y, y + h, 4) for px in range(x, x + w, 4)})


def since(mark):
    return [l for l in LOG[mark:] if "layout" not in l]


def run(dbg, qmp, tmp, res):
    dbg.send(f"gui spawn /bin/wm/apps/video {FIRST}")
    win, deadline = None, time.time() + 25
    while time.time() < deadline and win is None:
        time.sleep(0.4)
        win = dbg.window("Video Player")
    if not res.check("a Video Player window appeared", win is not None):
        return
    content = win["content"]
    res.check("it is a ring-3 client", win.get("client_pid", 0) > 0, f"client_pid {win.get('client_pid')}")

    lay = wait_layout(dbg, content, lambda l: l.has("video.picture") and l.state is not None
                      and l.state[1] >= 1)
    logs = poll_logs(dbg)

    # --- 1. the listing, and the file it was handed ----------------------
    want = len([f for f in os.listdir(HOST_VIDEOS) if f.endswith((".mpg", ".avi"))])
    rows = len([k for k in lay.r if k.startswith("list.row ")])
    res.check("the playlist lists every video the image carries", rows == want,
              f"{rows} row(s), the host seeded {want}")
    res.check("it plays the file it was handed",
              any("video: playing " + FIRST in l and "MPEG-1" in l for l in logs),
              f"{[l for l in logs if 'video: playing' in l][-2:]}")

    # --- 2. drawn and moving ---------------------------------------------
    pic = lay.screen_rect("video.picture")
    a = photo(qmp, tmp, "a.png")
    time.sleep(1.0)
    b = photo(qmp, tmp, "b.png")
    res.check("the picture has content, not a flat ground", colours(a, pic) > 20,
              f"{colours(a, pic)} colour(s) in {pic}")
    res.check("the picture MOVES: two photographs a second apart differ", differing(a, b, pic) > 50,
              f"{differing(a, b, pic)} sample(s) differ")
    res.check("the clock runs on with no sound device", lay.state[1] >= 1 and lay.state[2] == 0,
              f"state {lay.state}")

    # --- 3. a click pauses --------------------------------------------------
    mark = len(LOG)
    cx, cy = lay.centre("video.picture")
    dbg.warp_cursor(qmp, cx, cy)
    dbg.click(cx, cy)
    lay = wait_layout(dbg, content, lambda l: l.state is not None and l.state[0] == 1)
    res.check("a click on the picture pauses", lay.state and lay.state[0] == 1
              and any("video: paused" in l for l in since(mark)), f"state {lay.state}")
    held = lay.state[1] if lay.state else -1
    time.sleep(0.5)
    a = photo(qmp, tmp, "p1.png")
    time.sleep(1.2)
    b = photo(qmp, tmp, "p2.png")
    lay = wait_layout(dbg, content, lambda l: l.state is not None, timeout=3)
    res.check("...the clock stops", lay.state and lay.state[1] == held, f"{held} -> {lay.state}")
    res.check("...and the picture holds still", differing(a, b, pic) == 0,
              f"{differing(a, b, pic)} sample(s) differ while paused")

    # --- 4. Right seeks ten seconds ----------------------------------------
    before = lay.state[1]
    dbg.key(0xF785)                      # Right
    lay = wait_layout(dbg, content, lambda l: l.state is not None and l.state[1] >= before + 9)
    res.check("Right seeks ten seconds on", lay.state and before + 9 <= lay.state[1] <= before + 11,
              f"{before} -> {lay.state}")

    # --- 5. a playlist row plays its file ------------------------------------
    mark = len(LOG)
    orbit = None
    names = sorted(f for f in os.listdir(HOST_VIDEOS) if f.endswith((".mpg", ".avi")))
    if "orbit.avi" in names:
        orbit = names.index("orbit.avi")
    if res.check("orbit.avi is in the list (host side)", orbit is not None) and lay.has(f"list.row {orbit}"):
        rx, ry = lay.centre(f"list.row {orbit}")
        dbg.warp_cursor(qmp, rx, ry)
        dbg.click(rx, ry)
        dbg.settle()
        time.sleep(1.0)
        poll_logs(dbg)
        res.check("...clicking its row plays it, Motion JPEG from an AVI",
                  any("video: playing /usr/share/videos/orbit.avi" in l and "Motion JPEG" in l
                      for l in since(mark)), f"{since(mark)[-3:]}")

    # --- 6. Save frame ---------------------------------------------------------
    mark = len(LOG)
    dbg.key(0x13)                        # Ctrl+S
    time.sleep(1.0)
    poll_logs(dbg)
    saved = [l for l in since(mark) if "video: save frame" in l]
    res.check("Ctrl+S saves the frame showing", any("/home/orbit-" in l and l.endswith("ok") for l in saved),
              f"{saved}")

    # --- 7. the seek preview -------------------------------------------------------
    lay = wait_layout(dbg, content, lambda l: l.has("pos"))
    px, py, pw, ph = lay.screen_rect("pos")
    dbg.warp_cursor(qmp, px + pw // 2, py + ph // 2)
    dbg.send(f"gui move {px + pw // 2} {py + ph // 2}")
    lay = wait_layout(dbg, content, lambda l: l.preview is not None and l.preview >= 0, timeout=10)
    res.check("hovering the seek bar shows a still from that time",
              lay.preview is not None and 3 <= lay.preview <= 7, f"preview {lay.preview} s (the clip is 10 s)")
    dbg.warp_cursor(qmp, content["x"] + 4, content["y"] + 4)

    # --- 8. full screen ---------------------------------------------------------------
    st = dbg.json("gui state --json")
    mark = len(LOG)
    dbg.key(0xF880)                      # F11
    time.sleep(1.5)
    dbg.settle()
    win = dbg.window("Video Player") or {}
    lay = wait_layout(dbg, win.get("content", content), lambda l: l.has("pill"), timeout=6)
    res.check("F11 makes the window the whole screen",
              any("video: full screen on" in l for l in since(mark)) and win.get("w") == st["screen"]["w"],
              f"window w {win.get('w')}")
    res.check("...with the floating bar under the controls", lay.has("pill"))
    dbg.key(27)
    time.sleep(1.5)
    dbg.settle()
    win = dbg.window("Video Player") or {}
    res.check("...and Esc gives the window back", 0 < win.get("w", 0) < st["screen"]["w"],
              f"window w {win.get('w')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true", help="the VM already shows the desktop")
    ap.add_argument("--shot", metavar="DIR", help="keep the screenshots here")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "video_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Results()
    tmp = args.shot or tempfile.mkdtemp(prefix="video-")
    os.makedirs(tmp, exist_ok=True)
    print("video_test: checks")
    try:
        run(dbg, qmp, tmp, res)
    finally:
        # Closed even when a check threw: a window left playing would
        # answer the NEXT run's lookups and mix its log into them.
        for i, w in reversed(list(enumerate(dbg.windows() or []))):
            if w.get("title") == "Video Player":
                dbg.send(f"gui close {i}")
        dbg.close()
    print(f"\nvideo_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
