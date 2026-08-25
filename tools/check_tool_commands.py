#!/usr/bin/env python3
"""Every guest command a tool drives still exists.

WHAT THIS CATCHES, AND WHAT IT HONESTLY DOES NOT
------------------------------------------------
The tools in this directory drive the OS by typing commands at it and
reading what comes back. When a command is RENAMED or DELETED, every
tool that drives it breaks -- and because most of these tools are on
demand, nothing runs them and nobody finds out. Two were discovered red
by accident in one session, having rotted for an unknown period.

This check reads each tool for the command names it sends and verifies
each name still resolves, against the same authority `check_docs.py`
uses: the seeded /bin tree plus the builtins parsed out of both shells.
It is static, so it costs a second and needs no VM.

**It does not catch a command whose OUTPUT changed**, which is the other
half of that rot and the half that actually bit: `tools/fs_switch_test.py`
parsed `df` for a "Filesystem:" line, `df` moved to /bin and started
printing a table, and the name `df` stayed perfectly valid the whole
time. Nothing static can see that. `tools/ondemand_sweep.py` catches it
by RUNNING the tools, which is slower and is the real answer; this check
is the cheap subset that runs in the gate.

Said plainly because a check that is oversold is worse than no check:
green here means "no tool names a command that has vanished", not "the
tools work".
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Words that appear where a command name would, and are not commands.
# Each is here for a reason, not to silence a finding:
NOT_COMMANDS = {
    "gui",          # the debug console's own verb, not a shell command
    "sh",           # ...and its shell escape
    "help", "quit", "exit",
}

# How a tool sends a command. `sh <cmd>` goes to the guest's shell; the
# bare forms are vm.py's exec and the flow helpers.
PATTERNS = [
    re.compile(r'\bdbg\.send\(\s*f?"sh ([a-z_][a-z0-9_]*)'),
    re.compile(r'\bvm_exec\(\s*f?"([a-z_][a-z0-9_]*)'),
    re.compile(r'\bexec_one\(\s*f?"([a-z_][a-z0-9_]*)'),
    re.compile(r'\brun_command\(\s*f?"([a-z_][a-z0-9_]*)'),
]


def known_commands():
    """/bin programs plus both shells' builtins, plus `rescue`'s verbs."""
    names = set()
    seed_bin = os.path.join(REPO, "seed", "sync", "bin")
    if not os.path.isdir(seed_bin):
        return None   # no seed tree -- `make iso` has not run
    for n in os.listdir(seed_bin):
        if os.path.isfile(os.path.join(seed_bin, n)):
            names.add(n)

    def read(rel):
        p = os.path.join(REPO, rel)
        return open(p).read() if os.path.exists(p) else ""

    for m in re.finditer(r'k_strcmp\(cmd, "([a-z0-9_]+)"\)', read("apps/shell.c")):
        names.add(m.group(1))
    for m in re.finditer(r'seq\(cmd, "([a-z0-9_]+)"\)', read("userland/lib/tosh.c")):
        names.add(m.group(1))
    # `rescue` dispatches its own verbs; a tool writes `rescue df`, and
    # the name that has to exist is `rescue`.
    for m in re.finditer(r'k_strcmp\(\w+, "([a-z0-9_]+)"\)', read("apps/shell_rescue.c")):
        names.add("rescue")
        break
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    names = known_commands()
    if names is None:
        print("check_tool_commands: SKIP -- no seed tree (run `make iso`)")
        return 0

    problems = []
    seen = {}
    for fn in sorted(os.listdir(HERE)):
        if not fn.endswith(".py"):
            continue
        path = os.path.join(HERE, fn)
        try:
            src = open(path).read()
        except OSError:
            continue
        for pat in PATTERNS:
            for m in pat.finditer(src):
                cmd = m.group(1)
                if cmd in NOT_COMMANDS:
                    continue
                line = src[:m.start()].count("\n") + 1
                seen.setdefault(cmd, []).append((fn, line))
                if cmd not in names:
                    problems.append(f"{fn}:{line} drives `{cmd}`, which is neither a "
                                    f"/bin program nor a builtin -- renamed or deleted?")

    if args.verbose:
        for cmd in sorted(seen):
            where = ", ".join(f"{f}:{ln}" for f, ln in seen[cmd][:3])
            mark = "ok " if cmd in names else "GONE"
            print(f"  {mark}  {cmd:<12} {where}")

    if problems:
        print(f"check_tool_commands: {len(problems)} problem(s)\n")
        for p in problems:
            print(f"  {p}")
        print("\nA renamed command breaks every tool that drives it, and most of "
              "these tools are on demand -- nothing would have run them.")
        return 1
    print(f"check_tool_commands: ok -- {len(seen)} distinct command(s) driven by "
          f"tools, all still present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
