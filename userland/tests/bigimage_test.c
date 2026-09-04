// A ring-3 image BIGGER THAN THE OLD CEILING, and a heap that does not
// alias it -- run by tools/usertest_run.py, or by hand
// (`run bigimage_test`).
//
// **THIS FILE WOULD NOT HAVE LINKED.** userland/rt/link.ld carried an
// ASSERT refusing any binary whose sections reached UADDR_HEAP_BASE, a
// fixed 1 MiB above the image base, because SYS_SBRK started handing out
// pages there: a larger image would have had its own .bss silently
// aliased by its own malloc. The 4 MiB of .bss below is four times that
// limit, so the mere fact that this builds is half the assertion, and
// the checks are the other half.
//
// What replaced the ceiling is a heap base derived from where the image
// actually ends (kernel/proc/elf.c's out_image_end -> struct sched_mm's
// heap_base), which is what Linux's fs/binfmt_elf.c does in set_brk().
//
// **THE PATTERN IS DERIVED FROM THE ADDRESS, for the reason /tests/
// memtest states**: a constant fill cannot detect two virtual pages
// sharing one physical frame, because both read back the constant and
// both look perfect. Aliasing is precisely the failure this guards, so
// the bytes have to be able to say which address they belong to.
//
// .bss rather than .data on purpose: a 4 MiB initialised array would be
// 4 MiB of ELF on disk to be read at every spawn, and it is the MEMORY
// image being tested, not the file.
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/utest.h"

// Four times the old 1 MiB image limit.
#define BIG_BYTES (4u * 1024u * 1024u)
static unsigned char g_big[BIG_BYTES];

static unsigned char pattern_at(unsigned long i) {
    return (unsigned char)((i >> 12) ^ (i * 31u) ^ 0x5Au);
}

// Every page, not every byte: 4 MiB of byte-at-a-time work is slow under
// TCG and a whole page is the granularity anything could alias at.
#define STEP 4096u

int main(void) {
    utest_begin("bigimage_test", "a 4 MiB image loads, and its heap is elsewhere", 0);

    unsigned long lo = (unsigned long)(unsigned long long)(void *)&g_big[0];
    unsigned long hi = (unsigned long)(unsigned long long)(void *)&g_big[BIG_BYTES - 1];
    utest_notef(".bss spans %lx .. %lx", lo, hi);

    // The array must genuinely straddle the old limit, or every check
    // below passes on a kernel that still had one. Derived from the
    // image base the linker script uses, not from a kernel header --
    // uaddr.h is kernel-internal and a ring-3 test cannot see it.
    utest_check(hi - 0x8000000000UL > 0x100000UL,
          "the image really does extend past the old 1 MiB ceiling");

    for (unsigned long i = 0; i < BIG_BYTES; i += STEP) g_big[i] = pattern_at(i);

    int intact = 1;
    for (unsigned long i = 0; i < BIG_BYTES; i += STEP)
        if (g_big[i] != pattern_at(i)) { intact = 0; break; }
    utest_check(intact, "every page of the image's .bss holds its own pattern");

    // THE ALIASING CHECK, which is what the old ASSERT was protecting.
    // A heap starting at a fixed 1 MiB would be handing out pages this
    // array is already using; writing a megabyte of heap and re-reading
    // the array is what makes that visible rather than theoretical.
    size_t heap_bytes = 1024u * 1024u;
    unsigned char *heap = malloc(heap_bytes);
    utest_check(heap != NULL, "malloc succeeded on top of a big image");
    if (heap) {
        unsigned long hlo = (unsigned long)(unsigned long long)(void *)heap;
        utest_notef("heap block at %lx", hlo);
        utest_check(hlo > hi, "the heap starts ABOVE the end of the image");

        memset(heap, 0xC3, heap_bytes);

        intact = 1;
        for (unsigned long i = 0; i < BIG_BYTES; i += STEP)
            if (g_big[i] != pattern_at(i)) {
                utest_notef(".bss byte at offset %lu reads %02x, wanted %02x",
                            i, g_big[i], pattern_at(i));
                intact = 0;
                break;
            }
        utest_check(intact, "the image survived a megabyte of heap traffic");
        free(heap);
    }

    return utest_end();
}
