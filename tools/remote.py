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
BLKSIZE = 512


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

def _tftp_socket(timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    return s


def _tftp_error(pkt):
    code = int.from_bytes(pkt[2:4], "big")
    msg = pkt[4:].split(b"\0")[0].decode("utf-8", "replace")
    return f"tftp error {code}: {msg}"


def do_put(host, port, local, remote, timeout):
    with open(local, "rb") as f:
        data = f.read()
    s = _tftp_socket(timeout)
    req = bytes([0, OP_WRQ]) + remote.encode() + b"\0octet\0"
    s.sendto(req, (host, port))

    block = 0
    sent = 0
    peer = None
    # The packet a timeout resends. It is the REQUEST until the server's
    # first ACK names its TID, and the last data block after that -- kept
    # in one name so the retry path has nothing to decide.
    chunk_pkt = None
    try:
        while True:
            for _ in range(5):
                try:
                    pkt, addr = s.recvfrom(1024)
                except socket.timeout:
                    if peer and chunk_pkt:
                        s.sendto(chunk_pkt, peer)
                    else:
                        s.sendto(req, (host, port))
                    continue
                # The server answers from a NEW port (its TID) and every
                # later packet goes there, not to 69.
                peer = addr
                if pkt[1] == OP_ERROR:
                    raise RuntimeError(_tftp_error(pkt))
                if pkt[1] == OP_ACK and int.from_bytes(pkt[2:4], "big") == block:
                    break
            else:
                raise RuntimeError(f"no ACK for block {block}")

            if block * BLKSIZE >= len(data) and block:
                break
            payload = data[block * BLKSIZE:(block + 1) * BLKSIZE]
            block += 1
            chunk_pkt = (bytes([0, OP_DATA]) + block.to_bytes(2, "big")
                         + payload)
            s.sendto(chunk_pkt, peer)
            sent += len(payload)
            if len(payload) < BLKSIZE:
                # The short block ends it; its ACK is still owed.
                try:
                    pkt, _ = s.recvfrom(1024)
                    if pkt[1] == OP_ERROR:
                        raise RuntimeError(_tftp_error(pkt))
                except socket.timeout:
                    pass
                break
    finally:
        s.close()
    print(f"remote: put {local} -> {remote}, {sent} bytes")
    return 0


def do_get(host, port, remote, local, timeout):
    s = _tftp_socket(timeout)
    req = bytes([0, OP_RRQ]) + remote.encode() + b"\0octet\0"
    s.sendto(req, (host, port))
    out = bytearray()
    expect = 1
    peer = None
    ack = None          # the last ACK, which is what a timeout resends
    try:
        while True:
            for _ in range(5):
                try:
                    pkt, addr = s.recvfrom(1024)
                except socket.timeout:
                    if peer and ack:
                        s.sendto(ack, peer)
                    else:
                        s.sendto(req, (host, port))
                    continue
                peer = addr
                if pkt[1] == OP_ERROR:
                    raise RuntimeError(_tftp_error(pkt))
                if pkt[1] == OP_DATA and int.from_bytes(pkt[2:4], "big") == expect:
                    break
            else:
                raise RuntimeError(f"no block {expect}")
            payload = pkt[4:]
            out += payload
            ack = bytes([0, OP_ACK]) + expect.to_bytes(2, "big")
            s.sendto(ack, peer)
            expect += 1
            if len(payload) < BLKSIZE:
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
