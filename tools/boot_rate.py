#!/usr/bin/env python3
"""Reboot a bare-metal machine N times and count how often something appears.

An intermittent fault is a RATE, and a rate is a count over boots. This
is that loop, because `docs/bugs.md` asks for it by name -- "reboot that
machine 5-10 times counting `dmesg | grep -c polls`" -- and doing it by
hand is a dozen commands that are easy to get subtly wrong (reading
dmesg before the boot finished, counting a line the previous boot left,
losing the run because one reboot took longer than a fixed sleep).

    python3 tools/boot_rate.py --host 192.168.200.107 -n 10 \
        --grep "enumeration failed"

It prints a line per boot and a rate at the end. The per-boot lines are
the point as much as the rate: an intermittent that clusters is a
different animal from one that is evenly spread, and a total hides that.

`--transfer PATH` sends a file over TFTP immediately before each reboot,
which is the shape of the population `docs/bugs.md`'s enumeration failure
actually lives in: every recorded occurrence followed a `remote.py flash`,
i.e. a reboot taken while the USB network adapter was carrying a transfer.
A plain reboot loop measures the 0-in-43 population and will sit there
looking clean.

WHAT IT DOES NOT DO. It does not flash: measure one kernel, and change
the kernel between runs deliberately. It does not interpret: `--grep` is
a substring and the count is the count. And it does NOT reboot a machine
that failed to come back -- the run stops there and says so, because
every boot after a machine you have lost is a lie in the denominator.

WHY IT WAITS ON THE MACHINE, NOT THE CLOCK. A fixed sleep after `reboot`
is the classic way to measure the wrong boot: too short and dmesg is the
PREVIOUS boot's (the machine has not restarted yet), too long and it
wastes minutes per iteration. This waits for the host to stop answering
and then to answer again, so it can only read the boot it caused.
"""
import argparse
import subprocess
import sys
import time

HERE = __file__.rsplit("/", 1)[0]


def ping(host, timeout=2):
    return subprocess.run(["ping", "-c1", f"-W{timeout}", host],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def remote(host, command, timeout=30):
    """`command` in the machine's shell; its output, or None if unreachable."""
    r = subprocess.run(
        [sys.executable, f"{HERE}/remote.py", "--host", host,
         "--timeout", str(timeout), "exec", command],
        capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def wait_down(host, seconds):
    """Until it stops answering -- that is what proves the reboot took."""
    end = time.time() + seconds
    while time.time() < end:
        if not ping(host):
            return True
        time.sleep(1)
    return False


def wait_up(host, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if ping(host):
            # Answering ICMP is not the same as having a shell: the
            # network comes up before init has finished starting things.
            if remote(host, "true", timeout=15) is not None:
                return True
        time.sleep(2)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("-n", type=int, default=10, help="boots (default 10)")
    ap.add_argument("--grep", required=True,
                    help="substring counted in dmesg after each boot")
    ap.add_argument("--down-timeout", type=int, default=60)
    ap.add_argument("--up-timeout", type=int, default=180)
    ap.add_argument("--transfer", metavar="PATH",
                    help="TFTP this local file to the machine just before "
                         "each reboot, so the reboot happens while the USB "
                         "NIC is carrying traffic -- the population every "
                         "recorded occurrence of the enumeration failure "
                         "came from (docs/bugs.md)")
    ap.add_argument("--show", action="store_true",
                    help="print the matching lines, not just the count -- a "
                         "count alone cannot say WHICH thing matched")
    a = ap.parse_args()

    if not ping(a.host):
        print(f"boot_rate: {a.host} is not answering to start with",
              file=sys.stderr)
        return 1

    hits = boots = 0
    for i in range(1, a.n + 1):
        # The FIRST iteration measures the boot the machine is already
        # on, so a run of N gives N samples rather than N-1 and the
        # machine is left rebooted-and-up either way.
        if i > 1:
            if a.transfer:
                # Ignored if it fails: a refused transfer is worth a line
                # and not worth losing the run over, and the reboot below
                # is still a valid sample of the plain population.
                rc = subprocess.run([sys.executable, f"{HERE}/remote.py",
                                     "--host", a.host, "put", a.transfer,
                                     "/var/tmp/boot_rate_probe.bin"],
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL).returncode
                if rc != 0:
                    print(f"  boot {i:2d}: transfer FAILED -- "
                          f"this sample is a plain reboot")
            remote(a.host, "reboot", timeout=10)
            if not wait_down(a.host, a.down_timeout):
                print(f"boot_rate: {a.host} never went down -- did `reboot` "
                      f"run? stopping after {boots} boot(s)", file=sys.stderr)
                break
            if not wait_up(a.host, a.up_timeout):
                print(f"boot_rate: {a.host} did not come back within "
                      f"{a.up_timeout}s -- STOPPING. It may need a power "
                      f"cycle; {boots} boot(s) measured.", file=sys.stderr)
                break

        out = remote(a.host, "dmesg", timeout=60)
        if out is None:
            print(f"boot_rate: could not read dmesg on boot {i} -- stopping",
                  file=sys.stderr)
            break
        lines = [ln.strip() for ln in out.splitlines() if a.grep in ln]
        n = len(lines)
        boots += 1
        if n:
            hits += 1
        print(f"  boot {i:2d}: {n:3d} x {a.grep!r}{'' if n else '   clean'}")
        # A COUNT IS NOT EVIDENCE, and this run proved it: the first
        # substring tried here matched both "failed -- retrying" (which
        # usually recovers) and the terminal failure, so the rate could
        # not tell a recovery from a loss. Printing the lines is what
        # makes a surprising count interpretable without another run.
        if a.show:
            for ln in lines:
                print(f"           {ln}")

    if not boots:
        return 1
    print(f"\nboot_rate: {hits}/{boots} boot(s) had {a.grep!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
