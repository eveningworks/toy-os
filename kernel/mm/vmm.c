#include "vmm.h"
#include "pmm.h"
#include "string.h" // k_memcpy() -- the user-copy helpers below
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

static struct { uint64_t pml4; uint32_t pages; } g_acct[VMM_ACCT_MAX];

static int acct_slot(uint64_t pml4_phys, int create) {
    int free_slot = -1;
    for (int i = 0; i < VMM_ACCT_MAX; i++) {
        if (g_acct[i].pml4 == pml4_phys && pml4_phys) return i;
        if (!g_acct[i].pml4 && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    g_acct[free_slot].pml4 = pml4_phys;
    g_acct[free_slot].pages = 0;
    return free_slot;
}

uint64_t vmm_user_bytes(uint64_t pml4_phys) {
    int i = acct_slot(pml4_phys, 0);
    return i < 0 ? 0 : (uint64_t)g_acct[i].pages * 4096;
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
    if (!(pt[pt_index] & PAGE_PRESENT)) return 0;

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
        if (!(pt[i] & PAGE_PRESENT)) continue;
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
        if (i >= 0) { g_acct[i].pml4 = 0; g_acct[i].pages = 0; }
    }
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
static uint64_t user_phys_of(uint64_t pml4_phys, uint64_t vaddr) {
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

// Copies between a kernel buffer and user memory THROUGH THE KERNEL'S
// OWN IDENTITY MAP, never by dereferencing the user virtual address.
//
// **That is what makes SMAP absolute here rather than something the
// kernel keeps switching off.** SMAP faults a supervisor access whose
// mapping has U=1 unless EFLAGS.AC is set, and the usual answer is to
// bracket every such access in STAC/CLAC -- which means the protection
// is off for exactly the window where a bug would use it. Walking to
// the frame and copying through the kernel's identity mapping (U=0, all
// 4 GiB of it, see boot.asm) is a supervisor access to a supervisor
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
