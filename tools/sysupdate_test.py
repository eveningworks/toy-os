#!/usr/bin/env python3
"""System Update: it finds what changed, shows the install, and installs it.

WHAT IT CHECKS, and what a broken version would still pass:

  - the window opens, checks by itself and lands on "updates available"
    with the TWO files damaged beforehand -- a window that reported a
    view and counted nothing would fail on the count;
  - What's new is the page it opens on, with the notes NEWER than the
    guest's build counted (a cut in the wrong place changes the count)
    and DRAWN -- more text rows than the one-line "no notes" placeholder
    leaves, which `--positive-control` serves instead;
  - the OS's unseen changes sit under ONE folded "Under the hood" row,
    shut: clicking the row the widget reports opens it (its bit flips in
    the layout log), and scrolled to the end the row has RISEN with
    lines of text drawn below it -- a pane that is not taller can only
    have scrolled if the fold opened;
  - the Files tab, clicked at the slot the strip reports, shows the list;
  - the file list is DRAWN: ink in the table's rect, not only rows
    reported;
  - mid-install the overall bar is PART-filled, read from the pixels in
    its reported rect, and one row is tinted -- the progress the app
    exists to show. The server is throttled so there is a middle to see;
    a bar that jumped from empty to full would fail here;
  - it ends on "installed" and the files are right on disk, read by
    `sum` against the manifest -- an independent reader, not the app;
  - the bar ends full, and What's new still shows the notes afterwards.

The server is tools/update_server.py's handler in this process, on an
ephemeral loopback port the guest reaches as 10.0.2.2: nothing leaves
this machine.
"""
import http.server
import os
import re
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
import update_server                            # noqa: E402
from harness import Results  # noqa: E402

APP = "/bin/wm/system/sysupdate"
TITLE = "System Update"
# Not a library (that would wait for a restart) and not running (a
# desktop that restarted mid-test would find it truncated).
DAMAGE = ("/bin/hello", "/install/kernel.bin")
V_AVAILABLE, V_INSTALLED = 1, 4
NOTES = 2, 2, 1       # visible notes, under-the-hood ones, and not-the-OS commits
                      # the fixture puts above the guest's build



_res = Results()
check = _res.check
checks = _res.rows


def finish():
    return _res.finish("sysupdate_test")


class Fixture(update_server.Manifest):
    """The build's own manifest, with notes cut by the guest's build."""
    plain = False

    def notes(self):
        if self.plain:
            return None
        own = update_server._build_id().split("-")[0]   # git log never says -dirty
        return "\n".join(["# toy-os release notes",
                          "aaaaaaa1 new Crash Reports lists past crashes.",
                          "aaaaaaa2 -",
                          "aaaaaab1 internal Sound: The sound path is 32 bits wide.",
                          "aaaaaaa3 fixed Force-quitting System Update could freeze the desktop.",
                          "aaaaaab2 internal System: A program that never gives back a "
                          "window buffer gets a warning in the log.",
                          f"{own} -",
                          "aaaaaaa4 new Older than this build, never shown."]) + "\n"


def serve(throttle_kib, plain=False):
    manifest = Fixture(os.path.join(os.path.dirname(HERE), "seed", "sync"))
    manifest.plain = plain
    base = update_server.make_handler({"": manifest}, throttle_kib)

    class Quiet(base):
        # Not printed, but KEPT: the server's side of a stalled fetch.
        def do_GET(self):
            REQS.append((time.time(), "GET", self.path, ""))
            try:
                super().do_GET()
            except Exception as e:
                REQS.append((time.time(), "raised", self.path, repr(e)))
                raise

        def _say(self, status, size=None):
            REQS.append((time.time(), status, self.path, size))

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, manifest


LINES = []   # every app line views() has read: it clears the log as it goes
REQS = []    # (time, what, path, size): every request the fixture saw


def stall_evidence(qmp, dbg, t0):
    """A stalled fetch, as each side of the connection tells it: the
    server, QEMU's SLIRP (which the guest cannot fake), and the guest."""
    print("  evidence -- a fetch that never finished:")
    for t, what, path, size in REQS[-8:]:
        print(f"    server {t - t0:7.1f}s  {what} {path} {size}")
    try:
        for ln in qmp.hmp("info usernet").splitlines():
            if "TCP" in ln:
                print(f"    slirp  {ln.strip()}")
    except Exception as e:      # evidence only: never the verdict
        print(f"    slirp  unavailable: {e!r}")
    for cmd in ("sh netlog -n 6", "sh netctl", "sh ps"):
        try:
            for ln in (dbg.send(cmd) or "").strip().splitlines()[-14:]:
                print(f"    guest  {ln}")
        except Exception as e:
            print(f"    guest  {cmd}: {e!r}")


def views(dbg, seen):
    for line in dbg.logs("sysupdate:", clear=True):
        LINES.append(line)
        m = re.search(r"sysupdate: view (\d+) changed (\d+)", line)
        if m:
            seen.append((int(m.group(1)), int(m.group(2))))
    return seen


def wait_view(dbg, seen, n, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(views(dbg, seen)) >= n:
            return seen[n - 1]
        time.sleep(0.5)
    return None


def text_rows(im, rect):
    """Pixel rows in a rect carrying dark ink -- text, not the page."""
    x, y, w, h = rect
    return sum(1 for yy in range(y, y + h)
               if any(sum(im.getpixel((xx, yy))[:3]) < 300 for xx in range(x, x + w, 2)))


def screen_rect(w, name):
    r = w[name]
    return r["screen"]["x"], r["screen"]["y"], r["w"], r["h"]


def tab_slot(dbg, w, i):
    """Screen centre of tab `i`, from the strip's own report."""
    want = f"sysupdate: layout tabs.slot {i} "
    views(dbg, [])
    for line in reversed([ln for ln in LINES if want in ln]):
        p = line.split(want, 1)[1].split()
        if len(p) >= 4:
            ox = w["tabs"]["screen"]["x"] - w["tabs"]["x"]
            oy = w["tabs"]["screen"]["y"] - w["tabs"]["y"]
            x, y, ww, hh = (int(v) for v in p[:4])
            return ox + x + ww // 2, oy + y + hh // 2
    return None


def layout_rect(dbg, w, widget, key):
    """Screen rect of a sub-rect `widget` reports (`notes.fold 0 ...`)."""
    want = f"sysupdate: layout {widget}.{key} "
    views(dbg, [])
    for line in reversed([ln for ln in LINES if want in ln]):
        p = line.split(want, 1)[1].split()
        if len(p) >= 4:
            ox = w[widget]["screen"]["x"] - w[widget]["x"]
            oy = w[widget]["screen"]["y"] - w[widget]["y"]
            x, y, ww, hh = (int(v) for v in p[:4])
            return ox + x, oy + y, ww, hh
    return None


def layout_value(dbg, widget, key):
    want = f"sysupdate: layout {widget}.{key} "
    views(dbg, [])
    for line in reversed([ln for ln in LINES if want in ln]):
        try:
            return int(line.split(want, 1)[1].split()[0])
        except (IndexError, ValueError):
            pass
    return None


def accent_fraction(im, rect):
    """How much of the bar's inner width is filled with the accent."""
    x, y, w, h = rect
    row = y + h // 2
    filled = 0
    for cx in range(x + 1, x + w - 1):
        r, g, b = im.getpixel((cx, row))[:3]
        if b > r + 40 and b > 120:      # the accent blue, not white track or grey border
            filled += 1
    return filled / max(1, w - 2)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--positive-control", action="store_true",
                    help="serve NO notes and still expect them drawn")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sysupdate_test")

    why = update_server._stale_reason(os.path.join(os.path.dirname(HERE), "seed", "sync"))
    if why:
        check("the staging tree matches the build", False, f"{why} -- run `make iso`")
        return finish()

    srv, manifest = serve(throttle_kib=192, plain=args.positive_control)
    try:
        qmp = QMPSession(port=args.qmp_port)
        if not args.in_gui:
            enter_gui(qmp, args.sock)
        dbg = DebugConsole(args.sock)
        print("System Update (userland/update/upd.c, uui_progress)")
        url = f"http://10.0.2.2:{srv.server_address[1]}"
        dbg.send(f"sh update --server {url}")
        for path in DAMAGE:
            dbg.send(f"sh truncate -s 100 {path}")
        dbg.logs("sysupdate:", clear=True)

        seen, notes = [], []
        t0 = time.time()
        win = dbg.spawn(APP, TITLE)
        first = wait_view(dbg, seen, 1)
        for line in LINES:
            m = re.search(r"notes (\d+) hood (\d+) quiet (\d+) since (\d+)", line)
            if m:
                notes.append(tuple(int(v) for v in m.groups()))
        check("the notes are cut at the guest's own build",
              notes[-1:] == [NOTES + (1,)], f"notes/hood/quiet/since {notes[-1:]}")
        if not check("it checks by itself and finds the two damaged files",
                     first == (V_AVAILABLE, len(DAMAGE)), f"view/changed {first}"):
            if first is None:
                stall_evidence(qmp, dbg, t0)
            return finish()

        from PIL import Image
        dbg.settle()
        w = dbg.widgets(TITLE)
        shot = os.path.join(args.tmp, f"sysupdate_{os.getpid()}.png")
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        notes_rows = text_rows(im, screen_rect(w, "notes"))
        line_h = w["tabs"]["h"]
        check("What's new opens first, and the notes are drawn",
              notes_rows > 3 * line_h, f"{notes_rows} rows of ink, a line is ~{line_h}")

        # UNDER THE HOOD, shut, then opened by its own row.
        fold = layout_rect(dbg, w, "notes", "fold 0")
        opened = layout_value(dbg, "notes", "folds_open")
        if check("the unseen changes are behind one row, shut",
                 fold is not None and opened == 0, f"row {fold}, folds_open {opened}"):
            dbg.click(fold[0] + fold[2] // 3, fold[1] + fold[3] // 2)
            dbg.settle()
            opened = layout_value(dbg, "notes", "folds_open")
            check("...and clicking it opens it", opened == 1, f"folds_open {opened}")
            # WHAT IT OPENED IS DRAWN: scrolled to the end, the row rises
            # (the document is taller than the pane only because it opened)
            # and the band below it carries lines of text. The pane is
            # short, so what opens starts below its edge until scrolled.
            nx, ny, nw, nh = screen_rect(w, "notes")
            dbg.warp_cursor(qmp, nx + nw // 2, ny + nh // 2)
            dbg.wheel(-10)
            dbg.settle()
            moved = layout_rect(dbg, w, "notes", "fold 0")
            qmp.screenshot(shot)
            im = Image.open(shot).convert("RGB")
            band = ((nx, moved[1] + moved[3], nw - 16, ny + nh - moved[1] - moved[3])
                    if moved else None)
            ink = text_rows(im, band) if band and band[3] > 0 else 0
            check("...and what it opened is drawn, under the row",
                  moved is not None and moved[1] < fold[1] and ink > 2 * line_h,
                  f"row {fold} -> {moved}; {ink} rows of ink below it")
        slot = tab_slot(dbg, w, 1)
        if not check("the strip reports the Files tab", slot is not None):
            return finish()
        dbg.click(*slot)
        dbg.settle()
        w = dbg.widgets(TITLE)
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        t = w["files"]["screen"]
        tx, ty, tw, th = t["x"], t["y"], w["files"]["w"], w["files"]["h"]
        dark = sum(1 for yy in range(ty, ty + th, 2) for xx in range(tx, tx + tw, 2)
                   if sum(im.getpixel((xx, yy))) < 300)
        check("the file list is drawn", dark > 100, f"{dark} dark px (sampled)")

        x, y = dbg.widget_center("main", TITLE)
        dbg.click(x, y, settle=False)
        time.sleep(4.0)
        qmp.screenshot(shot, stable=False)
        im = Image.open(shot).convert("RGB")
        b = w["bar"]
        bar = (b["screen"]["x"], b["screen"]["y"], b["w"], b["h"])
        mid = accent_fraction(im, bar)
        check("mid-install the bar is part-filled", 0.02 < mid < 0.98, f"{mid:.2f} filled")
        sel = 0
        for yy in range(ty, ty + th):
            r, g, bb = im.getpixel((tx + tw // 2, yy))[:3]
            if (r, g, bb) != (255, 255, 255) and bb > r + 20 and r > 150:
                sel += 1
        check("...and the row being fetched is tinted", sel > 4, f"{sel} tinted px rows")

        last = wait_view(dbg, seen, 2, timeout=180)
        if not check("it ends on \"installed\", with no restart needed",
                     last == (V_INSTALLED, len(DAMAGE)), f"view/changed {last}") and last is None:
            stall_evidence(qmp, dbg, t0)
        text, _ = manifest.build()
        want = {ln.split(" ")[2]: int(ln.split(" ")[0]) for ln in text.splitlines()
                if not ln.startswith("#")}
        for path in DAMAGE:
            out = dbg.send(f"sh sum {path}") or ""
            m = re.search(r"(\d+) (\d+) " + re.escape(path), out)
            check(f"sum agrees with the manifest for {path}",
                  m and int(m.group(1)) == want.get(path), f"{out.strip()[-80:]}")
        dbg.settle()
        qmp.screenshot(shot)
        im = Image.open(shot).convert("RGB")
        end = accent_fraction(im, bar)
        check("the bar ends full", end > 0.98, f"{end:.2f} filled")
        slot = tab_slot(dbg, w, 0)
        if slot:
            dbg.click(*slot)
            dbg.settle()
            qmp.screenshot(shot)
            im = Image.open(shot).convert("RGB")
            after = text_rows(im, screen_rect(dbg.widgets(TITLE), "notes"))
            check("...and What's new still shows the notes", after > 3 * line_h,
                  f"{after} rows of ink")
        if win:
            wz = next((x for x in dbg.windows() if x.get("title") == TITLE), None)
            if wz:
                dbg.send(f"gui close {wz['z']}")
    finally:
        srv.shutdown()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
