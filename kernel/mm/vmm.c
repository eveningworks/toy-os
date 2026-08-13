#include "vmm.h"
#include "pmm.h"
#include <stddef.h>

// The kernel's own top-level page table, from boot.asm. Every process's
// PML4 shares entry 0 with this one -- see vmm.h for why.
extern uint64_t p4_table[512];

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_NX       (1ULL << 63) // requires EFER.NXE, set once in boot.asm
#define ADDR_MASK     0x000FFFFFFFFFF000ULL

// All of this runs with the kernel's own page tables still active (CR3
// hasn't been switched to the process yet), so every physical address
// here -- the new PML4 itself, and any PDPT/PD/PT frames allocated along
// the way -- is safely dereferenceable directly: it's within the low
// 4GiB the kernel identity-maps for itself.
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
    uint64_t pml4_phys = pmm_alloc_frame();
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
    uint64_t new_phys = pmm_alloc_frame();
    if (!new_phys) return 0;
    zero_table(new_phys);
    // USER must be set at every level of the walk, not just the leaf --
    // see paging.c / the README's "Process isolation" section for the
    // bug that taught us this the hard way.
    table[index] = new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    return new_phys;
}

int vmm_map_user_page_flags(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                             int writable, int executable) {
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

    uint64_t *pt = table_at(pt_phys);
    pt[pt_index] = (paddr & ADDR_MASK) | flags;
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
    return vmm_map_user_page_flags(pml4_phys, vaddr, paddr, 1, 0);
}

void vmm_switch_address_space(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

// The tear-down mirror of ensure_next_level() above: frees every present
// leaf frame in a PT, then the PT frame itself.
static void destroy_pt(uint64_t pt_phys) {
    uint64_t *pt = table_at(pt_phys);
    for (int i = 0; i < 512; i++) {
        if (pt[i] & PAGE_PRESENT) pmm_free_frame(pt[i] & ADDR_MASK);
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
}

uint64_t vmm_current_pml4(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Checks PRESENT + USER at every level for the single 4KiB page
// containing `vaddr`. Doesn't handle a 2MiB huge-page leaf at the PD
// level, since vmm_map_user_page() never creates one for process-private
// mappings -- it always descends to an individual 4KiB PTE.
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

int vmm_validate_user_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t len) {
    if (len == 0) return 1; // nothing to touch -- trivially fine

    uint64_t end = vaddr + len; // last byte is at end-1
    if (end < vaddr) return 0;  // overflow: the range wraps around

    uint64_t page = vaddr & ~0xFFFULL;
    uint64_t last_page = (end - 1) & ~0xFFFULL;

    for (;;) {
        if (!page_is_valid_user(pml4_phys, page)) return 0;
        if (page == last_page) break;
        page += 4096;
    }
    return 1;
}
