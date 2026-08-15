// SYS_SBRK's bound against the user stack's guard region -- a
// self-checking diagnostic, run by tools/usertest_run.py as well as by
// hand (`run guard_test`).
//
// The bug this exists to keep closed: sbrk() had no ceiling at all, so
// a large enough request mapped fresh pages straight over the live
// stack a page at a time. Nothing faulted and nothing was logged --
// the process simply found its own locals changing underneath it, which
// is about the worst failure shape available. The heap now stops at the
// guard region below the stack (kernel/include/kernel/uaddr.h).
//
// Two things about the checks below are load-bearing, and both were
// established by running this against a kernel with the bound removed
// rather than by reasoning:
//
//   * **the SIZE of the oversized request**. It asks for 2 MiB, just
//     past the ~1 MiB gap. An earlier version asked for 1 GiB and
//     PASSED against the unbounded kernel -- physical memory ran out
//     long before the stack did, so sbrk refused it for the wrong
//     reason and every check stayed green.
//   * **writing through the pointer** an unbounded kernel hands back.
//     Mapping over the stack costs nothing until something writes, so
//     the canary check at the end is only meaningful because of that
//     memset. Without it the control reddens the refusal checks and
//     leaves the corruption invisible.
#include "rt/sys.h"
#include "lib/string.h"

static int failures;

static void check(int ok, const char *what) {
    sys_print(ok ? "  ok   " : "  FAIL ");
    sys_print(what);
    sys_print("\n");
    if (!ok) failures++;
}

int main(void) {
    sys_print("guard_test: SYS_SBRK stops at the stack guard\n");

    // A canary in THIS frame, checked at the end. It sits on the user
    // stack, which is precisely what an unbounded heap walked over.
    volatile unsigned char canary[64];
    for (int i = 0; i < 64; i++) canary[i] = (unsigned char)(i * 7 + 3);

    // --- an ordinary request still works ---------------------------
    void *base = sys_sbrk(0);
    check(base != (void *)-1, "sbrk(0) reports the current break");

    void *first = sys_sbrk(4096);
    check(first == base, "sbrk() returns the OLD break");
    check(sys_sbrk(0) == (void *)((unsigned char *)base + 4096),
          "the break advanced by exactly what was asked for");

    // The page it just handed back must be real memory, not a number.
    unsigned char *p = (unsigned char *)first;
    memset(p, 0x5A, 4096);
    int readable = 1;
    for (int i = 0; i < 4096; i++) if (p[i] != 0x5A) readable = 0;
    check(readable, "the newly-broken page is actually mapped");

    // --- the gap is finite -----------------------------------------
    // **2 MiB, not something enormous, and the size is the test.** The
    // gap between the heap base and the guard is about 1 MiB, so this
    // is comfortably past it -- while still being an amount a guest
    // with no bound would have no trouble actually allocating. Asking
    // for 1 GiB here instead made this whole block PASS against a
    // kernel with the bound removed, because physical memory ran out
    // long before the stack did: the request was refused, the break
    // was left alone, and nothing was ever overwritten. Green, and
    // measuring "sbrk can fail" rather than "sbrk stops at the guard".
    void *before = sys_sbrk(0);
    void *big = sys_sbrk(2 * 1024 * 1024);
    check(big == (void *)-1, "a request past the guard is refused");
    check(sys_sbrk(0) == before, "a refused request left the break alone");

    if (big != (void *)-1) {
        // Unreachable on a kernel with the bound, and the reason the
        // canary check below means anything without it. A refused
        // request is visible immediately; an ALLOWED one is not --
        // mapping over the stack costs nothing until somebody writes
        // through the heap pointer, at which point the two aliases are
        // the same memory. So write, and let the canary say so.
        memset(big, 0, 2 * 1024 * 1024);
    }

    // The kernel reads the increment as UNSIGNED (see SYS_SBRK in
    // syscall.c -- there is no shrink), so a negative value is a
    // near-2^64 request, and brk + inc wraps. A kernel checking
    // `brk + inc > limit` instead of `inc > limit - brk` computes a
    // small sum, passes the check, and then loops mapping pages from
    // the break to an address below it.
    check(sys_sbrk(-4096) == (void *)-1,
          "a request that overflows the sum is refused");
    check(sys_sbrk(0) == before, "that one left the break alone too");

    // --- and the stack survived ------------------------------------
    int intact = 1;
    for (int i = 0; i < 64; i++) if (canary[i] != (unsigned char)(i * 7 + 3)) intact = 0;
    check(intact, "this frame's stack canary is untouched");

    if (failures) {
        sys_print("guard_test: FAILED\n");
        return 1;
    }
    sys_print("guard_test: all checks passed\n");
    return 0;
}
