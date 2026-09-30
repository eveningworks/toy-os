#!/usr/bin/env python3
"""/bin/update end to end: fetch, verify, install live, and stage for boot.

WHAT IT PROVES, each against an INDEPENDENT reader (`sum`, `cat`,
`dmesg`) rather than update's own report:

  1. A fresh copy of disk.img is up to date against its own build.
  2. A damaged binary and a missing document are both found -- the
     second as NEW -- and installed in place: `sum` then agrees with the
     manifest's crc32.
  3. A file the SERVER corrupts in transit is refused, and the machine is
     left exactly as it was: the old file, and no `.upd` behind.
  4. A damaged LIBRARY is not replaced under the running system: it is
     staged, listed in /var/lib/update/pending, and the file on disk is
     unchanged -- until the next boot, whose kernel applies it
     ("update: applied 1 staged file") before init starts.
  5. `update --server` persists the address and records it as recent.

**IT NEVER LEAVES THIS MACHINE.** The server is tools/update_server.py's
own handler, run in this process on an ephemeral loopback port and
reached through SLIRP's 10.0.2.2.

    python3 tools/update_test.py
    python3 tools/update_test.py --positive-control

THE POSITIVE CONTROL serves the file UNCORRUPTED in check 3 while still
expecting a refusal, so a run where verification is not happening shows
up as a pass that should have been a failure.
"""
import argparse
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from net_test import launch, kill  # noqa: E402
import update_server  # noqa: E402

GATEWAY = "10.0.2.2"


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


class Server:
    """update_server's handler, on a port the OS picks, with a switch
    that flips one byte of one file on its way out."""

    def __init__(self):
        self.manifest = update_server.Manifest(os.path.join(ROOT, "seed", "sync"))
        self.tamper = None
        base = update_server.make_handler({"": self.manifest})
        outer = self

        class Handler(base):
            def log_message(self, *a):
                pass

            def _say(self, *a):
                pass

            def do_GET(self):
                if outer.tamper and self.path == "/files" + outer.tamper:
                    _, files = outer.manifest.build()
                    with open(files[outer.tamper], "rb") as fh:
                        body = bytearray(fh.read())
                    body[len(body) // 2] ^= 0x55
                    self._send(200, bytes(body), "application/octet-stream")
                    return
                super().do_GET()

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def crc(self, path):
        text, _ = self.manifest.build()
        for line in text.splitlines():
            f = line.split(" ")
            if len(f) >= 3 and f[2] == path:
                return int(f[0])
        return None


def summed(sh, path):
    """(crc, size) as the guest's own /bin/sum reads it, or None."""
    for line in sh.run(f"sum {path}").splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == path and f[0].isdigit():
            return int(f[0]), int(f[1])
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--positive-control", action="store_true",
                    help="serve check 3's file intact and still expect a refusal")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("update_test: SKIP -- no disk.img; run `make iso` first")
        return 0
    why = update_server._stale_reason(os.path.join(ROOT, "seed", "sync"))
    if why:
        print(f"update_test: FAIL -- the staging tree is stale ({why}); run `make iso`")
        return 1

    c = Check()
    tmp = tempfile.mkdtemp(prefix="update_test.")
    pidfile = None
    srv = Server()
    url = f"http://{GATEWAY}:{srv.port}"
    try:
        disk = os.path.join(tmp, "update.img")
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", disk_src, disk], check=True)
        print("update_test: booting")
        sh, pidfile = launch(disk, tmp, "upd", "e1000")

        # 1. a fresh image is its own build
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("a fresh disk.img is up to date against its own build", "up to date" in out,
             out.strip()[-160:])

        # 2. found, then installed in place
        doc = "/usr/share/doc/guide/kernel.md"
        sh.run("truncate -s 100 /bin/hello")
        sh.run(f"rm {doc}")
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("--check lists a damaged binary", "/bin/hello" in out, out.strip()[-200:])
        c.ok("...and a missing file, as new", doc in out and " new" in out, out.strip()[-200:])
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("update installs both in place, no restart", "2 files updated" in out,
             out.strip()[-200:])
        for path in ("/bin/hello", doc):
            got = summed(sh, path)
            c.ok(f"sum agrees with the manifest for {path}",
                 got is not None and got[0] == srv.crc(path), f"sum {got} manifest {srv.crc(path)}")

        # 3. a corrupted transfer changes nothing
        sh.run("truncate -s 100 /bin/hello")
        srv.tamper = None if args.positive_control else "/bin/hello"
        out = sh.run(f"update --from {url}", timeout=600)
        srv.tamper = None
        c.ok("a file corrupted in transit is refused",
             "crc32 does not match" in out and "nothing was changed" in out, out.strip()[-200:])
        got = summed(sh, "/bin/hello")
        c.ok("...and the damaged file is left as it was", got is not None and got[1] == 100,
             f"sum {got}")
        c.ok("...with no staged copy behind", "hello.upd" not in sh.run("ls /bin"))

        # 4. a library waits for the boot
        sh.run(f"update --from {url}", timeout=600)   # put /bin/hello back first
        sh.run("truncate -s 100 /lib/libhello.so")
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("a library is staged for the next boot, not replaced",
             "staged" in out and "restart" in out, out.strip()[-200:])
        c.ok("...listed in /var/lib/update/pending",
             "/lib/libhello.so" in sh.run("cat /var/lib/update/pending"))
        got = summed(sh, "/lib/libhello.so")
        c.ok("...and the running system's copy is untouched", got is not None and got[1] == 100,
             f"sum {got}")
        sh.run("sync")
        kill(pidfile)
        pidfile = None
        sh, pidfile = launch(disk, tmp, "upd2", "e1000")
        c.ok("the next boot applies it before init",
             "applied 1 staged file at boot" in sh.run("dmesg"))
        got = summed(sh, "/lib/libhello.so")
        c.ok("...and sum agrees with the manifest",
             got is not None and got[0] == srv.crc("/lib/libhello.so"), f"sum {got}")
        c.ok("...and the pending list is gone",
             "no such file" in sh.run("cat /var/lib/update/pending"))

        # 5. the address
        sh.run(f"update --server {url}")
        c.ok("--server persists the address",
             f"server={url}" in sh.run("cat /etc/update.conf"))
        c.ok("...and records it as recent", f"recent: {url}" in sh.run("update --server"))
    finally:
        if pidfile and not args.keep:
            kill(pidfile)
        srv.srv.shutdown()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"update_test: guest and files kept in {tmp}")

    bad = c.failed()
    print(f"\nupdate_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")
    if args.positive_control:
        if bad:
            print("update_test: positive control OK -- verification is real")
            return 0
        print("update_test: POSITIVE CONTROL DID NOT FAIL -- a corrupt file would be installed")
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
