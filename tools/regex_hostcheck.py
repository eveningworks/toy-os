#!/usr/bin/env python3
"""tools/regex_hostcheck.py -- tolibc's regex against glibc's, on the HOST.

WHY THIS EXISTS SEPARATELY FROM /tests/regex_test
-------------------------------------------------
The ring-3 test asserts that `userland/libc/regex.c` agrees with the
expected spans written in `userland/tests/regex_cases.h`. Both halves
of that were written by the same person at the same time, so it cannot
catch the one failure mode that matters most for a spec implementation:
**an expectation that is simply wrong.**

So this compiles the SAME case table twice -- once against the real
`userland/libc/regex.c`, once against the host's glibc `<regex.h>` --
and compares. glibc shares no code, no author and no assumptions with
this engine; where the two agree, the expected value is almost
certainly right. Same idea as `tools/uimg_hostcheck.py` checking the
JPEG decoder against libjpeg.

It also pins the DIVERGENCES. Three are deliberate (below); anything
else appearing here is a bug in one of them, and historically the one
with the bug has not been glibc.

ON DEMAND, and not in the gate: it needs a host gcc and glibc, and
`preflight.sh` must not start requiring either -- the same rule that
keeps Docker and ruff out of it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Where tolibc deliberately differs from glibc. Each entry is a pattern
# from the case table plus WHY -- an unexplained entry here would just
# be a bug with a note on it.
# Keys are the pattern AS IT APPEARS IN THE HEADER SOURCE, i.e. with C
# escaping still on it (`a\\tb`, not `a\tb`) -- that is what the report
# below extracts, and normalising instead would mean re-implementing C
# string escaping to compare two strings.
KNOWN_DIVERGENCES = {
    r"a\\tb":
        "tolibc reads \\t \\n \\r \\f \\v as the control characters they "
        "name; POSIX leaves them undefined and glibc treats \\t as a "
        "literal 't'. GNU grep does what tolibc does, and it is what "
        "anybody typing \\t means.",
    "abc)":
        "tolibc REFUSES an unmatched ')'; glibc accepts it as a literal. "
        "POSIX calls it undefined, and a parser here rejects rather than "
        "guesses.",
    r"(a)\\1":
        "back-references. glibc supports them in ERE as a GNU extension; "
        "an NFA simulation fundamentally cannot, which is the price of "
        "not being able to blow up on (a*)*b. Refused by name.",
}

HARNESS = r'''
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include "regex_cases.h"

int main(void) {
    for (int i = 0; i < RX_CASE_COUNT; i++) {
        const struct rx_case *c = &RX_CASES[i];
        regex_t re;
        int cf = REG_EXTENDED | (c->icase ? REG_ICASE : 0);
        if (regcomp(&re, c->pattern, cf) != 0) { printf("%d ERR\n", i); continue; }
        regmatch_t m[10];
        if (regexec(&re, c->input, 10, m, 0) == 0)
            printf("%d %d %d\n", i, (int)m[0].rm_so, (int)m[0].rm_eo);
        else
            printf("%d NOMATCH\n", i);
        regfree(&re);
    }
    for (int i = 0; i < RX_BAD_COUNT; i++) {
        regex_t re;
        int ok = regcomp(&re, RX_BAD[i].pattern, REG_EXTENDED) == 0;
        if (ok) regfree(&re);
        printf("BAD%d %s\n", i, ok ? "ACCEPTED" : "rejected");
    }
    return 0;
}
'''


def build_and_run(workdir, use_ours):
    """Compile the harness against ours or glibc's regex; return its lines."""
    src = os.path.join(workdir, "h.c")
    with open(src, "w") as f:
        f.write(HARNESS)
    inc = [f"-I{os.path.join(REPO, 'userland', 'tests')}"]
    srcs = [src]
    if use_ours:
        # Our regex.h shadows the system one, and our regex.c provides
        # the symbols -- so the object file's strong definitions win
        # over libc's at link time.
        ourinc = os.path.join(workdir, "inc")
        os.makedirs(ourinc, exist_ok=True)
        shutil.copy(os.path.join(REPO, "userland", "include", "regex.h"), ourinc)
        inc.insert(0, f"-I{ourinc}")
        srcs.append(os.path.join(REPO, "userland", "libc", "regex.c"))
    out = os.path.join(workdir, "ours" if use_ours else "glibc")
    r = subprocess.run(["gcc", "-std=gnu11", "-O1", "-w", *inc, "-o", out, *srcs],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"regex_hostcheck: build failed\n{r.stderr}")
    r = subprocess.run([out], capture_output=True, text=True)
    return r.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if not shutil.which("gcc"):
        print("regex_hostcheck: SKIP -- no host gcc")
        return 0

    # The case table is the input; read it for the pattern names so a
    # divergence can be reported by pattern rather than by index.
    cases_h = open(os.path.join(REPO, "userland", "tests", "regex_cases.h")).read()

    with tempfile.TemporaryDirectory(prefix="rxhost.") as wd:
        ours = build_and_run(wd, True)
        glibc = build_and_run(wd, False)

    if len(ours) != len(glibc):
        sys.exit("regex_hostcheck: the two runs produced different line counts "
                 "-- the harness or the table is broken, not the engine")

    diffs, unexplained = [], []
    for a, b in zip(ours, glibc):
        if a == b:
            continue
        key = a.split()[0]
        # Recover the pattern this line is about, for the report.
        pat = None
        if key.startswith("BAD"):
            idx = int(key[3:])
            body = cases_h.split("RX_BAD[] = {")[1]
            rows = [ln for ln in body.splitlines() if ln.strip().startswith("{")]
            if idx < len(rows):
                pat = rows[idx].split('"')[1]
        else:
            idx = int(key)
            body = cases_h.split("RX_CASES[] = {")[1]
            rows = [ln for ln in body.splitlines() if ln.strip().startswith("{")]
            if idx < len(rows):
                pat = rows[idx].split('"')[1]
        diffs.append((pat, a, b))
        if pat not in KNOWN_DIVERGENCES:
            unexplained.append((pat, a, b))

    print(f"regex_hostcheck: {len(ours)} cases, {len(diffs)} divergence(s) from glibc")
    for pat, a, b in diffs:
        known = pat in KNOWN_DIVERGENCES
        print(f"  {'known ' if known else 'UNKNOWN'}  /{pat}/   ours: {a}   glibc: {b}")
        if known and args.verbose:
            print(f"           {KNOWN_DIVERGENCES[pat]}")

    if unexplained:
        print(f"\nregex_hostcheck: FAIL -- {len(unexplained)} unexplained divergence(s).")
        print("Either tolibc is wrong, or the difference is deliberate and belongs")
        print("in KNOWN_DIVERGENCES with the reason. Historically it is the former.")
        return 1
    print("regex_hostcheck: PASS -- every difference from glibc is a documented one")
    return 0


if __name__ == "__main__":
    sys.exit(main())
