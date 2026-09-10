#!/usr/bin/env python3
"""Resolve drivers.conf into the source files that are built as modules.

The Makefile calls this once per build:

    python3 tools/drivers_conf.py drivers.conf
    kernel/drivers/net/e1000.c

Each `<name> = module` line names a driver by its source file's basename
under kernel/drivers/; the file is found by walking that tree, so there
is no name -> path table to keep true. `builtin` lines are accepted and
print nothing. An unknown name, an ambiguous one (two files with the
same basename) or a value other than the two words is an ERROR, exit
non-zero, because a misspelt module silently becoming builtin is the
kind of failure nothing else would notice.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVERS = os.path.join(REPO, "kernel", "drivers")


def parse(path):
    """(name, value, lineno) per non-comment line."""
    out = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            s = line.split("#", 1)[0].strip()
            if not s:
                continue
            if "=" not in s:
                sys.exit(f"{path}:{n}: expected `<name> = builtin|module`")
            name, value = (x.strip() for x in s.split("=", 1))
            if value not in ("builtin", "module"):
                sys.exit(f"{path}:{n}: {name!r}: value must be builtin or module, not {value!r}")
            out.append((name, value, n))
    return out


def find_source(name):
    hits = []
    for root, _dirs, files in os.walk(DRIVERS):
        if name + ".c" in files:
            hits.append(os.path.relpath(os.path.join(root, name + ".c"), REPO))
    return sorted(hits)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: drivers_conf.py <drivers.conf>")
    path = sys.argv[1]
    if not os.path.exists(path):
        sys.exit(f"drivers_conf: {path}: no such file")
    seen = {}
    sources = []
    for name, value, n in parse(path):
        if name in seen:
            sys.exit(f"{path}:{n}: {name} is listed twice (first at line {seen[name]})")
        seen[name] = n
        hits = find_source(name)
        if not hits:
            sys.exit(f"{path}:{n}: no kernel/drivers/**/{name}.c")
        if len(hits) > 1:
            sys.exit(f"{path}:{n}: {name} is ambiguous: {', '.join(hits)}")
        if value == "module":
            sources.append(hits[0])
    print(" ".join(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
