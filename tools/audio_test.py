#!/usr/bin/env python3
"""tools/audio_test.py -- AC97 and the PCM stream, end to end, judged
on the HOST.

Nothing else boots audio hardware: every other tool (and `make test`)
runs with no -device AC97, so the ac97 KTESTs skip everywhere but here
-- which makes "0 skipped" a load-bearing assertion, the ahci_test
lesson. The guest plays two seconds of A440 (/tests/tone) and QEMU's
wav audiodev records what the DEVICE emitted to a host file; the
frequency is then MEASURED there by zero-crossing count. That oracle
shares no code with the driver, the core, or the tone generator -- the
regex_hostcheck/fat32_test move -- and it is what "it didn't crash"
cannot fake: a dead DMA engine records silence, a wrong rate records
the wrong pitch, and a broken consumed-chunk zeroing records the tone
looping into the tail instead of going quiet.

TWO BOOTS, each with its OWN recording, because one wav file cannot
hold two tones and be measured by frequency. The first plays
/tests/tone (the raw ring, no file involved); the second plays
/tests/sine1k.wav through /bin/aplay, which is the whole of
userland/lib/usnd.h -- the WAV parser, the 44.1 -> 48 kHz resampler,
the mixer and the sink. That second one is the sharper check: the
fixture is 44.1 kHz, so a build that did not resample at all would play
it 8.8% sharp (1088 Hz), which the tolerance here is set to catch.

    python3 tools/audio_test.py [--instance N] [--keep] [--card ac97|hda]

`--card hda` boots QEMU's `ich9-intel-hda` + `hda-output` instead of the
AC97 and expects the `hda` driver and KTESTs -- the same oracle, pointed
at the other PCI sound driver. Everything after the driver line is the
core and the library, and it is the same code either way.

On demand, not in the gate: it boots its own guest with extra hardware.
"""
import argparse
import os
import subprocess
import sys
import struct
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from gui_debug import DebugConsole  # noqa: E402


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'ok   ' if ok else 'FAIL '} {name}" +
              (f"  -- {detail}" if detail else ""))
        (self.passes if ok else self.fails).append(name)
        return ok


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


def measure(wav_path):
    """(rate, seconds, tone_hz, tone_peak, tail_peak) from the host wav.

    PARSED BY HAND, canonical 44-byte header: QEMU's wav backend
    finalises the RIFF size fields only on a clean device close, and a
    stopped VM leaves them zero -- python's wave module refuses that
    while every field this needs (rate, channels, width, and the
    samples themselves) is present and correct.

    MEASURED PER BURST, not over the whole file: under TCG the guest
    runs slower than wall clock in stretches, and QEMU's recorder pads
    the shortfall with host-side SILENCE -- so the tone arrives as
    correct-pitch bursts separated by gaps that say nothing about the
    guest (CLAUDE.md's "TCG says nothing about speed" class, observed
    here as a clean 440 chopped into thirds). Frequency comes from
    zero crossings within the non-silent windows; the TOTAL tone
    duration is the anti-loop assertion -- every generated sample must
    come out exactly once, so ~2s of tone proves the consumed-chunk
    zeroing end to end, where a loop records far more and a dead DMA
    engine far less.
    """
    with open(wav_path, "rb") as f:
        hdr = f.read(44)
        raw = f.read()
    if len(hdr) < 44 or hdr[:4] != b"RIFF" or hdr[8:12] != b"WAVE":
        return 0, 0.0, 0.0, 0, 0
    ch = struct.unpack_from("<H", hdr, 22)[0]
    rate = struct.unpack_from("<I", hdr, 24)[0]
    width = struct.unpack_from("<H", hdr, 34)[0] // 8
    if width != 2 or ch < 1 or rate == 0 or len(raw) < 4:
        return rate, 0.0, 0.0, 0, 0
    total = len(raw) // 2
    samples = struct.unpack(f"<{total}h", raw[:total * 2])
    left = samples[::ch]
    secs = len(left) / rate

    # 50ms windows, classified tone/silent; frequency over the tone
    # windows only, skipping each burst's edge windows.
    w = rate // 20
    tone_windows = []
    for i in range(0, len(left) - w, w):
        seg = left[i:i + w]
        if max(abs(x) for x in seg) > 2000:
            tone_windows.append(seg)
    tone_secs = len(tone_windows) * w / rate
    body = tone_windows[1:-1] if len(tone_windows) > 2 else tone_windows
    crossings = 0
    frames = 0
    peak_v = 0
    for seg in body:
        for a, b in zip(seg, seg[1:]):
            if (a < 0) != (b < 0):
                crossings += 1
        frames += len(seg)
        peak_v = max(peak_v, max(abs(x) for x in seg))
    hz = crossings / 2.0 / (frames / rate) if frames else 0.0
    return rate, secs, hz, peak_v, tone_secs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true",
                    help="keep the recorded wav and print its path")
    ap.add_argument("--card", choices=("ac97", "hda"), default="ac97",
                    help="which PCI sound card to attach and which driver "
                         "to expect (default ac97)")
    args = ap.parse_args()
    card = args.card
    # The driver's boot line and the core's registration line name the
    # device: `ac97` for the AC'97, `hda0` for the first HDA controller.
    devname = "ac97" if card == "ac97" else "hda0"
    n = args.instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"

    res = Result()
    tmp = tempfile.mkdtemp(prefix="audio_test_")
    wav_path = os.path.join(tmp, "out.wav")
    wav2_path = os.path.join(tmp, "out_wavplay.wav")
    wav3_path = os.path.join(tmp, "out_mp3play.wav")
    img = os.path.join(tmp, "disk.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", "disk.img", img],
                   cwd=REPO, check=True)

    def boot(recording):
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)
        return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                               "--instance", str(n), "--disk", img,
                               "--audio", card,
                               "--audio-wav", recording, "start"], cwd=REPO)

    def halt():
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(n), "stop"], capture_output=True)

    if boot(wav_path).returncode != 0:
        res.check(f"the guest booted with an {card} attached", False)
        return 1

    try:
        dbg = wait_serial(sock)
        if not res.check("the serial console answers", dbg is not None):
            return 1

        dmesg = dbg.send("sh dmesg") or ""
        res.check("the driver claimed the controller",
                  f"{devname}:" in dmesg and f"sound: {devname} registered" in dmesg,
                  next((line.strip() for line in dmesg.splitlines()
                        if f"{devname}:" in line), f"no {devname} line"))

        # The KTESTs that skip on every other boot -- 0 skipped is the
        # load-bearing half (the ahci_test lesson).
        for suite in (card, "sound"):
            kt = dbg.send(f"sh ktest {suite}") or ""
            res.check(f"ktest {suite} passes with 0 skipped",
                      "PASSED" in kt and ", 0 skipped" in kt,
                      kt.splitlines()[-1].strip() if kt.strip() else "no output")

        # Asserted through the FILESYSTEM: a spawned child's console
        # lines reach the serial capture unreliably, so the app writes
        # its verdict to a file too. TCG stretch means 2s of audio can
        # take several wall seconds.
        dbg.send("sh rm /tmp/tone_done")
        dbg.send("sh spawn /tests/tone")
        out = ""
        deadline = time.time() + 45
        while "played 440Hz" not in out and time.time() < deadline:
            time.sleep(1.0)
            out = dbg.send("sh cat /tmp/tone_done") or ""
        res.check("the tone app played through the shared ring",
                  "played 440Hz" in out, out.strip()[-120:])
        dbg.close()
    finally:
        halt()

    # --- second boot: a WAV FILE through the whole library ---------------
    #
    # Its own recording, because frequency is how this is judged and one
    # file cannot carry two tones. /tests/sine1k.wav is 44.1 kHz stereo,
    # so this exercises the parser, the resampler, the mixer and the sink
    # in one go -- and the pitch is what catches a resampler that is not
    # running at all.
    aplay_out = ""
    if boot(wav2_path).returncode != 0:
        res.check("the guest rebooted for the WAV phase", False)
    else:
        try:
            dbg = wait_serial(sock)
            if res.check("the serial console answers (WAV phase)", dbg is not None):
                info = dbg.send("sh aplay -i /tests/sine1k.wav") or ""
                res.check("aplay reads the WAV header",
                          "wav" in info and "44100" in info,
                          info.strip()[-120:])
                # Bare name at a `#` prompt spawns AND waits, so this
                # returns once the file has played and drained. The
                # console's own timeout is raised for it: 1.5s of audio
                # is several wall seconds of TCG.
                was = dbg.timeout
                dbg.timeout = 90
                aplay_out = dbg.send("sh aplay /tests/sine1k.wav") or ""
                dbg.timeout = was
                res.check("aplay ran to completion",
                          "sine1k" in aplay_out and "44100" in aplay_out,
                          aplay_out.strip()[-120:])
                dbg.close()
        finally:
            halt()

    # --- third boot: the same tone as an MP3 ----------------------------
    #
    # THE POINT IS THAT THE ORACLE CANNOT TELL THE TWO APART. /tests/
    # sine1k.mp3 is the same 1 kHz tone as the WAV above, so the host
    # measures it the same way and the decoder has to produce the same
    # pitch through a completely different path -- Huffman, requantiser,
    # IMDCT and filterbank instead of a header and a memcpy. A frequency
    # check is also the one assertion a plausible-but-wrong decoder
    # cannot pass: garbage has no pitch.
    mp3_out = ""
    if boot(wav3_path).returncode != 0:
        res.check("the guest rebooted for the MP3 phase", False)
    else:
        try:
            dbg = wait_serial(sock)
            if res.check("the serial console answers (MP3 phase)", dbg is not None):
                info = dbg.send("sh aplay -i /tests/sine1k.mp3") or ""
                # `aplay -i` prints the codec's own `detail` line, which for
                # MP3 names the layer and bitrate rather than the rate (the
                # WAV check above matches on 44100 because a WAV's detail
                # does carry it). Matching the wrong field here reported a
                # working decoder as broken.
                res.check("aplay reads the MP3 header",
                          "mp3" in info and "Layer III" in info,
                          info.strip()[-120:])
                was = dbg.timeout
                dbg.timeout = 120      # decoding costs more than parsing
                mp3_out = dbg.send("sh aplay /tests/sine1k.mp3") or ""
                dbg.timeout = was
                res.check("aplay played the MP3 to completion",
                          "sine1k" in mp3_out, mp3_out.strip()[-120:])
                dbg.close()
        finally:
            halt()

    # --- the host-side oracle ------------------------------------------
    rate, secs, hz, tone_peak, tone_secs = measure(wav_path)
    res.check("the host recording contains real signal",
              tone_secs >= 0.5 and tone_peak > 4000,
              f"{secs:.1f}s recorded at {rate}Hz, {tone_secs:.2f}s of tone, "
              f"peak {tone_peak}")
    res.check("...and it is A440, measured with no guest code",
              abs(hz - 440.0) < 22,  # 5%: resampling and burst edges
              f"measured {hz:.1f}Hz")
    # 2.05s measured on a healthy build; a disabled consumed-chunk
    # zeroing measured 2.45s (the app's own STOP bounds the leak, and
    # the sound KTEST is the unit-level guard that reddens first).
    res.check("...and every sample came out exactly ONCE (no ring loop)",
              1.6 < tone_secs < 2.3, f"{tone_secs:.2f}s of tone for 2s generated")

    rate2, secs2, hz2, peak2, tone2 = measure(wav2_path)
    res.check("the WAV file reached the device",
              tone2 >= 0.5 and peak2 > 4000,
              f"{secs2:.1f}s recorded, {tone2:.2f}s of tone, peak {peak2}")
    # THE LOAD-BEARING CHECK. The fixture is 44.1 kHz and the device is
    # 48 kHz, so a build that skipped resampling plays it at
    # 1000 * 48000/44100 = 1088 Hz. 3% (30 Hz) accepts the interpolator's
    # own error and rejects that.
    res.check("...at the right PITCH, so the 44.1 -> 48 kHz resampler ran",
              abs(hz2 - 1000.0) < 30, f"measured {hz2:.1f}Hz, wanted 1000")
    res.check("...for its whole length, so nothing was cut or looped",
              1.2 < tone2 < 1.9, f"{tone2:.2f}s of tone for 1.5s of file")

    rate3, secs3, hz3, peak3, tone3 = measure(wav3_path)
    res.check("the MP3 reached the device",
              tone3 >= 0.5 and peak3 > 4000,
              f"{secs3:.1f}s recorded, {tone3:.2f}s of tone, peak {peak3}")
    # Same bar as the WAV: the file is 44.1 kHz and the device is 48 kHz,
    # so this catches a missing resample as well as a broken decode.
    res.check("...at 1 kHz, so the MP3 decoder produced the right pitch",
              abs(hz3 - 1000.0) < 30, f"measured {hz3:.1f}Hz, wanted 1000")
    res.check("...for its whole length",
              1.2 < tone3 < 1.9, f"{tone3:.2f}s of tone for 1.5s of file")

    if args.keep:
        print(f"  recordings kept: {wav_path}, {wav2_path}, {wav3_path}")

    print(f"\naudio_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
