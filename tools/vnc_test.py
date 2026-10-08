#!/usr/bin/env python3
"""/bin/remoted's VNC server, end to end, from a VNC client written here.

WHAT IT CHECKS
  1. A WRONG PASSWORD IS REFUSED with RFB 3.8's reason string, and the
     right one is let in -- the DES here is OpenSSL's, so a broken
     cipher on either side fails the right-password case.
  2. THE PICTURE IS THE SCREEN: a full Raw update is compared pixel for
     pixel with QEMU's own screendump, the one oracle the guest cannot
     fake (CLAUDE.md). A broken encoder, stride or pixel format moves
     most of the frame, so the bar is 99.5% identical -- what is left
     is the pointer, which QEMU's dump does not draw and remoted does.
  3. ZRLE DECODES TO THE SAME FRAME as Raw, at 32 bits and at 16 bits
     (RGB565, compared after the same quantisation), through Python's
     zlib -- which shares nothing with udeflate.c.
  4. INPUT ARRIVES AS THE MACHINE'S OWN: a PointerEvent puts the
     pointer where the compositor reports it, Super opens the Start
     menu, and text typed into a Terminal -- capitals and Shift
     punctuation included -- reaches the shell's history file.
  5. AN INCREMENTAL UPDATE CARRIES ONLY WHAT CHANGED: after the Start
     menu opens, the rectangles sent cover less than the screen and the
     patched frame matches a fresh screendump again.

ON DEMAND (ondemand_sweep.py): it boots its own guest on a copy of
disk.img, about two minutes under TCG.

  python3 tools/vnc_test.py
  python3 tools/vnc_test.py --instance 3 --keep    # leave the guest up
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import port_guard                  # noqa: E402
from harness import copy_disk      # noqa: E402
from qmp_test import QMPSession    # noqa: E402
from gui_debug import DebugConsole  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")
PASSWORD = "toyvnc12"

CONF = f"""when = always
from = anywhere
[vnc]
enabled = yes
port = 5900
password = {PASSWORD}
"""


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, what, ok, detail=""):
        (self.passes if ok else self.fails).append(what)
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}"
              + (f"\n          {detail}" if detail and not ok else ""))


# --- a small RFB client ------------------------------------------------------

def des_response(password, challenge):
    """VNC Authentication: DES-ECB of the challenge, key bytes bit-reversed."""
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in password.encode()[:8].ljust(8, b"\0"))
    r = subprocess.run(["openssl", "enc", "-des-ecb", "-nopad", "-K", key.hex(),
                        "-provider", "legacy", "-provider", "default"],
                       input=challenge, capture_output=True, check=True)
    return r.stdout


class Viewer:
    def __init__(self, port, password, timeout=30):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.z = zlib.decompressobj()
        self.bpp = 32
        if self.recv(12) != b"RFB 003.008\n":
            raise RuntimeError("no RFB 3.8 banner")
        self.s.sendall(b"RFB 003.008\n")
        n = self.recv(1)[0]
        types = self.recv(n)
        if 2 not in types:
            raise RuntimeError(f"no VNC auth offered: {types!r}")
        self.s.sendall(b"\x02")
        self.s.sendall(des_response(password, self.recv(16)))
        res = struct.unpack(">I", self.recv(4))[0]
        if res != 0:
            ln = struct.unpack(">I", self.recv(4))[0]
            raise PermissionError(self.recv(ln).decode())
        self.s.sendall(b"\x01")   # shared
        self.w, self.h = struct.unpack(">HH", self.recv(4))
        self.recv(16)
        self.name = self.recv(struct.unpack(">I", self.recv(4))[0]).decode()
        self.fb = bytearray(self.w * self.h * 3)
        self.cursor = None

    def recv(self, n):
        out = b""
        while len(out) < n:
            c = self.s.recv(n - len(out))
            if not c:
                raise EOFError("server closed")
            out += c
        return out

    def set_pixel_format(self, bpp):
        if bpp == 32:
            pf = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        else:   # RGB565, little-endian
            pf = struct.pack(">BBBBHHHBBBxxx", 16, 16, 0, 1, 31, 63, 31, 11, 5, 0)
        self.s.sendall(b"\x00\x00\x00\x00" + pf)
        self.bpp = bpp

    def set_encodings(self, encs):
        self.s.sendall(struct.pack(">BxH", 2, len(encs)) + b"".join(
            struct.pack(">i", e) for e in encs))

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.s.sendall(struct.pack(">BBHHHH", 3, int(incremental), x, y,
                                   w or self.w, h or self.h))

    def key(self, keysym, down):
        self.s.sendall(struct.pack(">BBxxI", 4, int(down), keysym))

    def tap(self, keysym):
        self.key(keysym, True)
        self.key(keysym, False)

    def pointer(self, x, y, mask=0):
        self.s.sendall(struct.pack(">BBHH", 5, mask, x, y))

    def rgb(self, raw):
        if self.bpp == 32:
            v = struct.unpack("<I", raw)[0]
            return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
        v = struct.unpack("<H", raw)[0]
        return ((v >> 11) & 31, (v >> 5) & 63, v & 31)

    def put(self, x, y, rgb):
        o = (y * self.w + x) * 3
        self.fb[o:o + 3] = bytes(rgb)

    def read_update(self):
        """One FramebufferUpdate into self.fb; returns its rectangles."""
        t = self.recv(1)[0]
        while t != 0:
            raise RuntimeError(f"unexpected server message {t}")
        self.recv(1)
        n = struct.unpack(">H", self.recv(2))[0]
        rects = []
        for _ in range(n):
            x, y, w, h, enc = struct.unpack(">HHHHi", self.recv(12))
            rects.append((x, y, w, h, enc))
            if enc == 0:
                bp = self.bpp // 8
                data = self.recv(w * h * bp)
                for j in range(h):
                    for i in range(w):
                        o = (j * w + i) * bp
                        self.put(x + i, y + j, self.rgb(data[o:o + bp]))
            elif enc == 16:
                ln = struct.unpack(">I", self.recv(4))[0]
                self.zrle(x, y, w, h, self.z.decompress(self.recv(ln)))
            elif enc == -239:   # the pointer's shape: pixels, then a 1-bit mask
                bp = self.bpp // 8
                self.cursor = (x, y, w, h, self.recv(w * h * bp), self.recv((w + 7) // 8 * h))
            else:
                raise RuntimeError(f"encoding {enc} was not asked for")
        return rects

    def cpixel(self, d, o):
        if self.bpp == 32:
            return (d[o + 2], d[o + 1], d[o]), o + 3     # little-endian, low 3 bytes
        return self.rgb(d[o:o + 2]), o + 2

    def zrle(self, x0, y0, w, h, d):
        o = 0
        for ty in range(y0, y0 + h, 64):
            for tx in range(x0, x0 + w, 64):
                tw, th = min(64, x0 + w - tx), min(64, y0 + h - ty)
                sub = d[o]
                o += 1
                px = []
                if sub == 0:
                    for _ in range(tw * th):
                        c, o = self.cpixel(d, o)
                        px.append(c)
                elif sub == 1:
                    c, o = self.cpixel(d, o)
                    px = [c] * (tw * th)
                elif 2 <= sub <= 16:
                    pal = []
                    for _ in range(sub):
                        c, o = self.cpixel(d, o)
                        pal.append(c)
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    for _ in range(th):
                        rowbytes = (tw * bits + 7) // 8
                        row = d[o:o + rowbytes]
                        o += rowbytes
                        for i in range(tw):
                            bit = i * bits
                            v = (row[bit // 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)
                            px.append(pal[v])
                elif sub == 128 or sub >= 130:
                    pal = []
                    if sub >= 130:
                        for _ in range(sub - 128):
                            c, o = self.cpixel(d, o)
                            pal.append(c)
                    while len(px) < tw * th:
                        if sub == 128:
                            c, o = self.cpixel(d, o)
                            ln = 1
                        else:
                            idx = d[o]
                            o += 1
                            c = pal[idx & 127]
                            ln = 1
                            if not idx & 128:
                                px.append(c)
                                continue
                        while d[o] == 255:
                            ln += 255
                            o += 1
                        ln += d[o]
                        o += 1
                        px.extend([c] * ln)
                else:
                    raise RuntimeError(f"bad ZRLE subencoding {sub}")
                for j in range(th):
                    for i in range(tw):
                        self.put(tx + i, ty + j, px[j * tw + i])
        if o != len(d):
            raise RuntimeError(f"ZRLE: {len(d) - o} bytes left over")

    def close(self):
        self.s.close()


# --- the guest ---------------------------------------------------------------

def vm(args, *rest, timeout=180):
    return subprocess.run([sys.executable, VM, "--instance", str(args.instance),
                           "--disk", args.disk, *rest], cwd=REPO,
                          capture_output=True, text=True, timeout=timeout)


def dump(q, tmp):
    png = os.path.join(tmp, "dump.png")
    q.screenshot(png)
    from PIL import Image
    im = Image.open(png).convert("RGB")
    return im.size, im.tobytes()


def frame(r, v, what, incremental=False):
    """One update into v.fb. A frame that will not decode is a FAIL of
    `what` -- and the viewer is unusable after it, so None comes back."""
    try:
        v.request(incremental)
        return v.read_update()
    except (RuntimeError, IndexError, zlib.error, EOFError, OSError) as e:
        r.check(what, False, f"the update did not decode: {e!r}")
        return None


def same_fraction(a, b, quant=None):
    n = len(a) // 3
    same = 0
    for i in range(0, len(a), 3):
        pa, pb = a[i:i + 3], b[i:i + 3]
        if quant:
            pa = tuple(v >> s for v, s in zip(pa, quant))
            pb = tuple(v >> s for v, s in zip(pb, quant))
        same += pa == pb
    return same / n


def connect(args, password=PASSWORD, wait=60):
    """The server comes up when remoted next reads its config."""
    end = time.time() + wait
    last = None
    while time.time() < end:
        try:
            return Viewer(args.port, password)
        except PermissionError:
            raise   # an answer, not a server still starting
        except (OSError, EOFError, RuntimeError) as e:
            last = e
            time.sleep(1.5)
    raise RuntimeError(f"no VNC server on port {args.port}: {last}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    args = ap.parse_args()
    os.chdir(REPO)   # port_guard names the serial sockets relative to the repo
    args.port = 15900 + args.instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="vnc_test_")
    args.disk = copy_disk(os.path.join(REPO, "disk.img"), os.path.join(tmp, "disk.img"))

    print(f"vnc_test: booting slot {args.instance}, guest 5900 on host {args.port}")
    vm(args, "stop")
    boot = vm(args, "--hostfwd", f"tcp::{args.port}-:5900", "start", timeout=400)
    if "ready" not in boot.stdout:
        print(f"vnc_test: guest did not boot\n{boot.stdout}\n{boot.stderr}")
        return 1
    try:
        conf = os.path.join(tmp, "remote.conf")
        with open(conf, "w") as fh:
            fh.write(CONF)
        vm(args, "put", conf, "/etc/remote.conf")
        q = QMPSession(port=port_guard.instance_qmp(args.instance))
        dc = DebugConsole(port_guard.instance_sock(args.instance))

        # 1. the password
        try:
            connect(args, password="wrongpw!").close()
            r.check("a wrong password is refused", False, "it was let in")
        except PermissionError as e:
            r.check("a wrong password is refused", "failed" in str(e).lower(), str(e))
        v = connect(args)
        r.check("the right password is let in, as toy-os", v.name == "toy-os", v.name)

        # Park the pointer in the corner, so the one thing remoted draws
        # and QEMU's dump does not is out of the way.
        v.pointer(v.w - 1, v.h - 1)
        time.sleep(1.0)
        cur = dc.cursor()
        r.check("a PointerEvent moves the pointer", cur == (v.w - 1, v.h - 1), str(cur))
        v.pointer(v.w // 3, v.h // 4)
        time.sleep(1.0)
        cur = dc.cursor()
        r.check("...to exactly where it was sent", cur == (v.w // 3, v.h // 4), str(cur))
        v.pointer(v.w - 1, v.h - 1)
        time.sleep(1.0)

        # 2. Raw against the screendump
        v.set_encodings([0])
        frame(r, v, "a Raw frame is the screen")
        size, ref = dump(q, tmp)
        r.check("the server's size is the screen's", size == (v.w, v.h), f"{size} vs {v.w}x{v.h}")
        frac = same_fraction(bytes(v.fb), ref) if size == (v.w, v.h) else 0
        r.check("a Raw frame is the screen", frac >= 0.995, f"{frac:.4%} identical")
        raw = bytes(v.fb)

        # 3. ZRLE at 32 and 16 bits
        v.set_encodings([16, 0])
        if frame(r, v, "a ZRLE frame decodes to the Raw one") is not None:
            frac = same_fraction(bytes(v.fb), raw)
            r.check("a ZRLE frame decodes to the Raw one", frac >= 0.999, f"{frac:.4%}")
        v.close()

        v = connect(args)
        v.set_pixel_format(16)
        v.set_encodings([16])
        ok16 = frame(r, v, "ZRLE at 16 bits (RGB565) is the same frame") is not None
        # The viewer's buffer holds 5/6/5-bit values; compare with the
        # Raw frame cut to the same depth.
        cut = bytearray()
        for i in range(0, len(raw), 3):
            cut += bytes(((raw[i] * 31 + 127) // 255, (raw[i + 1] * 63 + 127) // 255,
                          (raw[i + 2] * 31 + 127) // 255))
        if ok16:
            frac = same_fraction(bytes(v.fb), bytes(cut))
            r.check("ZRLE at 16 bits (RGB565) is the same frame", frac >= 0.995, f"{frac:.4%}")
        v.close()

        # 4/5. input, and an incremental update
        # Raw from here on, so the input checks stand apart from ZRLE's.
        v = connect(args)
        v.set_encodings([0])
        frame(r, v, "a full frame before the input checks")
        v.tap(0xFFEB)                       # Super: the Start menu
        time.sleep(2.0)
        st = dc.state()
        r.check("Super from the viewer opens the Start menu",
                st["overlays"]["start_menu"], str(st["overlays"]))
        rects = frame(r, v, "the incremental update is only what changed", True) or []
        area = sum(w * h for _, _, w, h, _ in rects)
        r.check("the incremental update is only what changed", 0 < area < v.w * v.h,
                f"{area} of {v.w * v.h} pixels in {len(rects)} rectangles")
        size, ref = dump(q, tmp)
        frac = same_fraction(bytes(v.fb), ref)
        r.check("...and the patched frame is the screen again", frac >= 0.995, f"{frac:.4%}")
        v.tap(0xFF1B)                       # Esc closes the menu
        time.sleep(1.0)

        # The Cursor pseudo-encoding: the shape arrives, and a move over
        # plain desktop then costs no pixels at all.
        v.close()
        v = connect(args)
        v.set_encodings([0, -239])
        frame(r, v, "the pointer's shape arrives as a Cursor rectangle")
        cur = v.cursor
        ok = cur is not None and 0 < cur[2] <= 64 and 0 < cur[3] <= 64 and any(cur[5])
        r.check("the pointer's shape arrives as a Cursor rectangle", ok, str(cur and cur[:4]))
        v.pointer(v.w // 2, v.h // 2)
        time.sleep(0.3)
        v.pointer(v.w // 2 + 40, v.h // 2 + 30)
        v.request(True)
        v.s.settimeout(1.5)
        # NEAR THE POINTER, not anywhere: the taskbar clock may tick in
        # the same second and is a real change (it failed once on that).
        px0, py0, px1, py1 = v.w // 2 - 64, v.h // 2 - 64, v.w // 2 + 40 + 64, v.h // 2 + 30 + 64
        try:
            rects = v.read_update()
            moved = [rc for rc in rects if rc[4] != -239 and rc[0] < px1 and rc[0] + rc[2] > px0
                     and rc[1] < py1 and rc[1] + rc[3] > py0]
            r.check("with it, a pointer move sends no pixels", not moved, str(rects))
        except socket.timeout:
            r.check("with it, a pointer move sends no pixels", True)
        v.s.settimeout(30)

        # Ctrl+Alt+T opens a Terminal; type into it.
        for k in (0xFFE3, 0xFFE9):
            v.key(k, True)
        v.tap(ord("t"))
        for k in (0xFFE9, 0xFFE3):
            v.key(k, False)
        time.sleep(6.0)
        line = "echo VNC_typed-42!"
        for ch in line:
            v.tap(ord(ch))
        v.tap(0xFF0D)
        time.sleep(3.0)
        dc.close()   # vm.py exec needs the console's socket
        hist = vm(args, "exec", "cat /etc/tosh_history").stdout
        r.check("typed text reaches the shell, Shift characters and all",
                line in hist, hist[-300:])
        v.close()
        q.close()
    finally:
        if not args.keep:
            vm(args, "stop")

    print(f"vnc_test: {len(r.passes)} passed, {len(r.fails)} failed")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
