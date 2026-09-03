#!/usr/bin/env python3
"""tools/check_copy_user.py -- a vmm_copy_*_user() result tested as if it
were an errno.

`kernel/include/kernel/vmm.h`'s copy helpers return 1 on success and 0
on a bad user range -- never negative -- so `if (vmm_copy_from_user(...)
< 0)` is a branch that cannot be taken, and the handler below it runs on
whatever the kernel buffer held. Twelve socket syscalls shipped that
way; the header's comment did not stop them, so the build does.

Flags any `vmm_copy_from_user`, `vmm_copy_to_user` or
`vmm_copy_string_from_user` call whose result is compared with `<`,
`<=` or against a negative literal -- or DISCARDED (a bare statement),
which 26 sites did under a "validated above" comment. The idioms it accepts:
`!vmm_copy_...(...)`, `if (vmm_copy_...(...))`, or the result stored and
tested as a boolean. Exits non-zero with file:line. Run by preflight.sh.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ROOTS = ("kernel", "apps")

# The call, its balanced argument list (one level of nesting is enough
# for every caller here), then a comparison that only makes sense for a
# signed result.
BAD = re.compile(
    r"vmm_copy_(?:from|to|string_from)_user\s*\((?:[^()]|\([^()]*\))*\)"
    r"\s*(?:<=?\s*|[=!]=\s*-)")


# A copy whose result is DISCARDED: the statement is the bare call. Every
# one of the 26 that shipped sat under a "validated above" comment, and
# the header says the copy is the check.
DISCARDED = re.compile(r"^[ \t]*vmm_copy_(?:from|to|string_from)_user\s*\(", re.M)


def c_files():
    for root in ROOTS:
        for d, _dirs, files in os.walk(os.path.join(REPO, root)):
            for f in files:
                if f.endswith((".c", ".h")):
                    yield os.path.join(d, f)


def main():
    problems = []
    for path in c_files():
        src = open(path, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(path, REPO)
        for m in BAD.finditer(src):
            line = src.count("\n", 0, m.start()) + 1
            problems.append(f"{rel}:{line}: vmm_copy_*_user() returns 1/0, "
                            f"never negative -- test it with `!`")
        for m in DISCARDED.finditer(src):
            line = src.count("\n", 0, m.start()) + 1
            problems.append(f"{rel}:{line}: vmm_copy_*_user()'s result is "
                            f"discarded -- the copy IS the check; return -EFAULT")
    if problems:
        print("check_copy_user: a copy helper's result tested as an errno:")
        for p in problems:
            print("  " + p)
        return 1
    print("check_copy_user: ok -- every vmm_copy_*_user() result is tested "
          "as a boolean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
