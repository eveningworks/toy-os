#ifndef VMM_H
#define VMM_H

#include <stdint.h>

// Gives each process its own top-level page table (PML4) instead of
// everything sharing the one boot.asm sets up. Builds on paging.h (which
// only ever punches holes in that single shared table) and pmm.h (which
// supplies the physical frames these page tables are built from).
//
// Every process's PML4 shares entry 0 with the kernel's own -- i.e. the
// same physical PDPT/PD structures that identity-map the low 4GiB, so
// kernel code, the IDT/GDT, and the framebuffer/VGA memory stay reachable
// no matter which process's CR3 is loaded. This isn't optional: an
// interrupt does NOT switch CR3 on entry, so if a process's page tables
// didn't include a working mapping for kernel code, the very first
// interrupt while that process is running would fault trying to execute
// the handler. What's actually private per-process is everything mapped
// at VMM_USER_BASE and above -- a virtual range no other process's page
// tables have any entry for at all.

// Creates a new address space: a fresh PML4 with entry 0 copied from the
// kernel's own (see above). Returns the new PML4's physical address
// (also usable as an opaque "address space" handle for the other
// functions here), or 0 on allocation failure.
uint64_t vmm_create_address_space(void);

// Maps one 4KiB page at `vaddr` to physical frame `paddr`, present +
// user, allocating any intermediate page-table levels needed along the
// way. Private to whichever address space `pml4_phys` identifies.
// Writable and NOT executable -- the secure default for the data pages
// every caller of this particular function maps (stack, heap, GUI
// framebuffer/window buffers). Returns 1 on success, 0 on allocation
// failure. See vmm_map_user_page_flags() below for a caller that needs
// different permissions (the ELF loader's executable .text segment).
int vmm_map_user_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr);

// Same as vmm_map_user_page(), but with explicit control over the
// writable and executable (NX) bits instead of the secure "writable,
// not executable" default -- what elf.c's loader uses, deriving both
// from each PT_LOAD segment's real p_flags (PF_W/PF_X) instead of
// mapping every segment identically. Requires EFER.NXE to already be
// set (boot.asm, once at boot) for `executable == 0` to actually be
// enforced by the CPU rather than silently ignored.
int vmm_map_user_page_flags(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                             int writable, int executable);

// Loads CR3 with the given address space.
void vmm_switch_address_space(uint64_t pml4_phys);

// Frees every physical frame this address space privately owns -- every
// present page-table page at every level (PDPT/PD/PT) AND every leaf
// page they map -- then frees the PML4 frame itself. Walks entries
// 1..511 of the PML4 only, deliberately skipping entry 0 (the shared
// kernel identity map -- every address space's PML4 points at the SAME
// physical PDPT there, see vmm_create_address_space(); freeing it would
// corrupt every other process's mapping, not just this one's).
//
// Caller's responsibility, not this function's: switch CR3 away from
// `pml4_phys` BEFORE calling this (e.g. via vmm_switch_address_space()
// to vmm_kernel_pml4_phys()) -- this frees the PML4 frame CR3 points at,
// and leaving CR3 pointing at a freed frame that the very next
// pmm_alloc_frame() could hand out to something else is a use-after-free
// waiting to happen, even on hardware with no real concurrency to race
// against. A no-op if `pml4_phys` is 0.
void vmm_destroy_address_space(uint64_t pml4_phys);

// Reads CR3 -- the address space active right now. Inside a syscall
// handler (which doesn't switch CR3 on entry -- see the note above),
// this is the calling process's own address space, which is exactly
// what vmm_validate_user_range() needs to check a user-supplied pointer
// against.
uint64_t vmm_current_pml4(void);

// Checks that every byte in [vaddr, vaddr+len) is mapped present AND
// user-accessible in the given address space -- at every level of the
// page-table walk, not just the leaf page, matching how the CPU itself
// enforces access (see the README's "Process isolation" section for the
// bug that taught us USER has to be checked/set at every level, not
// just the final one). Returns 1 if the whole range is valid, 0
// otherwise (unmapped, kernel-only, or the range overflows).
//
// This exists so syscalls that accept a pointer from ring 3 (see
// syscall.c's SYS_WRITE) don't have to trust it blindly: without this,
// a process could hand the kernel an address it could never legally
// read itself -- kernel-only memory is still *present* in every
// process's page tables (PML4 entry 0 is shared, see above), just not
// user-accessible -- and get the kernel, running at full privilege, to
// read it on the process's behalf.
int vmm_validate_user_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t len);

// The physical address of the kernel's own (shared, original) PML4 --
// what CR3 pointed to before any process address space existed.
uint64_t vmm_kernel_pml4_phys(void);

// Where process-private mappings should live: 512GiB, deliberately far
// from the shared 0-4GiB kernel identity map so there's no risk of ever
// colliding with it. Lands exactly on a PML4-entry boundary (entry 1),
// keeping it cleanly separate from entry 0's shared kernel mapping.
#define VMM_USER_BASE 0x0000008000000000ULL

#endif
