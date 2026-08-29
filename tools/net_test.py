#!/usr/bin/env python3
"""tools/net_test.py -- the network stack, on both NICs, judged on the HOST.

WHAT THIS COVERS
----------------
Everything from the NIC driver up to `/bin/ping`: kernel/drivers/net/,
kernel/drivers/virtio/virtio_net.c and kernel/net/. The KTESTs in
kernel/net/net_test.c drive the protocols by hand through eth_input(),
which needs no network and therefore says nothing about whether a frame
ever reaches a wire. This is the half that does.

THE ORACLE IS THE HOST, TWICE OVER, and that is the point.

  1. QEMU's user-mode network (SLIRP) answers the pings. It shares no
     code with this OS, so a wrong ARP, a wrong checksum or a wrong
     destination address is simply never replied to -- the guest cannot
     satisfy this check by agreeing with itself.

  2. Every frame is DUMPED TO A PCAP and decoded here. SLIRP can be
     lenient; a decoder that recomputes the IPv4 and ICMP checksums from
     the bytes on the wire cannot. This is the same call
     regex_hostcheck.py and fat32_test.py make -- a self-test cannot
     catch an EXPECTATION being wrong, because the same person wrote
     both halves.

THE PHASES, and what a broken build would still pass
----------------------------------------------------
  1. e1000 -- the card QEMU's default machine has always had. `ping`
     answered, and the pcap holds a well-formed ARP request and echo
     request.
  2. virtio-net -- the ONLY thing here that reaches virtio_net.c. Same
     assertions; a stack that only worked on one driver passes phase 1
     and fails this.
  3. Two cards at once, which is the shape the device table exists for.
     Only the first is addressed at boot; the second is configured by
     hand onto a DIFFERENT subnet, and the assertion is that traffic
     then leaves through it -- per-device counters, not "a ping worked",
     which a single-homed stack would also satisfy.
  4. No card at all (`-nic none`). `ping` must report and exit, not
     hang: a machine with no network is an ordinary state.
  5. The ARP retransmit rate. Pinging an address nobody answers put 104
     frames on the wire before kernel/net/arp.c rate-limited requests
     (measured, which is how it was found); this fails if that
     regresses.

    python3 tools/net_test.py
    echo $?
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import install_grub  # noqa: E402

PROMPT = "dbg> "
BOOT_TIMEOUT_S = 60.0
GATEWAY = "10.0.2.2"
GUEST_IP = "10.0.2.15"
# On-link and unanswered: SLIRP replies for the gateway and for nothing
# else on the subnet, which is what makes this the "no ARP reply" case
# rather than a routing one.
UNANSWERED = "10.0.2.99"


class Shell:
    """One command at a time over the serial debug console. Its own few
    lines rather than vm.py's, for multidisk_test.py's reason: this test
    launches QEMU differently (a pcap filter, a chosen NIC) and owns the
    guest it drives."""

    def __init__(self, sock_path, timeout=25.0):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(sock_path)
        self.timeout = timeout
        self._drain()

    def wait_ready(self, deadline_s=BOOT_TIMEOUT_S):
        """Wait for the debug console's prompt before typing anything.

        THE SOCKET EXISTS BEFORE THE GUEST DOES. QEMU opens it at
        launch, so connecting proves nothing about the kernel -- and a
        command sent early loses its first characters to a console that
        is not reading yet, which surfaces as `unknown command: h` for a
        command that starts with `sh`. Waiting on the PROMPT is waiting
        on the observable, which is this repo's own rule about polls."""
        deadline = time.time() + deadline_s
        seen = b""
        last_nudge = 0.0
        while time.time() < deadline:
            # A newline now and then, in case the banner was printed
            # before this socket connected -- but never in a tight loop:
            # the guest is not reading yet, so the send buffer fills and
            # sendall() itself blocks.
            if time.time() - last_nudge > 2.0:
                last_nudge = time.time()
                try:
                    self.s.settimeout(1.0)
                    self.s.sendall(b"\n")
                except OSError:
                    pass
            try:
                self.s.settimeout(1.0)
                chunk = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            finally:
                self.s.settimeout(self.timeout)
            if not chunk:
                break
            seen += chunk
            if PROMPT.strip().encode() in seen:
                self._drain()
                return True
        return False

    def _drain(self):
        self.s.settimeout(0.4)
        try:
            while True:
                if not self.s.recv(65536):
                    break
        except Exception:
            pass
        self.s.settimeout(self.timeout)

    def run(self, cmd, timeout=None):
        if not cmd.startswith(("sh ", "spawn ")) and cmd:
            cmd = "sh " + cmd
        self.s.sendall((cmd + "\n").encode())
        out, deadline = b"", time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            out += chunk
            if out.rstrip().endswith(PROMPT.strip().encode()):
                break
        return out.decode("utf-8", "replace")

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
        if not ok and detail:
            print(f"        {detail}")


# --- the host-side decoder -------------------------------------------
#
# Deliberately hand-written rather than scapy/dpkt: a dependency the
# gate might not have is a check that silently stops running, and the
# formats involved are twenty lines each. It shares nothing with
# kernel/net/, which is the whole reason it is evidence.

def read_pcap(path):
    """Every frame in a QEMU filter-dump, as raw bytes."""
    if not os.path.exists(path) or os.path.getsize(path) < 24:
        return []
    with open(path, "rb") as f:
        data = f.read()
    magic = struct.unpack("<I", data[:4])[0]
    if magic == 0xA1B2C3D4:
        endian, scale = "<", 1
    elif magic == 0xD4C3B2A1:
        endian, scale = ">", 1
    else:
        return []
    del scale
    frames, off = [], 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        if off + incl > len(data):
            break
        frames.append(data[off:off + incl])
        off += incl
    return frames


def checksum(b):
    """The one's-complement sum every header here carries. Independent
    of the guest's implementation on purpose -- this is the check that
    catches a checksum SLIRP was willing to tolerate."""
    if len(b) % 2:
        b += b"\x00"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(a):
    return struct.unpack(">I", socket.inet_aton(a))[0]


def decode(frames):
    """Split a capture into the three things the checks ask about."""
    arps, echoes, replies, bad = [], [], [], []
    for f in frames:
        if len(f) < 14:
            continue
        ethertype = struct.unpack(">H", f[12:14])[0]
        if ethertype == 0x0806 and len(f) >= 42:
            op = struct.unpack(">H", f[20:22])[0]
            arps.append({
                "op": op,
                "sender_mac": f[22:28],
                "sender_ip": struct.unpack(">I", f[28:32])[0],
                "target_ip": struct.unpack(">I", f[38:42])[0],
            })
        elif ethertype == 0x0800 and len(f) >= 34:
            ihl = (f[14] & 0x0F) * 4
            total = struct.unpack(">H", f[16:18])[0]
            hdr = f[14:14 + ihl]
            # A correct header sums to zero WITH its checksum field in
            # place, which is the property the guest relies on when it
            # verifies -- so checking it that way tests the same rule.
            if checksum(hdr) != 0:
                bad.append("ipv4 header checksum")
                continue
            if f[23] != 1:      # not ICMP
                continue
            icmp = f[14 + ihl:14 + total]
            if checksum(icmp) != 0:
                bad.append("icmp checksum")
                continue
            rec = {
                "src": struct.unpack(">I", f[26:30])[0],
                "dst": struct.unpack(">I", f[30:34])[0],
                "type": icmp[0],
                "id": struct.unpack(">H", icmp[4:6])[0],
                "seq": struct.unpack(">H", icmp[6:8])[0],
                "payload": icmp[8:],
            }
            (echoes if rec["type"] == 8 else replies).append(rec)
    return arps, echoes, replies, bad


# --- the guest --------------------------------------------------------

def nic_args(kind, pcap):
    dump = f" -object filter-dump,id=fd0,netdev=n0,file={pcap}" if pcap else ""
    if kind == "none":
        return " -nic none"
    if kind == "e1000":
        return f" -netdev user,id=n0 -device e1000,netdev=n0{dump}"
    if kind == "virtio":
        return f" -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on{dump}"
    if kind == "both":
        return (f" -netdev user,id=n0 -device e1000,netdev=n0{dump}"
                f" -netdev user,id=n1 -device virtio-net-pci,netdev=n1,disable-legacy=on")
    raise ValueError(kind)


def launch(disk, tmp, tag, kind, pcap=None):
    serial = os.path.abspath(os.path.join(tmp, f"net_{tag}.log"))
    sock = os.path.abspath(os.path.join(tmp, f"net_{tag}.serial"))
    pidfile = os.path.abspath(os.path.join(tmp, f"net_{tag}.pid"))
    for f in (serial, pidfile, sock):
        if os.path.exists(f):
            os.remove(f)
    boot = " ".join(install_grub.qemu_boot_args(
        install_grub.boot_medium(disk, None), os.path.join(ROOT, "toy-os.iso")))
    cmd = (f"qemu-system-x86_64 {boot}"
           f" -drive file={disk},format=raw,if=ide,discard=unmap"
           f"{nic_args(kind, pcap)}"
           f" -m 1024 -display none -no-reboot"
           f" -serial unix:{sock},server,nowait"
           f" -daemonize -pidfile {pidfile}")
    subprocess.run(cmd, shell=True, check=True, cwd=ROOT)

    deadline = time.time() + BOOT_TIMEOUT_S
    while time.time() < deadline:
        if os.path.exists(sock):
            try:
                sh = Shell(sock)
            except OSError:
                time.sleep(0.4)
                continue
            if not sh.wait_ready():
                # Kill it here rather than leaving it to the caller's
                # finally: this raises BEFORE the caller has a pidfile
                # to clean up, which leaked a guest that outlived the
                # run and its own temp directory.
                kill(pidfile)
                raise RuntimeError(f"guest {tag} never reached a debug prompt")
            return sh, pidfile
        time.sleep(0.4)
    raise RuntimeError(f"guest {tag} never opened its serial socket")


def kill(pidfile):
    # Only ever the PID our own launch wrote -- never a pattern match,
    # which cannot tell this guest from the user's own `make run`.
    try:
        with open(pidfile) as f:
            os.kill(int(f.read().strip()), 15)
    except Exception:
        pass


def tx_of(text, name):
    """The tx packet count `ifconfig` reported for one device."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip().startswith(name + ":"):
            for follow in lines[i + 1:i + 6]:
                s = follow.strip()
                if s.startswith("tx "):
                    return int(s.split()[1])
    return None


# --- the phases -------------------------------------------------------

def phase_one_nic(r, disk, tmp, kind, driver):
    pcap = os.path.abspath(os.path.join(tmp, f"net_{kind}.pcap"))
    sh, pidfile = launch(disk, tmp, kind, kind, pcap)
    try:
        cfg = sh.run("ifconfig")
        r.check(f"[{kind}] the card is registered as net0 and named {driver}",
                "net0:" in cfg and driver in cfg, cfg.strip()[-300:])
        r.check(f"[{kind}] it came up with the user-networking address",
                GUEST_IP in cfg, cfg.strip()[-300:])

        out = sh.run(f"ping -c 3 {GATEWAY}", timeout=40.0)
        r.check(f"[{kind}] ping {GATEWAY} answers with no loss",
                "0% packet loss" in out, out.strip()[-400:])
        r.check(f"[{kind}] every reply came from the gateway",
                out.count(f"bytes from {GATEWAY}") >= 3, out.strip()[-400:])

        moved = sh.run("ifconfig")
        r.check(f"[{kind}] the device counters moved",
                (tx_of(moved, "net0") or 0) >= 3, moved.strip()[-300:])
    finally:
        kill(pidfile)
        sh.close()
        time.sleep(1.0)   # QEMU flushes the dump as it exits

    frames = read_pcap(pcap)
    arps, echoes, replies, bad = decode(frames)
    r.check(f"[{kind}] the host captured traffic at all", len(frames) > 0,
            f"{pcap}: {len(frames)} frames")
    r.check(f"[{kind}] an ARP request for the gateway is on the wire, well formed",
            any(a["op"] == 1 and a["target_ip"] == ipv4(GATEWAY)
                and a["sender_ip"] == ipv4(GUEST_IP) for a in arps),
            f"{len(arps)} ARP frames seen")
    # THE LOAD-BEARING CHECK: the host recomputed both checksums from
    # the bytes on the wire. A guest that miscomputes either one still
    # passes every in-guest assertion above if SLIRP happens to answer.
    r.check(f"[{kind}] echo requests carry valid IPv4 and ICMP checksums",
            len(echoes) >= 3 and not bad, f"{len(echoes)} echoes, bad: {sorted(set(bad))}")
    r.check(f"[{kind}] the requests are addressed to the gateway, from us",
            all(e["dst"] == ipv4(GATEWAY) and e["src"] == ipv4(GUEST_IP) for e in echoes),
            f"{len(echoes)} echoes")
    r.check(f"[{kind}] the sequence numbers advance",
            sorted(e["seq"] for e in echoes)[:3] == [1, 2, 3],
            str(sorted(e["seq"] for e in echoes)))
    r.check(f"[{kind}] replies came back and the payload is echoed unchanged",
            len(replies) >= 3 and all(rep["payload"] == echoes[0]["payload"] for rep in replies),
            f"{len(replies)} replies")


def phase_two_nics(r, disk, tmp):
    sh, pidfile = launch(disk, tmp, "both", "both")
    try:
        cfg = sh.run("ifconfig")
        r.check("[both] both cards are registered, on both drivers",
                "net0:" in cfg and "net1:" in cfg and "e1000" in cfg and "virtio-net" in cfg,
                cfg.strip()[-400:])
        # Only the first gets an address: two cards on one address is
        # worse than one card with none (kernel/drivers/net/net.c).
        r.check("[both] only the first card is addressed",
                cfg.count("(unconfigured)") == 1, cfg.strip()[-400:])

        # Move the e1000 to a subnet nothing answers on, and put the
        # user-network address on the virtio card instead. A stack that
        # routes by "the first device" rather than by SUBNET now sends
        # everything into the void.
        sh.run("ifconfig net0 192.168.5.15 255.255.255.0 192.168.5.1")
        sh.run(f"ifconfig net1 {GUEST_IP} 255.255.255.0 {GATEWAY}")
        before = sh.run("ifconfig")
        out = sh.run(f"ping -c 2 {GATEWAY}", timeout=40.0)
        after = sh.run("ifconfig")

        r.check("[both] the ping still answers once the address moved",
                "0% packet loss" in out, out.strip()[-400:])
        b0, a0 = tx_of(before, "net0"), tx_of(after, "net0")
        b1, a1 = tx_of(before, "net1"), tx_of(after, "net1")
        r.check("[both] the traffic left through the card that owns the subnet",
                a1 is not None and b1 is not None and a1 > b1,
                f"net1 tx {b1} -> {a1}")
        r.check("[both] and NOT through the other one",
                a0 == b0, f"net0 tx {b0} -> {a0}")
    finally:
        kill(pidfile)
        sh.close()


def phase_no_nic(r, disk, tmp):
    sh, pidfile = launch(disk, tmp, "none", "none")
    try:
        cfg = sh.run("ifconfig")
        r.check("[none] a machine with no card says so rather than printing nothing",
                "no network devices" in cfg, cfg.strip()[-300:])
        # Must REPORT, not hang: the timeout here is the assertion.
        out = sh.run(f"ping -c 1 {GATEWAY}", timeout=30.0)
        r.check("[none] ping reports and exits instead of hanging",
                "100% packet loss" in out or "failed" in out or "no ARP reply" in out,
                out.strip()[-300:])
    finally:
        kill(pidfile)
        sh.close()


def phase_arp_rate(r, disk, tmp):
    """Pinging an unanswered on-link address must not flood the wire.

    Before kernel/net/arp.c rate-limited requests, `ping` polling every
    10 ms for a second sent one broadcast per poll: 104 frames for two
    pings, measured. The number below is a ceiling with room in it --
    what it catches is a return to one-request-per-retry, not a change
    of a few frames."""
    sh, pidfile = launch(disk, tmp, "rate", "e1000")
    try:
        before = sh.run("ifconfig")
        out = sh.run(f"ping -c 2 {UNANSWERED}", timeout=40.0)
        after = sh.run("ifconfig")
        r.check("[arp] an unanswered address is reported as such",
                "no ARP reply" in out, out.strip()[-300:])
        sent = (tx_of(after, "net0") or 0) - (tx_of(before, "net0") or 0)
        r.check("[arp] the requests are rate limited, not one per retry",
                sent <= 12, f"{sent} frames sent for two pings (was 104 unlimited)")
    finally:
        kill(pidfile)
        sh.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"))
    ap.add_argument("--keep", action="store_true", help="keep the temp directory")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="net_test_")
    # A COPY, sparse: this launches several guests and the user may have
    # their own QEMU holding disk.img's write lock.
    disk = os.path.join(tmp, "net_test.img")
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", args.disk, disk], check=True)

    r = Result()
    try:
        print("Phase 1: the e1000, the card every boot already had")
        phase_one_nic(r, disk, tmp, "e1000", "e1000")
        print("Phase 2: virtio-net, the only path to virtio_net.c")
        phase_one_nic(r, disk, tmp, "virtio", "virtio-net")
        print("Phase 3: two cards, two subnets")
        phase_two_nics(r, disk, tmp)
        print("Phase 4: no card at all")
        phase_no_nic(r, disk, tmp)
        print("Phase 5: the ARP retransmit rate")
        phase_arp_rate(r, disk, tmp)
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"  (kept {tmp})")

    print(f"\nnet_test: {len(r.passes)} passed, {len(r.fails)} failed")
    for f in r.fails:
        print(f"  FAILED: {f}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
