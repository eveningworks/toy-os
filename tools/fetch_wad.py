#!/usr/bin/env python3
"""tools/fetch_wad.py -- put a Doom IWAD where the build will seed it.

WHY THIS IS A SCRIPT AND NOT A COMMITTED FILE
---------------------------------------------
`/bin/wm/apps/doom` needs an IWAD: the lumps -- levels, sprites, sounds,
the menu graphics -- that the engine is separate from. doomgeneric is
GPL-2 and vendored in `userland/ports/doom/`; a WAD is not GPL and is
not ours.

id's shareware licence permits redistributing the unmodified shareware
PACKAGE. Lifting one file out of it into an unrelated project's git
history is a different act, and not one this repo needs to perform in
order to work -- so the WAD is `.gitignore`d and this script is how a
checkout gets one. A clean clone builds and boots fine without it; DOOM
opens and says, in its own window, that it could not find an IWAD.

WHAT COUNTS AS AN IWAD HERE
---------------------------
Anything Doom accepts, dropped at data/doom/doom1.wad.
The shareware `doom1.wad` (~4 MB, episode 1) is the default because it
is the one that can be fetched without owning anything. Freedoom's
`freedoom1.wad` is BSD-licensed and drops in unchanged -- pass its path
to --from. A retail `DOOM.WAD` works too, and is the case --from exists
for: if you own Doom, point this at your copy rather than downloading
anything.

    python3 tools/fetch_wad.py                 # fetch the shareware WAD
    python3 tools/fetch_wad.py --freedoom      # ...and Freedoom's release zip
    python3 tools/fetch_wad.py --from ~/DOOM.WAD
    make iso                                   # seeds it onto disk.img

VERIFYING WHAT ARRIVED
----------------------
A download that returns an HTML error page is still a 200, and a WAD
that is 341 bytes of `<?xml` fails much later and much more confusingly
than it should -- so this checks the magic (`IWAD`/`PWAD`) and a
plausible size before it writes anything, and says which mirror it used.
A PWAD is REFUSED with its own message: it is a patch, not a game, and
Doom cannot start from one.
"""

import argparse
import hashlib
import os
import shutil
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_extras import urlopen  # noqa: E402 -- names itself; see USER_AGENT

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# `data/`, NOT `seed/sync/`. seed/sync is a STAGING directory the build
# writes into from data/ on every `make iso`; a file placed there
# directly survives until the next clean and then vanishes, which is a
# confusing way to lose a 4 MB download. data/ is the canonical source
# for everything seeded onto the image.
DEST_DIR = os.path.join(REPO, "data", "doom")
DEST = os.path.join(DEST_DIR, "doom1.wad")

# Ordered; the first that yields something WAD-shaped wins. Mirrors of
# the shareware episode, which id has allowed to circulate since 1993.
# (ibiblio's slitaz copy answered 404 on 2026-10-04 and was dropped.)
MIRRORS = [
    "https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/master/doom1.wad",
]

# --freedoom: the release zip DOOM's own downloader fetches, for
# tools/doom_data_test.py to serve on loopback. Pinned by the SHA-256 the
# release signs (freedoom-0.13.0-CHECKSUM); userland/doom/doom_wad.c pins
# the same, so a bump changes both.
FREEDOOM_URL = ("https://github.com/freedoom/freedoom/releases/download/"
                "v0.13.0/freedoom-0.13.0.zip")
FREEDOOM_SHA = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
FREEDOOM_DEST = os.path.join(DEST_DIR, "freedoom-0.13.0.zip")

MIN_BYTES = 1_000_000


def looks_like_iwad(blob):
    """(ok, why). Checked BEFORE writing -- see the docstring."""
    if len(blob) < MIN_BYTES:
        return False, f"only {len(blob)} bytes; an IWAD is megabytes"
    magic = blob[:4]
    if magic == b"PWAD":
        return False, ("this is a PWAD (a patch WAD). Doom cannot start from "
                       "one -- it needs an IWAD, a complete game")
    if magic != b"IWAD":
        printable = magic.decode("latin-1").replace("\n", " ")
        return False, (f"magic is {printable!r}, not 'IWAD' -- probably an "
                       f"error page rather than a WAD")
    return True, ""


def fetch_freedoom(force):
    if os.path.exists(FREEDOOM_DEST) and not force:
        print(f"fetch_wad: {FREEDOOM_DEST} already exists")
        return 0
    os.makedirs(DEST_DIR, exist_ok=True)
    print(f"fetch_wad: fetching {FREEDOOM_URL}")
    with urlopen(FREEDOOM_URL, timeout=120) as r:
        blob = r.read()
    got = hashlib.sha256(blob).hexdigest()
    if got != FREEDOOM_SHA:
        print(f"fetch_wad: SHA-256 {got} is not the release's {FREEDOOM_SHA}", file=sys.stderr)
        return 1
    with open(FREEDOOM_DEST, "wb") as f:
        f.write(blob)
    print(f"fetch_wad: wrote {FREEDOOM_DEST} ({len(blob)} bytes), checksum matches")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src",
                    help="copy this local WAD instead of downloading one")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing WAD")
    ap.add_argument("--freedoom", action="store_true",
                    help="also fetch Freedoom's release zip, for doom_data_test.py")
    args = ap.parse_args()

    if args.freedoom and fetch_freedoom(args.force):
        return 1

    if os.path.exists(DEST) and not args.force:
        print(f"fetch_wad: {DEST} already exists ({os.path.getsize(DEST)} bytes)")
        print("fetch_wad: --force to replace it. Run `make iso` to seed it.")
        return 0

    os.makedirs(DEST_DIR, exist_ok=True)

    if args.src:
        src = os.path.expanduser(args.src)
        with open(src, "rb") as f:
            blob = f.read()
        ok, why = looks_like_iwad(blob)
        if not ok:
            print(f"fetch_wad: {src}: {why}", file=sys.stderr)
            return 1
        shutil.copyfile(src, DEST)
        print(f"fetch_wad: copied {src} -> {DEST} ({len(blob)} bytes)")
        print("fetch_wad: run `make iso` to seed it onto disk.img")
        return 0

    for url in MIRRORS:
        print(f"fetch_wad: trying {url}")
        try:
            with urlopen(url, timeout=60) as r:
                blob = r.read()
        except Exception as e:                      # noqa: BLE001 -- any failure is "try the next mirror"
            print(f"fetch_wad:   failed: {e}")
            continue
        ok, why = looks_like_iwad(blob)
        if not ok:
            print(f"fetch_wad:   rejected: {why}")
            continue
        with open(DEST, "wb") as f:
            f.write(blob)
        print(f"fetch_wad: wrote {DEST} ({len(blob)} bytes) from {url}")
        print("fetch_wad: run `make iso` to seed it onto disk.img")
        return 0

    print("fetch_wad: no mirror yielded a WAD.", file=sys.stderr)
    print("fetch_wad: pass --from <path> to use a copy you already have "
          "(shareware doom1.wad, freedoom1.wad, or a retail DOOM.WAD).",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
