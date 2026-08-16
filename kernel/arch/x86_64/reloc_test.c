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
#include "multiboot.h" // module ranges -- see the module test at the bottom

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

// Stage 3. These are written to pass whether or not this boot actually
// relocated -- `nokaslr`, or a machine with no room above the image,
// are legitimate outcomes, and a test that demanded relocation would
// fail on exactly the configurations where declining was correct. What
// they assert is CONSISTENCY: if it moved, everything must agree about
// where it moved to.
KTEST("reloc", "a relocated kernel is where it says it is") {
    uint64_t delta = kernel_reloc_delta();
    if (delta == 0) return; // did not relocate this boot -- see above

    // The link base is 1M (linker.ld) and the delta is always a whole
    // number of 2MiB pages, which is what keeps the read-only band
    // inside a single 2MiB page for paging_enforce_wx().
    KTEST_ASSERT((delta % 0x200000ULL) == 0);
    KTEST_ASSERT((uint64_t)(uintptr_t)__kimage_start == 0x100000ULL + delta);
}

// The check that would have caught the bug this stage nearly shipped.
// paging.c reaches the page tables by LINKER SYMBOL (`extern uint64_t
// p2_tables[2048]`), so after relocation those symbols name the COPIED
// tables -- and if CR3 still pointed at the originals, every write
// paging_enforce_wx() and vmm_map_user_page() make would land in a
// table nobody walks. Nothing faults; W^X just silently stops applying.
// Asserting the CPU and the symbols agree is the whole test.
KTEST("reloc", "CR3 walks the same page tables the symbols name") {
    extern uint64_t p4_table[512];
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    KTEST_ASSERT((cr3 & ~0xFFFULL) == ((uint64_t)(uintptr_t)p4_table & ~0xFFFULL));
}

KTEST("reloc", "the table sits between its own linker symbols") {
    // .krelocs must stay inside the image: linker.ld places it after
    // .data and before .bss, and an orphaned section would land
    // somewhere else entirely -- with PHDRS declared, wherever ld felt
    // like putting it.
    extern const uint32_t __krelocs_start[];
    extern const uint32_t __krelocs_end[];
    // Compared as addresses, not as arrays: `a > b` on two arrays is
    // a -Warray-compare warning (and in C++23 an error), because it
    // reads as comparing contents.
    KTEST_ASSERT((const char *)__krelocs_start >= __kimage_start);
    KTEST_ASSERT((const char *)__krelocs_end <= _kernel_end);
    KTEST_ASSERT((const void *)__krelocs_end > (const void *)__krelocs_start);
}


// The relocation must not land on a GRUB MODULE.
//
// This is the check that would have caught the Live CD blocker
// (docs/live-cd-design.md): slot_usable() weighed the old image and the
// multiboot info structure and knew nothing about modules, so a big
// enough module in low memory could be overwritten by the relocated
// kernel -- randomly, depending on the base, and never under `nokaslr`.
//
// SKIPS when there are no modules, and says so: a green tick here on a
// boot with nothing loaded would be a check that cannot fail. The Live
// CD work is what makes this meaningful, and the skip is the honest
// state until then.
KTEST("reloc", "the running image does not overlap any GRUB module") {
    struct multiboot_module_info mod;
    if (!multiboot_get_module(0, &mod) || !mod.found) {
        KTEST_SKIP("no GRUB modules loaded on this boot");
    }

    uint64_t img_start = (uint64_t)(uintptr_t)__kimage_start;
    uint64_t img_end = (uint64_t)(uintptr_t)_kernel_end;

    for (int i = 0; ; i++) {
        if (!multiboot_get_module(i, &mod) || !mod.found) break;
        // Half-open ranges on both sides: touching end-to-start is fine,
        // overlapping by a byte is not.
        KTEST_ASSERT(mod.end <= img_start || mod.start >= img_end);
    }
}
