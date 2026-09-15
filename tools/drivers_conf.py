#!/usr/bin/env python3
"""Resolve drivers.conf: which drivers are modules, and how the build is tuned.

The Makefile calls this twice per build, for the file's two halves:

    python3 tools/drivers_conf.py drivers.conf
    kernel/drivers/net/e1000.c

    python3 tools/drivers_conf.py drivers.conf --options
    STRIP ?= 1
    KCMDLINE ?= video=1920x1080 nokaslr

**ONE FILE SAYS HOW THIS KERNEL IS BUILT**, which is FreeBSD's
`sys/amd64/conf/GENERIC` (its `device` and `options` lines together) and
Linux's `.config` (which drivers are modules, beside every other
setting). The driver half came first; the options half joined it rather
than starting a second file, because "what is in this kernel" and "how
is it built" are one question asked twice.

    <name> = builtin | module      a driver
    option <name> = <value>        a build setting

Each `<name> = module` line names a driver by its source file's basename
under kernel/drivers/; the file is found by walking that tree, so there
is no name -> path table to keep true. `builtin` lines are accepted and
print nothing. An unknown name, an ambiguous one (two files with the
same basename) or a value other than the two words is an ERROR, exit
non-zero, because a misspelt module silently becoming builtin is the
kind of failure nothing else would notice.

**THE SAME STRICTNESS APPLIES TO OPTIONS, AND FOR THE SAME REASON.** An
unknown option name is an error, not a line quietly ignored: `option
strp = no` doing nothing is exactly as invisible as a misspelt driver,
and the consequence -- a build that is not configured the way the file
says -- is worse, because the file is what somebody will believe.

**A COMMAND-LINE VARIABLE STILL WINS**, because the emitted assignments
are `?=` and make ranks a command-line variable above any makefile
assignment. So the file is the checkout's default and `make STRIP=0` is
this one build, which is the precedence anyone would expect.

Another file: `make iso DRIVERS_CONF=my.conf`.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVERS = os.path.join(REPO, "kernel", "drivers")

# EVERY OPTION, ITS MAKE VARIABLE AND WHAT IT ACCEPTS. A table rather
# than a chain of ifs, which is this project's rule for anything that
# grows by one entry at a time -- and it is what lets an unknown name be
# rejected with the list of real ones.
#
#   bool   yes/no/on/off/1/0  -> 1 or 0
#   int    digits
#   text   anything, including spaces
OPTIONS = {
    "strip":        ("STRIP", "bool",
                     "split the kernel's debug info into build/kernel.debug"),
    "compress":     ("COMPRESS", "bool",
                     "gzip the live filesystem image into the ISO"),
    "cmdline":      ("KCMDLINE", "text",
                     "boot words baked into both media (docs/boot-flags.md)"),
    "grub_timeout": ("GRUB_TIMEOUT", "int",
                     "seconds GRUB shows its menu; 0 boots straight through"),
    # THE ONLY OPTION HERE THAT REACHES THE NETWORK, which is why it is
    # called out rather than listed quietly. `yes` makes `make iso` fetch
    # the optional differently-licensed material -- and since this file
    # is TRACKED, committing `yes` would turn every clone's build into
    # one that downloads. The licence prompt still gates it, and a build
    # with no terminal is still refused rather than prompted.
    "extras":       ("EXTRAS", "bool",
                     "fetch optional differently-licensed material (NETWORK)"),
}

BOOL_TRUE = ("yes", "on", "true", "1")
BOOL_FALSE = ("no", "off", "false", "0")


def parse(path):
    """(kind, name, value, lineno) per non-comment line; kind is
    'driver' or 'option'."""
    out = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            s = line.split("#", 1)[0].strip()
            if not s:
                continue
            if "=" not in s:
                sys.exit(f"{path}:{n}: expected `<name> = builtin|module` "
                         f"or `option <name> = <value>`")
            name, value = (x.strip() for x in s.split("=", 1))
            if name.startswith("option ") or name.startswith("option\t"):
                opt = name[len("option"):].strip()
                if not opt:
                    sys.exit(f"{path}:{n}: `option` with no name")
                out.append(("option", opt, value, n))
                continue
            if value not in ("builtin", "module"):
                sys.exit(f"{path}:{n}: {name!r}: value must be builtin or module, "
                         f"not {value!r}")
            out.append(("driver", name, value, n))
    return out


def find_source(name):
    hits = []
    for root, _dirs, files in os.walk(DRIVERS):
        if name + ".c" in files:
            hits.append(os.path.relpath(os.path.join(root, name + ".c"), REPO))
    return sorted(hits)


def option_value(path, n, opt, value):
    """The make-side value, or exit with what was wrong."""
    var, kind, _help = OPTIONS[opt]
    if kind == "bool":
        low = value.lower()
        if low in BOOL_TRUE:
            return var, "1"
        if low in BOOL_FALSE:
            return var, "0"
        sys.exit(f"{path}:{n}: option {opt}: want yes or no, not {value!r}")
    if kind == "int":
        if not value.isdigit():
            sys.exit(f"{path}:{n}: option {opt}: want a number, not {value!r}")
        return var, value
    # text: anything, including empty and including spaces. A make
    # comment character would end the assignment early, so it is refused
    # rather than silently truncating the value.
    if "#" in value:
        sys.exit(f"{path}:{n}: option {opt}: a '#' cannot appear in a value")
    return var, value


def main():
    args = [a for a in sys.argv[1:]]
    want_options = "--options" in args
    if want_options:
        args.remove("--options")
    if len(args) != 1:
        sys.exit("usage: drivers_conf.py <drivers.conf> [--options]")
    path = args[0]
    if not os.path.exists(path):
        sys.exit(f"drivers_conf: {path}: no such file")

    seen_driver = {}
    seen_option = {}
    sources = []
    assignments = []

    for kind, name, value, n in parse(path):
        if kind == "option":
            if name not in OPTIONS:
                known = ", ".join(sorted(OPTIONS))
                sys.exit(f"{path}:{n}: unknown option {name!r}; the options "
                         f"are: {known}")
            if name in seen_option:
                sys.exit(f"{path}:{n}: option {name} is set twice "
                         f"(first at line {seen_option[name]})")
            seen_option[name] = n
            var, val = option_value(path, n, name, value)
            assignments.append(f"{var} ?= {val}")
            continue

        if name in seen_driver:
            sys.exit(f"{path}:{n}: {name} is listed twice "
                     f"(first at line {seen_driver[name]})")
        seen_driver[name] = n
        hits = find_source(name)
        if not hits:
            sys.exit(f"{path}:{n}: no kernel/drivers/**/{name}.c")
        if len(hits) > 1:
            sys.exit(f"{path}:{n}: {name} is ambiguous: {', '.join(hits)}")
        if value == "module":
            sources.append(hits[0])

    if want_options:
        # ONE PER LINE, into a file the Makefile includes -- not a
        # $(shell) whose newlines collapse to spaces, which would split
        # `KCMDLINE ?= video=1920x1080 nokaslr` into two assignments.
        print("\n".join(assignments))
    else:
        print(" ".join(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
