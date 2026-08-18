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
#include "proc_info.h"

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
    // **This block has now gone stale TWICE against the address map,
    // and the second time is why it no longer walks.** It began as a
    // hardcoded 2 MiB request ("comfortably past the ~1 MiB gap"),
    // which simply succeeded once the heap grew to ~14 MiB. It was then
    // rewritten to walk 1 MiB at a time up to 64 MiB, which stopped
    // reaching the bound the moment the heap became ~2 GiB -- and, with
    // sbrk lazy, walking now means WRITING through gigabytes to find
    // out where the ceiling is, i.e. trying to allocate the machine.
    //
    // So it does two separate things instead of one walk, and neither
    // depends on knowing where the limit is:
    //
    //   * grow a MODEST amount and write through all of it, which is
    //     what proves growth is real and mapped (and is what the canary
    //     check at the end needs -- aliasing costs nothing until
    //     somebody writes);
    //   * ask for an increment NO heap could hold, and require the
    //     refusal. A kernel with the bound removed accepts it, so this
    //     is the check the control reddens.
    #define STEP  (1024 * 1024)
    #define STEPS 8              // 8 MiB: real growth, trivial cost
    void *before = sys_sbrk(0);
    int steps = 0;
    for (int i = 0; i < STEPS; i++) {
        void *chunk = sys_sbrk(STEP);
        if (chunk == (void *)-1) break;
        // Every chunk, not just the last: an ALLOWED request that
        // overlaps the stack is invisible until something writes.
        memset(chunk, 0, STEP);
        steps++;
    }
    check(steps == STEPS, "sbrk grows, and every page it hands back is writable");

    before = sys_sbrk(0);
    // A terabyte. Bigger than the address space the map reserves, so no
    // bound-checking kernel can accept it and an unbounded one will.
    check(sys_sbrk((int64_t)1 << 40) == (void *)-1,
          "sbrk stops before the stack rather than growing forever");
    check(sys_sbrk(0) == before, "a refused request left the break alone");
    #undef STEP
    #undef STEPS

    // The kernel reads the increment as UNSIGNED (see SYS_SBRK in
    // syscall.c -- there is no shrink), so a negative value is a
    // near-2^64 request, and brk + inc wraps. A kernel checking
    // `brk + inc > limit` instead of `inc > limit - brk` computes a
    // small sum, passes the check, and then loops mapping pages from
    // the break to an address below it.
    check(sys_sbrk(-4096) == (void *)-1,
          "a request that overflows the sum is refused");
    check(sys_sbrk(0) == before, "that one left the break alone too");

    // --- a page the KERNEL touches first ----------------------------
    //
    // The half of demand paging that never faults, and would therefore
    // never be exercised by any test that simply uses its memory.
    //
    // The kernel does not dereference user pointers: it walks the page
    // tables and copies through its own identity map (that is what
    // makes SMAP absolute here). So handing a syscall a page that sbrk
    // has reserved and ring 3 has NOT touched produces no #PF at all --
    // it produces a walk that finds nothing. Without a fault-in on that
    // path the syscall reports a perfectly legal buffer as a bad
    // pointer, and every syscall taking a caller-allocated buffer
    // breaks the moment sbrk goes lazy.
    //
    // Deliberately untouched between the sbrk and the syscall, which is
    // the entire point -- a memset here would map the page from ring 3
    // and the check would pass either way.
    struct proc_info *info = (struct proc_info *)sys_sbrk(4096);
    check(info != (struct proc_info *)-1, "sbrk handed back a fresh page");
    if (info != (struct proc_info *)-1) {
        int got = sys_proc_info(0, info);
        check(got != 0, "a syscall can write into an untouched sbrk page");
        // And it wrote something real, not zeros a blank page would
        // also show: slot 0 is init on any boot that has one.
        check(got != 0 && info->pid != 0,
              "and what it wrote is the real process table");
    }

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
