#ifndef KERNEL_KSYMS_H
#define KERNEL_KSYMS_H

#include <stdint.h>

// Turning a kernel address into a function name, for panics.
//
// The table is baked into the image by tools/gen_syms.py and lives in
// its own `.ksyms` section (see linker.ld). It stores LINK-TIME
// addresses, so a caller passes a running address and this subtracts
// the relocation delta itself -- the kernel moves to a random base at
// boot, which is exactly why a raw address in a panic was unreadable.
//
// Deliberately usable from the fault handler: no allocation, no locks,
// no filesystem, and a binary search over a table that cannot change.
// If the section is empty (a build without the generator, or a first
// link pass) every lookup simply reports nothing rather than failing.

// Names `addr` (a RUNNING address, as taken from a trapframe). Returns
// the function name and, through `out_off`, how far into it the address
// sits -- so a caller prints `try_merge_next+0x2f`. Returns NULL when
// the address is outside every known function or the table is absent.
const char *ksyms_lookup(uint64_t addr, uint32_t *out_off);

// How many symbols the table holds. 0 means there is no table, which is
// worth saying once at boot rather than leaving a reader to wonder why
// panics have no names.
uint32_t ksyms_count(void);

#endif
