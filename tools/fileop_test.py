#!/usr/bin/env python3
"""lib/ufileop through /bin/cp, /bin/mv and /bin/rm.

WHAT IS UNDER TEST
------------------
The copy loop and the tree walk are ONE implementation now, shared by
the three shell programs and by the File Manager's worker thread (see
`userland/lib/ufileop.h`). That sharing is the whole point -- it is what
keeps "one implementation of copying, testable as text at a prompt" true
after the GUI stopped spawning children -- and it is worth nothing
unless something actually tests it from a prompt. This is that.

Driven through `vm.py exec`, so what it asserts on is TEXT: the listing
after the operation, read back with `ls`, which is a different reader
from the program that did the work. A broken copy cannot make the two
agree.

Six properties, each with a failure the others would not catch:

1. **A tree copy reaches the leaves.** A walk that stopped at the first
   level would still produce a plausible-looking directory.
2. **An overwrite replaces the CONTENT**, not just the name -- checked
   by size, since a copy that created an empty file passes any check
   that only looks at the listing.
3. **A move across parents works**, which is the case a v1 TFS3 volume
   refuses at the rename (five journal credits, four slots) and the
   engine's copy-then-delete fallback exists for.
4. **A move does not leave the source behind**, which is what separates
   a move from a copy and is the half a naive fallback forgets.
5. **`rm -r` empties the tree AND removes the directories**, deepest
   first -- a walk that unlinked in discovery order would leave every
   directory behind, since a non-empty one cannot be unlinked.
6. **The two refusals still say what they refuse.** Copying a directory
   into itself never terminates, and the message is the only thing that
   tells a person which mistake they made.

RUN IT ON DEMAND against a throwaway copy of disk.img -- it creates and
deletes a tree under /ft and never touches anything else.
"""
import argparse
import os
import subprocess
import sys
from harness import Results  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ROOT = "/ft"


Result = Results


def run(cmd, instance=None):
    """One kernel-shell command; returns its output with the noise gone."""
    argv = [sys.executable, os.path.join(HERE, "vm.py")]
    if instance is not None:
        argv += ["--instance", str(instance)]
    argv += ["exec", cmd]
    out = subprocess.run(argv, cwd=REPO, capture_output=True, text=True).stdout
    keep = []
    for line in out.splitlines():
        s = line.strip()
        # The kernel narrates every ring-3 load and exit on the same
        # console; none of it is this command's output.
        if not s or s.startswith(("vm:", "syscall:", "elf_run:", "init:",
                                   "wm:", "win_surface:", "cursor:", "mouse:")):
            continue
        keep.append(s)
    return keep


def listing(path, instance=None):
    return sorted(n.rstrip("/") for n in run(f"ls {path}", instance)
                   if ":" not in n and "exit" not in n)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--instance", type=int, default=None)
    args = ap.parse_args()
    inst = args.instance
    res = Result()

    def sh(cmd):
        return run(cmd, inst)

    sh(f"rm -r {ROOT}")
    for c in (f"mkdir {ROOT}", f"mkdir {ROOT}/a", f"mkdir {ROOT}/a/deep"):
        sh(c)
    sh(f"cp /etc/timezones {ROOT}/a/one.txt")
    sh(f"cp /etc/timezones {ROOT}/a/deep/two.txt")

    # 1. the tree copy reaches the leaves
    sh(f"cp -r {ROOT}/a {ROOT}/b")
    res.check("cp -r copies the whole tree, leaves included",
              listing(f"{ROOT}/b", inst) == ["deep", "one.txt"] and
              listing(f"{ROOT}/b/deep", inst) == ["two.txt"],
              f"b={listing(f'{ROOT}/b', inst)} deep={listing(f'{ROOT}/b/deep', inst)}")

    # 2. an overwrite replaces the CONTENT. Sizes, not names: a copy
    #    that made an empty file would pass any listing check.
    def size(directory, name):
        # `ls -l` LISTS A DIRECTORY -- pointing it at a file does not
        # describe that file -- so the row is found by name.
        for line in sh(f"ls -l {directory}"):
            parts = line.split()
            if len(parts) >= 5 and parts[0] == "-" and parts[-1] == name:
                return int(parts[1])
        return -1

    before = size(f"{ROOT}/b", "one.txt")
    sh(f"cp /etc/resolv.conf {ROOT}/b/one.txt")
    after = size(f"{ROOT}/b", "one.txt")
    res.check("cp onto an existing file replaces its CONTENT",
              before > 0 and after > 0 and before != after,
              f"{before} bytes -> {after} bytes (both from /etc, different files)")

    # 3 + 4. a move across parents, and the source is gone
    sh(f"mv {ROOT}/b {ROOT}/a/moved")
    res.check("mv relocates a directory across parents",
              listing(f"{ROOT}/a/moved", inst) == ["deep", "one.txt"],
              f"moved={listing(f'{ROOT}/a/moved', inst)}")
    res.check("...and the source is GONE, which is what makes it a move",
              "b" not in listing(ROOT, inst), f"root={listing(ROOT, inst)}")

    # 5. rm -r, deepest first
    sh(f"rm -r {ROOT}/a/moved")
    res.check("rm -r removes the files AND the directories under them",
              "moved" not in listing(f"{ROOT}/a", inst),
              f"a={listing(f'{ROOT}/a', inst)}")

    # 6. the two refusals still name themselves
    into = " ".join(sh(f"cp -r {ROOT}/a {ROOT}/a/sub"))
    same = " ".join(sh("cp /etc/timezones /etc/timezones"))
    res.check("copying a directory into itself is refused, and says so",
              "into itself" in into, repr(into[:90]))
    res.check("copying a file onto itself is refused, and says so",
              "same file" in same, repr(same[:90]))

    sh(f"rm -r {ROOT}")
    print(f"\nfileop_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print(f"  FAILED: {f}")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
