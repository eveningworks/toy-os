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
// writable + user, allocating any intermediate page-table levels needed
// along the way. Private to whichever address space `pml4_phys`
// identifies. Returns 1 on success, 0 on allocation failure.
int vmm_map_user_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr);

// Loads CR3 with the given address space.
void vmm_switch_address_space(uint64_t pml4_phys);

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
