#!/usr/bin/env python3
"""What the dash port still needs from tolibc, measured rather than listed.

WHY THIS EXISTS
---------------
`docs/roadmap.md`'s "A ported POSIX shell" list was DERIVED twice -- once
from BusyBox ash, then re-derived from dash -- and was wrong in both
directions both times. It named `getrlimit`, `getpwnam`, `sysconf`,
`times`, `fnmatch` and `glob`, every one of which is an `AC_CHECK_FUNCS`
probe dash has a fallback for and never needs. It named none of the ten
missing headers, `SIGPIPE`, `uid_t`/`gid_t`, `DT_LNK`, the three errno
constants, or `htonl`.

The compiler knows the answer exactly. This runs it and prints what it
says, so the requirements list re-measures itself as tolibc grows
instead of being a list somebody has to keep true -- the same reasoning
that made `docs/decisions.md`'s index generated.

WHAT IT DOES
------------
On the HOST, with no guest and no network:

  1. Runs dash's six build-time generators (`mktokens`, `mkbuiltins`,
     `mkinit`, `mknodes`, `mksyntax`, `mksignames`, plus `cpp` over
     `builtins.def.in`) against a hand-written `config.h`. This is the
     half of the port that is already free, and a failure here is
     reported as a harness failure rather than a gap.
  2. Compiles all 32 sources against tolibc with **`-nostdinc`**, and
     reports which headers tolibc cannot provide.
  3. Compiles them again with those headers SHIMMED, and reports the
     symbol-level gap that remains.

**`-nostdinc` IS THE WHOLE POINT OF STEP 2, AND LEAVING IT OUT INVENTS A
PASS.** `USERLAND_CFLAGS` carries `-ffreestanding`, which does NOT stop
`#include <sys/ioctl.h>` finding `/usr/include`. Measured without it,
this harness reported 17 of 32 sources compiling and two tolibc "bugs"
that were glibc's declarations colliding with tolibc's. With it, 31 of
32 failed on missing headers. Any port built here needs the flag for the
same reason.

  4. With --link, resolves every undefined symbol in the compiled
     objects against what tolibc actually EXPORTS, which is the question
     compiling cannot answer: a declaration satisfies the compiler and
     only the linker knows whether anything stands behind it. It caught
     that dash needs `sysconf` at RUNTIME -- dash declares its own when
     HAVE_SYSCONF is unset, so it compiles and then calls sh_error().

WHAT IT DOES NOT CHECK, said plainly because an oversold check is worse
than none: that dash RUNS, or that it fits the ring-3 frame budget. It answers "what would the compiler
still refuse", which is the question the roadmap kept guessing at.

The shim headers below are MEASUREMENT SCAFFOLDING, not a port. They
exist so step 3 can see past a missing header to the symbols behind it;
what tolibc should actually put in each is a separate decision.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DASH = os.path.join(REPO, "userland", "ports", "dash")

# src/Makefile.am's dash_CFILES, plus the five generated .c files. Kept
# here rather than parsed out of the Makefile.am: a parser for automake
# is a second thing to get wrong, and this list changes when dash is
# re-vendored, which is exactly when somebody is reading this file.
SOURCES = """
alias.c arith_yacc.c arith_yylex.c cd.c error.c eval.c exec.c expand.c
histedit.c input.c jobs.c mail.c main.c memalloc.c miscbltin.c
mystring.c options.c parser.c redir.c show.c trap.c output.c
bltin/printf.c system.c bltin/test.c bltin/times.c var.c
builtins.c init.c nodes.c signames.c syntax.c
""".split()

# **THE REAL config.h, NOT A COPY OF IT.** This file used to carry its
# own, which meant the measurement and the build could answer autoconf's
# probes differently and nothing would notice -- the exact
# two-copies-drift shape this repo keeps deleting. It is port glue, so
# it lives with the port's other glue.
CONFIG_H_PATH = os.path.join(REPO, "userland", "backends", "dash", "config.h")

# SCAFFOLDING. Enough of each missing header to let the parse continue
# to the symbols behind it -- NOT a proposal for what tolibc should
# ship. Step 2 reports the header as missing either way.
SHIMS = {
    "sys/param.h": "#include <limits.h>\n#ifndef MAXPATHLEN\n#define MAXPATHLEN 4096\n#endif\n",
    "sys/time.h": "#include <time.h>\n",
    "sys/times.h": "struct tms { long tms_utime, tms_stime, tms_cutime, tms_cstime; };\nlong times(struct tms *);\n",
    "sys/resource.h": "struct rlimit { unsigned long rlim_cur, rlim_max; };\n"
                      "int getrlimit(int, struct rlimit *);\nint setrlimit(int, const struct rlimit *);\n",
    "sys/ioctl.h": "int ioctl(int, unsigned long, ...);\n",
    "wchar.h": "#include <stddef.h>\n",
    "wctype.h": "typedef int wctype_t;\n",
    "locale.h": "#define LC_ALL 0\nchar *setlocale(int, const char *);\n",
    "arpa/inet.h": "unsigned int htonl(unsigned int);\n",
    "getopt.h": "#include <unistd.h>\n",
}

CFLAGS = [
    "-std=gnu11", "-ffreestanding", "-fpie", "-mno-red-zone",
    "-mcmodel=small", "-O1", "-w", "-c", "-DBSD=1", "-DSHELL",
]


def run(cmd, cwd, capture=True):
    return subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                          capture_output=capture, text=True)


def build_generated(work):
    """Run dash's six generators. Returns None on success, else why not."""
    gen = ["builtins.c", "builtins.h", "init.c", "nodes.c", "nodes.h",
           "signames.c", "syntax.c", "syntax.h", "token.h", "token_vars.h"]
    steps = [
        ["sh", "mktokens"],
        *[["gcc", "-O1", "-w", "-o", h, h + ".c"]
          for h in ("mkinit", "mknodes", "mksignames")],
        ["gcc", "-O1", "-w", "-I.", "-o", "mksyntax", "mksyntax.c"],
        ["./mknodes", "nodetypes", "nodes.c.pat"],
        ["./mksyntax"],
        ["./mksignames"],
        ["gcc", "-E", "-x", "c", "-DBSD=1", "-DSHELL", "-include", "config.h",
         "-o", "builtins.def", "builtins.def.in"],
        ["sh", "mkbuiltins", "builtins.def"],
        # mkinit reads every non-generated source and emits init.c.
        ["./mkinit"] + [s for s in SOURCES if s not in gen],
    ]
    for step in steps:
        r = run(step, work)
        if r.returncode != 0:
            return f"{' '.join(step[:2])} failed:\n{r.stderr.strip()[:400]}"
    missing = [g for g in gen if not os.path.exists(os.path.join(work, g))]
    if missing:
        return "generators ran but produced nothing for: " + ", ".join(missing)
    return None


def compile_all(work, shimmed, drop_tolibc=False):
    """Compile every source. Returns (failed_files, stderr_text)."""
    gccinc = subprocess.run(["gcc", "-print-file-name=include"],
                            capture_output=True, text=True).stdout.strip()
    inc = ["-nostdinc", "-I" + gccinc]
    if shimmed:
        inc.append("-I" + os.path.join(work, "shim"))
    # NOTE: with shimmed=False this reports only the FIRST missing header
    # per file, which is why discover_headers() iterates rather than
    # trusting one pass.
    if not drop_tolibc:
        inc.append("-I" + os.path.join(REPO, "userland", "include"))
    inc += ["-I" + os.path.join(REPO, "kernel", "include", "api"),
            "-I" + os.path.join(REPO, "kernel", "include", "abi"),
            "-I" + os.path.join(REPO, "userland"),
            "-I."]
    failed, errs = [], []
    for src in SOURCES:
        # Kept, not sent to /dev/null: link_gap() reads them back, and a
        # second compile just to produce them would be a second thing to
        # keep in step with this one.
        out = os.path.join(work, src[:-2] + ".o")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        r = run(["gcc"] + CFLAGS + ["-include", "config.h"] + inc +
                [src, "-o", out], work)
        if r.returncode != 0:
            failed.append(src)
        errs.append(r.stderr)
    return failed, "\n".join(errs)


def missing_headers(text):
    return sorted(set(re.findall(r"fatal error: ([^:]+): No such file", text)))


def classify(text):
    """Group gcc's complaints into what tolibc would have to provide."""
    funcs = set(re.findall(r"implicit declaration of function [`'‘]([A-Za-z_]\w*)", text))
    types = set(re.findall(r"unknown type name [`'‘]([A-Za-z_]\w*)", text))
    undecl = set(re.findall(r"[`'‘]([A-Za-z_]\w*)['’] undeclared", text))
    structs = set(re.findall(r"invalid use of undefined type [`'‘](?:const )?struct ([A-Za-z_]\w*)", text))
    # "storage size of 'x' isn't known" names the VARIABLE, not the type,
    # so it adds nothing a struct line has not already said -- counting it
    # would report `b1`, `b2` and `statb` as three separate gaps.
    return funcs, types, undecl, structs


def measure(work, shimmed):
    failed, errs = compile_all(work, shimmed)
    return failed, errs, missing_headers(errs), classify(errs)


def write_shim(work, name):
    """Place one shim header. An unknown one gets an empty file, which is
    enough to see what is behind it and is reported as unshimmed."""
    p = os.path.join(work, "shim", name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write("#pragma once\n" + SHIMS.get(name, ""))


def discover_headers(work):
    """Every header tolibc cannot provide, not just the first one per file.

    A missing header is a FATAL error, so gcc stops there and never sees
    the includes below it -- one pass reported six of the ten. This
    shims what it found and asks again until nothing new appears.
    """
    shim = os.path.join(work, "shim")
    shutil.rmtree(shim, ignore_errors=True)
    os.makedirs(shim, exist_ok=True)
    found, rounds = [], 0
    while rounds < 12:
        rounds += 1
        _, errs = compile_all(work, shimmed=True)
        new = [h for h in missing_headers(errs) if h not in found]
        if not new:
            break
        found.extend(new)
        for h in new:
            write_shim(work, h)
    return sorted(found)


def link_gap(work):
    """Undefined symbols that nothing in tolibc exports.

    Compiling proves a DECLARATION exists; this proves something stands
    behind it. The two fail differently and at different times, which is
    why both stages are here.
    """
    objs = []
    for src in SOURCES:
        o = os.path.join(work, src[:-2] + ".o")
        if os.path.exists(o):
            objs.append(o)
    if not objs:
        return None, "nothing compiled"

    r = subprocess.run(["nm", "-u"] + objs, capture_output=True, text=True)
    need = {ln.split()[-1] for ln in r.stdout.splitlines()
            if ln.strip().startswith("U ")}

    have = set()
    for lib in ("build/lib/libc.so", "build/lib/libuapp.so",
                "build/userland/rt/sys.o", "build/userland/rt/stack_chk.o",
                "build/userland/rt/tls.o"):
        path = os.path.join(REPO, lib)
        if not os.path.exists(path):
            return None, f"{lib} not built -- run `make all` first"
        flag = "-D" if lib.endswith(".so") else ""
        cmd = ["nm"] + ([flag] if flag else []) + ["--defined-only", path]
        rr = subprocess.run(cmd, capture_output=True, text=True)
        for ln in rr.stdout.splitlines():
            parts = ln.split()
            if len(parts) >= 3:
                have.add(parts[-1])

    # Dash's own symbols resolve among the objects themselves.
    own = set()
    rr = subprocess.run(["nm", "--defined-only"] + objs, capture_output=True, text=True)
    for ln in rr.stdout.splitlines():
        parts = ln.split()
        if len(parts) >= 3:
            own.add(parts[-1])

    missing = sorted(s for s in need
                     if s not in have and s not in own
                     and s != "_GLOBAL_OFFSET_TABLE_")
    return missing, None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--link", action="store_true",
                    help="also resolve every undefined symbol against what "
                         "tolibc EXPORTS -- the question compiling cannot "
                         "answer")
    ap.add_argument("--verbose", action="store_true",
                    help="dump the raw compiler errors too")
    ap.add_argument("--positive-control", action="store_true",
                    help="drop a header tolibc DOES have and require the "
                         "gap to grow -- proves the measurement is live")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(DASH, "src")):
        print("dash_gap: no userland/ports/dash/src -- nothing vendored")
        return 2
    if not shutil.which("gcc"):
        print("dash_gap: no gcc on PATH")
        return 2

    work = tempfile.mkdtemp(prefix="dash_gap.")
    try:
        shutil.copytree(os.path.join(DASH, "src"), work, dirs_exist_ok=True)
        shutil.copyfile(CONFIG_H_PATH, os.path.join(work, "config.h"))

        why = build_generated(work)
        if why:
            print("dash_gap: HARNESS FAILURE -- the generators did not run")
            print("  " + why.replace("\n", "\n  "))
            return 2
        print("generators: ok -- all six ran, every generated file produced")

        bare_failed, _ = compile_all(work, shimmed=False)
        headers = discover_headers(work)
        failed, errs, _, (funcs, types, undecl, structs) = measure(work, shimmed=True)

        n = len(SOURCES)
        print(f"sources:    {n - len(bare_failed)} of {n} compile against tolibc alone")
        print(f"            {n - len(failed)} of {n} once the {len(headers)} missing "
              f"headers are shimmed")

        print(f"\nheaders tolibc does not provide ({len(headers)}):")
        for h in headers:
            note = "" if h in SHIMS else "   (no shim -- symbols behind it unmeasured)"
            print(f"  {h}{note}")

        for label, items in (("functions", funcs), ("types", types),
                             ("structs", structs), ("macros/constants", undecl)):
            if items:
                print(f"\nmissing {label} ({len(items)}):")
                print("  " + "  ".join(sorted(items)))

        if args.link:
            missing, why = link_gap(work)
            if why:
                print(f"\nlink: SKIPPED -- {why}")
            elif missing:
                print(f"\nsymbols nothing in tolibc exports ({len(missing)}):")
                print("  " + "  ".join(missing))
            else:
                print("\nlink: ok -- every undefined symbol resolves against "
                      "libc.so, libuapp.so and rt/")

        if args.verbose:
            print("\n--- raw errors (shimmed pass) ---\n" + errs)

        if args.positive_control:
            # **TAKE tolibc AWAY AND REQUIRE THE GAP TO COME BACK.**
            # The first version of this withdrew one SHIM, which worked
            # only while shims were still needed -- the day the last
            # header landed it reported "cannot run", leaving the
            # harness with no way to show it can fail at all. Removing
            # the userland/include path instead is a control that keeps
            # working however complete tolibc becomes, and it tests the
            # thing that actually matters: that this measurement depends
            # on tolibc rather than on something else on the path.
            ctl_failed, ctl_errs = compile_all(work, shimmed=True,
                                               drop_tolibc=True)
            if len(ctl_failed) >= len(SOURCES):
                print(f"\npositive control: ok -- without tolibc on the include "
                      f"path all {len(ctl_failed)} sources fail, so the pass "
                      f"above is tolibc's doing")
            else:
                print(f"\npositive control: FAILED -- {len(SOURCES) - len(ctl_failed)} "
                      f"source(s) still compiled with tolibc REMOVED. Something "
                      f"else on the include path is satisfying them, and the "
                      f"result above is not measuring what it claims.")
                return 1
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
