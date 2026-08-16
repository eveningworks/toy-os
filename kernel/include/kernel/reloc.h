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
