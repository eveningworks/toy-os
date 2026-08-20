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

// The memory type a user mapping gets. Ordinary RAM is VMM_MT_NORMAL
// (cacheable, what every mapping here was before this existed); a
// framebuffer wants VMM_MT_WC.
//
// **Write-combining is a WRITE optimisation and makes reads strictly
// worse** -- stores coalesce into burst transfers, while a load is a
// full uncached round trip with no cache fill and no prefetch. So a
// region mapped WC must be written and not read back. That is exactly
// the framebuffer's access pattern and exactly why the console had to
// stop scrolling by reading pixels.
enum vmm_memtype { VMM_MT_NORMAL = 0, VMM_MT_WC = 1 };

// Both of the above plus the memory type. The type is a named argument
// rather than a bit the caller sets, because the PAT bit's POSITION
// depends on the page size (bit 7 on a 4KiB page, bit 12 on a 2MiB one,
// where bit 12 is part of the physical address instead) -- getting that
// wrong does not fault, it repoints the mapping. That reasoning stays
// in one place here.
//
// Note this sets the type on the USER mapping only. The kernel's own
// identity map of the same frames is a separate PTE with its own type,
// set by paging_set_write_combining() at display probe -- the two are
// independent, and a framebuffer wants both.
int vmm_map_user_page_type(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                            int writable, int executable, int memtype);

// Loads CR3 with the given address space.
// Clears one user page's mapping. Does NOT free the frame it pointed
// at, nor the page tables above it -- the caller owns the frame (only
// it knows whether that means pmm_free_frame() or
// pmm_free_contiguous()), and the tables belong to the address space,
// freed wholesale by vmm_destroy_address_space(). Returns 1 if a
// mapping was removed, 0 if nothing was mapped there (not an error).
// Maps a frame this address space does NOT own. Same mapping in every
// other respect; what differs is teardown -- vmm_destroy_address_space()
// unmaps a borrowed page and leaves the frame alone, where an ordinary
// mapping's frame is freed.
//
// Use it for any frame whose lifetime is somebody else's: the kernel's
// glyph tables shared into a GUI client, the real framebuffer, a window
// buffer the window server allocated and frees itself, another
// process's buffer shared with the compositor, the shared zero page a
// revoked slot is poisoned with. The rule is simply "who calls
// pmm_free_frame() for this?" -- if the answer is not "this address
// space's teardown", the mapping is borrowed.
//
// Recorded in a spare PTE bit rather than a side table, because the
// teardown walk has the PTE and nothing else to go on.
int vmm_map_user_borrowed(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                           int writable, int executable, int memtype);

int vmm_unmap_user_page(uint64_t pml4_phys, uint64_t vaddr);

// What one address space's user mappings look like to the physical
// allocator. `dangling` is the one that is a BUG: a present mapping
// pointing at a frame pmm considers free, i.e. memory the allocator may
// hand to somebody else while this process is still reading and writing
// it. Everything else is descriptive.
struct vmm_audit {
    uint64_t pages;           // present user PTEs walked
    uint64_t borrowed;        // of which mapped with vmm_map_user_borrowed()
    uint64_t unmanaged;       // frames pmm doesn't account for (MMIO) -- normal
    uint64_t huge;            // 2MiB leaves, not walked -- none today
    uint64_t dangling;        // present mappings of a FREE frame -- the violation
    uint64_t first_bad_va;    // where the first one was, for reporting
    uint64_t first_bad_frame;
};

// Walks one address space's user half and fills `out`; returns the
// dangling count, so `if (vmm_audit_space(as, 0))` is a valid check.
// `out` may be NULL. Read-only -- it changes nothing, so it is safe to
// call from a shell command or a KTEST at any point.
uint64_t vmm_audit_space(uint64_t pml4_phys, struct vmm_audit *out);

// Reported once per dangling mapping, in walk order (ascending virtual
// address). `frame` is the physical frame the mapping points at.
typedef void (*vmm_dangling_cb)(uint64_t va, uint64_t frame, void *ctx);

// The same walk, also calling `cb` for EVERY dangling mapping rather
// than only recording the first in `first_bad_va`. `/bin/meminfo
// --audit` reports findings as a list and needs each one; the summary
// alone cannot say where the second violation is, and a space with two
// of them is exactly when you want to know.
//
// `cb` may be NULL, which makes this identical to vmm_audit_space().
// The callback must not allocate or map anything -- this is a read-only
// walk over live page tables, and changing them underneath it would
// invalidate the walk in progress.
uint64_t vmm_audit_space_cb(uint64_t pml4_phys, struct vmm_audit *out,
                            vmm_dangling_cb cb, void *ctx);

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
// How many bytes of USER memory are mapped into this address space.
//
// Counted as pages are mapped and unmapped rather than by walking the
// tables, so reading it is free -- Task Manager asks once per process
// per refresh. Counts only what vmm_map_user_page*() placed: the page
// TABLES themselves are not included, since they are the kernel's
// bookkeeping rather than the process's memory.
//
// A remap of an already-mapped address (a window resize does this)
// replaces a frame and does not double count. 0 for an address space
// that has mapped nothing, and for one that no longer exists.
uint64_t vmm_user_bytes(uint64_t pml4_phys);

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

// --- demand paging: the fault-in hook ---------------------------------
//
// SYS_SBRK reserves address space and maps nothing (kernel/uaddr.h), so
// a heap page exists only once something touches it. Three places have
// to be able to produce that page, and they are NOT all faults:
//
//   - the #PF handler, when ring 3 touches it (idt.c);
//   - the copy helpers, which walk to the frame themselves and would
//     otherwise report a perfectly legal buffer as unmapped;
//   - vmm_validate_user_range(), for the same reason.
//
// The second and third are the ones that make this a hook rather than
// a line in the fault handler. A freshly sbrk'd buffer handed to
// sys_read() never faults -- the kernel never dereferences the user
// address -- so without this it would simply be rejected, and demand
// paging would silently break every syscall that takes a buffer.
//
// vmm cannot answer "is this address inside somebody's heap" itself:
// the break lives in the process layer. So that layer REGISTERS a
// handler, the same shape as display_driver and block_device. Returns 1
// if it mapped a page and the access should be retried, 0 if the
// address is not its business (a wild pointer, the stack guard, or a
// heap page it could not allocate a frame for -- all of which must stay
// fatal).
typedef int (*vmm_fault_fn)(uint64_t pml4_phys, uint64_t vaddr);
void vmm_set_fault_handler(vmm_fault_fn fn);

// Asks the registered handler to fault `vaddr` in. 0 when there is no
// handler, which is what every path saw before demand paging existed.
int vmm_fault_in(uint64_t pml4_phys, uint64_t vaddr);

// **The ONLY sanctioned way for kernel code to touch user memory.**
//
// These walk the page tables to each frame and copy through the
// kernel's own identity map rather than dereferencing the user virtual
// address. That is a supervisor access to a supervisor page, so CR4.SMAP
// -- which faults a supervisor access to a USER page unless EFLAGS.AC is
// set -- never applies, and this kernel therefore never sets AC at all.
// The alternative (STAC/CLAC around each access) turns the protection
// off for precisely the window a bug would use it in.
//
// So: **once SMAP is on, dereferencing a ring-3 pointer from kernel code
// is a page fault, not a subtle bug.** If you find yourself wanting a
// raw `*(struct foo *)user_ptr`, that is the thing these replaced.
//
// They also subsume vmm_validate_user_range() at the call sites that
// used to pair it with a manual copy loop: the walk and the copy are one
// operation per page here, so there is no window between checking a
// mapping and using it. Keep using the validator on its own only where
// nothing is copied (a pointer's mere validity is the question).
//
// All three return 1 on success, 0 if any byte of the range is not
// present-and-user-accessible. **A failed copy is all-or-nothing from
// the caller's point of view** -- bytes may already have been written
// before the bad page was reached, so treat the destination as holding
// nothing trustworthy rather than salvaging a prefix.
int vmm_copy_from_user(uint64_t pml4_phys, void *dst, uint64_t uaddr, uint64_t len);
int vmm_copy_to_user(uint64_t pml4_phys, uint64_t uaddr, const void *src, uint64_t len);

// A NUL-terminated string, copied byte at a time (the length isn't known
// until the terminator is found) and always NUL-terminated in `dst`,
// truncating at `max - 1` if the user's string is longer. Fails only on
// an unreadable page, NOT on truncation -- every caller here is copying
// into a fixed path buffer that caps the length anyway.
int vmm_copy_string_from_user(uint64_t pml4_phys, char *dst, uint64_t uaddr, uint64_t max);

// The physical address of the kernel's own (shared, original) PML4 --
// what CR3 pointed to before any process address space existed.
uint64_t vmm_kernel_pml4_phys(void);

// Where process-private mappings should live: 512GiB, deliberately far
// from the shared 0-4GiB kernel identity map so there's no risk of ever
// colliding with it. Lands exactly on a PML4-entry boundary (entry 1),
// keeping it cleanly separate from entry 0's shared kernel mapping.
#define VMM_USER_BASE 0x0000008000000000ULL

#endif
