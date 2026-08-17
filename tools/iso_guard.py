"""Refuse to boot a stale toy-os.iso.

THE BUG THIS EXISTS FOR, which has cost real time in many sessions:

    make all          # NOT `make iso` -- or `make iso` that FAILED
    python3 tools/ktest_run.py
    -> PASS, 256 tests

Both of those pass cleanly while testing the PREVIOUS build. Every
headless test here boots `toy-os.iso`, and nothing rebuilds it: `make
all` produces `build/kernel.bin` and stops, and a `make iso` that dies
on a compile error leaves the last good ISO sitting there. So the tests
run, the tests pass, and they are measuring code that no longer exists.

It does not fail loudly -- it fails as a clean PASS, which is the worst
possible direction. It is also what makes a positive control come back
green and send a session auditing the test instead of the build.

The check is two rounds of mtime comparison:

  1. Each SOURCE tree against the build output it actually feeds
     (kernel/ and apps/ -> build/kernel.bin, userland/ ->
     build/userland) -- catches a build that FAILED, or never ran.
  2. Each build output against the MEDIA the tests boot
     (build/kernel.bin -> toy-os.iso, build/userland -> build/.seeded)
     -- catches `make all` without `make iso`.

Both rounds are needed. Only the first, and a failed `make iso` still
boots a consistent-but-old pair; only the second, and a successful
`make all` looks fine while the ISO lags.

Pairing each tree with ITS OWN output matters more than it sounds: the
first version compared everything against build/kernel.bin and cried
wolf on the first userland-only edit. A guard that false-alarms is a
guard people switch off.

Deliberately mtime-based rather than a content hash: it costs one stat
per file, it is exactly the question `make` itself asks, and a false
alarm (touch a file, change nothing) costs one rebuild while a missed
alarm costs a session.

Bypass with TOYOS_ALLOW_STALE_ISO=1 when you MEAN it -- e.g. bisecting
against a deliberately older image. It prints that it is bypassing, so
a bypass left set in a shell cannot silently become the previous
behaviour.
"""

import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Each source tree, against the ARTIFACT IT ACTUALLY PRODUCES.
#
# Getting this pairing wrong is a false alarm, and a false alarm is not
# harmless: it teaches whoever sees it to set the bypass, which turns
# the check off permanently. The first version of this file compared
# every tree against build/kernel.bin and cried wolf on the first
# userland-only edit -- editing `userland/gui/foo.c` correctly rebuilds
# `build/userland/...` and correctly does NOT touch kernel.bin.
#
# `tools/` and `docs/` are absent on purpose: a test script or a
# document needs no rebuild.
SOURCE_TREES = (
    # (source dir, the build output it feeds, how to find that output)
    ("kernel", "build/kernel.bin", "file"),
    ("apps", "build/kernel.bin", "file"),
    ("userland", "build/userland", "tree"),
)
SOURCE_SUFFIXES = (".c", ".h", ".asm", ".ld")

# What `make iso` produces from those, and which build output has to be
# no newer than it. disk.img is re-seeded with the userland ELFs on
# every `make iso`, which is what makes it the right witness for them.
# The userland side uses build/.seeded, a stamp the Makefile's `seed`
# target touches unconditionally, rather than disk.img's own mtime:
# seeding is content-hash based, so a rebuild producing byte-identical
# ELFs correctly rewrites nothing and leaves the image untouched. Using
# the image would then report a no-op rebuild as staleness, and a guard
# that cries wolf is a guard people switch off.
ARTIFACT_PAIRS = (
    ("build/kernel.bin", "toy-os.iso"),
    ("build/userland", "build/.seeded"),
)

BYPASS_ENV = "TOYOS_ALLOW_STALE_ISO"


# Trees that feed NO seeded artifact, and so must not make the seed look
# stale. `userland/wm` is M41 stage 4b's ring-3 WM port: it is built by
# its own on-demand `make toywm` target, is not in USERLAND_PROGRAM_DIRS,
# and nothing copies it onto the disk image -- so building it correctly
# changes nothing the tests boot, and complaining would send a session
# chasing a re-seed that would fix nothing.
#
# This is the guard's own rule applied to a new tree: pair each tree with
# the artifact it actually feeds, because a guard that false-alarms is a
# guard people switch off. Remove this when 4c seeds the WM for real.
UNSEEDED = ("build/userland/wm",)


def _newest(root: Path, suffixes=None):
    """(path, mtime) of the newest file under `root`, or (None, 0.0)."""
    if root.is_file():
        return root, root.stat().st_mtime
    if not root.is_dir():
        return None, 0.0
    newest, newest_mtime = None, 0.0
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if suffixes is not None and path.suffix not in suffixes:
            continue
        if any(u in path.as_posix() for u in UNSEEDED):
            continue
        m = path.stat().st_mtime
        if m > newest_mtime:
            newest, newest_mtime = path, m
    return newest, newest_mtime


def check_iso_fresh(repo: Path = REPO, iso_name: str = "toy-os.iso"):
    """Return a list of complaint strings; empty means the ISO is current.

    Returns rather than raises so a caller can decide -- `vm.py` fails
    hard, but a tool booting a DIFFERENT image (the live or demo ISO)
    can ask about its own and ignore the answer.
    """
    problems = []

    if not (repo / iso_name).exists():
        return [f"{iso_name} does not exist -- run `make iso`."]

    # 1. Did the BUILD run? Each source tree against its own output.
    for src_dir, out_rel, _kind in SOURCE_TREES:
        src, src_m = _newest(repo / src_dir, SOURCE_SUFFIXES)
        if src is None:
            continue
        out, out_m = _newest(repo / out_rel)
        if out is None:
            problems.append(f"{out_rel} does not exist -- run `make all && make iso`.")
            continue
        if src_m > out_m:
            problems.append(
                f"{src.relative_to(repo)} is NEWER than {out_rel} "
                f"(by {src_m - out_m:.0f}s) -- that build did not run, or it FAILED. "
                f"Check `make iso`'s output for an error."
            )

    # 2. Did the build reach the MEDIA the tests actually boot? This is
    #    the `make all` without `make iso` case, which is the one that
    #    silently reports a clean pass against the previous build.
    for out_rel, media_rel in ARTIFACT_PAIRS:
        out, out_m = _newest(repo / out_rel)
        media = repo / media_rel
        if out is None or not media.exists():
            continue
        media_m = media.stat().st_mtime
        if out_m > media_m:
            problems.append(
                f"{out_rel} is NEWER than {media_rel} (by {out_m - media_m:.0f}s) "
                f"-- a `make all` without `make iso`. Every headless test boots "
                f"{iso_name} off the seeded image, so it would test the previous build."
            )

    return problems


def assert_iso_fresh(repo: Path = REPO, iso_name: str = "toy-os.iso"):
    """Exit non-zero with an explanation if the ISO is stale."""
    if os.environ.get(BYPASS_ENV) == "1":
        print(f"iso_guard: {BYPASS_ENV}=1 -- booting {iso_name} without checking it "
              f"is current.", file=sys.stderr)
        return

    problems = check_iso_fresh(repo, iso_name)
    if not problems:
        return

    print("", file=sys.stderr)
    print("iso_guard: REFUSING to boot a stale image.", file=sys.stderr)
    for p in problems:
        print(f"  * {p}", file=sys.stderr)
    print("", file=sys.stderr)
    print("  Fix:    make iso", file=sys.stderr)
    print(f"  Bypass: {BYPASS_ENV}=1 (only when you mean to test an older image)",
          file=sys.stderr)
    print("", file=sys.stderr)
    sys.exit(2)


if __name__ == "__main__":
    # Usable on its own: `python3 tools/iso_guard.py` exits 0 when the
    # ISO is current, 2 when it is not.
    assert_iso_fresh()
    print("iso_guard: toy-os.iso is current.")
