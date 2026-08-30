#!/usr/bin/env python3
"""tools/usb_audio_test.py -- USB Audio Class 1.0 playback, end to end,
judged on the HOST.

The same oracle tools/audio_test.py uses, pointed at a different bus:
the guest plays two seconds of A440 and QEMU's wav audiodev records
what the DEVICE emitted, so the frequency is measured by zero-crossing
count in code that shares nothing with the driver. What that catches
here and nothing else can: a configured isochronous endpoint that never
gets a packet records silence, a wrong packet size records the wrong
pitch, and a consumed-chunk zeroing that stopped working records the
tone looping into the tail.

THREE PHASES, and the middle one is the reason the harness grew a
second recording:

  1. usb only     -- it binds, it plays, and unplugging it is survivable.
  2. usb + ac97   -- WHICH device played. Each card records to its own
                     wav file, so "the tone is in the USB file and not
                     the AC97 one" is an assertion; with both on one
                     audiodev the file holds their mix and says nothing.
  3. a reboot     -- the choice came back from /etc.

    python3 tools/usb_audio_test.py [--instance N] [--keep]

On demand, not in the gate: it boots its own guest with extra hardware.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from audio_test import measure, wait_serial  # noqa: E402  -- one oracle, not two
from qmp_test import QMPSession  # noqa: E402


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'ok   ' if ok else 'FAIL '} {name}" +
              (f"  -- {detail}" if detail else ""))
        (self.passes if ok else self.fails).append(name)
        return ok


class Guest:
    """One booted guest with sound hardware, on a copy of disk.img."""

    def __init__(self, instance, disk):
        self.instance = instance
        self.disk = disk

    def start(self, audio, wav, wav2=None):
        self.stop()
        cmd = [sys.executable, os.path.join(HERE, "vm.py"),
               "--instance", str(self.instance), "--disk", self.disk,
               "--audio", audio, "--audio-wav", wav]
        if wav2:
            cmd += ["--audio-wav2", wav2]
        return subprocess.run(cmd + ["start"], cwd=REPO).returncode == 0

    def stop(self):
        subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                        "--instance", str(self.instance), "stop"],
                       capture_output=True)

    def sock(self):
        n = self.instance
        return os.path.join(REPO, ".vm.serial" if n == 0 else f".vm.{n}.serial")

    def qmp(self):
        return QMPSession(port=4445 + self.instance)


def play_tone(dbg, res, label, timeout=90):
    """Run /tests/tone and wait for the verdict it writes to a FILE.

    Through the filesystem rather than the console: a spawned child's
    output reaches the serial capture unreliably, and TCG stretch means
    2 s of audio can take several wall seconds.
    """
    dbg.send("sh rm /tmp/tone_done")
    dbg.send("sh spawn /tests/tone")
    out = ""
    deadline = time.time() + timeout
    while "played 440Hz" not in out and time.time() < deadline:
        time.sleep(1.5)
        out = dbg.send("sh cat /tmp/tone_done") or ""
    return res.check(f"the tone played ({label})", "played 440Hz" in out,
                     out.strip()[-100:])


def phase_usb_only(g, res, wav):
    print("\nphase 1: a USB audio device on its own")
    if not res.check("the guest boots with usb-audio attached",
                     g.start("usb", wav)):
        return
    try:
        dbg = wait_serial(g.sock())
        if not res.check("the serial console answers", dbg is not None):
            return

        dmesg = dbg.send("sh dmesg") or ""
        res.check("the class driver bound the device",
                  "bound as usb-audio" in dmesg,
                  next((ln.strip() for ln in dmesg.splitlines()
                        if "usb-audio" in ln), "no usb-audio line"))
        # A control transfer that fails used to hang the waiter for two
        # million polls and leave ep0 halted, so its absence is checked
        # rather than assumed -- it is the bug that made the volume
        # control look impossible.
        res.check("...with no control transfer timing out",
                  "control transfer timed out" not in dmesg)
        res.check("...and the feature unit answered, so volume exists",
                  ", volume" in dmesg)

        # The KTESTs that skip on every other boot. "0 skipped" is the
        # load-bearing half (the ahci_test lesson): the usb-audio suite
        # is pure descriptor parsing and runs everywhere, but `sound`
        # covers the device list and would skip with a stream open.
        for suite in ("usb-audio", "sound"):
            kt = dbg.send(f"sh ktest {suite}") or ""
            res.check(f"ktest {suite} passes with 0 skipped",
                      "PASSED" in kt and ", 0 skipped" in kt,
                      kt.splitlines()[-1].strip() if kt.strip() else "no output")

        lsdev = dbg.send("lsdev") or ""
        res.check("lsdev names it as the active sound device",
                  "usb-audio  [active]" in lsdev,
                  next((ln.strip() for ln in lsdev.splitlines()
                        if "usb-audio" in ln), "no sound line"))

        play_tone(dbg, res, "usb only")

        # Isochronous underruns are counted separately from errors, so a
        # stream that merely stuttered can be told from one that failed.
        usb = dbg.send("usb") or ""
        ok_line = next((ln for ln in usb.splitlines() if "xfer ok" in ln), "")
        res.check("the endpoint moved packets and reported no bad ones",
                  " bad 0 " in ok_line, ok_line.strip())

        # UNPLUGGED, which is the state an AC'97 cannot reach.
        qmp = g.qmp()
        qmp.device_del("usbaud")
        time.sleep(3)
        dmesg = dbg.send("sh dmesg") or ""
        res.check("unplugging it is reported, not ignored",
                  "sound: usb-audio removed" in dmesg)
        lsdev = dbg.send("lsdev") or ""
        res.check("...and the machine is left with no sound device",
                  "Sound: no device" in lsdev,
                  next((ln.strip() for ln in lsdev.splitlines()
                        if "Sound" in ln), "no sound line"))
        dbg.close()
    finally:
        g.stop()


def phase_two_cards(g, res, ac97_wav, usb_wav):
    print("\nphase 2: two cards, and which one plays")
    if not res.check("the guest boots with usb-audio AND an AC97",
                     g.start("both", ac97_wav, usb_wav)):
        return
    try:
        dbg = wait_serial(g.sock())
        if not res.check("the serial console answers (two cards)",
                         dbg is not None):
            return
        dbg.send("sh config set audio_device auto")

        lsdev = dbg.send("lsdev") or ""
        res.check("both cards are registered", "ac97" in lsdev and
                  "usb-audio" in lsdev)
        # FIRST DISCOVERED wins with no choice made -- usb_init() runs
        # before ac97_init(), so this is the USB device. The assertion
        # is on the marker, not on the order of the lines.
        res.check("...and with no choice made the FIRST one is active",
                  "usb-audio  [active]" in lsdev,
                  " | ".join(ln.strip() for ln in lsdev.splitlines()
                             if "ac97" in ln or "usb-audio" in ln))
        play_tone(dbg, res, "auto -> usb")

        # Now choose the other one. The setting is what the volume
        # popup's device list writes, so this is that path.
        out = dbg.send("sh config set audio_device ac97") or ""
        res.check("the device can be chosen by name", "invalid" not in out.lower(),
                  out.strip()[-80:])
        lsdev = dbg.send("lsdev") or ""
        res.check("...and the choice moves the active marker",
                  "ac97  [active]" in lsdev and "usb-audio  [active]" not in lsdev,
                  " | ".join(ln.strip() for ln in lsdev.splitlines()
                             if "ac97" in ln or "usb-audio" in ln))
        play_tone(dbg, res, "chosen -> ac97")
        dbg.close()
    finally:
        g.stop()


def phase_switch_mid_stream(g, res, ac97_wav, usb_wav):
    """Choosing a device WHILE a stream is running moves the audio.

    THE FAILURE THIS EXISTS FOR is silence that never ends, and it is
    invisible to every check that opens a stream after choosing: ring 3
    starts the engine once and never again (usnd_sink_dev.c), so a
    switch that stopped the old card and left the new one idle muted the
    app for the life of its stream -- reported from a real session, with
    the Audio Player, which holds its sink open across tracks.

    The measurement is the split: the tone must appear on the FIRST
    card up to the switch and on the SECOND card after it. Its control
    (dropping the resume from activate()) puts 1.20 s on the USB card
    and 0.00 s on the AC97 one, which is the bug exactly.
    """
    print("\nphase 4: switching devices while the stream is running")
    if not res.check("the guest boots for the mid-stream switch",
                     g.start("both", ac97_wav, usb_wav)):
        return
    try:
        dbg = wait_serial(g.sock())
        if not res.check("the serial console answers (mid-stream)",
                         dbg is not None):
            return
        dbg.send("sh config set audio_device auto")
        dbg.send("sh rm /tmp/tone_done")
        dbg.send("sh spawn /tests/tone")
        # Far enough in that the first card is demonstrably playing, and
        # early enough that the tone is still going. Under TCG the
        # guest runs slower than wall clock, so 2 s of audio takes
        # several wall seconds and this lands comfortably inside it.
        time.sleep(1.2)
        dbg.send("sh config set audio_device ac97")
        out = ""
        deadline = time.time() + 90
        while "played 440Hz" not in out and time.time() < deadline:
            time.sleep(1.5)
            out = dbg.send("sh cat /tmp/tone_done") or ""
        res.check("the tone ran to completion across the switch",
                  "played 440Hz" in out, out.strip()[-60:])
        dbg.close()
    finally:
        g.stop()


def phase_persisted(g, res, wav, usb_wav):
    print("\nphase 3: the choice survives a reboot")
    if not res.check("the guest reboots with both cards",
                     g.start("both", wav, usb_wav)):
        return
    try:
        dbg = wait_serial(g.sock())
        if not res.check("the serial console answers (reboot)", dbg is not None):
            return
        # The FIRST LINE, not a substring of everything the console has
        # said: boot chatter mentions ac97 too, so `"ac97" in reply`
        # passed on a guest whose setting had not been restored at all.
        reply = (dbg.send("sh config get audio_device") or "").strip()
        got = next((ln.strip() for ln in reply.splitlines() if ln.strip()), "")
        res.check("the chosen device came back from /etc", got == "ac97", repr(got))
        lsdev = dbg.send("lsdev") or ""
        res.check("...and it is the active one on this boot",
                  "ac97  [active]" in lsdev,
                  " | ".join(ln.strip() for ln in lsdev.splitlines()
                             if "ac97" in ln or "usb-audio" in ln))
        # Put it back, so a re-run of this tool starts from `auto` and
        # the disk image is not left configured by a test.
        dbg.send("sh config set audio_device auto")
        dbg.close()
    finally:
        g.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true",
                    help="keep the recorded wav files and print their paths")
    args = ap.parse_args()

    res = Result()
    tmp = tempfile.mkdtemp(prefix="usb_audio_test_")
    usb_only = os.path.join(tmp, "usb_only.wav")
    ac97_wav = os.path.join(tmp, "two_ac97.wav")
    usb_wav = os.path.join(tmp, "two_usb.wav")
    boot3_a = os.path.join(tmp, "boot3_ac97.wav")
    boot3_u = os.path.join(tmp, "boot3_usb.wav")
    mid_a = os.path.join(tmp, "mid_ac97.wav")
    mid_u = os.path.join(tmp, "mid_usb.wav")
    img = os.path.join(tmp, "disk.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", "disk.img", img],
                   cwd=REPO, check=True)

    g = Guest(args.instance, img)
    phase_usb_only(g, res, usb_only)
    phase_two_cards(g, res, ac97_wav, usb_wav)
    phase_persisted(g, res, boot3_a, boot3_u)
    phase_switch_mid_stream(g, res, mid_a, mid_u)

    # --- the host-side oracle -------------------------------------------
    print("\nthe recordings, measured on the host")
    rate, secs, hz, peak, tone = measure(usb_only)
    res.check("the USB device emitted real signal",
              tone >= 0.5 and peak > 4000,
              f"{secs:.1f}s recorded at {rate}Hz, {tone:.2f}s of tone, peak {peak}")
    res.check("...and it is A440, measured with no guest code",
              abs(hz - 440.0) < 22, f"measured {hz:.1f}Hz")
    # 2.00s measured on a healthy build. Long means the ring looped --
    # the consumed-chunk zeroing reaching the USB path is not implied by
    # the AC'97's, since this driver COPIES out of the ring and reports
    # its own position.
    res.check("...and every sample came out exactly ONCE (no ring loop)",
              1.6 < tone < 2.3, f"{tone:.2f}s of tone for 2s generated")

    _, _, hz_u, peak_u, tone_u = measure(usb_wav)
    _, _, hz_a, peak_a, tone_a = measure(ac97_wav)
    # THE LOAD-BEARING PAIR. A build that ignored the setting would put
    # all four seconds in the USB file and leave the AC97 one silent --
    # which is exactly what "it plays" alone cannot distinguish.
    res.check("with two cards, the tone went to the ACTIVE one",
              1.6 < tone_u < 2.3 and abs(hz_u - 440.0) < 22,
              f"usb file: {tone_u:.2f}s at {hz_u:.1f}Hz, peak {peak_u}")
    res.check("...and choosing the other one MOVED the audio to it",
              1.6 < tone_a < 2.3 and abs(hz_a - 440.0) < 22,
              f"ac97 file: {tone_a:.2f}s at {hz_a:.1f}Hz, peak {peak_a}")

    _, _, hz_mu, _, tone_mu = measure(mid_u)
    _, _, hz_ma, _, tone_ma = measure(mid_a)
    # THE SPLIT. Both halves matter: the first card must have been
    # playing (or the switch proved nothing) and the second must pick it
    # up (which is what the bug got wrong). Their SUM is not asserted --
    # the moment of the switch is wall-clock, and TCG's is elastic.
    res.check("a switch mid-stream leaves the first card playing up to it",
              tone_mu >= 0.4 and abs(hz_mu - 440.0) < 22,
              f"usb file: {tone_mu:.2f}s at {hz_mu:.1f}Hz")
    res.check("...and the stream CONTINUES on the newly chosen card",
              tone_ma >= 0.2 and abs(hz_ma - 440.0) < 22,
              f"ac97 file: {tone_ma:.2f}s at {hz_ma:.1f}Hz "
              f"(0.00s is the bug: the app is never told to restart)")

    if args.keep:
        print(f"  recordings kept in {tmp}")

    print(f"\nusb_audio_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
