// The page tables behind KASAN's shadow (api/kasan.h has the layout;
// kernel/lib/kasan.c is the runtime that reads it).
//
// TWO STAGES, because GCC's stack instrumentation WRITES shadow from the
// first instrumented function -- long before there is a frame allocator
// to back 1/8 of RAM with:
//
//   kasan_early_init()  the whole shadow slot -> ONE shared zero page,
//                       through three page tables in .bss. Called first
//                       thing in kernel_relocate_boot(), before anything
//                       instrumented can run. Writes land in the shared
//                       page and mean nothing; nothing is checked yet.
//   kasan_init()        real zeroed pages behind the shadow of every
//                       usable RAM range, the free frames poisoned, and
//                       checking switched on.
//
// THE LIVE TABLES ARE FOUND THROUGH THE PML4 ENTRY, never by symbol.
// kernel_relocate_boot() copies the image after the early tables are
// wired in, so the symbols below name the COPIES -- while every PML4 the
// kernel ever loads points at the originals, which stay reserved (they
// sit below the relocated _kernel_end; see reloc.c's trap 2).
//
// NEVER INSTRUMENTED (KASAN_EXCLUDE): kasan_early_init() runs before the
// shadow exists at all.
#include <stdint.h>
#include "kasan.h"

#ifdef TOYOS_KASAN

#include "pmm.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "kstack.h"
#include "scheduler.h"

#define P_PRESENT (1ULL << 0)
#define P_WRITE   (1ULL << 1)
#define P_NX      (1ULL << 63)
#define P_ADDR    0x000FFFFFFFFFF000ULL
#define TABLE     (P_PRESENT | P_WRITE)           // supervisor only
#define LEAF      (P_PRESENT | P_WRITE | P_NX)

// 64 GiB of shadow covers the 512 GiB kernel map: 64 PDPT entries.
#define SHADOW_PDPT_ENTRIES (KASAN_COVERED_BYTES / 8 / (1ULL << 30))

static uint64_t early_pdpt[512] __attribute__((aligned(4096)));
static uint64_t early_pd[512]   __attribute__((aligned(4096)));
static uint64_t early_pt[512]   __attribute__((aligned(4096)));
static uint8_t  early_zero[4096] __attribute__((aligned(4096)));

static inline uint64_t *live_pml4(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(uintptr_t)(cr3 & P_ADDR);
}

static inline void flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
}

void kasan_early_init(void) {
    for (int i = 0; i < 512; i++) early_pt[i] = (uint64_t)(uintptr_t)early_zero | LEAF;
    for (int i = 0; i < 512; i++) early_pd[i] = (uint64_t)(uintptr_t)early_pt | TABLE;
    for (uint64_t i = 0; i < SHADOW_PDPT_ENTRIES; i++)
        early_pdpt[i] = (uint64_t)(uintptr_t)early_pd | TABLE;
    live_pml4()[KASAN_PML4_INDEX] = (uint64_t)(uintptr_t)early_pdpt | TABLE;
    flush_tlb();
}

// --- kasan_init() ------------------------------------------------------

static uint64_t g_shared_pd, g_shared_pt, g_zero;  // the early tables, by address
static uint64_t g_pages, g_limit;
static int g_oom;

static uint64_t fresh_frame(void) {
    uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!f) { g_oom = 1; return 0; }
    uint64_t *p = (uint64_t *)(uintptr_t)f;
    for (int i = 0; i < 512; i++) p[i] = 0;
    return f;
}

// A private copy of a shared early table, so one slot of it can change.
static uint64_t unshare(uint64_t shared) {
    uint64_t f = fresh_frame();
    if (!f) return 0;
    const uint64_t *src = (const uint64_t *)(uintptr_t)shared;
    uint64_t *dst = (uint64_t *)(uintptr_t)f;
    for (int i = 0; i < 512; i++) dst[i] = src[i];
    return f;
}

// Backs one 4 KiB page of shadow at `va` with a real zeroed frame.
static void back(uint64_t va) {
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(live_pml4()[KASAN_PML4_INDEX] & P_ADDR);
    unsigned i3 = (va >> 30) & 511, i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    if ((pdpt[i3] & P_ADDR) == g_shared_pd) {
        uint64_t pd = unshare(g_shared_pd);
        if (!pd) return;
        pdpt[i3] = pd | TABLE;
    }
    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[i3] & P_ADDR);
    if ((pd[i2] & P_ADDR) == g_shared_pt) {
        uint64_t pt = unshare(g_shared_pt);
        if (!pt) return;
        pd[i2] = pt | TABLE;
    }
    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[i2] & P_ADDR);
    if ((pt[i1] & P_ADDR) != g_zero) return;             // already real
    uint64_t f = fresh_frame();
    if (!f) return;
    pt[i1] = f | LEAF;
    g_pages++;
}

static void back_region(const struct multiboot_mmap_region *r) {
    if (r->type != 1 || !r->length) return;
    uint64_t lo = r->base, hi = r->base + r->length;
    if (lo >= KASAN_COVERED_BYTES) return;
    if (hi > KASAN_COVERED_BYTES) hi = KASAN_COVERED_BYTES;
    if (hi > g_limit) g_limit = hi;
    uint64_t s0 = ((lo >> 3) + KASAN_SHADOW_OFFSET) & ~0xFFFULL;
    uint64_t s1 = (((hi + 7) >> 3) + KASAN_SHADOW_OFFSET + 0xFFF) & ~0xFFFULL;
    for (uint64_t va = s0; va < s1 && !g_oom; va += 4096) back(va);
}

void kasan_init(void) {
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(live_pml4()[KASAN_PML4_INDEX] & P_ADDR);
    g_shared_pd = pdpt[0] & P_ADDR;
    g_shared_pt = ((uint64_t *)(uintptr_t)g_shared_pd)[0] & P_ADDR;
    g_zero      = ((uint64_t *)(uintptr_t)g_shared_pt)[0] & P_ADDR;

    multiboot_mmap_foreach(back_region);
    flush_tlb();
    // What the early writes left in the shared page: stack redzones from
    // before now, none of which describe anything real any more.
    for (int i = 0; i < 4096; i++) ((volatile uint8_t *)(uintptr_t)g_zero)[i] = 0;

    if (g_oom) {
        klog_printf(KLOG_ERR "kasan: out of frames after %lu shadow pages -- NOT enabled\n",
                    (unsigned long)g_pages);
        return;
    }
    pmm_kasan_poison_free();
    kasan_set_ready(g_limit);
    klog_printf("kasan: enabled over [0, 0x%lx), %lu KiB of shadow\n",
                (unsigned long)g_limit, (unsigned long)(g_pages * 4));
}

// --- stacks ------------------------------------------------------------

extern uint8_t stack_bottom[], stack_top[];   // boot.asm: the kernel context's stack

void kasan_unpoison_stack_below(uint64_t sp) {
    uint64_t lo = 0;
    if (sp > (uint64_t)(uintptr_t)stack_bottom && sp <= (uint64_t)(uintptr_t)stack_top) {
        lo = (uint64_t)(uintptr_t)stack_bottom;
    } else {
        for (int i = 0; i < scheduler_slot_end() && !lo; i++) {
            uint64_t base = scheduler_kstack_base(i);
            if (sp > base && sp <= base + KSTACK_BYTES) lo = base;
        }
    }
    if (lo) kasan_unpoison((const void *)(uintptr_t)lo, (size_t)(sp - lo));
}

#else

void kasan_early_init(void) {}
void kasan_init(void) {}
void kasan_unpoison_stack_below(uint64_t sp) { (void)sp; }

#endif
