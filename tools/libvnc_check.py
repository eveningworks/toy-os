#!/usr/bin/env python3
"""/bin/remoted against a FOREIGN VNC client: libvncclient, what Remmina uses.

tools/vnc_test.py's client is this repo's own, so a misreading of RFB shared
by the server and that client passes there. This builds a tiny client on
the real libvncclient (Debian's, in Docker) and connects it to a guest:
it logs in, takes frames, and reports the security type it ended up with,
the encoding the server sent and how many updates arrived -- with
`--tls` it must have used VeNCrypt (RFB type 19), and without it plain
VNC Authentication.

  python3 tools/libvnc_check.py --port 15903 --password toyvnc12
  python3 tools/libvnc_check.py --port 15903 --password toyvnc12 --tls

It needs Docker and, the first time, the network: the image is debian:12
plus libvncserver-dev from deb.debian.org (`--build` rebuilds it). It
boots nothing -- point it at a guest that is up, a vm.py one with
`--hostfwd tcp::15903-:5900` -- and reaches it through the host network.

ON DEMAND (ondemand_sweep.py's exclusions say why).
"""
import argparse
import os
import subprocess
import sys
import tempfile

IMAGE = "toyos-libvnc:1"

DOCKERFILE = """FROM debian:12
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev \\
    libvncserver-dev libgnutls28-dev ca-certificates && rm -rf /var/lib/apt/lists/*
"""

# The client: libvncclient's own handshake (security negotiation, VeNCrypt,
# decoding), so what it accepts is what Remmina's core accepts.
PROBE = r"""
#include <rfb/rfbclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_pw, *g_ca;
static int g_updates, g_last_enc = -1, g_tls;

static char *get_password(rfbClient *c) { (void)c; return strdup(g_pw); }

// VeNCrypt's X509 subtypes ask for a CA; none is given, which is how a
// viewer meets a self-signed server for the first time.
static rfbCredential *get_credential(rfbClient *c, int type) {
    (void)c;
    rfbCredential *cr = calloc(1, sizeof *cr);
    if (type == rfbCredentialTypeX509) {
        g_tls = 1;
        // The server's own certificate as the CA: pinning it, which is
        // what a person does after comparing the fingerprint.
        if (g_ca) cr->x509Credential.x509CACertFile = strdup(g_ca);
        cr->x509Credential.x509CrlVerifyMode = rfbX509CrlVerifyNone;
        return cr;
    }
    if (type == rfbCredentialTypeUser) {
        cr->userCredential.username = strdup("toy");
        cr->userCredential.password = strdup(g_pw);
        return cr;
    }
    free(cr);
    return NULL;
}

static void got_update(rfbClient *c, int x, int y, int w, int h) {
    (void)c; (void)x; (void)y; (void)w; (void)h;
    g_updates++;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    g_pw = argv[3];
    g_ca = argc > 4 ? argv[4] : NULL;

    rfbClient *c = rfbGetClient(8, 3, 4);
    // With a CA to trust, VeNCrypt only -- whatever order the server gave.
    if (g_ca) { static const uint32_t want[] = { 19, 0 }; SetClientAuthSchemes(c, want, -1); }
    c->GetPassword = get_password;
    c->GetCredential = get_credential;
    c->GotFrameBufferUpdate = got_update;
    c->serverHost = strdup(argv[1]);
    c->serverPort = atoi(argv[2]);
    c->appData.encodingsString = "tight zrle ultra copyrect hextile zlib corre rre raw";
    if (!rfbInitClient(c, NULL, NULL)) { printf("RESULT init-failed\n"); return 1; }
    for (int i = 0; i < 200 && g_updates < 3; i++) {
        int r = WaitForMessage(c, 100000);
        if (r < 0) break;
        if (r > 0 && !HandleRFBServerMessage(c)) { printf("RESULT handle-failed\n"); return 1; }
        if (i % 10 == 0) SendFramebufferUpdateRequest(c, 0, 0, c->width, c->height, i > 0);
    }
    printf("RESULT size=%dx%d updates=%d tls=%d\n", c->width, c->height, g_updates, g_tls);
    rfbClientCleanup(c);
    return g_updates > 0 ? 0 : 1;
}
"""


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15903)
    ap.add_argument("--password", required=True)
    ap.add_argument("--tls", action="store_true", help="it must have negotiated VeNCrypt")
    ap.add_argument("--ca", help="the server's certificate (PEM) to trust; implies --tls")
    ap.add_argument("--build", action="store_true", help="rebuild the image")
    args = ap.parse_args()

    if args.build or not sh(["docker", "image", "inspect", IMAGE]).returncode == 0:
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "Dockerfile"), "w") as fh:
                fh.write(DOCKERFILE)
            print(f"libvnc_check: building {IMAGE} (debian:12 + libvncserver-dev)", flush=True)
            r = sh(["docker", "build", "-t", IMAGE, d])
            if r.returncode:
                print(r.stdout[-2000:], r.stderr[-2000:])
                return 1
    if args.ca:
        args.tls = True
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "probe.c"), "w") as fh:
            fh.write(PROBE)
        ca = ""
        if args.ca:
            with open(args.ca) as src, open(os.path.join(d, "ca.pem"), "w") as dst:
                dst.write(src.read())
            ca = " /w/ca.pem"
        r = sh(["docker", "run", "--rm", "--network", "host", "-v", f"{d}:/w", IMAGE, "sh", "-c",
                f"gcc -O1 -o /w/probe /w/probe.c -lvncclient && /w/probe {args.host} {args.port} "
                f"'{args.password}'{ca}"], timeout=120)
    out = r.stdout + r.stderr
    print(out[-1500:])
    res = [ln for ln in out.splitlines() if ln.startswith("RESULT")]
    if not res or "updates=0" in res[-1] or "failed" in res[-1]:
        print("libvnc_check: FAIL -- libvncclient got no picture")
        return 1
    if args.tls and "tls=1" not in res[-1]:
        print("libvnc_check: FAIL -- the session was not encrypted (no VeNCrypt)")
        return 1
    print("libvnc_check: PASS --", res[-1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
