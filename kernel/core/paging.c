#include "paging.h"
#include <stddef.h>

// boot.asm identity-maps the first 4GiB with 2MiB pages, laid out as one
// flat 2048-entry PDE array (four 1GiB P2 tables back to back). Exported
// via `global p2_tables` there.
extern uint64_t p2_tables[2048];

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)

// One freshly-split 4KB page table per 2MB slot we've touched. This
// milestone only needs to split a couple of slots (for a small user-test
// region), so a small fixed pool is enough -- this is not a general
// physical-memory allocator.
#define MAX_SPLIT_TABLES 4
static uint64_t split_tables[MAX_SPLIT_TABLES][512] __attribute__((aligned(4096)));
static int split_slot_pde[MAX_SPLIT_TABLES];
static int split_slot_count = 0;

static void flush_tlb(void) {
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        ::: "rax", "memory"
    );
}

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
