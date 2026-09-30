#!/usr/bin/env python3
"""A positive control, done safely: break the code on purpose, show the test goes red.

    python3 tools/mutate.py \\
        --edit kernel/fs/tfs3_write.c 'if (free_all_blocks(sbi, &victim) < 0)' \\
                                      'if (0 && free_all_blocks(sbi, &victim) < 0)' \\
        -- python3 tools/ktest_run.py --suite fs

CLAUDE.md: a green test proves nothing until it has been seen red. The
way to see it is to break the thing it covers, run it, and put the code
back -- by hand that is a `.bak` copy, an edit, a rebuild, a run, a
restore and a second rebuild, and the step that gets forgotten is the one
that matters: a restore missed ships the break, and a rebuild missed
leaves every later run testing the broken build while the source says
otherwise (predates.py's lesson).

So: each `--edit FILE OLD NEW` must match EXACTLY ONCE (a replacement
that could land in two places is refused, not guessed); the originals
are backed up to build/.mutate/ BEFORE anything changes, so even a
SIGKILL leaves them recoverable (`--recover`); the tree is restored in a
`finally` and checked byte for byte; and the build is re-run afterwards
so the artifacts match the source again.

Exit status: 0 when the command FAILED under the mutation (the control
fired -- the test can see this break), 1 when it passed anyway (the test
is blind to it), 2 when the mutated tree did not build (inconclusive).
"""
import argparse
import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP = os.path.join(REPO, "build", ".mutate")
INDEX = os.path.join(BACKUP, "index.json")


def sh(cmd, label, tail=8):
    print(f"mutate: [{label}] {cmd}", flush=True)
    r = subprocess.run(cmd, shell=True, cwd=REPO, capture_output=True, text=True)
    lines = (r.stdout + r.stderr).rstrip().splitlines()
    for ln in lines[-tail:]:
        print(f"    {ln}")
    print(f"mutate: [{label}] exit {r.returncode}", flush=True)
    return r.returncode


def recover():
    """Put back what an interrupted run left mutated. Returns how many."""
    if not os.path.exists(INDEX):
        return 0
    with open(INDEX) as fh:
        index = json.load(fh)
    for rel, saved in index.items():
        with open(os.path.join(BACKUP, saved), "rb") as src, \
                open(os.path.join(REPO, rel), "wb") as dst:
            dst.write(src.read())
        print(f"mutate: restored {rel}")
    for saved in index.values():
        os.remove(os.path.join(BACKUP, saved))
    os.remove(INDEX)
    return len(index)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--edit", nargs=3, action="append", metavar=("FILE", "OLD", "NEW"),
                    help="replace OLD (which must occur exactly once) with NEW; repeatable")
    ap.add_argument("--build", default="make iso",
                    help="how to build before the run and after the restore (default: make iso)")
    ap.add_argument("--recover", action="store_true",
                    help="restore the files an interrupted run left mutated, and exit")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="-- then the test command")
    args = ap.parse_args()

    if args.recover:
        n = recover()
        print(f"mutate: {n} file(s) restored" if n else "mutate: nothing to recover")
        return 0
    if os.path.exists(INDEX):
        sys.exit("mutate: an earlier run was interrupted with files still mutated -- "
                 "`python3 tools/mutate.py --recover` first")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not args.edit or not command:
        ap.error("need at least one --edit and a command after --")

    # Validate every edit before touching anything.
    originals = {}
    for rel, old, new in args.edit:
        path = os.path.join(REPO, rel)
        with open(path, "rb") as fh:
            data = originals.get(rel) or fh.read()
        n = data.count(old.encode())
        if n != 1:
            sys.exit(f"mutate: `{old}` occurs {n} times in {rel} -- it must occur exactly once")
        originals.setdefault(rel, data)

    os.makedirs(BACKUP, exist_ok=True)
    index = {}
    for i, (rel, data) in enumerate(originals.items()):
        saved = f"{i}.orig"
        with open(os.path.join(BACKUP, saved), "wb") as fh:
            fh.write(data)
        index[rel] = saved
    with open(INDEX, "w") as fh:
        json.dump(index, fh)

    # SIGTERM becomes an exception, so the finally below runs for it too.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))

    run_rc = build_rc = None
    try:
        current = dict(originals)
        for rel, old, new in args.edit:
            current[rel] = current[rel].replace(old.encode(), new.encode(), 1)
            print(f"mutate: {rel}: `{old}` -> `{new}`")
        for rel, data in current.items():
            with open(os.path.join(REPO, rel), "wb") as fh:
                fh.write(data)
        build_rc = sh(args.build, "build, mutated")
        if build_rc == 0:
            run_rc = sh(" ".join(shlex.quote(c) for c in command), "run, mutated", tail=12)
    finally:
        for rel, data in originals.items():
            with open(os.path.join(REPO, rel), "wb") as fh:
                fh.write(data)
            with open(os.path.join(REPO, rel), "rb") as fh:
                if hashlib.sha256(fh.read()).digest() != hashlib.sha256(data).digest():
                    sys.exit(f"mutate: {rel} DID NOT RESTORE -- backup in {BACKUP}")
        for saved in index.values():
            os.remove(os.path.join(BACKUP, saved))
        os.remove(INDEX)
        print(f"mutate: restored {len(originals)} file(s)")
        after = sh(args.build, "build, restored", tail=3)
        if after != 0:
            print("mutate: WARNING -- the restored tree did not build; the artifacts are "
                  "still the MUTATED ones")

    if build_rc != 0:
        print("mutate: INCONCLUSIVE -- the mutated tree did not build")
        return 2
    if run_rc != 0:
        print("mutate: CONTROL FIRED -- the command fails with this break, so it can see it")
        return 0
    print("mutate: CONTROL DID NOT FIRE -- the command passes with the code broken; "
          "it does not test what you think")
    return 1


if __name__ == "__main__":
    sys.exit(main())
