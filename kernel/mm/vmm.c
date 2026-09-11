#include "vmm.h"
#include "pmm.h"
#include "swap.h"
#include "string.h" // k_memcpy() -- the user-copy helpers below
#include "kfmt.h"   // klog_printf() -- the fork walk's refusals
#include <stddef.h>

// The kernel's own top-level page table, from boot.asm. Every process's
// PML4 shares entry 0 with this one -- see vmm.h for why.
extern uint64_t p4_table[512];

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)  // a PDPT/PD entry that IS the leaf, not a table pointer
// Bits 9-11 of a PTE are IGNORED by the hardware and reserved for the
// OS. This one records that the mapping does NOT own the frame behind
// it, so vmm_destroy_address_space() must not free it. See
// vmm_map_user_borrowed().
#define PAGE_BORROWED (1ULL << 9)
// The second software bit, and it is only ever set with PAGE_PRESENT
// CLEAR: the page is swapped out and the rest of the entry holds its
// slot, not a frame. See the swap-entry section further down.
#define PAGE_SWAPPED  (1ULL << 10)
// The third: this leaf WAS writable and is shared with another address
// space after a fork; a write fault copies (or, for the last owner,
// restores W). Only ever set with PRESENT set and WRITABLE clear.
#define PAGE_COW      (1ULL << 11)
// #PF error-code bit 1: the access was a write.
#define PF_WRITE      (1ULL << 1)
#define PAGE_NX       (1ULL << 63) // requires EFER.NXE, set once in boot.asm
// Selects PAT slot 4 (paging.c points it at write-combining at boot).
// Bit 7 on a 4KiB PTE -- note that is the same bit PAGE_HUGE uses at the
// levels ABOVE this one, which is why this constant is named for the
// page size it is valid on. Every page this file maps is 4KiB.
#define PAGE_PAT_4K   (1ULL << 7)
#define ADDR_MASK     0x000FFFFFFFFFF000ULL

// All of this runs with the kernel's own page tables still active (CR3
// hasn't been switched to the process yet), so every physical address
// here -- the new PML4 itself, and any PDPT/PD/PT frames allocated along
// the way -- is safely dereferenceable directly: the identity map covers
// every frame pmm manages, above 4 GiB included (paging.h).
static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;
}

static void zero_table(uint64_t phys) {
    uint64_t *t = table_at(phys);
    for (int i = 0; i < 512; i++) t[i] = 0;
}

uint64_t vmm_kernel_pml4_phys(void) {
    return (uint64_t)(uintptr_t)p4_table;
}

uint64_t vmm_create_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!pml4_phys) return 0;
    zero_table(pml4_phys);

    uint64_t *pml4 = table_at(pml4_phys);
    pml4[0] = p4_table[0]; // share the kernel's identity-mapped low 4GiB

    return pml4_phys;
}

// Returns the physical address of the next-level table at `table[index]`,
// allocating and zeroing a fresh one first if it isn't present yet.
static uint64_t ensure_next_level(uint64_t *table, int index) {
    if (table[index] & PAGE_PRESENT) {
        return table[index] & ADDR_MASK;
    }
    uint64_t new_phys = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!new_phys) return 0;
    zero_table(new_phys);
    // USER must be set at every level of the walk, not just the leaf --
    // see paging.c / the README's "Process isolation" section for the
    // bug that taught us this the hard way.
    table[index] = new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    return new_phys;
}

// --- per-address-space user page accounting --------------------------
//
// How much memory a process is using, counted where mapping actually
// happens rather than guessed from anywhere else. Task Manager's memory
// column is this (see abi/proc_info.h); nothing in the kernel could
// answer the question before.
//
// Keyed by PML4 rather than by pid because that is what this layer
// has: vmm takes an address space, never a process, and deliberately so
// (see vmm.h -- the address space is passed explicitly so a mapping
// never depends on which process happens to be current).
//
// A small linear table. One entry per live address space, so it is
// bounded by the process table; a linear scan of that is nothing beside
// the page-table walk it accompanies.
#define VMM_ACCT_MAX 72

// `pages` is RESIDENT -- what is mapped right now. `swapped` is what
// this address space still owns but is not holding a frame for. They
// are two numbers rather than one because eviction moves a page from
// the first to the second, and a reader shown only the first watches a
// process appear to shrink while it is doing nothing of the kind.
static struct { uint64_t pml4; uint32_t pages; uint32_t swapped; } g_acct[VMM_ACCT_MAX];

static int acct_slot(uint64_t pml4_phys, int create) {
    int free_slot = -1;
    for (int i = 0; i < VMM_ACCT_MAX; i++) {
        if (g_acct[i].pml4 == pml4_phys && pml4_phys) return i;
        if (!g_acct[i].pml4 && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    g_acct[free_slot].pml4 = pml4_phys;
    g_acct[free_slot].pages = 0;
    g_acct[free_slot].swapped = 0;
    return free_slot;
}

// Defined with the other swap-entry code below; declared here because
// unmap, release and teardown all sit above it and all three have to
// give a slot back.
static int drop_swap_entry(uint64_t *pt, int i);

uint64_t vmm_user_bytes(uint64_t pml4_phys) {
    int i = acct_slot(pml4_phys, 0);
    return i < 0 ? 0 : (uint64_t)g_acct[i].pages * 4096;
}

uint64_t vmm_user_swapped_bytes(uint64_t pml4_phys) {
    int i = acct_slot(pml4_phys, 0);
    return i < 0 ? 0 : (uint64_t)g_acct[i].swapped * 4096;
}

static int map_user(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                     int writable, int executable, int memtype, int borrowed) {
    int pml4_index = (int)((vaddr >> 39) & 0x1FF);
    int pdpt_index = (int)((vaddr >> 30) & 0x1FF);
    int pd_index   = (int)((vaddr >> 21) & 0x1FF);
    int pt_index   = (int)((vaddr >> 12) & 0x1FF);

    uint64_t pdpt_phys = ensure_next_level(table_at(pml4_phys), pml4_index);
    if (!pdpt_phys) return 0;

    uint64_t pd_phys = ensure_next_level(table_at(pdpt_phys), pdpt_index);
    if (!pd_phys) return 0;

    uint64_t pt_phys = ensure_next_level(table_at(pd_phys), pd_index);
    if (!pt_phys) return 0;

    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (writable) flags |= PAGE_WRITABLE;
    if (!executable) flags |= PAGE_NX;
    // PAT slot 4 (PAT=1, PCD=0, PWT=0) is pointed at write-combining by
    // paging.c at boot. On a 4KiB page the PAT bit is bit 7 -- bit 12,
    // which selects it on a 2MiB page, is part of the PHYSICAL ADDRESS
    // here and setting it would silently repoint the mapping rather
    // than fault. Every page this function maps is 4KiB.
    if (memtype == VMM_MT_WC) flags |= PAGE_PAT_4K;
    // Ownership, recorded in the PTE itself rather than in a side table:
    // the teardown walk has the PTE in hand and nothing else, and a side
    // table would have to be kept in step with every map and unmap.
    if (borrowed) flags |= PAGE_BORROWED;

    uint64_t *pt = table_at(pt_phys);
    // Only count a page that was not already mapped here. A remap of the
    // same address (win_server.c does exactly that on a window resize)
    // replaces one frame with another and must not count twice.
    int was_present = (pt[pt_index] & PAGE_PRESENT) != 0;
    pt[pt_index] = (paddr & ADDR_MASK) | flags;
    if (!was_present) {
        int i = acct_slot(pml4_phys, 1);
        if (i >= 0) g_acct[i].pages++;
    }
    return 1;
}

// The plain, no-questions-asked mapper -- writable, and deliberately
// NOT executable. Every call site that predates the NX/W^X work (a
// process's stack, SYS_SBRK heap growth, the GUI framebuffer, a
// window's pixel buffer -- see syscall.c/elf_run.c/scheduler.c) is
// data, never code, so this default is both the secure one and the
// correct one for all of them with no caller-side changes needed.
// elf.c's ELF loader is the one caller that DOES need per-segment
// control (a .text segment has to be executable) -- it calls
// vmm_map_user_page_flags() directly instead of this wrapper.
int vmm_map_user_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr) {
    return map_user(pml4_phys, vaddr, paddr, 1, 0, VMM_MT_NORMAL, 0);
}

// The permissions-only form, which is what every caller but the
// framebuffer grant wants: ordinary cacheable memory.
int vmm_map_user_page_flags(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                             int writable, int executable) {
    return map_user(pml4_phys, vaddr, paddr, writable, executable,
                     VMM_MT_NORMAL, 0);
}

int vmm_map_user_page_type(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                            int writable, int executable, int memtype) {
    return map_user(pml4_phys, vaddr, paddr, writable, executable, memtype, 0);
}

// The BORROWED form: this address space gets to see the frame, and does
// not own it. Its PTE carries PAGE_BORROWED, so destroying the address
// space unmaps it and leaves the frame alone.
//
// Every caller is a mapping of memory that belongs to somebody else --
// the kernel's own glyph tables (one instance, read-only, in every GUI
// client), the real framebuffer, a window buffer the window server
// allocated, another process's buffer shared with the compositor, or the
// one shared zero page a revoked slot is poisoned with. Freeing any of
// those on exit hands live memory back to the allocator; for the font
// that is a page of the kernel image.
int vmm_map_user_borrowed(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                           int writable, int executable, int memtype) {
    return map_user(pml4_phys, vaddr, paddr, writable, executable, memtype, 1);
}

// The inverse of vmm_map_user_page(): clears one page's PTE so the
// address stops resolving.
//
// It deliberately does NOT free the frame that was mapped, or any of
// the page tables above it. The caller owns the frame (it allocated it
// and knows how -- pmm_free_frame() vs pmm_free_contiguous() are not
// interchangeable), and the intermediate tables belong to the address
// space, which frees them wholesale in vmm_destroy_address_space().
// Freeing either from here would be this function guessing.
//
// This exists because a mapping that outlives what it points at is a
// use-after-free the CPU will happily service: a client window's buffer
// is freed when the window is destroyed, and without this the client
// would keep a writable mapping onto frames that had been handed to
// somebody else (see kernel/proc/win_server.c).
//
// Returns 1 if a mapping was removed, 0 if nothing was mapped there --
// which is not an error, just nothing to do.
int vmm_unmap_user_page(uint64_t pml4_phys, uint64_t vaddr) {
    int pml4_index = (int)((vaddr >> 39) & 0x1FF);
    int pdpt_index = (int)((vaddr >> 30) & 0x1FF);
    int pd_index   = (int)((vaddr >> 21) & 0x1FF);
    int pt_index   = (int)((vaddr >> 12) & 0x1FF);

    // Walk without creating anything -- an absent level just means the
    // address was never mapped.
    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[pml4_index] & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = table_at(pml4[pml4_index] & ADDR_MASK);
    if (!(pdpt[pdpt_index] & PAGE_PRESENT)) return 0;
    uint64_t *pd = table_at(pdpt[pdpt_index] & ADDR_MASK);
    if (!(pd[pd_index] & PAGE_PRESENT)) return 0;
    uint64_t *pt = table_at(pd[pd_index] & ADDR_MASK);
    if (!(pt[pt_index] & PAGE_PRESENT)) {
        // Not present is usually "nothing was mapped here" -- except
        // for a swapped page, whose storage is a slot rather than a
        // frame and which nobody else can give back.
        if (drop_swap_entry(pt, pt_index)) {
            int i = acct_slot(pml4_phys, 0);
            if (i >= 0 && g_acct[i].swapped) g_acct[i].swapped--;
            return 1;
        }
        return 0;
    }

    pt[pt_index] = 0;
    {
        int i = acct_slot(pml4_phys, 0);
        if (i >= 0 && g_acct[i].pages) g_acct[i].pages--;
    }

    // Only worth an INVLPG if this address space is the live one. For
    // any other, the stale TLB entry cannot be reached without a CR3
    // load first, and writing CR3 flushes non-global entries anyway.
    if (vmm_current_pml4() == pml4_phys) {
        __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
    return 1;
}

// vmm_unmap_user_page() plus the disposal the caller usually wants:
// the frame goes back to pmm unless the PTE says PAGE_BORROWED, the
// same rule the teardown walk applies. Exists for SYS_MUNMAP, where
// "clear the mapping" and "free the frame" done as two calls would
// need the caller to re-derive the borrowed bit this walk already has
// in hand. Returns 1 if a mapping was removed, 0 if nothing was there.
int vmm_release_user_page(uint64_t pml4_phys, uint64_t vaddr) {
    int pml4_index = (int)((vaddr >> 39) & 0x1FF);
    int pdpt_index = (int)((vaddr >> 30) & 0x1FF);
    int pd_index   = (int)((vaddr >> 21) & 0x1FF);
    int pt_index   = (int)((vaddr >> 12) & 0x1FF);

    uint64_t *pml4 = table_at(pml4_phys);
    if (!(pml4[pml4_index] & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = table_at(pml4[pml4_index] & ADDR_MASK);
    if (!(pdpt[pdpt_index] & PAGE_PRESENT)) return 0;
    uint64_t *pd = table_at(pdpt[pdpt_index] & ADDR_MASK);
    if (!(pd[pd_index] & PAGE_PRESENT)) return 0;
    uint64_t *pt = table_at(pd[pd_index] & ADDR_MASK);
    uint64_t pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) {
        if (drop_swap_entry(pt, pt_index)) {
            int i = acct_slot(pml4_phys, 0);
            if (i >= 0 && g_acct[i].swapped) g_acct[i].swapped--;
            return 1;
        }
        return 0;
    }

    pt[pt_index] = 0;
    {
        int i = acct_slot(pml4_phys, 0);
        if (i >= 0 && g_acct[i].pages) g_acct[i].pages--;
    }
    if (vmm_current_pml4() == pml4_phys) {
        __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
    if (!(pte & PAGE_BORROWED)) pmm_free_frame(pte & ADDR_MASK);
    return 1;
}

// ---- swapped-out pages ----------------------------------------------
//
// A swapped page's PTE has PRESENT clear, so the hardware faults on the
// next touch, PAGE_SWAPPED set so a walker can tell it from an address
// that was never mapped, and the SLOT where the frame's address used to
// be. Linux's swp_entry_t, and it costs no memory anywhere: the record
// of where the page went IS the entry that used to point at it.
//
// Slot 0 is the swap header and is never handed out (swap.h), so a
// zeroed PTE cannot read as "swapped to slot 0".
static uint64_t swap_pte(uint32_t slot) {
    return ((uint64_t)slot << 12) | PAGE_SWAPPED;
}

// The page table holding `vaddr`, or NULL. Walks without creating: an
// absent level means the address was never mapped. USER is required at
// every level and a huge leaf is refused, because this is asked about
// ANY address a syscall was handed -- a low one lands in the kernel's
// identity map, whose PD entries are 2 MiB leaves, and reading one as
// a table would hand back RAM to be edited as PTEs.
static uint64_t *pt_for(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t e = table_at(pml4_phys)[(vaddr >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;
    e = table_at(e & ADDR_MASK)[(vaddr >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER) || (e & PAGE_HUGE)) return 0;
    e = table_at(e & ADDR_MASK)[(vaddr >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER) || (e & PAGE_HUGE)) return 0;
    return table_at(e & ADDR_MASK);
}

// THIS FUNCTION IS WHERE "WHAT MAY BE EVICTED" IS DECIDED, and it is
// the only place that should decide it. A candidate is PRESENT, OWNED
// (not PAGE_BORROWED -- somebody else's frame, and every shared or DMA
// mapping in the system is borrowed for exactly that reason) and
// MANAGED (pmm accounts for it; the raw framebuffer is mapped owned but
// unmanaged, so the bit alone is not enough). Anything else is refused.
//
// The frame is freed here, so the caller must have written it to `slot`
// FIRST -- there is no way back afterwards.
int vmm_set_swap_entry(uint64_t pml4_phys, uint64_t vaddr, uint32_t slot) {
    if (!slot) return 0;
    uint64_t *pt = pt_for(pml4_phys, vaddr);
    if (!pt) return 0;
    int i = (int)((vaddr >> 12) & 0x1FF);
    uint64_t pte = pt[i];
    if (!(pte & PAGE_PRESENT)) return 0;
    if (pte & PAGE_BORROWED) return 0;
    uint64_t frame = pte & ADDR_MASK;
    if (!pmm_frame_is_managed(frame)) return 0;
    // A frame two address spaces share after a fork: the other owner's
    // PTE would keep pointing at a frame this call frees, and there is
    // no reverse map to find it by. Not a candidate (docs/fork-design.md).
    if (pmm_frame_refs(frame) > 1) return 0;

    pt[i] = swap_pte(slot);
    if (vmm_current_pml4() == pml4_phys) {
        __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
    }
    pmm_free_frame(frame);
    int a = acct_slot(pml4_phys, 0);
    if (a >= 0) {
        if (g_acct[a].pages) g_acct[a].pages--;
        g_acct[a].swapped++;
    }
    return 1;
}

// The slot `vaddr` is swapped to, or 0 if it is not swapped.
uint32_t vmm_swap_entry(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pt = pt_for(pml4_phys, vaddr);
    if (!pt) return 0;
    uint64_t pte = pt[(vaddr >> 12) & 0x1FF];
    if (pte & PAGE_PRESENT) return 0;
    if (!(pte & PAGE_SWAPPED)) return 0;
    return (uint32_t)(pte >> 12);
}

// Drop a swap entry and the slot behind it. Shared by unmap, release
// and teardown, because "this address no longer refers to anything" has
// to give the slot back in all three -- the process cannot, it does not
// know the number, and nothing audits slot usage to catch the leak.
static int drop_swap_entry(uint64_t *pt, int i) {
    uint64_t pte = pt[i];
    if ((pte & PAGE_PRESENT) || !(pte & PAGE_SWAPPED)) return 0;
    swap_slot_free((uint32_t)(pte >> 12));
    pt[i] = 0;
    return 1;
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

// The tear-down mirror of ensure_next_level() above: frees every present
// leaf frame in a PT, then the PT frame itself.
static void destroy_pt(uint64_t pt_phys) {
    uint64_t *pt = table_at(pt_phys);
    for (int i = 0; i < 512; i++) {
        // A BORROWED mapping's frame belongs to somebody still using it
        // -- the kernel image, the framebuffer, the window server, or
        // another process. Unmapping it is this walk's business; freeing
        // it is not. Without this an exiting GUI client returned pages
        // of kernel .rodata (the glyph tables it had mapped read-only)
        // to the physical allocator, which then handed them out again.
        // A swapped page holds no frame, but it does hold a SLOT, and
        // a process that dies swapped would otherwise leak it with no
        // detector -- nothing audits slot usage the way meminfo audits
        // frames.
        if (!(pt[i] & PAGE_PRESENT)) { drop_swap_entry(pt, i); continue; }
        if (pt[i] & PAGE_BORROWED) continue;
        pmm_free_frame(pt[i] & ADDR_MASK);
    }
    pmm_free_frame(pt_phys);
}

static void destroy_pd(uint64_t pd_phys) {
    uint64_t *pd = table_at(pd_phys);
    for (int i = 0; i < 512; i++) {
        if (pd[i] & PAGE_PRESENT) destroy_pt(pd[i] & ADDR_MASK);
    }
    pmm_free_frame(pd_phys);
}

static void destroy_pdpt(uint64_t pdpt_phys) {
    uint64_t *pdpt = table_at(pdpt_phys);
    for (int i = 0; i < 512; i++) {
        if (pdpt[i] & PAGE_PRESENT) destroy_pd(pdpt[i] & ADDR_MASK);
    }
    pmm_free_frame(pdpt_phys);
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = table_at(pml4_phys);
    // Start at 1, not 0 -- entry 0 is the shared kernel mapping every
    // address space's PML4 points at the SAME physical PDPT for (see
    // vmm_create_address_space()); walking into it here would free
    // memory every other process (and the kernel itself) still needs.
    for (int i = 1; i < 512; i++) {
        if (pml4[i] & PAGE_PRESENT) destroy_pdpt(pml4[i] & ADDR_MASK);
    }
    pmm_free_frame(pml4_phys);

    // Release the accounting slot. Load-bearing rather than tidiness:
    // pmm hands the same physical frame out again, so a later address
    // space can be born at this exact PML4 address and would otherwise
    // inherit this one's page count.
    {
        int i = acct_slot(pml4_phys, 0);
        if (i >= 0) { g_acct[i].pml4 = 0; g_acct[i].pages = 0; g_acct[i].swapped = 0; }
    }
}

// --- fork: copy-on-write sharing ---------------------------------------

static void flush_if_live(uint64_t pml4_phys, uint64_t vaddr) {
    if (vmm_current_pml4() == pml4_phys)
        __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

static int frame_pinned(const struct vmm_fork_opts *o, uint64_t frame) {
    for (int i = 0; i < o->npinned; i++)
        if ((o->pinned[i] & ADDR_MASK) == frame) return 1;
    return 0;
}

// Writes one leaf into the child, building the tables above it.
static int child_set_leaf(uint64_t child, uint64_t va, uint64_t pte) {
    uint64_t pdpt = ensure_next_level(table_at(child), (int)((va >> 39) & 0x1FF));
    if (!pdpt) return 0;
    uint64_t pd = ensure_next_level(table_at(pdpt), (int)((va >> 30) & 0x1FF));
    if (!pd) return 0;
    uint64_t pt = ensure_next_level(table_at(pd), (int)((va >> 21) & 0x1FF));
    if (!pt) return 0;
    table_at(pt)[(va >> 12) & 0x1FF] = pte;
    int a = acct_slot(child, 1);
    if (a >= 0) g_acct[a].pages++;
    return 1;
}

// One present, OWNED leaf of the parent, into the child. The refcount is
// raised BEFORE the child's leaf exists, so a failure part way leaves
// the frame with one owner too many rather than one too few -- the
// child's teardown puts the count back; the other order would have it
// free a frame the parent still maps.
static int fork_leaf(uint64_t parent, uint64_t child, uint64_t *ppte, uint64_t va,
                     int live, const struct vmm_fork_opts *o) {
    uint64_t pte = *ppte;
    uint64_t frame = pte & ADDR_MASK;

    if ((pte & PAGE_WRITABLE) && frame_pinned(o, frame)) {
        uint64_t nf = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!nf) return 0;
        k_memcpy(table_at(nf), table_at(frame), 4096);
        if (!child_set_leaf(child, va, (pte & ~ADDR_MASK) | nf)) {
            pmm_free_frame(nf);
            return 0;
        }
        return 1;
    }

    uint64_t npte = pte;
    if (pte & PAGE_WRITABLE) npte = (pte & ~PAGE_WRITABLE) | PAGE_COW;
    pmm_frame_ref(frame);
    if (!child_set_leaf(child, va, npte)) {
        pmm_free_frame(frame);
        return 0;
    }
    if (npte != pte) {
        *ppte = npte;
        if (live) flush_if_live(parent, va);
    }
    return 1;
}

uint64_t vmm_fork_address_space(uint64_t parent, const struct vmm_fork_opts *o) {
    static const struct vmm_fork_opts none = {0};
    if (!o) o = &none;
    uint64_t child = vmm_create_address_space();
    if (!child) return 0;
    int live = vmm_current_pml4() == parent;
    const char *why = 0;

    uint64_t *pml4 = table_at(parent);
    for (int i = 1; i < 512 && !why; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t *pdpt = table_at(pml4[i] & ADDR_MASK);
        for (int j = 0; j < 512 && !why; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            if (pdpt[j] & PAGE_HUGE) { why = "a 1 GiB leaf"; break; }
            uint64_t *pd = table_at(pdpt[j] & ADDR_MASK);
            for (int k = 0; k < 512 && !why; k++) {
                if (!(pd[k] & PAGE_PRESENT)) continue;
                if (pd[k] & PAGE_HUGE) { why = "a 2 MiB leaf"; break; }
                uint64_t *pt = table_at(pd[k] & ADDR_MASK);
                uint64_t base = ((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21);
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = pt[l];
                    uint64_t va = base | ((uint64_t)l << 12);
                    if (!(pte & PAGE_PRESENT)) {
                        if (pte & PAGE_SWAPPED) { why = "a swapped page"; break; }
                        continue;
                    }
                    if (pte & PAGE_BORROWED) {
                        // Somebody else's frame: repeated only where the
                        // caller vouches for it, and never refcounted.
                        if (o->inherit_borrowed && o->inherit_borrowed(o->ctx, va) &&
                            !child_set_leaf(child, va, pte)) { why = "no memory"; break; }
                        continue;
                    }
                    if (!pmm_frame_is_managed(pte & ADDR_MASK)) continue; // a device grant
                    if (!fork_leaf(parent, child, &pt[l], va, live, o)) { why = "no memory"; break; }
                }
            }
        }
    }
    if (why) {
        klog_printf("vmm: fork of %#lx refused -- %s\n", parent, why);
        vmm_destroy_address_space(child);
        return 0;
    }
    return child;
}

int vmm_cow_break(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pt = pt_for(pml4_phys, vaddr);
    if (!pt) return 0;
    int i = (int)((vaddr >> 12) & 0x1FF);
    uint64_t pte = pt[i];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_COW)) return 0;
    uint64_t frame = pte & ADDR_MASK;

    if (pmm_frame_refs(frame) <= 1) {
        // The other owner has gone: this page is private again.
        pt[i] = (pte & ~PAGE_COW) | PAGE_WRITABLE;
        flush_if_live(pml4_phys, vaddr & ~0xFFFULL);
        return 1;
    }
    uint64_t nf = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!nf) {
        klog_printf("mm: no frame to un-share %#lx -- the process dies here\n", vaddr);
        return 0;
    }
    k_memcpy(table_at(nf), table_at(frame), 4096);
    pt[i] = (pte & ~(PAGE_COW | ADDR_MASK)) | nf | PAGE_WRITABLE;
    flush_if_live(pml4_phys, vaddr & ~0xFFFULL);
    pmm_free_frame(frame); // one owner fewer
    return 1;
}

// Before the kernel WRITES into a user page: the page must be private.
// Returns 0 only when it is COW and could not be un-shared.
static int unshare_for_write(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t *pt = pt_for(pml4_phys, vaddr);
    if (!pt) return 1;
    uint64_t pte = pt[(vaddr >> 12) & 0x1FF];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_COW)) return 1;
    return vmm_cow_break(pml4_phys, vaddr);
}

// --- auditing page tables against the allocator ----------------------
//
// The invariant: every frame a live mapping points at must be one the
// physical allocator considers HANDED OUT. Nothing checked it before,
// and the two bugs that prompted this both broke it in the direction
// that is silent -- a mapping left pointing at a frame pmm had put back
// in its free list, which costs nothing at all until pmm hands that
// frame to somebody else and they write to it.
//
// This audits the direction that is CHEAP and DANGEROUS. The reverse --
// a frame marked used that nothing references, i.e. a leak -- is not
// symmetric: page tables, the kernel heap, the kernel image and any DMA
// buffer all hold frames no page table references, so a sweep would
// report every one of them. Answering that needs each owner to declare
// its frames, which is a much larger job; see docs/roadmap.md.
//
// Borrowed pages are audited too, and deliberately: a borrowed mapping
// whose owner has already freed the frame is exactly the use-after-free
// PAGE_BORROWED exists to make possible to reason about, so leaving it
// unchecked would audit away the interesting half.
// The walk carries a callback as well as the counters, because a caller
// that wants EVERY dangling mapping cannot get them from the summary --
// `first_bad_va` records one. `/bin/meminfo --audit` reports them as a
// list, one record per finding, so it needs each. A callback rather than
// an array in `struct vmm_audit` for the reason geom.h draws through
// one: the walk should not have to guess how many the caller can hold.
struct audit_ctx {
    struct vmm_audit *a;
    vmm_dangling_cb   cb;   // may be NULL
    void             *ctx;
};

static void audit_pt(uint64_t pt_phys, uint64_t base_va, struct audit_ctx *c) {
    struct vmm_audit *a = c->a;
    uint64_t *pt = table_at(pt_phys);
    for (int i = 0; i < 512; i++) {
        if (!(pt[i] & PAGE_PRESENT)) {
            // A SWAPPED PAGE IS COUNTED, NOT SKIPPED. This walk's
            // invariant is about frames, and a swapped page has none --
            // so it cannot be checked. Saying how many were not checked
            // is the difference between a bounded answer and a silence
            // that reads like a clean bill of health.
            if (pt[i] & PAGE_SWAPPED) a->swapped++;
            continue;
        }
        uint64_t frame = pt[i] & ADDR_MASK;
        uint64_t va = base_va + (uint64_t)i * 4096;
        a->pages++;
        if (pt[i] & PAGE_BORROWED) a->borrowed++;
        if (pt[i] & PAGE_COW) a->cow++;
        if (!pmm_frame_is_managed(frame)) {
            // MMIO, or memory the firmware never reported as RAM -- a
            // framebuffer is the usual one. Not pmm's to account for,
            // so there is nothing to compare against.
            a->unmanaged++;
        } else if (!pmm_frame_is_used(frame)) {
            if (!a->dangling) { a->first_bad_va = va; a->first_bad_frame = frame; }
            a->dangling++;
            if (c->cb) c->cb(va, frame, c->ctx);
        }
    }
}

static void audit_pd(uint64_t pd_phys, uint64_t base_va, struct audit_ctx *c) {
    uint64_t *pd = table_at(pd_phys);
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        // A huge page is a leaf here, not a table pointer -- walking
        // into one would read pixel data as page-table entries.
        if (pd[i] & PAGE_HUGE) { c->a->huge++; continue; }
        audit_pt(pd[i] & ADDR_MASK, base_va + (uint64_t)i * 0x200000, c);
    }
}

uint64_t vmm_audit_space(uint64_t pml4_phys, struct vmm_audit *out) {
    return vmm_audit_space_cb(pml4_phys, out, 0, 0);
}

uint64_t vmm_audit_space_cb(uint64_t pml4_phys, struct vmm_audit *out,
                            vmm_dangling_cb cb, void *cbctx) {
    struct vmm_audit zero = {0};
    if (!out) out = &zero;
    *out = zero;
    if (!pml4_phys) return 0;
    struct audit_ctx c = { out, cb, cbctx };

    uint64_t *pml4 = table_at(pml4_phys);
    // From 1, not 0: entry 0 is the shared kernel mapping every address
    // space points at, and it maps the kernel's own frames, the heap and
    // the page tables -- none of which this audit's invariant covers.
    // Same reason vmm_destroy_address_space() starts there.
    for (int i = 1; i < 512; i++) {
        if (!(pml4[i] & PAGE_PRESENT)) continue;
        uint64_t *pdpt = table_at(pml4[i] & ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            if (pdpt[j] & PAGE_HUGE) { out->huge++; continue; }
            uint64_t base = ((uint64_t)i << 39) | ((uint64_t)j << 30);
            audit_pd(pdpt[j] & ADDR_MASK, base, &c);
        }
    }
    return out->dangling;
}

uint64_t vmm_current_pml4(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// The physical address `vaddr` maps to, or 0 if it isn't a present,
// user-accessible mapping at every level of the walk.
//
// 0 doubles as the failure value, which is sound here for the same
// reason it is throughout pmm.c: frame 0 is the real-mode IVT and is
// never handed out. Doesn't handle a 2MiB huge-page leaf at the PD
// level, since vmm_map_user_page() never creates one for
// process-private mappings -- it always descends to an individual 4KiB
// PTE -- and a huge PDE reaching here would be read as a pointer to a
// page table, so it is refused explicitly rather than mis-walked.
static uint64_t user_phys_of_walk(uint64_t pml4_phys, uint64_t vaddr) {
    int pml4_index = (int)((vaddr >> 39) & 0x1FF);
    int pdpt_index = (int)((vaddr >> 30) & 0x1FF);
    int pd_index   = (int)((vaddr >> 21) & 0x1FF);
    int pt_index   = (int)((vaddr >> 12) & 0x1FF);

    uint64_t e = table_at(pml4_phys)[pml4_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    e = table_at(e & ADDR_MASK)[pdpt_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;
    if (e & PAGE_HUGE) return 0; // a 1GiB leaf -- see above

    e = table_at(e & ADDR_MASK)[pd_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;
    if (e & PAGE_HUGE) return 0; // a 2MiB leaf -- see above

    e = table_at(e & ADDR_MASK)[pt_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    return (e & ADDR_MASK) | (vaddr & 0xFFF);
}

uint64_t vmm_user_phys(uint64_t pml4_phys, uint64_t vaddr) {
    return user_phys_of_walk(pml4_phys, vaddr);
}

// The walk every user access goes through, with ONE retry through the
// fault-in hook.
//
// This is where demand paging meets the copy helpers, and it is the
// half that is easy to forget: the kernel never dereferences a user
// address, so a page the process is entitled to but has not touched yet
// produces no #PF here -- it produces a walk that finds nothing, and
// without this the syscall would report a perfectly legal buffer as a
// bad pointer. Asked once: a handler that claims to have mapped a page
// and leaves the walk failing is a bug in the handler, and looping
// would turn it into a hang.
static uint64_t user_phys_of(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t phys = user_phys_of_walk(pml4_phys, vaddr);
    if (phys) return phys;
    if (!vmm_fault_in(pml4_phys, vaddr & ~0xFFFULL, 0)) return 0;
    return user_phys_of_walk(pml4_phys, vaddr);
}

// Copies between a kernel buffer and user memory THROUGH THE KERNEL'S
// OWN IDENTITY MAP, never by dereferencing the user virtual address.
//
// **That is what makes SMAP absolute here rather than something the
// kernel keeps switching off.** SMAP faults a supervisor access whose
// mapping has U=1 unless EFLAGS.AC is set, and the usual answer is to
// bracket every such access in STAC/CLAC -- which means the protection
// is off for exactly the window where a bug would use it. Walking to
// the frame and copying through the kernel's identity mapping (U=0
// throughout, see boot.asm and paging_extend_identity_map()) is a
// supervisor access to a supervisor
// page, so SMAP never applies and AC is never touched at all. Nothing
// in this kernel may reach into a user pointer any other way once CR4
// SMAP is on.
//
// It also closes a TOCTOU gap the old shape had: validating a range and
// then dereferencing it separately leaves room for the mapping to
// change in between. Here the walk and the copy are the same operation,
// per page.
//
// `dir` is which way the bytes move; both directions share the walk,
// the page splitting and the failure semantics. Returns 1 only if EVERY
// byte was copied -- a partial copy is reported as failure, with
// whatever was already written left in place, so a caller must treat 0
// as "the destination holds nothing you can trust" rather than trying
// to salvage a prefix.
enum copy_dir { COPY_FROM_USER, COPY_TO_USER };

static int copy_user(uint64_t pml4_phys, uint64_t uaddr, void *kbuf,
                      uint64_t len, enum copy_dir dir) {
    if (len == 0) return 1;
    if (uaddr + len < uaddr) return 0; // the range wraps

    uint8_t *k = (uint8_t *)kbuf;
    while (len) {
        // A write must land in a page this address space owns alone --
        // the identity-map copy below never faults, so the un-share the
        // #PF path would do has to happen here (docs/fork-design.md).
        if (dir == COPY_TO_USER && !unshare_for_write(pml4_phys, uaddr)) return 0;
        uint64_t phys = user_phys_of(pml4_phys, uaddr);
        if (!phys) return 0;

        // Never copy past the end of the page just walked -- the next
        // one is a separate mapping and may not exist at all.
        uint64_t in_page = 4096 - (uaddr & 0xFFF);
        uint64_t n = len < in_page ? len : in_page;

        uint8_t *u = (uint8_t *)(uintptr_t)phys;
        if (dir == COPY_FROM_USER) k_memcpy(k, u, (size_t)n);
        else                        k_memcpy(u, k, (size_t)n);

        uaddr += n;
        k += n;
        len -= n;
    }
    return 1;
}

int vmm_copy_from_user(uint64_t pml4_phys, void *dst, uint64_t uaddr, uint64_t len) {
    return copy_user(pml4_phys, uaddr, dst, len, COPY_FROM_USER);
}

int vmm_copy_to_user(uint64_t pml4_phys, uint64_t uaddr, const void *src, uint64_t len) {
    // The cast drops const, which copy_user() then honours by direction
    // rather than by type -- one walk/split implementation is worth more
    // than the constness it costs at this one line.
    return copy_user(pml4_phys, uaddr, (void *)(uintptr_t)src, len, COPY_TO_USER);
}

int vmm_copy_string_from_user(uint64_t pml4_phys, char *dst, uint64_t uaddr, uint64_t max) {
    if (max == 0) return 0;

    // Byte at a time rather than a bulk copy, because the length is not
    // known until the NUL is found: a caller's buffer may legitimately
    // sit near the end of its last mapped page, and reading `max` bytes
    // to look for a terminator would refuse a perfectly valid short
    // string. (The old shape validated a full FS_PATH_MAX range for
    // exactly this reason and had to accept that refusal.)
    for (uint64_t i = 0; i < max - 1; i++) {
        uint64_t phys = user_phys_of(pml4_phys, uaddr + i);
        if (!phys) return 0;
        char c = *(char *)(uintptr_t)phys;
        dst[i] = c;
        if (c == '\0') return 1;
    }
    dst[max - 1] = '\0';
    return 1; // truncated at max-1, NUL-terminated -- callers cap paths anyway
}

// Checks PRESENT + USER at every level for the single 4KiB page
// containing `vaddr`. See user_phys_of() above, which this now wraps --
// the two used to be one function that discarded the physical address
// it had just walked to.
static int page_is_valid_user(uint64_t pml4_phys, uint64_t vaddr) {
    int pml4_index = (int)((vaddr >> 39) & 0x1FF);
    int pdpt_index = (int)((vaddr >> 30) & 0x1FF);
    int pd_index   = (int)((vaddr >> 21) & 0x1FF);
    int pt_index   = (int)((vaddr >> 12) & 0x1FF);

    uint64_t e = table_at(pml4_phys)[pml4_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    e = table_at(e & ADDR_MASK)[pdpt_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    e = table_at(e & ADDR_MASK)[pd_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    e = table_at(e & ADDR_MASK)[pt_index];
    if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) return 0;

    return 1;
}

// --- demand paging: the fault-in hook ---------------------------------
//
// Registered by the process layer, which owns the break. See vmm.h for
// why the copy helpers need this and not only the #PF handler.
static vmm_fault_fn g_fault_fn = 0;

void vmm_set_fault_handler(vmm_fault_fn fn) { g_fault_fn = fn; }

int vmm_fault_in(uint64_t pml4_phys, uint64_t vaddr, uint64_t err) {
    uint64_t *pt = pt_for(pml4_phys, vaddr);
    if (pt && (pt[(vaddr >> 12) & 0x1FF] & PAGE_PRESENT)) {
        // Present already: a protection fault, not a missing page. The
        // one kind this layer answers is a write to a COW page.
        if ((err & PF_WRITE) && (pt[(vaddr >> 12) & 0x1FF] & PAGE_COW))
            return vmm_cow_break(pml4_phys, vaddr);
        return 0;
    }
    if (!g_fault_fn) return 0;
    return g_fault_fn(pml4_phys, vaddr);
}

int vmm_validate_user_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t len) {
    if (len == 0) return 1; // nothing to touch -- trivially fine

    uint64_t end = vaddr + len; // last byte is at end-1
    if (end < vaddr) return 0;  // overflow: the range wraps around

    uint64_t page = vaddr & ~0xFFFULL;
    uint64_t last_page = (end - 1) & ~0xFFFULL;

    for (;;) {
        // A miss is not a refusal until the fault-in hook has had a
        // look: with a lazy heap the page may be perfectly legal and
        // simply absent. Asked ONCE per page -- a handler that mapped
        // something and still leaves the walk failing is a bug in the
        // handler, and retrying would spin.
        if (!page_is_valid_user(pml4_phys, page)) {
            if (!vmm_fault_in(pml4_phys, page, 0)) return 0;
            if (!page_is_valid_user(pml4_phys, page)) return 0;
        }
        if (page == last_page) break;
        page += 4096;
    }
    return 1;
}
