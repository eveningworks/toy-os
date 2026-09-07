#!/usr/bin/env python3
"""Check tolibc's 128-bit division helpers against Python bignums, on the HOST.

WHY THESE EXIST AT ALL. x86-64 multiplies 64x64 into 128 in one
instruction, so `unsigned __int128` multiply compiles inline; there is
no 128-bit DIVIDE instruction, so `/` and `%` on a 128-bit type become
calls to `__udivti3` and friends, which normally live in libgcc. toy-os
links -nostdlib with no libgcc, so `userland/libc/divti3.c` supplies
them and a missing one is a link error rather than a warning.

WHY A HOST HARNESS. These are pure arithmetic over a value space no
guest test can sample usefully by hand -- the bugs live at the 64-bit
boundary, at powers of two, and where the shift-subtract loop meets its
fast path. That is thousands of vectors, not nine, and the oracle wants
arbitrary precision.

The oracle shares no code with what it judges: Python's integers are
arbitrary-precision, so `a // b` is the answer by construction rather
than by another implementation agreeing. Needs nothing but gcc.

  python3 tools/divti3_hostcheck.py
  python3 tools/divti3_hostcheck.py --random 20000
  python3 tools/divti3_hostcheck.py --positive-control

ONE CASE IS EXCLUDED, DELIBERATELY: INT128_MIN / -1. The true quotient
is +2^127, which no signed 128-bit type can hold, so C leaves it
undefined -- the same shape as INT_MIN / -1. libgcc returns
INT128_MIN there and so does divti3.c; asserting Python's
mathematically-correct answer would fail a correct implementation.

WHAT IT DOES NOT COVER: divide-by-zero, which raises #DE by design and
so cannot be probed from a process that expects to keep running.

Exit status is non-zero if any vector disagrees.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The helpers are renamed at compile time so the driver calls OURS and
# not libgcc's, which defines the same symbols and would otherwise win
# or clash depending on link order.
RENAME = ["__udivmodti4=toy_udivmodti4", "__udivti3=toy_udivti3",
          "__umodti3=toy_umodti3", "__divti3=toy_divti3", "__modti3=toy_modti3"]

DRIVER = r"""
#include <stdio.h>
#include <string.h>
typedef unsigned __int128 u128;
typedef signed __int128 i128;
u128 toy_udivti3(u128, u128);
u128 toy_umodti3(u128, u128);
i128 toy_divti3(i128, i128);
i128 toy_modti3(i128, i128);

static u128 rd(const char *s) { u128 v = 0; for (; *s; s++) v = v * 10 + (u128)(*s - '0'); return v; }
static void pr(u128 v) {
    char b[41]; int i = 40; b[40] = 0;
    if (!v) { puts("0"); return; }
    while (v) { b[--i] = '0' + (int)(v % 10); v /= 10; }
    puts(b + i);
}

/* One process per vector would dominate the runtime, so the driver
   reads "op a b" lines from stdin and answers one line each. */
int main(void) {
    char line[128], op[8], as[48], bs[48];
    while (fgets(line, sizeof line, stdin)) {
        if (sscanf(line, "%7s %47s %47s", op, as, bs) != 3) continue;
        u128 a = rd(as), b = rd(bs);
        if (!strcmp(op, "u")) pr(toy_udivti3(a, b));
        else if (!strcmp(op, "um")) pr(toy_umodti3(a, b));
        else if (!strcmp(op, "s")) {
            i128 r = toy_divti3((i128)a, (i128)b);
            u128 m = (r < 0) ? (u128)0 - (u128)r : (u128)r;
            if (r < 0) putchar('-');
            pr(m);
        } else if (!strcmp(op, "sm")) {
            i128 r = toy_modti3((i128)a, (i128)b);
            u128 m = (r < 0) ? (u128)0 - (u128)r : (u128)r;
            if (r < 0) putchar('-');
            pr(m);
        }
        fflush(stdout);
    }
    return 0;
}
"""

M128 = (1 << 128) - 1
SIGN = 1 << 127
INT128_MIN = -(1 << 127)


def build(tmp, poison=False):
    """Compile divti3.c + the driver with the host gcc.

    divti3.c is COPIED into the temp directory, which is what makes
    --positive-control possible without leaving a test hook in shipped
    code: the copy is edited, the original never is.
    """
    src = os.path.join(tmp, "divti3.c")
    with open(os.path.join(ROOT, "userland/libc/divti3.c")) as f:
        text = f.read()
    if poison:
        # Break the fast path only. A wrong answer then appears just for
        # operands that both fit in 64 bits, which is the branch a naive
        # sweep of large random values would never reach -- so this also
        # checks the sweep covers small values at all.
        old = "uint64_t q = (uint64_t)n / (uint64_t)d;"
        assert old in text, "positive control could not find the fast path"
        text = text.replace(old, "uint64_t q = (uint64_t)n / (uint64_t)d + 1;")
    with open(src, "w") as f:
        f.write(text)

    drv = os.path.join(tmp, "driver.c")
    with open(drv, "w") as f:
        f.write(DRIVER)

    exe = os.path.join(tmp, "divti3check")
    cmd = ["gcc", "-O2", "-w", "-o", exe]
    for r in RENAME:
        cmd += ["-D" + r]
    cmd += [drv, src]
    subprocess.run(cmd, check=True)
    return exe


def vectors(n_random, seed):
    """Edge pairs first, then random widths.

    The widths are drawn independently so a/b spans every combination of
    "both small", "both large" and "one of each" -- which is what
    decides whether the fast path or the loop runs.
    """
    edges = [1, 2, 3, 7,
             (1 << 32), (1 << 32) - 1,
             (1 << 63) - 1, 1 << 63,
             (1 << 64) - 1, 1 << 64, (1 << 64) + 1,
             1 << 96,
             (1 << 127) - 1, 1 << 127,
             M128 - 1, M128]
    for a in edges:
        for b in edges:
            yield a, b
    rng = random.Random(seed)
    for _ in range(n_random):
        a = rng.getrandbits(rng.randint(1, 128))
        b = rng.getrandbits(rng.randint(1, 128))
        yield a, b or 1


def signed(x):
    return x - (1 << 128) if x >= SIGN else x


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--random", type=int, default=4000,
                    help="random vector pairs after the edge sweep (default 4000)")
    ap.add_argument("--seed", type=int, default=20260907)
    ap.add_argument("--positive-control", action="store_true",
                    help="break the fast path in a COPY and require the sweep to fail")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)

        asks, wants = [], []
        for a, b in vectors(args.random, args.seed):
            if b == 0:
                continue
            asks.append("u %d %d" % (a, b)); wants.append(("udiv", a, b, str(a // b)))
            asks.append("um %d %d" % (a, b)); wants.append(("umod", a, b, str(a % b)))
            sa, sb = signed(a), signed(b)
            if sb == 0 or (sa == INT128_MIN and sb == -1):
                continue  # see the module docstring: the one UB case
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q                      # C truncates toward zero
            r = abs(sa) % abs(sb)
            if sa < 0:
                r = -r                      # remainder takes the DIVIDEND's sign
            asks.append("s %d %d" % (a, b)); wants.append(("sdiv", sa, sb, str(q)))
            asks.append("sm %d %d" % (a, b)); wants.append(("smod", sa, sb, str(r)))

        proc = subprocess.run([exe], input="\n".join(asks) + "\n",
                              capture_output=True, text=True)
        got = proc.stdout.split("\n")

        bad = []
        for i, (op, a, b, want) in enumerate(wants):
            if i >= len(got) or got[i] != want:
                bad.append((op, a, b, got[i] if i < len(got) else "<no answer>", want))

        print("divti3_hostcheck: %d vectors, %d disagreed" % (len(wants), len(bad)))
        for op, a, b, g, w in bad[:10]:
            print("  %-5s %d / %d -> got %s want %s" % (op, a, b, g, w))
        if len(bad) > 10:
            print("  ... and %d more" % (len(bad) - 10))

        if args.positive_control:
            if bad:
                print("divti3_hostcheck: positive control OK -- the sweep can fail")
                return 0
            print("divti3_hostcheck: POSITIVE CONTROL DID NOT FAIL -- "
                  "this suite is measuring nothing")
            return 1
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
