#!/usr/bin/env python3
"""Prove the machine stops through ITS OWN ACPI tables, not a hardcoded port.

WHY THIS IS NOT "DID IT SHUT DOWN"
----------------------------------
It used to write `outw(0x604, 0x2000)` -- QEMU/Bochs's fixed shortcut --
and QEMU's own FADT happens to name port 0x604 and sleep type 0. So a
correct implementation and the old hardcode produce the SAME observable
here: the guest stops. "The machine powered off" is exactly the
assertion a broken version passes.

What distinguishes them is the KERNEL LOG. `acpi: S5 via PM1a 0x604
type 0` is printed by the path that read the FADT and the DSDT's `_S5_`;
`power: no ACPI poweroff path -- trying the QEMU/Bochs port` is printed
only when that path declined. Asserting on the first and the ABSENCE of
the second is the check that fails when the ACPI path breaks -- verified
by disabling `acpi_poweroff()` and watching exactly those two lines swap
over, with the guest still stopping.

The third line, `status cleared at 0x...`, says the PM1 EVENT block was
parsed and the wake-status bits written before the sleep. What this
CANNOT check is the bug that added it: a guest has no pending wake
event, so skipping the clear breaks nothing here. See docs/bugs.md.

TWO CHIPSETS, because one is not a sample. i440fx (QEMU's default)
presents a revision-0 RSDP, an RSDT, a 116-byte FADT and NO reset
register; q35 presents an XSDT-capable RSDP, a 244-byte revision-3 FADT
and a real reset register at port 0xcf9. The reset half of this can only
be tested on the second, and a table walk that works on one and not the
other is the failure mode a single machine type cannot see.

WHAT IT CANNOT COVER. VirtualBox and real hardware, which is the reason
the feature exists. There is no VirtualBox on the machines this runs on;
what is testable here is that the values come from the tables rather
than from a constant, which is the property those platforms need.

ON DEMAND, never a gate: it boots four guests and deliberately ends
three of them by stopping the machine.

    python3 tools/poweroff_test.py
    python3 tools/poweroff_test.py --only q35 -v
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_PORT = 4561  # not ktest_run.py's 4555 nor virtio_boot_test.py's 4557


def launch(iso, disk, machine, qemu_log, reboot_ok=False):
    cmd = ["qemu-system-x86_64"]
    if machine:
        cmd += ["-machine", machine]
    # order=c: the disk carries GRUB and the kernel (tools/install_grub.py).
    cmd += ["-boot", "order=c"]
    if machine and machine.startswith("q35"):
        # q35 has no legacy IDE at all -- the same image goes behind the
        # ICH9 HBA instead, or the guest boots nothing and the failure
        # reads as an ACPI bug.
        cmd += ["-device", "ich9-ahci,id=ahci",
                "-drive", f"file={disk},format=raw,if=none,id=sata0",
                "-device", "ide-hd,drive=sata0,bus=ahci.0"]
    else:
        cmd += ["-drive", f"file={disk},format=raw,if=ide"]
    cmd += ["-m", "512", "-display", "none",
            # `server` without `nowait` so QEMU waits for us and nothing
            # printed before the connection is lost -- see ktest_run.py.
            "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server"]
    # NEITHER -no-shutdown NOR -no-reboot on the poweroff runs: QEMU
    # EXITING is half the evidence, and -no-shutdown would keep the
    # process alive through a perfectly good S5 and report it as a
    # failure. The reset run wants the machine to actually come back, so
    # it does not get -no-reboot either.
    if not reboot_ok:
        cmd += ["-no-reboot"]
    log = open(qemu_log, "wb")
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)


def connect(timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1.0)
        except OSError:
            time.sleep(0.2)
    return None


def read_until(sock, needle, timeout, transcript):
    deadline = time.time() + timeout
    sock.settimeout(0.5)
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        transcript.append(chunk.decode("utf-8", "replace"))
        if needle and needle in "".join(transcript):
            return True
    return False


def boot_and_run(iso, disk, machine, qemu_log, command, wait_for, timeout,
                 reboot_ok=False):
    """Boot, wait for the debug console, send one command, read on.

    Returns (transcript, exited, error). `exited` is whether the QEMU
    PROCESS ended within the wait -- which for a poweroff is the point,
    and is a fact about the host that the guest cannot fake.
    """
    qemu = launch(iso, disk, machine, qemu_log, reboot_ok)
    transcript = []
    try:
        sock = connect(timeout)
        if sock is None:
            return None, False, "could not connect to the guest's serial console"
        if not read_until(sock, "debug console ready", timeout, transcript):
            return None, False, "the debug console never came up"

        # `sh ` because this is the KERNEL DEBUG CONSOLE, not a shell:
        # its own verbs are things like `kstack` and `ktest`, and `sh`
        # is the one that hands a line to the shell's dispatch. A bare
        # command here is answered by nothing at all, so the wait below
        # burns its whole timeout and the tool reports a healthy machine
        # as a failure -- which is exactly how this was found.
        sock.sendall(("sh " + command + "\n").encode())
        # Count from HERE. "debug console ready" is already in the
        # transcript from the first boot, so a reset run waiting on it
        # would be satisfied instantly by the line it is trying to see a
        # SECOND time.
        before = "".join(transcript).count(wait_for) if wait_for else 0
        deadline = time.time() + timeout
        while time.time() < deadline:
            read_until(sock, None, 1.0, transcript)  # None: read for the slice
            if "".join(transcript).count(wait_for) > before:
                break
            if qemu.poll() is not None:
                break

        # The guest was told to stop; give the process time to actually
        # go. Polled rather than slept so a fast exit is not paid for.
        deadline = time.time() + 15.0
        while time.time() < deadline:
            if qemu.poll() is not None:
                break
            time.sleep(0.25)
        return "".join(transcript), qemu.poll() is not None, None
    finally:
        if qemu.poll() is None:
            qemu.kill()
        try:
            qemu.wait(timeout=5)
        except Exception:
            pass


ACPI_S5 = "acpi: S5 via"
FALLBACK = "no ACPI poweroff path -- trying the QEMU/Bochs port"
STS_CLEARED = "status cleared at 0x"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="toy-os.iso")
    ap.add_argument("--disk", default="disk.img",
                    help="seed image to COPY (never written to directly)")
    ap.add_argument("--work", default="poweroff_test.img")
    ap.add_argument("--only", choices=("i440fx", "q35"), default=None)
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--qemu-log", default="poweroff_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.disk):
        print(f"poweroff_test: {args.disk} not found -- build it first (make iso)")
        return 1

    # A COPY, sparse: disk.img is a few MB of data in a 9 GB sparse file,
    # and the real one may be open in the user's own QEMU.
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, args.work],
                   check=True)

    checks = []

    def check(name, ok, detail=""):
        checks.append((name, ok))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))

    machines = [("i440fx", None), ("q35", "q35")]
    if args.only:
        machines = [m for m in machines if m[0] == args.only]

    for label, machine in machines:
        print(f"poweroff_test: {label} -- the tables this machine describes")
        t, _, err = boot_and_run(args.iso, args.work, machine, args.qemu_log,
                                 "acpi", "Processors (MADT)", args.timeout)
        if t is None:
            print(f"poweroff_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t)

        check(f"{label}: the RSDP was found", "acpi: RSDP rev" in t)
        # The FADT line names a port. That it names ANY is the assertion
        # -- naming which one would bake this machine's numbers into a
        # test whose whole point is that they are not baked in anywhere.
        check(f"{label}: a FADT named a PM1a port", "acpi: FADT pm1a=0x" in t
              and "pm1a=0x0 " not in t)
        check(f"{label}: _S5_ decoded out of the DSDT", "acpi: _S5_ sleep types" in t)
        check(f"{label}: the MADT listed a processor", "acpi: MADT lists" in t
              and "lists 0 processor" not in t)
        check(f"{label}: /bin/acpi reports poweroff as available",
              "poweroff:      yes" in t)

        print(f"poweroff_test: {label} -- powering off")
        t, exited, err = boot_and_run(args.iso, args.work, machine, args.qemu_log,
                                      "reboot --poweroff", ACPI_S5, args.timeout)
        if t is None:
            print(f"poweroff_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t)

        # THE DISCRIMINATING PAIR. The guest stops either way; only these
        # two lines say which code did it.
        check(f"{label}: the S5 write came from the parsed tables", ACPI_S5 in t)
        check(f"{label}: the legacy port trick did NOT run", FALLBACK not in t)
        # THE WAKE-STATUS CLEAR. What QEMU can check is that the PM1
        # EVENT block was parsed and written -- NOT that skipping it
        # breaks anything, because a guest has no pending wake event to
        # come back up on. The bug it exists for is bare-metal only:
        # entering S5 with a wake pending reads as "it restarts instead
        # of shutting down". See docs/bugs.md.
        sts = next((ln for ln in t.splitlines() if STS_CLEARED in ln), "")
        addr = sts.split(STS_CLEARED)[-1].strip() if sts else ""
        check(f"{label}: the wake-status bits were cleared first",
              bool(sts) and addr not in ("0", ""), sts.strip() or "no line")
        check(f"{label}: QEMU exited -- the machine really stopped", exited)

    # RESET, which only q35 can exercise here: i440fx's FADT has no
    # reset register at all, so `reboot` there correctly stays on the
    # 8042 pulse and there is nothing new to check.
    if any(m[0] == "q35" for m in machines):
        print("poweroff_test: q35 -- resetting through the FADT's reset register")
        t, _, err = boot_and_run(args.iso, args.work, "q35", args.qemu_log,
                                 "reboot", "debug console ready", args.timeout,
                                 reboot_ok=True)
        if t is None:
            print(f"poweroff_test: FAIL -- {err}")
            return 1
        if args.verbose:
            print(t)
        check("q35: the reset went through the ACPI reset register",
              "acpi: reset via" in t)
        # And it really reset rather than dying: the console comes up a
        # second time on the same QEMU process.
        check("q35: the machine came back up",
              t.count("debug console ready") >= 2,
              f"saw {t.count('debug console ready')} boot(s)")

    failed = [n for n, ok in checks if not ok]
    print(f"\npoweroff_test: {len(checks) - len(failed)}/{len(checks)} checks passed")
    if failed:
        for n in failed:
            print(f"  FAILED: {n}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
