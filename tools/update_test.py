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
  7. STALE FILES: a manifest that stops listing a file removes it, once
     a record of the previous manifest exists -- unless it was edited
     here, which keeps it. A stale LIBRARY waits for the boot like a
     changed one ("-/lib/..." in the pending list).
  8. RELEASE NOTES: `update --check` prints the notes NEWER than this
     machine's build and none older -- cut first at the commit the guest
     was compiled from, then, once an install has recorded a manifest
     with a different `# commit`, at the RECORD's commit instead.
  6. THE KERNEL, served as a copy of this build's with one boot message
     changed so the new one can be told from the old: on the stock image,
     whose GRUB has `set timeout=0`, the update is REFUSED whole and
     /boot is untouched; with the timeout raised on the HOST (mtools, as
     fat32_test.py does) it installs, keeps `kernel.old`, and the next
     boot prints the changed message. Skipped without mtools.

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
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from net_test import launch, kill  # noqa: E402
from fat32_test import esp_window, mtools_at, mrun  # noqa: E402
import update_server  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

GATEWAY = "10.0.2.2"
# A message every boot prints, and a same-length twin: a kernel carrying
# the twin is a different kernel that boots identically and says so.
BOOT_MSG = b"toy-os: kernel heap initialized"
TWIN_MSG = b"toy-os: KERNEL heap initialized"


Check = Results


class Server:
    """update_server's handler, on a port the OS picks, with a switch
    that flips one byte of one file on its way out."""

    def __init__(self):
        self.manifest = update_server.Manifest(os.path.join(ROOT, "seed", "sync"))
        self.tamper = None
        self.hide = set()      # paths the served manifest leaves out
        self.notes = None      # a notes file to serve instead of the build's own
        self.commit = None     # a `# commit` to claim instead of the build's own
        base = update_server.make_handler({"": self.manifest})
        outer = self

        class Handler(base):
            def log_message(self, *a):
                pass

            def _say(self, *a):
                pass

            def do_GET(self):
                if outer.notes is not None and self.path == "/notes":
                    self._send(200, outer.notes.encode(), "text/plain")
                    return
                if (outer.hide or outer.commit) and self.path == "/manifest":
                    text, _ = outer.manifest.build()
                    keep = [ln for ln in text.splitlines()
                            if len(ln.split(" ")) < 3 or ln.split(" ")[2] not in outer.hide]
                    if outer.commit:
                        keep = [f"# commit {outer.commit}" if ln.startswith("# commit ") else ln
                                for ln in keep]
                    self._send(200, ("\n".join(keep) + "\n").encode(), "text/plain")
                    return
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


def patched_kernel(tmp):
    with open(os.path.join(ROOT, "build", "kernel.bin"), "rb") as fh:
        data = fh.read()
    if data.count(BOOT_MSG) != 1:
        return None
    out = os.path.join(tmp, "kernel.twin")
    with open(out, "wb") as fh:
        fh.write(data.replace(BOOT_MSG, TWIN_MSG))
    return out


def raise_grub_timeout(disk):
    """`set timeout=0` -> 1 in the image's grub.cfg, from the host."""
    start, _ = esp_window(disk)
    at = mtools_at(disk, start)
    r = mrun(["mtype", "-i", at, "::/boot/grub/grub.cfg"])
    if r.returncode or "set timeout=0" not in r.stdout:
        return False
    local = disk + ".grub.cfg"
    with open(local, "w") as fh:
        fh.write(r.stdout.replace("set timeout=0", "set timeout=1"))
    return mrun(["mcopy", "-o", "-i", at, local, "::/boot/grub/grub.cfg"]).returncode == 0


def kernel_checks(c, sh, pidfile, disk, tmp, srv, url):
    twin = patched_kernel(tmp)
    if not c.ok("a twin kernel can be made (one boot message changed)", twin):
        return sh, pidfile
    srv.manifest.kernel_elf, srv.manifest.kernel_media = twin, ""
    before = summed(sh, "/boot/boot/kernel.bin")
    out = sh.run(f"update --from {url}", timeout=600)
    c.ok("with no GRUB menu a kernel update is refused whole",
         "cannot update" in out and "nothing" in out.lower(), out.strip()[-200:])
    c.ok("...and /boot/boot/kernel.bin is untouched",
         before is not None and summed(sh, "/boot/boot/kernel.bin") == before)

    sh.run("sync")
    kill(pidfile)
    c.ok("the image's GRUB timeout can be raised from the host", raise_grub_timeout(disk))
    sh, pidfile = launch(disk, tmp, "upd3", "e1000")
    out = sh.run(f"update --from {url}", timeout=600)
    c.ok("with a menu the kernel installs", "kernel: installed" in out, out.strip()[-200:])
    c.ok("...keeping the running one as kernel.old", "kernel.old" in sh.run("ls /boot/boot"))
    sh.run("sync")
    kill(pidfile)
    sh, pidfile = launch(disk, tmp, "upd4", "e1000")
    c.ok("the next boot runs the NEW kernel",
         TWIN_MSG.decode() in sh.run("dmesg"))
    out = sh.run(f"update --from {url} --check", timeout=600)
    c.ok("...and is up to date against it", "up to date" in out, out.strip()[-300:])
    return sh, pidfile


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
        copy_disk(disk_src, disk)
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

        # 7. stale files: removed when unedited, kept when edited, and a
        # library's removal waits for the boot. The record exists by now:
        # every successful run above wrote one.
        c.ok("a successful run records the manifest it applied",
             "/bin/hello" in sh.run("cat /var/lib/update/installed"))
        edited = "/usr/share/doc/guide/kernel.md"
        sh.run(f"truncate -s 64 {edited}")
        srv.hide = {"/bin/hello", edited}
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("a file the manifest stopped listing is removed",
             "/bin/hello: removed" in out and "hello" not in sh.run("ls /bin"),
             out.strip()[-240:])
        c.ok("...but one edited here is kept, and said so",
             "changed on this machine -- kept" in out and summed(sh, edited) is not None,
             out.strip()[-240:])
        srv.hide = {"/lib/libhello.so"}
        out = sh.run(f"update --from {url}", timeout=600)
        c.ok("a stale library is staged for the boot, not removed under the system",
             "staged" in out and summed(sh, "/lib/libhello.so") is not None, out.strip()[-200:])
        c.ok("...as a `-` line in the pending list",
             "-/lib/libhello.so" in sh.run("cat /var/lib/update/pending"))
        sh.run("sync")
        kill(pidfile)
        pidfile = None
        sh, pidfile = launch(disk, tmp, "upd3", "e1000")
        c.ok("the next boot removes it", "removed 1" in sh.run("dmesg")
             and summed(sh, "/lib/libhello.so") is None)
        # Serving it again brings it back -- a library, so after a boot.
        srv.hide = set()
        out = sh.run(f"update --from {url}", timeout=600)
        if "restart" in out:
            sh.run("sync")
            kill(pidfile)
            pidfile = None
            sh, pidfile = launch(disk, tmp, "upd5", "e1000")
        c.ok("serving them again puts them back",
             all(summed(sh, f) is not None for f in ("/bin/hello", "/lib/libhello.so")),
             out.strip()[-200:])

        # 8. release notes, cut at this machine's build
        own = update_server._build_id().split("-")[0]   # git log never says -dirty
        recorded = "fffffff0"
        srv.notes = "\n".join([
            "# toy-os release notes",
            "aaaaaaa1 new Crash Reports lists past crashes.",
            "aaaaaaa2 internal System: HOOD-SYSTEM-LINE",
            f"{recorded} fixed RECORDED-BUILD-NOTE",
            "aaaaaaa3 -",
            "aaaaaaa5 internal Sound: HOOD-SOUND-LINE",
            f"{own} -",
            "aaaaaaa4 new OLDER-THAN-THIS-BUILD",
        ]) + "\n"
        sh.run("truncate -s 100 /bin/hello")
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("--check prints What's new with the newer notes",
             "What's new" in out and "Crash Reports lists past crashes" in out
             and "RECORDED-BUILD-NOTE" in out, out.strip()[-300:])
        # UNDER THE HOOD: the `internal` lines, folded under one summary
        # that counts them by area -- printed open, since a terminal
        # cannot fold -- and a `-` (not the OS's) is not mentioned at all.
        c.ok("...with the OS's unseen changes under one summary, by area",
             "Under the hood -- 2 changes you won't see: System 1, Sound 1" in out
             and "HOOD-SYSTEM-LINE" in out and "HOOD-SOUND-LINE" in out, out.strip()[-400:])
        c.ok("...and a change that is not the OS's not even counted",
             "no visible effect" not in out and "aaaaaaa3" not in out, out.strip()[-300:])
        c.ok("...and none at or older than the guest's own build",
             "OLDER-THAN-THIS-BUILD" not in out, out.strip()[-300:])
        srv.commit = recorded
        sh.run(f"update --from {url}", timeout=600)
        srv.commit = None
        c.ok("the record carries the manifest's commit",
             f"# commit {recorded}" in sh.run("cat /var/lib/update/installed"))
        sh.run("truncate -s 100 /bin/hello")
        srv.notes = srv.notes.replace(f"{recorded} fixed RECORDED-BUILD-NOTE",
                                      f"{recorded} -")
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("once recorded, the cut is at the RECORD's commit",
             "Crash Reports lists past crashes" in out and "Under the hood -- 1 change you "
             "won't see: System 1" in out and "HOOD-SOUND-LINE" not in out
             and "OLDER-THAN" not in out, out.strip()[-400:])
        sh.run(f"update --from {url}", timeout=600)
        srv.notes = None

        # 5. the address
        sh.run(f"update --server {url}")
        c.ok("--server persists the address",
             f"server={url}" in sh.run("cat /etc/update.conf"))
        c.ok("...and records it as recent", f"recent: {url}" in sh.run("update --server"))

        # 6. the kernel
        if not shutil.which("mtype") or not shutil.which("mcopy"):
            print("  skip  the kernel checks -- no mtools on the host")
        else:
            sh, pidfile = kernel_checks(c, sh, pidfile, disk, tmp, srv, url)
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
