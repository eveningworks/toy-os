#!/usr/bin/env python3
"""How long a VNC viewer waits for the screen to answer -- a number for
/bin/remoted's responsiveness, so a change to it is measured, not felt.

Each round moves the pointer (or, with --type, opens nothing and types
into whatever has focus), asks for an incremental update and times the
reply: the first byte, the whole update, and its size. Pixels are not
decoded -- only the rectangles' lengths are read -- so the client costs
nothing next to what it measures. With the Cursor pseudo-encoding
offered, a pointer move that needs no pixels shows up as an EMPTY wait:
the update never comes, and that is the point -- a round reports the
timeout as `none`.

  python3 tools/vnc_latency.py --host <machine-ip> --password PW
  python3 tools/vnc_latency.py --host 127.0.0.1 --port 15903 --rounds 50

On demand, against a machine you name; it changes nothing there but the
pointer's position.
"""
import argparse
import os
import socket
import statistics
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vnc_test import des_response   # noqa: E402

CURSOR = -239


class Probe:
    def __init__(self, host, port, password):
        self.s = socket.create_connection((host, port), timeout=10)
        if self.recv(12)[:8] != b"RFB 003.":
            raise RuntimeError("not an RFB server")
        self.s.sendall(b"RFB 003.008\n")
        self.recv(self.recv(1)[0])
        self.s.sendall(b"\x02")
        self.s.sendall(des_response(password, self.recv(16)))
        if struct.unpack(">I", self.recv(4))[0]:
            raise PermissionError("refused")
        self.s.sendall(b"\x01")
        self.w, self.h = struct.unpack(">HH", self.recv(4))
        self.recv(16)
        self.recv(struct.unpack(">I", self.recv(4))[0])
        self.z = zlib.decompressobj()

    def recv(self, n):
        out = b""
        while len(out) < n:
            c = self.s.recv(n - len(out))
            if not c:
                raise EOFError
            out += c
        return out

    def encodings(self, encs):
        self.s.sendall(struct.pack(">BxH", 2, len(encs)) + b"".join(struct.pack(">i", e) for e in encs))

    def request(self, inc):
        self.s.sendall(struct.pack(">BBHHHH", 3, inc, 0, 0, self.w, self.h))

    def update(self):
        """Reads one FramebufferUpdate; returns (bytes, rects)."""
        t = self.recv(1)[0]
        if t != 0:
            raise RuntimeError(f"message {t}")
        self.recv(1)
        n = struct.unpack(">H", self.recv(2))[0]
        total = 4
        for _ in range(n):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
            total += 12
            if enc == 0:
                total += len(self.recv(w * h * 4))
            elif enc == 16:
                ln = struct.unpack(">I", self.recv(4))[0]
                self.z.decompress(self.recv(ln))   # keeps the stream in step
                total += 4 + ln
            elif enc == CURSOR:
                total += len(self.recv(w * h * 4 + ((w + 7) // 8) * h))
            elif enc == -223:
                pass
            else:
                raise RuntimeError(f"encoding {enc}")
        return total, n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--password", required=True)
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--encoding", choices=("zrle", "raw"), default="zrle")
    ap.add_argument("--no-cursor", action="store_true", help="do not offer the Cursor pseudo-encoding")
    args = ap.parse_args()

    p = Probe(args.host, args.port, args.password)
    encs = [16 if args.encoding == "zrle" else 0]
    if not args.no_cursor:
        encs.append(CURSOR)
    p.encodings(encs)
    t0 = time.time()
    p.request(0)
    size, _ = p.update()
    print(f"full frame: {p.w}x{p.h}, {size} bytes in {(time.time() - t0) * 1000:.0f} ms")

    lat, sizes, none = [], [], 0
    x, y = p.w // 2, p.h // 2
    p.s.settimeout(1.0)
    for i in range(args.rounds):
        x = p.w // 4 + (i * 37) % (p.w // 2)
        y = p.h // 4 + (i * 23) % (p.h // 2)
        t0 = time.time()
        p.s.sendall(struct.pack(">BBHH", 5, 0, x, y))
        p.request(1)
        try:
            size, _ = p.update()
            lat.append((time.time() - t0) * 1000)
            sizes.append(size)
        except socket.timeout:
            none += 1
            # The request is still pending; the next update answers it.
            p.s.settimeout(2.0)
            try:
                p.update()
            except socket.timeout:
                pass
            p.s.settimeout(1.0)
        time.sleep(0.05)
    if lat:
        print(f"pointer move -> update: median {statistics.median(lat):.0f} ms, "
              f"p90 {sorted(lat)[int(len(lat) * 0.9) - 1]:.0f} ms, "
              f"max {max(lat):.0f} ms over {len(lat)} rounds; "
              f"median {int(statistics.median(sizes))} bytes")
    print(f"rounds with no update within 1 s: {none}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
