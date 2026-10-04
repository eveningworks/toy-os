#!/usr/bin/env python3
"""DOOM's game data and key sheet: the downloader, the launcher, F1.

userland/doom/: with no IWAD, DOOM shows a card that downloads one;
afterwards a launcher page with the keys; in game, F1 is a key sheet
over a paused level and F1 on the sheet is DOOM's own help.

THE DOWNLOAD NEVER LEAVES THIS MACHINE. The guest's /etc/doom.conf
gets `mirror=` pointed at a server this tool runs on loopback (QEMU's
10.0.2.2), serving data/doom/doom1.wad and, when it is there,
data/doom/freedoom-0.13.0.zip -- the latter behind a 302 to a URL over
a kilobyte long, which is what GitHub's release assets do.

THE CONTROL COMES FIRST: the server hands out a doom1.wad with one byte
flipped, and the download must be REFUSED on its SHA-256 and leave no
file behind. A downloader that skipped the check would sail through
every later assertion.

THE LICENCE comes before each download: id's must be agreed to, once
per text (a second Download does not ask), Freedoom's BSD is a notice;
both end up in /usr/share/licenses, Freedoom's as its own COPYING.txt.

Without data/doom/doom1.wad on the host only the card and the control
run, and the tool says what it skipped. `tools/fetch_wad.py` gets it;
`--freedoom` also gets the zip.

    python3 tools/vm.py start
    python3 tools/doom_data_test.py
    python3 tools/vm.py stop
"""

import argparse
import gzip
import http.server
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                  # noqa: E402
import port_guard                                # noqa: E402

try:
    from PIL import Image
except ImportError:
    print("doom_data_test: needs Pillow (pip install pillow)", file=sys.stderr)
    sys.exit(2)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAD = os.path.join(REPO, "data", "doom", "doom1.wad")
ZIP = os.path.join(REPO, "data", "doom", "freedoom-0.13.0.zip")
EXEC = "/bin/wm/apps/doom"
SHAREWARE_SHA = "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771"
LONG = "/release-assets/" + "a" * 1100 + "?sig=xyz"

checks = []


def check(name, ok, detail=""):
    checks.append((name, ok))
    print(("  PASS  " if ok else "  FAIL  ") + name + (f"   {detail}" if detail else ""))
    return ok


# --- the mirror ---------------------------------------------------------------

class Mirror:
    def __init__(self):
        self.tamper = False
        self.gzip = False   # Content-Encoding: gzip, as a CDN may answer
        self.hits = []
        mirror = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, data):
                self.send_response(200)
                if mirror.gzip:
                    data = gzip.compress(data)
                    self.send_header("Content-Encoding", "gzip")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                mirror.hits.append(self.path[:60])
                if self.path == "/doom1.wad":
                    if mirror.tamper or not os.path.exists(WAD):
                        data = bytearray(open(WAD, "rb").read() if os.path.exists(WAD) else b"IWAD" * 4096)
                        data[len(data) // 2] ^= 0x01
                        return self._send(bytes(data))
                    return self._send(open(WAD, "rb").read())
                if self.path == "/freedoom-0.13.0.zip" and os.path.exists(ZIP):
                    self.send_response(302)
                    self.send_header("Location", f"http://10.0.2.2:{mirror.port}{LONG}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path == LONG and os.path.exists(ZIP):
                    return self._send(open(ZIP, "rb").read())
                self.send_response(404)
                self.end_headers()

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


# --- the app, asked rather than guessed ------------------------------------

def logs(dbg):
    return [line for line in dbg.logs("doom:", clear=False) if "layout" not in line]


def wait_log(dbg, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hit = [line for line in logs(dbg) if needle in line]
        if hit:
            return hit[-1]
        time.sleep(0.4)
    return None


def layout(dbg):
    """The newest frame's controls: {name: [x, y, w, h, ...]}, content-relative."""
    lay = {}
    for line in dbg.logs("doom: layout ", clear=False):
        parts = line.split("doom: layout ", 1)[1].split()
        if parts[0] == "view":
            lay = {}
        lay[parts[0]] = parts[1:]
    return lay


def press(dbg, name, lay=None):
    lay = lay or layout(dbg)
    win = dbg.window("DOOM") or next((w for w in dbg.windows() if "DOOM" in w["title"]
                                      or "Freedoom" in w["title"]), None)
    v = lay.get(name)
    if not (win and v and len(v) >= 4):
        return False
    c = win["content"]
    x, y, w, h = (int(n) for n in v[:4])
    dbg.click(c["x"] + x + w // 2, c["y"] + y + h // 2)
    dbg.settle()
    return True


def kill_doom(dbg):
    for p in dbg.processes():
        if p.get("name") == "doom":
            dbg.send(f"sh kill {p['pid']}")
    time.sleep(1.5)


def spawn(dbg):
    dbg.logs("doom:", clear=True)
    dbg.send(f"gui spawn {EXEC}")


def files(dbg):
    return dbg.send("sh ls /usr/share/doom").split()


def row_named(lay, wad):
    for k, v in lay.items():
        if k.startswith("row") and v and v[-1] == wad:
            return k
    return None


def run(dbg, qmp, mirror, tmp):
    kill_doom(dbg)
    dbg.send("sh rm /usr/share/doom/doom1.wad")
    dbg.send("sh rm /usr/share/doom/freedoom1.wad")
    dbg.send("sh rm /usr/share/licenses/doom-shareware.txt")
    dbg.write_lines("/etc/doom.conf", [f"mirror=http://10.0.2.2:{mirror.port}"])

    # --- 1. no game data: the card ------------------------------------
    spawn(dbg)
    if not check("with no IWAD, DOOM opens the game-data card",
                 bool(wait_log(dbg, "game data card", 30))):
        return
    dbg.settle()
    lay = layout(dbg)
    have = sorted(v[-1] for k, v in lay.items() if k.startswith("row"))
    check("...offering the shareware and Freedoom", have == ["doom1.wad", "freedoom1.wad"], str(have))

    # --- 2. THE CONTROL: a tampered file is refused --------------------
    mirror.tamper = True
    row = row_named(lay, "doom1.wad")
    press(dbg, row, lay)
    press(dbg, "primary", lay)
    # id's licence comes first, and must be AGREED to (mockup L2).
    check("Download shows the shareware's licence, to agree to",
          bool(wait_log(dbg, "licence for doom1.wad shown (agree)", 10)))
    dbg.settle()
    lay = layout(dbg)
    check("...with Back and Agree and download", "agree" in lay and "back" in lay, str(list(lay)))
    press(dbg, "agree", lay)
    check("...and agreeing is remembered", bool(wait_log(dbg, "licence for doom1.wad agreed", 10)))
    said = wait_log(dbg, "download of doom1.wad failed", 60)
    check("a doom1.wad with one byte flipped is REFUSED", bool(said) and "SHA-256" in said, str(said))
    left = files(dbg)
    check("...and nothing is left in /usr/share/doom",
          not any(f.startswith("doom1.wad") for f in left), str(left))
    check("...and the card is back to try again", bool(wait_log(dbg, "game data card", 10)))
    mirror.tamper = False

    if not os.path.exists(WAD):
        print("\ndoom_data_test: no data/doom/doom1.wad on the host -- the download, the "
              "game, the sheet and the launcher were NOT checked (tools/fetch_wad.py)")
        kill_doom(dbg)
        return

    # --- 3. the real download, checked, and the game ---------------------
    # GZIPPED: Content-Length is then the COMPRESSED size, and a download
    # that compared it with the bytes it wrote threw a good file away.
    mirror.gzip = True
    lay = layout(dbg)
    press(dbg, row_named(lay, "doom1.wad"), lay)
    dbg.logs("doom:", clear=True)       # AFTER reading the layout: it is in the log
    press(dbg, "primary", lay)
    check("a second Download does not ask again",
          bool(wait_log(dbg, "licence for doom1.wad accepted before", 10)))
    check("the shareware downloads and its SHA-256 matches",
          bool(wait_log(dbg, "downloaded doom1.wad, checksum matches", 120)))
    check("...and the game starts on it", bool(wait_log(dbg, "doom: ready", 120)))
    mirror.gzip = False
    lic = dbg.send("sh cat /usr/share/licenses/doom-shareware.txt") or ""
    check("its licence is saved beside it", "id Software" in lic, lic.strip()[:80])
    sha = dbg.send("sh sum -a sha256 /usr/share/doom/doom1.wad") or ""
    check("the file on the guest's disk is id's 1.9 shareware", SHAREWARE_SHA in sha, sha.strip()[:80])
    deadline = time.time() + 30
    while time.time() < deadline and not dbg.window("DOOM Shareware"):
        time.sleep(0.5)
    win = dbg.window("DOOM Shareware")
    check("the game retitles its window from the WAD", win is not None)
    if not win:
        return

    # --- 4. F1: the sheet over a PAUSED level; F1 again: DOOM's help ----
    for k in ("esc", "ret", "ret", "ret", "ret"):   # menu, New Game, episode, skill
        qmp.send_key(k)
        time.sleep(0.8)
    time.sleep(3)
    c = win["content"]
    before = os.path.join(tmp, "doom_data_before.png")
    qmp.screenshot(before, stable=False)
    dbg.logs("doom:", clear=True)
    qmp.send_key("f1")
    shown = wait_log(dbg, "key sheet shown", 10)
    check("F1 in a level shows the key sheet", bool(shown))
    time.sleep(1.0)
    lay = layout(dbg)
    after = os.path.join(tmp, "doom_data_sheet.png")
    qmp.screenshot(after, stable=False)
    r = lay.get("resume")
    if check("...which reports its Resume button", bool(r), str(lay)):
        # The card's footer, left of Resume: the theme's window ground,
        # where the game drew before.
        px = (c["x"] + int(r[0]) - 30, c["y"] + int(r[1]) + int(r[3]) // 2)
        a = Image.open(before).convert("RGB").getpixel(px)
        b = Image.open(after).convert("RGB").getpixel(px)
        grey = all(abs(ch - b[0]) <= 6 for ch in b) and b[0] > 190
        check("...drawn: the card's light ground where the game was", grey and a != b,
              f"before={a} after={b}")
    qmp.send_key("f1")
    closed = wait_log(dbg, "key sheet closed", 10)
    check("F1 on the sheet closes it -- the level was paused under it",
          bool(closed) and "paused" in closed, str(closed))
    check("...and opens DOOM's own help", bool(wait_log(dbg, "menu open", 10)))
    qmp.send_key("esc")
    check("Esc then leaves DOOM's help", bool(wait_log(dbg, "menu closed", 10)))

    # --- 5. the launcher, next time -------------------------------------
    kill_doom(dbg)
    spawn(dbg)
    check("with an IWAD here, DOOM opens the launcher page",
          bool(wait_log(dbg, "launcher doom1.wad", 30)))
    dbg.settle()
    press(dbg, "start")
    check("...and Start starts the game", bool(wait_log(dbg, "doom: ready", 120)))

    # --- 6. Freedoom: a redirect over a kilobyte, a zip, a second IWAD ----
    if not os.path.exists(ZIP):
        print("\ndoom_data_test: no data/doom/freedoom-0.13.0.zip -- the redirect and the "
              "unzip were NOT checked (tools/fetch_wad.py --freedoom)")
    else:
        kill_doom(dbg)
        spawn(dbg)
        wait_log(dbg, "launcher", 30)
        dbg.settle()
        press(dbg, "change")
        wait_log(dbg, "game data card", 10)
        dbg.settle()
        lay = layout(dbg)
        press(dbg, row_named(lay, "freedoom1.wad"), lay)
        dbg.logs("doom:", clear=True)
        press(dbg, "primary", lay)
        check("Freedoom's licence is a notice, not an agreement",
              bool(wait_log(dbg, "licence for freedoom1.wad shown (notice)", 10)))
        dbg.settle()
        lay = layout(dbg)
        press(dbg, "agree", lay)
        got = wait_log(dbg, "downloaded freedoom1.wad, checksum matches", 600)
        check("Freedoom downloads through a 1 KB redirect, unzips and checks out", bool(got),
              str(logs(dbg)[-3:]))
        check("...the server saw the redirect followed", any(h.startswith("/release-assets/") for h in mirror.hits))
        deadline = time.time() + 60
        while time.time() < deadline and not dbg.window("Freedoom: Phase 1"):
            time.sleep(0.5)
        check("...and the game runs it", dbg.window("Freedoom: Phase 1") is not None)
        lic = dbg.send("sh cat /usr/share/licenses/freedoom.txt") or ""
        check("...and its own COPYING.txt is saved as its licence",
              "Redistribution and use" in lic, lic.strip()[:80])

    kill_doom(dbg)
    dbg.send("sh rm /usr/share/doom/doom1.wad")
    dbg.send("sh rm /usr/share/doom/freedoom1.wad")
    dbg.send("sh rm /usr/share/licenses/doom-shareware.txt")
    dbg.send("sh rm /usr/share/licenses/freedoom.txt")
    dbg.send("sh rm /etc/doom.conf")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "doom_data_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    mirror = Mirror()
    print(f"doom game data, from a mirror on loopback port {mirror.port}")
    try:
        with DebugConsole(args.sock) as dbg:
            dbg.settle()
            run(dbg, qmp, mirror, args.tmp)
    finally:
        mirror.close()
    bad = [n for n, ok in checks if not ok]
    print(f"\ndoom_data_test: {len(checks) - len(bad)} passed, {len(bad)} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
