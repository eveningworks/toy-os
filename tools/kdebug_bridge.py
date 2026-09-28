#!/usr/bin/env python3
"""Carry GDB to the kernel debugger's NETWORK transport.

`kdebug=net,...` (docs/kdebug-design.md, stage 3) speaks GDB's protocol
inside authenticated UDP datagrams: magic, a sequence number, 16 bytes of
HMAC-SHA256 under the boot line's key, then the payload. GDB itself speaks
plain RSP over TCP, so this sits between them -- the job WinDbg's own
KDNET client does on Windows:

    gdb --TCP--> 127.0.0.1:1235  [this]  --UDP, keyed--> target:50000

    make run KDEBUG=net                        # terminal 1
    python3 tools/kdebug_bridge.py             # terminal 2
    gdb build/kernel.bin -ex "target remote localhost:1235"

The defaults match `make run KDEBUG=net`: target 127.0.0.1:50000 (QEMU's
forward onto the debugger's card) and the key in build/kdebug.key. For a
real machine: --target <its ip>:50000 --key <the hex on its GRUB line>.

EVERY GDB CONNECT OPENS A SESSION: a keyed hello carrying a fresh nonce,
answered by the target with one of its own, and every datagram's MAC then
covers both. A datagram recorded in any other session or boot fails its
MAC, in either direction, and so does one that fails it or does not carry
a sequence number above the last accepted -- dropped without a word. So
the bridge can outlive the target: a reboot needs only a reconnect.
Only the stdlib: hmac and hashlib.
"""

import argparse
import hashlib
import hmac
import os
import select
import socket
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_FILE = os.path.join(REPO, "build", "kdebug.key")
HDR = 28
NONCE = 16
PAYLOAD_MAX = 1400


def mac16(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()[:16]


def seal(key, magic, seq, payload, nonces):
    """A data datagram of the session `nonces` (host nonce + target nonce)."""
    head = magic + seq.to_bytes(8, "little")
    return head + mac16(key, head + nonces + payload) + payload


def open_(key, magic, data, last_seq, nonces):
    """(seq, payload) for an authentic datagram of this session newer than
    last_seq, else None."""
    if len(data) < HDR or data[:4] != magic:
        return None
    seq = int.from_bytes(data[4:12], "little")
    want = mac16(key, data[:12] + nonces + data[HDR:])
    if not hmac.compare_digest(want, data[12:HDR]) or seq <= last_seq:
        return None
    return seq, data[HDR:]


def hello_query(key, hn):
    return b"TKDQ" + hn + mac16(key, b"TKDQ" + hn)


def hello_answer(key, hn, data):
    """The target's nonce from its answer to OUR hello `hn`, else None."""
    if len(data) != 4 + 2 * NONCE + 16 or data[:4] != b"TKDN" or data[4:4 + NONCE] != hn:
        return None
    if not hmac.compare_digest(mac16(key, data[:4 + 2 * NONCE]), data[4 + 2 * NONCE:]):
        return None
    return data[4 + NONCE:4 + 2 * NONCE]


class UdpLink:
    """A byte stream over the keyed datagrams, shaped like a socket --
    sendall/recv/settimeout/close -- so a protocol client can sit on it."""

    def __init__(self, host, port, key):
        self.key = key
        self.addr = (host, port)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.nonces = None     # set by hello()
        self.seq = self.last_rx = 0
        self.pending = b""
        self.first = None      # the first datagram sent, for a replay test
        self.first_rx = None   # and the first received

    def hello(self, tries=5, wait=1.0):
        """Open a session. Each try has its OWN nonce, so a late answer to
        an earlier one cannot set the session. False if nothing answered."""
        old = self.s.gettimeout()
        try:
            for _ in range(tries):
                hn = os.urandom(NONCE)
                self.s.sendto(hello_query(self.key, hn), self.addr)
                end = time.monotonic() + wait
                while time.monotonic() < end:
                    self.s.settimeout(max(end - time.monotonic(), 0.01))
                    try:
                        data, _ = self.s.recvfrom(65536)
                    except (socket.timeout, BlockingIOError):
                        break
                    tn = hello_answer(self.key, hn, data)
                    if tn:
                        self.nonces = hn + tn
                        self.seq = self.last_rx = 0
                        self.pending = b""
                        return True
            return False
        finally:
            self.s.settimeout(old)

    def seal(self, payload):
        self.seq += 1
        return seal(self.key, b"TKDH", self.seq, payload, self.nonces)

    def open(self, data):
        """An authentic reply's payload (and the counter moved on), else None."""
        got = open_(self.key, b"TKDT", data, self.last_rx, self.nonces)
        if not got:
            return None
        self.first_rx = self.first_rx or data
        self.last_rx, payload = got
        return payload

    def sendall(self, data):
        if self.nonces is None and not self.hello():
            raise ConnectionError("the target did not answer the hello")
        for i in range(0, max(len(data), 1), PAYLOAD_MAX):
            d = self.seal(data[i:i + PAYLOAD_MAX])
            self.first = self.first or d
            self.s.sendto(d, self.addr)

    def send_raw(self, datagram):
        self.s.sendto(datagram, self.addr)

    def settimeout(self, t):
        self.s.settimeout(t)

    def recv(self, n):
        while not self.pending:
            data, _ = self.s.recvfrom(65536)
            self.pending = self.open(data) or b""
        out, self.pending = self.pending[:n], self.pending[n:]
        return out

    def close(self):
        self.s.close()


def load_key(arg):
    if arg:
        return bytes.fromhex(arg)
    with open(KEY_FILE) as f:
        return bytes.fromhex(f.read().strip())


def serve(listen, link):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(listen)
    srv.listen(1)
    print(f"kdebug_bridge: gdb -> {listen[0]}:{listen[1]}, target {link.addr[0]}:{link.addr[1]}",
          flush=True)
    while True:
        conn, _ = srv.accept()
        link.s.setblocking(True)
        if not link.hello():
            print(f"kdebug_bridge: no answer from {link.addr[0]}:{link.addr[1]} -- "
                  "is it armed, and is this its key?", flush=True)
            conn.close()
            continue
        print("kdebug_bridge: debugger connected, new session", flush=True)
        link.s.setblocking(False)
        try:
            while True:
                r, _, _ = select.select([conn, link.s], [], [])
                if conn in r:
                    data = conn.recv(65536)
                    if not data:
                        break
                    link.sendall(data)
                if link.s in r:
                    try:
                        data, _ = link.s.recvfrom(65536)
                    except BlockingIOError:
                        continue
                    payload = link.open(data)
                    if payload:
                        conn.sendall(payload)
        finally:
            conn.close()
            print("kdebug_bridge: debugger disconnected", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--target", default="127.0.0.1:50000", help="HOST:PORT of the stub")
    ap.add_argument("--listen", default="127.0.0.1:1235", help="where gdb connects")
    ap.add_argument("--key", default=None, help=f"the key in hex (default: {KEY_FILE})")
    args = ap.parse_args()
    try:
        key = load_key(args.key)
    except (OSError, ValueError) as e:
        print(f"kdebug_bridge: no key: {e}", file=sys.stderr)
        return 2
    host, port = args.target.rsplit(":", 1)
    lhost, lport = args.listen.rsplit(":", 1)
    try:
        serve((lhost, int(lport)), UdpLink(host, int(port), key))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
