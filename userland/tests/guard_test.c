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
    //
    // **The size of the request used to be a hardcoded 2 MiB, and that
    // is exactly how this check stopped measuring anything.** The
    // comment justifying the number said "the gap is about 1 MiB, so
    // this is comfortably past it" -- true until M41 stage 4b widened
    // the heap to ~14 MiB for a ring-3 compositor's back buffer, at
    // which point the request simply succeeded and the branch under
    // test was never reached. A number chosen against an address map is
    // stale the moment that map moves, and it goes stale SILENTLY.
    //
    // So walk to the boundary instead of guessing where it is: grow in
    // modest steps until one is refused. That measures the property --
    // "sbrk stops before the stack" -- at any heap size, and needs no
    // edit the next time the map changes.
    //
    // The step matters for the same reason the old constant did. It is
    // small enough that a guest can really allocate each one, so a
    // refusal means "the bound refused it" rather than "physical memory
    // ran out" -- asking for 1 GiB in one go made this whole block pass
    // against a kernel with the bound REMOVED, because the allocator
    // gave up long before the stack did. And the cap bounds a kernel
    // with no bound at all: without it, such a kernel loops here until
    // it has mapped the entire address space.
    #define STEP  (1024 * 1024)
    #define MAX_STEPS 64        // 64 MiB, far past any plausible heap
    void *before = sys_sbrk(0);
    void *big = (void *)-1;
    int steps = 0;
    while (steps < MAX_STEPS) {
        before = sys_sbrk(0);
        big = sys_sbrk(STEP);
        if (big == (void *)-1) break;
        // Write through EVERY chunk handed back, not just the last.
        // A refused request is visible immediately; an ALLOWED one that
        // overlaps the stack is not -- the aliasing costs nothing until
        // somebody writes, at which point the canary below reports it.
        memset(big, 0, STEP);
        steps++;
    }
    check(big == (void *)-1,
          "sbrk stops before the stack rather than growing forever");
    check(steps > 0, "and it allowed real growth before stopping");
    check(sys_sbrk(0) == before, "a refused request left the break alone");
    #undef STEP
    #undef MAX_STEPS

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
