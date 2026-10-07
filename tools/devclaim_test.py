#!/usr/bin/env python3
"""tools/devclaim_test.py -- a ring-3 process takes the sound card off
the kernel, reads its registers, and gives it back.

Stage 2 of docs/umdf-design.md. The KTESTs beside `dev_claim_take()`
cover the table and every refusal; `userland/tests/devclaim_test.c`
covers the syscalls from a process. Neither can cover the one thing the
stage exists for -- THE KERNEL LETTING GO OF A DEVICE IT WAS DRIVING --
because no other runner here attaches a device whose driver has a
`remove()` and which is safe to take. So this boots one.

WHAT A BROKEN VERSION WOULD STILL PASS. A claim that recorded a pid and
unbound nothing leaves the in-guest test green on any machine: its
unbound-device leg needs no driver to let go. So the assertions here are
about the DRIVER's state, read from outside the claim: `hda` bound at
boot, gone while the process holds it (its own registration line), back
afterwards. The round trip is the sound core's "hda0 registered" line
appearing AGAIN after the release -- counted as a difference from a
baseline taken before the claim, since the klog ring rotates -- and
nothing but a real remove-and-reprobe produces it. Ring 3 also has to
WRITE GCTL to bring the controller out of reset before its registers
read anything, which a mapping of the wrong page cannot fake.

    python3 tools/devclaim_test.py [--instance N] [--keep] [--no-card]

`--no-card` is the positive control: the same run with no controller
attached, where the in-guest test must SAY it skipped the leg. Without
it, a tool whose HDA assertions silently matched nothing would look
exactly like a pass.

On demand, not in the gate: it boots its own guest with extra hardware.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time
from harness import copy_disk  # noqa: E402

VERDICT_DONE = ("all checks passed", "FAILED --")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'ok   ' if ok else 'FAIL '} {name}" +
              (f"  -- {detail}" if detail else ""))
        (self.passes if ok else self.fails).append(name)
        return ok

    def report(self):
        print(f"\ndevclaim_test: {len(self.passes)} passed, {len(self.fails)} failed")
        for f in self.fails:
            print(f"  FAILED: {f}")
        return 1 if self.fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--no-card", action="store_true",
                    help="positive control: boot with no HD Audio controller")
    args = ap.parse_args()
    n = str(args.instance)
    res = Result()

    tmp = tempfile.mkdtemp(prefix="devclaim_test_")
    img = os.path.join(tmp, "disk.img")
    # A COPY, so a re-seed underneath this run cannot change what it
    # booted and the user's own QEMU keeps its write lock.
    copy_disk("disk.img", img, cwd=REPO)

    def vm(*argv):
        # BOTH STREAMS: vm.py labels some replies on stderr, and a
        # check that read stdout alone reported an empty dmesg as a
        # missing log line.
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                            "--instance", n, *argv],
                           cwd=REPO, capture_output=True, text=True, check=False)
        return r.stdout + r.stderr

    def verdict(deadline_s=60):
        """The spawned test's own verdict, polled until it TERMINATES.

        `spawn` returns as soon as the child exists, so a single read
        catches whatever had been written by then -- which here was
        everything but the epilogue, and read exactly like a test that
        died half way. usertest_run.py waits on the same artifact for
        the same reason.
        """
        deadline = time.time() + deadline_s
        text = ""
        while True:
            text = vm("exec", "cat /tmp/devclaim_test.out")
            if any(d in text for d in VERDICT_DONE) or time.time() >= deadline:
                return text
            time.sleep(1.0)

    vm("stop")
    boot = [sys.executable, os.path.join(HERE, "vm.py"), "--instance", n,
            "--disk", img]
    if not args.no_card:
        boot += ["--audio", "hda"]
    boot += ["start"]
    if subprocess.run(boot, cwd=REPO).returncode != 0:
        res.check("the guest booted", False)
        return 1

    try:
        # THE `snddrv` SERVICE DRIVES THE CARD FROM RING 3 SINCE BOOT, and
        # this test is about claiming it off the KERNEL's driver -- so
        # hand it back first: a polite stop releases with REBIND.
        if not args.no_card:
            vm("exec", "service stop snddrv")
        before = ""
        for _ in range(20):
            before = vm("exec", "lspci -k")
            if args.no_card or "kernel driver: hda" in before:
                break
            time.sleep(1.0)
        want_card = not args.no_card
        if not res.check("an HD Audio controller is present and bound"
                         if want_card else "no HD Audio controller is present",
                         ("kernel driver: hda" in before) == want_card,
                         "lspci -k said: " + " / ".join(
                             l.strip() for l in before.splitlines()
                             if "hda" in l) or "(no hda line)"):
            return res.report()

        # A dmesg BASELINE, because the klog ring holds a few hundred
        # lines and the boot messages may already have scrolled out by
        # the time the test runs. Every count below is a DIFFERENCE.
        base = vm("exec", "dmesg")

        vm("exec", "spawn /tests/devclaim_test")
        report = verdict()
        print("--- the guest's own verdict ---")
        print(report.strip())
        print("--- end ---")

        res.check("the in-guest test ran to a verdict",
                  any(d in report for d in VERDICT_DONE))
        res.check("no check failed in the guest", "all checks passed" in report)

        skipped = "no HD Audio controller here" in report
        # THE CONTROL. With a card the unbind leg must run; without one
        # it must say so. A tool that could not tell the two apart would
        # pass either way, which is the whole reason --no-card exists.
        res.check("the unbind leg ran" if want_card else
                  "the unbind leg was skipped and SAID so",
                  skipped != want_card,
                  "the guest noted the skip" if skipped else "the leg ran")

        if want_card:
            # The controller was handed over IN RESET, and ring 3 wrote
            # GCTL to bring it up -- a device changing its own status
            # bits is something a mapping of the wrong page cannot fake.
            res.check("the controller arrived in reset",
                      "handed over in reset" in report)
            res.check("ring 3 wrote GCTL and the device came out of reset",
                      "brought it out of reset" in report)
            res.check("ring 3 read HD Audio 1.0 out of the register file",
                      "HD Audio 1.0" in report)

            after = vm("exec", "lspci -k")
            res.check("the kernel driver has the controller again",
                      "kernel driver: hda" in after)

            dmesg = vm("exec", "dmesg")
            res.check("the bus logged the unbind", "pci: hda released" in dmesg)
            res.check("a process was recorded as the holder",
                      "claimed pci" in dmesg)
            # THE ROUND TRIP, and the assertion a fake claim cannot
            # reach: the sound core registers the card again when the
            # rebind re-probes it. Counted as a DIFFERENCE from the
            # baseline, so a rotated-out boot line cannot fail it.
            grew = (dmesg.count("sound: hda0 registered")
                    - base.count("sound: hda0 registered"))
            res.check("the sound core saw the card come back", grew >= 1,
                      f"hda0 registered {grew} more time(s) than before the claim")
        return res.report()
    finally:
        if not args.keep:
            vm("stop")


if __name__ == "__main__":
    sys.exit(main())
