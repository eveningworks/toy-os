// Tests for the kernel's own relocation table (kernel ASLR, M2).
//
// These run in the LIVE kernel, after kernel_main() has already applied
// the table with a delta of zero -- so they are checking the table this
// running image actually shipped with, not a reconstruction of it.
//
// What they can and cannot prove is worth stating, because the gap is
// the whole of stage 3. They prove the table describes this image: the
// right size, every entry inside the image, every entry pointing at a
// word that holds a reference into the image, and a correct verdict on
// which deltas are safe. They do NOT prove that a nonzero delta yields
// a kernel that boots -- nothing has moved the image, and no test
// inside a running kernel can move it out from under itself. That is
// stage 3's job, and until then it is a gap, not a covered case.
#include "ktest.h"
#include "kapi.h"
#include "reloc.h"

extern char __kimage_start[];
extern char _kernel_end[];

KTEST("reloc", "the image carries a relocation table") {
    // A real kernel has thousands of absolute references; an empty or
    // tiny table means genrelocs.py filtered everything out, which
    // would leave kernel_relocate() a silent no-op rather than a
    // failure. The bound is deliberately loose -- this is a smoke test
    // for "the table exists and is plausibly complete", not a count
    // that has to be updated whenever the kernel grows a function.
    KTEST_ASSERT(kernel_reloc_count() > 1000);
    KTEST_ASSERT(kernel_reloc_table_bytes() == kernel_reloc_count() * 4);
}

KTEST("reloc", "every entry describes a reference into this image") {
    // The check that would catch a table that had drifted from its
    // image -- a stale entry points at a word holding whatever else
    // happens to be there, which is almost never an in-image address.
    KTEST_ASSERT(kernel_reloc_implausible() == 0);
}

KTEST("reloc", "a zero delta is safe, and was already applied at boot") {
    KTEST_ASSERT(kernel_reloc_check(0) == 0);
}

KTEST("reloc", "a delta that keeps the image under 2GiB is safe") {
    // 64MiB up: comfortably inside the range -mcmodel=kernel's
    // sign-extended 32-bit immediates can address, and roughly the
    // scale stage 3 would actually pick from.
    KTEST_ASSERT(kernel_reloc_check(64 * 1024 * 1024) == 0);
}

// The bound that decides how high a randomized base may go, and the
// reason kernel_reloc_check() exists at all rather than being assumed.
// -mcmodel=kernel emits every 32-bit absolute reference as a SIGN-
// EXTENDED immediate, so a base that pushes any of them past 2GiB
// turns that reference negative. This asserts the check actually
// notices -- without it, "0 bad entries" at a sane delta would be
// indistinguishable from a function that always returns 0.
KTEST("reloc", "a delta that pushes the image past 2GiB is refused") {
    KTEST_ASSERT(kernel_reloc_check(0x7FFFFFFF) > 0);
}

KTEST("reloc", "the table sits between its own linker symbols") {
    // .krelocs must stay inside the image: linker.ld places it after
    // .data and before .bss, and an orphaned section would land
    // somewhere else entirely -- with PHDRS declared, wherever ld felt
    // like putting it.
    extern const uint32_t __krelocs_start[];
    extern const uint32_t __krelocs_end[];
    KTEST_ASSERT((char *)__krelocs_start >= __kimage_start);
    KTEST_ASSERT((char *)__krelocs_end <= _kernel_end);
    KTEST_ASSERT(__krelocs_end > __krelocs_start);
}
