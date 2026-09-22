#!/usr/bin/env python3
"""Measure what the G6 actually PLAYS, by recording it back on line in.

WHY THIS EXISTS
---------------
Every USB audio tool in this directory judges the driver. None of them
judge the SOUND. `usb_audio_test.py` cannot help -- QEMU's emulated
`usb-audio` plays nothing (see `docs/bugs.md`), so the only device that
exercises the real path is a passed-through Creative G6, and a
passed-through DAC plays out its own jack rather than into QEMU's wav
capture. That left a person listening as the only oracle, which is how
`docs/bugs.md` ended up recording "crackles" as a finding.

"It crackles" is not a measurement, and the entry it sits in is a
warning about exactly that: the packets-per-second number went UP while
the audio got WORSE. This tool closes the loop -- a cable from the G6's
headphone out into the motherboard's line in -- so the output can be
recorded and counted instead of described.

WHAT IT MEASURES, given a steady tone
-------------------------------------
A sine obeys x[n] = 2cos(w)x[n-1] - x[n-2] exactly, so every sample is
predictable from the two before it. Anything the driver does wrong to
the STREAM -- a gap, a repeated packet, a torn buffer -- breaks that
recurrence and shows up as a residual spike. That gives a glitch COUNT
and a glitch RATE, which is what "crackles" was missing.

It also reports dropouts (the envelope collapsing) separately from
clicks (the waveform tearing while the level holds), because they have
different causes: a dropout is the ring running dry, a click is a
discontinuity where the samples were there but wrong.

THE CABLE IS NOT NORMALLY CONNECTED
-----------------------------------
It is patched in for a measurement and unplugged again, so this tool
SKIPS rather than fails when the loop is open. That distinction is the
whole point: an absent cable and a silent guest are both flat ADC hiss,
and reporting "no cable" as a failure would convict the OS of a fault
in the test rig. `--selftest` is the positive control -- it plays a tone
from the HOST and proves the loop carries signal before anything else
is believed. Run it BEFORE passing the G6 through to a guest; once
QEMU has the device the host cannot open it for playback.

    python3 tools/audio_loopback_test.py                  # selftest
    python3 tools/audio_loopback_test.py --record 10 -o cap.wav
    python3 tools/audio_loopback_test.py --analyse cap.wav
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import time
import wave

FS = 48000
HERE = os.path.dirname(os.path.abspath(__file__))

# The two cards, by the NAME in /proc/asound/cards rather than by index:
# index depends on probe order and moves when a card is added.
PLAY_CARD = "G6"
CAP_CARD = "Generic"

# The G6's `Speaker` control is 0..128 in exact 0.5 dB steps, and 107
# (-10.5 dB) is the point where a -6 dBFS source comes back at -6 dBFS:
# unity gain through DAC, cable and ADC. Measured 2026-09-22; 119 is the
# last clean step and 128 clips at 34% THD.
G6_UNITY = 107

# Below this, the loop is open. A connected loop at unity returns the
# tone within a dB of where it went in; ADC hiss with no cable is about
# -66 dBFS, so -40 separates them with 25 dB to spare either way.
LOOP_PRESENT_DBFS = -40.0


def db(x):
    return -999.0 if x <= 1e-12 else 20 * math.log10(x)


def card_index(name):
    """Index of a card by name, or None. Names are stable; indices are not."""
    try:
        with open("/proc/asound/cards") as f:
            for line in f:
                # " 0 [G6             ]: USB-Audio - Sound BlasterX G6"
                if "[" in line and "]" in line:
                    idx = line.split("[")[0].strip()
                    nm = line.split("[")[1].split("]")[0].strip()
                    if nm == name and idx.isdigit():
                        return int(idx)
    except OSError:
        return None
    return None


def have_tools():
    from shutil import which
    return which("aplay") is not None and which("arecord") is not None


def amixer(card, *args):
    subprocess.run(["amixer", "-c", str(card)] + list(args),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def set_capture_gain(cap):
    """Zero the ALC892's capture gain.

    IT COMES UP AT +30 dB `Capture` AND +30 dB `Line Boost` -- 60 dB on a
    line input being fed by a headphone amp. Left alone it clips into
    mush, which reads as a broken cable rather than as a mixer setting.
    `Input Source` has TWO instances and the second defaults to Rear Mic.
    """
    amixer(cap, "sset", "Input Source,0", "Line")
    amixer(cap, "sset", "Input Source,1", "Line")
    amixer(cap, "sset", "Line Boost", "0")
    amixer(cap, "sset", "Capture", "0dB", "cap")


def write_tone(path, seconds, freq=1000.0, amp=0.5):
    w = wave.open(path, "wb")
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(FS)
    w.writeframes(b"".join(
        struct.pack("<hh", s, s) for s in
        (int(amp * 32767 * math.sin(2 * math.pi * freq * i / FS))
         for i in range(int(seconds * FS)))))
    w.close()


def record(path, seconds, cap):
    subprocess.run(["arecord", "-D", f"plughw:{cap},0", "-f", "S16_LE",
                    "-r", str(FS), "-c", "2", "-d", str(int(seconds + 1)), path],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_wav(path):
    w = wave.open(path)
    n = w.getnframes()
    ch = w.getnchannels()
    pcm = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return [pcm[i * ch] / 32768.0 for i in range(n)]


def rms(s):
    return math.sqrt(sum(v * v for v in s) / len(s)) if s else 0.0


def goertzel(s, f):
    n = len(s)
    k = 2 * math.pi * f / FS
    c = 2 * math.cos(k)
    s1 = s2 = 0.0
    for i, x in enumerate(s):
        x *= 0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))   # Hann
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return 4 * math.hypot(s1 - s2 * math.cos(k), s2 * math.sin(k)) / n


def skip_first_second(sig):
    """Drop the ADC's start-up transient.

    Every capture opens with roughly -26 dBFS decaying over ~0.7 s. It is
    louder than the tone's noise floor, so anything that looks for a
    level finds it first and every later measurement lands in the wrong
    place.
    """
    return sig[FS:]


def estimate_freq(sig):
    """Dominant frequency from interpolated zero crossings.

    Cheap and accurate on a tone, and it needs no FFT. Returns None when
    the signal is too quiet or too broken to have a frequency.
    """
    a = rms(sig)
    if a < 1e-4:
        return None
    zc = [i for i in range(1, len(sig)) if sig[i - 1] < 0 <= sig[i]]
    if len(zc) < 20:
        return None

    def frac(i):
        p, q = sig[i - 1], sig[i]
        return i - 1 + (0 - p) / (q - p)

    span = frac(zc[-1]) - frac(zc[0])
    return (len(zc) - 1) * FS / span if span > 0 else None


def analyse(sig, label):
    """Count dropouts and clicks in a captured tone. Returns a dict."""
    out = {"label": label}
    sig = skip_first_second(sig)
    if len(sig) < FS // 2:
        out["error"] = "too short after dropping the start-up transient"
        return out

    # TRIM LEADING AND TRAILING SILENCE, or the analysis counts the quiet
    # before and after the tone as a dropout and the edges as clicks --
    # which is what this tool's own negative control caught first time
    # out. Only CONTIGUOUS silence at each end goes; a hole in the middle
    # is the thing being measured and must survive.
    tb = int(0.010 * FS)
    tenv = [rms(sig[i:i + tb]) for i in range(0, len(sig) - tb, tb)]
    if tenv:
        top = max(tenv)
        live = [i for i, e in enumerate(tenv) if e > top * 0.25]
        if live:
            # Plus a 25 ms guard: the first and last samples of a tone are
            # a hard edge, and an edge is a discontinuity like any other.
            # Guarding them is not hiding a glitch -- 25 ms at each end of
            # a multi-second capture cannot mask a crackle, and without it
            # every clean capture reports one click.
            g = int(0.025 * FS)
            sig = sig[live[0] * tb + g:max(live[0] * tb + g, (live[-1] + 1) * tb - g)]
    if len(sig) < FS // 2:
        out["error"] = "less than half a second of signal in the capture"
        return out

    lvl = rms(sig)
    out["level_dbfs"] = db(lvl)
    f0 = estimate_freq(sig)
    out["freq"] = f0
    if f0 is None or lvl < 1e-4:
        out["error"] = "no tone found -- nothing to measure"
        return out

    # --- dropouts: the envelope collapsing -------------------------------
    blk = int(0.010 * FS)
    env = [rms(sig[i:i + blk]) for i in range(0, len(sig) - blk, blk)]
    env_sorted = sorted(env)
    med = env_sorted[len(env_sorted) // 2]
    out["median_dbfs"] = db(med)
    # -12 dB below the median block is a hole, not a fluctuation.
    holes = [i for i, e in enumerate(env) if e < med * 0.25]
    runs = []
    for i in holes:
        if runs and i == runs[-1][1] + 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    out["dropouts"] = len(runs)
    out["dropout_ms"] = sum((b - a + 1) for a, b in runs) * blk / FS * 1000.0
    out["dropout_rate"] = len(runs) / (len(sig) / FS)
    out["worst_dropout_ms"] = max(((b - a + 1) * blk / FS * 1000.0
                                   for a, b in runs), default=0.0)

    # --- clicks: the waveform tearing while the level holds --------------
    # A sine satisfies x[n] = 2cos(w)x[n-1] - x[n-2] exactly, so the
    # residual is noise until something breaks the stream. This sees a
    # repeated or dropped packet that a level meter cannot.
    c = 2 * math.cos(2 * math.pi * f0 / FS)
    res = [abs(sig[n] - (c * sig[n - 1] - sig[n - 2])) for n in range(2, len(sig))]
    rs = sorted(res)
    medres = rs[len(rs) // 2]
    # Robust threshold: well above the noise, well below a real tear.
    thr = max(20 * medres, 0.02 * lvl * math.sqrt(2))
    out["residual_floor"] = db(medres)
    out["worst_residual"] = db(max(res))
    bad = [n for n, r in enumerate(res) if r > thr]
    ev = []
    for n in bad:
        # Group samples within 5 ms into one audible click.
        if ev and n - ev[-1][1] < 0.005 * FS:
            ev[-1][1] = n
        else:
            ev.append([n, n])
    out["clicks"] = len(ev)
    out["click_rate"] = len(ev) / (len(sig) / FS)
    out["seconds"] = len(sig) / FS
    return out


def report(a):
    print(f"\n-- {a['label']} --")
    if "error" in a:
        print(f"   {a['error']}"
              + (f"   (level {a['level_dbfs']:.1f} dBFS)" if "level_dbfs" in a else ""))
        return
    print(f"   {a['seconds']:.1f} s at {a['freq']:.2f} Hz, "
          f"level {a['level_dbfs']:.2f} dBFS (median block {a['median_dbfs']:.2f})")
    print(f"   dropouts  {a['dropouts']:4d}   {a['dropout_rate']:6.2f}/s   "
          f"{a['dropout_ms']:8.1f} ms total, worst {a['worst_dropout_ms']:.1f} ms")
    print(f"   clicks    {a['clicks']:4d}   {a['click_rate']:6.2f}/s   "
          f"residual floor {a['residual_floor']:.1f} dB, worst {a['worst_residual']:.1f} dB")


def loop_probe(play, cap, tmp):
    """Play a tone on the G6 and see whether it comes back. The control."""
    tone = os.path.join(tmp, "probe_tone.wav")
    got = os.path.join(tmp, "probe_rec.wav")
    write_tone(tone, 2.0)
    rec = subprocess.Popen(
        ["arecord", "-D", f"plughw:{cap},0", "-f", "S16_LE", "-r", str(FS),
         "-c", "2", "-d", "3", got],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.2)          # clear the ADC start-up transient first
    subprocess.run(["aplay", "-D", f"plughw:{play},0", tone],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rec.communicate()
    sig = skip_first_second(read_wav(got))
    return db(goertzel(sig[:int(1.2 * FS)], 1000.0)), got


# --- the crackle A/B -------------------------------------------------------
# WHY THIS IS A MODE AND NOT A SEPARATE TOOL: the finding it exists to
# reproduce is a COMPARISON, and a number from one driver alone is what
# misled this bug for weeks. Both legs must come off one cable, one
# stimulus and one analyser, or the difference is not attributable.

VM = os.path.join(HERE, "vm.py")


def vm(*args, timeout=300):
    return subprocess.run([sys.executable, VM] + list(args),
                          capture_output=True, text=True, timeout=timeout).stdout


def make_tone_mp3(path, seconds, freq=1000.0):
    """A steady tone, encoded -- so the DECODER sits in the producer.

    The stimulus is the whole point. A WAV of this same tone is pristine
    through both drivers; it is the decode cost in `aplay` that brings
    the crackle on, so an uncompressed fixture measures nothing.
    """
    wav = path[:-4] + ".wav"
    write_tone(wav, seconds, freq=freq)
    r = subprocess.run(["lame", "-b", "128", "--quiet", wav, path],
                       capture_output=True)
    return r.returncode == 0


def leg(tag, guest_path, seconds, instance):
    """Play the stimulus in the guest and measure what the G6 emitted."""
    out = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"crackle_{tag}.wav")
    rec = subprocess.Popen(
        [sys.executable, os.path.abspath(__file__), "--record",
         str(seconds + 12), "-o", out, "--keep-gain", "--quiet-analyse"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)                      # past the ADC start-up transient
    vm("--instance", str(instance), "exec", f"spawn /bin/aplay {guest_path}")
    rec.wait()
    return analyse(read_wav(out), tag)


def crackle(args):
    """Measure the ring-3 driver against the in-kernel one, same file."""
    from shutil import which
    if not which("lame"):
        print("SKIP: lame is not installed -- it encodes the stimulus, and a "
              "check that cannot run on a clean checkout is one people learn "
              "to skim past")
        return 0
    cap = card_index(CAP_CARD)
    play = card_index(PLAY_CARD)
    if cap is None or play is None:
        print(f"SKIP: need both {PLAY_CARD!r} and {CAP_CARD!r} on the host -- "
              f"the G6 must be plugged in and NOT already passed through")
        return 0
    set_capture_gain(cap)
    amixer(play, "sset", "Speaker", str(G6_UNITY))

    # THE CABLE IS PROVEN BEFORE THE GUEST TAKES THE DEVICE, because once
    # QEMU holds the G6 the host cannot play through it to check.
    lvl, _ = loop_probe(play, cap, os.environ.get("TMPDIR", "/tmp"))
    print(f"loop probe: 1 kHz returned at {lvl:.2f} dBFS")
    if lvl < LOOP_PRESENT_DBFS:
        print("SKIP: the loop is OPEN -- patch the G6 headphone-out to line-in "
              "cable in and re-run")
        return 0

    mp3 = os.path.join(os.environ.get("TMPDIR", "/tmp"), "crackle_tone.mp3")
    print(f"encoding a {args.seconds:.0f} s 1 kHz tone as MP3 ...")
    if not make_tone_mp3(mp3, args.seconds):
        print("SKIP: lame failed to encode the stimulus")
        return 0

    inst = str(args.instance)
    print("booting a guest with the G6 passed through ...")
    vm("--instance", inst, "--usb-host", "041e:3256", "--usb", "xhci",
       "--audio", "none", "start", timeout=400)
    try:
        vm("--instance", inst, "put", mp3, "/tmp/crackle.mp3")
        # IN-KERNEL FIRST, while it still owns the device: it is the
        # control, and it has to be taken before snddrv claims the card.
        print("leg 1/2: the in-kernel usb-audio driver ...")
        a = leg("kernel", "/tmp/crackle.mp3", args.seconds, inst)
        print("leg 2/2: the ring-3 usbaudio.so ...")
        vm("--instance", inst, "exec", "spawn /bin/snddrv --driver usbaudio")
        time.sleep(1.5)
        b = leg("ring3", "/tmp/crackle.mp3", args.seconds, inst)
    finally:
        vm("--instance", inst, "stop", timeout=120)

    report(a)
    report(b)
    if "error" in a or "error" in b:
        print("\nINCOMPLETE: a leg produced no audio, so there is nothing to "
              "compare. Re-run; if it repeats, check that snddrv bound the G6.")
        return 1
    print("\n-- the comparison, which is the whole point --")
    print(f"   clicks/s   in-kernel {a['click_rate']:7.2f}   "
          f"ring-3 {b['click_rate']:7.2f}   ratio {b['click_rate']/max(a['click_rate'],1e-9):5.1f}x")
    print(f"   wall clock in-kernel {a['seconds']:7.1f} s ring-3 {b['seconds']:7.1f} s "
          f"for {args.seconds:.0f} s of material")
    # A REPORT, NOT A GATE. The crackle is a known open bug (docs/bugs.md);
    # a check that is permanently red is one people learn to ignore. What
    # this must catch is the rig going wrong, not the bug still existing.
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--record", type=float, metavar="SECONDS",
                    help="capture only, while something ELSE plays (a guest "
                         "holding the G6 through USB passthrough)")
    ap.add_argument("-o", "--out", default="loopback.wav",
                    help="where --record writes (default loopback.wav)")
    ap.add_argument("--analyse", "--analyze", dest="analyse", metavar="WAV",
                    help="analyse a capture taken earlier")
    ap.add_argument("--selftest", action="store_true",
                    help="the default: host plays, host records, loop proven")
    ap.add_argument("--keep-gain", action="store_true",
                    help="do not touch the mixer (the caller set it)")
    ap.add_argument("--crackle", action="store_true",
                    help="the A/B: the same MP3 through the in-kernel driver "
                         "and the ring-3 one, on one cable and one analyser")
    ap.add_argument("--seconds", type=float, default=60.0,
                    help="length of the --crackle stimulus (default 60)")
    ap.add_argument("--instance", type=int, default=0,
                    help="vm.py slot for --crackle (see CLAUDE.md on ports)")
    ap.add_argument("--quiet-analyse", action="store_true",
                    help="record without printing the analysis (internal)")
    args = ap.parse_args()

    if args.crackle:
        return crackle(args)

    tmp = os.environ.get("TMPDIR", "/tmp")

    if args.analyse:
        report(analyse(read_wav(args.analyse), os.path.basename(args.analyse)))
        return 0

    if not have_tools():
        print("SKIP: aplay/arecord not on PATH -- alsa-utils is not installed")
        return 0

    cap = card_index(CAP_CARD)
    if cap is None:
        print(f"SKIP: no capture card named {CAP_CARD!r} in /proc/asound/cards")
        return 0
    if not args.keep_gain:
        set_capture_gain(cap)

    # --- capture-only: the guest is playing, we are only listening -------
    if args.record:
        print(f"recording {args.record:.0f} s from card {cap} ({CAP_CARD}) "
              f"line in -> {args.out}")
        record(args.out, args.record, cap)
        if args.quiet_analyse:
            return 0
        a = analyse(read_wav(args.out), os.path.basename(args.out))
        report(a)
        if "error" in a:
            print("\n  NOTE: no tone in the capture. Before reading that as a "
                  "silent guest,\n  check the cable is patched in -- an open "
                  "loop looks identical.")
        return 0

    # --- selftest: the positive control ----------------------------------
    play = card_index(PLAY_CARD)
    if play is None:
        print(f"SKIP: no playback card named {PLAY_CARD!r} -- the G6 is "
              f"unplugged, or already passed through to a guest")
        return 0

    print(f"selftest: card {play} ({PLAY_CARD}) out -> card {cap} ({CAP_CARD}) in")
    if not args.keep_gain:
        amixer(play, "sset", "Speaker", str(G6_UNITY))
    lvl, got = loop_probe(play, cap, tmp)
    print(f"loop probe: 1 kHz returned at {lvl:.2f} dBFS "
          f"(threshold {LOOP_PRESENT_DBFS:.0f})")
    if lvl < LOOP_PRESENT_DBFS:
        print("\nSKIP: the loop is OPEN -- no tone came back.\n"
              "  The G6 headphone-out -> line-in cable is not normally\n"
              "  connected. Patch it in and re-run. This is a SKIP and not\n"
              "  a failure on purpose: an absent cable and a silent source\n"
              "  are the same flat hiss, and calling it red would convict\n"
              "  the wrong half.")
        return 0

    checks = []
    a = analyse(read_wav(got), "host selftest tone")
    report(a)
    # The host path is known good, so this doubles as the analyser's
    # NEGATIVE control: a clean tone must produce no glitches. If these
    # fire here, the detector is wrong, not the audio.
    checks.append(("the loop carries the tone", lvl > LOOP_PRESENT_DBFS))
    checks.append(("level is within 2 dB of unity", abs(lvl + 6.0) < 2.0))
    checks.append(("a clean host tone shows no dropouts", a.get("dropouts", 1) == 0))
    checks.append(("a clean host tone shows no clicks", a.get("clicks", 1) == 0))
    checks.append(("frequency is 1000 Hz within 1 Hz",
                   a.get("freq") is not None and abs(a["freq"] - 1000.0) < 1.0))

    print()
    for name, ok in checks:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}")
    npass = sum(1 for _, ok in checks if ok)
    print(f"\naudio_loopback: {npass}/{len(checks)} checks passed")
    return 0 if npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
