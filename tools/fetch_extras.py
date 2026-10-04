#!/usr/bin/env python3
"""tools/fetch_extras.py -- the optional, differently-licensed material.

WHY THIS EXISTS
---------------
Almost everything toy-os ships is ours or is generated here, which is
what keeps `LICENSE` short and a release asset unambiguous. A few things
are neither: the Doom shareware IWAD is the standing example, and it is
deliberately NOT in the repository (see tools/fetch_wad.py).

**FETCHING IS NOT DISTRIBUTING, AND THAT IS THE WHOLE POINT.** A script
that downloads a file onto YOUR machine makes you the recipient; nothing
third-party enters this repository or its history. What collapses that
distinction is publishing a BUILT ARTIFACT: an ISO carrying a fetched
WAD, uploaded as a release asset, is you distributing that WAD. So this
never runs as part of an ordinary build, and every image built with it
carries a manifest saying what is inside -- see `stamp_manifest()`.

This is a REGISTRY, not a script per item. The tenth extra should be a
row in EXTRAS below, the same shape `display_driver` and `block_device`
already use, because a target per item multiplies exactly as fast as a
recipe per item does.

USAGE
    make iso EXTRAS=1                 # fetch what is missing, then build
    make iso EXTRAS=1 LICENSE=agree   # ...without being asked
    python3 tools/fetch_extras.py --list
    python3 tools/fetch_extras.py --only doom

ACCEPTANCE
----------
Each item names a licence that is NOT this repository's. You are asked
once, on a terminal; the answer is recorded in `.extras-accepted` at the
repo root (gitignored) keyed by item name AND a hash of the licence
summary, so a NEW extra -- or a changed licence -- asks again. Accepting
one thing must not silently accept the next thing somebody adds.

**A NON-TTY IS REFUSED, NEVER PROMPTED.** `make` under CI, or with its
output captured, has nothing to answer with, and a prompt there hangs
forever -- which is the mtools trap CLAUDE.md already records, arriving
from a different direction. Without an acceptance on file such a build
stops and says which flag to pass.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The record of what has been agreed to. At the REPO ROOT rather than
# under build/, because `make clean` wipes build/ and preflight.sh starts
# with one -- an acceptance that has to be given again after every clean
# build is an acceptance nobody reads.
ACCEPTED = os.path.join(REPO, ".extras-accepted")


class Extra:
    """One fetchable thing, and everything a person needs to decide."""

    def __init__(self, name, what, licence, summary, source, dest, fetch):
        self.name = name          # what --only takes
        self.what = what          # one line: what it is for
        self.licence = licence    # the licence's NAME
        self.summary = summary    # what it permits, in plain words
        self.source = source      # where it comes from
        self.dest = dest          # repo-relative, so --list can say if present
        self.fetch = fetch        # argv to run

    def present(self):
        return os.path.exists(os.path.join(REPO, self.dest))

    def key(self):
        # Keyed on the licence TEXT, so changing what someone agreed to
        # invalidates the agreement rather than inheriting it.
        h = hashlib.sha256((self.licence + "\n" + self.summary).encode())
        return f"{self.name}:{h.hexdigest()[:16]}"


EXTRAS = [
    Extra(
        name="doom",
        what="The Doom shareware IWAD -- the levels, sprites and sounds "
             "/bin/wm/apps/doom plays. Without it the app opens and says so.",
        licence="id Software shareware licence (not an OSI licence)",
        summary=(
            "id permits redistributing the UNMODIFIED shareware package, "
            "non-commercially, at no charge beyond media cost. It is a "
            "1990s bespoke licence, not MIT/BSD/GPL. The Doom ENGINE in "
            "userland/ports/doom/ is GPL-2-or-later and is already in "
            "this repository; the WAD is data and is not GPL. Fetching a "
            "copy for yourself is not redistribution -- publishing an "
            "image containing it is."
        ),
        source="https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/ "
               "(a third party's copy; see tools/fetch_wad.py)",
        dest="data/doom/doom1.wad",
        fetch=[sys.executable, os.path.join(HERE, "fetch_wad.py")],
    ),
    Extra(
        name="ca-bundle",
        what="Mozilla's CA root certificates -- the trust anchors that let "
             "`wget https://` verify a public server. Without them the trust "
             "store is empty and https refuses by name.",
        licence="MPL-2.0 (Mozilla Public License 2.0)",
        summary=(
            "MPL-2.0 is FILE-SCOPED copyleft: it reaches the covered file "
            "and nothing that merely reads it, and section 3.3 permits "
            "distributing it inside a larger work under other terms. So it "
            "sits beside MIT code without affecting any of it -- far weaker "
            "than the Doom engine's GPL, and it is DATA rather than linked "
            "code, so there is no linking question at all. The obligation "
            "is to keep the notice and offer the file's source, which the "
            "file itself is. curl ships the same conversion and is MIT."
        ),
        source="https://curl.se/ca/cacert.pem -- curl's PEM conversion of "
               "Mozilla's certdata.txt (see tools/fetch_ca_bundle.py for "
               "why not certdata.txt directly)",
        dest="data/etc/ssl/certs/mozilla-roots.pem",
        fetch=[sys.executable, os.path.join(HERE, "fetch_ca_bundle.py")],
    ),
    Extra(
        name="soundfont",
        what="GeneralUser GS, a ~31 MB General MIDI SoundFont of recorded "
             "instruments. MIDI files play through it instead of the small "
             "built-in bank (toy-gm.sf2), which stays as the fallback.",
        licence="GeneralUser GS License v2.0 (bespoke, permissive; not an OSI licence)",
        summary=(
            "Use without restriction, private or commercial, including in "
            "software projects, modified or not. The author cannot vouch for "
            "the origin of every sample (none from commercial packages, no "
            "complaint since 2000) and asks that a WEBSITE not hotlink his "
            "downloads. It is DATA read by our synth, not linked code, so "
            "there is no licence interaction with this repository. The "
            "licence text is fetched with it and seeded beside it."
        ),
        source="https://github.com/mrbumpy409/GeneralUser-GS -- the author's "
               "repository (see tools/fetch_soundfont.py)",
        dest="data/soundfonts/GeneralUser-GS.sf2",
        fetch=[sys.executable, os.path.join(HERE, "fetch_soundfont.py")],
    ),
]


def load_accepted():
    try:
        with open(ACCEPTED) as f:
            return set(json.load(f).get("accepted", []))
    except (OSError, ValueError):
        return set()


def save_accepted(keys):
    with open(ACCEPTED, "w") as f:
        json.dump({
            "_comment": "Written by tools/fetch_extras.py. Gitignored. "
                        "Each entry is an item name plus a hash of the "
                        "licence you agreed to, so a changed licence asks "
                        "again. Delete this file to be asked afresh.",
            "accepted": sorted(keys),
        }, f, indent=2)
        f.write("\n")


def show(e):
    print(f"\n  {e.name} -- {e.what}")
    print(f"    Licence: {e.licence}")
    for line in _wrap(e.summary, 68):
        print(f"      {line}")
    print(f"    Source:  {e.source}")
    print(f"    Lands:   {e.dest}")


def _wrap(text, width):
    out, line = [], ""
    for word in text.split():
        if len(line) + len(word) + 1 > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word).strip()
    if line:
        out.append(line)
    return out


def agreed(e, accepted, auto):
    if e.key() in accepted:
        return True
    show(e)
    if auto:
        print("    -> accepted by LICENSE=agree")
        return True
    if not sys.stdin.isatty():
        # Flush first: the licence above went to stdout and this goes to
        # stderr, and a captured build otherwise shows the refusal before
        # the thing being refused.
        sys.stdout.flush()
        print(f"\nfetch_extras: refusing to fetch '{e.name}' without an "
              f"answer.\n  This build has no terminal to ask on. Pass "
              f"LICENSE=agree if you accept\n  the licence above, or run "
              f"`python3 tools/fetch_extras.py --only {e.name}`\n  once on "
              f"a terminal to record it.", file=sys.stderr)
        return False
    try:
        reply = input("\n    Accept this licence and fetch it? [y/N] ")
    except (EOFError, KeyboardInterrupt):
        print()
        return False
    return reply.strip().lower() in ("y", "yes")


def stamp_manifest(fetched):
    """Say, INSIDE the image, what non-ours material it carries.

    An image built with extras is one you must not publish casually, and
    the only durable place to record that is the image itself -- a note
    in a shell's scrollback is gone by the time anybody uploads anything.
    """
    d = os.path.join(REPO, "data", "usr", "share", "licenses")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "extras.txt")
    if not fetched:
        if os.path.exists(path):
            os.remove(path)
        return
    with open(path, "w") as f:
        f.write("Material in this image that is NOT covered by toy-os's\n"
                "own licence. Fetched by tools/fetch_extras.py, which is\n"
                "off by default -- this image was built with EXTRAS=1.\n\n"
                "Publishing this image distributes what is listed here.\n\n")
        for e in fetched:
            f.write(f"{e.name}: {e.what}\n")
            f.write(f"  Licence: {e.licence}\n")
            for line in _wrap(e.summary, 66):
                f.write(f"    {line}\n")
            f.write(f"  Source: {e.source}\n\n")
    print(f"fetch_extras: wrote {os.path.relpath(path, REPO)} -- this image "
          f"carries third-party material")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--list", action="store_true",
                    help="show every extra and whether it is present")
    ap.add_argument("--only", metavar="NAME", help="just this one")
    ap.add_argument("--agree", action="store_true",
                    help="accept every licence without asking (what "
                         "LICENSE=agree passes)")
    args = ap.parse_args()

    auto = args.agree or os.environ.get("TOYOS_LICENSE", "") == "agree"
    chosen = [e for e in EXTRAS if not args.only or e.name == args.only]
    if args.only and not chosen:
        print(f"fetch_extras: no extra named '{args.only}' -- "
              f"try --list", file=sys.stderr)
        return 2

    if args.list:
        for e in EXTRAS:
            print(f"  {'present' if e.present() else 'absent '}  {e.name:10s} "
                  f"{e.licence}")
            print(f"                       -> {e.dest}")
        return 0

    accepted = load_accepted()
    fetched, refused = [], []
    for e in chosen:
        if e.present():
            print(f"fetch_extras: {e.name} is already at {e.dest}")
            fetched.append(e)
            continue
        if not agreed(e, accepted, auto):
            refused.append(e)
            continue
        accepted.add(e.key())
        save_accepted(accepted)
        print(f"fetch_extras: fetching {e.name}...")
        r = subprocess.run(e.fetch, cwd=REPO)
        if r.returncode != 0 or not e.present():
            # LOUD, because a silently missing extra ships an image that
            # is quietly wrong -- the failure the cursor themes had.
            print(f"fetch_extras: FAILED to fetch {e.name}", file=sys.stderr)
            return 1
        fetched.append(e)

    stamp_manifest(fetched)
    if refused:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
