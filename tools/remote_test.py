#!/usr/bin/env python3
"""telnetd, tftpd and tools/remote.py, end to end against a QEMU guest.

WHAT IT CHECKS
  1. `service disable` removes a descriptor and `service enable` copies
     one back out of /usr/share/services and init starts it -- the only
     test of the available-vs-enabled split anywhere. The disable runs
     FIRST so the check establishes its own precondition: disk.img is
     synced rather than reformatted, so a service left enabled by an
     earlier run would otherwise be what is measured.
  2. A remote command returns the OUTPUT of that command, not an echo of
     the command line. The distinction is the whole framing problem: a
     broken framer returns the echo and looks like it worked.
  3. A file pushed and pulled back is byte-for-byte identical, with
     CONTENT THAT IS NOT UNIFORM -- a run of identical bytes cannot tell
     a working block walk from one that repeats a block.
  4. A file crossing the 512-byte block boundary EXACTLY, which is the
     case that needs a final zero-length DATA packet and the one a
     hand-written TFTP nearly always gets wrong.
  5. A binary pushed over TFTP actually RUNS on the far side. This is
     the property the whole feature exists for and the only check here
     that would notice a transfer that is subtly wrong rather than
     absent.
  6. OPTION NEGOTIATION (RFC 2347/2348/7440) in both directions, and
     the fallback to RFC 1350's defaults when a client asks for nothing.
     **The fallback is the half that breaks silently**: a transfer that
     quietly drops to 512-byte lockstep still succeeds and only looks
     slow, so the round-trip checks alone cannot see it -- which is why
     one check reads the server's own log for the negotiated values.

WHY SLIRP IS ENOUGH. TFTP's reply normally comes from a fresh ephemeral
port, which no NAT forwards back -- but the shipped service runs with
`-1` (see docs/commands/tftpd.md), so the reply comes from port 69 and
QEMU's hostfwd maps it. A guest configured the RFC way would need a real
network and is not what ships.

ON DEMAND, not in gui_regress: it boots its own guest, enables services
that ship disabled, and takes about a minute.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import remote as rmod                  # noqa: E402 -- WANT_BLKSIZE/WANT_WINDOW

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VM = os.path.join(HERE, "vm.py")
REMOTE = os.path.join(HERE, "remote.py")

TELNET_PORT = 2323
TFTP_PORT = 6969


class Result:
    def __init__(self):
        self.passes = []
        self.fails = []

    def check(self, what, ok, detail=""):
        (self.passes if ok else self.fails).append(what)
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}"
              + (f"\n          {detail}" if detail and not ok else ""))


def vm(*args, timeout=180):
    return subprocess.run([sys.executable, VM, *args], cwd=REPO,
                          capture_output=True, text=True, timeout=timeout)


def remote(*args, timeout=120):
    return subprocess.run(
        [sys.executable, REMOTE, "--host", "127.0.0.1",
         "--telnet-port", str(TELNET_PORT), "--tftp-port", str(TFTP_PORT),
         *args],
        cwd=REPO, capture_output=True, text=True, timeout=timeout)


def main():
    r = Result()
    tmp = tempfile.mkdtemp(prefix="remote_test_")
    print("remote_test: booting a guest with telnet and tftp forwarded")
    vm("stop")
    boot = vm("--hostfwd", f"tcp::{TELNET_PORT}-:23",
              "--hostfwd", f"udp::{TFTP_PORT}-:69", "start", timeout=300)
    if "ready" not in boot.stdout:
        print(f"remote_test: guest did not boot\n{boot.stdout}\n{boot.stderr}")
        return 1

    try:
        # 1. ESTABLISH THE PRECONDITION RATHER THAN INHERIT IT. disk.img
        #    is synced, never reformatted, so a service enabled by an
        #    earlier run is still enabled -- asserting "not enabled out
        #    of the box" against it tests the last person's session, not
        #    this build. Disabling first also makes this the only test of
        #    `service disable`.
        vm("exec", "service disable telnetd", "service disable tftpd")
        vm("exec", "service stop telnetd", "service stop tftpd")
        before = vm("exec", "service").stdout
        r.check("service disable takes the descriptor away",
                "telnetd" not in before and "tftpd" not in before,
                before[-300:])

        # AN ADMIN STOP OUTRANKS Restart=, so `enable` alone does not
        # bring back something explicitly stopped -- that is init's rule
        # (data/etc/services.d/README.md), and encoding it here is how
        # the next person finds out without losing an hour: the services
        # reported `running` and nothing answered the network.
        vm("exec", "service enable telnetd", "service enable tftpd")
        vm("exec", "service start telnetd", "service start tftpd")
        after = vm("exec", "service").stdout
        r.check("service enable + start brings telnetd back",
                "telnetd" in after and "stopped" not in after, after[-400:])
        r.check("service enable + start brings tftpd back",
                "tftpd" in after, after[-400:])

        # 2. Output, not echo. `uptime` is used because its output shares
        #    no words with the command line, so an echoed command line
        #    cannot pass this by accident.
        out = remote("exec", "uptime")
        r.check("a remote command returns its output",
                "up " in out.stdout and "uptime" not in out.stdout,
                repr(out.stdout[:300]) + repr(out.stderr[:200]))

        # 3/4. Round trips. The second size is an exact multiple of the
        #      512-byte block, which is the case that needs a final
        #      zero-length DATA packet to terminate.
        for size in (5000, 4096):
            src = os.path.join(tmp, f"blob{size}.bin")
            back = os.path.join(tmp, f"back{size}.bin")
            # Address-derived, not uniform: a walk that repeats or skips
            # a block reads back the wrong bytes rather than the same
            # ones. (The same rule /tests/memtest follows.)
            data = bytes(((i * 7 + (i >> 8) * 31) & 0xFF) for i in range(size))
            with open(src, "wb") as f:
                f.write(data)
            p = remote("put", src, f"/tmp/rt{size}.bin")
            g = remote("get", f"/tmp/rt{size}.bin", back)
            got = open(back, "rb").read() if os.path.exists(back) else b""
            r.check(f"a {size}-byte file round-trips byte for byte",
                    got == data,
                    f"put={p.stdout.strip()} get={g.stdout.strip()} "
                    f"{len(got)} of {size} bytes")

        # 6. OPTION NEGOTIATION (RFC 2347/2348/7440), in BOTH directions
        #    and including the fallback -- which is the half that can
        #    break silently, because a transfer that quietly drops to
        #    512/lockstep still succeeds and only looks slow.
        #
        #    The sizes straddle a 1428-byte block so the short final
        #    block and the exact-multiple case are both exercised at the
        #    negotiated size, not just at 512.
        for size in (1428 * 3, 1428 * 3 + 17):
            src = os.path.join(tmp, f"opt{size}.bin")
            back = os.path.join(tmp, f"optback{size}.bin")
            data = bytes(((i * 11 + (i >> 7) * 29) & 0xFF) for i in range(size))
            with open(src, "wb") as f:
                f.write(data)
            remote("put", src, f"/tmp/opt{size}.bin")
            remote("get", f"/tmp/opt{size}.bin", back)
            got = open(back, "rb").read() if os.path.exists(back) else b""
            r.check(f"a {size}-byte file round-trips with options on",
                    got == data, f"{len(got)} of {size} bytes")

        # THE SERVER MUST SAY WHAT IT AGREED TO, and the log is where it
        # says it. Without this the checks above pass on a server that
        # ignored every option -- which is exactly what they did before
        # the options existed.
        log = remote("exec", "dmesg").stdout
        r.check("the server negotiated a big block and a window",
                "blksize 1428, window 3" in log,
                [l for l in log.splitlines() if "tftpd: wrote" in l][-3:])

        # AND THE FALLBACK, driven by asking for nothing. A client that
        # negotiates no options must still work, because that is every
        # boot ROM and the RFC 1350 default.
        want_b, want_w = rmod.WANT_BLKSIZE, rmod.WANT_WINDOW
        try:
            rmod.WANT_BLKSIZE, rmod.WANT_WINDOW = 512, 1
            src = os.path.join(tmp, "plain.bin")
            back = os.path.join(tmp, "plainback.bin")
            data = bytes(((i * 13) & 0xFF) for i in range(3000))
            with open(src, "wb") as f:
                f.write(data)
            remote("put", src, "/tmp/plain.bin")
            remote("get", "/tmp/plain.bin", back)
            got = open(back, "rb").read() if os.path.exists(back) else b""
            r.check("a client asking for the defaults still round-trips",
                    got == data, f"{len(got)} of 3000 bytes")
        finally:
            rmod.WANT_BLKSIZE, rmod.WANT_WINDOW = want_b, want_w

        # 5. THE ONE THAT MATTERS: a binary pushed over the network runs
        #    on the far side.
        hello = os.path.join(REPO, "seed", "sync", "bin", "hello")
        if os.path.exists(hello):
            remote("put", hello, "/bin/hello_rt")
            run = remote("exec", "hello_rt")
            r.check("a binary pushed over tftp runs on the guest",
                    "ring 3" in run.stdout,
                    repr(run.stdout[:300]))
        else:
            r.check("a binary pushed over tftp runs on the guest", False,
                    "seed/sync/bin/hello missing -- run `make iso` first")
    finally:
        vm("stop")

    print(f"\nremote_test: {len(r.passes)} passed, {len(r.fails)} failed")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
