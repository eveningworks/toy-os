#!/usr/bin/env python3
"""tools/remote.py cannot hang: every wait is bounded, against fakes that misbehave.

A `remote.py flash` once blocked for 25 minutes after its session died,
and was killed with the Lenovo half-flashed. These are the ways its code
could wait without end, each driven by a fake peer on localhost -- no
guest, no network, a few seconds:

  - a TFTP server that answers every window with a STALE ACK: `put`
    must give up rather than resend the window forever;
  - a TFTP server that resends the same DATA block forever: `get` must
    give up the same way;
  - a TFTP server that LOSES the ACK ending a window and resends the
    window, as tftpd does, faster than the client's own timeout: `get`
    must re-ACK on the duplicate and finish, or the server gives up;
  - a telnet peer that talks without end and never sends the marker:
    `read_until_line` must still honour its timeout;
  - `_mark_executable` given the flash's held session must USE it, not
    open a fresh one -- a fresh one is what dies mid-flash.

--against PATH runs the same checks against another copy of remote.py
(`git show <rev>:tools/remote.py > /tmp/old.py`): the pre-fix one must
fail them, or they are not testing the fix.

    python3 tools/remote_hang_test.py [--against /tmp/old_remote.py]
"""

import argparse
import importlib.util
import os
import socket
import sys
import tempfile
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
LIMIT = 20.0   # seconds a bounded call may take here; the fakes use a 1s timeout


def load(path):
    spec = importlib.util.spec_from_file_location("remote_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def bounded(fn):
    """(finished, raised) for fn() run with a LIMIT: a hang is (False, None)."""
    box = {}

    def go():
        try:
            fn()
            box["raised"] = None
        except BaseException as e:   # noqa: BLE001 -- the result under test
            box["raised"] = e
    t = threading.Thread(target=go, daemon=True)
    t.start()
    t.join(LIMIT)
    return (not t.is_alive()), box.get("raised")


def udp_server(behave):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(0.2)

    def loop():
        end = time.time() + LIMIT + 5
        while time.time() < end:
            try:
                pkt, addr = s.recvfrom(65536)
            except socket.timeout:
                continue
            behave(s, pkt, addr)
    threading.Thread(target=loop, daemon=True).start()
    return s.getsockname()[1]


def stale_acks(s, pkt, addr):
    # Every WRQ and every DATA gets "ACK 0": answered, never moved.
    s.sendto(bytes([0, 4, 0, 0]), addr)


def same_block(s, pkt, addr):
    # Every RRQ and every ACK gets DATA block 1 again, full-sized so it
    # never reads as the last one.
    s.sendto(bytes([0, 3, 0, 1]) + b"x" * 512, addr)


def lost_ack_server():
    """Two blocks. The first ACK of block 1 is 'lost': block 1 is resent
    every 0.3 s -- inside the client's 0.5 s timeout, so only a re-ACK on
    the duplicate moves it -- and after five, the server gives up."""
    state = {"acks": 0, "moved": False}
    one = bytes([0, 3, 0, 1]) + b"a" * 512
    two = bytes([0, 3, 0, 2]) + b"b" * 100

    def resend(s, addr):
        for _ in range(5):
            time.sleep(0.3)
            if state["moved"]:
                return
            s.sendto(one, addr)
        if not state["moved"]:
            s.sendto(bytes([0, 5, 0, 0]) + b"gave up\0", addr)

    def behave(s, pkt, addr):
        if pkt[1] == 1:                                    # RRQ: options ignored
            s.sendto(one, addr)
            threading.Thread(target=resend, args=(s, addr), daemon=True).start()
        elif pkt[1] == 4 and pkt[2:4] == b"\0\1":
            state["acks"] += 1
            if state["acks"] > 1 and not state["moved"]:
                state["moved"] = True
                s.sendto(two, addr)
    return udp_server(behave), b"a" * 512 + b"b" * 100


def chatty_telnet():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def serve():
        c, _ = srv.accept()
        end = time.time() + LIMIT + 5
        try:
            while time.time() < end:
                c.sendall(b"still talking\r\n")
                time.sleep(0.02)
        except OSError:
            pass
    threading.Thread(target=serve, daemon=True).start()
    return srv.getsockname()[1]


class FakeSession:
    def __init__(self):
        self.ran = []

    def run(self, cmd, timeout=None):
        self.ran.append(cmd)
        return []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--against", default=os.path.join(TOOLS, "remote.py"),
                    help="the remote.py to test (default: this checkout's)")
    args = ap.parse_args()
    r = load(args.against)
    fails = []

    def check(name, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"\n        {detail}"))
        if not ok:
            fails.append(name)

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "payload")
        with open(src, "wb") as f:
            f.write(os.urandom(4000))

        port = udp_server(stale_acks)
        done, err = bounded(lambda: r.do_put("127.0.0.1", port, src, "/x", 0.5, quiet=True))
        check("put gives up on a server that only repeats a stale ACK",
              done and err is not None, "still running" if not done else f"returned without error: {err!r}")

        port = udp_server(same_block)
        done, err = bounded(lambda: r.do_get("127.0.0.1", port, "/x", os.path.join(tmp, "out"), 0.5))
        check("get gives up on a server that resends one block forever",
              done and err is not None, "still running" if not done else f"returned without error: {err!r}")

        port, want = lost_ack_server()
        out = os.path.join(tmp, "out2")
        done, err = bounded(lambda: r.do_get("127.0.0.1", port, "/x", out, 0.5))
        got = open(out, "rb").read() if done and err is None and os.path.exists(out) else b""
        check("get re-ACKs a resent window, so a lost ACK does not end it",
              got == want, "still running" if not done else f"{err!r}, {len(got)} bytes")

        port = chatty_telnet()
        t = r.Telnet("127.0.0.1", port, 1.0)
        done, err = bounded(lambda: t.read_until_line("__done0__", 1.0))
        t.close()
        check("a telnet peer that never stops talking still times out",
              done and isinstance(err, TimeoutError),
              "still running" if not done else f"ended with {err!r}")

        # _mark_executable must use a lent session. Opening its own is made
        # to fail loudly here, so a version that ignores the loan is caught.
        bindir = os.path.join(tmp, "bin")
        os.mkdir(bindir)
        open(os.path.join(bindir, "hello"), "w").close()
        fake = FakeSession()

        def no_new_session(*a, **k):
            raise AssertionError("opened a fresh session")
        real_session, r.Session = r.Session, no_new_session
        try:
            try:
                r._mark_executable("127.0.0.1", 1, bindir, "/bin", 1.0, fake)
                used = fake.ran == ["chmod 755 /bin/hello"]
                detail = repr(fake.ran)
            except TypeError as e:            # no `sess` parameter at all
                used, detail = False, str(e)
        finally:
            r.Session = real_session
        check("the chmod pass uses the flash's held session", used, detail)

    print(f"\nremote_hang_test: {5 - len(fails)} passed, {len(fails)} failed")
    # Daemon threads may still be spinning inside a hung call: leave hard.
    sys.stdout.flush()
    os._exit(1 if fails else 0)


if __name__ == "__main__":
    main()
