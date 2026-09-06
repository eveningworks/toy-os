#!/usr/bin/env python3
"""tools/soundd_test.py -- the sound daemon: TWO programs audible at once,
judged on the HOST.

THE ASSERTION IS THE WHOLE POINT, and no in-guest check can make it. A
mixer that silently serialised its clients -- played one, dropped the
other -- would pass every "did it crash", "did aplay return 0" and "is
the daemon running" check there is. So this plays 1000 Hz and 440 Hz
from two separate processes AT THE SAME TIME, records what the DEVICE
emitted through QEMU's wav audiodev, and requires BOTH frequencies in
that one recording. The oracle (a Goertzel filter here on the host)
shares no code with the daemon, the library or the driver.

Zero crossings cannot do this. audio_test.py counts them because it has
one tone per recording; two mixed tones cross zero at neither of their
frequencies, so this measures the energy AT each frequency instead and
also samples a third, unplayed one as a control -- if 700 Hz reads as
loud as 440, the measurement is noise and proves nothing.

    python3 tools/soundd_test.py [--instance N] [--card ac97|hda] [--keep]
                                 [--positive-control]

`--positive-control` stops the daemon before playing, so the second
aplay gets -EBUSY and only one tone can reach the card. It MUST go red:
a run where both tones are found with the mixer disabled is a run whose
measurement is measuring something else.

On demand, not in the gate: it boots its own guest with extra hardware,
as audio_test.py does.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from gui_debug import DebugConsole          # noqa: E402
import port_guard                           # noqa: E402

TONE_A_HZ = 1000   # /tests/sine1k.wav
TONE_B_HZ = 440    # /tests/sine440.wav
CONTROL_HZ = 700   # played by nobody -- the noise floor


class Result:
    def __init__(self):
        self.fails = 0
        self.checks = 0

    def check(self, what, ok, detail=""):
        self.checks += 1
        if not ok:
            self.fails += 1
        print(f"  {'ok  ' if ok else 'FAIL'} {what}"
              + (f" -- {detail}" if detail and not ok else ""))
        return ok


def read_wav(path):
    """(rate, left channel) from QEMU's recording, header parsed by hand.

    audio_test.py's reason: the RIFF size fields are finalised only on a
    clean device close, and a stopped VM leaves them zero, which python's
    wave module refuses while every field needed here is correct.
    """
    with open(path, "rb") as f:
        hdr = f.read(44)
        raw = f.read()
    if len(hdr) < 44 or hdr[:4] != b"RIFF" or hdr[8:12] != b"WAVE":
        return 0, []
    ch = struct.unpack_from("<H", hdr, 22)[0]
    rate = struct.unpack_from("<I", hdr, 24)[0]
    width = struct.unpack_from("<H", hdr, 34)[0] // 8
    if width != 2 or ch < 1 or rate == 0 or len(raw) < 4:
        return rate, []
    total = len(raw) // 2
    samples = struct.unpack(f"<{total}h", raw[:total * 2])
    return rate, list(samples[::ch])


def goertzel(samples, rate, freq):
    """Energy at one frequency, normalised by the block's own energy.

    A ratio rather than an absolute: the recording's level depends on
    the volume setting, on how many voices were summed and on TCG's
    padding, none of which this is trying to measure. What it answers is
    "how much of what came out was at this pitch".
    """
    n = len(samples)
    if n < 64:
        return 0.0
    k = int(0.5 + n * freq / rate)
    w = 2.0 * math.pi * k / n
    coeff = 2.0 * math.cos(w)
    s0 = s1 = s2 = 0.0
    energy = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
        energy += float(x) * float(x)
    mag = s1 * s1 + s2 * s2 - coeff * s1 * s2
    if energy <= 0.0:
        return 0.0
    return math.sqrt(max(mag, 0.0)) / math.sqrt(energy * n) * 2.0


def loudest_window(left, rate):
    """The half-second with the most energy.

    TCG pads the guest's slow stretches with host-side silence, so a
    measurement over the whole file is mostly measuring the gaps. The
    loudest window is where both programs were actually playing.
    """
    w = rate // 2
    if len(left) <= w:
        return left
    best, best_e = left[:w], -1.0
    for i in range(0, len(left) - w, rate // 10):
        seg = left[i:i + w]
        e = sum(float(x) * x for x in seg)
        if e > best_e:
            best, best_e = seg, e
    return best


def wait_serial(sock, timeout_s=60):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            dbg = DebugConsole(sock)
            if dbg.send("sh help"):
                return dbg
            dbg.close()
        except OSError:
            pass
        time.sleep(1.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    port_guard.add_instance_args(ap)
    ap.add_argument("--card", choices=("ac97", "hda"), default="ac97")
    ap.add_argument("--keep", action="store_true",
                    help="keep the recorded wav and print its path")
    ap.add_argument("--positive-control", action="store_true",
                    help="stop the daemon first, so only one tone can play")
    args = ap.parse_args()

    port_guard.resolve_instance(args, "soundd_test")
    n = args.instance if args.instance is not None else 0
    sock = args.sock or port_guard.instance_sock(n)

    res = Result()
    tmp = tempfile.mkdtemp(prefix="soundd_test_")
    wav_path = os.path.join(tmp, "out.wav")
    img = os.path.join(tmp, "disk.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", "disk.img", img],
                   cwd=REPO, check=True)

    def halt():
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)

    halt()
    boot = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                           "--instance", str(n), "--disk", img,
                           "--audio", args.card,
                           "--audio-wav", wav_path, "start"], cwd=REPO)
    if boot.returncode != 0:
        res.check(f"the guest booted with an {args.card} attached", False)
        return 1

    try:
        dbg = wait_serial(sock)
        if not res.check("the serial console answers", dbg is not None):
            return 1

        # IT SHIPS DISABLED (docs/bugs.md), so the tool turns it on
        # rather than assuming a machine that has it running.
        dbg.send("sh service enable soundd")
        dbg.send("sh service start soundd")
        time.sleep(2.0)

        services = dbg.send("sh service list") or ""
        row = next((ln for ln in services.splitlines()
                    if ln.split()[:1] == ["soundd"]), "")
        res.check("soundd is running", "running" in row,
                  row.strip() or "no soundd row in `service list`")

        shm = dbg.send("sh dmesg") or ""
        res.check("it claimed the hardware stream",
                  "soundd: serving" in shm,
                  next((ln.strip() for ln in shm.splitlines()
                        if "soundd:" in ln), "no soundd line"))

        if args.positive_control:
            dbg.send("sh service stop soundd")
            time.sleep(1.0)
            print("  (positive control: the daemon is stopped)")

        # Both at once. Backgrounded so the second starts while the
        # first is still playing -- which is the entire experiment.
        # NO `&`: `spawn` at the ring-0 shell already returns as soon as
        # the child exists, and the kernel shell has no job control -- a
        # trailing ampersand arrives as a second ARGUMENT and the child
        # opens a file with that name. It failed exactly that way once,
        # as "cannot open file" with the daemon working perfectly.
        dbg.send("sh spawn /bin/aplay /tests/sine1k.wav")
        dbg.send("sh spawn /bin/aplay /tests/sine440.wav")

        # 1.5s of audio, and TCG stretch means several wall seconds.
        time.sleep(12.0)

        # THEY MUST ALSO FINISH. Being audible is not the whole contract:
        # a client whose write cursor the daemon overtakes reads "no room"
        # forever and never exits.
        #
        # **THIS CHECK DOES NOT REPRODUCE THAT BUG**, and the honesty is
        # the point: with the guard deliberately removed it stayed GREEN,
        # because two equal 1.5s tones started together do not leave a
        # client idle-but-running long enough to be lapped. It reproduced
        # on the LAPTOP, with a 78s track and a short effect over it. So
        # this is a cheap regression tripwire, not coverage -- provoking
        # it here needs fixtures of very different lengths, which is on
        # docs/roadmap.md.
        ps = dbg.send("sh ps") or ""
        res.check("both players exited instead of stalling",
                  "aplay" not in ps,
                  next((ln.strip() for ln in ps.splitlines()
                        if "aplay" in ln), "") + " -- still running")
        dbg.close()
    finally:
        halt()

    rate, left = read_wav(wav_path)
    if not res.check("the host recorded something", rate > 0 and len(left) > rate // 2,
                     f"rate={rate} samples={len(left)}"):
        return 1

    seg = loudest_window(left, rate)
    a = goertzel(seg, rate, TONE_A_HZ)
    b = goertzel(seg, rate, TONE_B_HZ)
    c = goertzel(seg, rate, CONTROL_HZ)
    print(f"  energy at {TONE_A_HZ} Hz: {a:.3f}   {TONE_B_HZ} Hz: {b:.3f}"
          f"   {CONTROL_HZ} Hz (nobody): {c:.3f}")

    # The control is what makes the two above mean anything: it is the
    # same measurement at a frequency nothing played, so a threshold
    # both tones clear and it does not is a threshold measuring pitch.
    res.check(f"{CONTROL_HZ} Hz, which nobody played, is quiet", c < 0.05,
              f"{c:.3f} -- the measurement is not selective")

    if args.positive_control:
        res.check("with the mixer stopped, only ONE tone is present",
                  (a > 0.10) != (b > 0.10),
                  f"{TONE_A_HZ}={a:.3f} {TONE_B_HZ}={b:.3f} -- both present "
                  f"means the daemon was not really stopped")
    else:
        res.check(f"{TONE_A_HZ} Hz is present", a > 0.10, f"{a:.3f}")
        res.check(f"{TONE_B_HZ} Hz is present", b > 0.10, f"{b:.3f}")
        res.check("BOTH programs were audible in one recording",
                  a > 0.10 and b > 0.10,
                  "the mixer served one client and dropped the other")

    if args.keep:
        print(f"  recording: {wav_path}")
    print(f"soundd_test: {'PASS' if res.fails == 0 else 'FAIL'} -- "
          f"{res.checks - res.fails}/{res.checks} checks")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
