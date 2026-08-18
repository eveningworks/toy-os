#include "paging.h"
#include "multiboot.h"
#include "string.h"
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

// --- guard pages (kernel stacks) -------------------------------------
//
// One 4KiB page of the identity map made NOT PRESENT, so a touch faults
// instead of scribbling on whatever the neighbour happens to be. Its one
// caller is the per-process kernel stack array (scheduler.c), which
// leaves a guard page below each stack -- Linux's CONFIG_VMAP_STACK in
// miniature, and the same reasoning uaddr.h already gives for the ring-3
// stack: an overflow that faults is a diagnosable bug, one that silently
// overwrites the next slot is a mystery in some unrelated process.
//
// The kernel stacks live in .bss, which paging_enforce_wx() leaves as
// 2MiB huge pages, so the slot has to be split first. The split
// populates every sibling entry from wx_page_flags(), i.e. with exactly
// the permissions the huge page was already providing -- nothing else
// sharing the 2MiB changes behaviour.
//
// Its own pool, not the user-page or W^X one: those hand out pages to
// ring 3 and describe .text respectively, and a guard page has no
// business landing inside either table.
#define MAX_GUARD_TABLES 4
static uint64_t guard_tables[MAX_GUARD_TABLES][512] __attribute__((aligned(4096)));
static int guard_table_pde[MAX_GUARD_TABLES];
static int guard_table_count = 0;

int paging_unmap_kernel_page(uint64_t vaddr) {
    uint64_t pde_index = (vaddr >> 21) & 0x7FF;
    if (pde_index >= PDE_COUNT) return 0;
    uint64_t pte_index = (vaddr >> 12) & 0x1FF;

    uint64_t pde = p2_tables[pde_index];
    uint64_t *pt;

    if (pde & PAGE_HUGE) {
        // Not split yet. Reuse a table if this slot already has one --
        // 64 guard pages fall in very few 2MiB slots, which is why the
        // pool can be this small.
        pt = 0;
        for (int i = 0; i < guard_table_count; i++) {
            if (guard_table_pde[i] == (int)pde_index) { pt = guard_tables[i]; break; }
        }
        if (!pt) {
            if (guard_table_count >= MAX_GUARD_TABLES) return 0;
            pt = guard_tables[guard_table_count];
            guard_table_pde[guard_table_count] = (int)pde_index;
            guard_table_count++;
            uint64_t base = (uint64_t)pde_index * HUGE_SIZE;
            for (int i = 0; i < 512; i++) {
                uint64_t addr = base + (uint64_t)i * 4096;
                pt[i] = addr | wx_page_flags(addr);
            }
        }
        p2_tables[pde_index] = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
    } else {
        if (!(pde & PAGE_PRESENT)) return 0;
        pt = (uint64_t *)(uintptr_t)(pde & 0x000FFFFFFFFFF000ULL);
    }

    pt[pte_index] = 0; // not present -- the whole point
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

// --- write-combining the framebuffer ---
//
// The problem this solves is worth stating plainly, because it is
// invisible in every test this repo can run: GRUB's linear framebuffer
// is UNCACHED MMIO on real hardware, so each store to it is a bus
// transaction the CPU waits on. gfx_present() pushes millions of them
// per frame, which is seconds per full repaint on a real machine and
// entirely unnoticeable under QEMU, where the "framebuffer" is ordinary
// cached host RAM. Write-combining lets the CPU gather those stores in
// a fill buffer and burst them out, which is the whole fix.
//
// Preferred mechanism is PAT, for one reason worth keeping: it is
// PER-PAGE, so it needs no power-of-two sizing, no natural alignment and
// no free range register -- all three of which an MTRR demands and a
// framebuffer does not always offer.

#define MSR_IA32_PAT       0x277u
#define MSR_IA32_MTRRCAP   0xFEu
#define MSR_IA32_MTRR_DEF  0x2FFu
#define MSR_IA32_MTRR_BASE 0x200u   // base/mask pairs run 0x200,0x201,0x202,...

#define PAT_TYPE_WC   0x01ULL
#define MTRR_TYPE_WC  0x01ULL

#define PAGE_PWT      (1ULL << 3)
#define PAGE_PCD      (1ULL << 4)
// Bit 12 is PAT on a 2MiB page. On a 4KiB page bit 12 is part of the
// PHYSICAL ADDRESS and PAT is bit 7 instead -- setting the wrong one
// does not fault, it silently repoints the mapping somewhere else. Every
// write below checks PAGE_HUGE before choosing.
#define PAGE_PAT_HUGE (1ULL << 12)
#define PAGE_PAT_4K   (1ULL << 7)

static uint64_t read_msr_local(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void write_msr_local(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                                  "d"((uint32_t)(val >> 32)));
}

static uint32_t cpuid_edx1(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(1), "c"(0));
    return edx;
}

// Physical address width, for building an MTRR mask. CPUID leaf
// 0x80000008 is the architectural answer; 36 bits is the pre-leaf
// default and is what the fallback assumes.
static uint32_t phys_addr_bits(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(0x80000000u), "c"(0));
    if (eax < 0x80000008u) return 36;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(0x80000008u), "c"(0));
    uint32_t bits = eax & 0xFFu;
    return (bits >= 32 && bits <= 52) ? bits : 36;
}

static int nopat_requested(void) {
    const char *cmdline = multiboot_cmdline();
    return cmdline && k_strstr(cmdline, "nopat") ? 1 : 0;
}

// Points PAT slot 4 at write-combining, leaving slots 0-3 at their
// architectural defaults so every mapping that does not opt in keeps
// exactly the meaning it had. Slot 4 is selected by PAT=1, PCD=0, PWT=0,
// which is why the PDE writes below set one bit and clear two.
static void pat_install_wc_slot(void) {
    uint64_t pat = read_msr_local(MSR_IA32_PAT);
    pat &= ~(0xFFULL << 32);            // slot 4 lives in byte 4
    pat |= (PAT_TYPE_WC << 32);
    write_msr_local(MSR_IA32_PAT, pat);
}

static int pat_apply(uint64_t phys, uint64_t size) {
    pat_install_wc_slot();

    uint64_t first = phys / HUGE_SIZE;
    uint64_t last  = (phys + size - 1) / HUGE_SIZE;

    for (uint64_t pde = first; pde <= last; pde++) {
        uint64_t e = p2_tables[pde];
        if (!(e & PAGE_PRESENT)) continue;

        if (e & PAGE_HUGE) {
            e &= ~(PAGE_PCD | PAGE_PWT);
            e |= PAGE_PAT_HUGE;
            p2_tables[pde] = e;
            continue;
        }
        // Split by paging_enforce_wx() -- retype the 4KiB leaves instead.
        // Not expected for a framebuffer (it is far above the kernel
        // image), but writing the huge-page bit here would corrupt the
        // mapping rather than fail, so handle it rather than assume.
        uint64_t *pt = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
        for (int i = 0; i < 512; i++) {
            if (!(pt[i] & PAGE_PRESENT)) continue;
            pt[i] = (pt[i] & ~(PAGE_PCD | PAGE_PWT)) | PAGE_PAT_4K;
        }
    }

    // The region was uncached, so nothing of it is in cache to write
    // back -- but the old translations are, and a stale TLB entry would
    // keep the old type. Cheap once at boot.
    __asm__ volatile ("wbinvd" ::: "memory");
    flush_tlb();
    return PAGING_WC_PAT;
}

// A variable-range MTRR describes a power-of-two block at a naturally
// aligned base -- neither of which a framebuffer is obliged to be. So
// cover the range greedily: at each step take the largest block that
// both fits the remaining length and matches the current base's
// alignment. Refuses rather than covering PART of the framebuffer,
// since a half-write-combined framebuffer is a performance bug that
// looks exactly like the one being fixed.
#define MTRR_MAX_USED 8

static int mtrr_apply(uint64_t phys, uint64_t size) {
    uint64_t cap = read_msr_local(MSR_IA32_MTRRCAP);
    uint32_t vcnt = (uint32_t)(cap & 0xFFu);
    if (vcnt == 0) return PAGING_WC_NONE;
    if (vcnt > MTRR_MAX_USED) vcnt = MTRR_MAX_USED;

    // Plan the decomposition before touching any register, so a refusal
    // costs nothing and leaves the MTRRs exactly as the firmware left
    // them.
    uint64_t base[MTRR_MAX_USED], len[MTRR_MAX_USED];
    uint32_t n = 0;
    uint64_t p = phys, remaining = size;
    while (remaining > 0) {
        if (n >= vcnt) return PAGING_WC_NONE;   // needs more ranges than exist
        uint64_t block = 0x1000ULL;
        // Grow while the block stays aligned to its own size and fits.
        while (block * 2 <= remaining && (p % (block * 2)) == 0) block *= 2;
        if (p % block) return PAGING_WC_NONE;   // not even 4KiB aligned
        base[n] = p;
        len[n] = block;
        n++;
        p += block;
        remaining -= block;
    }

    uint64_t mask_hi = ((1ULL << phys_addr_bits()) - 1) & ~0xFFFULL;

    // SDM 11.11.8's memory-type change protocol. Skipping any of it
    // risks the CPU servicing a fetch under the old type mid-change; it
    // costs microseconds, once, at boot.
    unsigned long flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");

    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    // Global pages would survive the TLB flush below and keep the old
    // memory type with them.
    uint64_t cr4_nopge = cr4 & ~(1ULL << 7);
    if (cr4 != cr4_nopge) __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4_nopge) : "memory");

    // CD=1, NW=0 -- no-fill cache mode.
    __asm__ volatile ("mov %0, %%cr0" :: "r"((cr0 | (1ULL << 30)) & ~(1ULL << 29)) : "memory");
    __asm__ volatile ("wbinvd" ::: "memory");
    flush_tlb();

    uint64_t def = read_msr_local(MSR_IA32_MTRR_DEF);
    write_msr_local(MSR_IA32_MTRR_DEF, def & ~(1ULL << 11));   // E = 0

    for (uint32_t i = 0; i < n; i++) {
        write_msr_local(MSR_IA32_MTRR_BASE + 2 * i, base[i] | MTRR_TYPE_WC);
        write_msr_local(MSR_IA32_MTRR_BASE + 2 * i + 1,
                        ((~(len[i] - 1)) & mask_hi) | (1ULL << 11)); // V = 1
    }

    write_msr_local(MSR_IA32_MTRR_DEF, def | (1ULL << 11));    // E = 1

    __asm__ volatile ("wbinvd" ::: "memory");
    flush_tlb();
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0) : "memory");
    if (cr4 != cr4_nopge) __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");

    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
    return PAGING_WC_MTRR;
}

int paging_set_write_combining(uint64_t phys, uint64_t size) {
    if (size == 0) return PAGING_WC_NONE;
    if (phys + size > (uint64_t)PDE_COUNT * HUGE_SIZE) return PAGING_WC_NONE;

    uint32_t edx = cpuid_edx1();
    int have_pat  = (edx >> 16) & 1u;
    int have_mtrr = (edx >> 12) & 1u;

    if (have_pat && !nopat_requested()) return pat_apply(phys, size);
    if (have_mtrr) return mtrr_apply(phys, size);
    return PAGING_WC_NONE;
}

const char *paging_wc_name(int result) {
    switch (result) {
        case PAGING_WC_PAT:  return "PAT";
        case PAGING_WC_MTRR: return "MTRR";
        default:             return "none (uncached)";
    }
}
