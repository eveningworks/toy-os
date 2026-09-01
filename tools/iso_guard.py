"""Refuse to boot a stale toy-os.iso.

THE BUG THIS EXISTS FOR, which has cost real time in many sessions:

    make all          # NOT `make iso` -- or `make iso` that FAILED
    python3 tools/ktest_run.py
    -> PASS, 256 tests

Both of those pass cleanly while testing the PREVIOUS build. Nothing in
`make all` reaches a boot medium: it produces `build/kernel.bin` and
stops, and a `make iso` that dies on a compile error leaves the last
good image sitting there. So the tests run, the tests pass, and they are
measuring code that no longer exists.

There are TWO media now -- the kernel lives on disk.img as well as in
the ISO (tools/install_grub.py), and `medium=` says which one is being
booted so the check compares against the one that will actually run.

It does not fail loudly -- it fails as a clean PASS, which is the worst
possible direction. It is also what makes a positive control come back
green and send a session auditing the test instead of the build.

The check is two rounds of mtime comparison:

  1. Each SOURCE tree against the build output it actually feeds
     (kernel/ and apps/ -> build/kernel.bin, userland/ ->
     build/userland) -- catches a build that FAILED, or never ran.
  2. Each build output against the MEDIA the tests boot
     (build/kernel.bin -> toy-os.iso, or -> build/.bootdisk on a disk
     boot; build/userland -> build/.seeded) -- catches `make all`
     without `make iso`.

Both rounds are needed. Only the first, and a failed `make iso` still
boots a consistent-but-old pair; only the second, and a successful
`make all` looks fine while the ISO lags.

There is a third, separate check for a COPY of disk.img passed with
`--disk` -- see check_disk_fresh(). That one WARNS rather than refuses,
because a copy is often deliberately old.

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

# The same pairing for a DISK boot. The kernel lives at /boot/kernel.bin
# inside disk.img's FAT32 partition now (tools/install_grub.py), and a
# file inside a FAT image has no mtime this can stat -- so the witness
# is build/.bootdisk, the stamp the Makefile's seed step touches after
# installing it. Exactly the reason build/.seeded exists for the
# userland ELFs, one artifact along.
DISK_ARTIFACT_PAIRS = (
    ("build/kernel.bin", "build/.bootdisk"),
    ("build/userland", "build/.seeded"),
)


# **WHICH ARTIFACT A SOURCE FEEDS IS ASKED, NOT ASSUMED.**
#
# Pairing by DIRECTORY is a guess, and a wrong guess is a false alarm --
# which the comment above already names as the thing to avoid, because it
# teaches whoever sees it to set the bypass. The guess WAS wrong, for
# `kernel/include/api/build_date.h`: it sits under kernel/ and is included
# by exactly one file, `userland/wm/desktop.c`, so it never rebuilds the
# kernel. gen_version.sh rewrites it whenever the DAY changes, so this
# refused a perfectly current image on the first build after every
# midnight, then self-healed as soon as anything touched the kernel --
# which is why nobody caught it.
#
# The build already knows the answer and writes it down: `-MMD` emits a
# `.d` beside every object naming the headers that object actually
# depends on, and `tools/check_deps.py` proves per build directory that
# the tracking is live. So the mapping is READ from those files rather
# than kept by hand. A per-file exception list would have fixed
# build_date.h and left the next generated header to rediscover this.
#
# The fallback matters as much as the rule: a source no `.d` mentions --
# added since the last build, or in a tree never built here -- keeps the
# directory pairing, which is the conservative answer. This can only make
# the guard quieter about a file the compiler positively placed elsewhere.
_DEP_ROOTS = (
    ("build/kernel.bin", "build/kernel"),
    ("build/kernel.bin", "build/apps"),
    ("build/userland", "build/userland"),
    ("build/userland", "build/userland-pic"),
)

_DEPS = None


def _dep_map(repo: Path):
    """{repo-relative source -> {artifacts that depend on it}}."""
    out = {}
    for artifact, sub in _DEP_ROOTS:
        root = repo / sub
        if not root.is_dir():
            continue
        for d in root.rglob("*.d"):
            try:
                text = d.read_text()
            except OSError:
                continue
            body = text.split(":", 1)[1] if ":" in text else ""
            for tok in body.replace("\\\n", " ").split():
                if not tok or tok.endswith(":"):
                    continue
                q = Path(tok) if os.path.isabs(tok) else (repo / tok)
                try:
                    rel = q.resolve().relative_to(repo).as_posix()
                except (ValueError, OSError):
                    continue
                out.setdefault(rel, set()).add(artifact)
    return out


def _feeds(path: Path, artifact: str, repo: Path):
    """Does `path` feed `artifact`? True when nothing is known about it."""
    global _DEPS
    if _DEPS is None:
        _DEPS = _dep_map(repo)
    try:
        rel = path.relative_to(repo).as_posix()
    except ValueError:
        return True
    known = _DEPS.get(rel)
    if not known:
        return True                 # never compiled here: keep the pairing
    return artifact in known


BYPASS_ENV = "TOYOS_ALLOW_STALE_ISO"


# Trees that feed NO seeded artifact, and so must not make the seed look
# stale -- the guard's own rule applied to a new tree: pair each tree
# with the artifact it actually feeds, because a guard that false-alarms
# is a guard people switch off.
#
# Empty, and that is the good outcome. It held `userland/wm` while the
# ring-3 WM was built by an on-demand target rather than by `make all`;
# the WM is an ordinary program now, so the ordinary pairing covers it.
UNSEEDED = ()


def _newest(root: Path, suffixes=None, artifact=None, repo: Path = REPO):
    """(path, mtime) of the newest file under `root`, or (None, 0.0).

    `artifact` narrows the walk to sources that actually feed it (see
    _feeds); without it every file counts, which is what the
    artifact-against-media comparisons want.
    """
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
        # Feeds some other artifact, and the compiler said so -- it is
        # checked when THAT artifact is checked, not here.
        if artifact is not None and not _feeds(path, artifact, repo):
            continue
        m = path.stat().st_mtime
        if m > newest_mtime:
            newest, newest_mtime = path, m
    return newest, newest_mtime


# Files the BUILD writes rather than a person. Their mtime moving is
# not evidence of a failed build, so the refusal above says something
# different about them. Name only files that are genuinely generated --
# putting a hand-written file here would silence a real staleness bug.
GENERATED_SOURCES = {
    "kernel/include/api/version.h",   # tools/gen_version.sh, on every git state change
}

# Files whose mtime carries NO information about whether the image is
# stale, so comparing them against it can only produce a false alarm.
#
# build_stamp.h is written by tools/gen_version.sh at Makefile PARSE
# time -- so EVERY make invocation rewrites it, `make clean-disk` and
# `make help` included, and those do not rebuild anything. It cannot
# indicate a failed build either: the one object that includes it is
# FORCEd, so any real build relinks the kernel after it.
#
# This is not the same as GENERATED_SOURCES above, which still reports
# (differently worded) because version.h moving CAN mean the image is
# behind.
IGNORED_SOURCES = {
    "kernel/include/api/build_stamp.h",
}


def check_iso_fresh(repo: Path = REPO, iso_name: str = "toy-os.iso",
                    medium: str = "cd"):
    """Return a list of complaint strings; empty means the boot media are current.

    Returns rather than raises so a caller can decide -- `vm.py` fails
    hard, but a tool booting a DIFFERENT image (the live or demo ISO)
    can ask about its own and ignore the answer.

    `medium` is 'cd' or 'disk'. It changes only what the kernel is
    compared against, and it has to: booting from the disk, a stale
    toy-os.iso is not evidence of anything, and a stale /boot/kernel.bin
    is exactly the silent-clean-pass this file exists to prevent.
    """
    problems = []

    if medium == "cd" and not (repo / iso_name).exists():
        return [f"{iso_name} does not exist -- run `make iso`."]

    # 1. Did the BUILD run? Each source tree against its own output.
    for src_dir, out_rel, _kind in SOURCE_TREES:
        src, src_m = _newest(repo / src_dir, SOURCE_SUFFIXES,
                             artifact=out_rel, repo=repo)
        if src is None:
            continue
        out, out_m = _newest(repo / out_rel)
        if out is None:
            problems.append(f"{out_rel} does not exist -- run `make all && make iso`.")
            continue
        if src_m > out_m:
            rel = src.relative_to(repo)
            if str(rel) in IGNORED_SOURCES:
                continue
            # A GENERATED file being newer is the ordinary case, not a
            # failure, and saying "check make iso's output for an error"
            # sends the reader hunting for one that is not there.
            #
            # version.h is the one that actually fires: it embeds
            # `git rev-parse --short HEAD` plus a dirty marker
            # (tools/gen_version.sh), so it is rewritten every time you
            # COMMIT or `git add` -- and the next build regenerates it
            # before relinking, which is exactly this comparison. It
            # cost four confused rebuilds in one session before anybody
            # read the script.
            if str(rel) in GENERATED_SOURCES:
                problems.append(
                    f"{rel} is NEWER than {out_rel} (by {src_m - out_m:.0f}s) "
                    f"-- but it is GENERATED, and it is regenerated whenever HEAD "
                    f"or the dirty flag changes (i.e. you have just committed or "
                    f"staged something). Nothing is wrong: run `make iso` again "
                    f"and it will be current."
                )
            else:
                problems.append(
                    f"{rel} is NEWER than {out_rel} "
                    f"(by {src_m - out_m:.0f}s) -- that build did not run, or it FAILED. "
                    f"Check `make iso`'s output for an error."
                )

    # 2. Did the build reach the MEDIA the tests actually boot? This is
    #    the `make all` without `make iso` case, which is the one that
    #    silently reports a clean pass against the previous build.
    for out_rel, media_rel in (DISK_ARTIFACT_PAIRS if medium == "disk" else ARTIFACT_PAIRS):
        out, out_m = _newest(repo / out_rel)
        media = repo / media_rel
        if out is None or not media.exists():
            continue
        media_m = media.stat().st_mtime
        if out_m > media_m:
            booted = "disk.img" if medium == "disk" else iso_name
            problems.append(
                f"{out_rel} is NEWER than {media_rel} (by {out_m - media_m:.0f}s) "
                f"-- a `make all` without `make iso`. Every headless test boots "
                f"{booted}, so it would test the previous build."
            )

    return problems


def check_disk_fresh(disk: str, repo: Path = REPO):
    """Complain if `disk` is a COPY taken before the last seed.

    THE BUG THIS EXISTS FOR, measured 2026-08-19. Testing against a copy
    of disk.img is the documented way to avoid QEMU's write lock and to
    stop `make iso` re-seeding the image underneath a running VM
    (CLAUDE.md). But `make iso` re-seeds the REAL disk.img with the newly
    built /bin binaries, so a copy taken before a rebuild still holds the
    OLD ones -- and the VM then runs the NEW kernel from the ISO against
    the OLD userland.

    THAT GOT WORSE, and the fix is the same. The kernel is installed
    onto disk.img too now, so a copy is a BOOT MEDIUM: a stale one runs
    the previous kernel as well as the previous userland, which is a
    consistent earlier build masquerading as this one -- and therefore
    even harder to spot than a mismatched pair. The message says which
    of the two you are looking at.

    That reads exactly like a bug in the app. It cost a session a wrong
    conclusion in the worst possible place: a POSITIVE CONTROL, lowering
    a kernel limit to prove /bin/ls would report a truncated listing. The
    guest ran the previous ls, which compared against a limit it had
    never been given, so the message did not appear -- and a control that
    fails reads as "the feature is broken", not "the fixture is stale".

    Returns complaints rather than raising: a copy is often deliberately
    old (a fixture staged by tfs3_writer.py, an image kept for a
    reproduction), so this is a WARNING at the call site, not a refusal.
    """
    problems = []
    d = Path(disk)
    if not d.exists():
        return [f"{disk} does not exist."]

    # The repo's own disk.img is re-seeded by `make iso` itself, so it is
    # covered by the ARTIFACT_PAIRS check above and never stale here.
    try:
        if d.resolve() == (repo / "disk.img").resolve():
            return []
    except OSError:
        return []

    seeded = repo / "build" / ".seeded"
    if not seeded.exists():
        return []
    lag = seeded.stat().st_mtime - d.stat().st_mtime
    if lag > 0:
        # WHAT A STALE COPY COSTS YOU GREW. It has always held the
        # previous /bin binaries; now that the kernel is installed onto
        # the image too (tools/install_grub.py), a copy the guest BOOTS
        # runs the previous kernel as well -- so the mismatch is no
        # longer new-kernel-against-old-userland, it is a whole previous
        # build wearing the current one's name.
        import install_grub
        what = ("the previous kernel AND /bin binaries -- the guest would run "
                "an entire earlier build" if install_grub.is_bootable(str(d))
                else "the previous /bin binaries, so the guest would run the "
                     "new kernel against the old userland")
        problems.append(
            f"{disk} was copied {lag:.0f}s BEFORE the last seed -- it holds "
            f"{what}. Re-copy it: "
            f"cp --reflink=auto --sparse=always disk.img {disk}"
        )
    return problems


def warn_if_disk_stale(disk, repo: Path = REPO):
    """Print check_disk_fresh()'s complaints to stderr. Never fatal."""
    if not disk or os.environ.get(BYPASS_ENV) == "1":
        return
    for p in check_disk_fresh(disk, repo):
        print(f"iso_guard: WARNING -- {p}", file=sys.stderr)


def assert_iso_fresh(repo: Path = REPO, iso_name: str = "toy-os.iso",
                     medium: str = "cd"):
    """Exit non-zero with an explanation if the boot media are stale."""
    if os.environ.get(BYPASS_ENV) == "1":
        print(f"iso_guard: {BYPASS_ENV}=1 -- booting {iso_name} without checking it "
              f"is current.", file=sys.stderr)
        return

    problems = check_iso_fresh(repo, iso_name, medium)
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
