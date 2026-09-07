#!/usr/bin/env python3
"""Drive /bin/wget's HTTPS path against a TLS server on this machine.

WHAT IT PROVES, and the order matters -- each check is only meaningful
because the one before it can fail:

  1. https:// fetches a body, over TLS 1.3, with the certificate
     VERIFIED against an anchor in /etc/ssl/certs.
  2. ...and the same fetch is REFUSED when the store holds the wrong
     anchor. Without this, check 1 proves only that bytes moved.
  3. ...and refused differently when the store is EMPTY, because "this
     machine was never told whom to trust" and "this certificate is not
     trusted" send you to different places.
  4. -k connects anyway AND SAYS SO. An unverified fetch that printed
     nothing would be the worst outcome of the three.
  5. The entropy gate refuses to key a connection from TSC jitter, and
     names the source it is refusing.
  6. Plain http:// still works, which is the regression check for
     routing wget through the same library.

**IT NEVER LEAVES THIS MACHINE.** The server is a Python http.server on
the host with a self-signed certificate, reached through SLIRP's
10.0.2.2 -- the same arrangement tools/net_test.py uses, and the reason
this can run offline. Verifying against a PUBLIC certificate is a
different question and is not this tool's.

    python3 tools/https_test.py
    python3 tools/https_test.py --keep          # leave the guest running
    python3 tools/https_test.py --positive-control

THE POSITIVE CONTROL swaps the trusted anchor for an unrelated one while
still expecting check 1 to succeed. A run in which it stays green is a
run where verification is not happening at all.

Needs `openssl` on PATH to make the certificates; SKIPS cleanly without
it rather than failing, since the gate must not start requiring one.
"""
import argparse
import http.server
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# net_test already solved launching a guest and WAITING FOR ITS ADDRESS.
# The address arrives about a second after the debug prompt does, so a
# fetch issued at the prompt drives an unconfigured machine and fails as
# "cannot connect" -- which looks nothing like what it is. Reusing that
# is the whole reason this file is short.
from net_test import launch, kill  # noqa: E402
from mkpart_test import volume_of  # noqa: E402

PORT_TLS = 18443
PORT_PLAIN = 18080
GATEWAY = "10.0.2.2"
BODY = b"hello over TLS from the host\n"


class Check:
    def __init__(self):
        self.rows = []

    def ok(self, name, cond, detail=""):
        self.rows.append((bool(cond), name, detail))
        print(f"  {'ok  ' if cond else 'FAIL'}  {name}"
              + (f"   [{detail}]" if detail and not cond else ""))
        return bool(cond)

    def failed(self):
        return [r for r in self.rows if not r[0]]


def make_certs(tmp):
    """A server certificate for the gateway address, and an unrelated
    one that must NOT validate it.

    The SAN carries IP:10.0.2.2 because that is what the guest connects
    to -- a certificate whose name does not match is a different failure
    from one that is not trusted, and conflating them would make check 2
    pass for the wrong reason."""
    server = os.path.join(tmp, "server.pem")
    serverkey = os.path.join(tmp, "server.key")
    other = os.path.join(tmp, "other.pem")
    otherkey = os.path.join(tmp, "other.key")
    for cert, key, subj, san in (
        (server, serverkey, "/CN=toyos-test", f"subjectAltName=IP:{GATEWAY},DNS:toyos-test"),
        (other, otherkey, "/CN=not-the-server", "subjectAltName=DNS:not-the-server"),
    ):
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key,
             "-out", cert, "-days", "30", "-nodes", "-subj", subj, "-addext", san],
            check=True, capture_output=True)
    return server, serverkey, other


def tls_server(cert, key, port):
    """THREADED, and that is not an optimisation. A single-threaded
    HTTPServer does the whole TLS handshake inside accept(), so one
    client that connects and walks away -- which is exactly what the
    entropy-gate check does -- wedges every later connection and the
    remaining checks fail as "cannot connect"."""
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)

        def log_message(self, *a):
            pass

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def plain_server(port):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def install_anchor(disk, pem, name):
    """Put a PEM into the image's /etc/ssl/certs.

    The volume is FOUND rather than assumed: disk.img is partitioned, so
    tfs3_writer needs the offset, and `volume_of` locates the TFS3
    volume by looking at the table instead of trusting "partition 1"."""
    lba, sectors = volume_of(disk)
    subprocess.run(
        [sys.executable, os.path.join(HERE, "tfs3_writer.py"), "write",
         "--at-lba", str(lba), "--sectors", str(sectors), disk, pem,
         f"/etc/ssl/certs/{name}"],
        check=True, capture_output=True)


def clear_anchors(disk):
    """Empty the trust store COMPLETELY, by listing it rather than by
    deleting the names this tool put there.

    An image built with `make iso EXTRAS=1` already holds the Mozilla
    bundle, so removing only our own anchors leaves 121 roots behind --
    and the "empty store" check then sees a verification FAILURE (those
    roots correctly do not vouch for a self-signed test certificate)
    instead of the refusal it is looking for. That is a fixture the test
    did not establish, wearing the costume of a code regression."""
    lba, sectors = volume_of(disk)

    def writer(*argv):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, "tfs3_writer.py"), argv[0],
             "--at-lba", str(lba), "--sectors", str(sectors), disk, *argv[1:]],
            capture_output=True, text=True)

    listing = writer("ls", "/etc/ssl/certs")
    for line in listing.stdout.splitlines():
        parts = line.split()
        # "-  1155  ino=490  toyos-test.pem" -- files only, so the "."
        # and ".." rows (which are "d") are skipped.
        if len(parts) < 4 or parts[0] != "-":
            continue
        writer("delete", f"/etc/ssl/certs/{parts[-1]}")


def boot(disk, tmp, tag):
    return launch(disk, tmp, tag, "e1000")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--positive-control", action="store_true",
                    help="trust the WRONG anchor and still expect check 1 to pass")
    args = ap.parse_args()

    if not shutil.which("openssl"):
        print("https_test: SKIP -- no openssl on PATH to make certificates")
        return 0

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("https_test: SKIP -- no disk.img; run `make iso` first")
        return 0

    c = Check()
    tmp = tempfile.mkdtemp(prefix="https_test.")
    pidfile = None
    srv = srv2 = None
    try:
        server, serverkey, other = make_certs(tmp)
        srv = tls_server(server, serverkey, PORT_TLS)
        srv2 = plain_server(PORT_PLAIN)

        # A COPY, so the real image is never modified -- and because a
        # copy of disk.img must stay SPARSE or it costs 9 GB.
        disk = os.path.join(tmp, "https.img")
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", disk_src, disk],
                       check=True)

        clear_anchors(disk)
        trusted = other if args.positive_control else server
        install_anchor(disk, trusted, "anchor.pem")

        print("https_test: booting")
        sh, pidfile = boot(disk, tmp, "https")

        url = f"https://{GATEWAY}:{PORT_TLS}/x"
        plain = f"http://{GATEWAY}:{PORT_PLAIN}/x"

        # 1. verified fetch
        out = sh.run(f"wget --weak-entropy {url}")
        c.ok("a verified https fetch returns the body",
             "hello over TLS from the host" in out, out.strip()[:120])
        c.ok("...over TLS 1.3", "TLSv1.3" in out, out.strip()[:120])
        c.ok("...and prints no 'not verified' warning",
             "NOT verified" not in out, out.strip()[:120])

        # 5. the entropy gate, before the store is disturbed
        out = sh.run(f"wget {url}")
        c.ok("without --weak-entropy the entropy gate refuses",
             "randomness" in out and "TSC jitter" in out, out.strip()[:140])

        # 4. -k connects and says so
        out = sh.run(f"wget --weak-entropy -k {url}")
        c.ok("-k fetches the body", "hello over TLS from the host" in out,
             out.strip()[:120])
        c.ok("-k WARNS that the identity was not verified",
             "NOT verified" in out, out.strip()[:120])

        # 6. plain http still works through the same library
        out = sh.run(f"wget {plain}")
        c.ok("plain http still works", "hello over TLS from the host" in out,
             out.strip()[:120])

        # 7. the scheme guess. A bare host tries https first and falls
        # back only when 443 does not answer -- and the fallback must be
        # ANNOUNCED, because the user never asked for plaintext.
        out = sh.run(f"wget --weak-entropy {GATEWAY}/x")
        c.ok("a bare host falls back to http when 443 is not listening",
             "falling back to http" in out, out.strip()[:140])

        # An explicit non-443 port cancels the guess, so a plain server
        # on an odd port just works instead of failing at a handshake it
        # is not having.
        out = sh.run(f"wget {GATEWAY}:{PORT_PLAIN}/x")
        c.ok("a bare host:port uses http and does not guess https",
             "hello over TLS from the host" in out and "falling back" not in out,
             out.strip()[:140])

        # A scheme this does not speak is REFUSED rather than treated as
        # a hostname -- "ftp://x" is not a host called "ftp:".
        out = sh.run("wget ftp://10.0.2.2/x")
        c.ok("an unknown scheme is refused, not guessed at",
             "cannot parse" in out, out.strip()[:140])

        # 2 and 3 need the store changed, so the guest is rebooted with
        # a different one rather than the file being edited underneath a
        # running kernel.
        kill(pidfile)
        pidfile = None
        clear_anchors(disk)
        install_anchor(disk, other if not args.positive_control else server, "wrong.pem")
        sh, pidfile = boot(disk, tmp, "https2")

        out = sh.run(f"wget --weak-entropy {url}")
        c.ok("a WRONG anchor fails verification",
             "verification failed" in out, out.strip()[:140])

        kill(pidfile)
        pidfile = None
        clear_anchors(disk)
        sh, pidfile = boot(disk, tmp, "https3")

        out = sh.run(f"wget --weak-entropy {url}")
        c.ok("an EMPTY trust store refuses, and says which",
             "no trust anchors" in out, out.strip()[:140])

    finally:
        if pidfile and not args.keep:
            kill(pidfile)
        for s in (srv, srv2):
            if s:
                s.shutdown()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"https_test: guest and files kept in {tmp}")

    bad = c.failed()
    print(f"\nhttps_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")

    if args.positive_control:
        if bad:
            print("https_test: positive control OK -- verification is real")
            return 0
        print("https_test: POSITIVE CONTROL DID NOT FAIL -- "
              "the certificate is not being verified at all")
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
