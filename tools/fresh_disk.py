#!/usr/bin/env python3
"""A pristine disk image, and whether an existing one has drifted.

    python3 tools/fresh_disk.py                  # is disk.img clean?
    python3 tools/fresh_disk.py --make OUT.img   # a pristine image of this build

WHY. `make iso` SYNCS disk.img rather than reformatting it, so what a
test or a `make run` left behind -- `/etc/windows.conf`'s remembered
window positions, a launcher in `/home/desktop` -- stays on the image and
reaches every later VM booted from a copy of it. On 2026-09-30 that made
four GUI tools fail deterministically, and `predates.py` then called them
pre-existing, because both of its runs boot the same image. A fresh image
cleared three of the four.

`drift()` is the cheap question: the files on the image under a seeded
directory that nothing in seed/sync places (check_layout.py's orphan
list, the same answer preflight prints as a warning). It sees what the OS
and the tests WROTE; it cannot see a seeded file edited in place.

`make_fresh()` is the honest answer, from host tools alone and in about
twenty seconds: a new sparse image, seed_disk.py, install_grub.py -- the
recipe `make iso` runs for disk.img, pointed elsewhere, so disk.img and
the Makefile's `.seeded` stamp are never touched.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from check_layout import orphans_on_image   # noqa: E402

SEED = os.path.join(REPO, "seed")
DISK_SIZE = "9G"          # the Makefile's $(DISK_IMG) rule


def drift(disk):
    """[(image path, seed dir)] -- files a build did not put there. [] on
    an image that cannot be read, which is not this function's to report."""
    if not os.path.exists(disk) or not os.path.isdir(os.path.join(SEED, "sync")):
        return []
    try:
        return orphans_on_image(disk, HERE, os.path.join(SEED, "sync"))
    except (OSError, ValueError, subprocess.SubprocessError):
        return []


def describe(disk, found, limit=6):
    """The warning, as lines -- the same words from every tool that asks."""
    out = [f"WARNING: {os.path.relpath(disk, REPO)} carries {len(found)} file(s) no build "
           "puts there -- state a test or `make run` left:"]
    out += [f"    {p}" for p, _ in found[:limit]]
    if len(found) > limit:
        out.append(f"    ... and {len(found) - limit} more")
    out.append("  A failure below may be the FIXTURE. Re-run on a fresh image "
               "(`--fresh`, or `make clean-disk && make iso`) before believing it.")
    return out


def make_fresh(dest, quiet=True):
    """Build a pristine image of the CURRENT build at `dest`. Returns dest."""
    kernel = os.path.join(REPO, "build", "kernel.media")
    cfg = os.path.join(REPO, "build", "grub-disk.cfg")
    for need in (kernel, cfg, os.path.join(SEED, "sync")):
        if not os.path.exists(need):
            raise SystemExit(f"fresh_disk: no {os.path.relpath(need, REPO)} -- run `make iso` first")
    if os.path.exists(dest):
        os.remove(dest)
    out = subprocess.DEVNULL if quiet else None
    subprocess.run(["truncate", "-s", DISK_SIZE, dest], check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "seed_disk.py"), dest, SEED],
                   check=True, stdout=out, cwd=REPO)
    subprocess.run([sys.executable, os.path.join(HERE, "install_grub.py"), dest,
                    "--kernel", kernel, "--grub-cfg", cfg, "--optional"],
                   check=True, stdout=out, cwd=REPO)
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--make", metavar="OUT", help="write a pristine image to OUT")
    args = ap.parse_args()
    if args.make:
        make_fresh(args.make, quiet=False)
        print(f"fresh_disk: {args.make}")
        return 0
    found = drift(args.disk)
    if found:
        print("\n".join(describe(args.disk, found, limit=50)))
        return 1
    print(f"fresh_disk: {os.path.relpath(args.disk, REPO)} holds nothing a build did not put there")
    return 0


if __name__ == "__main__":
    sys.exit(main())
