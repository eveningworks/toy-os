#include "paging.h"
#include <stddef.h>

// boot.asm identity-maps the first 4GiB with 2MiB pages, laid out as one
// flat 2048-entry PDE array (four 1GiB P2 tables back to back). Exported
// via `global p2_tables` there.
extern uint64_t p2_tables[2048];

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63) // requires EFER.NXE, set once in boot.asm

#define HUGE_SIZE     0x200000ULL
#define PDE_COUNT     2048         // 4GiB / 2MiB

// Section boundaries from linker.ld. Their VALUES are what matters here,
// not their contents -- declared as arrays so the symbol's address is
// the number, which is the standard way to read a linker symbol from C.
extern char __kimage_start[];
extern char __ktext_start[];
extern char __ktext_end[];
extern char __kdata_start[];

// One freshly-split 4KB page table per 2MB slot we've touched. This
// milestone only needs to split a couple of slots (for a small user-test
// region), so a small fixed pool is enough -- this is not a general
// physical-memory allocator.
#define MAX_SPLIT_TABLES 4
static uint64_t split_tables[MAX_SPLIT_TABLES][512] __attribute__((aligned(4096)));
static int split_slot_pde[MAX_SPLIT_TABLES];
static int split_slot_count = 0;

// The same idea for W^X, kept separate on purpose: these tables are
// installed once at boot and never handed to a user page, while the pool
// above hands single pages to ring 3. Sharing one pool would let a
// paging_make_user_page() call land inside a table that is describing
// the kernel's own .text.
//
// The pool bounds how much read-only kernel image can be covered: one
// table per 2MiB, so 4 tables is 8MiB. Everything from __kimage_start to
// __kdata_start (.boot + .text + .rodata + .eh_frame + .ktests) is about
// 620KiB today and lands in a single table.
#define MAX_WX_TABLES 4
static uint64_t wx_tables[MAX_WX_TABLES][512] __attribute__((aligned(4096)));
static int wx_table_count = 0;

static void flush_tlb(void) {
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        ::: "rax", "memory"
    );
}

// NOTE: the pages this hands out are writable AND executable, which is
// what its one intended caller (the legacy ring-3 test path) needs -- it
// pokes code into a scratch region and jumps to it. It has no callers
// left today, and paging_wx_violations() below will correctly report
// every page it creates, because they genuinely are W^X holes. If it
// ever gains a caller again, that caller owes the region an explicit
// permission split the way elf.c gives one to a real process.
int paging_make_user_page(uint64_t vaddr) {
    uint64_t pde_index = (vaddr >> 21) & 0x7FF; // 2048 entries total (4GiB / 2MiB)
    if (pde_index >= 2048) return 0;

    uint64_t pte_index = (vaddr >> 12) & 0x1FF;
    uint64_t page_base = vaddr & ~0xFFFULL;
    uint64_t region_base_2mb = vaddr & ~0x1FFFFFULL;

    uint64_t *pt = 0;
    for (int i = 0; i < split_slot_count; i++) {
        if (split_slot_pde[i] == (int)pde_index) { pt = split_tables[i]; break; }
    }

    if (!pt) {
        if (split_slot_count >= MAX_SPLIT_TABLES) return 0;
        pt = split_tables[split_slot_count];
        split_slot_pde[split_slot_count] = (int)pde_index;
        split_slot_count++;

        // Populate all 512 entries to match exactly what the huge page
        // was already providing (identity-mapped, supervisor-only), so
        // nothing else sharing this 2MB region changes behavior.
        for (int i = 0; i < 512; i++) {
            uint64_t phys = region_base_2mb + (uint64_t)i * 4096;
            pt[i] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        }

        // The PDE itself also needs the USER bit -- x86-64 paging ANDs
        // permissions down the hierarchy, so without it here, no 4KB
        // page under this table could ever be user-accessible regardless
        // of its own PTE flags.
        p2_tables[pde_index] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    pt[pte_index] = page_base | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    flush_tlb();
    return 1;
}

// The permissions one 4KiB page of the identity map should end up with.
// Three bands, decided purely by where the address falls relative to the
// linker symbols:
//
//   [__ktext_start, __ktext_end)     .text            -- read-only, executable
//   [__kimage_start, __kdata_start)  .boot/.rodata/
//                                    .eh_frame/.ktests -- read-only, NX
//   everything else                  low 1MiB, .data,
//                                    .bss, all RAM,
//                                    the framebuffer,
//                                    MMIO              -- writable, NX
//
// Note the low 1MiB deliberately stays WRITABLE: 0xB8000 is the VGA text
// buffer boot.asm's error path writes to, and nothing there is code the
// kernel jumps to.
static uint64_t wx_page_flags(uint64_t addr) {
    if (addr >= (uintptr_t)__ktext_start && addr < (uintptr_t)__ktext_end) {
        return PAGE_PRESENT;
    }
    if (addr >= (uintptr_t)__kimage_start && addr < (uintptr_t)__kdata_start) {
        return PAGE_PRESENT | PAGE_NX;
    }
    return PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX;
}

int paging_enforce_wx(void) {
    // Which 2MiB slots contain something that must NOT be writable. Only
    // those get split down to 4KiB; every other slot in the 4GiB map is
    // pure data and only needs its NX bit set, which leaves it a huge
    // page and costs no memory and no extra TLB pressure.
    uint64_t split_lo = (uintptr_t)__kimage_start & ~(HUGE_SIZE - 1);
    uint64_t split_hi = ((uintptr_t)__kdata_start + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);

    for (int pde = 0; pde < PDE_COUNT; pde++) {
        uint64_t base = (uint64_t)pde * HUGE_SIZE;

        if (base < split_lo || base >= split_hi) {
            p2_tables[pde] |= PAGE_NX;
            continue;
        }

        if (wx_table_count >= MAX_WX_TABLES) return 0;
        uint64_t *pt = wx_tables[wx_table_count++];
        for (int i = 0; i < 512; i++) {
            uint64_t addr = base + (uint64_t)i * 4096;
            pt[i] = addr | wx_page_flags(addr);
        }

        // The PDE is not a leaf any more, so it carries no NX and stays
        // writable: permissions are ANDed down the walk, and it is the
        // PTEs above that decide. Supervisor-only -- no USER bit; this
        // region is the kernel's own image.
        p2_tables[pde] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
    }

    // CR0.WP is what makes the read-only half of this mean anything.
    // With WP clear -- the state the CPU comes out of reset in, and what
    // GRUB hands us -- a supervisor write IGNORES the read-write bit
    // entirely, so ring 0 could still scribble over .text through a
    // mapping that reads as read-only in every page table. NX needs no
    // such switch (EFER.NXE, set in boot.asm, covers it), which is
    // exactly why this one is easy to miss: half the protection would
    // work, and the half that didn't would still LOOK right in a dump of
    // the tables.
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16); // WP
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    flush_tlb();
    return 1;
}

uint64_t paging_kernel_leaf(uint64_t vaddr) {
    uint64_t pde_index = (vaddr >> 21) & 0x7FF;
    if (pde_index >= PDE_COUNT) return 0;

    uint64_t pde = p2_tables[pde_index];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (pde & PAGE_HUGE) return pde;

    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);
    return pt[(vaddr >> 12) & 0x1FF];
}

int paging_wx_violations(void) {
    int bad = 0;
    for (int pde = 0; pde < PDE_COUNT; pde++) {
        uint64_t e = p2_tables[pde];
        if (!(e & PAGE_PRESENT)) continue;

        if (e & PAGE_HUGE) {
            if ((e & PAGE_WRITABLE) && !(e & PAGE_NX)) bad++;
            continue;
        }

        // A non-leaf PDE grants nothing by itself -- walk its PTEs. But
        // NX on the PDE would forbid execution beneath it regardless, so
        // a table under an NX parent can't produce a violation.
        if (e & PAGE_NX) continue;
        uint64_t *pt = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
        for (int i = 0; i < 512; i++) {
            uint64_t p = pt[i];
            if (!(p & PAGE_PRESENT)) continue;
            if ((p & PAGE_WRITABLE) && !(p & PAGE_NX)) bad++;
        }
    }
    return bad;
}

// ---------------------------------------------------------------------
// SMEP / SMAP
// ---------------------------------------------------------------------
//
// Two CR4 bits that make the CPU itself refuse what the page tables only
// describe. SMEP (bit 20): ring 0 cannot EXECUTE a page marked
// user-accessible, which kills the return-to-userspace family of exploits
// outright -- a corrupted kernel return address pointing into a ring-3
// buffer faults instead of running it. SMAP (bit 21): ring 0 cannot READ
// or WRITE a user page either, unless EFLAGS.AC is set.
//
// **This kernel never sets AC.** Kernel code reaches user memory only
// through vmm.h's copy helpers, which walk to the frame and go through
// the kernel's own identity map (U=0) -- a supervisor access to a
// supervisor page, which SMAP does not police. So SMAP here has no
// relaxation window at all, and STAC/CLAC appear nowhere. Read vmm.h
// before adding any other way to touch a ring-3 pointer.
//
// **The trap, and it is a live one:** paging_make_user_page() adds the
// USER bit to the KERNEL's own identity mapping of a page. Any page it
// touches becomes SMAP-protected against the kernel's ordinary access to
// it, at the address the kernel normally uses. Nothing calls it today
// outside this file; a future caller must copy through vmm.h's helpers
// or expect a fault in code that looks entirely innocent.
//
// Both are silently absent on QEMU's default `qemu64` CPU model, so the
// hardware path only runs under `--cpu max` (or a real machine) -- the
// same trap the RDRAND work paid for. Absence is not a failure: an
// unsupported bit is left clear and reported as such, because setting a
// reserved CR4 bit is a #GP, not a no-op.

#define CR4_SMEP_BIT (1ULL << 20)
#define CR4_SMAP_BIT (1ULL << 21)

static uint64_t read_cr4_local(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

int paging_enable_smep_smap(void) {
    // CPUID.07H:0:EBX bit 7 = SMEP, bit 20 = SMAP. Read directly rather
    // than through cpu_info_get(), which builds a large struct and is
    // not necessarily initialised this early in kernel_main().
    uint32_t ebx = 0;
    uint32_t eax = 0, ecx = 0, edx = 0;
    __asm__ volatile ("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(7), "c"(0));

    int have_smep = (ebx >> 7) & 1u;
    int have_smap = (ebx >> 20) & 1u;

    uint64_t cr4 = read_cr4_local();
    if (have_smep) cr4 |= CR4_SMEP_BIT;
    if (have_smap) cr4 |= CR4_SMAP_BIT;
    if (have_smep || have_smap) {
        __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }

    return (have_smep ? PAGING_SMEP_ON : 0) | (have_smap ? PAGING_SMAP_ON : 0);
}

int paging_smep_smap_state(void) {
    uint64_t cr4 = read_cr4_local();
    return ((cr4 & CR4_SMEP_BIT) ? PAGING_SMEP_ON : 0) |
            ((cr4 & CR4_SMAP_BIT) ? PAGING_SMAP_ON : 0);
}
