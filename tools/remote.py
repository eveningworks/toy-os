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
    python3 tools/remote.py --host 192.168.200.104 screenshot shot.png
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
import hashlib
import os
import re
import socket
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_guard  # noqa: E402  -- the staging-freshness check, see do_flash()

# RFC 854.
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA, OPT_NAWS = 1, 3, 31

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_TELNET_PORT = 23
DEFAULT_TFTP_PORT = 69

# The rescue-kernel copy is a multi-megabyte filesystem operation, so it
# gets a floor of its own rather than inheriting --timeout.
ROTATE_TIMEOUT = 180.0

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
        # ...OR A LINE THAT ENDS WITH IT, because the marker does not
        # always land alone. The shell repaints its input line with
        # carriage returns and ANSI, and after a long reply the prompt
        # and the marker render as ONE line -- `/$__done20__` -- which
        # an equality test misses and then times out on output that is
        # sitting right there. Seen once `gui menu --json` grew past a
        # kilobyte.
        #
        # The ECHO of the marker's own command ends with it too, and
        # matching that would stop before the command being framed has
        # run -- so a line carrying `echo <marker>` is explicitly not
        # the end of the reply. That is the guarantee this function is
        # for, kept.
        def is_end(line):
            return line.endswith(marker) and f"echo {marker}" not in line

        end = time.time() + timeout
        while True:
            lines = render(self.buf.decode("utf-8", "replace"))
            for i, line in enumerate(lines):
                if is_end(line):
                    return lines[:i]
            # THE DEADLINE IS CHECKED, not only a quiet recv: a peer that
            # keeps talking without ever sending the marker never lets
            # recv time out, and this looped forever on one.
            left = end - time.time()
            try:
                if left <= 0:
                    raise socket.timeout
                self.s.settimeout(left)
                chunk = self.s.recv(4096)
            except socket.timeout:
                # NAME THE FIX. A command that simply runs longer than
                # this looks identical to a wedged machine, and the
                # partial output below is then read as the command's
                # RESULT rather than as a truncated capture -- which is
                # how a 25s `kbd` recording was read as "no keypresses".
                raise TimeoutError(
                    f"no {marker!r} line within {timeout}s -- if the command "
                    f"runs longer than that, raise it with --timeout and the "
                    f"output below is TRUNCATED, not the answer; got: "
                    f"{self.buf[-400:]!r}")
            if not chunk:
                raise EOFError(f"connection closed; got: {self.buf[-400:]!r}")
            self.buf += self._strip(chunk)

    def drain_quiet(self, idle=0.3, cap=3.0):
        """Read until the machine has said nothing for `idle` seconds.

        THE MARKER MUST NOT BE TYPED WHILE THE GUEST IS BUSY TALKING.
        The shell echoes every character it receives, so sending the
        framing line straight after a command that is about to print
        two kilobytes makes the guest echo and print at once -- and its
        input ring drops characters under exactly that load. Measured
        on the ASUS with `guictl menu --json`: 2 runs in 3, the third
        losing the marker and timing out on a machine that was working
        perfectly.

        Bounded by `cap` because a command that streams for a long time
        must not hold this forever: the marker is still the authority,
        and sending it into a quiet moment is an optimisation of the
        input path, not a substitute for the framing.
        """
        end = time.time() + cap
        while time.time() < end:
            try:
                self.s.settimeout(idle)
                chunk = self.s.recv(4096)
            except socket.timeout:
                return          # quiet for `idle` -- the moment to type into
            except OSError:
                return
            if not chunk:
                return
            self.buf += self._strip(chunk)

    def send_line(self, line):
        self.s.sendall(line.encode() + b"\r\n")


ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
# The same alphabet, as a SPLIT: render() has to see the erase sequences
# rather than have them stripped, so it walks tokens instead of a
# scrubbed string. Capturing group, so re.split keeps the separators.
ANSI_TOKEN_RE = re.compile(r"(\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\))")


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

    AND THE ERASE IS PART OF THAT REPAINT. The editor sends `\r`, then
    `ESC[J`, then the new text -- so a line that got SHORTER leaves the
    old tail behind unless the erase is honoured. Stripping every escape
    sequence, which this did, rendered `/$__done0__` as
    `/$__done0__one0__`: the marker was on the wire, the framing did not
    match it, and `remote.py exec` timed out on output that was sitting
    right there. Only the erases are interpreted; every other sequence
    is still dropped, because colour and cursor-shape changes do not
    move text.
    """
    lines, cur, pos = [], "", 0
    for tok in ANSI_TOKEN_RE.split(text):
        if not tok:
            continue
        if tok.startswith("\x1b"):
            # ESC[J / ESC[0J (to end of display) and ESC[K / ESC[0K (to
            # end of line) both mean "nothing after the cursor on this
            # line". The display-wide forms would also clear the lines
            # BELOW, which a stream being replayed into a list does not
            # have -- there is nothing after the cursor yet.
            if tok in ("\x1b[J", "\x1b[0J", "\x1b[K", "\x1b[0K"):
                cur = cur[:pos]
            continue
        for ch in tok:
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


class Session:
    """One shell session, with the marker framing `exec` documents.

    Its own class because `sync` runs dozens of commands and a session
    per command costs a TCP connect and a settle each time -- which is
    most of the wall clock when the commands themselves are `sum`.
    """

    def __init__(self, host, port, timeout):
        self.t = Telnet(host, port, timeout)
        self.timeout = timeout
        self._n = 0
        # Settle: wait for the shell to say something. A prompt is
        # whatever it is -- we never match on it, only on our markers.
        time.sleep(0.4)
        try:
            self.t.s.settimeout(2.0)
            self.t.buf += self.t._strip(self.t.s.recv(4096))
        except (socket.timeout, OSError):
            pass

    def run(self, cmd, timeout=None):
        """Run one command, returning its output lines."""
        marker = f"__done{self._n}__"
        self._n += 1
        self.t.buf = b""
        self.t.send_line(cmd)
        # ...and only then the frame, once the machine has gone quiet --
        # see drain_quiet(). The marker echo is the frame; `echo` is a
        # /bin program, so this also proves the shell is still running
        # commands rather than sitting in one that never returned.
        self.t.drain_quiet()
        self.t.send_line(f"echo {marker}")
        lines = self.t.read_until_line(marker, timeout or self.timeout)
        # Drop what the shell echoed back at us -- the two command
        # lines we typed, each with a prompt in front of it -- and
        # any blank lines those left at the ends.
        lines = [ln for ln in lines
                 if not ln.endswith(cmd) and not ln.endswith(f"echo {marker}")]
        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and not lines[-1].strip():
            lines.pop()
        return lines

    def close(self):
        self.t.close()


def do_exec(host, port, commands, timeout):
    rc = 0
    try:
        sess = Session(host, port, timeout)
    except OSError as e:
        print(f"remote: {e}", file=sys.stderr)
        return 1
    try:
        for cmd in commands:
            lines = sess.run(cmd, timeout)
            if len(commands) > 1:
                print(f"--- {cmd} ---")
            print("\n".join(lines))
    except (TimeoutError, EOFError) as e:
        print(f"remote: {e}", file=sys.stderr)
        rc = 1
    finally:
        sess.close()
    return rc


# --- TFTP ---------------------------------------------------------------
#
# OPTIONS ARE NEGOTIATED (RFC 2347), and the two that matter are
# `blksize` (RFC 2348) and `windowsize` (RFC 7440). Lock-step is not
# slow for the reason it looks slow -- the cost is a round trip, not
# bandwidth: a blocked server process resumes on the next 10 ms
# scheduler tick. Measured 2026-09-14, a whole `flash --force` to the
# ASUS moved ~57 MiB of 240 files in 210 s, about 280 KB/s or ~5 ms a
# block at blksize 1428. (An earlier note here said 32 ms a block; that
# was 512-byte blocks before blksize was negotiated, and quoting it
# extrapolates a flash six times longer than it takes.)
#
# `windowsize` IS NEGOTIATED NOW, at 3 -- the guest's tftpd.c caps it
# there (WINDOW_MAX), so three blocks fly per ACK and a round trip buys
# 3 * 1428 bytes instead of 512. This comment said "asks for a window of
# 1, so every block still costs a round trip" long after that stopped
# being true; the measurement above (~280 KB/s) is the CURRENT rate and
# already includes it.
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


# A TFTP transfer gives up when nothing has MOVED it for this many
# timeouts -- silence or chatter alike.
STALL_ROUNDS = 6


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


def do_put(host, port, local, remote, timeout, quiet=False):
    # A HAND-NAMED seed/sync FILE DODGES THE FLASH'S OWN GUARD, and
    # sending one reads as the fix not working rather than as the fix
    # not being sent. See iso_guard.check_staged_file().
    iso_guard.assert_staged_file(local)
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
        # PROGRESS BOUNDS THIS, NOT ONLY SILENCE. `tries` counts quiet
        # waits, and any reply resets nothing towards it -- so a server
        # repeating a stale ACK kept the loop resending one window forever.
        stall = time.time() + timeout * STALL_ROUNDS

        while acked < total:
            if time.time() > stall:
                raise RuntimeError(f"no progress past block {acked} in "
                                   f"{timeout * STALL_ROUNDS:.0f}s -- the server "
                                   "answers, but nothing it says moves the transfer")
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
            before = acked
            if a == want:
                acked += n
            elif ((a - acked) & 0xFFFF) <= n:
                acked += (a - acked) & 0xFFFF
            # Only a MOVE is progress: an ACK for the block already
            # confirmed advances nothing, and resetting on it was a hang.
            if acked == before:
                continue
            tries = 0
            stall = time.time() + timeout * STALL_ROUNDS
            sent_bytes = min(acked * blksize, len(data))
    finally:
        s.close()
    if not quiet:
        print(f"remote: put {local} -> {remote}, {sent_bytes} bytes")
    return 0


def do_screenshot(a, host, tport, fport, timeout):
    """What the real machine's screen looks like, as a file on this one.

    THE ONLY WAY TO SEE A BARE-METAL SCREEN FROM HERE. `vm.py` drives a
    QEMU guest and can ask QMP for a screendump; the laptop has no such
    channel and nobody is sitting in front of it. So the capture is taken
    BY THE MACHINE -- /bin/screenshot, an ordinary program that asks the
    compositor for pixels -- and fetched over the same TFTP link
    everything else here uses.

    PNG by default, because the point is to open it on THIS machine and
    nothing here decodes QOI. The guest writes it; there is no host-side
    conversion step to go wrong.

    /var/tmp, never /tmp: the latter is a ramfs mount, and TFTP reads the
    file back through the filesystem a moment later -- which works either
    way, but "anything that must survive is on the disk" is the rule and
    a screenshot is worth keeping if the fetch fails.
    """
    fmt = a.format
    if not fmt:
        fmt = "qoi" if a.local.lower().endswith(".qoi") else "png"
    remote = f"/var/tmp/remote-shot.{fmt}"

    cmd = f"screenshot -f {fmt}"
    if a.window:
        cmd += " -w"
    if a.pointer:
        cmd += " -p"
    if a.region:
        cmd += f" -r {a.region}"
    if a.delay:
        cmd += f" -d {a.delay}"
    cmd += f" {remote}"

    sess = Session(host, tport, timeout)
    try:
        out = sess.run(cmd, timeout)
        sess.run("sync", timeout)
    finally:
        sess.close()
    # The command is SILENT when it succeeds with a named path, so any
    # output at all is the failure -- which is why this reports what it
    # said rather than only that the fetch found nothing.
    for line in out:
        if line.startswith("screenshot:"):
            print(f"remote: {line}", file=sys.stderr)
            return 1

    rc = do_get(host, fport, remote, a.local, timeout)
    if rc == 0:
        print(f"{a.local} ({os.path.getsize(a.local)} bytes)")
    return rc


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
        stall = time.time() + timeout * STALL_ROUNDS   # see do_put()
        while True:
            if time.time() > stall:
                raise RuntimeError(f"no progress past block {expect - 1} in "
                                   f"{timeout * STALL_ROUNDS:.0f}s -- the server "
                                   "answers, but nothing it sends is the next block")
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
            stall = time.time() + timeout * STALL_ROUNDS
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



# --- sync ---------------------------------------------------------------
#
# WHY NOT JUST PUSH EVERYTHING. `/bin` is ~24 MB across ~90 files, and
# TFTP to the laptop runs about 430 KB/s, so a blind push is a couple of
# minutes every time -- for a change that usually moves three binaries.
# The pieces to do better already exist: `/bin/sum` (with libhash.so)
# can hash the machine's own files, so the host can ask what is already
# right and send only what is not. rsync's idea, minus the rolling
# checksum, which buys nothing for files this size.
#
# CRC32 AND SIZE, NOT SHA-256. This decides whether to re-send a file
# the developer just built, not whether to trust one -- and crc32 is
# what the guest computes fastest. The size is carried alongside because
# crc32 collides readily on adversarial input and essentially never on
# (crc, size) for two builds of the same program. `sum -a sha256` is
# there if a reason to distrust it ever turns up.
#
# ONE SESSION FOR EVERY QUERY, one TFTP transfer per file that differs.
# The `sum` calls are batched because a shell line has a length limit
# and 90 paths do not fit in one.

# THE COMMAND LINE IS 128 BYTES, so the manifest goes over as a FILE.
# `KLINE_MAX` (kernel/include/api/klineedit.h) bounds a shell line, and
# a `sum` with a dozen paths runs past it -- the line is TRUNCATED
# mid-path, so every file after the cut reports "no such file" and looks
# like it needs sending. The first version batched by path COUNT and
# re-sent 51 of 86 files on a second run against a machine that was
# already correct. `sum -c LISTFILE` reads the expected checksums from a
# file instead, which is one command whatever the tree's size.

# /var/tmp, not /tmp: this file's whole job is to still be there on the
# NEXT run, so `sync` can send only what differs, and /tmp on the target
# is a ramfs mount now. /var/tmp is the FHS's "scratch that survives".
SUMS_REMOTE = "/var/tmp/sync.sums"


def _local_manifest(local_dir):
    """(relative path -> (crc32, size)) for everything under `local_dir`."""
    out = {}
    for root, _dirs, files in os.walk(local_dir):
        for name in sorted(files):
            full = os.path.join(root, name)
            if os.path.islink(full):
                continue
            rel = os.path.relpath(full, local_dir)
            data = open(full, "rb").read()
            out[rel.replace(os.sep, "/")] = (zlib.crc32(data), len(data))
    return out


def _remote_mismatches(sess, host, tftp_port, remote_dir, want, timeout,
                       missing_only=False):
    """Which of `want` the machine does NOT already have, byte for byte.

    Asks the machine rather than trusting a local record of what was
    sent: a record is a second source of truth, and the case that
    matters most -- somebody rebuilt and did not deploy -- is exactly
    when it would be wrong.
    """
    base = remote_dir.rstrip("/")
    lines = "".join(f"{crc} {size} {base}/{rel}\n"
                    for rel, (crc, size) in sorted(want.items()))
    tmp = os.path.join(tempfile.gettempdir(), "toyos_sync.sums")
    with open(tmp, "w") as f:
        f.write(lines)
    if do_put(host, tftp_port, tmp, SUMS_REMOTE, timeout, quiet=True):
        raise RuntimeError("could not send the checksum manifest")

    bad = []
    for line in sess.run(f"sum -c {SUMS_REMOTE}", timeout):
        # "<path>: OK", "<path>: FAILED", "<path>: FAILED open or read".
        # Its own summary lines start with "sum:" and are not results.
        if line.startswith("sum:") or ": " not in line:
            continue
        path, _, verdict = line.rpartition(": ")
        if verdict.startswith("OK"):
            continue
        # "FAILED open or read" is ABSENT; a bare "FAILED" is PRESENT AND
        # DIFFERENT. `sum` is the only thing that tells those apart
        # cheaply, and for /etc they mean opposite things -- see
        # do_sync()'s missing_only.
        if missing_only and not verdict.startswith("FAILED open"):
            continue
        if path.startswith(base + "/"):
            bad.append(path[len(base) + 1:])
    return bad


def do_sync(host, telnet_port, tftp_port, local_dir, remote_dir, timeout,
            dry_run=False, missing_only=False, sess=None, force=False):
    """`missing_only` sends only what the machine does NOT already have.

    That is dpkg's conffile rule, and it is what /etc needs: a new
    service or setting descriptor has to ARRIVE, while a file the
    machine already has is its CONFIGURATION -- overwriting it would
    throw away whatever was set on that machine.

    **`sess` LETS A CALLER SUPPLY THE SESSION, and do_flash must.** The
    comparison needs a shell -- it lists and hashes the remote tree --
    and a flash REPLACES the programs that shell is made of. Opening a
    fresh session per tree therefore works exactly until the first tree
    lands: with a new /bin against the still-old /lib, `tosh` cannot
    start, telnetd accepts a connection and closes it, and the flash
    fails on the NEXT tree having written no kernel. One session opened
    before any write survives all of it, which is the same property the
    rescue session relies on.
    """
    if not os.path.isdir(local_dir):
        print(f"remote: {local_dir} is not a directory", file=sys.stderr)
        return 1

    want = _local_manifest(local_dir)
    # An EMPTY tree is a tree: /home/desktop ships empty by design, and
    # the machine still needs the directory. Its subdirectories come
    # from the local walk below, files or not.
    local_dirs = set()
    for root, subdirs, _files in os.walk(local_dir):
        for d in subdirs:
            rel = os.path.relpath(os.path.join(root, d), local_dir)
            local_dirs.add(rel.replace(os.sep, "/"))

    own_session = sess is None
    if own_session:
        try:
            sess = Session(host, telnet_port, timeout)
        except OSError as e:
            print(f"remote: {e}", file=sys.stderr)
            return 1

    try:
        # `force` sends the whole tree, comparing nothing: the comparison
        # runs the MACHINE's `sum`, and the laptop's is wrong on large
        # files (docs/bugs.md) -- a flash that trusts it can leave a
        # binary behind that the new kernel then cannot run.
        todo = sorted(want) if force else \
               _remote_mismatches(sess, host, tftp_port, remote_dir, want,
                                  timeout, missing_only)

        # Every directory a file needs, THE ROOT INCLUDED, parents
        # first. `mkdir` on one that exists is harmless, and asking
        # first would cost a round trip per directory to save nothing.
        #
        # The root is the part that was missing: a first sync to a path
        # that does not exist yet wrote every file into nowhere, and
        # TFTP reported each one as sent. /bin and /lib already existed,
        # so only a scratch directory exposed it.
        base = remote_dir.rstrip("/")
        dirs = {base}
        for r in todo:
            d = os.path.dirname(r)
            while d:
                dirs.add(f"{base}/{d}")
                d = os.path.dirname(d)
        for d in local_dirs:
            dirs.add(f"{base}/{d}")
        dirs = sorted(dirs, key=lambda x: x.count("/"))
        bytes_todo = sum(want[r][1] for r in todo)
        print(f"remote: {len(want)} file(s), {len(todo)} to send "
              f"({bytes_todo / 1024.0:.0f} KB)")
        if dry_run:
            for r in todo:
                print(f"  would send {r}")
            return 0
        for d in dirs:
            sess.run(f"mkdir {d}", timeout)
    except (TimeoutError, EOFError, RuntimeError) as e:
        print(f"remote: {e}", file=sys.stderr)
        return 1
    finally:
        if own_session:
            sess.close()   # a BORROWED session is the caller's to close

    sent = 0
    for r in todo:
        local = os.path.join(local_dir, r.replace("/", os.sep))
        remote = f"{remote_dir.rstrip('/')}/{r}"
        rc = do_put(host, tftp_port, local, remote, timeout)
        if rc:
            print(f"remote: FAILED at {r} after {sent} file(s)",
                  file=sys.stderr)
            return 1
        sent += 1

    if not todo:
        print("remote: already up to date")
    else:
        print(f"remote: sent {sent} file(s)")

    _mark_executable(host, telnet_port, local_dir, remote_dir, timeout,
                     None if own_session else sess)
    return 0


# Directories whose contents must be executable, as the host seeder
# spells it (tools/tfs3_writer.py's EXEC_DIRS). Kept in step by hand
# rather than imported, because that module is about writing an IMAGE
# and this one is about a running machine.
EXEC_DIRS = ("bin", "tests")


def _mark_executable(host, telnet_port, local_dir, remote_dir, timeout, sess=None):
    """chmod 755 everything under /bin and /tests on the target.

    **A SYNC CANNOT LEAVE THE MODE TO THE KERNEL'S DEFAULT, and this is
    not belt-and-braces.** A file arrives here through the kernel rather
    than through the host seeder, so it gets whatever
    kernel/fs/tfs3_internal.h's T3_MODE_DEFAULT says -- and a file that did NOT
    change is not re-sent at all, so it keeps whatever mode it was
    written with, possibly years and several defaults ago. That left
    /bin/cat at 0644 on the laptop after a full --force flash, and dash
    refuses a file with no execute bit (EACCES), so the shell could not
    run a single external command.

    Every file is chmod'd, not only the ones just sent, for exactly that
    reason: the stale ones are the problem. It is one telnet round trip
    per file and only runs on a sync, which is already the slow path.
    """
    # **TWO CALL SHAPES, AND MISSING ONE OF THEM FAILED SILENTLY.**
    # `sync seed/sync /` passes the whole tree, so /bin is a
    # SUBDIRECTORY of local_dir. `flash` syncs one tree at a time
    # (USERLAND_TREES), so local_dir IS seed/sync/bin and remote_dir is
    # /bin. The first version handled only the former, found no
    # subdirectory named bin, and returned without a word -- so a full
    # flash left every mode untouched and said nothing about it.
    remote_top = "/" + remote_dir.strip("/").split("/")[0] if remote_dir.strip("/") else ""
    names = []
    if remote_top.lstrip("/") in EXEC_DIRS:
        for f in sorted(os.listdir(local_dir)):
            if os.path.isfile(os.path.join(local_dir, f)):
                names.append(f"{remote_dir.rstrip('/')}/{f}")
    else:
        for d in EXEC_DIRS:
            local_d = os.path.join(local_dir, d)
            if not os.path.isdir(local_d):
                continue
            for f in sorted(os.listdir(local_d)):
                if os.path.isfile(os.path.join(local_d, f)):
                    names.append(f"{remote_dir.rstrip('/')}/{d}/{f}")
    if not names:
        return
    # THE FLASH'S HELD SESSION WHEN THERE IS ONE. A fresh session is
    # exactly what dies once /bin is new and /lib old -- the whole reason
    # do_flash holds one -- and this used to open its own, so a flash
    # printed "could not set execute bits" and left every mode unset.
    own = sess is None
    if own:
        sess = Session(host, telnet_port, timeout)
    try:
        for n in names:
            # A failure is not fatal: the filesystem may not store modes
            # at all (ramfs), and a machine that cannot chmod is still a
            # machine that booted.
            sess.run(f"chmod 755 {n}", timeout)
    except Exception as e:      # noqa: BLE001 -- reported, never raised
        print(f"remote: could not set execute bits ({e})", file=sys.stderr)
    finally:
        if own:
            sess.close()
    print(f"remote: marked {len(names)} file(s) executable")


def _boot_device(sess, timeout):
    """The block device mounted at /boot, and whether it is writable."""
    for ln in sess.run("mount", timeout):
        f = ln.split()
        if len(f) >= 4 and f[1] == "/boot":
            return f[0], f[3]
    return None, None


def _grub_timeout_ok(sess, timeout):
    """True if grub.cfg draws a menu -- see do_flash's docstring.

    Asked with `grep` rather than `cat`: the config is a few KB of
    commentary and reading it whole over telnet outruns the session
    timeout, which reads as the machine not answering.

    THE PATTERN IS ONE WORD BECAUSE `tosh` DOES NOT QUOTE -- it splits
    a line on whitespace and hands the pieces over as they are, so
    `'^set timeout='` arrives as two arguments and grep reads the
    second as a filename. Anchoring is done here instead.
    """
    for ln in sess.run("grep timeout /boot/boot/grub/grub.cfg", timeout):
        t = ln.strip()
        if t.startswith("set timeout=") and t != "set timeout=0":
            return True
    return False


def _core_modules(sess, timeout):
    """What the machine's INSTALLED bootloader can do, as a set of GRUB
    module names -- empty when nothing says.

    /etc/grub-core.modules is written by `install --bootloader`, and it
    is the only readable record: core.img sits in raw sectors nothing can
    open and is lzma-compressed besides. An older machine has no stamp,
    which reads as "cannot be shown to have gzio" -- the safe direction,
    since the consequence of being wrong is a dead default menu entry.
    """
    mods = set()
    for ln in sess.run("cat /etc/grub-core.modules", timeout):
        t = ln.strip()
        if not t or ":" in t:      # an error line names the path
            continue
        mods.update(t.split())
    return mods


def _sha256(sess, remote, timeout):
    for ln in sess.run(f"sum -a sha256 {remote}", timeout):
        f = ln.split()
        if len(f) == 2 and len(f[0]) == 64:
            return f[0]
    return None


def _verify_offline(host, tftp_port, local, want, timeout,
                    rescue=None, reboot=False):
    """Verify the flashed kernel WITHOUT a shell, and say what to do.

    Reached when a FRESH telnet session dies after the /lib sync -- see
    do_flash(). TFTP is a separate service and keeps working, so the
    kernel can still be read back and checked.

    `rescue` is a session opened BEFORE /lib was replaced, if do_flash
    managed to hold one. It can still reboot the machine because
    /bin/reboot is statically linked (see the Makefile): the shell on
    the far end is already running, and the one thing it has to spawn
    does not go through /lib. Without it this ends in an instruction to
    press the power button, which is where it ended for every flash
    before 2026-09-07.
    """
    print("remote: the shell went away after the /lib sync -- verifying "
          "over TFTP instead", file=sys.stderr)
    fd, tmp = tempfile.mkstemp(prefix="remote_verify_")
    os.close(fd)
    try:
        if do_get(host, tftp_port, "/boot/boot/kernel.bin", tmp, timeout):
            print("remote: COULD NOT READ THE KERNEL BACK. Its state is "
                  "unknown.\nremote: /boot/boot/kernel.old still holds the "
                  "kernel this machine\nremote: is running -- pick "
                  "\"toy-os (previous kernel)\" in GRUB if it will not boot.",
                  file=sys.stderr)
            return 1
        with open(tmp, "rb") as fh:
            got = hashlib.sha256(fh.read()).hexdigest()
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass

    if got != want:
        print(f"remote: FLASH DID NOT VERIFY\nremote:   want {want}"
              f"\nremote:   got  {got}\nremote: DO NOT REBOOT -- pick "
              "\"toy-os (previous kernel)\" in GRUB if you already have.",
              file=sys.stderr)
        return 1

    print(f"remote: flashed and verified {os.path.basename(local)} "
          f"({os.path.getsize(local)} bytes), over TFTP")

    if reboot and rescue is not None:
        print("remote: rebooting through the session held from before the "
              "/lib sync")
        try:
            rescue.run("reboot", 3.0)
        except (TimeoutError, EOFError, OSError):
            pass              # the machine going away IS the reply
        return 0

    if rescue is not None:
        # Held a session, but nobody asked for a reboot. Say what is
        # true rather than the old blanket instruction: this machine
        # CAN be rebooted from here, on request.
        print("remote: the new kernel and userland are both in place. This "
              "machine is\nremote: running the old kernel with the new "
              "shared libraries, so re-run\nremote: with --reboot (or press "
              "the power button) to come back matched.")
        return 0

    print("remote: PRESS THE POWER BUTTON. The new kernel and userland are "
          "both in place,\nremote: but this machine is running the old "
          "kernel with the new shared\nremote: libraries, so nothing here "
          "can reboot it. It comes back on the\nremote: new kernel.")
    return 0


# What `make iso` stages, and where each tree lives on the machine. The
# kernel is only half of a build: a change to a syscall ABI struct moves
# fields under binaries compiled against the old layout, and the failure
# is not a crash -- it is a machine that boots perfectly and cannot be
# given an address. That happened, from this tool, on 2026-09-05.
# /etc IS IN THE LIST AND IS SYNCED NEW-FILES-ONLY (do_sync's
# missing_only), which is dpkg's conffile rule. It has to be here at all
# because a NEW service or setting descriptor otherwise never reaches a
# machine: `flash` replaced the kernel and the binaries, the machine
# came up perfectly, and the feature simply was not there -- which is
# how the `tmpfs` service was found missing on both laptops on
# 2026-09-06, with /tmp still on disk and nothing saying why. And it has
# to be new-files-only because everything else under /etc is that
# machine's CONFIGURATION; sending the staged copy would throw away
# whatever was set on it.
#
# /lib LAST, and that ordering is load-bearing. Replacing a shared
# library under a running system is the one part of this that hurts
# immediately rather than at the next boot -- a flash interrupted after
# /lib leaves a machine whose telnetd accepts a connection and closes
# it, which is how it was found. Doing it last keeps the window between
# "libraries replaced" and "rebooted into the matching kernel" as small
# as this can make it.
#
# **/bin FIRST IS ITS OWN HAZARD, and the order does not fix it.** The
# new binaries in /bin are linked against the new libraries, so from the
# moment /bin lands until /lib follows, nothing dynamic on the machine
# can start -- including the shell every later step needs. There is no
# ordering that avoids that; what makes the flash survive it is holding
# ONE session opened before the first write (do_flash).
# (staged subdirectory, path on the machine, new files only)
# /home is NEW FILES ONLY like /etc: the desktop folder has to EXIST on
# the machine, and what a person put in it is theirs.
# `install` IS A TREE LIKE ANY OTHER, and leaving it out meant a machine
# could never hand on a bootloader newer than the one it was installed
# with: the laptop's /install/core.img was three weeks stale, from before
# `gzio` joined CORE_MODULES, which is also the image `install
# --bootloader` writes.
#
# **/etc/settings.d IS AN EXCEPTION TO THE /etc RULE, and it has to come
# FIRST.** Those files are not that machine's configuration -- they are
# shipped metadata describing what each setting IS (its type, bounds and
# choices), and since the desktop's settings became declarations rather
# than kernel C they are what makes a setting EXIST at all. New-files-
# only would leave a machine's older copies in place, and an older copy
# without a `Type=` line declares nothing: every desktop setting would
# quietly vanish from System Settings on a flashed machine while the
# newly-added ones appeared, which reads as a half-broken settings app.
# The disk image has always treated this directory that way (it is under
# seed/sync, content-hash synced, not seed/once), so this is the two
# paths agreeing rather than a new policy. It precedes ("etc", ...)
# because the /etc pass that follows skips whatever already exists.
USERLAND_TREES = (("bin", "/bin", False), ("tests", "/tests", False),
                  ("usr", "/usr", False),
                  ("etc/settings.d", "/etc/settings.d", False),
                  ("etc", "/etc", True),
                  ("home", "/home", True), ("lib", "/lib", False),
                  ("install", "/install", False))


def do_flash(host, telnet_port, tftp_port, local, timeout, reboot,
             kernel_only=False, staging="seed/sync", force=False):
    """Replace the kernel on the machine's own boot partition.

    THE RESCUE ENTRY IS THE POINT. grub.cfg already offers "toy-os
    (previous kernel)" reading /boot/kernel.old, so a kernel that will
    not boot is one menu pick away from one that will -- but only if
    GRUB draws a menu at all, and an installed machine has
    `set timeout=0`. So this refuses to flash until the timeout is
    nonzero, and rotates the RUNNING kernel into kernel.old rather than
    trusting whatever was there.

    The order is what makes it survivable: menu first, rescue copy
    second, the new kernel last, and a sha256 read back off the
    partition before anything reboots.

    AND THE USERLAND GOES WITH IT. A kernel is half a build. Flashing
    one alone is fine until an ABI struct changes size, at which point
    the machine boots and every syscall taking that struct reads the
    wrong fields -- which presented as a laptop that came up healthy
    with no network and could not be reached to fix it. So /bin, /lib,
    /tests and /usr are synced from the build staging first, and
    --kernel-only is the deliberate way to not.

    /etc goes too, but NEW FILES ONLY -- a new service or setting
    descriptor has to arrive, and a file the machine already has is its
    configuration and is not ours to overwrite.

    AND THE STAGING TREE IS CHECKED BEFORE ANY OF IT. `make all` writes
    build/ and stops; seed/sync is populated by the `seed` target that
    `make iso` runs -- so flashing after a bare `make all` sends the
    PREVIOUS build's userland, and the sync cannot save you because it
    faithfully compares the machine against that stale staging and
    correctly reports "already up to date". That is how a laptop came to
    run a /bin/about one build behind its libraries and page-fault in
    __rt_tls_init. Same refusal, same bypass variable, as a stale ISO.

    The sync goes FIRST so a failure there costs nothing: the machine is
    still running the kernel it booted. It does leave a short window of
    new userland on the old kernel, which the reboot closes -- and which
    is why this reboots for you when asked rather than leaving it.

    DO NOT PUT A WALL-CLOCK TIMEOUT AROUND THIS. A full userland is
    ~25 MB over TFTP and takes minutes; one killed part-way leaves a
    machine with some trees replaced and no matching kernel, which needs
    a power cycle to recover because the half that broke is the half
    that answers telnet. If you must bound it, bound it generously.
    """
    kernel_current = 0      # set when the machine already has this kernel
    if not kernel_only:
        iso_guard.assert_staging_fresh()
    if local is not None and not os.path.isfile(local):
        print(f"remote: no such file: {local}", file=sys.stderr)
        return 1

    sess = Session(host, telnet_port, timeout)
    try:
        dev, opts = _boot_device(sess, timeout)
        if not dev:
            print("remote: nothing is mounted at /boot", file=sys.stderr)
            return 1
        if opts != "rw":
            sess.run("umount /boot", timeout)
            sess.run(f"mount {dev} /boot", timeout)
            dev, opts = _boot_device(sess, timeout)
            if opts != "rw":
                print("remote: /boot will not mount read-write",
                      file=sys.stderr)
                return 1

        if not _grub_timeout_ok(sess, timeout):
            print("remote: /boot/boot/grub/grub.cfg has `set timeout=0`, so "
                  "the rescue entry\nremote: cannot be reached. Fix that "
                  "first -- a bad flash would need a USB stick.",
                  file=sys.stderr)
            return 1

        # **WHICH KERNEL, DECIDED BY WHAT THE MACHINE'S GRUB CAN DO.**
        # The compressed kernel is worth ~1 MB of a 280 KB/s link and
        # boots only where `gzio` is in the core image, which is a
        # property of the machine and not of this checkout -- so it is
        # READ from the machine (`install --bootloader` records it) and
        # never assumed. No stamp means the ELF, which every GRUB reads.
        gzio = "gzio" in _core_modules(sess, timeout)
        if local is None:
            media = os.path.join(REPO, "build", "kernel.media")
            elf = os.path.join(REPO, "build", "kernel.bin")
            local = media if (gzio and os.path.isfile(media)) else elf
            if not os.path.isfile(local):
                print(f"remote: no such file: {local} -- run `make all` "
                      "first", file=sys.stderr)
                return 1
        with open(local, "rb") as fh:
            blob = fh.read()
        want = hashlib.sha256(blob).hexdigest()

        # A GZIPPED KERNEL TO A MACHINE THAT CANNOT UNPACK ONE is not a
        # failed flash -- it is a machine whose default menu entry is
        # dead, recovered only from the rescue entry at its own keyboard.
        # That happened; hence the refusal rather than a warning.
        if blob[:2] == b"\x1f\x8b" and not gzio:
            print(f"remote: {local} is gzipped, and this machine's "
                  "bootloader does not\nremote: record `gzio` "
                  "(/etc/grub-core.modules). GRUB would read it as raw "
                  "bytes:\nremote:   no multiboot header found\n"
                  "remote: Run `install --bootloader confirm` on the "
                  "machine first, or send build/kernel.bin.",
                  file=sys.stderr)
            return 1
        if blob[:2] == b"\x1f\x8b":
            print(f"remote: sending the compressed kernel "
                  f"({len(blob) // 1024} KiB) -- this machine's GRUB "
                  "records gzio")

        if _sha256(sess, "/boot/boot/kernel.bin", timeout) == want:
            # **A MATCHING KERNEL SKIPS THE KERNEL WRITE AND NOTHING
            # ELSE.** This used to `return 0` here -- so a flash whose
            # kernel happened to match did not sync the userland, did not
            # reboot, and said "that kernel is already installed", which
            # reads like success. That is a flash that silently changes
            # nothing, and it is how the laptop came to run a /bin/about
            # one build behind its libraries: the kernel was current, so
            # nothing else was even looked at. Measured 2026-09-16 --
            # corrupt /bin/hello on the machine, flash an unchanged
            # kernel, and the corruption survives.
            #
            # An earlier fix made --force alone fall through, which left
            # the ordinary path carrying the bug it was diagnosing.
            kernel_current = 1
            print("remote: that kernel is already installed -- syncing the "
                  "userland anyway")

        if kernel_current:
            print("remote: leaving /boot alone -- the kernel already matches")
        else:
            print("remote: rotating the running kernel to /boot/boot/kernel.old")
        # ITS OWN TIMEOUT, not --timeout. This copies ~5 MB through the
        # guest's filesystem and took longer than the 15s default on a
        # real laptop, which aborted the flash before it had sent a byte
        # -- and left the rescue slot holding a partial copy.
        if not kernel_current:
            sess.run("cp /boot/boot/kernel.bin /boot/boot/kernel.old",
                     max(timeout, ROTATE_TIMEOUT))
    finally:
        sess.close()

    # ONE SESSION FOR THE WHOLE FLASH, opened before the FIRST write and
    # held to the end. Replacing what a running machine's programs are
    # made of kills every LATER spawn the moment the pieces disagree
    # about an ABI struct: telnetd accepts a connection and its shell
    # dies, so a FRESH session cannot be made. A session opened before
    # any of that is already running and survives it, and /bin/reboot is
    # statically linked so the one thing it still has to spawn does not
    # go through /lib.
    #
    # **IT USED TO BE OPENED JUST BEFORE /lib, AND THAT WAS TOO LATE.**
    # /bin breaks a shell sooner: its new binaries are linked against
    # the new libraries, which are sent LAST, so `tosh` stops starting
    # the moment /bin lands. do_sync then opened a fresh session per
    # tree, got "connection closed" on the very next one, and the flash
    # gave up having written no kernel -- leaving /bin new, /lib old and
    # the machine reachable only over TFTP. Both laptops were left in
    # that state on 2026-09-07, which is how this was found.
    #
    # So the session is opened first and LENT to every sync (do_sync's
    # `sess`), which is also what turned "press the power button" into
    # a reboot.
    rescue = None
    if not kernel_only:
        try:
            rescue = Session(host, telnet_port, timeout)
        except (TimeoutError, EOFError, OSError):
            # Not fatal where nothing that breaks a shell gets written
            # (a machine already up to date). It IS fatal for a real
            # update, and the sync says so when it cannot list a tree.
            print("remote: could not hold a session before the sync -- if "
                  "this flash replaces /bin or /lib it will not complete, "
                  "and a reboot may need the power button", file=sys.stderr)
        for sub, remote, new_only in USERLAND_TREES:
            local_dir = os.path.join(staging, sub)
            if not os.path.isdir(local_dir):
                print(f"remote: no {local_dir} -- run `make iso` first",
                      file=sys.stderr)
                if rescue is not None:
                    rescue.close()
                return 1
            if do_sync(host, telnet_port, tftp_port, local_dir, remote,
                       timeout, False, new_only, sess=rescue,
                       force=force and not new_only):
                print(f"remote: FAILED syncing {remote} -- the kernel has "
                      "NOT been written", file=sys.stderr)
                if rescue is not None:
                    rescue.close()
                return 1

    # The userland is in sync by here whatever happens next, which is the
    # whole point of not returning early above.
    if kernel_current:
        print("remote: kernel unchanged, so nothing to write or verify")
    else:
        rc = do_put(host, tftp_port, local, "/boot/boot/kernel.bin", timeout)
        if rc:
            if rescue is not None:
                rescue.close()
            return rc

    # THE VERIFY SESSION IS ON THE WRONG SIDE OF THE /lib SYNC, and that
    # is not a hypothetical: the new libc.so and libuapp.so are already
    # in place while the machine still runs the OLD kernel, so when the
    # two disagree about an ABI struct, telnetd's child dies the moment
    # it spawns and every later connection is accepted and closed. The
    # flash itself is fine at that point -- the kernel reached /boot --
    # but nothing can be asked about it and nothing can reboot it.
    #
    # So a dead session here falls back to reading the kernel BACK over
    # TFTP, which is a different service and keeps working, and says
    # plainly that the machine needs its power button. Failing with
    # "connection closed" instead sent two sessions looking for a fault
    # that was not there.
    try:
        sess = Session(host, telnet_port, timeout)
    except (TimeoutError, EOFError, OSError):
        try:
            return _verify_offline(host, tftp_port, local, want, timeout,
                                   rescue, reboot)
        finally:
            if rescue is not None:
                rescue.close()
    try:
        sess.run("sync", timeout)
        got = _sha256(sess, "/boot/boot/kernel.bin", timeout)
        if got != want:
            print(f"remote: FLASH DID NOT VERIFY\nremote:   want {want}"
                  f"\nremote:   got  {got}\nremote: kernel.old still holds "
                  "the kernel this machine is running -- do not reboot "
                  "before\nremote: retrying, and pick the rescue entry if "
                  "you already have.", file=sys.stderr)
            return 1
        print(f"remote: flashed and verified {os.path.basename(local)} "
              f"({os.path.getsize(local)} bytes)")
        if reboot:
            print("remote: rebooting")
            try:
                sess.run("reboot", 3.0)
            except (TimeoutError, EOFError, OSError):
                pass          # the machine going away IS the reply
    except (TimeoutError, EOFError, OSError):
        # The session died PART WAY THROUGH -- the /lib mismatch can
        # take the shell out between two commands as easily as before
        # the first one, so the rescue path is the same one here.
        return _verify_offline(host, tftp_port, local, want, timeout,
                               rescue, reboot)
    finally:
        for open_sess in (sess, rescue):
            if open_sess is None:
                continue
            try:
                open_sess.close()
            except OSError:
                pass
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
    # LINE-BUFFERED, so a flash redirected to a file records how far it
    # got: block-buffered, a killed run lost every progress line and kept
    # only stderr -- which is how a hang's location went unrecorded.
    sys.stdout.reconfigure(line_buffering=True)
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

    y = sub.add_parser("sync", help="copy a directory tree, skipping what "
                                    "already matches")
    y.add_argument("local")
    y.add_argument("remote")
    y.add_argument("--dry-run", action="store_true",
                   help="say what would be sent, send nothing")
    y.add_argument("--new-only", action="store_true",
                   help="send only files the machine does NOT have, never "
                        "overwrite one it does -- what /etc needs")
    y.add_argument("--force", action="store_true",
                   help="send EVERY file, comparing nothing -- the machine's "
                        "`sum` is what the comparison trusts, and the "
                        "laptop's is wrong on large files (docs/bugs.md)")

    f = sub.add_parser("flash", help="replace the kernel on the machine's "
                                     "own boot partition")
    # OMITTED, THE MACHINE DECIDES: build/kernel.media where its own
    # /etc/grub-core.modules records `gzio`, and build/kernel.bin where
    # it does not. A flash must not depend on a capability the INSTALLED
    # bootloader may not have -- it replaces the kernel alone, never the
    # bootloader, and the laptop's GRUB predated that module and read the
    # gzip bytes raw: `no multiboot header found`, default entry dead.
    # Name a file to override; a gzipped one is still refused where the
    # stamp does not back it.
    f.add_argument("kernel", nargs="?", default=None)
    # **REBOOTING IS THE DEFAULT, and it is the safe direction.** A
    # flash that verifies a kernel and does NOT boot it leaves the
    # machine running the OLD kernel against the NEW /lib -- the exact
    # mismatch do_flash's rescue session exists to survive, and the one
    # that strands a machine. So the dangerous state was what you got by
    # FORGETTING a flag, which is the wrong way round; every session
    # forgot it. `--reboot` is still accepted, and now says nothing.
    f.add_argument("--no-reboot", dest="reboot", action="store_false",
                   help="leave the machine on the OLD kernel -- it will be "
                        "running that against the new /lib until something "
                        "reboots it")
    f.add_argument("--reboot", dest="reboot", action="store_true",
                   help="(default; kept so an old command line still works)")
    f.set_defaults(reboot=True)
    f.add_argument("--kernel-only", action="store_true",
                   help="do NOT sync /bin, /lib, /tests and /usr first. An "
                        "ABI change then leaves the machine unreachable")
    f.add_argument("--staging", default="seed/sync",
                   help="what `make iso` staged (default: seed/sync)")
    f.add_argument("--force", action="store_true",
                   help="sync every file, comparing nothing (see sync --force)")

    sc = sub.add_parser("screenshot", help="capture the machine's screen")
    sc.add_argument("local", nargs="?", default="screenshot.png",
                    help="where to write it (default screenshot.png)")
    sc.add_argument("--window", action="store_true",
                    help="the topmost window rather than the whole screen")
    sc.add_argument("--pointer", action="store_true",
                    help="include the mouse pointer")
    sc.add_argument("--region", metavar="X,Y,W,H", help="a rectangle")
    sc.add_argument("--delay", type=int, default=0, metavar="SECONDS",
                    help="wait before capturing")
    sc.add_argument("--format", choices=("png", "qoi"), default=None,
                    help="default: from the filename's extension")

    sub.add_parser("shell", help="an interactive session (Ctrl-] quits)")

    a = ap.parse_args()
    try:
        if a.cmd == "exec":
            return do_exec(a.host, a.telnet_port, a.commands, a.timeout)
        if a.cmd == "put":
            return do_put(a.host, a.tftp_port, a.local, a.remote, a.timeout)
        if a.cmd == "get":
            return do_get(a.host, a.tftp_port, a.remote, a.local, a.timeout)
        if a.cmd == "sync":
            return do_sync(a.host, a.telnet_port, a.tftp_port,
                           a.local, a.remote, a.timeout, a.dry_run,
                           a.new_only, force=a.force)
        if a.cmd == "flash":
            return do_flash(a.host, a.telnet_port, a.tftp_port,
                            a.kernel, a.timeout, a.reboot,
                            a.kernel_only, a.staging, force=a.force)
        if a.cmd == "screenshot":
            return do_screenshot(a, a.host, a.telnet_port, a.tftp_port, a.timeout)
        if a.cmd == "shell":
            return do_shell(a.host, a.telnet_port, a.timeout)
    except (OSError, RuntimeError) as ex:
        print(f"remote: {ex}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
