#!/usr/bin/env python3
"""Serve toy-os builds to machines running /bin/update, on two channels.

    python3 tools/update_server.py                  # both channels, 0.0.0.0:8080
    python3 tools/update_server.py --publish        # this build -> `stable` (preflight-passed)
    python3 tools/update_server.py --list           # published builds; * = current
    python3 tools/update_server.py --promote NAME   # point `stable` at another one
    python3 tools/update_server.py --install-service   # run it under systemd --user
    python3 tools/update_server.py --print [--channel stable]
    python3 tools/update_server.py --throttle 300   # 300 KiB/s: watch the progress bar

The PULL half of docs/update-design.md: a machine runs `update` (or the
System Update window) and fetches from here; nothing listens on the
machine. `remote.py flash` is the PUSH half and still the tool for a
machine with no network configuration yet.

TWO CHANNELS -- A MACHINE'S `update.server` NAMES ONE
-----------------------------------------------------
    http://<host>:8080/dev      this checkout's seed/sync, LIVE: every
                                `make iso` is at once what a machine gets
    http://<host>:8080/stable   the published snapshot `current` points at,
                                under ~/.local/share/toy-os/updates

`dev` is what a VM wants while a build is being worked on. `stable` is
what a machine somebody USES wants: a build only arrives there when it is
published (after preflight, say), and a publish is a COPY, so rebuilding
the checkout never reaches it. `--publish` refuses a build preflight has
not passed on this exact tree (tools/preflight_stamp.py; `--force`
overrides), and `dev` answers 503 while a `make iso` is writing it
(build/.seeding).
apt's published repository and WSUS's approval step are the shape. The
last few snapshots are kept, so `--promote` is also the rollback.

Each channel serves
    GET <channel>/manifest      one line per file:  <crc32> <size> <path> [opts]
                                (<path> URL-quoted: some names have spaces)
    GET <channel>/files/<path>  that file, and ONLY a file the manifest names
    GET <channel>/notes         what changed, newest commit first (below);
                                404 from a build published before it existed

RELEASE NOTES ARE `Release-note:` TRAILERS, collected from `git log`
---------------------------------------------------------------------
A commit that changes something a person can see carries one or more

    Release-note: fixed: Force-quitting System Update could freeze the desktop.

with the kind `new`, `improved` or `fixed`, in the user's words (GitLab's
`Changelog:` trailer is the shape). The notes file is one line per
commit, newest first -- `<sha> <kind> <text>`, or `<sha> -` for a commit
with none -- and the CLIENT cuts it at its own build, as apt-listchanges
cuts a changelog at the installed version. A trailer with any other kind
is left out with a warning, never guessed at.

A commit that went out WITHOUT one gets it as a git note, which changes
no SHA (`git notes` is how Gerrit keeps its review data on commits):

    git notes --ref=release add -m "fixed: <text>" <sha>
    git push origin refs/notes/release

one `<kind>: <text>` per line, read the same way as the trailer.

The `dev` manifest is GENERATED from the staging tree `make iso` seeds on
every request, never kept beside it -- a list maintained by hand is a
pointer somebody forgets to update. The trees and the new-files-only rule
for /etc and /home are `remote.py`'s USERLAND_TREES, imported rather than
copied, so the push and the pull agree about what a machine's own files
are. The kernel is offered twice, the ELF and the gzipped image, and the
client picks by what its GRUB can load. A `stable` snapshot freezes that
manifest and the files it names at publish time.

A STALE STAGING TREE IS REFUSED (503 with the reason), on `dev` per
request and by `--publish` outright: `make all` without `make iso` leaves
seed/sync one build behind, and a client would faithfully install the
previous build and report success. iso_guard's check, and its
TOYOS_ALLOW_STALE_ISO bypass.

WHAT IT IS NOT
--------------
Not authenticated. The crc32 in the manifest proves a file arrived
intact, not who built it: anyone who can answer on this port is the
update server. Bind to a LAN you trust.
"""
import argparse
import datetime
import http.server
import json
import re
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso_guard          # noqa: E402
import preflight_stamp    # noqa: E402
from remote import USERLAND_TREES   # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERNEL_TARGET = "/boot/boot/kernel.bin"
STORE = os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"),
                     "toy-os", "updates")
SEEDING = os.path.join(REPO, "build", ".seeding")   # the Makefile's seed step, while it runs
UNIT_NAME = "toy-os-update.service"
UNIT_TEMPLATE = os.path.join(REPO, "tools", "systemd", UNIT_NAME)
VERSION_H = os.path.join(REPO, "kernel", "include", "api", "version.h")
NOTE_KINDS = ("new", "improved", "fixed")
NOTES_COMMITS = 200       # how far back a machine can be and still get "since your build"
NOTES_REF = "refs/notes/release"   # notes added AFTER the commit, one `<kind>: <text>` per line


class Unavailable(Exception):
    """A channel with nothing to serve right now; the text says why."""


class Manifest:
    """The manifest text and the file each served path maps to.

    crc32 is cached per (path, size, mtime), so regenerating on every
    request costs a stat per file after the first."""

    def __init__(self, staging, kernel=True, kernel_elf=None, kernel_media=None):
        self.staging = staging
        self.kernel = kernel
        # Overridable so a test can serve a DIFFERENT kernel; "" leaves
        # the gzipped one out.
        self.kernel_elf = kernel_elf or os.path.join(REPO, "build", "kernel.bin")
        self.kernel_media = (os.path.join(REPO, "build", "kernel.media")
                             if kernel_media is None else kernel_media)
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
                    f"# built {built}", f"# commit {_build_id()}"]
            return "\n".join(head + sorted(lines, key=_line_path)) + "\n", files

    def snapshot(self):
        if os.path.exists(SEEDING) and os.path.realpath(self.staging) == os.path.realpath(
                os.path.join(REPO, "seed", "sync")):
            raise Unavailable("a `make iso` is writing this build right now, or one failed "
                              "part-way -- try again when it has finished")
        why = _stale_reason(self.staging)
        if why:
            raise Unavailable(f"stale build on the server: {why}")
        return self.build()

    def notes(self):
        return release_notes(_build_id())

    def _add_kernel(self, lines, files):
        elf, media = self.kernel_elf, self.kernel_media
        if not os.path.isfile(elf):
            return
        crc, size, mtime = self._crc32(elf)
        lines.append(f"{crc} {size} {KERNEL_TARGET} kernel,src=/_kernel/kernel.bin")
        files["/_kernel/kernel.bin"] = elf
        # The gzipped image is made by `make iso`, not `make all`: one
        # older than the ELF is the PREVIOUS kernel, so it is left out.
        if media and os.path.isfile(media) and os.stat(media).st_mtime >= mtime:
            crc, size, _ = self._crc32(media)
            lines.append(f"{crc} {size} {KERNEL_TARGET} kernel-gz,src=/_kernel/kernel.media")
            files["/_kernel/kernel.media"] = media


class Published:
    """The `stable` channel: whatever snapshot `<store>/current` points at.

    Re-read when the link moves, so a --publish or --promote takes effect
    without restarting the service."""

    def __init__(self, store):
        self.store = store
        self._cached = (None, None, None)

    def current(self):
        link = os.path.join(self.store, "current")
        return os.path.realpath(link) if os.path.islink(link) else None

    def snapshot(self):
        cur = self.current()
        if not cur or not os.path.isfile(os.path.join(cur, "manifest")):
            raise Unavailable("nothing published yet: run `update_server.py --publish`")
        if self._cached[0] != cur:
            with open(os.path.join(cur, "manifest")) as fh:
                text = fh.read()
            with open(os.path.join(cur, "index.json")) as fh:
                index = json.load(fh)
            files = {key: os.path.join(cur, "files", rel) for key, rel in index.items()}
            self._cached = (cur, text, files)
        return self._cached[1], self._cached[2]

    def notes(self):
        cur = self.current()
        try:
            with open(os.path.join(cur or "", "notes")) as fh:
                return fh.read()
        except OSError:
            return None


def _commit():
    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, check=True,
                             capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=REPO).returncode != 0
        return sha + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "nogit"


def _build_id():
    """The commit the BUILD came from, as gen_version.sh stamped it --
    not HEAD now, which may be ahead of what `make iso` staged."""
    try:
        with open(VERSION_H) as fh:
            m = re.search(r'#define TOYOS_BUILD_ID "([^"]*)"', fh.read())
        if m:
            return m.group(1)
    except OSError:
        pass
    return _commit()


def _uncommitted_reason():
    """Why the build is not exactly HEAD, or None."""
    built = _build_id()
    head = _commit()
    if built.endswith("-dirty"):
        return f"the build is of uncommitted changes ({built})"
    if head.split("-")[0] != built:
        return f"the build is of {built}, but HEAD is {head.split('-')[0]} -- rebuild"
    if head.endswith("-dirty"):
        return "the working tree has changes the build does not include"
    return None


def release_notes(build_id, limit=NOTES_COMMITS):
    """The notes file for a build: its last `limit` commits, newest first."""
    rev = build_id.split("-")[0]
    lines = ["# toy-os release notes", f"# commit {build_id}"]
    try:
        out = subprocess.run(
            ["git", "log", f"-n{limit}", "--abbrev=8", f"--notes={NOTES_REF}",
             "--format=%h%x1f%(trailers:key=Release-note,valueonly,separator=%x1e)%x1f%N%x1d",
             rev],
            cwd=REPO, check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return "\n".join(lines) + "\n"
    for rec in out.split("\x1d"):
        sha, _, rest = rec.strip("\n").partition("\x1f")
        if not sha:
            continue
        trailers, _, noted = rest.partition("\x1f")
        said = 0
        for v in trailers.split("\x1e") + noted.splitlines():
            v = " ".join(v.split())          # a folded trailer is one line
            if v.lower().startswith("release-note:"):
                v = v[len("release-note:"):].strip()
            if not v:
                continue
            kind, sep, text = v.partition(":")
            kind = kind.strip().lower()
            if not sep or kind not in NOTE_KINDS or not text.strip():
                print(f"update_server: {sha}: Release-note {v!r} is not "
                      f"`<{'|'.join(NOTE_KINDS)}>: <text>` -- left out", file=sys.stderr)
                continue
            lines.append(f"{sha} {kind} {text.strip()[:300]}")
            said += 1
        if not said:
            lines.append(f"{sha} -")
    return "\n".join(lines) + "\n"


def _set_current(store, name):
    """Point `current` at builds/<name> ATOMICALLY: a new link beside it,
    renamed over it, so the server never finds no link at all."""
    tmp = os.path.join(store, ".current.new")
    if os.path.lexists(tmp):
        os.remove(tmp)
    os.symlink(os.path.join("builds", name), tmp)
    os.replace(tmp, os.path.join(store, "current"))


def publish(staging, store, keep, force=False):
    why = _stale_reason(staging)
    if why:
        sys.exit(f"update_server: REFUSING to publish a stale build: {why}\n"
                 "  run `make iso` first")
    # A RELEASE IS A COMMIT. The snapshot's `# commit` and its notes come
    # from the id stamped into the BUILD, so a build of uncommitted work
    # (`-dirty`) or of an earlier HEAD would ship notes that miss the
    # change being published -- cargo publish's dirty-tree refusal.
    why = _uncommitted_reason()
    if why and not force:
        sys.exit(f"update_server: REFUSING to publish: {why}\n"
                 "  The order is: commit, then tools/preflight.sh (it rebuilds with the\n"
                 "  commit's id), then --publish.  (--force publishes anyway)")
    if why:
        print(f"update_server: publishing a build that is not a commit ({why})")
    # A RELEASE IS A TESTED BUILD. preflight stamps the tree it passed on.
    why = preflight_stamp.why_not()
    if why and not force:
        sys.exit(f"update_server: REFUSING to publish: {why}\n"
                 "  (--force publishes anyway)")
    if why:
        print(f"update_server: publishing WITHOUT a preflight pass ({why})")
    text, files = Manifest(staging).build()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}-{_commit()}"
    builds = os.path.join(store, "builds")
    os.makedirs(builds, exist_ok=True)
    part = os.path.join(builds, f".{name}.partial")
    shutil.rmtree(part, ignore_errors=True)
    index = {}
    for key, local in files.items():
        rel = key.lstrip("/")
        dst = os.path.join(part, "files", rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(local, dst)
        index[key] = rel
    with open(os.path.join(part, "manifest"), "w") as fh:
        fh.write(text)
    with open(os.path.join(part, "notes"), "w") as fh:
        fh.write(release_notes(_build_id()))
    with open(os.path.join(part, "index.json"), "w") as fh:
        json.dump(index, fh)
    os.rename(part, os.path.join(builds, name))    # complete, or not there at all
    _set_current(store, name)
    size = sum(os.path.getsize(p) for p in files.values())
    print(f"published {name}: {len(files)} files, {size // (1024 * 1024)} MiB -> stable")
    names = sorted(n for n in os.listdir(builds) if not n.startswith("."))
    for old in names[:-keep] if keep > 0 else []:
        if old != name:
            shutil.rmtree(os.path.join(builds, old))
            print(f"pruned {old}")
    return 0


def list_builds(store):
    builds = os.path.join(store, "builds")
    cur = Published(store).current()
    names = sorted(n for n in os.listdir(builds) if not n.startswith(".")) \
        if os.path.isdir(builds) else []
    if not names:
        print(f"nothing published in {store}")
    for n in names:
        path = os.path.join(builds, n)
        info = ""
        try:
            with open(os.path.join(path, "manifest")) as fh:
                hdr = [ln[2:] for ln in fh.read(512).splitlines() if ln.startswith("# ")]
            info = ", ".join(h for h in hdr if h.startswith(("version", "built")))
        except OSError:
            pass
        print(f"{'*' if cur == os.path.realpath(path) else ' '} {n}   {info}")
    return 0


def promote(store, name):
    if not os.path.isfile(os.path.join(store, "builds", name, "manifest")):
        sys.exit(f"update_server: no published build called {name} (see --list)")
    _set_current(store, name)
    print(f"stable -> {name}")
    return 0


def install_service(args):
    """Fill in the tracked unit template and enable it for this user."""
    with open(UNIT_TEMPLATE) as fh:
        unit = fh.read()
    extra = f" --port {args.port} --bind {args.bind}"
    if args.store != STORE:
        extra += f" --store {args.store}"
    unit = unit.replace("@PYTHON@", sys.executable).replace("@REPO@", REPO) \
               .replace("@ARGS@", extra)
    dest_dir = os.path.expanduser("~/.config/systemd/user")
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, UNIT_NAME)
    with open(dest, "w") as fh:
        fh.write(unit)
    print(f"wrote {dest}")
    for cmd in (["systemctl", "--user", "daemon-reload"],
                ["systemctl", "--user", "enable", "--now", UNIT_NAME],
                ["systemctl", "--user", "restart", UNIT_NAME]):
        subprocess.run(cmd, check=True)
    print(f"running: `systemctl --user status {UNIT_NAME}`, "
          f"`journalctl --user -u {UNIT_NAME}` for the access log")
    return 0


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


def make_handler(channels, throttle_kib=0):
    """`channels` maps a URL prefix ("/dev", "/stable", or "" for a lone
    source at the root) to anything with snapshot() -> (text, files)."""
    prefixes = sorted(channels, key=len, reverse=True)

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "toy-os-update/2"

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

        def _index(self):
            lines = ["toy-os update server. Channels:"]
            for p in prefixes:
                try:
                    text, files = channels[p].snapshot()
                    ver = next((ln[2:] for ln in text.splitlines()
                                if ln.startswith("# built")), "")
                    lines.append(f"  {p or '/'}  {len(files)} files, {ver}")
                except Unavailable as e:
                    lines.append(f"  {p or '/'}  unavailable: {e}")
            self._send(200, ("\n".join(lines) + "\n").encode())

        def do_GET(self):
            prefix = next((p for p in prefixes if self.path in (p + "/manifest", p + "/notes")
                           or self.path.startswith(p + "/files/")), None)
            if prefix is None:
                if self.path in ("/", ""):
                    self._index()
                else:
                    self._send(404, ("not found -- the server address names a channel: "
                                     + " or ".join(p + "/" for p in prefixes if p)
                                     + "\n").encode())
                return
            try:
                text, files = channels[prefix].snapshot()
            except Unavailable as e:
                print(f"update_server: {prefix or '/'} unavailable: {e}", flush=True)
                self._send(503, f"{e}\n".encode())
                return
            rest = self.path[len(prefix):]
            if rest == "/manifest":
                self._send(200, text.encode())
                return
            if rest == "/notes":
                notes = getattr(channels[prefix], "notes", lambda: None)()
                if notes is None:
                    self._send(404, b"no release notes for this build\n")
                else:
                    self._send(200, notes.encode())
                return
            local = files.get(urllib.parse.unquote(rest[len("/files"):]))
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
    ap.add_argument("--staging", default=os.path.join(REPO, "seed", "sync"),
                    help="the dev channel's tree (default: this checkout's seed/sync)")
    ap.add_argument("--store", default=STORE, help=f"published builds (default {STORE})")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--no-kernel", action="store_true",
                    help="leave the kernel out of the dev manifest")
    ap.add_argument("--print", action="store_true", help="print a manifest and exit")
    ap.add_argument("--channel", choices=("dev", "stable"), default="dev",
                    help="which one --print shows")
    ap.add_argument("--publish", action="store_true",
                    help="snapshot the current build and make it `stable`")
    ap.add_argument("--force", action="store_true",
                    help="with --publish: publish a build preflight has not passed")
    ap.add_argument("--keep", type=int, default=5, metavar="N",
                    help="published builds to keep (default 5)")
    ap.add_argument("--list", action="store_true", help="list published builds")
    ap.add_argument("--promote", metavar="NAME", help="point `stable` at a published build")
    ap.add_argument("--install-service", action="store_true",
                    help=f"install and start {UNIT_NAME} under systemd --user")
    ap.add_argument("--throttle", type=int, default=0, metavar="KIB",
                    help="send at most KIB KiB/s per file -- to watch progress on a fast link")
    args = ap.parse_args()

    if args.install_service:
        return install_service(args)
    if args.list:
        return list_builds(args.store)
    if args.promote:
        return promote(args.store, args.promote)
    if not os.path.isdir(args.staging):
        sys.exit(f"update_server: no {args.staging} -- run `make iso` first")
    if args.publish:
        return publish(args.staging, args.store, args.keep, args.force)

    dev = Manifest(args.staging, kernel=not args.no_kernel)
    stable = Published(args.store)
    if args.print:
        try:
            sys.stdout.write((dev if args.channel == "dev" else stable).snapshot()[0])
        except Unavailable as e:
            sys.exit(f"update_server: {args.channel}: {e}")
        return 0
    for name, src in (("dev", dev), ("stable", stable)):
        try:
            text, files = src.snapshot()
            print(f"{name}: {len(files)} files")
        except Unavailable as e:
            print(f"{name}: unavailable for now -- {e}")
    srv = http.server.ThreadingHTTPServer(
        (args.bind, args.port), make_handler({"/dev": dev, "/stable": stable}, args.throttle))
    print(f"serving http://{args.bind}:{args.port}/dev and /stable  (a QEMU guest reaches "
          f"this host as 10.0.2.2)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
