#!/usr/bin/env python3
"""tools/fetch_soundfont.py -- put a full General MIDI SoundFont where the
build will seed it.

WHY THIS IS A SCRIPT AND NOT A COMMITTED FILE
---------------------------------------------
The MIDI codec (userland/lib/usnd_mid.c) plays through whatever SoundFont
is in /usr/share/soundfonts. The repository tracks a small bank of its
own (tools/gen_sf2.py, toy-gm.sf2) so a clean checkout plays MIDI at
all; a bank of RECORDED instruments is ~30 MB of somebody else's work
and does not belong in this history. This fetches one into
data/soundfonts/ (gitignored), and `make iso EXTRAS=1` seeds it -- where
it wins over the built-in bank, because usnd_mid.c prefers any other
.sf2 it finds.

WHICH BANK
----------
GeneralUser GS by S. Christian Collins (www.schristiancollins.com):
~31 MB, a complete GM/GS set, voiced against FluidSynth -- whose
conventions usnd_synth.c follows, down to reading the attenuation
generator at 0.4 dB a unit. Its licence lets you use it without
restriction and ship it in software; the one request is that a WEBSITE
not hotlink the author's own downloads. This fetches from the project's
GitHub repository, which is its published distribution. The licence
text is saved beside the bank and travels onto the image with it.

Any other .sf2 works too -- FluidR3_GM (MIT, 141 MB) is the obvious
alternative: pass it with --from.

    python3 tools/fetch_soundfont.py              # fetch GeneralUser GS
    python3 tools/fetch_soundfont.py --from ~/FluidR3_GM.sf2
    make iso EXTRAS=1                             # seeds it onto disk.img

A download that returns an HTML error page is still a 200, so the magic
(RIFF/sfbk) and a plausible size are checked BEFORE anything is written.
"""
import argparse
import os
import shutil
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_extras import urlopen  # noqa: E402 -- names itself; see USER_AGENT

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# data/, NOT seed/sync/ -- see tools/fetch_wad.py for why.
DEST_DIR = os.path.join(REPO, "data", "soundfonts")
DEST = os.path.join(DEST_DIR, "GeneralUser-GS.sf2")
LICENSE_DEST = os.path.join(DEST_DIR, "GeneralUser-GS-LICENSE.txt")

BASE = "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/main/"
BANK_URL = BASE + "GeneralUser-GS.sf2"
LICENSE_URL = BASE + "documentation/LICENSE.txt"

MIN_BYTES = 1_000_000


def looks_like_sf2(blob):
    """(ok, why). Checked BEFORE writing."""
    if len(blob) < MIN_BYTES:
        return False, f"only {len(blob)} bytes; a GM bank is megabytes"
    if blob[:4] != b"RIFF" or blob[8:12] != b"sfbk":
        head = blob[:12].decode("latin-1").replace("\n", " ")
        return False, f"starts {head!r}, not RIFF/sfbk -- probably an error page"
    if b"pdta" not in blob[-4 * 1024 * 1024:]:
        return False, "no preset data (pdta) where a SoundFont keeps it"
    return True, ""


def fetch(url):
    with urlopen(url, timeout=120) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src",
                    help="copy this local .sf2 instead of downloading one")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing bank")
    args = ap.parse_args()

    os.makedirs(DEST_DIR, exist_ok=True)

    if args.src:
        src = os.path.expanduser(args.src)
        with open(src, "rb") as f:
            blob = f.read()
        ok, why = looks_like_sf2(blob)
        if not ok:
            print(f"fetch_soundfont: {src}: {why}", file=sys.stderr)
            return 1
        dest = os.path.join(DEST_DIR, os.path.basename(src))
        shutil.copyfile(src, dest)
        print(f"fetch_soundfont: copied {src} -> {dest} ({len(blob)} bytes)")
        print("fetch_soundfont: run `make iso EXTRAS=1` to seed it onto disk.img")
        return 0

    if os.path.exists(DEST) and not args.force:
        print(f"fetch_soundfont: {DEST} already exists ({os.path.getsize(DEST)} bytes)")
        print("fetch_soundfont: --force to replace it. `make iso EXTRAS=1` seeds it.")
        return 0

    print(f"fetch_soundfont: fetching {BANK_URL}")
    try:
        blob = fetch(BANK_URL)
        licence = fetch(LICENSE_URL)
    except Exception as e:                      # noqa: BLE001 -- report any failure the same way
        print(f"fetch_soundfont: failed: {e}", file=sys.stderr)
        print("fetch_soundfont: download it from www.schristiancollins.com and "
              "pass --from <path>", file=sys.stderr)
        return 1
    ok, why = looks_like_sf2(blob)
    if not ok:
        print(f"fetch_soundfont: rejected: {why}", file=sys.stderr)
        return 1
    with open(DEST, "wb") as f:
        f.write(blob)
    with open(LICENSE_DEST, "wb") as f:
        f.write(licence)
    print(f"fetch_soundfont: wrote {DEST} ({len(blob)} bytes) and its licence")
    print("fetch_soundfont: run `make iso EXTRAS=1` to seed it onto disk.img")
    return 0


if __name__ == "__main__":
    sys.exit(main())
