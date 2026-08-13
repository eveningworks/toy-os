#ifndef USERLAND_CONTRACT_H
#define USERLAND_CONTRACT_H

// Constants shared between userland test programs (userland/*.c) and the
// kernel code that loads and inspects them (kernel/proc/elf.c and
// kernel/proc/elf_run.c).
//
// Currently empty, and that's the honest state of it. It used to hold
// USERLAND_MARKER_ADDR: elf.c only handles PT_LOAD segments (no
// symbol-table parsing), so the kernel can't find a symbol like
// "marker" by name inside a loaded binary, and the old `elftest`
// harness worked around that by agreeing on a fixed address with
// userland/hello.c -- the program poked it, and the harness mapped a
// page there and read it back to prove the binary had run.
//
// That harness is gone (the ELF64-to-`/bin` migration folded `elftest`
// into the generic `run hello` path, which maps no such page), so the
// address outlived its only mechanism and left `hello` faulting on a
// write to unmapped memory. Removed along with the fix rather than
// kept as a constant nothing implements -- and it was aliasing
// ELF_RUN_HEAP_VADDR (elf_run.c) exactly, so a future read-back test
// wanting this back needs its own address clear of the heap and stack,
// not this one. See docs/decisions.md and CHANGELOG.md.
//
// The header itself stays: it is the natural home for the next thing
// the kernel and a freestanding `/bin` binary have to agree on, and
// removing it would just mean recreating it.

#endif
