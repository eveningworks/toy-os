#!/usr/bin/env python3
"""tools/check_deps.py -- prove the build's header dependency tracking works.

WHAT THIS IS
------------
Every object is compiled with `-MMD -MP`, which writes a `.d` file next
to it listing every header that object included. The Makefile pulls
those in with a single `-include` line, and that is what makes editing a
shared header rebuild everything that includes it instead of only the
files whose own `.c` changed.

That line is one glob away from silently doing nothing. It named six
directories by hand for a long time
(`$(BUILD)/core/*.d $(BUILD)/drivers/*.d ...`), and when source
discovery went recursive the kernel's objects moved from `build/core/`
to `build/kernel/core/`. The glob stopped matching. Nothing failed, no
warning was printed, and 88 of 161 `.d` files were simply not read --
so `touch kernel/include/kernel/process.h && make all` rebuilt nothing
at all and left every kernel object compiled against the previous
version of the header.

That failure mode is nasty out of proportion to its cause: a stale `.o`
built against an OLD struct layout sitting next to freshly-built ones is
not a compile error, it is an array indexed with the wrong stride at
runtime. This project has paid for it twice -- once as a Start menu
drawing function prologues as text, once as a filesystem honesty check
reading garbage and refusing a good backend (see CLAUDE.md).

So this tool checks the property directly, per build directory:

    for each directory under build/ that holds .d files:
        pick one object in it
        pick a repo header that object's .d says it depends on
        touch that header
    run `make all -n` ONCE
    every picked object must appear in what make says it would rebuild

Directories are discovered from `build/` rather than listed here, so a
new source directory is covered the moment it has been built once --
the same reasoning that made the Makefile's own line a `find`. Header
mtimes are restored afterwards, so a run leaves the tree exactly as it
found it and does not itself trigger a rebuild.

    python3 tools/check_deps.py           # exits 0 if every directory is covered
    python3 tools/check_deps.py -v        # show which header stood in for each

Run it after `make all` (it needs `.d` files to exist). It is part of
`tools/preflight.sh` and of CI.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BUILD = os.path.join(REPO, "build")

# Generated, and regenerated as a side effect of `make all` -- touching
# it would mean this tool's own probe races the build step that rewrites
# it. There is always another header to pick.
SKIP_HEADERS = {"kernel/include/api/version.h"}


def dep_headers(dpath):
    """Repo-relative .h files the first rule in a .d file depends on."""
    try:
        text = open(dpath).read()
    except OSError:
        return []
    # -MP appends a phony `header.h:` target per header after the real
    # rule; cut at the first blank-ish boundary by taking only up to the
    # second ':' -terminated line start. Simpler and robust enough: take
    # everything before the first line that is a bare `<path>:`.
    lines = []
    for line in text.replace("\\\n", " ").splitlines():
        if re.match(r"^\S+:\s*$", line) and lines:
            break
        lines.append(line)
    body = " ".join(lines)
    if ":" in body:
        body = body.split(":", 1)[1]

    out = []
    for tok in body.split():
        if not tok.endswith(".h"):
            continue
        rel = os.path.relpath(os.path.join(REPO, tok), REPO)
        if rel.startswith("..") or rel in SKIP_HEADERS:
            continue
        if os.path.exists(os.path.join(REPO, rel)):
            out.append(rel)
    return out


def collect_probes():
    """One (object, header, .d) probe per build directory holding .d files."""
    by_dir = {}
    for root, _dirs, files in os.walk(BUILD):
        ds = sorted(f for f in files if f.endswith(".d"))
        if ds:
            by_dir[root] = ds

    probes = []
    for root in sorted(by_dir):
        for d in by_dir[root]:
            dpath = os.path.join(root, d)
            heads = dep_headers(dpath)
            if not heads:
                continue
            obj = os.path.relpath(dpath[: -len(".d")] + ".o", REPO)
            if not os.path.exists(os.path.join(REPO, obj)):
                continue
            probes.append((os.path.relpath(root, REPO), obj, heads[0]))
            break
    return probes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print the header chosen to stand in for each directory")
    args = ap.parse_args()

    if not os.path.isdir(BUILD):
        print("check_deps: no build/ -- run `make all` first")
        return 2

    probes = collect_probes()
    if not probes:
        print("check_deps: no .d files under build/ -- run `make all` first")
        return 2

    saved = {}
    for _bdir, _obj, header in probes:
        p = os.path.join(REPO, header)
        saved.setdefault(header, os.stat(p)[7:9])  # (atime, mtime)

    try:
        for header in saved:
            os.utime(os.path.join(REPO, header), None)
        # `make all` does not reach every build directory. The ring-3
        # WM (M41 stage 4b) is built by its own on-demand `toywm`
        # target, because the port does not link yet and wiring an
        # incomplete program into the default build would turn this
        # check, preflight and CI red for the length of the migration.
        #
        # Asking the RIGHT target rather than skipping the directory:
        # a skipped directory is one whose dependency tracking nobody
        # checks, which is exactly the hole this tool exists to close --
        # `build/kernel` went unverified that way for months. Both
        # targets' plans are concatenated, so a probe is satisfied by
        # whichever one builds it.
        #
        # Drop `toywm` here when stage 4c folds the WM into `all`.
        planned = ""
        for target in ("all", "toywm"):
            r = subprocess.run(["make", target, "-n"], cwd=REPO,
                               capture_output=True, text=True)
            planned += r.stdout + r.stderr
    finally:
        for header, times in saved.items():
            os.utime(os.path.join(REPO, header), times)

    failures = []
    print(f"check_deps: {len(probes)} build director"
          f"{'y' if len(probes) == 1 else 'ies'}, "
          f"{len(saved)} header(s) touched\n")
    for bdir, obj, header in probes:
        ok = f"-o {obj}" in planned
        if not ok:
            failures.append((bdir, obj, header))
        mark = "ok  " if ok else "GAP "
        if args.verbose or not ok:
            print(f"  {mark} {bdir:<28} {obj:<44} via {header}")
        else:
            print(f"  {mark} {bdir:<28} {obj}")

    if failures:
        print("\ncheck_deps: FAILED -- these directories' .d files are not "
              "reaching make.")
        print("  Touching the header above rebuilt nothing in them, so a "
              "header edit leaves")
        print("  stale objects behind. Check the Makefile's `-include` line "
              "near the bottom.")
        return 1

    print("\ncheck_deps: every build directory honours its .d files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
