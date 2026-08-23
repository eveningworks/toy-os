#!/usr/bin/env python3
"""Refuse a dispatch chain that has grown big enough to want a table.

The recurring shape in this codebase: something dispatches on a "kind"
-- a syscall number, a command name, a message type -- as one long
if/else or switch, and it grows by one branch per capability until the
file is unmaintainable and the handlers have nowhere to live but beside
it. `kernel/proc/syscall.c` reached 37 branches over 40 syscalls in
1,492 lines before it became a table, and its `strace` twin had already
drifted out of step with it.

The pattern that replaces it is one this project already uses well --
`display_driver`, `block_device`, `clocksource`, `struct setting`, the
`.ktests` section, and now `syscall_table.c`: a table or a registry,
with the handlers living wherever they belong. Nothing was missing
conceptually. What was missing was anything that NOTICED, because one
more `else if` is always cheaper than a table right up until it isn't.

So this is deliberately a very dumb check. It counts branches and
complains past a threshold. It cannot tell a dispatch chain that will
grow forever from a bounded one -- a keymap is bounded by the keyboard,
a PCI class table is data -- which is what waivers are for.

WAIVING: put a comment containing `dispatch-ok:` and a reason within a
few lines of the `switch` or the opening `if`. The reason is the point;
"this is bounded by X" or "tracked in docs/roadmap.md" are both fine
answers, and having to write one is the whole mechanism. A waiver is
as cheap to add as a branch, which is accepted: the goal is a conscious
decision, not a wall.

Usage:
    python3 tools/check_dispatch.py            # fail past the threshold
    python3 tools/check_dispatch.py --list     # the biggest chains, waived or not
    python3 tools/check_dispatch.py --max 30
"""
import argparse
import collections
import os
import re
import sys

DEFAULT_MAX = 20
WAIVER = "dispatch-ok:"
SKIP_DIRS = {".git", "build", "screenshots", "worktrees", ".claude"}
# Generated data, not code anybody dispatches through.
SKIP_FILES = ("font_ttf.c",)
ROOTS = ("kernel", "apps", "userland", "tools")

# VENDORED THIRD-PARTY SOURCE, WHICH THIS RULE CANNOT APPLY TO.
#
# The convention this script enforces is about how code in THIS project
# is structured, and its remedy is either to rewrite the chain as a table
# or to waive it with a comment. Both are edits, and `userland/ports/` is
# the one place where an edit is forbidden by policy: its value is that
# it is upstream byte for byte (see each port's README).
#
# So a 72-branch `switch` in Doom's p_spec.c is not a finding here. It is
# somebody else's code, written in 1993, and this project's opinion about
# dispatch tables has no standing over it -- the same reasoning that
# turns -Wall off for the same directory in the Makefile.
SKIP_ROOTS = (os.path.join("userland", "ports"),)


def _blank(src):
    """Comments and literals blanked, LENGTH AND LINE BREAKS PRESERVED.

    Offsets have to keep meaning after this or every reported line
    number is wrong, so each removed character becomes a space rather
    than disappearing.
    """
    out = list(src)

    def wipe(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    n = len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            wipe(i, j)
            i = j
        elif two == "//":
            j = src.find("\n", i)
            j = n if j < 0 else j
            wipe(i, j)
            i = j
        elif src[i] in "\"'":
            q = src[i]
            j = i + 1
            while j < n and src[j] != q:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            wipe(i, j)
            i = j
        else:
            i += 1
    return "".join(out)


# Generous, because an if/else chain's recorded position is its first
# `else if` -- the opening `if` and the waiver above it are already
# several lines back, and a waiver reason worth reading is a few lines
# long. `dispatch-ok:` is deliberate enough that a stray match is not a
# real risk.
WAIVER_LOOKBACK = 20


def _waived(src, pos, lines_before=WAIVER_LOOKBACK):
    """Is there a waiver comment just above `pos`, or ON ITS OWN LINE?

    Read from the RAW source. **The trailing-comment form counts**, and
    that is a fix rather than a nicety: `switch (x) { // dispatch-ok: ...`
    reads naturally, TWO constructs in this tree were already written
    that way, and both were silently unwaived -- the comment sits AFTER
    the switch keyword, so a look-backwards window could never see it.
    Nothing failed at the time because both were under the branch limit;
    the first one to grow past it reported a chain whose author believed
    they had waived it years earlier.
    """
    start = pos
    for _ in range(lines_before):
        nl = src.rfind("\n", 0, start)
        if nl < 0:
            start = 0
            break
        start = nl
    if WAIVER in src[start:pos]:
        return True
    # ...and the rest of the line the construct starts on.
    eol = src.find("\n", pos)
    if eol < 0:
        eol = len(src)
    return WAIVER in src[pos:eol]


def scan(path):
    raw = open(path, errors="replace").read()
    src = _blank(raw)
    found = []
    depth = 0
    line = 1
    # An else-if chain is not contiguous text -- each branch's body sits
    # between the branches -- so runs are grouped by BRACE DEPTH inside a
    # file. Two separate chains at the same depth in one function would
    # merge, which overcounts rather than undercounts; that is the safe
    # direction for a check like this.
    runs = collections.defaultdict(lambda: [0, 0, 0])  # depth -> [count, line, pos]
    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        if ch == "\n":
            line += 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        elif ch == "e" and re.match(r"else\s+if\s*\(", src[i:i + 40]):
            r = runs[depth]
            if r[0] == 0:
                r[1], r[2] = line, i
            r[0] += 1
            i += 4
            continue
        elif ch == "s" and re.match(r"switch\s*\(", src[i:i + 20]):
            brace = src.find("{", i)
            if brace >= 0:
                d = 0
                k = brace
                while k < n:
                    if src[k] == "{":
                        d += 1
                    elif src[k] == "}":
                        d -= 1
                        if d == 0:
                            break
                    k += 1
                cases = len(re.findall(r"\bcase\b", src[brace:k]))
                if cases:
                    found.append(("switch", line, cases, i))
        i += 1

    for _, (count, ln, pos) in runs.items():
        if count:
            found.append(("if/else", ln, count + 1, pos))

    return [(kind, ln, cnt, _waived(raw, pos)) for kind, ln, cnt, pos in found]


def walk():
    for root in ROOTS:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            if any(dirpath == r or dirpath.startswith(r + os.sep)
                   for r in SKIP_ROOTS):
                continue
            for f in filenames:
                if f.endswith(".c") and f not in SKIP_FILES:
                    yield os.path.join(dirpath, f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=DEFAULT_MAX)
    ap.add_argument("--list", action="store_true",
                    help="print the biggest chains, waived or not, and exit 0")
    args = ap.parse_args()

    rows = []
    for path in walk():
        for kind, line, count, waived in scan(path):
            rows.append((count, path, line, kind, waived))
    rows.sort(reverse=True)

    if args.list:
        for count, path, line, kind, waived in rows[:25]:
            print("%4d  %-8s %s%s:%d" % (count, kind, "(waived) " if waived else "",
                                          path, line))
        return 0

    bad = [r for r in rows if r[0] > args.max and not r[4]]
    if bad:
        print("check_dispatch: %d dispatch chain(s) over %d branches and not waived\n"
              % (len(bad), args.max))
        for count, path, line, kind, _ in bad:
            print("  %s:%d -- %s with %d branches" % (path, line, kind, count))
        print("\nMake it a table (kernel/proc/syscall_table.c is the worked example),")
        print("or waive it with a `%s <reason>` comment above the construct if the" % WAIVER)
        print("set of branches is genuinely bounded.")
        return 1

    waived = sum(1 for r in rows if r[0] > args.max and r[4])
    print("check_dispatch: ok -- no unwaived dispatch chain over %d branches "
          "(%d waived)" % (args.max, waived))
    return 0


if __name__ == "__main__":
    sys.exit(main())
