// SYS_GETRANDOM from ring 3 -- a self-checking diagnostic, run by
// tools/usertest_run.py as well as by hand (`run random_test`).
//
// Like the kernel-side KTESTs, this does not try to measure randomness:
// a statistical test here would be a number that looks like evidence
// and is not (see kernel/lib/krandom_test.c's top comment). What it
// checks is the SYSCALL's contract, and specifically the parts a
// plausible-looking implementation gets wrong:
//
//   * the buffer is filled and NOTHING past the requested length is
//     touched -- the kernel writes into a user pointer here, so an
//     overrun is a ring-0 write past a ring-3 buffer;
//   * two calls differ, which is what a stuck source looks like;
//   * a bad pointer is REFUSED rather than faulting the kernel, which
//     is the whole reason the dispatcher validates the range;
//   * an oversized count is refused, and a zero count is a legal no-op
//     rather than an error.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
    sys_print(ok ? "  ok   " : "  FAIL ");
    sys_print(what);
    sys_print("\n");
    if (!ok) failures++;
}

int main(void) {
    sys_print("random_test: SYS_GETRANDOM\n");

    // --- fills exactly, and not one byte more ---------------------
    unsigned char buf[64];
    memset(buf, 0xAA, sizeof buf);
    int n = sys_getrandom(buf + 8, 32);
    check(n == 32, "returns the requested count");

    int guard_ok = 1;
    for (int i = 0; i < 8; i++) if (buf[i] != 0xAA) guard_ok = 0;
    for (int i = 40; i < 64; i++) if (buf[i] != 0xAA) guard_ok = 0;
    check(guard_ok, "wrote nothing outside the requested range");

    int any_set = 0;
    for (int i = 8; i < 40; i++) if (buf[i] != 0xAA) any_set = 1;
    check(any_set, "actually wrote into the buffer");

    // --- two calls differ ------------------------------------------
    unsigned char a[32], b[32];
    sys_getrandom(a, sizeof a);
    sys_getrandom(b, sizeof b);
    check(memcmp(a, b, sizeof a) != 0, "two calls return different bytes");

    // A buffer that came back all-zero is the RDRAND-failure value and
    // also what a kernel that validated the pointer but forgot to fill
    // would leave behind -- worth its own check, since "two calls
    // differ" would not catch a source stuck at zero on both.
    int all_zero = 1;
    for (unsigned i = 0; i < sizeof a; i++) if (a[i]) all_zero = 0;
    check(!all_zero, "output is not all zero");

    // --- refusals ---------------------------------------------------
    check(sys_getrandom((void *)0x10, 16) == -1, "refuses an unmapped pointer");
    check(sys_getrandom(buf, 100000) == -1, "refuses a count over the maximum");
    check(sys_getrandom(buf, 0) == 0, "a zero-length request is a no-op, not an error");

    if (failures) {
        sys_print("random_test: FAILED\n");
        return 1;
    }
    sys_print("random_test: all checks passed\n");
    return 0;
}
