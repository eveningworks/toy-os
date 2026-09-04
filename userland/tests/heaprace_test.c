// The ring-3 allocator, from several threads at once.
//
// **WHAT THIS CAN AND CANNOT SHOW, MEASURED BEFORE IT WAS BELIEVED.**
// It is a concurrency smoke test for the allocator, NOT a proof that
// the lock is necessary. The control was run three times -- ring 3's
// `heap_os_lock()` emptied out -- against 600 allocations, then 8000
// against a list whose holes fit the requests, then 8000 against a list
// whose holes were all too small to fit any. **None of them failed.**
//
// The reason is arithmetic, and it is worth knowing before trusting any
// concurrency test in this system: the window a missing lock opens is
// the few instructions between kmalloc()'s fit test and its store of
// HEAP_IN_USE, preemption arrives on a 100 Hz timer, and there is one
// core -- so the odds of a tick landing in that window are somewhere
// around one in a million per call. The race is real by inspection and
// essentially unreachable by scheduling. It stops being unreachable
// under SMP, where two cores execute at once and the window is wall
// time rather than a tick boundary (docs/smp-design.md).
//
// So what this file is FOR: it fails if the lock deadlocks, if the
// allocator breaks under interleaved use, or if a future change widens
// that window -- and it is the fixture an SMP run would use unchanged.
//
// **THIS IS THE TEST THAT HAD NO REASON TO EXIST UNTIL THREADS DID.**
// `kernel/lib/heap_core.c` holds one address-ordered free list and
// splits and coalesces it in place; the kernel is never preempted
// mid-kmalloc, so ring 0 needs no lock and the file's top comment said
// so. Ring 3 is preempted at any instruction, so two threads in
// malloc() rewrite one list -- and every existing malloc test passes,
// because all of them are single-threaded.
//
// **THE PATTERN IS DERIVED FROM THE ADDRESS**, which is what makes this
// able to see the failure at all. A constant fill cannot detect two
// threads being handed overlapping blocks: both write the constant and
// both read it back happily. Deriving each byte from its own address
// plus the writer's tid means the loser reads the OTHER thread's value
// and can say whose it was.
//
// It must be SPAWNED, not `run` -- threads need a scheduler slot.
// kernel/proc/thread_test.c drives it.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt/sys.h"



#include "lib/utest.h"

#define WORKERS 4
#define ROUNDS  200

// **THE FREE LIST IS DELIBERATELY MADE LONG FIRST, and without that
// this test measures nothing.** The window a missing lock opens is the
// first-fit WALK inside kmalloc, and preemption here arrives on a 100 Hz
// timer -- so a walk of a handful of blocks takes microseconds against a
// 10 ms tick and is essentially never interrupted. Measured: with a
// short list, 600 allocations across four threads found zero overlaps
// with the lock removed entirely.
//
// Allocating thousands of small blocks and freeing every OTHER one
// leaves a list that is long and full of holes NOTHING CAN USE, so
// every worker allocation walks the length of it -- tens of
// microseconds, which a tick can land inside. This is the same lesson
// as the truncate tests that wrote 16 KB into a filesystem whose direct
// pointers already covered it: ask what input shape actually reaches
// the branch.
#define FRAGMENT_BLOCKS 2000

// **EVERY WORKER REQUEST IS LARGER THAN EVERY HOLE THE FIXTURE LEAVES**,
// and that is the whole point of these numbers. First fit stops at the
// first block big enough, so a fixture whose holes MATCH the request is
// answered from the front of the list in a few iterations -- measured,
// and it found nothing with the lock removed. Holes of FRAGMENT_SIZE
// that no request can use force the walk past all of them, every time.
static const unsigned SIZES[] = { 264, 296, 328, 360, 392, 424, 456, 488 };
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

#define FRAGMENT_SIZE 24   // smaller than the smallest request above

struct worker {
    int tid;
    int mismatches;   // somebody else's bytes turned up in our block
    int nulls;        // malloc returned nothing
    int rounds;
};

static uint8_t pattern_byte(const uint8_t *p, int tid) {
    uintptr_t a = (uintptr_t)p;
    // Address-derived AND writer-derived: the address half detects an
    // overlap, the tid half says who the other writer was.
    return (uint8_t)((a * 31u) ^ (uint8_t)(tid * 7 + 1));
}

static void *worker_main(void *arg) {
    struct worker *w = (struct worker *)arg;
    w->tid = sys_gettid();

    for (int r = 0; r < ROUNDS; r++) {
        unsigned size = SIZES[(r + w->tid) % NSIZES];
        uint8_t *p = (uint8_t *)malloc(size);
        if (!p) { w->nulls++; continue; }

        for (unsigned i = 0; i < size; i++) p[i] = pattern_byte(p + i, w->tid);
        // THE YIELD IS THE TEST, the same way it is in thread_test's
        // critical section: it guarantees another thread runs while
        // this block is live, so an overlap is found rather than raced
        // for.
        sys_yield();
        for (unsigned i = 0; i < size; i++)
            if (p[i] != pattern_byte(p + i, w->tid)) { w->mismatches++; break; }

        free(p);
        w->rounds++;
    }
    return NULL;
}

int main(void) {
    utest_begin("heaprace_test", "malloc/free from several threads at once", UTEST_VERDICT_FILE);

    // Everything this needs is allocated BEFORE the workers start, so
    // the harness itself is not part of what is being measured.
    static struct worker w[WORKERS];
    static pthread_t t[WORKERS];
    memset(w, 0, sizeof w);

    // The fragmentation fixture -- see FRAGMENT_BLOCKS.
    static void *frag[FRAGMENT_BLOCKS];
    int frag_ok = 1;
    for (int i = 0; i < FRAGMENT_BLOCKS; i++) {
        frag[i] = malloc(FRAGMENT_SIZE);
        if (!frag[i]) frag_ok = 0;
    }
    for (int i = 0; i < FRAGMENT_BLOCKS; i += 2) { free(frag[i]); frag[i] = NULL; }
    utest_check(frag_ok, "the fixture built a long, fragmented free list");

    uint64_t brk_before = (uint64_t)(uintptr_t)sys_sbrk(0);

    int started = 0;
    for (int i = 0; i < WORKERS; i++)
        if (pthread_create(&t[i], NULL, worker_main, &w[i]) == 0) started++;
    utest_check(started == WORKERS, "four threads started");

    unsigned long long t0 = sys_monotonic_ns();
    for (int i = 0; i < started; i++) pthread_join(t[i], NULL);
    unsigned long long ms = (sys_monotonic_ns() - t0) / 1000000ull;

    int total_rounds = 0, mismatches = 0, nulls = 0;
    for (int i = 0; i < started; i++) {
        total_rounds += w[i].rounds;
        mismatches   += w[i].mismatches;
        nulls        += w[i].nulls;
    }

    // The ELAPSED TIME is reported because it is what says whether the
    // walk was long enough to be interruptible at all: a run that
    // finishes in milliseconds never gave the timer a chance.
    utest_notef("%d allocations in %u ms, %d mismatches, %d nulls",
                total_rounds, (unsigned)ms, mismatches, nulls);

    utest_check(total_rounds == WORKERS * ROUNDS, "every round completed");
    utest_check(nulls == 0, "no allocation was refused");
    // THE LOAD-BEARING CHECK: a corrupted free list hands two threads
    // memory that overlaps, and the address-derived pattern is what
    // notices.
    utest_check(mismatches == 0, "no thread's block held another thread's bytes");

    // The list still works AFTER the hammering -- a corruption that did
    // not overlap anything live can still have left the list unwalkable,
    // and this is what finds that.
    int alloc_ok = 1;
    void *big[16];
    for (int i = 0; i < 16; i++) {
        big[i] = malloc(4096);
        if (!big[i]) alloc_ok = 0;
    }
    for (int i = 0; i < 16; i++) free(big[i]);
    utest_check(alloc_ok, "the heap still serves large allocations afterwards");

    // Freed memory is REUSED rather than the break marching upward
    // forever: with the list intact, 600 allocations of at most 1 KiB
    // do not need much beyond what the first few rounds claimed.
    uint64_t brk_after = (uint64_t)(uintptr_t)sys_sbrk(0);
    uint64_t grew = brk_after - brk_before;
    utest_notef("the break moved %u KiB", (unsigned)(grew / 1024));
    utest_check(grew < 1024 * 1024, "the break did not run away -- blocks were reused");

    for (int i = 1; i < FRAGMENT_BLOCKS; i += 2) free(frag[i]);

    return utest_end();
}
