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
#include "multiboot.h"
#include "random_hw.h"
#include "string.h"

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

// How far the running image was moved. Declared up here because every
// function that READS a fixup location needs it: the table records
// LINK-time addresses, so once the image has moved, the word an entry
// describes lives at location + g_delta. Reading the bare location
// instead reaches into the abandoned image -- which is still mapped and
// still holds pre-relocation values, so it does not fault; it just
// answers questions about a kernel that is no longer running.
static uint64_t g_delta = 0;

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
        uint64_t loc = (uint64_t)(*e & RELOC_ADDR_MASK) + g_delta;
        if (*e & RELOC_WIDE) continue; // 64-bit references cannot overflow
        int64_t v = (int64_t)*(const int32_t *)loc + delta;
        if (v < 0 || v > 0x7FFFFFFF) bad++;
    }
    return bad;
}

// ---- stage 3: picking a base and moving the image ----
//
// Everything below runs from long_mode_start, BEFORE kernel_main, from
// the image still sitting at its link address. Three constraints shape
// it, and none of them are obvious from the outside:
//
//   * It cannot log. klog_write() goes straight out the serial port and
//     serial_init() has not run, so every decision here is recorded in
//     a global and printed by kernel_main() once it can.
//   * It cannot call krandom_init(). That harvests jitter by spinning
//     until pit_ticks() changes, and the PIT is not initialised yet --
//     on a machine without RDSEED/RDRAND (QEMU's default qemu64, which
//     is most test runs) it would spin forever. So the base gets its
//     own minimal entropy path, and reports honestly which one it got.
//   * Every global it sets must be set BEFORE the copy, because the
//     copy is what carries them into the image that will actually run.
//     Assigning after the copy writes only to the abandoned image, and
//     the running kernel would report a delta of zero while sitting at
//     a relocated address.

#define TWO_MIB 0x200000ULL

static int g_entropy_hw = 0;
static uint64_t g_slots = 0;
static const char *g_note = "not attempted";

// The relocated base must be 2MiB-aligned RELATIVE to the link base,
// i.e. the delta is a multiple of 2MiB, which keeps every page-level
// assumption elsewhere intact. paging_enforce_wx() in particular relies
// on the whole read-only band (~672KB) sitting inside a single 2MiB
// page; a delta that is any other size would straddle two and silently
// leave half the image writable.
#define MAX_SLOT_INDEX 2048 // 2048 * 2MiB = the 4GiB the identity map covers

#define MAX_REGIONS 32
static struct { uint64_t base, len; } g_avail[MAX_REGIONS];
static int g_avail_count = 0;

static void collect_region(const struct multiboot_mmap_region *r) {
    if (r->type != 1) return; // available RAM only
    if (g_avail_count >= MAX_REGIONS) return;
    g_avail[g_avail_count].base = r->base;
    g_avail[g_avail_count].len = r->length;
    g_avail_count++;
}

static int range_fits_in_ram(uint64_t start, uint64_t end) {
    for (int i = 0; i < g_avail_count; i++) {
        uint64_t rs = g_avail[i].base, re = g_avail[i].base + g_avail[i].len;
        if (start >= rs && end <= re) return 1;
    }
    return 0; // must fit ENTIRELY within one reported available region
}

static int overlaps(uint64_t a1, uint64_t a2, uint64_t b1, uint64_t b2) {
    return a1 < b2 && b1 < a2;
}

// One 64-bit value to choose a slot with. Hardware if the CPU has it,
// otherwise the timestamp counter -- which is weak, and is why
// kernel_reloc_entropy_hw() exists for the boot log to say so rather
// than let a reader assume the base is unpredictable.
static uint64_t boot_entropy(void) {
    uint64_t v;
    if (arch_has_rdseed() && arch_rdseed64(&v)) { g_entropy_hw = 1; return v; }
    if (arch_has_rdrand() && arch_rdrand64(&v)) { g_entropy_hw = 1; return v; }
    g_entropy_hw = 0;
    return arch_rdtsc();
}

// Is `k` (a slot index, delta = k * 2MiB) a base this image can live at?
// Does [start, end) land on top of a GRUB MODULE?
//
// It could, and nothing stopped it: this function checked the old image
// and the multiboot info structure and nothing else, because there have
// been no modules since it was written. A Live CD puts a multi-megabyte
// filesystem image in low memory as a module (docs/live-cd-design.md),
// and relocating the kernel over it corrupts it silently -- the failure
// depends on the random base, so it reproduces on some boots and not
// others, and `nokaslr` makes it vanish entirely. That reads as "the
// ASLR code is broken" rather than "the module was eaten", which is the
// worst possible signpost.
//
// pmm.c already reserves module ranges, and does not help: this runs
// long before the PMM exists.
static int hits_a_module(uint64_t start, uint64_t end) {
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        if (!mod.found) break;
        if (overlaps(start, end, mod.start, mod.end)) return 1;
    }
    return 0;
}

static int slot_usable(uint64_t k, uint64_t img_start, uint64_t img_size,
                       uint64_t old_end, uint64_t info_start, uint64_t info_end) {
    uint64_t delta = k * TWO_MIB;
    uint64_t start = img_start + delta;
    uint64_t end = start + img_size;

    // Above the old image, always. Two things depend on it: the copy
    // becomes non-overlapping (so a forward k_memcpy is correct), and
    // the abandoned image stays below the new one where pmm.c reserves
    // it -- it still holds the GDT the CPU uses until gdt_init().
    if (start < old_end) return 0;
    if (end > 0x100000000ULL) return 0;      // the identity map stops at 4GiB
    if (!range_fits_in_ram(start, end)) return 0;
    if (info_end > info_start && overlaps(start, end, info_start, info_end)) return 0;
    if (hits_a_module(start, end)) return 0;
    if (kernel_reloc_check((int64_t)delta) != 0) return 0; // would break a 32-bit reference
    return 1;
}

// Repoints CR3 at the RELOCATED copy of boot.asm's page tables.
//
// This is the step stage 2 concluded was unnecessary, and it was wrong
// about it -- for a reason that has nothing to do with mapping. The
// tables do map the new image already, since they identity-map the
// whole low 4GiB. The problem is that paging.c reaches them by LINKER
// SYMBOL (`extern uint64_t p2_tables[2048]`), so after relocation
// paging_enforce_wx() and vmm_map_user_page() write into the copied
// tables while the CPU is still walking the originals. Nothing faults;
// W^X simply never takes effect and user mappings land in a table
// nobody reads.
//
// The copy's p2 entries are already correct -- they are pure identity
// mappings, whose values do not depend on where the table itself
// lives. Only the two levels of internal pointers need rewriting.
//
// Every address here is computed as `symbol + delta` because this runs
// from the OLD image, where the symbols still name the old tables.
// Writing through the bare symbols would rewrite the tables we are
// about to abandon and reload CR3 with the address it already holds --
// a no-op that looks exactly like a working call.
static void adopt_relocated_page_tables(uint64_t delta) {
    extern uint64_t p4_table[512];
    extern uint64_t p3_table[512];
    extern uint64_t p2_tables[2048];

    uint64_t new_p4 = (uint64_t)(uintptr_t)p4_table + delta;
    uint64_t new_p3 = (uint64_t)(uintptr_t)p3_table + delta;
    uint64_t new_p2 = (uint64_t)(uintptr_t)p2_tables + delta;

    // present + writable + user, matching boot.asm -- permissions are
    // ANDed down the whole walk, so a missing USER bit at a parent
    // level would block ring 3 no matter what the leaf says.
    for (int i = 0; i < 4; i++) {
        ((uint64_t *)(uintptr_t)new_p3)[i] = (new_p2 + (uint64_t)i * 4096) | 0x7;
    }
    ((uint64_t *)(uintptr_t)new_p4)[0] = new_p3 | 0x7;

    __asm__ volatile("mov %0, %%cr3" :: "r"(new_p4) : "memory");
}

uint64_t kernel_reloc_delta(void) { return g_delta; }
int kernel_reloc_entropy_hw(void) { return g_entropy_hw; }
uint64_t kernel_reloc_slots(void) { return g_slots; }
const char *kernel_reloc_note(void) { return g_note; }

uint64_t kernel_relocate_boot(uint64_t mb2_info) {
    uint64_t img_start = (uint64_t)(uintptr_t)__kimage_start;
    uint64_t old_end = (uint64_t)(uintptr_t)_kernel_end;
    uint64_t img_size = old_end - img_start;

    // Safe here: this only records the pointer in a global, which the
    // copy then carries into the new image. kernel_main() calls it
    // again, harmlessly.
    multiboot_set_info(mb2_info);

    // An explicit off switch, and the only recovery path if a machine
    // turns out not to survive being relocated. Same spelling Linux
    // uses, on the GRUB command line.
    const char *cmdline = multiboot_cmdline();
    if (cmdline && k_strstr(cmdline, "nokaslr")) {
        g_note = "disabled by nokaslr on the command line";
        return 0;
    }

    g_avail_count = 0;
    multiboot_mmap_foreach(collect_region);
    if (g_avail_count == 0) {
        g_note = "no memory map -- nowhere known to be safe";
        return 0;
    }

    uint64_t info_start = 0, info_end = 0;
    multiboot_get_info_range(&info_start, &info_end);

    uint64_t slots = 0;
    for (uint64_t k = 1; k < MAX_SLOT_INDEX; k++) {
        if (slot_usable(k, img_start, img_size, old_end, info_start, info_end)) slots++;
    }
    if (slots == 0) {
        g_slots = 0;
        g_note = "no candidate base fits -- not enough RAM above the image";
        return 0;
    }

    uint64_t pick = boot_entropy() % slots;
    uint64_t chosen = 0;
    uint64_t seen = 0;
    for (uint64_t k = 1; k < MAX_SLOT_INDEX; k++) {
        if (!slot_usable(k, img_start, img_size, old_end, info_start, info_end)) continue;
        if (seen == pick) { chosen = k; break; }
        seen++;
    }
    if (chosen == 0) { // unreachable, but a wrong base is a dead machine
        g_note = "slot selection disagreed with itself";
        return 0;
    }

    uint64_t delta = chosen * TWO_MIB;

    // EVERY global must be written before the copy -- see this section's
    // top comment. After the copy they belong to the abandoned image.
    g_delta = delta;
    g_slots = slots;
    g_note = "relocated";

    // Non-overlapping by construction (delta >= img_size, since the base
    // is above the old image's end), so a forward copy is correct.
    // .bss is copied rather than zeroed on purpose: it holds the stack
    // this code is standing on, and the caller continues on the copy.
    k_memcpy((void *)(uintptr_t)(img_start + delta), (const void *)(uintptr_t)img_start, img_size);

    kernel_relocate((int64_t)delta); // patches the NEW image; the table holds link-time addresses
    adopt_relocated_page_tables(delta);
    return delta;
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
        uint64_t loc = (uint64_t)(*e & RELOC_ADDR_MASK) + g_delta;
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
