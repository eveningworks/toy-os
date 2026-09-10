#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Marks the single 4KB page containing `vaddr` as user-accessible (adds
// the USER bit to its page-table entry), splitting the 2MB huge page
// that currently covers it into individual 4KB pages first if needed.
// Everything else in that 2MB region keeps its original identity mapping
// (present + writable, no USER bit) -- only the specific page(s) you
// call this on become reachable from ring 3.
//
// vaddr must fall within the first 4GiB (the range boot.asm identity-maps
// with 2MiB pages). Returns 1 on success, 0 if vaddr is out of range or
// the small fixed pool of split page tables is exhausted (see
// MAX_SPLIT_TABLES in paging.c -- this isn't a general-purpose allocator,
// just enough for a handful of user-test regions).
int paging_make_user_page(uint64_t vaddr);

// Applies W^X to the kernel's own identity map, which boot.asm leaves
// flat present+writable across all 4GiB. Call once, as early in
// kernel_main() as possible -- it needs no allocator, no heap and no
// interrupts, only the linker symbols describing where .text ends.
//
// Afterwards exactly one range in the kernel map is executable (.text,
// which is also read-only), the rest of the kernel image is read-only
// and NX, and every other page -- RAM, the framebuffer, MMIO -- is
// writable and NX. It also sets CR0.WP, without which ring 0 would
// ignore the read-only bit entirely; see paging.c for why that half is
// easy to leave out and hard to notice.
//
// Returns 1 on success, 0 if the fixed pool of split page tables wasn't
// big enough to cover the read-only part of the kernel image (see
// MAX_WX_TABLES in paging.c). A 0 means the map is left partly or
// wholly unprotected -- it is a real failure, not a nicety, and the
// "paging" KTEST asserts against it.
int paging_enforce_wx(void);

// The leaf page-table entry the kernel's own map resolves `vaddr`
// through -- a 2MiB PDE where the map is still huge, a 4KiB PTE where
// paging_enforce_wx() split it. Returns 0 if nothing is mapped. For
// tests and diagnostics; nothing in the kernel's normal operation needs
// to ask.
// Makes one 4KiB page of the identity map NOT PRESENT, splitting the
// 2MiB huge page that covers it first if needed (siblings keep exactly
// the permissions they had). Returns 0 if the address is out of the
// identity map or the split pool is exhausted.
//
// For GUARD PAGES: the per-process kernel stacks (scheduler.c) each have
// one below them, so an overflow faults instead of silently overwriting
// the next slot's saved trapframe. Must run AFTER paging_enforce_wx(),
// which rewrites every PDE.
int paging_unmap_kernel_page(uint64_t vaddr);

uint64_t paging_kernel_leaf(uint64_t vaddr);

// Makes [phys, phys+size) of the identity map EXECUTABLE and read-only
// (exec=1), or writable and NX again (exec=0) -- the permissions of a
// loaded module's text, which paging_enforce_wx() gave the blanket
// "RAM: writable, NX". Splits the covering 2 MiB leaves on first use,
// siblings keeping what they had. Page-granular: `phys` and `size`
// must be 4 KiB multiples. Returns 0 when the range is outside the
// identity map or the split pool is spent -- and a 0 on exec=1 means
// the code there will #PF on its first instruction, so the caller
// must not run it.
int paging_set_kernel_exec(uint64_t phys, uint64_t size, int exec);

// Undoes paging_set_write_combining() over a range (PAT only; an MTRR
// range stays). For a TEST that types a frame and must give it back
// cached; nothing in normal operation clears a type.
int paging_clear_write_combining(uint64_t phys, uint64_t size);

// Whether the kernel map resolves `vaddr` through a 2 MiB PDE (1) or a
// 4 KiB PTE (0); -1 when nothing is mapped. Asked separately because a
// leaf cannot say for itself: bit 7 is HUGE in a PDE and PAT in a PTE.
int paging_kernel_leaf_is_huge(uint64_t vaddr);

// The write-combining (PAT) bit of a leaf, given which kind it is.
#define PAGING_LEAF_WC(e, huge) ((huge) ? (((e) >> 12) & 1ULL) : (((e) >> 7) & 1ULL))

// The kernel map's 2 MiB granule. pmm manages the zone above 4 GiB in
// whole granules so that every managed high frame is a mapped one.
#define PAGING_HUGE_SIZE 0x200000ULL

// Extend the identity map over every usable region above 4 GiB, in
// whole 2 MiB slots, with page directories taken from PMM_ZONE_DMA32.
// Runs once, after pmm_init() and after paging_enforce_wx(); the new
// leaves are writable and NX like the RAM below. Returns bytes mapped.
uint64_t paging_extend_identity_map(void);

// One past the highest identity-mapped byte: 4 GiB on a small machine,
// the end of the last extended slot otherwise. A kernel pointer below
// it is physical by construction; one above it is a bug.
uint64_t paging_identity_limit(void);

// ioremap: the kernel virtual address for a device's MMIO window, or
// NULL. Below 4 GiB it is the physical address (the boot map covers it
// and always has); above, a fresh uncached mapping in the
// UADDR_KDEV_BASE arena -- so a driver keeps the POINTER, and must not
// assume it equals the BAR. Never unmapped.
volatile void *paging_map_device(uint64_t phys, uint64_t size);

// How many pages in the kernel's own map are simultaneously writable
// and executable, i.e. how many times W^X is violated. Should be 0
// after paging_enforce_wx(); walks all 2048 PDEs and every split table
// under them.
int paging_wx_violations(void);

// Whether each of the two CR4 protections is on, as returned by both
// functions below.
#define PAGING_SMEP_ON (1 << 0)
#define PAGING_SMAP_ON (1 << 1)

// Sets CR4.SMEP and CR4.SMAP where the CPU supports them, and returns
// which ones were turned on. Call once from kernel_main(), before
// anything drops to ring 3.
//
// SMEP stops ring 0 EXECUTING a user page; SMAP stops it READING or
// WRITING one. Neither is a nicety layered over the page-table bits --
// they are the CPU refusing what the bits merely describe, which is
// what makes a corrupted kernel pointer into a fault rather than an
// exploit.
//
// **The rule SMAP imposes on the whole kernel: touch user memory only
// through vmm.h's copy helpers.** Those go via the kernel's identity
// map, so AC is never set anywhere in this kernel and there is no
// window in which the protection is off. A raw `*(T *)user_ptr` in
// kernel code faults once this is on -- which is the point.
//
// A CPU without either bit gets neither set (writing a reserved CR4 bit
// is a #GP, not a no-op) and the return value says so. QEMU's default
// `qemu64` model is exactly that case, so testing the hardware path
// needs `--cpu max`.
int paging_enable_smep_smap(void);

// What CR4 says right now -- for the KTESTs and `lscpu`-style
// reporting, so "we asked for it" and "the CPU has it" stay separate
// questions.
int paging_smep_smap_state(void);

// How a range's memory type was arranged. Reported rather than assumed
// because the three outcomes perform ENORMOUSLY differently and the
// difference is invisible on screen -- see paging_set_write_combining().
enum paging_wc_result {
    PAGING_WC_NONE = 0,  // left as firmware set it (uncached, on real hardware)
    PAGING_WC_PAT,       // a write-combining IA32_PAT slot, selected per page
    PAGING_WC_MTRR,      // a variable-range MTRR covering the region
};

// Asks the CPU to treat [phys, phys+size) as WRITE-COMBINING: stores are
// gathered in a fill buffer and flushed to the bus in bursts instead of
// each one making the CPU wait on its own transaction.
//
// **This is the difference between a usable framebuffer and an unusable
// one, and nothing on screen says which you got.** GRUB's linear
// framebuffer is uncached MMIO on real hardware, where a single-byte
// store costs a full bus round trip -- a 1920x1080 frame is millions of
// them, i.e. seconds per repaint. Under QEMU the "framebuffer" is
// ordinary cached host RAM, so the entire problem is invisible there and
// no test in this repo can observe it; `gfxbench` on real hardware is
// the only way to see the difference.
//
// Two mechanisms, because one of them is not always available:
// PAT (per-page, precise, needs CPUID.01H:EDX[16]) is preferred, and a
// variable-range MTRR is the fallback. `nopat` on the GRUB command line
// forces the MTRR path -- without that switch the fallback would be
// unreachable on every machine this OS can run on and therefore a guess,
// the same reason `ata nodma` exists.
//
// `size` 0, an unmapped range or a range crossing 4GiB is refused.
// Returns an enum paging_wc_result saying which mechanism actually
// applied, NOT whether the call was reasonable.
int paging_set_write_combining(uint64_t phys, uint64_t size);

// A name for enum paging_wc_result, for logs and `gfxbench`.
const char *paging_wc_name(int result);

#endif
