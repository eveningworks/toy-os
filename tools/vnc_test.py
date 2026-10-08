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
import hashlib
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
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


def vencrypt(conn, security, password, ca=None):
    """VeNCrypt 0.2 after security type 19 was picked: the subtype
    ("tls-vnc" = X509Vnc, "tls-plain" = X509Plain), the TLS handshake
    -- verifying against `ca` (a PEM, checked for 127.0.0.1) or taking
    any certificate -- and, for X509Plain, the credentials. `conn` has
    .s and .recv(); .s becomes the TLS socket, and .cert_der and
    .tls_version are set. X509Vnc's DES challenge is the caller's."""
    if conn.recv(2) != b"\x00\x02":
        raise RuntimeError("not VeNCrypt 0.2")
    conn.s.sendall(b"\x00\x02")
    if conn.recv(1) != b"\x00":
        raise RuntimeError("VeNCrypt version refused")
    n = conn.recv(1)[0]
    conn.subtypes = list(struct.unpack(f">{n}I", conn.recv(4 * n)))
    sub = 261 if security == "tls-vnc" else 262
    conn.s.sendall(struct.pack(">I", sub))
    if conn.recv(1) != b"\x01":
        raise RuntimeError(f"subtype {sub} refused (offered {conn.subtypes})")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    if ca:
        ctx.load_verify_locations(cadata=ca)   # check_hostname stays on
    else:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    conn.s = ctx.wrap_socket(conn.s, server_hostname="127.0.0.1")
    conn.cert_der = conn.s.getpeercert(binary_form=True)
    conn.tls_version = conn.s.version()
    if sub == 262:
        u, p = b"toy", password.encode()
        conn.s.sendall(struct.pack(">II", len(u), len(p)) + u + p)


class Viewer:
    def __init__(self, port, password, timeout=30, security="vnc", ca=None):
        """`security`: "vnc" (type 2), or VeNCrypt's "tls-vnc" (X509Vnc)
        or "tls-plain" (X509Plain). `ca`: a PEM to verify the server's
        certificate against, for 127.0.0.1, as a careful viewer does;
        without one the certificate is taken and kept in self.cert_der."""
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.z = zlib.decompressobj()
        self.bpp = 32
        self.cert_der = None
        try:
            self.login(password, security, ca)
        except BaseException:
            self.s.close()   # a refused attempt must not hold a server slot
            raise

    def login(self, password, security, ca):
        if self.recv(12) != b"RFB 003.008\n":
            raise RuntimeError("no RFB 3.8 banner")
        self.s.sendall(b"RFB 003.008\n")
        n = self.recv(1)[0]
        if n == 0:
            raise PermissionError(self.recv(struct.unpack(">I", self.recv(4))[0]).decode())
        self.types = list(self.recv(n))
        want = 2 if security == "vnc" else 19
        if want not in self.types:
            raise RuntimeError(f"security type {want} not offered: {self.types}")
        self.s.sendall(bytes([want]))
        if want == 19:
            vencrypt(self, security, password, ca)
        if security != "tls-plain":
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


def connect(args, password=PASSWORD, wait=60, security="vnc"):
    """The server comes up when remoted next reads its config."""
    end = time.time() + wait
    last = None
    while time.time() < end:
        try:
            return Viewer(args.port, password, security=security)
        except PermissionError:
            raise   # an answer, not a server still starting
        except (OSError, EOFError, RuntimeError) as e:
            last = e
            time.sleep(1.5)
    raise RuntimeError(f"no VNC server on port {args.port}: {last}")


# --- stage 2: asking at the screen, the tray, the settings page -------------

ASK_CONF = f"""when = ask
from = anywhere
[vnc]
enabled = yes
port = 5900
password = {PASSWORD}
"""


def put_conf(args, path, text):
    with open(path, "w") as fh:
        fh.write(text)
    vm(args, "put", path, "/etc/remote.conf")


def connect_async(args):
    """Connects in a thread -- the handshake waits for the answer at the
    screen -- and returns a dict the thread fills: viewer or error."""
    out = {}

    def run():
        try:
            out["v"] = Viewer(args.port, PASSWORD, timeout=60)
        except PermissionError as e:
            out["refused"] = str(e)
        except (OSError, EOFError, RuntimeError) as e:
            out["error"] = repr(e)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    out["thread"] = t
    return out


def wait_notice(dc, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        n = dc.state().get("notice")
        if n and n.get("title") == "Remote desktop request":
            return n
        time.sleep(0.5)
    return None


def press(dc, notice, label):
    b = [b for b in notice["buttons"] if b["label"] == label]
    if not b:
        return False
    dc.click(b[0]["x"] + b[0]["w"] // 2, b[0]["y"] + b[0]["h"] // 2)
    return True


def stage2(args, r, tmp, conf):
    put_conf(args, conf, ASK_CONF)
    time.sleep(3)
    dc = DebugConsole(port_guard.instance_sock(args.instance))

    # Allow: the card names the asker, and the answer lets it in.
    c = connect_async(args)
    n = wait_notice(dc)
    r.check("an unknown viewer is asked about at the screen", n is not None, str(n))
    r.check("...the card says who is asking", n is not None and "10.0.2.2" in n.get("sub", ""),
            str(n and n.get("sub")))
    if n:
        press(dc, n, "Allow")
    c["thread"].join(30)
    v = c.get("v")
    r.check("Allow lets the viewer in", v is not None, str(c))

    # The kernel lists the session, the tray lights, and the flyout's
    # View only takes the keyboard and mouse away.
    # The flyout reads the kernel's list once a second.
    time.sleep(2.5)
    rj = dc.json("gui remote --json") if v else {}
    r.check("the session is listed live, and the tray icon is lit",
            bool(rj.get("live")) and rj.get("accent") is True, str(rj)[:300])
    if v and rj.get("live"):
        dc.click(rj["tray"]["cx"], rj["tray"]["cy"])
        time.sleep(1)
        rj = dc.json("gui remote --json")
        view = rj["live"][0].get("view")
        if view:
            dc.click(view["cx"], view["cy"])
            time.sleep(1.5)
        before = dc.cursor()
        v.pointer(before[0] // 2 + 7, before[1] // 2 + 5)
        time.sleep(1.5)
        r.check("View only from the tray stops the viewer's mouse", dc.cursor() == before,
                f"{before} -> {dc.cursor()}")
        rj = dc.json("gui remote --json")
        if not rj.get("open"):
            dc.click(rj["tray"]["cx"], rj["tray"]["cy"])
            time.sleep(1)
            rj = dc.json("gui remote --json")
        disc = rj["live"][0].get("disconnect") if rj.get("live") else None
        if disc:
            dc.click(disc["cx"], disc["cy"])
            time.sleep(2.5)
        try:
            v.s.settimeout(5)
            gone = v.s.recv(1) == b""
        except OSError:
            gone = True
        rj = dc.json("gui remote --json")
        r.check("Disconnect in the tray ends the session", gone and not rj.get("live"), str(rj)[:300])
    if v:
        v.close()

    # Deny: refused in RFB's own words.
    c = connect_async(args)
    n = wait_notice(dc)
    if n:
        press(dc, n, "Deny")
    c["thread"].join(30)
    r.check("Deny refuses the viewer, and says why", "did not allow" in c.get("refused", ""), str(c))

    # "Ask every time" offers no "Always allow" -- it would not be kept.
    c = connect_async(args)
    n = wait_notice(dc)
    r.check("\"Ask every time\" offers no Always allow",
            n is not None and "Always allow" not in [b["label"] for b in n["buttons"]], str(n))
    if n:
        press(dc, n, "Deny")
    c["thread"].join(30)

    # Always allow + View only, under "Ask unless trusted": in,
    # read-only, and saved for next time.
    dc.close()
    put_conf(args, conf, ASK_CONF.replace("when = ask", "when = ask-unless-trusted"))
    dc = DebugConsole(port_guard.instance_sock(args.instance))
    time.sleep(3)
    c = connect_async(args)
    n = wait_notice(dc)
    if n:
        press(dc, n, "Always allow")
        time.sleep(0.5)
        n = wait_notice(dc)
        press(dc, n, "View only")
    c["thread"].join(30)
    v = c.get("v")
    r.check("Always allow with View only lets the viewer in", v is not None, str(c))
    if v:
        before = dc.cursor()
        v.pointer(before[0] // 3 + 11, before[1] // 3 + 13)
        time.sleep(1.5)
        r.check("...and its mouse does nothing", dc.cursor() == before, f"{before} -> {dc.cursor()}")
        v.close()
    dc.close()
    saved = vm(args, "exec", "cat /etc/remote.conf").stdout
    dc = DebugConsole(port_guard.instance_sock(args.instance))
    r.check("...and the address is trusted from now on", "10.0.2.2" in saved, saved[-300:])
    time.sleep(3)
    # Getting in AT ALL within the timeout is the check: an untrusted
    # viewer would sit on the card's thirty seconds first.
    try:
        v = Viewer(args.port, PASSWORD, timeout=20)
        r.check("a trusted viewer gets in without being asked", True)
        v.close()
    except (OSError, EOFError, RuntimeError, PermissionError) as e:
        r.check("a trusted viewer gets in without being asked", False, repr(e))

    # The settings page: its VNC switch writes /etc/remote.conf.
    dc.send("gui spawn /bin/wm/system/settings remotedesktop")
    w = {}
    end = time.time() + 30
    while time.time() < end and "rd_vnc" not in w:
        time.sleep(1)
        w = dc.widgets("System Settings")
    sw = w.get("rd_vnc")
    r.check("the Remote Desktop page opens on its own name", sw is not None, str(list(w)[:20]))
    if sw:
        dc.click(sw["screen"]["x"] + sw["w"] // 2, sw["screen"]["y"] + sw["h"] // 2)
        time.sleep(2)
    dc.close()
    after = vm(args, "exec", "cat /etc/remote.conf").stdout
    r.check("its VNC switch turns the server off in /etc/remote.conf",
            "enabled = no" in after or "enabled=no" in after, after[-300:])


# --- stage 3: VeNCrypt --------------------------------------------------------

def stage3(args, r, q, tmp, conf):
    put_conf(args, conf, CONF)   # stage 2 left the server switched off
    try:
        v = connect(args, security="tls-vnc")
    except (PermissionError, RuntimeError) as e:
        r.check("VeNCrypt (X509Vnc) lets a viewer in over TLS", False, repr(e))
        return
    r.check("VeNCrypt (X509Vnc) lets a viewer in over TLS", v.name == "toy-os",
            f"{v.name} {v.tls_version}")
    r.check("...plain VNC is listed first, for viewers with no CA to give",
            v.types == [2, 19], str(v.types))
    fp = ":".join(f"{b:02X}" for b in hashlib.sha256(v.cert_der).digest())
    saved = vm(args, "exec", "cat /etc/remote.conf").stdout
    r.check("the certificate is the one whose fingerprint Settings shows", fp in saved,
            f"{fp}\n{saved[-300:]}")
    v.pointer(v.w - 1, v.h - 1)
    time.sleep(1.0)
    v.set_encodings([0])
    if frame(r, v, "a Raw frame over TLS is the screen") is not None:
        size, ref = dump(q, tmp)
        frac = same_fraction(bytes(v.fb), ref) if size == (v.w, v.h) else 0
        r.check("a Raw frame over TLS is the screen", frac >= 0.995, f"{frac:.4%} identical")
    v.close()

    # A viewer that VERIFIES: the certificate as its trust anchor, and
    # the address it dialled checked against the names in it.
    pem = ssl.DER_cert_to_PEM_cert(v.cert_der)
    try:
        Viewer(args.port, PASSWORD, security="tls-vnc", ca=pem).close()
        r.check("the certificate names the address a viewer dialled", True)
    except (ssl.SSLError, OSError, RuntimeError) as e:
        r.check("the certificate names the address a viewer dialled", False, repr(e))
    try:
        Viewer(args.port, "wrongpw!", security="tls-plain").close()
        r.check("X509Plain refuses a wrong password", False, "it was let in")
    except PermissionError:
        r.check("X509Plain refuses a wrong password", True)
    except (OSError, EOFError, RuntimeError) as e:
        r.check("X509Plain refuses a wrong password", False, repr(e))
    try:
        Viewer(args.port, PASSWORD, security="tls-plain").close()
        r.check("...and lets the right one in", True)
    except (PermissionError, OSError, EOFError, RuntimeError) as e:
        r.check("...and lets the right one in", False, repr(e))

    # Required: VeNCrypt alone is offered, and RFB 3.3 -- which cannot
    # negotiate -- is told why it is refused.
    put_conf(args, conf, CONF + "encryption = require\n")
    end = time.time() + 30
    types = None
    while time.time() < end and types != [19]:   # until remoted rereads the file
        time.sleep(2)
        try:
            Viewer(args.port, PASSWORD).close()
            types = "plain VNC still let in"
        except RuntimeError as e:
            types = [19] if str(e).endswith("[19]") else str(e)
        except (PermissionError, OSError, EOFError) as e:
            types = repr(e)
    r.check("\"Required\" offers VeNCrypt alone", types == [19], str(types))
    try:
        Viewer(args.port, PASSWORD, security="tls-vnc").close()
        r.check("...which still lets a viewer in", True)
    except (PermissionError, OSError, EOFError, RuntimeError) as e:
        r.check("...which still lets a viewer in", False, repr(e))
    s = socket.create_connection(("127.0.0.1", args.port), timeout=20)
    s.recv(12)
    s.sendall(b"RFB 003.003\n")
    got = b""
    while True:
        c = s.recv(4096)
        if not c:
            break
        got += c
    s.close()
    r.check("...and an RFB 3.3 viewer is told why it is refused",
            got[:4] == b"\0\0\0\0" and b"ncrypt" in got, repr(got[:120]))

    # Viewers that connect and say nothing hold the sessions -- and are
    # dropped at the handshake's deadline rather than locking everyone out.
    silent = [socket.create_connection(("127.0.0.1", args.port), timeout=60) for _ in range(2)]
    time.sleep(3)
    try:
        Viewer(args.port, PASSWORD, security="tls-vnc").close()
        held = False
    except (PermissionError, OSError, EOFError, RuntimeError):
        held = True
    r.check("two silent viewers hold both sessions", held)
    t0 = time.time()
    for c in silent:
        try:
            while c.recv(4096):   # the banner, then the close
                pass
        except OSError:
            pass
        c.close()
    waited = time.time() - t0
    try:
        Viewer(args.port, PASSWORD, security="tls-vnc").close()
        r.check("...until the handshake's deadline drops them", waited < 45, f"{waited:.0f} s")
    except (PermissionError, OSError, EOFError, RuntimeError) as e:
        r.check("...until the handshake's deadline drops them", False, f"{waited:.0f} s, {e!r}")


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
    # --cpu max: RDRAND, without which remoted refuses to make a TLS key.
    boot = vm(args, "--cpu", "max", "--hostfwd", f"tcp::{args.port}-:5900", "start", timeout=400)
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

        # A SESSION THAT DIES WITH A NUDGE PENDING FREES ITS SLOT. Each of
        # these takes a frame with the pointer drawn in, moves the pointer
        # (damage, so the compositor nudges it) and hangs up before
        # asking again. The compositor has four screen-sharing slots; the
        # sixth viewer got no picture at all until a dead one's slot was
        # freed when it went (the ASUS, after a day of connections).
        v.close()
        got = True
        for i in range(6):
            d = connect(args)
            d.set_encodings([0] if i < 5 else [16, -239])
            d.s.settimeout(10)
            try:
                d.request(False)
                d.read_update()
            except (socket.timeout, OSError, EOFError, RuntimeError):
                got = False   # no picture: every slot is held by the dead
            if i < 5:
                d.pointer(100 + 40 * i, 100)
                time.sleep(0.5)
            d.close()
            time.sleep(0.5)
            if not got:
                break
        r.check("a viewer after five that hung up mid-nudge still gets the picture", got,
                f"no picture for viewer {i + 1}")
        v = connect(args)
        v.set_encodings([16, -239])

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
        # A POINTER-SHAPE CHANGE MUST NOT SILENCE WHAT FOLLOWS. It froze
        # once (one "nudged" flag for both kinds, cleared only by a pixel
        # fetch) -- and only on a QUIET screen: a blinking caret's
        # repaints fetched pixels and cleared it. So the screen is let
        # go still first (the caret stops ten seconds after the last
        # key), then the pointer turns into an I-beam over the Terminal
        # (a cursor-only update), then the Start menu must arrive.
        tw = [w for w in dc.windows() if w["title"].startswith("Terminal")]
        if tw:
            c = tw[0]["content"]
            v.pointer(5, 5)            # the desktop: an arrow
            time.sleep(11)
            v.s.settimeout(1.5)
            for _ in range(20):        # drain until the screen is still
                try:
                    v.request(True)
                    v.read_update()
                except (socket.timeout, OSError, EOFError, RuntimeError):
                    break
            v.s.settimeout(5)
            v.pointer(c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
            time.sleep(1.0)
            try:
                shape = v.read_update()   # answers the request the drain left open
            except (socket.timeout, OSError, EOFError, RuntimeError):
                shape = []
            v.tap(0xFFEB)              # Super: the Start menu, real pixels
            time.sleep(1.5)
            try:
                v.request(True)
                rects = v.read_update()
                pix = [rc for rc in rects if rc[4] != -239]
            except (socket.timeout, OSError, EOFError, RuntimeError) as e:
                pix = []
                rects = repr(e)
            v.s.settimeout(30)
            r.check("after the pointer changes shape, a screen change still arrives", bool(pix),
                    f"shape update {shape}, then {rects}")
            v.tap(0xFF1B)
            time.sleep(1.0)
        dc.close()   # vm.py exec needs the console's socket
        hist = vm(args, "exec", "cat /etc/tosh_history").stdout
        r.check("typed text reaches the shell, Shift characters and all",
                line in hist, hist[-300:])
        v.close()
        stage2(args, r, tmp, conf)
        stage3(args, r, q, tmp, conf)
        q.close()
    finally:
        if not args.keep:
            vm(args, "stop")

    print(f"vnc_test: {len(r.passes)} passed, {len(r.fails)} failed")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
