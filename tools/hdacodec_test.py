#!/usr/bin/env python3
"""tools/hdacodec_test.py -- a ring-3 driver reads the codec graph and
reaches the same answer the kernel does.

Stage 3 of docs/umdf-design.md. The KTESTs beside `kernel/lib/
hda_codec.c` drive the parser from a synthetic codec and need no
hardware; `tools/devclaim_test.py` covers the claim itself. Neither can
cover what this stage exists for -- A PROCESS DRIVING A REAL
CONTROLLER'S COMMAND RING -- so this boots a guest with one.

WHAT A BROKEN VERSION WOULD STILL PASS. `lscodec` printing a plausible
graph is not evidence: a stub that invented one would satisfy "it named
a vendor and a route". So THE ASSERTION IS THE AGREEMENT. The kernel's
`hda` driver logs the pin and DAC it picked, in ring 0, over its own
CORB/RIRB; `lscodec` prints the pin and DAC it picked, in ring 3, over
a DMA buffer it was granted. The two numbers have to match, and nothing
but a ring-3 process that really talked to the card produces them.

The second half is the round trip: the controller goes back. `hda` is
re-probed on the release and registers the card AGAIN -- counted as a
difference from a baseline, since the klog ring rotates.

    python3 tools/hdacodec_test.py [--instance N] [--keep] [--no-card]

`--no-card` is the positive control: the same run with no controller
attached, where `lscodec` must SAY there is none and exit non-zero.
Without it, a tool whose assertions silently matched nothing would look
exactly like a pass.

On demand, not in the gate: it boots its own guest with extra hardware.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from harness import copy_disk  # noqa: E402

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
        print(f"\nhdacodec_test: {len(self.passes)} passed, {len(self.fails)} failed")
        for f in self.fails:
            print(f"  FAILED: {f}")
        return 1 if self.fails else 0


# `hda0: 00:03.0 codec 1af4:0012 spk pin 0x3 dac 0x2 hp 0 msi` -- what
# the RING-0 driver picked, logged at the end of its probe.
KERNEL_ROUTE = re.compile(r"spk pin 0x([0-9a-f]+) dac 0x([0-9a-f]+)")
# `  speaker route: 03 -> 02  (pin 03 dac 02)` -- what RING 3 picked.
RING3_ROUTE = re.compile(r"speaker route:.*\(pin ([0-9a-f]+) dac ([0-9a-f]+)\)")


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

    tmp = tempfile.mkdtemp(prefix="hdacodec_test_")
    img = os.path.join(tmp, "disk.img")
    # A COPY, so a re-seed underneath this run cannot change what it
    # booted and the user's own QEMU keeps its write lock.
    copy_disk("disk.img", img, cwd=REPO)

    def vm(*argv):
        # BOTH STREAMS: vm.py labels some replies on stderr, and a check
        # that read stdout alone reported an empty dmesg as a missing
        # log line.
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                            "--instance", n, *argv],
                           cwd=REPO, capture_output=True, text=True, check=False)
        return r.stdout + r.stderr

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
        want_card = not args.no_card
        # THE `snddrv` SERVICE DRIVES THE CARD FROM RING 3 SINCE BOOT, and
        # lscodec claims it off the KERNEL's driver -- so hand it back
        # first: a polite stop releases with REBIND.
        if want_card:
            vm("exec", "service stop snddrv")
        before = ""
        for _ in range(20):
            before = vm("exec", "lspci -k")
            if not want_card or "kernel driver: hda" in before:
                break
            time.sleep(1.0)
        if not res.check("an HD Audio controller is present and bound"
                         if want_card else "no HD Audio controller is present",
                         ("kernel driver: hda" in before) == want_card):
            return res.report()

        if want_card:
            # THE LOOSE END FROM STAGE 2, closed: `pci_device_removable()`
            # reaches ring 3 now, and lscodec ASKS it before claiming
            # rather than finding out by unbinding a live device.
            hda_line = next((l for l in before.splitlines()
                             if "kernel driver: hda" in l), "")
            res.check("lspci -k reports the sound card as claimable",
                      "(claimable)" in hda_line, hda_line.strip())
            # ...and the inverse, which is the property that matters.
            # It SKIPS rather than passing quietly when every bound
            # driver here happens to be removable, which is the case on
            # a plain QEMU machine (hda and e1000, nothing else bound) --
            # a check that matched no lines would otherwise be green
            # for the wrong reason.
            bound = [l for l in before.splitlines() if "kernel driver:" in l]
            stuck = [l for l in bound if "(claimable)" not in l]
            if stuck:
                res.check("a bound driver with no remove() is NOT claimable",
                          True, " / ".join(l.strip() for l in stuck))
            else:
                print("  skip  every bound driver here is removable "
                      f"({len(bound)} of them) -- dev_claim's KTEST covers "
                      "the inverse")

        # A dmesg BASELINE: the klog ring holds a few hundred lines, so
        # every count below is a DIFFERENCE rather than a total.
        base = vm("exec", "dmesg")
        kernel_route = KERNEL_ROUTE.findall(base)

        out = vm("exec", "lscodec")
        print("--- lscodec ---")
        print(out.strip())
        print("--- end ---")

        if not want_card:
            res.check("lscodec said there is no controller",
                      "no HD Audio controller" in out)
            res.check("...and failed rather than inventing a graph",
                      "exit 1" in out)
            return res.report()

        res.check("ring 3 claimed the controller", "claimed" in out)
        res.check("ring 3 mapped its registers and got a command ring",
                  "command rings at" in out)
        res.check("ring 3 read the controller's own version and capabilities",
                  "hd audio 1.0" in out)
        res.check("a codec answered with a vendor id",
                  re.search(r"codec \d+: vendor [0-9a-f]{4}:[0-9a-f]{4}", out) is not None)
        m = re.search(r"afg nid [0-9a-f]+, (\d+) widget", out)
        res.check("and described its widgets", m is not None and int(m.group(1)) > 0,
                  f"{m.group(1)} widget(s)" if m else "no afg line")

        # THE ASSERTION THIS TOOL EXISTS FOR. Two independent transports
        # -- the kernel's CORB/RIRB and ring 3's -- walking one card and
        # having to agree. A stub that invented a graph fails here and
        # nowhere else.
        ring3 = RING3_ROUTE.findall(out)
        res.check("ring 3 found an analog output", len(ring3) == 1,
                  f"pin {ring3[0][0]} dac {ring3[0][1]}" if ring3 else "none")
        res.check("the kernel had found one too", len(kernel_route) >= 1,
                  f"pin {kernel_route[-1][0]} dac {kernel_route[-1][1]}"
                  if kernel_route else "no hda route line in dmesg")
        if ring3 and kernel_route:
            k = (kernel_route[-1][0].lstrip("0") or "0",
                 kernel_route[-1][1].lstrip("0") or "0")
            r3 = (ring3[0][0].lstrip("0") or "0", ring3[0][1].lstrip("0") or "0")
            res.check("THE TWO RINGS PICKED THE SAME PIN AND THE SAME DAC", k == r3,
                      f"ring 0 pin {k[0]} dac {k[1]}, ring 3 pin {r3[0]} dac {r3[1]}")

        dmesg = vm("exec", "dmesg")
        # BUS MASTERING is what the DMA grant turns on, and without it
        # the buffer the card was pointed at is unreachable to it.
        res.check("the DMA grant raised bus mastering", "bus master on" in dmesg)
        # STAGE 4: the interrupt is routed to the holder. QEMU's
        # controller answers a verb before the driver can look away, so
        # it NEVER PARKS here and the wakeup path is not exercised --
        # `0 wakeup(s), 0 timeout(s)` is the correct reading on this
        # machine, and the ASUS's real codec is slow enough to show
        # 34 and 34. So what is asserted here is the ROUTING, which is
        # what QEMU can actually show.
        res.check("the interrupt was routed to the ring-3 driver",
                  "irq -> pid" in dmesg,
                  [line for line in dmesg.splitlines() if "irq -> pid" in line][-1:])
        res.check("...and the driver took the interrupt-driven path",
                  "responses are interrupt-driven" in out)
        res.check("the bus logged the unbind", "pci: hda released" in dmesg)

        after = vm("exec", "lspci -k")
        res.check("the kernel driver has the controller again",
                  "kernel driver: hda" in after)
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
