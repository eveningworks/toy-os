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
  6. UDP, with a REAL PYTHON SOCKET on the host as the far end. The
     guest sends a datagram and the host receives it, echoes it, and the
     guest reads the echo -- a round trip through SLIRP where neither
     end shares a line of code with the other. The pcap check here is
     the UDP checksum, which covers a pseudo-header that is not on the
     wire: a stack that omits it agrees with itself perfectly and is
     rejected by everything else.
  7. ICMP port unreachable, judged from the capture: nothing is bound,
     so the guest must answer type 3 code 3 rather than dropping in
     silence.
  8. DHCP on a NON-DEFAULT SLIRP subnet. This is the load-bearing DHCP
     check: on the default network a working client and a hardcoded
     10.0.2.15 are indistinguishable, so the guest is booted on
     192.168.76.0/24 where only a real lease can produce the address.
  9. DNS, through SLIRP's forwarder to the host's own resolver. SKIPS
     when the host cannot resolve -- an offline machine is not a bug in
     this OS -- but the "no nameserver configured" path is checked
     unconditionally, because that one needs nothing but the guest.
 11. The guest as a SERVER: /bin/httpd serves a file the host staged
     into the image, and the host fetches it back with python's own
     http.client and compares it byte for byte. Needs a port forward --
     SLIRP is a NAT, so this is the one direction that does not work
     without one. Its load-bearing check is the SECOND request: a
     listener consumed by its first connection passes everything else.
 10. TCP, against a REAL HTTP SERVER on the host -- python's own
     http.server, which shares no code with this OS and will simply not
     answer a malformed handshake. Deliberately local rather than a site
     on the internet: the suite must not depend on this machine having
     connectivity. The capture is then checked for a genuine three-way
     handshake and for TCP checksums recomputed here, which is the same
     pseudo-header trap UDP has.

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

def nic_args(kind, pcap, extra=""):
    dump = f" -object filter-dump,id=fd0,netdev=n0,file={pcap}" if pcap else ""
    if kind == "none":
        return " -nic none"
    if kind == "e1000":
        return f" -netdev user,id=n0{extra} -device e1000,netdev=n0{dump}"
    if kind == "virtio":
        return f" -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on{dump}"
    if kind == "both":
        return (f" -netdev user,id=n0 -device e1000,netdev=n0{dump}"
                f" -netdev user,id=n1 -device virtio-net-pci,netdev=n1,disable-legacy=on")
    raise ValueError(kind)


def launch(disk, tmp, tag, kind, pcap=None, netdev_extra=""):
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
           f"{nic_args(kind, pcap, netdev_extra)}"
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


UDP_ECHO_PORT = 19999


def udp_echo_server(port, deadline_s=45.0):
    """A real host socket, as the far end of the guest's datagram.

    SLIRP maps traffic the guest sends to 10.0.2.2 onto the host's
    loopback, so this needs no port forwarding -- and it is an oracle in
    the strong sense: a wrong UDP checksum, a wrong length or a wrong
    pseudo-header means SLIRP never hands the datagram over and this
    receives nothing at all."""
    import threading
    got = {}

    def run():
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError as e:
            got["error"] = str(e)
            return
        sock.settimeout(deadline_s)
        try:
            data, addr = sock.recvfrom(2048)
            got["data"] = data
            got["from"] = addr
            sock.sendto(b"pong:" + data, addr)
            got["replied"] = True
        except socket.timeout:
            got["error"] = "timed out"
        finally:
            sock.close()

    t = threading.Thread(target=run, daemon=True)
    t.start()
    time.sleep(0.3)   # bound before the guest is told to send
    return t, got


def tcp_checksum_ok(frame):
    """Recompute a captured segment's TCP checksum, pseudo-header and
    all. TCP has no length field of its own, so the covered length comes
    from the IP total length minus the header -- which is exactly the
    step an implementation can get wrong while agreeing with itself."""
    ihl = (frame[14] & 0x0F) * 4
    total = struct.unpack(">H", frame[16:18])[0]
    seg = frame[14 + ihl:14 + total]
    pseudo = frame[26:34] + b"\x00\x06" + struct.pack(">H", len(seg))
    return checksum(pseudo + seg) == 0


def udp_checksum_ok(frame):
    """Recompute a captured datagram's UDP checksum, pseudo-header and
    all. Independent of the guest by construction."""
    ihl = (frame[14] & 0x0F) * 4
    udp = frame[14 + ihl:]
    length = struct.unpack(">H", udp[4:6])[0]
    udp = udp[:length]
    if struct.unpack(">H", udp[6:8])[0] == 0:
        return True    # "not computed" is legal in IPv4
    pseudo = frame[26:34] + b"\x00\x11" + struct.pack(">H", length)
    return checksum(pseudo + udp) == 0


def phase_udp(r, disk, tmp):
    pcap = os.path.abspath(os.path.join(tmp, "net_udp.pcap"))
    sh, pidfile = launch(disk, tmp, "udp", "e1000", pcap)
    try:
        out = sh.run("udp_test")
        r.check("[udp] the socket API's own checks pass in the guest",
                "0 failed" in out, out.strip()[-500:])

        _, got = udp_echo_server(UDP_ECHO_PORT)
        payload = "toyos-udp-payload"
        out = sh.run(f"udp_test {GATEWAY} {UDP_ECHO_PORT} {payload}", timeout=40.0)

        # Give the host thread a moment to record the reply it sent.
        for _ in range(40):
            if "data" in got or "error" in got:
                break
            time.sleep(0.25)

        r.check("[udp] a real socket ON THE HOST received the guest's datagram",
                got.get("data") == payload.encode(),
                f"host got {got.get('data')!r}, error {got.get('error')!r}")
        # NOT checked at the host socket: SLIRP is a NAT and rewrites
        # the source port, so what a host socket sees is SLIRP's port
        # and never the guest's. The guest's own port is in the capture
        # below, which is the only place it survives.
        r.check("[udp] and the guest read the reply back",
                f"pong:{payload}" in out, out.strip()[-400:])
    finally:
        kill(pidfile)
        sh.close()
        time.sleep(1.0)

    frames = read_pcap(pcap)
    udps = [f for f in frames
            if len(f) >= 34 and struct.unpack(">H", f[12:14])[0] == 0x0800 and f[23] == 17]
    r.check("[udp] datagrams reached the wire", len(udps) >= 1, f"{len(udps)} UDP frames")
    # The guest's OWN source port, before SLIRP translates it. An
    # unbound sender must have been given one from the ephemeral range;
    # a stack that sent from port 0 would still round-trip through a NAT
    # and pass every check above.
    # OUTBOUND frames only: a capture holds both directions, and the
    # replies come from the port the host was listening on.
    sports = []
    for f in udps:
        if struct.unpack(">I", f[26:30])[0] != ipv4(GUEST_IP):
            continue
        ihl = (f[14] & 0x0F) * 4
        sports.append(struct.unpack(">H", f[14 + ihl:14 + ihl + 2])[0])
    r.check("[udp] the guest sent from an ephemeral port",
            bool(sports) and all(49152 <= p <= 65535 for p in sports), str(sports))
    # THE LOAD-BEARING CHECK: the pseudo-header is not in the packet, so
    # only an implementation that reconstructs it correctly passes.
    r.check("[udp] every datagram carries a valid checksum over the pseudo-header",
            all(udp_checksum_ok(f) for f in udps), f"{len(udps)} checked")


def phase_port_unreachable(r, disk, tmp):
    pcap = os.path.abspath(os.path.join(tmp, "net_unreach.pcap"))
    # hostfwd is what lets the HOST start the conversation: it maps a
    # host port onto a guest port, which is the only way to make a
    # datagram ARRIVE at a port the guest has nothing bound to.
    extra = f",hostfwd=udp::{UDP_ECHO_PORT + 1}-:{UDP_ECHO_PORT + 1}"
    sh, pidfile = launch(disk, tmp, "unreach", "e1000", pcap, netdev_extra=extra)
    try:
        sh.run("ifconfig")   # settle; nothing is bound to that port
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(3):
            sock.sendto(b"nobody-is-listening", ("127.0.0.1", UDP_ECHO_PORT + 1))
            time.sleep(0.3)
        sock.close()
        time.sleep(1.5)
        sh.run("ifconfig")   # a command, so the guest's idle loop runs
    finally:
        kill(pidfile)
        sh.close()
        time.sleep(1.0)

    frames = read_pcap(pcap)
    unreachable = []
    for f in frames:
        if len(f) < 34 or struct.unpack(">H", f[12:14])[0] != 0x0800 or f[23] != 1:
            continue
        ihl = (f[14] & 0x0F) * 4
        icmp = f[14 + ihl:]
        if len(icmp) >= 2 and icmp[0] == 3 and icmp[1] == 3:
            unreachable.append(icmp)
    r.check("[unreach] the guest answers an unbound port with ICMP type 3 code 3",
            len(unreachable) >= 1, f"{len(unreachable)} reports in {len(frames)} frames")
    # The report is only useful if it quotes enough for the sender to
    # match it: the IP header plus the first 8 bytes after it.
    r.check("[unreach] and quotes the offending datagram's header",
            all(len(i) >= 8 + 20 + 8 for i in unreachable),
            str([len(i) for i in unreachable]))


def phase_dhcp(r, disk, tmp):
    """A NON-DEFAULT SLIRP subnet, which is the whole point.

    On QEMU's default network a working DHCP client and a hardcoded
    10.0.2.15 produce the same `ifconfig` output, so this boots on
    192.168.76.0/24 where the address can only have come from a lease."""
    extra = ",net=192.168.76.0/24,host=192.168.76.2,dhcpstart=192.168.76.20"
    sh, pidfile = launch(disk, tmp, "dhcp", "e1000", None, netdev_extra=extra)
    try:
        before = sh.run("ifconfig")
        r.check("[dhcp] before the lease the guest has the built-in default",
                "10.0.2.15" in before, before.strip()[-300:])

        out = sh.run("dhcp", timeout=40.0)
        r.check("[dhcp] the client gets a lease from the server on this segment",
                "192.168.76." in out, out.strip()[-400:])
        r.check("[dhcp] and a nameserver with it",
                "nameserver 192.168.76." in out, out.strip()[-400:])

        after = sh.run("ifconfig")
        r.check("[dhcp] the address is applied to the device",
                "192.168.76." in after and "10.0.2.15" not in after, after.strip()[-400:])
        r.check("[dhcp] with the gateway this network actually has",
                "192.168.76.2" in after, after.strip()[-400:])

        # The lease is only real if it works: a client that applied a
        # plausible address to the wrong device would pass everything
        # above and fail this.
        ping = sh.run("ping -c 2 192.168.76.2", timeout=40.0)
        r.check("[dhcp] and the gateway answers on the leased address",
                "0% packet loss" in ping, ping.strip()[-400:])
    finally:
        kill(pidfile)
        sh.close()


def host_can_resolve():
    try:
        socket.setdefaulttimeout(4)
        socket.gethostbyname("example.com")
        return True
    except Exception:
        return False


def phase_dns(r, disk, tmp):
    sh, pidfile = launch(disk, tmp, "dns", "e1000")
    try:
        # The deterministic half first: with no resolver configured the
        # failure must NAME what is missing. This needs no network at
        # all, so it runs on an offline machine too.
        sh.run("rm /etc/resolv.conf")
        out = sh.run("host example.com", timeout=30.0)
        r.check("[dns] with no nameserver configured, host says so",
                "no nameserver" in out, out.strip()[-300:])

        out = sh.run("dhcp", timeout=40.0)
        r.check("[dns] dhcp writes the resolver it was given",
                "/etc/resolv.conf" in out, out.strip()[-300:])

        if not host_can_resolve():
            print("  SKIP  [dns] the host cannot resolve; skipping the live lookups")
            return

        out = sh.run("host example.com", timeout=40.0)
        r.check("[dns] a name resolves through SLIRP's forwarder",
                "has address" in out, out.strip()[-300:])

        # A name that does not exist must be told apart from a server
        # that did not answer -- different codes, different fixes.
        out = sh.run("host no-such-host.invalid", timeout=40.0)
        r.check("[dns] a nonexistent name is reported as not found",
                "not found" in out, out.strip()[-300:])

        out = sh.run("ping -c 2 example.com", timeout=60.0)
        r.check("[dns] ping resolves a name and reaches it",
                "0% packet loss" in out, out.strip()[-400:])
    finally:
        kill(pidfile)
        sh.close()


HTTP_PORT = 18088
HTTP_BODY = b"toy-os fetched this\n" * 4


def http_server(port, deadline_s=60.0):
    """A real HTTP server on the host, as the far end of the guest's
    connection. Python's own http.server: an independent implementation
    that will not complete a handshake this OS gets wrong, and will not
    answer a request it cannot parse."""
    import http.server
    import socketserver
    import threading

    state = {"served": 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            state["served"] += 1
            state["path"] = self.path
            state["agent"] = self.headers.get("User-Agent", "")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(HTTP_BODY)))
            self.end_headers()
            self.wfile.write(HTTP_BODY)

        def log_message(self, *a):
            pass

    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.TCPServer(("127.0.0.1", port), Handler)
    srv.timeout = deadline_s
    t = threading.Thread(target=srv.serve_forever, kwargs={"poll_interval": 0.2},
                         daemon=True)
    t.start()
    return srv, state


def phase_tcp(r, disk, tmp):
    pcap = os.path.abspath(os.path.join(tmp, "net_tcp.pcap"))
    srv, state = http_server(HTTP_PORT)
    sh, pidfile = launch(disk, tmp, "tcp", "e1000", pcap)
    try:
        out = sh.run(f"wget http://{GATEWAY}:{HTTP_PORT}/toyos", timeout=60.0)
        r.check("[tcp] the guest fetched the page",
                "toy-os fetched this" in out, out.strip()[-400:])
        # THE HOST'S VIEW, which a guest agreeing with itself cannot fake:
        # python parsed a real request off a real connection.
        r.check("[tcp] a real HTTP server on the host served the request",
                state.get("served", 0) >= 1, str(state))
        r.check("[tcp] and saw the path and agent the guest sent",
                state.get("path") == "/toyos" and "toy-os" in state.get("agent", ""),
                str(state))

        # -O writes it to the guest's own filesystem, read back with a
        # different program -- so "the bytes arrived" is checked through
        # something that never touched the socket.
        sh.run(f"wget -O /tmp/fetched.txt http://{GATEWAY}:{HTTP_PORT}/f", timeout=60.0)
        out = sh.run("cat /tmp/fetched.txt")
        r.check("[tcp] a fetched body lands on disk intact",
                out.count("toy-os fetched this") >= 4, out.strip()[-300:])

        # Nothing listens on port 9. A RST must become "refused" rather
        # than a timeout -- different codes, different fixes.
        out = sh.run(f"wget http://{GATEWAY}:9/nope", timeout=40.0)
        r.check("[tcp] a refused connection says so, and does not hang",
                "refused" in out, out.strip()[-300:])
    finally:
        srv.shutdown()
        srv.server_close()
        kill(pidfile)
        sh.close()
        time.sleep(1.0)

    frames = read_pcap(pcap)
    segs = []
    for f in frames:
        if len(f) < 34 or struct.unpack(">H", f[12:14])[0] != 0x0800 or f[23] != 6:
            continue
        ihl = (f[14] & 0x0F) * 4
        tcp = f[14 + ihl:]
        total = struct.unpack(">H", f[16:18])[0]
        tcp = tcp[:total - ihl]
        segs.append({
            "src": struct.unpack(">I", f[26:30])[0],
            "flags": tcp[13],
            "ok": tcp_checksum_ok(f),
        })

    ours = [x for x in segs if x["src"] == ipv4(GUEST_IP)]
    r.check("[tcp] segments reached the wire", len(ours) >= 3, f"{len(segs)} TCP frames")
    # A real handshake: our SYN, their SYN+ACK, our ACK. A stack that
    # merely emitted something would pass the fetch check if the server
    # were forgiving; python is not, but the capture says it outright.
    r.check("[tcp] the guest sent a bare SYN and later a FIN",
            any(x["flags"] == 0x02 for x in ours) and
            any(x["flags"] & 0x01 for x in ours),
            str(sorted({x["flags"] for x in ours})))
    r.check("[tcp] the peer answered SYN+ACK",
            any(x["src"] != ipv4(GUEST_IP) and x["flags"] == 0x12 for x in segs),
            str(sorted({x["flags"] for x in segs if x["src"] != ipv4(GUEST_IP)})))
    # Same pseudo-header trap as UDP, recomputed here.
    r.check("[tcp] every segment carries a valid checksum over the pseudo-header",
            all(x["ok"] for x in segs), f"{len(segs)} checked")


def _volume_args(disk):
    """Where the TFS3 volume sits in the image. Asked rather than
    assumed: hardcoding LBA 2048 is the pointer-somebody-must-maintain
    shape, and "partition 1" is wrong too -- volume_of() finds it by
    looking. (ls_test.py stages its fixture the same way.)"""
    import mkpart_test
    base, sectors = mkpart_test.volume_of(disk)
    return ["--at-lba", str(base), "--sectors", str(sectors)] if base else []


SERVER_PORT = 18089
SERVER_FILE = "httpd_probe.txt"
SERVER_TEXT = "served from inside toy-os\n" * 3


def phase_server(r, disk, tmp):
    """The guest as the SERVER: the host connects IN.

    This is the one phase that needs a port forward. SLIRP is a NAT, so
    everything else in this file works with no configuration and nothing
    can start a conversation WITH the guest without hostfwd.

    The oracle is curl-free on purpose -- python's own http.client, so
    the check does not depend on a binary this machine may not have --
    and the body is compared against the file as STAGED FROM THE HOST,
    which is the only way to know the bytes survived the round trip
    rather than merely arriving."""
    import http.client

    # The file the guest will serve, written into the image from here.
    subprocess.run([sys.executable, os.path.join(HERE, "tfs3_writer.py"),
                    "mkdir", disk, "/tmp", *_volume_args(disk)],
                   cwd=ROOT, capture_output=True)
    probe = os.path.join(tmp, SERVER_FILE)
    with open(probe, "w") as f:
        f.write(SERVER_TEXT)
    w = subprocess.run([sys.executable, os.path.join(HERE, "tfs3_writer.py"),
                        "write", disk, probe, "/tmp/" + SERVER_FILE,
                        *_volume_args(disk)],
                       cwd=ROOT, capture_output=True, text=True)
    if w.returncode != 0:
        r.check("[server] the probe file could be staged", False,
                (w.stdout + w.stderr)[-300:])
        return

    extra = f",hostfwd=tcp::{SERVER_PORT}-:80"
    sh, pidfile = launch(disk, tmp, "server", "e1000", None, netdev_extra=extra)
    try:
        # httpd holds the console, so the command is sent and not waited
        # for -- the assertion is what answers on the socket, not what
        # the shell prints.
        sh.s.sendall(b"sh httpd /tmp\n")
        time.sleep(3.0)

        body, status, listing = None, 0, ""
        try:
            conn = http.client.HTTPConnection("127.0.0.1", SERVER_PORT, timeout=15)
            conn.request("GET", "/" + SERVER_FILE)
            resp = conn.getresponse()
            status = resp.status
            body = resp.read().decode("utf-8", "replace")
            conn.close()
        except Exception as e:
            body = f"<{e}>"

        r.check("[server] the host connected to a server INSIDE the guest",
                status == 200, f"status {status}, body {body!r}")
        # Byte for byte against what the host wrote into the image: a
        # response that arrives truncated or re-ordered fails here and
        # passes every check that only asks whether something came back.
        r.check("[server] and got the file back exactly as staged",
                body == SERVER_TEXT, repr(body)[:300])

        try:
            conn = http.client.HTTPConnection("127.0.0.1", SERVER_PORT, timeout=15)
            conn.request("GET", "/")
            resp = conn.getresponse()
            listing = resp.read().decode("utf-8", "replace")
            conn.close()
        except Exception as e:
            listing = f"<{e}>"
        r.check("[server] a directory is served as a listing",
                SERVER_FILE in listing, listing[:300])

        # A SECOND request proves the server went back to accepting --
        # a listener consumed by its first connection passes everything
        # above.
        second = 0
        try:
            conn = http.client.HTTPConnection("127.0.0.1", SERVER_PORT, timeout=15)
            conn.request("GET", "/" + SERVER_FILE)
            second = conn.getresponse().status
            conn.close()
        except Exception:
            pass
        r.check("[server] and it serves a second client after the first",
                second == 200, f"status {second}")

        # A path leaving the root is refused rather than resolved.
        forbidden = 0
        try:
            conn = http.client.HTTPConnection("127.0.0.1", SERVER_PORT, timeout=15)
            conn.putrequest("GET", "/../etc/toyos.conf", skip_host=True,
                            skip_accept_encoding=True)
            conn.endheaders()
            forbidden = conn.getresponse().status
            conn.close()
        except Exception:
            pass
        r.check("[server] a path leaving the served root is refused",
                forbidden == 403, f"status {forbidden}")
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
        print("Phase 6: UDP, with a real host socket at the far end")
        phase_udp(r, disk, tmp)
        print("Phase 7: ICMP port unreachable")
        phase_port_unreachable(r, disk, tmp)
        print("Phase 8: DHCP, on a subnet the default cannot produce")
        phase_dhcp(r, disk, tmp)
        print("Phase 9: DNS")
        phase_dns(r, disk, tmp)
        print("Phase 10: TCP, against a real HTTP server on the host")
        phase_tcp(r, disk, tmp)
        print("Phase 11: the guest as a SERVER, with the host connecting in")
        phase_server(r, disk, tmp)
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
