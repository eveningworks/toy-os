#!/usr/bin/env python3
"""Drive a toy-os machine over the network: run commands, move files.

WHY THIS EXISTS
---------------
`tools/vm.py` drives a QEMU guest through its serial debug console. It
cannot reach the BARE-METAL laptop, which has no serial console attached
and is where half the open bugs in `docs/bugs.md` live -- a USB mouse
that will not bind, a power button that needs two presses, a garbled
product string. Those need a session to be able to type at that machine
and to put a rebuilt binary on it.

That is what `/bin/telnetd` and `/bin/tftpd` are for, and this is their
host side. It is deliberately ONE tool rather than "use telnet, then use
curl": the two halves are always used together, the telnet negotiation
has to be answered before a command can be sent, and getting either
wrong looks like the guest being broken.

    python3 tools/remote.py --host 192.168.200.104 exec "lsusb" "dmesg"
    python3 tools/remote.py --host 192.168.200.104 put build/userland/bin/ls /bin/ls
    python3 tools/remote.py --host 192.168.200.104 get /tmp/crash.log ./crash.log
    python3 tools/remote.py --host 192.168.200.104 shell     # interactive

WHAT IT IS NOT
--------------
Not a general telnet client. It answers negotiation the minimum amount
needed to get a usable session and strips the protocol back out; it does
not implement terminal emulation, so a full-screen program (`edit`,
`less`) is not usable through `exec`. Use `shell` for those, or better,
do not -- the point of this is text you can assert on.

Not authenticated or encrypted, because neither end can be: see
`docs/commands/telnetd.md`. This connects to whatever answers on the
port and trusts it.

THE PROMPT IS THE FRAME, and that is the load-bearing decision here.
There is no request/response framing in a shell session, so `exec` finds
the end of a command's output by waiting for the next prompt. A command
that changes the prompt, or one whose own output ends in something that
looks like one, will confuse it -- which is why `exec` sends a marker
`echo` after each command and reads up to THAT instead. A marker the
guest echoes back is a frame the guest cannot accidentally produce.
"""
import argparse
import os
import re
import socket
import sys
import time

# RFC 854.
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA, OPT_NAWS = 1, 3, 31

DEFAULT_TELNET_PORT = 23
DEFAULT_TFTP_PORT = 69

# TFTP, RFC 1350.
OP_RRQ, OP_WRQ, OP_DATA, OP_ACK, OP_ERROR = 1, 2, 3, 4, 5


class Telnet:
    """Just enough telnet to hold a shell session."""

    def __init__(self, host, port=DEFAULT_TELNET_PORT, timeout=10.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self._st = 0
        self._cmd = 0

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _answer(self, cmd, opt):
        # Refuse everything. This end needs no options at all: it is not
        # a terminal, so ECHO and SGA are the server's business and NAWS
        # would be a lie about a window that does not exist.
        if cmd == DO:
            self.s.sendall(bytes([IAC, WONT, opt]))
        elif cmd == WILL:
            self.s.sendall(bytes([IAC, DONT, opt]))

    def _strip(self, data):
        """Pull the protocol out, returning only the shell's bytes."""
        out = bytearray()
        for b in data:
            if self._st == 0:
                if b == IAC:
                    self._st = 1
                else:
                    out.append(b)
            elif self._st == 1:
                if b == IAC:
                    out.append(IAC)
                    self._st = 0
                elif b == SB:
                    self._st = 3
                elif b in (DO, DONT, WILL, WONT):
                    self._cmd = b
                    self._st = 2
                else:
                    self._st = 0
            elif self._st == 2:
                self._answer(self._cmd, b)
                self._st = 0
            elif self._st == 3:
                if b == IAC:
                    self._st = 4
            elif self._st == 4:
                self._st = 0 if b == SE else 3
        return bytes(out)

    def read_until_line(self, marker, timeout=15.0):
        """Read until a RENDERED LINE is exactly `marker`.

        A rendered line, not a substring, and that is the whole trick.
        The shell echoes what it is sent, so the marker's own command
        line comes back first -- matching the raw bytes would stop at
        that echo, before the command it is supposed to be framing has
        run. Rendered, the echo is the line `/$ echo __done0__` and the
        output is the line `__done0__`; only one of those can be equal.

        Raises on timeout rather than returning what it has: a partial
        answer that looks like a whole one is the failure mode this
        whole tool exists to avoid.
        """
        end = time.time() + timeout
        while True:
            lines = render(self.buf.decode("utf-8", "replace"))
            if marker in lines:
                return lines[:lines.index(marker)]
            self.s.settimeout(max(0.1, end - time.time()))
            try:
                chunk = self.s.recv(4096)
            except socket.timeout:
                raise TimeoutError(
                    f"no {marker!r} line within {timeout}s; got: "
                    f"{self.buf[-400:]!r}")
            if not chunk:
                raise EOFError(f"connection closed; got: {self.buf[-400:]!r}")
            self.buf += self._strip(chunk)

    def send_line(self, line):
        self.s.sendall(line.encode() + b"\r\n")


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")


def render(text):
    """Replay the byte stream the way a terminal would, into lines.

    CR IS NOT A LINE ENDING HERE, IT IS A SEEK, and treating it as one
    is what made the first version of this unreadable. The shared line
    editor (kernel/lib/klineedit.c) repaints the WHOLE line from column
    0 on every keystroke -- `vga_cursor_move()` is a non-destructive
    seek ring 3 cannot reach, so `\r` plus a rewrite is how it moves the
    cursor. Stripping the CRs concatenates forty partial repaints into
    one line of garbage; honouring them collapses the repaints onto each
    other and leaves the final state, which is what was on the screen.
    """
    lines, cur, pos = [], "", 0
    for ch in ANSI_RE.sub("", text):
        if ch == "\n":
            lines.append(cur)
            cur, pos = "", 0
        elif ch == "\r":
            pos = 0
        elif ch == "\b":
            pos = max(0, pos - 1)
        elif ch == "\x07":
            continue
        else:
            cur = cur[:pos] + ch + cur[pos + 1:]
            pos += 1
    if cur:
        lines.append(cur)
    return lines


def do_exec(host, port, commands, timeout):
    t = Telnet(host, port, timeout)
    rc = 0
    try:
        # Settle: wait for the shell to say something. A prompt is
        # whatever it is -- we never match on it, only on our markers.
        time.sleep(0.4)
        try:
            t.s.settimeout(2.0)
            t.buf += t._strip(t.s.recv(4096))
        except (socket.timeout, OSError):
            pass

        for i, cmd in enumerate(commands):
            marker = f"__done{i}__"
            t.buf = b""
            t.send_line(cmd)
            # The marker echo is the frame. `echo` is a /bin program, so
            # this also proves the shell is still running commands rather
            # than sitting in one that never returned.
            t.send_line(f"echo {marker}")
            lines = t.read_until_line(marker, timeout)
            # Drop what the shell echoed back at us -- the two command
            # lines we typed, each with a prompt in front of it -- and
            # any blank lines those left at the ends.
            lines = [ln for ln in lines
                     if not ln.endswith(cmd) and not ln.endswith(f"echo {marker}")]
            while lines and not lines[0].strip():
                lines.pop(0)
            while lines and not lines[-1].strip():
                lines.pop()
            if len(commands) > 1:
                print(f"--- {cmd} ---")
            print("\n".join(lines))
    except (TimeoutError, EOFError) as e:
        print(f"remote: {e}", file=sys.stderr)
        rc = 1
    finally:
        t.close()
    return rc


# --- TFTP ---------------------------------------------------------------
#
# OPTIONS ARE NEGOTIATED (RFC 2347), and the two that matter are
# `blksize` (RFC 2348) and `windowsize` (RFC 7440). 512-byte lock-step
# is not slow for the reason it looks slow: measured against the
# bare-metal laptop, a block cost 32 ms of which ~1.8 ms was the
# network. The rest was a 10 ms scheduler tick per round trip -- a
# blocked process runs at the next tick -- plus the server's write.
#
# 1428 IS THE CEILING, and it is the guest's, not a convention:
# kernel/net/ipv4.c does not fragment or reassemble, so a block that
# does not fit the MTU is DROPPED rather than split. A server that does
# not answer with an OACK gets the RFC 1350 defaults and everything
# still works -- which is what makes this safe against a stock tftpd.

OP_OACK = 6

BLKSIZE = 512            # RFC 1350's default, and the fallback
WANT_BLKSIZE = 1428      # see above
WANT_WINDOW = 3          # the guest's socket holds 3 datagrams -- see
                         # userland/bin/tftpd.c's WINDOW_MAX. Asking for
                         # more is answered with 3 anyway (the OACK says
                         # what was agreed), so this is belt and braces.


def _tftp_socket(timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    return s


def _tftp_error(pkt):
    code = int.from_bytes(pkt[2:4], "big")
    msg = pkt[4:].split(b"\0")[0].decode("utf-8", "replace")
    return f"tftp error {code}: {msg}"


def _request(op, remote, opts):
    r = bytes([0, op]) + remote.encode() + b"\0octet\0"
    for k, v in opts.items():
        r += k.encode() + b"\0" + str(v).encode() + b"\0"
    return r


def _parse_oack(pkt):
    """The options the SERVER agreed to -- which may be fewer, or
    smaller, than were asked for. Anything it did not name is not in
    force, so the default stands (RFC 2347)."""
    got = {}
    fields = pkt[2:].split(b"\0")
    for i in range(0, len(fields) - 1, 2):
        k = fields[i].decode("utf-8", "replace").lower()
        v = fields[i + 1].decode("utf-8", "replace")
        if k:
            got[k] = v
    return got


def _negotiate(s, host, port, req, timeout):
    """Sends the request and reads the first reply. Returns
    (peer, blksize, window, first_pkt) -- first_pkt is None when the
    server OACKed, and the reply itself when it did not (a server with
    no option support answers the request directly, and that packet is
    data we must not drop)."""
    blksize, window = BLKSIZE, 1
    for _ in range(5):
        s.sendto(req, (host, port))
        try:
            pkt, addr = s.recvfrom(65536)
        except socket.timeout:
            continue
        if pkt[1] == OP_ERROR:
            raise RuntimeError(_tftp_error(pkt))
        if pkt[1] == OP_OACK:
            got = _parse_oack(pkt)
            if "blksize" in got:
                blksize = int(got["blksize"])
            if "windowsize" in got:
                window = int(got["windowsize"])
            return addr, blksize, window, None
        # No OACK: an option-unaware server. Its reply is already the
        # first ACK or the first data block.
        return addr, BLKSIZE, 1, pkt
    raise RuntimeError("no reply to the request")


def do_put(host, port, local, remote, timeout):
    with open(local, "rb") as f:
        data = f.read()
    s = _tftp_socket(timeout)
    req = _request(OP_WRQ, remote, {"blksize": WANT_BLKSIZE,
                                    "windowsize": WANT_WINDOW})
    try:
        peer, blksize, window, first = _negotiate(s, host, port, req, timeout)
        if first is not None and not (first[1] == OP_ACK
                                      and int.from_bytes(first[2:4], "big") == 0):
            raise RuntimeError("server did not acknowledge the write request")

        total = (len(data) + blksize - 1) // blksize
        if len(data) % blksize == 0:
            total += 1        # a final short (empty) block ends it
        acked = 0             # blocks the server has confirmed
        tries = 0
        sent_bytes = 0

        while acked < total:
            # Send one window without waiting -- this is the whole point
            # of RFC 7440. A 16-deep window is 16 blocks per round trip
            # rather than one.
            n = min(window, total - acked)
            for i in range(n):
                b = acked + 1 + i
                payload = data[(b - 1) * blksize: b * blksize]
                s.sendto(bytes([0, OP_DATA]) + (b & 0xFFFF).to_bytes(2, "big")
                         + payload, peer)
            try:
                pkt, _ = s.recvfrom(65536)
            except socket.timeout:
                tries += 1
                if tries > 5:
                    raise RuntimeError(f"no ACK after block {acked}")
                continue
            if pkt[1] == OP_ERROR:
                raise RuntimeError(_tftp_error(pkt))
            if pkt[1] != OP_ACK:
                continue
            a = int.from_bytes(pkt[2:4], "big")
            # A PARTIAL ACK IS THE RECOVERY PATH, not an error: it says
            # how far the server got, and the next window starts there.
            want = (acked + n) & 0xFFFF
            if a == want:
                acked += n
            elif ((a - acked) & 0xFFFF) <= n:
                acked += (a - acked) & 0xFFFF
            else:
                continue      # a stale ACK from before this window
            tries = 0
            sent_bytes = min(acked * blksize, len(data))
    finally:
        s.close()
    print(f"remote: put {local} -> {remote}, {sent_bytes} bytes")
    return 0


def do_get(host, port, remote, local, timeout):
    s = _tftp_socket(timeout)
    req = _request(OP_RRQ, remote, {"blksize": WANT_BLKSIZE,
                                    "windowsize": WANT_WINDOW,
                                    "tsize": 0})
    out = bytearray()
    try:
        peer, blksize, window, first = _negotiate(s, host, port, req, timeout)
        if first is None:
            # An OACK is acknowledged with ACK 0 before any data flows.
            s.sendto(bytes([0, OP_ACK, 0, 0]), peer)
            pending = None
        else:
            pending = first

        expect = 1
        acked = 0
        tries = 0
        while True:
            if pending is not None:
                pkt, pending = pending, None
            else:
                try:
                    pkt, _ = s.recvfrom(65536)
                except socket.timeout:
                    tries += 1
                    if tries > 5:
                        raise RuntimeError(f"no block {expect}")
                    # Tell the server where we got to, so it resumes
                    # there rather than waiting on an ACK we never sent.
                    s.sendto(bytes([0, OP_ACK]) + (acked & 0xFFFF).to_bytes(2, "big"),
                             peer)
                    continue
            tries = 0
            if pkt[1] == OP_ERROR:
                raise RuntimeError(_tftp_error(pkt))
            if pkt[1] != OP_DATA:
                continue
            b = int.from_bytes(pkt[2:4], "big")
            if b != (expect & 0xFFFF):
                continue      # a gap or a duplicate: do not advance
            payload = pkt[4:]
            out += payload
            expect += 1
            final = len(payload) < blksize
            if final or (expect - 1 - acked) >= window:
                acked = expect - 1
                s.sendto(bytes([0, OP_ACK]) + (acked & 0xFFFF).to_bytes(2, "big"),
                         peer)
            if final:
                break
    finally:
        s.close()
    with open(local, "wb") as f:
        f.write(out)
    print(f"remote: get {remote} -> {local}, {len(out)} bytes")
    return 0



def do_shell(host, port, timeout):
    """A raw interactive session, for a human. Ctrl-] quits."""
    import termios
    import tty
    import select

    t = Telnet(host, port, timeout)
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    print("remote: connected; Ctrl-] to quit")
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = select.select([fd, t.s], [], [], 0.2)
            if fd in r:
                b = os.read(fd, 1024)
                if b"\x1d" in b:            # Ctrl-]
                    break
                t.s.sendall(b.replace(b"\n", b"\r\n"))
            if t.s in r:
                chunk = t.s.recv(4096)
                if not chunk:
                    break
                os.write(1, t._strip(chunk))
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        t.close()
        print("\nremote: closed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True, help="the toy-os machine")
    ap.add_argument("--telnet-port", type=int, default=DEFAULT_TELNET_PORT)
    ap.add_argument("--tftp-port", type=int, default=DEFAULT_TFTP_PORT)
    ap.add_argument("--timeout", type=float, default=15.0)
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("exec", help="run commands and print their output")
    e.add_argument("commands", nargs="+")

    p = sub.add_parser("put", help="copy a local file onto the machine")
    p.add_argument("local")
    p.add_argument("remote")

    g = sub.add_parser("get", help="copy a file off the machine")
    g.add_argument("remote")
    g.add_argument("local")

    sub.add_parser("shell", help="an interactive session (Ctrl-] quits)")

    a = ap.parse_args()
    try:
        if a.cmd == "exec":
            return do_exec(a.host, a.telnet_port, a.commands, a.timeout)
        if a.cmd == "put":
            return do_put(a.host, a.tftp_port, a.local, a.remote, a.timeout)
        if a.cmd == "get":
            return do_get(a.host, a.tftp_port, a.remote, a.local, a.timeout)
        if a.cmd == "shell":
            return do_shell(a.host, a.telnet_port, a.timeout)
    except (OSError, RuntimeError) as ex:
        print(f"remote: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
