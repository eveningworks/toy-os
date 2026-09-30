#!/usr/bin/env python3
"""tools/doom_sound_test.py -- DOOM's sound effects and music, judged on
the HOST.

ON DEMAND, NOT IN THE GATE, for two reasons at once: it needs an IWAD
(which is not in this repository -- tools/fetch_wad.py), and it boots
its own guest with an AC97 attached, which nothing else in the suite
does. It SKIPS cleanly without a WAD rather than failing.

WHAT MAKES IT DISCRIMINATING
----------------------------
"There is audio" is the weak check this avoids. Doom has two
independent audio paths and one recording cannot tell them apart -- a
build where only the music worked and one where only the effects worked
would both produce noise.

So it never measures a mixture. It boots twice and ISOLATES each path,
which the app forwarding its argv to Doom is what makes possible:

  1. `-nosfx` -- music only. Any signal at all proves the OPL
     emulation, the pushed source and the sink all reach the device,
     because nothing else can be making it.
  2. `-nomusic` -- effects only. Any signal proves the DMX decode, the
     voice mixer and the panning path, for the same reason.

Each run is conclusive on its own, which a comparison against a
mixture is not: measured, music+effects covered 86% of windows and
effects alone still covered 69%, because Doom's attract demo is almost
continuously noisy. A 17-point gap is a weak thing to hang a verdict
on. Two silences that should not be silent are not.

The oracle is QEMU's wav audiodev -- what the DEVICE emitted, measured
on the host, sharing no code with the guest. Same move as
tools/audio_test.py.

WHAT IT CANNOT SEE. Whether the music is the RIGHT music: nothing here
checks pitch or melody against the WAD's MIDI. A wrong instrument bank
or a transposed OPL would pass. Nor does it check that the two paths
mix correctly when both are on -- only that each works alone.

    python3 tools/doom_sound_test.py [--instance N] [--keep]
"""
import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
from port_guard import port_is_free             # noqa: E402
from harness import copy_disk  # noqa: E402

# A 50 ms window counts as carrying sound above this peak. Well clear of
# the dither the recorder adds and well below any real effect.
LOUD = 500
WINDOW_MS = 50

# Only the LAST stretch of each recording is measured. The first ten-odd
# seconds are the WAD loading and 56 effects being converted, during
# which nothing plays -- counting that silence dropped a run with
# perfectly good music to 80% coverage and failed it.
TAIL_S = 20


class Result:
    def __init__(self):
        self.passes, self.fails, self.skipped = [], [], False

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(("  ok    " if ok else "  FAIL  ") + name + (f"  -- {detail}" if detail else ""))
        return ok



def wait_port_free(port, timeout=30):
    """A stopped VM does not release its QMP port instantly, and the
    next boot is refused if it has not.

    **port_guard's OWN predicate, not a connect.** It checks by BIND
    with SO_REUSEADDR deliberately unset, so a port in TIME_WAIT is
    still "in use" to it while a connect to that same port already
    fails -- a hand-rolled connect-based wait therefore reports free and
    the next launch is refused anyway. That cost a whole run here."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_is_free(port):
            return True
        time.sleep(0.5)
    return False


def coverage(path):
    """(seconds, peak, fraction of windows carrying sound) over the TAIL.

    The header is parsed BY HAND: QEMU finalises the RIFF sizes only on
    a clean device close, and a stopped VM leaves them zero, which
    python's `wave` refuses while every field this needs is correct.
    """
    with open(path, "rb") as f:
        hdr, raw = f.read(44), f.read()
    if len(hdr) < 44 or hdr[:4] != b"RIFF":
        return 0.0, 0, 0.0
    ch = struct.unpack_from("<H", hdr, 22)[0] or 1
    rate = struct.unpack_from("<I", hdr, 24)[0] or 1
    n = len(raw) // 2
    if n < ch:
        return 0.0, 0, 0.0
    s = struct.unpack(f"<{n}h", raw[:n * 2])
    left = s[::ch]
    total_s = len(left) / rate
    if len(left) > rate * TAIL_S:
        left = left[-rate * TAIL_S:]
    w = max(1, rate * WINDOW_MS // 1000)
    windows = [left[i:i + w] for i in range(0, len(left) - w, w)]
    if not windows:
        return 0.0, 0, 0.0
    loud = sum(1 for seg in windows if max(abs(x) for x in seg) > LOUD)
    return total_s, max(abs(x) for x in left), loud / len(windows)


def play(instance, img, wav, extra, seconds, res, label):
    """Boot, run DOOM for `seconds`, return its log lines."""
    n = instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"
    subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                    "--instance", str(n), "stop"], capture_output=True)
    wait_port_free(4445 + n)
    boot = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                           "--instance", str(n), "--disk", img,
                           "--audio-wav", wav, "start"], cwd=REPO)
    if boot.returncode != 0:
        res.check(f"[{label}] the guest booted with an AC97 attached", False)
        return None
    logs = []
    try:
        qmp = QMPSession(port=4445 + n)
        enter_gui(qmp, sock)
        dbg = DebugConsole(sock)
        dbg.send("gui spawn /bin/wm/apps/doom" + (" " + extra if extra else ""))
        # Wait for the game to REPORT itself ready rather than sleeping a
        # guessed amount: precaching 56 effects is real work under TCG.
        deadline = time.time() + 120
        while time.time() < deadline:
            logs += dbg.logs("doom:", clear=True)
            if any("doom: ready" in x for x in logs):
                break
            time.sleep(1.0)
        time.sleep(seconds)
        logs += dbg.logs("doom:", clear=True)
        dbg.close()
    finally:
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)
        wait_port_free(4445 + n)
    return logs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    res = Result()

    if not glob.glob(os.path.join(REPO, "data", "doom", "*.wad")):
        print("doom_sound_test: SKIPPED -- no IWAD (tools/fetch_wad.py).")
        return 0

    tmp = tempfile.mkdtemp(prefix="doom_sound_")
    img = os.path.join(tmp, "disk.img")
    copy_disk("disk.img", img, cwd=REPO)
    wav_music = os.path.join(tmp, "music.wav")
    wav_sfx = os.path.join(tmp, "sfx.wav")

    print("doom_sound_test: run 1 -- MUSIC ONLY (-nosfx)")
    logs = play(args.instance, img, wav_music, "-nosfx", 40, res, "music")
    if logs is None:
        print("\ndoom_sound_test: run 1 did not boot; nothing can be judged.")
        return 1
    joined = "\n".join(logs)
    if "no IWAD" in joined:
        print("doom_sound_test: SKIPPED -- the image has no IWAD.")
        return 0

    res.check("the music module came up", "doom: music on" in joined,
              next((x for x in logs if "music on" in x or "no music" in x), "no line"))
    res.check("...and the effects module did NOT", "doom: sound on" not in joined,
              "effects started despite -nosfx")

    secs1, peak1, cov1 = coverage(wav_music)
    # LOAD-BEARING. Nothing but the OPL path can be making this sound,
    # so any real signal proves the whole music chain end to end:
    # mus2mid, the GENMIDI instruments, dbopl, the render thread, usnd's
    # pushed source and the sink.
    # A LOWER BAR THAN THE EFFECTS', deliberately: OPL output is far
    # quieter than sampled effects even after dg_music.c's balancing
    # gain, and calibrating this on the effects' level is what failed
    # here first. Silence is 0; a healthy run measured ~5000.
    res.check("MUSIC ALONE reaches the device", peak1 > 1500,
              f"{secs1:.1f}s recorded, peak {peak1}, {cov1*100:.0f}% of windows loud")
    # Under TCG the guest falls behind wall clock and QEMU pads the
    # recording with host-side SILENCE, so this is well under 100% on a
    # healthy build (the trap tools/audio_test.py documents). Measured
    # ~86% with music playing.
    res.check("...continuously, as music is", cov1 > 0.70,
              f"{cov1*100:.0f}% of windows carry sound")

    print("\ndoom_sound_test: run 2 -- EFFECTS ONLY (-nomusic)")
    logs2 = play(args.instance, img, wav_sfx, "-nomusic", 40, res, "sfx")
    if logs2 is None:
        # NOT a set of vacuous passes: "the music module did not come up"
        # is trivially true of a guest that never booted, and reporting
        # that as a pass is how a broken run looks healthy.
        res.check("run 2 produced a recording to judge", False, "the guest did not boot")
        print(f"\ndoom_sound_test: {len(res.passes)} passed, {len(res.fails)} failed")
        return 1
    joined2 = "\n".join(logs2)
    res.check("the effects module came up", "doom: sound on" in joined2,
              next((x for x in logs2 if "sound on" in x or "no sound" in x), "no line"))
    res.check("...and the music module did NOT", "doom: music on" not in joined2,
              "music started despite -nomusic")
    cached = next((x for x in logs2 if "cached" in x), "")
    n_cached = 0
    if cached:
        try:
            n_cached = int(cached.split("cached")[1].split("/")[0])
        except (ValueError, IndexError):
            pass
    res.check("sound effects were precached", n_cached >= 40,
              cached.strip() or "no cache line")

    secs2, peak2, cov2 = coverage(wav_sfx)
    # The other half of the pair, and conclusive for the same reason:
    # with music off, the only thing that can make a noise is a DMX lump
    # decoded into a usnd voice.
    res.check("EFFECTS ALONE reach the device", peak2 > 4000,
              f"{secs2:.1f}s recorded, peak {peak2}, {cov2*100:.0f}% of windows loud")

    if args.keep:
        print(f"  recordings kept: {wav_music}, {wav_sfx}")
    print(f"\ndoom_sound_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
