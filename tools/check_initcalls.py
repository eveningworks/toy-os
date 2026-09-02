#!/usr/bin/env python3
"""tools/check_initcalls.py -- an init that is both an INITCALL and a
hand call, or an INITCALL at a level nothing walks.

`kernel/include/kernel/initcall.h` moved the boot's "run after X" inits
out of kernel_main() into a linker section: a subsystem declares
`INITCALL(foo_init, INIT_LEVEL)` beside foo_init() and the runner walks
each level in order. Two things can then go wrong silently, and this
fails the build on both:

  - kernel_main() (or any other file) still CALLS an init that is also
    declared -- so it runs twice, and the second run re-registers or
    re-resets whatever the first set up. The in-kernel KTEST cannot see
    a double run; the source can.
  - an INITCALL names a level that kernel_main() never passes to
    initcalls_run(), so it never runs. The KTEST catches this at boot;
    this catches it before the build finishes.

Exits non-zero with the offending file:line. Run by preflight.sh.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel")
MAIN = os.path.join(KERNEL, "core", "kernel.c")

DECL = re.compile(r"^\s*INITCALL\(\s*(\w+)\s*,\s*(INIT_\w+)\s*\)", re.M)
CALL = re.compile(r"^\s*(\w+_init)\(\)\s*;", re.M)
RUN = re.compile(r"initcalls_run\(\s*(INIT_\w+)\s*\)")


def c_files():
    for root, _dirs, files in os.walk(KERNEL):
        for f in files:
            if f.endswith(".c"):
                yield os.path.join(root, f)


def main():
    declared = {}   # fn -> (file, level)
    problems = []
    for path in c_files():
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in DECL.finditer(src):
            fn, level = m.group(1), m.group(2)
            line = src.count("\n", 0, m.start()) + 1
            if fn in declared:
                problems.append(f"{path}:{line}: {fn} is declared twice "
                                f"(also {declared[fn][0]})")
            declared[fn] = (f"{path}:{line}", level)

    main_src = open(MAIN, encoding="utf-8").read()
    walked = set(RUN.findall(main_src))
    for fn, (where, level) in sorted(declared.items()):
        if level not in walked:
            problems.append(f"{where}: {fn} is at {level}, which kernel_main() "
                            f"never walks (initcalls_run({level}) is missing)")

    for path in c_files():
        src = open(path, encoding="utf-8", errors="replace").read()
        for m in CALL.finditer(src):
            fn = m.group(1)
            if fn in declared and not path.endswith("_test.c"):
                line = src.count("\n", 0, m.start()) + 1
                problems.append(f"{path}:{line}: {fn}() is called by hand AND declared "
                                f"as an INITCALL at {declared[fn][0]} -- it would run twice")

    if problems:
        for p in problems:
            print(f"check_initcalls: {p}")
        return 1
    print(f"check_initcalls: ok -- {len(declared)} initcall(s), every level walked, "
          f"none also called by hand")
    return 0


if __name__ == "__main__":
    sys.exit(main())
