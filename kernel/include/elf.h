#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// Loads a static, non-PIE ELF64 executable already present in physical
// memory (e.g. a Multiboot2 module -- see multiboot_get_module()) into
// the given process address space, using vmm_map_user_page() (see
// vmm.h) for each PT_LOAD segment.
//
// Deliberately minimal: only PT_LOAD program headers are handled -- no
// relocations, no dynamic linking, no section or symbol-table parsing.
// That's enough for a statically-linked, fixed-address executable (like
// userland/hello.c, built exactly that way), but not for anything more
// sophisticated.
//
// Returns 1 on success and sets *out_entry to the ELF's entry point (a
// virtual address, ready to iretq/jmp to). Returns 0 on any error --
// bad magic, wrong class/machine/type, or a pmm_alloc_frame() /
// vmm_map_user_page() failure partway through.
int elf_load(uint64_t elf_phys_addr, uint64_t pml4_phys, uint64_t *out_entry);

#endif
