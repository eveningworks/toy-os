#!/usr/bin/env python3
"""The File Manager's thumbnail RATE, and the disk cache under it.

**THE ASSERTION IS A RATE, NOT A DURATION.** Thumbnails used to be
decoded a COUNT PER TICK -- two, on a 500 ms tick -- so a folder filled
in at four a second whatever the pictures cost, and `/usr/share/icons`
(53 tiny QOI files, 25 KB, microseconds of real work) took thirteen
seconds, two cells at a time. Four a second was that design's CEILING,
by construction, so a rate above it is something it could not have
produced however fast the host is -- where a wall-clock bound would just
be a bet on the emulator. Measured at 24/s on TCG when this was written;
the bar here is 8.

It reads the app's own drain report (`userland/fm/fm_thumbs.c` logs one
line per drained queue). **NOT `sh dmesg`** -- the kernel ring holds a
few hundred lines and any tool that has turned the layout log on fills
it in about a second, so the line being waited for is destroyed before
it can be read. `DebugConsole.logs()` is the wire, swept as it arrives.

WHY THIS IS ITS OWN TOOL rather than a section in filemanager_test.py:
that tool drives one long-lived File Manager through a layout-log state
machine, and this one wants to START A FRESH APP TWICE -- once cold,
once against a populated cache. Those are different fixtures, and a
section that navigates a shared pane somewhere else has to navigate it
back, which is a precondition to get wrong for no gain.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gui_debug import DebugConsole, enter_gui          # noqa: E402
from qmp_test import QMPSession                        # noqa: E402
import port_guard  # noqa: E402

FILES_EXEC = "/bin/wm/apps/files"
FILES_CONF = "/etc/files.conf"
ICON_DIR = "/usr/share/icons"
THUMB_CACHE = "/var/cache/thumbnails"
# REAL STORAGE, not /tmp: this fixture is about mtimes on disk
# (docs/filesystem-layout.md).
STALE_DIR = "/var/tmp/thumbstale"

# What the old count-per-tick could reach, and the bar this asserts.
OLD_CEILING = 4.0
MIN_RATE = 8.0
MIN_COUNT = 12


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, what, ok, detail=""):
        (self.passes if ok else self.fails).append(what)
        print(f"  {'PASS' if ok else 'FAIL'}  {what}")
        if not ok and detail:
            print(f"        {detail}")


def drain_reports(dbg, seen):
    """Every `N thumbnail(s) in M ms, C from the cache` line so far.

    Accumulates into `seen` because logs() CLEARS what it returns: a
    second call would otherwise find nothing and read as an app that
    never reported.
    """
    seen.extend(dbg.logs("thumbnail(s) in", clear=True))
    out = []
    for line in seen:
        try:
            head = line.split("files:", 1)[1].strip()
            n = int(head.split()[0])
            ms = int(head.split(" in ")[1].split(" ms")[0])
            cached = int(head.split(", ")[1].split()[0])
        except (IndexError, ValueError):
            continue
        out.append((n, ms, cached))
    return out


def wait_drain(dbg, seen, after, timeout=45.0):
    """The first drain report beyond the `after` already seen, or None."""
    end = time.time() + timeout
    while time.time() < end:
        got = drain_reports(dbg, seen)
        if len(got) > after:
            return got[after]
        time.sleep(0.5)
    return None


def open_icons_on(dbg, path):
    """A FRESH File Manager showing `path` in the icons view.

    The view mode is REMEMBERED per pane (/etc/files.conf), so a tool
    that inherited whatever the last session left would thumbnail
    nothing at all in details view and report that as a broken cache.
    Removing the file is how this establishes its own precondition --
    icons is the built-in default.
    """
    dbg.send(f"sh rm {FILES_CONF}")
    return dbg.send(f"gui spawn {FILES_EXEC} {path} {path}")


def close_all_files(dbg):
    for w in sorted(dbg.windows(), key=lambda w: -w["z"]):
        if w["title"] == "File Manager":
            dbg.send(f"gui close {w['z']}")
            time.sleep(0.4)


def run(dbg, res):
    seen = []
    dbg.send(f"sh rm -r {THUMB_CACHE}")
    gone = dbg.send(f"sh ls {THUMB_CACHE}") or ""
    res.check("(the cache starts empty)", "no such file" in gone or not gone.strip(),
              f"{THUMB_CACHE} still lists: {gone.strip()[:120]}")

    # --- 1. the cold pass: decode, at a rate the old design could not --
    close_all_files(dbg)
    before = len(drain_reports(dbg, seen))
    open_icons_on(dbg, ICON_DIR)
    cold = wait_drain(dbg, seen, before)
    res.check(f"a folder of images drains at over {MIN_RATE:.0f}/s",
              cold is not None and cold[0] >= MIN_COUNT and cold[1] > 0 and
              cold[0] * 1000.0 / cold[1] > MIN_RATE,
              f"drain report {cold} -- want >= {MIN_COUNT} thumbnails at over "
              f"{MIN_RATE:.0f}/s; the count-per-tick this replaced could not "
              f"exceed {OLD_CEILING:.0f}/s")
    # THE CONTROL FOR THAT NUMBER: with the cache just removed, a report
    # claiming cache hits is a report being read off a previous run.
    res.check("control: the cold pass decoded, it did not read the cache",
              cold is not None and cold[2] == 0,
              f"drain report {cold} -- {THUMB_CACHE} was removed above")

    # --- 2. it left something behind, named after the source -----------
    listing = dbg.send(f"sh ls {THUMB_CACHE}") or ""
    res.check("...and wrote thumbnails named after their source files",
              "usr%share%icons%" in listing,
              f"{THUMB_CACHE}: {listing.strip()[:200]}")

    # --- 3. the warm pass: a SECOND app reads them off the disk --------
    #
    # A fresh process is the point: an in-memory table would satisfy
    # "the second look is fast" without the disk ever being read, which
    # is the version of this check that measures nothing.
    close_all_files(dbg)
    before = len(drain_reports(dbg, seen))
    open_icons_on(dbg, ICON_DIR)
    warm = wait_drain(dbg, seen, before)
    res.check("a second, FRESH app takes them off the disk",
              warm is not None and warm[2] >= MIN_COUNT,
              f"drain report {warm} -- want at least {MIN_COUNT} `from the "
              "cache`; 0 means the cache was written and never read")

    # --- 4. a CHANGED file is re-decoded, not served stale -------------
    #
    # Staleness here is "the entry is not OLDER than its source"
    # (docs/decisions.md), and nothing records an mtime inside the file,
    # so this is the check that the rule works at all.
    #
    # **THE FIXTURE IS ITS OWN DIRECTORY, AND `touch` WILL NOT DO.** The
    # first version of this check touched a file in /usr/share/icons and
    # went red against working code: toy-os's `touch` does not move an
    # EXISTING file's mtime, so the input never reached the branch --
    # suspect the fixture before the code (CLAUDE.md). Copying a
    # different icon over it moves the mtime AND the size, and doing it
    # in a scratch directory keeps a system one unmodified.
    close_all_files(dbg)
    dbg.send(f"sh rm -r {STALE_DIR}")
    dbg.send(f"sh mkdir {STALE_DIR}")
    for n, src in (("a", "about"), ("b", "calculator"), ("c", "help")):
        dbg.send(f"sh cp {ICON_DIR}/{src}.qoi {STALE_DIR}/{n}.qoi")
    before = len(drain_reports(dbg, seen))
    open_icons_on(dbg, STALE_DIR)
    first = wait_drain(dbg, seen, before)
    res.check("(the scratch fixture thumbnailed, cold)",
              first is not None and first[0] >= 3 and first[2] == 0,
              f"drain report {first} -- want 3 decodes and no cache hits")

    close_all_files(dbg)
    dbg.send(f"sh cp {ICON_DIR}/doom.qoi {STALE_DIR}/a.qoi")   # mtime AND size
    before = len(drain_reports(dbg, seen))
    open_icons_on(dbg, STALE_DIR)
    after = wait_drain(dbg, seen, before)
    res.check("a rewritten source is decoded again, not served stale",
              after is not None and after[0] >= 3 and after[2] == after[0] - 1,
              f"drain report {after} -- want exactly one of three decoded; "
              "all-cached means the freshness test ignores the source's mtime, "
              "none-cached means it ignores the cache entirely")

    # PUT THE MACHINE BACK. This tool opens a File Manager four times and
    # removes /etc/files.conf to get a known view mode, and BOTH outlive
    # it: run straight afterwards, imgview_test went from 20/20 to 9/11,
    # failing on pixels with this tool's windows still on the desktop.
    # A tool that leaves the machine changed breaks whatever runs next
    # (CLAUDE.md), and the fact that gui_regress.py gives each tool its
    # own boot is not a reason to rely on it.
    dbg.send(f"sh rm -r {STALE_DIR}")
    close_all_files(dbg)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true",
                    help="the VM already shows the desktop")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "thumbcache_test")

    qmp = QMPSession(port=args.qmp_port)
    if not args.in_gui:
        enter_gui(qmp, args.sock)
    dbg = DebugConsole(args.sock)
    res = Result()
    print("thumbcache_test: checks")
    try:
        run(dbg, res)
    finally:
        dbg.close()
    print(f"\nthumbcache_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
