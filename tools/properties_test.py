#!/usr/bin/env python3
"""Properties: the facts it shows, the sections it opens, and the edits it makes.

WHAT IT CHECKS, and what a broken version would still pass:

  - it opens on a picture and REPORTS its facts (`properties: row ...`),
    and the hero is DRAWN: the stage behind the picture is not the page's
    grey, and the picture area is not flat -- a window that logged rows and
    painted a blank hero passes every log check;
  - a section header opens its section (the window reports it open and
    the permission boxes get a place);
  - clearing Others > Read changes the file's mode as `stat` -- an
    independent reader -- sees it;
  - Compute SHA-256 yields the digest `sum -a sha256` prints for the file;
  - choosing another app under Opens with writes /etc/mimeapps.conf;
  - typing a new name and Enter renames the file (`ls` sees it) and the
    window's title follows;
  - a folder's Contains settles on the true recursive count.

Geometry is the app's own report (`properties: layout ...` and
`properties: section ...`), never re-derived here.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard  # noqa: E402
from harness import Results  # noqa: E402

PROPS = "/bin/wm/apps/properties"
PIC = "/var/tmp/props_test.jpg"
NEW = "props_renamed.jpg"
DIR = "/var/tmp/props_dir"
PAGE = (236, 236, 236)   # utheme's panel_bg
K_DOWN, K_ENTER, K_SELALL = "0xf781", "0x0a", "0x01"

_res = Results()
check = _res.check


class Layout:
    def __init__(self):
        self.rect, self.sec, self.rows = {}, {}, {}

    def read(self, dbg):
        for line in dbg.logs("properties:", clear=True):
            m = re.search(r"properties: layout (\S+) (-?\d+) (-?\d+) (\d+) (\d+)$", line)
            if m:
                # Each frame reports what is shown; `info` comes first, so
                # it starts a fresh picture -- a hidden control or a header
                # scrolled away must not linger at its old place.
                if m.group(1) == "info":
                    self.rect, self.sec = {}, {}
                self.rect[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
            m = re.search(r"properties: section (.+?) (\d+) (\d+) (\d+) (\d+) (\d)$", line)
            if m:
                self.sec[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
            m = re.search(r"properties: row ([^=]+)=(.*)$", line)
            if m:
                self.rows[m.group(1)] = m.group(2).strip()
        return self


def wait(dbg, lay, ok, timeout=6.0):
    deadline = time.time() + timeout
    lay.read(dbg)
    while not ok(lay) and time.time() < deadline:
        time.sleep(0.2)
        lay.read(dbg)
    return lay


def window(dbg, part):
    wins = [w for w in dbg.windows() if part in w["title"] and "Properties" in w["title"]]
    return wins[-1] if wins else None


def click(dbg, win, rect, dx=None):
    c = win["content"]
    x, y, w, h = rect[:4]
    dbg.send("gui click %d %d" % (c["x"] + x + (w // 2 if dx is None else dx), c["y"] + y + h // 2))
    dbg.settle(0.6)


def toggle(dbg, win, lay, title, want_open):
    s = lay.sec.get(title)
    if not s or s[4] == want_open:
        return lay
    click(dbg, win, s, dx=40)
    return wait(dbg, lay, lambda l: l.sec.get(title, (0, 0, 0, 0, -1))[4] == want_open)


def close_all(dbg):
    for w in sorted(dbg.windows(), key=lambda w: -w["z"]):
        if "Properties" in w["title"]:
            dbg.send(f"gui close {w['z']}")
            time.sleep(0.3)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "properties_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    print("properties (lib/ufileinfo.h, ui/uui_fileinfo.h)")
    close_all(dbg)
    dbg.send(f"sh rm {PIC}")
    dbg.send(f"sh rm /var/tmp/{NEW}")
    dbg.send(f"sh cp /usr/share/wallpapers/aurora.jpg {PIC}")

    dbg.logs("properties:", clear=True)
    win = dbg.spawn(f"{PROPS} {PIC}", "props_test.jpg Properties")
    lay = wait(dbg, Layout(), lambda l: "Inode" in l.rows and "General" in l.sec and "name" in l.rect)
    if not check("Properties opens on a picture and reports its facts",
                 win and lay.rows.get("Name") == "props_test.jpg" and lay.rows.get("Type") == "JPEG image"
                 and "bytes" in lay.rows.get("Size", ""), f"rows={lay.rows}"):
        return finish()

    # DRAWN: the hero's stage is not the page grey; the picture area is
    # not flat. The thumbnail arrives from a worker, so give it a moment.
    time.sleep(1.5)
    dbg.settle(0.5)
    win = window(dbg, "props_test.jpg")
    from PIL import Image
    shot = os.path.join(args.tmp, f"props_{os.getpid()}.png")
    qmp.screenshot(shot)
    im = Image.open(shot).convert("RGB")
    c = win["content"]
    gy = lay.sec["General"][1]
    stage_px = im.getpixel((c["x"] + 6, c["y"] + 6))
    page_px = im.getpixel((c["x"] + c["w"] - 12, c["y"] + gy + lay.sec["General"][3] + 30))
    band = [im.getpixel((c["x"] + c["w"] // 2 + dx, c["y"] + gy // 3)) for dx in range(-60, 61, 6)]
    spread = max(sum(p) for p in band) - min(sum(p) for p in band)
    check("the hero is drawn: the stage is not the page's grey", stage_px != PAGE and sum(stage_px) < 400,
          f"stage {stage_px}")
    check("...the page below it is", page_px == PAGE, f"page {page_px}")
    check("...and the picture is in it (not flat)", spread > 30, f"brightness spread {spread}")

    # A section opens.
    lay = toggle(dbg, win, lay, "Image", 0)
    lay = toggle(dbg, win, lay, "Opens with", 0)
    if "Permissions" not in lay.sec:   # below the fold on a short screen: the wheel, as for Checksum
        dbg.warp_cursor(qmp, c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
        qmp.wheel("down", notches=10, delay=0.02)
        lay = wait(dbg, lay, lambda l: "Permissions" in l.sec)
    lay = toggle(dbg, win, lay, "Permissions", 1)
    lay = wait(dbg, lay, lambda l: "perm6" in l.rect)
    check("a section header opens its section", lay.sec.get("Permissions", (0,) * 5)[4] == 1 and "perm6" in lay.rect,
          f"sec={lay.sec.get('Permissions')} perm6={lay.rect.get('perm6')}")

    before = re.search(r"mode:\s+(\d+)", dbg.send(f"sh stat {PIC}"))
    if "perm6" in lay.rect:
        click(dbg, win, lay.rect["perm6"], dx=6)
    time.sleep(0.5)
    after = re.search(r"mode:\s+(\d+)", dbg.send(f"sh stat {PIC}"))
    b, a = (before.group(1) if before else "?"), (after.group(1) if after else "?")
    check("clearing Others > Read changes the mode (stat agrees)",
          before and after and int(b, 8) & 0o4 and not int(a, 8) & 0o4 and (int(b, 8) & ~0o4) == int(a, 8),
          f"{b} -> {a}")

    # The checksum, against sum. Opened where it sits -- at the window's
    # foot, Image and Opens with open above it -- because a slot partly
    # out of view once hid its controls entirely until a scroll.
    lay = toggle(dbg, win, lay, "Permissions", 0)
    lay = toggle(dbg, win, lay, "Image", 1)
    lay = toggle(dbg, win, lay, "Opens with", 1)
    if "Checksum" not in lay.sec:      # below the fold: the wheel, with the real pointer
        dbg.warp_cursor(qmp, c["x"] + c["w"] // 2, c["y"] + c["h"] // 2)
        qmp.wheel("down", notches=10, delay=0.02)
        lay = wait(dbg, lay, lambda l: "Checksum" in l.sec)
    lay = toggle(dbg, win, lay, "Checksum", 1)
    lay = wait(dbg, lay, lambda l: "sum" in l.rect and "compare" in l.rect)
    check("opening Checksum at the foot shows its button and compare field at once",
          "sum" in lay.rect and "compare" in lay.rect, f"rects={sorted(lay.rect)}")
    want = (dbg.send(f"sh sum -a sha256 {PIC}").split() or ["?"])[0]
    # The expected sum typed BEFORE computing, as one pastes it from a site.
    if "compare" in lay.rect:
        click(dbg, win, lay.rect["compare"])
        for ch in want:
            dbg.send("gui key 0x%02x" % ord(ch))
        dbg.settle(0.5)
    if "sum" in lay.rect:
        click(dbg, win, lay.rect["sum"])
    lay = wait(dbg, lay, lambda l: "SHA-256" in l.rows and l.rows.get("Compare") == "match", 8.0)
    check("Compute SHA-256 gives the digest `sum -a sha256` prints",
          lay.rows.get("SHA-256") == want and len(want) == 64, f"got {lay.rows.get('SHA-256')} want {want}")
    check("...and a sum typed before computing is compared: match",
          lay.rows.get("Compare") == "match", f"Compare={lay.rows.get('Compare')}")
    # Still on screen once computed, with no scroll: a slot that grew on
    # Compute once took its whole contents out of view.
    lay = wait(dbg, lay, lambda l: "compare" in l.rect, 3.0)
    check("...with the compare field still shown after Compute, no scroll needed",
          "compare" in lay.rect, f"rects={sorted(lay.rect)}")

    # Opens with: the next app in the list, into /etc/mimeapps.conf.
    lay = toggle(dbg, win, lay, "Checksum", 0)
    lay = toggle(dbg, win, lay, "Opens with", 1)
    lay = wait(dbg, lay, lambda l: "opens" in l.rect)
    was_opens = lay.rows.get("Opens with")
    if "opens" in lay.rect:
        click(dbg, win, lay.rect["opens"])
        dbg.key(K_DOWN)
        dbg.key(K_ENTER)
    lay = wait(dbg, lay, lambda l: l.rows.get("Opens with") != was_opens, 4.0)
    conf = dbg.send("sh cat /etc/mimeapps.conf")
    check("choosing another app writes /etc/mimeapps.conf",
          lay.rows.get("Opens with") != was_opens and re.search(r"^\.jpg=\S+", conf, re.M) is not None,
          f"opens {was_opens} -> {lay.rows.get('Opens with')}; conf: {conf.strip()[-160:]}")
    dbg.send("sh open -s .jpg imgview")   # put the type back

    # Rename.
    lay = toggle(dbg, win, lay, "Opens with", 0)
    lay = toggle(dbg, win, lay, "General", 1)
    lay = wait(dbg, lay, lambda l: "name" in l.rect)
    if "name" in lay.rect:
        click(dbg, win, lay.rect["name"])
        dbg.key(K_SELALL)
        for ch in NEW:
            dbg.send("gui key 0x%02x" % ord(ch))
        dbg.key(K_ENTER)
    time.sleep(0.8)
    listing = dbg.send("sh ls /var/tmp")
    titles = [w["title"] for w in dbg.windows()]
    check("typing a name and Enter renames the file (ls agrees)",
          NEW in listing and "props_test.jpg" not in listing, listing.strip()[-200:])
    check("...and the window's title follows", f"{NEW} Properties" in titles, f"{titles}")
    close_all(dbg)
    dbg.send(f"sh rm /var/tmp/{NEW}")

    # A folder settles on its recursive count.
    # Wallpapers, not /etc files: a fresh image is not guaranteed to have
    # written its config yet, and a failed cp made an empty folder.
    for cmd in (f"sh mkdir {DIR}", f"sh mkdir {DIR}/sub", f"sh cp /usr/share/wallpapers/dusk.jpg {DIR}/a.jpg",
                f"sh cp /usr/share/wallpapers/tide.jpg {DIR}/sub/b.jpg"):
        dbg.send(cmd)
    dbg.logs("properties:", clear=True)
    dbg.spawn(f"{PROPS} {DIR}", "props_dir Properties")
    lay = wait(dbg, Layout(), lambda l: "Contains" in l.rows and "counting" not in l.rows["Contains"], 10.0)
    check("a folder's Contains settles on the recursive count",
          lay.rows.get("Contains") == "2 files, 1 folder" and lay.rows.get("Type") == "Folder",
          f"rows={lay.rows}")
    close_all(dbg)
    for cmd in (f"sh rm {DIR}/sub/b.jpg", f"sh rm {DIR}/a.jpg", f"sh rm {DIR}/sub", f"sh rm {DIR}"):
        dbg.send(cmd)
    return finish()


def finish():
    return _res.finish("properties_test")


if __name__ == "__main__":
    sys.exit(main())
