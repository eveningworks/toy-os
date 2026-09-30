"""What every test tool here had written for itself: a results table, a
disk copy, and a small HTTP server on this machine.

    from harness import Results, copy_disk, BodyServer

    res = Results()
    res.check("the window opens", win is not None, f"win={win}")
    ...
    return res.finish("devmgr_test")      # "devmgr_test: 7 passed, 0 failed"

WHY ONE FILE. 118 tools defined their own result table, 43 spelled out
the same `cp` line and three their own `http.server`; a survey on
2026-09-30 found the tables in five storage shapes and a dozen print
formats for one idea. This is the part that does NOT differ between them
(dup_scan.py's rule: share what is identical, never a loop whose
differences become callbacks). A tool with a genuinely different table
-- `skip()`, a JSON report -- keeps its own.

`Results` is a superset of the shapes the tools used, so adopting it is
a one-line change: `.check()`/`.ok()` (returns the verdict), `.passes`
and `.fails` (names), `.rows` ((name, ok, detail)), `.failed()`. The
summary line `finish()` prints is the one `gui_regress.py` reads:
`<tool>: N passed, M failed`.
"""
import http.server
import ssl
import subprocess
import threading


class Results:
    def __init__(self):
        self.rows = []                 # (name, ok, detail), in order
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        ok = bool(ok)
        self.rows.append((name, ok, detail))
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""),
              flush=True)
        return ok

    ok = check

    def failed(self):
        return [r for r in self.rows if not r[1]]

    def finish(self, tool):
        """Print the summary line; 0 when something ran and all of it passed."""
        print(f"\n{tool}: {len(self.passes)} passed, {len(self.fails)} failed")
        return 0 if self.rows and not self.fails else 1


def copy_disk(src, dst, cwd=None):
    """A copy of a disk image to boot a test from, never the image itself.

    --reflink=auto: copy-on-write where the filesystem can (btrfs, XFS),
    a plain copy where it cannot -- never =always, which fails outright
    on ext4. --sparse=always: disk.img is megabytes of data in a 9 GB
    sparse file, and /tmp is often a tmpfs, where a filled-in copy costs
    9 GB of RAM (cp's default of `auto` usually gets this right; stating
    it means an edit cannot quietly lose it)."""
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, dst], check=True, cwd=cwd)
    return dst


class BodyServer:
    """A threaded HTTP(S) server on this machine answering every GET with
    one body (or `routes[path]`, 404 otherwise). A guest reaches it as
    SLIRP's 10.0.2.2; nothing leaves the machine.

    THREADED, and that is not an optimisation: a single-threaded server
    does a TLS handshake inside accept(), so one client that connects and
    walks away wedges every later connection."""

    def __init__(self, port=0, body=b"", routes=None, cert=None, key=None,
                 bind="0.0.0.0", ctype="text/plain"):
        routes = dict(routes or {})
        default = body

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def do_GET(self):
                data = routes.get(self.path, default if not routes else None)
                if data is None:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.routes = routes
        self.srv = http.server.ThreadingHTTPServer((bind, port), Handler)
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def shutdown(self):
        self.srv.shutdown()
        self.srv.server_close()
