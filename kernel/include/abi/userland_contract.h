#ifndef USERLAND_CONTRACT_H
#define USERLAND_CONTRACT_H

// Constants shared between userland test programs (userland/*.c) and the
// kernel code that loads and inspects them (kernel/proc/elf.c and
// kernel/proc/elf_run.c).
//
// elf.c only ever handles PT_LOAD segments -- there's no ELF symbol-table
// parsing (no .symtab/.strtab handling), so the kernel can't look up a
// symbol like "marker" by name inside the loaded binary. Instead, both
// sides agree on a fixed virtual address ahead of time: the userland
// program pokes it directly, and the kernel maps a page there separately
// from (and in addition to) whatever the ELF's own PT_LOAD segments
// cover, then reads it back after the test.
#define USERLAND_MARKER_ADDR 0x8000100000ULL

#endif
