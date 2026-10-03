#!/usr/bin/env python3
"""Every signal abi/signal_abi.h defines has a name in kernel/lib/ksignal.c.

WHY THIS EXISTS
---------------
ksignal.c's SIGNALS table is what signal_name() and signal_from_name()
read, in both rings -- the shell's "killed by SIGx" line, `kill -NAME`,
`strace`. It is a hand-kept copy of the header, and it fell five
signals behind it: ILL, ABRT, FPE, PIPE and TTOU had numbers and no
names, so a pipeline whose reader went away printed "killed by SIG?".

WHAT IT CHECKS: every SIG* the header defines (read with
gen_signames.py's own parser, so dash's table and this agree on what a
signal is) has a `{ SIGFOO, "FOO" }` row, and that row's string is the
name without its prefix. A row for something the header does not define
would not compile, so that direction needs no check.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gen_signames import ABI, REPO, parse  # noqa: E402

TABLE = os.path.join(REPO, "kernel", "lib", "ksignal.c")
ROW = re.compile(r'\{\s*SIG([A-Z0-9]+)\s*,\s*"([^"]*)"\s*\}')


def main():
    names, _ = parse(ABI)
    rows = dict(ROW.findall(open(TABLE).read()))
    bad = []
    for num, name in sorted(names.items()):
        if name not in rows:
            bad.append(f"SIG{name} ({num}) has no row")
        elif rows[name] != name:
            bad.append(f'SIG{name} is named "{rows[name]}"')
    if bad:
        print("check_signals: kernel/lib/ksignal.c's SIGNALS table is behind "
              "abi/signal_abi.h:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"check_signals: {len(names)} signals, every one named")
    return 0


if __name__ == "__main__":
    sys.exit(main())
