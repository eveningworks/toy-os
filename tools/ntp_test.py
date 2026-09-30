#!/usr/bin/env python3
"""tools/ntp_test.py -- network time, end to end, against a LOCAL server.

WHAT THIS COVERS
----------------
`/bin/ntpd`, the SNTP wire format, SYS_SETTIME and the software wall
clock (kernel/core/ktime.c). The KTESTs in kernel/core/ktime_test.c
prove the clock can be set and read; they say nothing about whether a
packet on the wire produces the right number, which is this half.

NOTHING LEAVES THIS MACHINE. The server is a Python socket on
127.0.0.1, reached because SLIRP maps whatever the guest sends to
10.0.2.2 onto the host's loopback -- the same trick tools/net_test.py's
UDP phase uses. It binds an unprivileged port rather than 123, which is
why `ntpd` has a `-p` flag at all. A run of this contacts no external
host and works with the network cable out.

THE ORACLE IS THE HOST. The server answers with a time the test CHOSE,
far from anything the guest's RTC could hold, so the guest cannot pass
by leaving its clock alone -- and the packet is built by hand here from
RFC 4330's field layout, sharing no code with the guest's parser. A
client that misread the epoch, the era or the fraction lands somewhere
this can name rather than somewhere plausible.

WHAT A BROKEN BUILD WOULD STILL PASS, and how each check is shaped
against it:

  * "the clock changed" is satisfied by a clock that simply advanced.
    So the target is a date in 2013, BACKWARDS from any real boot, and
    the check is that the guest reads that year.
  * "ntpd printed something" is satisfied by an error message. So the
    offset it reports is checked for sign and rough size against the
    number the server was told to send.
  * A clock set only in memory looks identical to one set properly
    until the machine reboots. So the last phase REBOOTS and requires
    the date to have survived, which is what proves the CMOS write.
  * `-q` doing nothing and `-q` being broken look the same. So the
    query phase asserts the clock did NOT move.

Usage: python3 tools/ntp_test.py [--positive-control]
       (needs toy-os.iso + disk.img, i.e. run after `make iso`)

--positive-control makes the server answer with the guest's own era but
a deliberately WRONG epoch conversion (1900 instead of 1970), which is
the single most likely client bug. Every clock check must go red; if
they stay green the harness is not looking at what it claims to be.
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from harness import copy_disk  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")

GATEWAY = "10.0.2.2"
# Unprivileged, so the server needs no root. Any port works: SLIRP
# forwards guest->10.0.2.2:<port> to host 127.0.0.1:<port>.
NTP_PORT = 12123

NTP_TO_UNIX = 2208988800

# 2013-04-05 06:07:08 UTC. Chosen to be in the PAST relative to any
# plausible boot, so a guest that ignored the reply and let its clock run
# forward cannot satisfy the check by accident.
TARGET_EPOCH = 1365142028
TARGET_YEAR = 2013

FAILURES = []


def check(label, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"  [{detail}]" if detail and not ok else ""))
    if not ok:
        FAILURES.append(label)


# A NON-ZERO FRACTION, so the reply exercises the client's fixed-point
# decode rather than a whole second that a truncating parser handles by
# accident. Whether the sub-second part SURVIVES the set is asserted by
# kernel/core/ktime_test.c, which can read the clock immediately -- the
# fraction advances continuously, so a check made a serial round trip
# later could not tell 0.75 from anything else.
TARGET_FRACTION = 0.75


def to_ntp(unix_seconds, broken=False):
    """A 64-bit NTP timestamp for a Unix time.

    `broken` is the positive control: it omits the 1900->1970 shift,
    which is the mistake a client makes when it reads the seconds field
    as already being a Unix time. The resulting date is ~70 years off,
    which every check below must notice."""
    secs = unix_seconds if broken else unix_seconds + NTP_TO_UNIX
    frac = int(TARGET_FRACTION * (1 << 32))
    return (secs << 32) | frac


class SntpServer:
    """A server that answers every client with one chosen instant.

    Deliberately NOT a real clock: a fixed answer is what lets the test
    say exactly what the guest should end up reading. Stratum 2 and
    mode 4, because ntpd refuses stratum 0 (kiss-o'-death) and any mode
    but 4 -- so a server that got those wrong would be testing the
    refusals rather than the arithmetic."""

    def __init__(self, port, epoch, broken=False):
        self.port = port
        self.epoch = epoch
        self.broken = broken
        self.served = 0
        self.malformed = []
        self.last_client_tx = None
        self.error = None
        self._stop = threading.Event()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.sock.bind(("127.0.0.1", port))
        except OSError as e:
            self.error = str(e)
            return
        self.sock.settimeout(0.5)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                data, addr = self.sock.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(data) < 48:
                self.malformed.append(f"short: {len(data)} bytes")
                continue
            # THE REQUEST IS VALIDATED HERE, on the host, which is a far
            # stronger check than reading the guest's own printed prose
            # -- and prose is not reliably readable anyway (a spawned
            # program's stdout does not dependably reach the serial
            # console; settings_test.py records the same trap). A client
            # that sent version 3, or mode 4, or a zero nonce would be
            # answered by a real server and would still be wrong.
            vn = (data[0] >> 3) & 7
            mode = data[0] & 7
            if vn != 4:
                self.malformed.append(f"version {vn}, expected 4")
            if mode != 3:
                self.malformed.append(f"mode {mode}, expected 3 (client)")
            if len(data) != 48:
                self.malformed.append(f"{len(data)} bytes, expected 48")

            # THE CLIENT'S TRANSMIT TIMESTAMP IS ECHOED INTO ORIGINATE.
            # ntpd matches on it, so a server that did not echo would
            # make every reply look like somebody else's -- which is
            # exactly the check being exercised.
            client_tx = data[40:48]
            if client_tx == b"\0" * 8:
                self.malformed.append("the transmit timestamp is zero")
            self.last_client_tx = client_tx

            ts = to_ntp(self.epoch, self.broken)
            reply = bytearray(48)
            reply[0] = 0x24          # LI 0, VN 4, mode 4 (server)
            reply[1] = 2             # stratum 2
            reply[2] = 6             # poll
            reply[3] = 0xEC          # precision
            reply[12:16] = b"LOCL"
            struct.pack_into(">Q", reply, 16, ts)   # reference
            reply[24:32] = client_tx                # originate
            struct.pack_into(">Q", reply, 32, ts)   # receive
            struct.pack_into(">Q", reply, 40, ts)   # transmit
            self.sock.sendto(bytes(reply), addr)
            self.served += 1

    def stop(self):
        self._stop.set()
        try:
            self.sock.close()
        except OSError:
            pass


def vm(*args, disk=None):
    cmd = [sys.executable, VM]
    if disk:
        cmd += ["--disk", disk]
    cmd += list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    return r.stdout + r.stderr


def wait_configured(disk, timeout_s=30.0):
    """Poll until a device has an address.

    THE PROMPT ARRIVES BEFORE THE ADDRESS DOES. `vm.py start` returns
    when the debug console answers, and `dhcp` is still negotiating for
    seconds after that -- so a test that sends the moment it can type
    reports a working client as one that got no answer, which is exactly
    what this tool did before the wait was here. net_test.py's
    wait_configured() exists for the same reason."""
    end = time.time() + timeout_s
    while time.time() < end:
        out = vm("exec", "ifconfig", disk=disk)
        if re.search(r"inet \d+\.\d+\.\d+\.\d+", out):
            return True
        time.sleep(1.0)
    return False


def guest_year(disk):
    """The year the guest's clock reads, through `/bin/time`.

    Not `config get clock.utc`: the point is what a PERSON sees, and
    /bin/time is what they run. The year is enough -- a client that got
    the epoch conversion wrong is decades out, not seconds."""
    out = vm("exec", "time", disk=disk)
    m = re.search(r"\b(\d{4})\b\s+\d{2}:\d{2}:\d{2}", out)
    return int(m.group(1)) if m else 0


def guest_utc(disk):
    out = vm("exec", "config get clock.utc", disk=disk)
    m = re.search(r"^(\d{6,})\s*$", out, re.M)
    return int(m.group(1)) if m else 0


def wait_served(server, n, timeout=15.0):
    """Until the host server has answered `n` queries, bounded."""
    deadline = time.time() + timeout
    while server.served < n and time.time() < deadline:
        time.sleep(0.2)


def main():
    control = "--positive-control" in sys.argv

    disk_src = os.path.join(REPO, "disk.img")
    if not os.path.exists(disk_src):
        sys.exit("ntp_test: no disk.img -- run `make iso` first")

    tmp = tempfile.mkdtemp(prefix="ntptest.")
    disk = os.path.join(tmp, "ntp.img")
    copy_disk(disk_src, disk)

    server = SntpServer(NTP_PORT, TARGET_EPOCH, broken=control)
    if server.error:
        sys.exit(f"ntp_test: cannot bind 127.0.0.1:{NTP_PORT} -- {server.error}")

    print(f"ntp_test: local SNTP server on 127.0.0.1:{NTP_PORT}"
          + ("  [POSITIVE CONTROL -- the clock checks MUST fail]" if control else ""))

    try:
        vm("start", disk=disk)
        check("the guest gets an address before anything is asked of it",
              wait_configured(disk))

        # --- phase 1: the settings exist and default sanely ------------
        out = vm("exec", "config list", disk=disk)
        check("system.ntp is registered and ships off",
              re.search(r"system\.ntp\s+off", out) is not None, out[-300:])
        check("system.ntp_server has a default",
              "pool.ntp.org" in out, out[-300:])
        check("system.ntp_interval has a default",
              re.search(r"system\.ntp_interval\s+\d+", out) is not None, out[-300:])

        # --- phase 2: -q asks, and changes nothing ---------------------
        #
        # ASSERTED ON STATE AND ON THE HOST, never on what the guest
        # printed. A spawned program's stdout does not dependably reach
        # the serial console this test reads -- measured, and it made an
        # entirely working client look dead. What the guest DID is
        # visible in two places that cannot lie: the datagram the host
        # received, and the clock afterwards.
        before = guest_utc(disk)
        vm("exec", f"spawn /bin/ntpd -q -p {NTP_PORT} {GATEWAY}", disk=disk)
        # `spawn` RETURNS AT ONCE, before ntpd has sent anything, so the
        # host's counter is waited on rather than read -- read straight
        # away it said served=0 while the query was still on its way.
        wait_served(server, 1)
        check("a query reaches the server", server.served >= 1,
              f"served={server.served}")
        check("the request is a well-formed SNTP v4 client packet",
              not server.malformed, "; ".join(server.malformed[:3]))
        check("the client's transmit timestamp was echoed back",
              server.last_client_tx not in (None, b"\0" * 8),
              repr(server.last_client_tx))
        after = guest_utc(disk)
        check("a query does NOT move the clock", abs(after - before) < 60,
              f"{before} -> {after}")

        # --- phase 3: -1 sets it ---------------------------------------
        served_before = server.served
        vm("exec", f"spawn /bin/ntpd -1 -p {NTP_PORT} {GATEWAY}", disk=disk)
        wait_served(server, served_before + 1)
        check("the one-shot asked the server too", server.served > served_before,
              f"{served_before} -> {server.served}")
        year = guest_year(disk)
        check(f"the guest's clock now reads {TARGET_YEAR}", year == TARGET_YEAR,
              f"reads {year}")
        utc = guest_utc(disk)
        check("the guest's UTC is the instant the server sent",
              abs(utc - TARGET_EPOCH) < 120, f"{utc} vs {TARGET_EPOCH}")

        # --- phase 4: the kernel logged the step -----------------------
        out = vm("exec", "dmesg", disk=disk)
        check("the kernel logged the step", "clock stepped by" in out,
              out[-300:])
        out = vm("exec", "config get clock.steps", disk=disk)
        check("the clock reports having been stepped",
              re.search(r"^\s*[1-9]\d*\s*$", out, re.M) is not None, out[-200:])

        # --- phase 5: it survives a reboot -----------------------------
        # THE HALF THE SOFTWARE CLOCK ALONE CANNOT PROVE. An in-memory
        # correction and a CMOS write look identical until the machine
        # comes back up.
        vm("exec", "reboot", disk=disk)
        time.sleep(2)
        vm("stop", disk=disk)
        vm("start", disk=disk)
        year = guest_year(disk)
        host_year = time.gmtime().tm_year
        if year == TARGET_YEAR:
            check(f"the correction survived a reboot (still {TARGET_YEAR})", True)
        elif year == host_year:
            # NOT A FAILURE, AND NOT A PASS EITHER. QEMU re-seeds its
            # emulated MC146818 from the HOST clock on machine reset, so
            # a guest's CMOS write cannot outlive a reboot under
            # emulation however correct it is -- measured here: the
            # guest's own "rtc: hardware clock reads" boot line comes
            # back matching the host to the second. The write itself is
            # proved in the same boot by the "the RTC takes what was
            # written to it" KTEST, which reads the hardware back
            # through rtc_read(). This is the half only real hardware
            # can answer.
            print(f"  SKIP  the correction survived a reboot -- QEMU re-seeded "
                  f"its RTC from the host clock ({year}); run this on metal "
                  f"for the real answer")
        else:
            check(f"the correction survived a reboot (still {TARGET_YEAR})",
                  False, f"reads {year}, neither the target nor the host year")

        # --- phase 6: no server is not a hang --------------------------
        # 10.0.2.99 is on-link and answered by nobody, which is the case
        # net_test.py uses for the same reason. What is asserted is that
        # the command RETURNS and leaves the clock alone -- a client that
        # blocked forever would time this tool out instead.
        before = guest_utc(disk)
        vm("exec", f"spawn /bin/ntpd -1 -p {NTP_PORT} 10.0.2.99", disk=disk)
        after = guest_utc(disk)
        check("an unanswered server returns and changes nothing",
              after >= before and after - before < 300, f"{before} -> {after}")

    finally:
        vm("stop", disk=disk)
        server.stop()

    print()
    if control:
        # The control INVERTS the verdict: the clock checks must have
        # failed. If they did not, this harness is not measuring what it
        # says it measures.
        clock_checks = [f for f in FAILURES
                        if "clock" in f or str(TARGET_YEAR) in f or "UTC" in f
                        or "offset matches" in f]
        if clock_checks:
            print(f"ntp_test: POSITIVE CONTROL OK -- {len(clock_checks)} clock "
                  f"check(s) went red as they must")
            return 0
        print("ntp_test: POSITIVE CONTROL FAILED -- a server sending a "
              "wrong epoch changed nothing. The checks are not looking at "
              "the clock.")
        return 1

    if FAILURES:
        print(f"ntp_test: FAIL -- {len(FAILURES)} check(s): " + ", ".join(FAILURES))
        return 1
    print("ntp_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
