// malloc/free in ring 3 -- the kernel's own allocator, compiled a
// second time over SYS_SBRK (userland/lib/stdlib.h, api/heap_os.h).
//
// WHAT IS ACTUALLY UNDER TEST. The allocator logic has KTESTs already
// (kernel/mm/mm_test.c) and would pass them either way; what those
// cannot reach is whether this code WORKS AT ALL on the other side of
// the syscall boundary -- whether sbrk-backed regions behave like
// pmm-backed ones, whether the pages a lazy sbrk hands back are really
// there when the free list writes a header into them, and whether the
// archive links. That is the same gap libc_test was written into.
//
// The checks that matter most are the ones a broken allocator still
// passes: "malloc returned non-NULL" is satisfied by an allocator that
// hands the same block to two callers, so every check below is about
// what happens between two allocations, not about one.
#include "rt/sys.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lib/utest.h"

// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing a hundred call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

#define N 32

int main(void) {
    utest_begin("malloc_test", "malloc, free and coalescing", UTEST_KLOG);

    char detail[64];

    // The break BEFORE anything is allocated, so the footprint reported
    // at the end is what this test claimed rather than an absolute
    // address (which is what it printed first, and which reads as half
    // a terabyte).
    uint64_t brk0 = (uint64_t)(uintptr_t)sys_sbrk(0);

    // --- distinct, and writable ------------------------------------
    //
    // Two allocations must not overlap, and the way to prove it is to
    // write DIFFERENT data into each and read them all back afterwards
    // -- the same reasoning memtest applies to pages. Checking pointers
    // are unequal is weaker: two blocks can be distinct pointers and
    // still overlap.
    void *p[N];
    int distinct = 1, sized = 1;
    for (int i = 0; i < N; i++) {
        p[i] = malloc(64 + (size_t)i * 8);
        if (!p[i]) { distinct = 0; break; }
        memset(p[i], i + 1, 64 + (size_t)i * 8);
    }
    check("32 allocations all succeed", distinct, 0);

    for (int i = 0; i < N && distinct; i++) {
        unsigned char *q = (unsigned char *)p[i];
        for (size_t j = 0; j < 64 + (size_t)i * 8; j++) {
            if (q[j] != (unsigned char)(i + 1)) { sized = 0; break; }
        }
    }
    check("and none of them overlaps another", sized,
          "each block still holds only its own byte");

    // --- free, and reuse -------------------------------------------
    //
    // Freeing everything and allocating the same shape again must NOT
    // grow the process: if it does, free() is a no-op and the free list
    // is not being consulted. Measured through sbrk(0), which is the
    // process's own footprint and an independent path from the
    // allocator's bookkeeping.
    void *before_brk = sys_sbrk(0);
    for (int i = 0; i < N; i++) free(p[i]);
    for (int i = 0; i < N; i++) p[i] = malloc(64 + (size_t)i * 8);
    void *after_brk = sys_sbrk(0);
    int reused = 1;
    for (int i = 0; i < N; i++) if (!p[i]) reused = 0;
    check("freed memory is handed out again", reused && after_brk == before_brk,
          after_brk == before_brk ? "the break did not move" : "the break MOVED -- free() did nothing");
    for (int i = 0; i < N; i++) free(p[i]);

    // --- coalescing -------------------------------------------------
    //
    // Free three adjacent blocks and ask for one that only their MERGED
    // extent can hold. The assertion is that the answer is `a` itself:
    // the list is address-ordered and first-fit, so a merged block
    // starts where the first of the three did, and an allocator that
    // did not merge cannot return that address -- `a` alone is far too
    // small for the request.
    //
    // **The obvious version of this check does not work, and it is
    // worth saying why.** It first asked whether the process's
    // FOOTPRINT grew, and stayed green with try_merge_next() disabled
    // outright: the allocator claims memory in 64 KiB regions, so three
    // 4 KiB blocks sit inside one, and the region's own leftover
    // satisfies a 12 KiB request whether anything merged or not. The
    // fixture never reached the branch under test. Comparing the
    // ADDRESS needs no assumption about region size at all.
    void *a = malloc(4096), *b = malloc(4096), *c = malloc(4096);
    check("three page-sized blocks", a && b && c, 0);
    int adjacent = a && b && c && (char *)a < (char *)b && (char *)b < (char *)c;
    check("and they are laid out in address order", adjacent, 0);
    free(a); free(b); free(c);
    void *big = malloc(4096 * 3);
    check("adjacent free blocks coalesce", big && big == a,
          big == a ? "the merged block starts where the first one did"
                   : "the request did not come back at the first block's address");
    free(big);

    // --- calloc -----------------------------------------------------
    unsigned char *z = (unsigned char *)calloc(200, 3);
    int zeroed = z != 0;
    for (int i = 0; zeroed && i < 600; i++) if (z[i]) zeroed = 0;
    check("calloc zeroes what it returns", zeroed, 0);
    free(z);

    // The overflow guard: n * size wraps, and a wrapped product would
    // allocate a small block for a huge request -- a caller then writes
    // far past it. Asked for directly because nothing else would ever
    // exercise it.
    check("calloc refuses a count that would overflow",
          calloc((size_t)-1 / 2, 4) == 0, 0);

    // --- a big one, to cross several sbrk regions -------------------
    void *huge = malloc(2 * 1024 * 1024);
    check("a 2 MiB allocation succeeds", huge != 0, 0);
    if (huge) {
        memset(huge, 0xAB, 2 * 1024 * 1024);
        unsigned char *h = (unsigned char *)huge;
        int intact = h[0] == 0xAB && h[1024 * 1024] == 0xAB && h[2 * 1024 * 1024 - 1] == 0xAB;
        check("and every page of it is really mapped", intact,
              "first, middle and last byte");
        free(huge);
    }

    snprintf(detail, sizeof detail, "%u KiB claimed from sbrk",
             (unsigned)(((uint64_t)(uintptr_t)sys_sbrk(0) - brk0) >> 10));
    sys_eprint("malloc_test: ");
    sys_eprint(detail);
    sys_eprint("\n");

    return utest_end();
}
