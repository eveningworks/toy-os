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

A datagram that fails its MAC, or does not carry a sequence number above
the last one accepted, is dropped without a word, in both directions.
The sequence numbers this sends start at the wall clock in nanoseconds,
so a restarted bridge is never below what the target has already seen.
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
PAYLOAD_MAX = 1400


def seal(key, magic, seq, payload):
    head = magic + seq.to_bytes(8, "little")
    mac = hmac.new(key, head + payload, hashlib.sha256).digest()[:16]
    return head + mac + payload


def open_(key, magic, data, last_seq):
    """(seq, payload) for an authentic datagram newer than last_seq, else None."""
    if len(data) < HDR or data[:4] != magic:
        return None
    seq = int.from_bytes(data[4:12], "little")
    want = hmac.new(key, data[:12] + data[HDR:], hashlib.sha256).digest()[:16]
    if not hmac.compare_digest(want, data[12:HDR]) or seq <= last_seq:
        return None
    return seq, data[HDR:]


class UdpLink:
    """A byte stream over the keyed datagrams, shaped like a socket --
    sendall/recv/settimeout/close -- so a protocol client can sit on it."""

    def __init__(self, host, port, key):
        self.key = key
        self.addr = (host, port)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = time.time_ns()
        self.last_rx = 0
        self.pending = b""
        self.first = None      # the first datagram sent, for a replay test

    def sendall(self, data):
        for i in range(0, max(len(data), 1), PAYLOAD_MAX):
            self.seq += 1
            d = seal(self.key, b"TKDH", self.seq, data[i:i + PAYLOAD_MAX])
            self.first = self.first or d
            self.s.sendto(d, self.addr)

    def send_raw(self, datagram):
        self.s.sendto(datagram, self.addr)

    def settimeout(self, t):
        self.s.settimeout(t)

    def recv(self, n):
        while not self.pending:
            data, _ = self.s.recvfrom(65536)
            got = open_(self.key, b"TKDT", data, self.last_rx)
            if got:
                self.last_rx, self.pending = got
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
        print("kdebug_bridge: debugger connected", flush=True)
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
                    got = open_(link.key, b"TKDT", data, link.last_rx)
                    if got:
                        link.last_rx, payload = got
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
