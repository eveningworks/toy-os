#!/usr/bin/env python3
"""Every test tool is named by a runner, or is exempt with a reason.

WHY THIS EXISTS
---------------
A tool in this directory that no runner names is run when somebody types
it, which is to say never. `tools/ondemand_sweep.py` was written because
two tools were found red by accident after rotting for an unknown
period -- and then FIVE MORE were found outside the sweep itself,
including `net_test.py` and one that was already failing. Adding a tool
to `tools/` does not add it to anything that runs it, and nothing
noticed the gap for months.

This is the same shape as `check_dispatch.py`, `check_widget_ops.py` and
`check_key_routing.py`: the repo already had the right pattern and no
check that the pattern was being followed.

WHAT IT CHECKS
--------------
Every `tools/*_test.py` is named by `preflight.sh`, `gui_regress.py` or
`ondemand_sweep.py`'s TOOLS table -- or appears in EXEMPT below with a
stated reason.

WHAT IT DOES NOT CHECK, said plainly because an oversold check is worse
than none: that the runner can actually run it (`ondemand_sweep.py`'s
`wants_vm` column is a real thing to get wrong, and getting it wrong
makes the tool fail on a missing socket rather than on its own
subject), and that the tool still passes. Green here means "nothing is
orphaned", not "the tools work".
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Tools deliberately named by no runner. A reason, not a silencer.
EXEMPT = {
    "mkpart_test.py":
        "despite the name a WRITER -- it takes a disk image argument, so "
        "running it bare is an argparse error rather than a result",
    "qmp_test.py":
        "a LIBRARY with an unfortunate name -- QMPSession and "
        "launch_qemu_cmd, imported by most of the tools above it",
}


def named_by_runners():
    """Every tool filename any runner mentions."""
    named = set()
    for runner in ("preflight.sh", "gui_regress.py", "ondemand_sweep.py"):
        path = os.path.join(HERE, runner)
        if not os.path.exists(path):
            sys.exit(f"check_tool_coverage: no {runner} -- cannot check coverage")
        # BY FILENAME ONLY. Every runner names its tools that way --
        # gui_regress's table holds "uidemo_test.py", preflight runs
        # "tools/ktest_run.py", the sweep's TOOLS column is a filename.
        # Matching a bare STEM as well would let any quoted word in any
        # runner count as coverage, which is a precondition wrong in the
        # PERMISSIVE direction: it manufactures coverage that does not
        # exist, and silence would then mean nothing.
        named |= set(re.findall(r"[a-z0-9_]+\.py", open(path).read()))
    return named


def main():
    named = named_by_runners()
    tools = sorted(f for f in os.listdir(HERE) if f.endswith("_test.py"))

    orphans = [t for t in tools if t not in named and t not in EXEMPT]
    stale = [t for t in EXEMPT if t not in tools]

    for t in orphans:
        print(f"  {t} is run by no runner -- add it to ondemand_sweep.py's "
              f"TOOLS, or to this script's EXEMPT with a reason")
    for t in stale:
        print(f"  EXEMPT names {t}, which no longer exists")

    if orphans or stale:
        print(f"\ncheck_tool_coverage: FAIL -- {len(orphans)} orphaned, "
              f"{len(stale)} stale exemption(s)")
        return 1
    print(f"check_tool_coverage: ok -- {len(tools)} test tool(s), all named "
          f"by a runner ({len(EXEMPT)} exempt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
