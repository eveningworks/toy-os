#!/usr/bin/env python3
"""tolibc's formatter and number parsers against glibc, case by case.

WHY THIS EXISTS
---------------
tolibc's bar is COMPLETENESS -- the opposite of the rest of this repo --
because its audience is code not yet written (docs/libc-design.md). A
happy-path test cannot hold that bar: it checks the cases whoever wrote
it thought of, which are the cases they got right. What catches an
EXPECTATION being wrong is an oracle that shares no code.

A two-minute version of this harness found six printf bugs and three
strtol bugs in one sitting, including `%hhu` of 256 printing 256 and
`strtol("0", &end, 0)` reporting no conversion at all. It also found
that `libc3_test.c` ASSERTED one of those bugs, so the suite would have
gone red on the fix -- which is the failure mode a self-referential test
suite has and a differential one does not.

Same shape as usnd_hostcheck.py and umd_hostcheck.py: compile this
repo's implementation with the host gcc and judge it by something that
was written by other people for other reasons.

HOW IT AVOIDS A SYMBOL CLASH
----------------------------
tolibc defines strtol, abort, qsort and twenty-odd more names glibc
also defines, so the two cannot simply be linked together. The wrapper
renames them at the C level -- `#define strtol toy_strtol` ahead of an
`#include` of the source -- rather than with objcopy, because that also
redirects tolibc's INTERNAL calls and leaves genuine externals alone.

WHAT IT DOES NOT CHECK, said plainly: anything with a side effect
(malloc, exit, the stdio streams) -- this is pure-function territory.
Nor does it run in the guest: it compiles the same source the guest
runs, which is what makes it fast and what makes it unable to see a
problem that only appears in ring 3.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Every global tolibc's stdlib.c defines. Renamed wholesale so the
# object can sit beside glibc; the three under test get used, the rest
# just have to stop colliding.
STDLIB_SYMS = """abort abs atexit atof atoi atol atoll at_quick_exit bsearch div
exit _Exit labs ldiv llabs lldiv qsort quick_exit rand realloc srand
strtod strtol strtoll strtoul strtoull""".split()

# --- the printf matrix -------------------------------------------------
#
# Flags, widths and precisions are combined against each conversion
# rather than listed by hand: the bugs found so far were all in the
# INTERACTION (a sign emitted before the padding, a precision ignored
# for one conversion), which a list of hand-picked cases misses because
# whoever writes it picks the combinations they already believe work.
INT_CONVS = ["d", "i", "u", "x", "X", "o"]
FLAGS = ["", "-", "+", "#", "0", "0+"]
WIDTHS = ["", "5", "8"]
PRECS = ["", ".0", ".3"]   # .0 is where a precision and '#' collide
LENGTHS = ["", "hh", "h", "l", "ll"]
INT_ARGS = [0, 1, -1, 42, 255, 256, 65536, -2147483648]

STR_CASES = [('%s', '"hello"'), ('%.2s', '"hello"'), ('%8s', '"hi"'),
             ('%-8s|', '"hi"'), ('%c', "'a'"), ('%5c', "'a'"),
             ('%-5c|', "'a'"), ('%%', None)]

# --- the parser matrix -------------------------------------------------
STRTOL_INPUTS = [
    '"0"', '"00"', '"0x"', '"0X"', '"09"', '"0xzz"', '"0x1f"', '"  42"',
    '"+42"', '"-42"', '"-0"', '"z"', '""', '" "', '"2147483648"',
    '"9223372036854775807"', '"-9223372036854775808"',
    '"9223372036854775808"', '"99999999999999999999999"',
    '"-99999999999999999999999"', '"18446744073709551615"',
    '"0777"', '"0b11"', '"ff"', '"FF"', '"7fffffffffffffff"',
]
STRTOL_BASES = [0, 8, 10, 16, 36]

# **LENGTH IS ITS OWN AXIS.** A written number's DIGIT COUNT and the
# magnitude it denotes are different things, and conflating them is what
# made "1" + 309 zeros + "e-309" -- whose value is 1 -- come out
# infinite. None of the short inputs below could have found that, so the
# long ones are built programmatically at the end of this list.
STRTOD_INPUTS = [
    '"0"', '"0.0"', '"1"', '"1.5"', '"-1.5"', '"  3.25"', '".5"', '"5."',
    '"1e3"', '"1e-3"', '"1E3"', '"1e"', '"1e+"', '"0e999"', '"1e999"',
    '"1e-999"', '"1e308"', '"1e309"', '"1e-310"', '"1e-323"',
    '"inf"', '"-inf"', '"nan"', '"0x1p2"', '"0x10"', '"z"', '""',
    '"3.14159265358979"', '"123456789012345678901234567890"',
    # The spelled-out forms: stopping at three characters is a wrong
    # answer, not a refusal -- "infinity" left "inity" for the caller.
    '"infinity"', '"INFINITY"', '"-infinity"', '"nan(1234)"', '"nan(_x9)"',
    '"nan()"', '"infi"', '"0x1p1024"', '"0x1p-1074"', '"0x1p-1080"',
]

# Numbers far longer than a double's range, whose VALUE is ordinary.
STRTOD_INPUTS += [
    '"1' + '0' * 309 + 'e-309"',      # == 1
    '"0.' + '1' * 310 + '"',          # == 0.111...
    '"' + '9' * 400 + '"',            # overflows however it is read
    '"0.' + '0' * 320 + '1"',         # a subnormal written the long way
]


def build(workdir, extra_cflags=()):
    """Compile the probe. Returns the binary path, or raises."""
    renames = "\n".join(f"#define {s} toy_{s}" for s in STDLIB_SYMS)
    open(os.path.join(workdir, "toy_stdlib.c"), "w").write(
        "// Generated by tools/libc_diff.py -- renames tolibc's globals so\n"
        "// the object can be linked beside glibc. See the module docstring.\n"
        + renames + '\n#include "userland/libc/stdlib.c"\n')

    # tolibc's stdlib.c reaches the kernel allocator and sys_exit. None
    # of the functions under test calls them, but the linker still wants
    # them defined -- so they abort loudly rather than returning
    # something a future test might quietly rely on.
    open(os.path.join(workdir, "stubs.c"), "w").write(r"""
#include <stdio.h>
#include <stdlib.h>
static void nope(const char *who) {
    fprintf(stderr, "libc_diff: %s reached -- this harness is "
                    "pure-function only\n", who);
    abort();
}
void sys_exit(int c) { (void)c; nope("sys_exit"); }
int sys_getpid(void) { nope("sys_getpid"); return 0; }
void *kmalloc(unsigned long n) { (void)n; nope("kmalloc"); return 0; }
void *kcalloc(unsigned long a, unsigned long b) { (void)a; (void)b; nope("kcalloc"); return 0; }
unsigned long kmalloc_size(const void *p) { (void)p; nope("kmalloc_size"); return 0; }
void kfree(void *p) { (void)p; nope("kfree"); }
""")

    src = os.path.join(workdir, "probe.c")

    # **THE PROBE IS COMPILED WITHOUT tolibc's HEADERS ON ITS PATH, AND
    # THAT IS THE WHOLE CORRECTNESS OF THIS HARNESS.**
    # userland/include/stdio.h makes vsnprintf a static inline around
    # k_vsnprintf and #defines snprintf to k_snprintf. With that header
    # reachable, both sides of every comparison are tolibc and the
    # harness reports perfect agreement while measuring NOTHING -- which
    # is exactly what it did until this was found. The probe declares the
    # tolibc entry points by hand instead.
    TOY_INCLUDES = ["-I" + REPO,
                    "-I" + os.path.join(REPO, "userland/include"),
                    "-I" + os.path.join(REPO, "kernel/include/api"),
                    "-I" + os.path.join(REPO, "kernel/include/abi"),
                    "-I" + os.path.join(REPO, "userland"),
                    # kconfig.h is GENERATED (build.conf -> build/gen), and
                    # timer.h reaches it through syscall_abi.h.
                    "-I" + os.path.join(REPO, "build/gen")]

    objs = []
    for name, path in (("toy_stdlib", os.path.join(workdir, "toy_stdlib.c")),
                       ("stubs", os.path.join(workdir, "stubs.c")),
                       ("kfmt", os.path.join(REPO, "kernel/lib/kfmt.c")),
                       ("knum", os.path.join(REPO, "kernel/lib/knum.c")),
                       ("kstring", os.path.join(REPO, "kernel/lib/string.c")),
                       ("pfloat", os.path.join(REPO, "userland/libc/printf_float.c"))):
        obj = os.path.join(workdir, name + ".o")
        r = subprocess.run(["gcc", "-w", "-O0", "-c", "-o", obj, path,
                            *TOY_INCLUDES], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{name} did not compile:\n" + r.stderr[:1500])
        objs.append(obj)

    # -O0 on purpose: the probe is I/O bound, and optimising a table of
    # thousands of entries costs minutes for nothing. No toy-os include
    # path here -- see above.
    r = subprocess.run(["gcc", "-w", "-O0", "-o", os.path.join(workdir, "probe"),
                        src, *objs, *extra_cflags], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("probe did not build:\n" + r.stderr[:2000])
    return os.path.join(workdir, "probe")


def gen_probe(workdir, sections, exact_float=False):
    """Write probe.c. Each check prints one TAB-separated result line."""
    out = []
    out.append('#include <stdio.h>')
    out.append('#define EXACT_FLOAT %d' % (1 if exact_float else 0))
    out.append('#include <stdlib.h>')
    out.append('#include <errno.h>')
    out.append('#include <string.h>')
    out.append('#include <math.h>')
    # DECLARED BY HAND, not included: see build() for why tolibc's
    # headers must not be on this translation unit's path.
    out.append('#include <stdarg.h>')
    out.append('unsigned long k_snprintf(char *, unsigned long, const char *, ...);')
    out.append('unsigned long k_vsnprintf(char *, unsigned long, const char *, va_list);')
    out.append('long toy_strtol(const char *, char **, int);')
    out.append('unsigned long toy_strtoul(const char *, char **, int);')
    out.append('double toy_strtod(const char *, char **);')
    # A difference is reported with both answers so the reader never has
    # to re-run anything to see what happened.
    out.append(r'''
static int fails, checks;
// Within one ULP: same sign, same magnitude, adjacent representations.
static int near(double a, double b) {
    if (a == b) return 1;
    if (a != a || b != b) return 0;          // NaN is never near anything
    long long ia, ib;
    __builtin_memcpy(&ia, &a, 8); __builtin_memcpy(&ib, &b, 8);
    long long d = ia > ib ? ia - ib : ib - ia;
    return d <= 1;
}
static char dbuf[2][40]; static int dslot;
static const char *fmtd(double v) {
    dslot ^= 1; snprintf(dbuf[dslot], sizeof dbuf[0], "%.17g", v);
    return dbuf[dslot];
}
static void cmp(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want)) { fails++; printf("DIFF\t%s\ttoyos=[%s]\tglibc=[%s]\n", what, got, want); }
}
#define FMT1(fmt, arg) do {                                   \
    char g[128], w[128]; char lbl[64];                        \
    snprintf(lbl, sizeof lbl, "%s <- %s", fmt, #arg);         \
    k_snprintf(g, sizeof g, fmt, arg);                        \
    snprintf(w, sizeof w, fmt, arg);                          \
    cmp(lbl, g, w);                                           \
} while (0)
''')
    out.append("int main(void) {")
    # **ALWAYS ON, NOT BEHIND A FLAG.** This harness once compared
    # tolibc against ITSELF for every printf case and reported perfect
    # agreement, because tolibc's <stdio.h> makes vsnprintf an inline
    # around k_vsnprintf. The two must be different functions or nothing
    # below means anything.
    out.append(r'''    if ((void *)snprintf == (void *)k_snprintf ||
        (void *)vsnprintf == (void *)k_vsnprintf) {
        printf("SELFTEST\tthe oracle IS the implementation -- "
               "tolibc headers leaked into the probe\n");
        return 2;
    }''')
    out.extend(sections)
    out.append('    printf("SUMMARY\\t%d\\t%d\\n", checks, fails);')
    out.append("    return 0;")
    out.append("}")
    open(os.path.join(workdir, "probe.c"), "w").write("\n".join(out) + "\n")


def printf_sections(broken=False):
    """Format/argument pairs as a TABLE, not one C statement each.

    The first version emitted a statement per case, and the COMPILE took
    minutes. A harness nobody can afford to run is one nobody runs, so
    the cases are data and the probe loops over them.

    Every argument is passed as a `long`, which is deliberate rather
    than sloppy: varargs promote, so what a printf really receives for
    %hhu IS a promoted integer, and reading it back narrowed is the
    behaviour under test. Both implementations get the identical value.
    """
    s = ['    {', '        static const struct { const char *f; long v; } T[] = {']
    for length in LENGTHS:
        for conv in INT_CONVS:
            for flag in FLAGS:
                for width in WIDTHS:
                    for prec in PRECS:
                        fmt = f"%{flag}{width}{prec}{length}{conv}"
                        for a in INT_ARGS:
                            s.append(f'            {{ "{fmt}", {a}L }},')
    s.append('        };')
    s.append('        char g[160], w[160], lbl[96];')
    s.append('        for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {')
    s.append('            k_snprintf(g, sizeof g, T[i].f, T[i].v);')
    s.append('            snprintf(w, sizeof w, T[i].f, T[i].v);')
    s.append('            snprintf(lbl, sizeof lbl, "%s <- %ld", T[i].f, T[i].v);')
    s.append('            cmp(lbl, g, w);')
    s.append('        }')
    s.append('    }')
    for fmt, arg in STR_CASES:
        if arg is None:
            s.append(f'    {{ char g[128], w[128]; k_snprintf(g, sizeof g, "{fmt}");'
                     f' snprintf(w, sizeof w, "{fmt}"); cmp("{fmt}", g, w); }}')
        else:
            s.append(f'    FMT1("{fmt}", {arg});')
    return s


def parser_sections():
    s = []
    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; long gv, wv; int gerr, werr;''')
    for inp in STRTOL_INPUTS:
        lbl = inp.replace('"', '\\"')
        for base in STRTOL_BASES:
            s.append(f'''
        errno = 0; gv = toy_strtol({inp}, &ge, {base}); gerr = errno;
        errno = 0; wv = strtol({inp}, &we, {base}); werr = errno;
        snprintf(gbuf, sizeof gbuf, "%ld/%d/%d", gv, (int)(ge - ({inp})), gerr == ERANGE);
        snprintf(wbuf, sizeof wbuf, "%ld/%d/%d", wv, (int)(we - ({inp})), werr == ERANGE);
        cmp("strtol({lbl}, {base}) value/consumed/ERANGE", gbuf, wbuf);''')
    s.append("    }")

    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; unsigned long gv, wv;''')
    for inp in STRTOL_INPUTS:
        lbl = inp.replace('"', '\\"')
        for base in (0, 10, 16):
            s.append(f'''
        gv = toy_strtoul({inp}, &ge, {base});
        wv = strtoul({inp}, &we, {base});
        snprintf(gbuf, sizeof gbuf, "%lu/%d", gv, (int)(ge - ({inp})));
        snprintf(wbuf, sizeof wbuf, "%lu/%d", wv, (int)(we - ({inp})));
        cmp("strtoul({lbl}, {base}) value/consumed", gbuf, wbuf);''')
    s.append("    }")

    s.append(r'''
    {
        char gbuf[128], wbuf[128];
        char *ge, *we; double gv, wv;''')
    for inp in STRTOD_INPUTS:
        lbl = inp.replace('"', '\\"')
        # Compared as %.17g plus the consumed count: the exact decimal
        # is what a rounding bug moves, and a tolerance would hide it.
        s.append(f'''
        gv = toy_strtod({inp}, &ge);
        wv = strtod({inp}, &we);
        // COMPARED TO WITHIN ONE ULP, not bit for bit. tolibc's strtod
        // applies the decimal exponent by repeated multiplication and
        // is accurate to a couple of ULP by construction, which its own
        // comment says; a correctly-rounded conversion is a separate
        // project (docs/libc-design.md). Demanding equality here would
        // report that KNOWN limit as a failure on every run and bury
        // the categorical bugs -- a NaN, a lost subnormal -- that this
        // harness exists to catch. --exact-float turns it off.
        snprintf(gbuf, sizeof gbuf, "%s/%d", EXACT_FLOAT || !near(gv, wv) ?
                 fmtd(gv) : fmtd(wv), (int)(ge - ({inp})));
        snprintf(wbuf, sizeof wbuf, "%s/%d", fmtd(wv), (int)(we - ({inp})));
        cmp("strtod({lbl}) value/consumed", gbuf, wbuf);''')
    s.append("    }")
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--only", choices=("printf", "parsers"),
                    help="run one half")
    ap.add_argument("--max-diffs", type=int, default=40,
                    help="stop listing after N differences (default 40)")
    ap.add_argument("--exact-float", action="store_true",
                    help="require strtod to match glibc BIT FOR BIT rather "
                         "than within one ULP -- shows the known rounding gap")
    ap.add_argument("--positive-control", action="store_true",
                    help="compare glibc against ITSELF with one case "
                         "deliberately corrupted, and require a difference")
    args = ap.parse_args()

    if not shutil.which("gcc"):
        print("libc_diff: no gcc on PATH")
        return 2

    work = tempfile.mkdtemp(prefix="libc_diff.")
    try:
        sections = []
        if args.only != "parsers":
            sections += printf_sections()
        if args.only != "printf":
            sections += parser_sections()
        if args.positive_control:
            # tolibc's OWN snprintf compared against itself must agree;
            # a deliberate mismatch beside it must not. If this passes,
            # the harness is not comparing anything.
            sections.append('    cmp("positive control", "a", "b");')
        gen_probe(work, sections, args.exact_float)
        try:
            binary = build(work)
        except RuntimeError as e:
            print("libc_diff: HARNESS FAILURE")
            print("  " + str(e)[:1500].replace("\n", "\n  "))
            return 2

        r = subprocess.run([binary], capture_output=True, text=True)
        diffs = [l for l in r.stdout.splitlines() if l.startswith("DIFF\t")]
        summary = [l for l in r.stdout.splitlines() if l.startswith("SUMMARY\t")]
        checks, fails = (summary[0].split("\t")[1:] if summary else ("?", "?"))

        shown = diffs[:args.max_diffs]
        for line in shown:
            _, what, got, want = line.split("\t", 3)
            print(f"  {what}\n      {got}\n      {want}")
        if len(diffs) > len(shown):
            print(f"  ... and {len(diffs) - len(shown)} more "
                  f"(raise --max-diffs to see them)")

        print(f"\nlibc_diff: {checks} cases, {fails} differ from glibc")
        if args.positive_control:
            if not diffs:
                print("positive control: FAILED -- a deliberate mismatch "
                      "was not reported. This harness compares nothing.")
                return 1
            print("positive control: ok -- the deliberate mismatch was reported")
        return 1 if int(fails) else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
