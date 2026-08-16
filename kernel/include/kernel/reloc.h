#ifndef RELOC_H
#define RELOC_H

#include <stdint.h>

// Kernel self-relocation -- the machinery kernel ASLR is built on.
// See reloc.c's top comment for how it works and what is deliberately
// not built yet, and tools/genrelocs.py for where the table comes from.
//
// Kernel-internal on purpose: nothing in apps/ has any business moving
// the kernel image.

// How many absolute references the image carries, and how much the
// table costs. Reported at boot and asserted by the KTESTs.
uint64_t kernel_reloc_count(void);
uint64_t kernel_reloc_table_bytes(void);

// Moves every absolute reference in the image by `delta`.
//
// MUST run before paging_enforce_wx(): it writes into .text, which
// that call makes read-only (with CR0.WP, so ring 0 is not exempt).
// `delta` must be >= 0 -- see reloc.c on why a lower base would hand
// the old image's pages to the frame allocator.
void kernel_relocate(int64_t delta);

// How many fixups would NOT survive a move of `delta`; 0 means safe.
// Non-zero only for 32-bit references, which -mcmodel=kernel emits as
// sign-extended immediates and which therefore cannot cross 2GiB.
// This is what bounds how high a randomized base may go.
uint64_t kernel_reloc_check(int64_t delta);

// ---- stage 3: choosing a base and moving the image ----

// Called from long_mode_start (boot.asm) with the Multiboot2 info
// pointer, BEFORE kernel_main. Picks a random 2MiB-aligned base above
// the link address, copies the image there, applies the relocation
// table to the copy and repoints CR3 at the copied page tables.
// Returns the delta applied, or 0 if it did not relocate (in which
// case kernel_reloc_note() says why).
//
// The caller must then continue in the RELOCATED image: add the delta
// to both the stack pointer and the address it calls kernel_main at.
// Returning normally would carry on in the abandoned copy.
//
// `nokaslr` on the GRUB command line disables it -- the recovery path
// if a machine turns out not to survive relocation.
uint64_t kernel_relocate_boot(uint64_t mb2_info);

// How far the running image was moved (0 = not relocated). pmm.c needs
// it to reserve the ABANDONED image as well: it still holds the GDT
// the CPU uses until gdt_init(), and it sits below the new image where
// nothing else would cover it.
uint64_t kernel_reloc_delta(void);

// Did the base come from RDSEED/RDRAND (1) or from the timestamp
// counter (0)? Reported at boot rather than assumed: the TSC fallback
// is weak, and it is what QEMU's default qemu64 gets. krandom_init()
// cannot be used this early -- it spins until the PIT ticks, and the
// PIT is not running yet.
int kernel_reloc_entropy_hw(void);

// How many candidate bases the chosen one was drawn from -- the honest
// form of "how much entropy", since it is bounded by RAM, not by the
// random source.
uint64_t kernel_reloc_slots(void);

// One phrase for the boot log: "relocated", or why not.
const char *kernel_reloc_note(void);

// How many table entries do not currently describe a reference into
// this image -- 0 for a table that matches the image it ships with.
// A drifted table's first symptom is otherwise a kernel that does not
// boot, with nothing to read.
//
// Covers the read-only part of the image only: a fixup in .data is a
// pointer the kernel may legitimately have reassigned since boot. See
// reloc.c.
uint64_t kernel_reloc_implausible(void);

#endif
