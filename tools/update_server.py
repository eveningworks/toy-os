#!/usr/bin/env python3
"""Serve this checkout's build to toy-os machines running /bin/update.

    python3 tools/update_server.py                 # 0.0.0.0:8080
    python3 tools/update_server.py --port 8081 --bind 127.0.0.1
    python3 tools/update_server.py --print         # the manifest, and exit
    python3 tools/update_server.py --throttle 300  # 300 KiB/s: watch the progress bar

The PULL half of docs/update-design.md: a machine runs `update` (or the
System Update window) and fetches from here; nothing listens on the
machine. `remote.py flash` is the PUSH half and still the tool for a
machine with no network configuration yet.

WHAT IT SERVES
--------------
    GET /manifest            one line per file:  <crc32> <size> <path> [opts]
                             (<path> URL-quoted: some names have spaces)
    GET /files/<path>        that file, and ONLY a file the manifest names

The manifest is GENERATED from the staging tree `make iso` seeds
(seed/sync) on every request, never kept beside it -- a list maintained
by hand is a pointer somebody forgets to update. The trees and the
new-files-only rule for /etc and /home are `remote.py`'s USERLAND_TREES,
imported rather than copied, so the push and the pull agree about what a
machine's own files are. The kernel is offered twice, the ELF and the
gzipped image, and the client picks by what its GRUB can load.

A STALE STAGING TREE IS REFUSED PER REQUEST (503 with the reason), not
served: `make all` without `make iso` leaves seed/sync one build behind,
and a client would faithfully install the previous build and report
success. iso_guard's check, and its TOYOS_ALLOW_STALE_ISO bypass.

WHAT IT IS NOT
--------------
Not authenticated. The crc32 in the manifest proves a file arrived
intact, not who built it: anyone who can answer on this port is the
update server. Bind to a LAN you trust.
"""
import argparse
import datetime
import http.server
import os
import sys
import threading
import time
import urllib.parse
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_guard          # noqa: E402
from remote import USERLAND_TREES   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL_TARGET = "/boot/boot/kernel.bin"


class Manifest:
    """The manifest text and the file each served path maps to.

    crc32 is cached per (path, size, mtime), so regenerating on every
    request costs a stat per file after the first."""

    def __init__(self, staging, kernel=True):
        self.staging = staging
        self.kernel = kernel
        self._crc = {}
        self._lock = threading.Lock()

    def _crc32(self, full):
        st = os.stat(full)
        key = (full, st.st_size, st.st_mtime_ns)
        crc = self._crc.get(key)
        if crc is None:
            with open(full, "rb") as fh:
                crc = zlib.crc32(fh.read())
            self._crc[key] = crc
        return crc, st.st_size, st.st_mtime

    def build(self):
        """(text, {served path: local file}, newest mtime)"""
        with self._lock:
            lines, files, seen, newest = [], {}, set(), 0.0
            for sub, remote, new_only in USERLAND_TREES:
                local_dir = os.path.join(self.staging, sub)
                for root, _dirs, names in os.walk(local_dir):
                    for name in sorted(names):
                        full = os.path.join(root, name)
                        if os.path.islink(full):
                            continue
                        rel = os.path.relpath(full, local_dir).replace(os.sep, "/")
                        target = f"{remote}/{rel}"
                        if target in seen:          # etc/settings.d before etc: first wins
                            continue
                        seen.add(target)
                        crc, size, mtime = self._crc32(full)
                        newest = max(newest, mtime)
                        opts = " new-only" if new_only else ""
                        lines.append(f"{crc} {size} {_quote(target)}{opts}")
                        files[target] = full
            if self.kernel:
                self._add_kernel(lines, files)
            built = datetime.datetime.fromtimestamp(newest).strftime("%Y-%m-%d %H:%M")
            head = ["# toy-os update manifest", f"# version {_version()}",
                    f"# built {built}"]
            return "\n".join(head + sorted(lines, key=_line_path)) + "\n", files

    def _add_kernel(self, lines, files):
        elf = os.path.join(REPO, "build", "kernel.bin")
        media = os.path.join(REPO, "build", "kernel.media")
        if not os.path.isfile(elf):
            return
        crc, size, mtime = self._crc32(elf)
        lines.append(f"{crc} {size} {KERNEL_TARGET} kernel,src=/_kernel/kernel.bin")
        files["/_kernel/kernel.bin"] = elf
        # The gzipped image is made by `make iso`, not `make all`: one
        # older than the ELF is the PREVIOUS kernel, so it is left out.
        if os.path.isfile(media) and os.stat(media).st_mtime >= mtime:
            crc, size, _ = self._crc32(media)
            lines.append(f"{crc} {size} {KERNEL_TARGET} kernel-gz,src=/_kernel/kernel.media")
            files["/_kernel/kernel.media"] = media


# A manifest line is space-separated and some names have spaces in them
# (`group.System.Startup target`), so a path is written URL-quoted --
# which is also the form the client puts in its request.
def _quote(path):
    return urllib.parse.quote(path, safe="/")


def _line_path(line):
    return line.split(" ", 3)[2]


def _version():
    try:
        with open(os.path.join(REPO, "VERSION")) as fh:
            return fh.read().strip()
    except OSError:
        return "unknown"


def _stale_reason(staging):
    if os.environ.get(iso_guard.BYPASS_ENV):
        return None
    return iso_guard.check_staging_fresh(staging=staging)


def make_handler(manifest, throttle_kib=0):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "toy-os-update/1"

        def log_message(self, fmt, *args):      # the access log is ours, below
            pass

        def _say(self, status, size=None):
            sz = f"  {size} B" if size is not None else ""
            print(f"{self.client_address[0]:<15} GET {self.path:<40} {status}{sz}",
                  flush=True)

        def _send(self, status, body, ctype="text/plain"):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self._say(status, len(body))

        def do_GET(self):
            why = _stale_reason(manifest.staging)
            if why:
                print(f"update_server: REFUSING, staging is stale: {why}", flush=True)
                self._send(503, f"stale build on the server: {why}\n".encode())
                return
            text, files = manifest.build()
            if self.path == "/manifest":
                self._send(200, text.encode())
                return
            if not self.path.startswith("/files/"):
                self._send(404, b"not found\n")
                return
            local = files.get(urllib.parse.unquote(self.path[len("/files"):]))
            if not local:
                self._send(404, b"not in the manifest\n")
                return
            size = os.path.getsize(local)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Connection", "close")
            self.end_headers()
            step = 16384 if throttle_kib else 65536
            with open(local, "rb") as fh:
                while True:
                    chunk = fh.read(step)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    if throttle_kib:
                        time.sleep(len(chunk) / (throttle_kib * 1024.0))
            self._say(200, size)

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--staging", default=os.path.join(REPO, "seed", "sync"))
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--no-kernel", action="store_true",
                    help="leave the kernel out of the manifest")
    ap.add_argument("--print", action="store_true",
                    help="print the manifest and exit")
    ap.add_argument("--throttle", type=int, default=0, metavar="KIB",
                    help="send at most KIB KiB/s per file -- to watch progress on a fast link")
    args = ap.parse_args()

    if not os.path.isdir(args.staging):
        sys.exit(f"update_server: no {args.staging} -- run `make iso` first")
    manifest = Manifest(args.staging, kernel=not args.no_kernel)
    if args.print:
        sys.stdout.write(manifest.build()[0])
        return 0
    why = _stale_reason(args.staging)
    if why:
        print(f"update_server: WARNING, staging is stale ({why}); requests will be "
              "refused until `make iso` runs", file=sys.stderr)
    text, files = manifest.build()
    print(f"manifest: {len(files)} files from {os.path.relpath(args.staging, REPO)}/")
    srv = http.server.ThreadingHTTPServer((args.bind, args.port),
                                          make_handler(manifest, args.throttle))
    print(f"serving http://{args.bind}:{args.port}  (a QEMU guest reaches this host "
          f"as http://10.0.2.2:{args.port})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
