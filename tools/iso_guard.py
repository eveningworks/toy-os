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

The check is two mtime comparisons:

  1. `toy-os.iso` must be at least as new as `build/kernel.bin`
     -- catches `make all` without `make iso`.
  2. `build/kernel.bin` must be at least as new as the newest source
     file -- catches a build that FAILED, or was never run.

Both are needed. Only the first, and a failed `make iso` still boots a
consistent-but-old pair; only the second, and a successful `make all`
looks fine while the ISO lags.

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

# Where a `.c`/`.h`/`.asm` change has to reach the ISO to be tested.
# `tools/` and `docs/` are deliberately absent: editing a test script or
# a document does not need a rebuild, and treating it as if it did would
# train everyone to set the bypass.
SOURCE_DIRS = ("kernel", "apps", "userland")
SOURCE_SUFFIXES = (".c", ".h", ".asm", ".ld")

BYPASS_ENV = "TOYOS_ALLOW_STALE_ISO"


def _newest_source(repo: Path):
    """(path, mtime) of the newest source file, or (None, 0.0)."""
    newest, newest_mtime = None, 0.0
    for d in SOURCE_DIRS:
        root = repo / d
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
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
    iso = repo / iso_name
    kernel = repo / "build" / "kernel.bin"

    if not iso.exists():
        return [f"{iso_name} does not exist -- run `make iso`."]

    iso_m = iso.stat().st_mtime

    if kernel.exists():
        kern_m = kernel.stat().st_mtime
        if kern_m > iso_m:
            problems.append(
                f"build/kernel.bin is NEWER than {iso_name} "
                f"(by {kern_m - iso_m:.0f}s) -- a `make all` without `make iso`. "
                f"Every headless test boots the ISO, so it would test the previous build."
            )

    src, src_m = _newest_source(repo)
    if src is not None and kernel.exists() and src_m > kernel.stat().st_mtime:
        rel = src.relative_to(repo)
        problems.append(
            f"{rel} is NEWER than build/kernel.bin "
            f"(by {src_m - kernel.stat().st_mtime:.0f}s) -- the build did not run, "
            f"or it FAILED. Check `make iso`'s output for an error."
        )
    elif src is not None and not kernel.exists():
        problems.append("build/kernel.bin does not exist -- run `make all && make iso`.")

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
