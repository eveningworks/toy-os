// The user stack GROWS ON DEMAND -- a self-checking diagnostic, run by
// tools/usertest_run.py as well as by hand (`spawn /tests/stackgrow_test`).
//
// **WHY THIS CANNOT BE A KTEST.** kernel/proc/uaddr_test.c asserts the
// map's arithmetic -- that the floor is below the initial bottom, that
// the guard is a whole number of pages -- and every one of those checks
// passes just as happily on a kernel whose fault handler grows nothing
// at all. The macros are right either way. Only a ring-3 process with a
// deep call chain can show that the pages actually arrive, which is why
// this file exists beside them rather than instead of them.
//
// The stack was FOUR PAGES, mapped eagerly by the loader, and a process
// that walked off the bottom died on the guard page below. It is four
// pages of working set inside an 8 MiB reservation now, with the pages
// in between mapped on first touch by proc_syscalls.c's uheap_fault()
// -- Linux's expand_downwards(), in the small.
//
// Two things about the checks below are load-bearing:
//
//   * **the pattern is derived from the frame's own ADDRESS.** A
//     constant fill cannot tell a working stack from one where two
//     depths were handed the SAME physical frame: both read back the
//     constant and both look perfect. Aliasing is the failure mode a
//     growth bug actually has, so the pattern has to be able to name
//     whose frame it is. Same reasoning as /tests/memtest's heap fill.
//   * **the frames are verified on the way OUT, not on the way in.**
//     Checking a frame immediately after filling it proves only that a
//     page was mapped. Checking it after the recursion below has
//     touched another megabyte of stack is what proves the page STAYED
//     -- which is the half a growth bug breaks.
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

// Each frame's own bytes, from its own address. Kept invertible so a
// mismatch could in principle say whose frame the memory holds.
static unsigned char pattern_for(const volatile unsigned char *p, int i) {
    unsigned long a = (unsigned long)(unsigned long long)(const void *)p;
    return (unsigned char)((a >> 4) ^ (unsigned long)(i * 31 + 7));
}

// One frame, deliberately sized well under the 2048-byte limit
// USERLAND_CFLAGS enforces -- see kernel/uaddr.h's note on why a frame
// larger than UADDR_STACK_GROW_GAP would leap the growable region
// instead of extending it.
#define FRAME_BYTES 1024

// Returns the lowest stack address reached. `depth` counts DOWN so the
// recursion is bounded by an argument rather than by a global.
static const void *descend(int depth) {
    volatile unsigned char frame[FRAME_BYTES];
    for (int i = 0; i < FRAME_BYTES; i++) frame[i] = pattern_for(frame, i);

    const void *deepest = (const void *)frame;
    if (depth > 0) {
        const void *below = descend(depth - 1);
        if (below < deepest) deepest = below;
    }

    // On the way OUT: this frame's bytes must have survived everything
    // the deeper calls did to the stack below it.
    for (int i = 0; i < FRAME_BYTES; i++) {
        if (frame[i] != pattern_for(frame, i)) {
            failures++;
            printf("  FAIL  frame at %lx corrupted at byte %d\n",
                   (unsigned long)(unsigned long long)(const void *)frame, i);
            break;
        }
    }
    return deepest;
}

int main(void) {
    sys_print("stackgrow_test: the user stack grows past its initial pages\n");

    volatile unsigned char top_marker[64];
    for (int i = 0; i < 64; i++) top_marker[i] = pattern_for(top_marker, i);

    // 1600 frames of 1 KiB is ~1.6 MiB of stack -- comfortably past the
    // four pages the loader maps, and comfortably inside the 8 MiB
    // reservation. Chosen the way guard_test.c chose its 2 MiB request:
    // big enough that the OLD kernel could not possibly pass, small
    // enough that it is not testing something else (running the machine
    // out of memory would refuse for the wrong reason and look green).
    const void *deepest = descend(1600);

    unsigned long span = (unsigned long)((const unsigned char *)top_marker
                                          - (const unsigned char *)deepest);
    printf("  reached %lu KiB below the entry frame (%lx .. %lx)\n",
           span / 1024,
           (unsigned long)(unsigned long long)deepest,
           (unsigned long)(unsigned long long)top_marker);

    // THE CHECK THE OLD KERNEL COULD NOT PASS. Four pages is 16 KiB, so
    // anything past that was unmapped address space a moment ago.
    check(span > 64u * 1024u, "the stack reached more than 64 KiB deep");
    check(span > 1024u * 1024u, "the stack reached more than 1 MiB deep");

    // The entry frame is the one furthest from anything that grew. If
    // growth mapped a page over it, this is what notices.
    int top_ok = 1;
    for (int i = 0; i < 64; i++)
        if (top_marker[i] != pattern_for(top_marker, i)) top_ok = 0;
    check(top_ok, "the entry frame survived the descent");

    // Growth must not have handed the stack pages the HEAP is using.
    // Anything sbrk returns has to sit below everything the recursion
    // touched -- if the two regions ever met, this is the cheap way to
    // see it, and it costs one syscall.
    void *heap = sys_sbrk(4096);
    check(heap != (void *)-1, "sbrk still works after the stack grew");
    if (heap != (void *)-1) {
        check((const void *)heap < deepest,
              "the heap is still below the deepest stack page");
        // Writing to it is the half that would corrupt rather than
        // merely look wrong, the same reason guard_test.c memsets.
        memset(heap, 0xA5, 4096);
        check(top_ok, "the entry frame survived a heap write");
    }

    if (failures == 0) sys_print("stackgrow_test: all checks passed\n");
    else               printf("stackgrow_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
