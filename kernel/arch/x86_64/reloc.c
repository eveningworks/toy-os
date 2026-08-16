// Kernel self-relocation: applying the image's own relocation table.
//
// The kernel is linked at 1M (linker.ld). To run from anywhere else,
// every ABSOLUTE reference in the image has to be adjusted by however
// far the image moved. tools/genrelocs.py finds those references at
// build time and links a table of them into the image; this file is
// what applies it. Together they are Milestone 2's kernel ASLR, and
// they are the shape Linux's CONFIG_RELOCATABLE uses -- a build-time
// relocs tool over `ld --emit-relocs`, not a PIE link.
//
// WHY THIS IS CHEAP HERE. boot.asm identity-maps the whole low 4GiB,
// so VA == PA and moving the image keeps it mapped. Every PC-relative
// reference (11,900 of them) then survives a move untouched, and only
// the 7,300 absolute ones need patching.
//
// WHAT IS AND IS NOT BUILT YET. The table, its build-time verification
// and this fixup walk are stage 1 and 2 of the roadmap's three. The
// walk runs at boot with a delta of ZERO, which patches every location
// with +0: that proves the table describes 7,300 real, mapped, writable
// words in this image, and that nothing in the walk faults. It does NOT
// prove a nonzero delta produces a working kernel -- nothing has moved
// the image yet. Stage 3 (pick a random base, copy, jump) is not here.
//
// TWO TRAPS, both of which would only show up as a machine that does
// not boot:
//
//   1. This must run BEFORE paging_enforce_wx(). The fixups write into
//      .text, and enforce_wx makes .text read-only and sets CR0.WP --
//      after which every one of these writes is a page fault in ring 0.
//      kernel_main() calls them in that order for this reason alone.
//
//   2. A relocated base must be ABOVE the link base, never below.
//      pmm.c reserves [0, _kernel_end), and after relocation
//      `_kernel_end` is the NEW end -- so a higher base leaves the old
//      image, the boot stack and boot.asm's page tables all inside the
//      reserved range for free. A lower base would hand the old image's
//      pages, including the live CR3, to the frame allocator.
//
// A note on the page tables, because it looks like an omission: this
// does NOT rebuild CR3. It does not need to. boot.asm's tables live in
// .bss and identity-map the entire low 4GiB, so they already map
// wherever the image lands, and by trap 2 they stay reserved. Keeping
// the boot tables where they are is what removes the "reload CR3 with
// relocated page-table addresses" step the roadmap anticipated.
#include "reloc.h"
#include "klog.h"

// linker.ld places the table after .data and before .bss, so that
// adding it cannot move any address it records. See that file.
extern const uint32_t __krelocs_start[];
extern const uint32_t __krelocs_end[];

// The image's own bounds, for deciding whether a value looks like a
// reference into the kernel rather than a constant.
extern char __kimage_start[];
extern char __kdata_start[]; // end of the read-only bands -- see kernel_reloc_implausible()
extern char _kernel_end[];

#define RELOC_WIDE (1u << 31) // this entry patches 8 bytes, not 4
#define RELOC_ADDR_MASK 0x7FFFFFFFu

uint64_t kernel_reloc_count(void) {
    return (uint64_t)(__krelocs_end - __krelocs_start);
}

uint64_t kernel_reloc_table_bytes(void) {
    return kernel_reloc_count() * sizeof(uint32_t);
}

// Applies every fixup, moving each absolute reference by `delta`.
//
// `delta` is added to the recorded location as well as to the value
// found there: the table records LINK-time addresses, so once the image
// has moved, the word to patch is at location + delta.
void kernel_relocate(int64_t delta) {
    for (const uint32_t *e = __krelocs_start; e != __krelocs_end; e++) {
        uint64_t loc = (uint64_t)(*e & RELOC_ADDR_MASK) + (uint64_t)delta;
        if (*e & RELOC_WIDE) {
            *(uint64_t *)loc += (uint64_t)delta;
        } else {
            // A 32-bit absolute reference, which -mcmodel=kernel emits
            // as a sign-extended immediate -- see kernel_reloc_check().
            *(uint32_t *)loc += (uint32_t)delta;
        }
    }
}

// Would `delta` produce a working image? Returns the number of entries
// that would NOT survive it, so 0 means the move is safe.
//
// This is the check that decides how high a base ASLR may pick, and it
// is not theoretical: -mcmodel=kernel makes every 32-bit absolute
// reference a SIGN-EXTENDED immediate, so a value that crosses 2GiB
// after the move silently becomes a negative address. Counting the
// failures at a candidate delta is far cheaper than discovering it as a
// kernel that triple-faults.
uint64_t kernel_reloc_check(int64_t delta) {
    uint64_t bad = 0;
    for (const uint32_t *e = __krelocs_start; e != __krelocs_end; e++) {
        uint64_t loc = (uint64_t)(*e & RELOC_ADDR_MASK);
        if (*e & RELOC_WIDE) continue; // 64-bit references cannot overflow
        int64_t v = (int64_t)*(const int32_t *)loc + delta;
        if (v < 0 || v > 0x7FFFFFFF) bad++;
    }
    return bad;
}

// Does every entry currently point at a word holding a reference into
// this image? A table that had drifted from the image it ships with
// would fail this almost everywhere, which is what makes it worth
// asserting from a KTEST rather than trusting the build step alone.
//
// Only the READ-ONLY part of the image can be checked this way, and
// that limit is the interesting part. A fixup in .data is a pointer
// the kernel initialised to some address and is then free to
// reassign -- to a kmalloc'd block, to the framebuffer, to anything
// outside the image -- so by the time a KTEST runs, a perfectly
// healthy .data entry routinely points elsewhere. Checking it anyway
// reports a working kernel as corrupt, which is how this function was
// first written. Everything below __kdata_start is immutable, so its
// values still say what the linker wrote.
uint64_t kernel_reloc_implausible(void) {
    uint64_t lo = (uint64_t)(uintptr_t)__kimage_start;
    uint64_t hi = (uint64_t)(uintptr_t)_kernel_end;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__kdata_start;
    uint64_t bad = 0;

    for (const uint32_t *e = __krelocs_start; e != __krelocs_end; e++) {
        uint64_t loc = (uint64_t)(*e & RELOC_ADDR_MASK);
        if (loc < lo || loc >= hi) { bad++; continue; }
        if (loc >= rodata_end) continue; // writable at runtime -- see above

        uint64_t v = (*e & RELOC_WIDE) ? *(const uint64_t *)loc
                                       : (uint64_t)*(const uint32_t *)loc;
        // A page of slack past the end, because a relocation's value is
        // symbol + ADDEND and the addend is not always zero: pmm.c's
        // reserve_range(0, _kernel_end) constant-folds its page
        // round-up into the relocation, so the image genuinely contains
        // a legitimate absolute reference to _kernel_end + 0xfff.
        // Exactly one entry needs this, and finding out which one is
        // why the bound is a measured constant rather than a guess.
        if (v < lo || v > hi + 4096) bad++;
    }
    return bad;
}
