#!/usr/bin/env python3
"""tools/audio_rate_test.py -- a card's rate follows what plays, judged
on the HOST.

Each scenario boots QEMU with one sound card and a wav recorder, plays
/tests/sine1k.flac (1 kHz, 1.5 s, 44.1 kHz), and measures the recording
by zero crossings -- audio_test.py's oracle, which shares no code with
the stack. The pitch and the length are what a wrong rate cannot fake:
a 44.1 kHz ring played as 48 kHz is 1088 Hz for 1.38 s, the reverse
919 Hz for 1.63 s.

    1. match (the default): soundd takes the card to 44.1 kHz for the
       file -- the dmesg line and `lssound -v` say so -- and the tone is
       exact, so nothing resampled it on the way.
    2. fixed at 48 kHz (`sndfmt -r 48000`): the card stays at 48 and the
       file is resampled to it -- still 1 kHz.
    3. two at once: a 48 kHz file joining a 44.1 kHz one does not move
       the card, because a switch would cut the first.
    4. no daemon: the playing program's own sink makes the same decision
       (lib/usnd_sink_dev.c).

    python3 tools/audio_rate_test.py [--instance N] [--card hda|ac97] [--keep]

On demand: it boots its own guests with extra hardware.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import struct  # noqa: E402
from audio_test import establish, segments, wait_serial  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

TONE = "/tests/sine1k.flac"      # 1 kHz, 1.5 s, 44.1 kHz mono
LONG = "/usr/share/music/first-boot.flac"   # 20 s at 44.1 kHz
OTHER = "/tests/ramp24.flac"     # 0.1 s at 48 kHz


def pitch(wav):
    """(Hz over the longest run of tone, ms of tone in all).

    By INTERPOLATED zero crossings across one continuous run: audio_test's
    measure() counts within fixed 50 ms windows and loses the crossings on
    their edges, which reads ~1% low -- too coarse for 919 vs 1000 vs
    1088 to be the only thing it can tell apart."""
    runs = segments(wav)
    if not runs:
        return 0.0, 0.0
    with open(wav, "rb") as f:
        hdr = f.read(44)
        raw = f.read()
    ch = struct.unpack_from("<H", hdr, 22)[0]
    rate = struct.unpack_from("<I", hdr, 24)[0]
    x = struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2])[::max(ch, 1)]
    start, ms, _ = max(runs, key=lambda r: r[1])
    a = int(start * rate)
    b = a + int(ms * rate / 1000)
    seg = x[a + (b - a) // 10: b - (b - a) // 10]
    cr = [i + (-seg[i]) / (seg[i + 1] - seg[i]) for i in range(len(seg) - 1)
          if seg[i] < 0 <= seg[i + 1]]
    hz = (len(cr) - 1) / ((cr[-1] - cr[0]) / rate) if len(cr) > 2 else 0.0
    return hz, sum(r[1] for r in runs)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--card", choices=("hda", "ac97"), default="hda")
    ap.add_argument("--keep", action="store_true", help="keep the recordings")
    args = ap.parse_args()
    n = args.instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"
    dev = "hda0" if args.card == "hda" else "ac97"

    res = Results()
    tmp = tempfile.mkdtemp(prefix="audio_rate_test_")
    img = os.path.join(tmp, "disk.img")
    copy_disk("disk.img", img, cwd=REPO)

    def vm(*a):
        return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                               "--instance", str(n), *a], cwd=REPO, capture_output=True,
                              text=True)

    def boot(wav):
        vm("stop")
        r = vm("--disk", img, "--audio", args.card, "--audio-wav", wav, "start")
        if r.returncode != 0:
            return None
        dbg = wait_serial(sock)
        if dbg:
            establish(dbg)
        return dbg

    def plays(dbg):
        """What `lssound -v` says the active card plays now."""
        for line in (dbg.send("sh lssound -v") or "").splitlines():
            if "plays:" in line:
                return line.split("plays:", 1)[1].strip()
        return ""

    def judge(what, wav, whole=True):
        hz, ms = pitch(wav)
        res.check(f"{what}: the tone is 1 kHz", abs(hz - 1000) < 2, f"{hz:.2f} Hz")
        # 1500 ms of tone, every frame once. The no-daemon start loses
        # ~50 ms on 2513429a too (docs/bugs.md), so it is only reported.
        if whole:
            res.check(f"{what}: and 1500 ms of it", abs(ms - 1500) < 15, f"{ms:.1f} ms")
        else:
            print(f"  note  {what}: {ms:.1f} ms of tone (a start glitch predates this)")

    def scenario(name, body):
        wav = os.path.join(tmp, f"{name}.wav")
        dbg = boot(wav)
        if not res.check(f"[{name}] the guest booted with {args.card}", dbg is not None):
            return
        try:
            body(dbg, wav)
        finally:
            dbg.close()
            vm("stop")      # flushes the recording
        if name in ("match", "fixed", "nodaemon"):
            judge(name, wav, whole=name != "nodaemon")

    def s_match(dbg, wav):
        dbg.send("sh sndfmt -r match -a 44100,48000 -b auto")
        dbg.send(f"sh aplay {TONE}")
        dmesg = dbg.send("sh dmesg") or ""
        res.check("match: soundd took the card to 44.1 kHz",
                  f"soundd: {dev} now at 44100 Hz" in dmesg,
                  next((l for l in dmesg.splitlines() if "now at" in l), "no switch line"))
        res.check("match: lssound says 44.1 kHz", "44.1 kHz" in plays(dbg), plays(dbg))

    def s_fixed(dbg, wav):
        out = dbg.send("sh sndfmt -r 48000") or ""
        res.check("fixed: sndfmt took the setting", "rate:    48 kHz" in out, out.strip()[-120:])
        dbg.send(f"sh aplay {TONE}")
        res.check("fixed: the card stayed at 48 kHz", "48 kHz" in plays(dbg)
                  and "44.1" not in plays(dbg), plays(dbg))
        dbg.send("sh sndfmt -r match")

    def s_mix(dbg, wav):
        dbg.send("sh sndfmt -r match -a 44100,48000")
        dbg.send(f"sh spawn /bin/aplay {LONG}")
        deadline = time.time() + 20
        while time.time() < deadline and "44.1" not in plays(dbg):
            time.sleep(0.5)
        res.check("mix: the first file took the card to 44.1 kHz", "44.1" in plays(dbg), plays(dbg))
        dbg.send(f"sh aplay {OTHER}")
        res.check("mix: a 48 kHz file joining it did not move the card",
                  "44.1" in plays(dbg), plays(dbg))
        dmesg = dbg.send("sh dmesg") or ""
        res.check("mix: no switch to 48 kHz was made under it",
                  f"{dev} now at 48000" not in dmesg,
                  next((l for l in dmesg.splitlines() if "now at 48000" in l), ""))

    def s_nodaemon(dbg, wav):
        dbg.send("sh service stop soundd")
        dbg.send(f"sh aplay {TONE}")
        res.check("nodaemon: the program's own sink took the card to 44.1 kHz",
                  "44.1" in plays(dbg), plays(dbg))

    print(f"audio_rate_test: {args.card}")
    scenario("match", s_match)
    scenario("fixed", s_fixed)
    scenario("mix", s_mix)
    scenario("nodaemon", s_nodaemon)

    if args.keep:
        print(f"audio_rate_test: recordings kept in {tmp}")
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\naudio_rate_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
