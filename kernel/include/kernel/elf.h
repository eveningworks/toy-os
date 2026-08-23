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
// `elf_size` is the length of the buffer at `elf_phys_addr`, and it is
// REQUIRED -- every bound in the file is checked against it. Without it
// the loader had no way to know where the file ended, so a segment's
// p_offset + p_filesz was unbounded and the copy ran off the end of the
// buffer, out of identity-mapped physical memory, into a page it then
// mapped into userland. Both callers had the size from fs_read() and
// discarded it.
//
// What is refused, all of it decided before anything is mapped:
//   - a buffer shorter than the header, or a header table outside it
//   - e_phentsize that is not sizeof(elf64_phdr), or e_phnum of 0 or
//     more than ELF_MAX_PHNUM
//   - a segment whose file range leaves the buffer, or whose memory
//     range leaves the image region (below the heap -- otherwise it
//     lands under the stack the runner maps afterwards)
//   - p_filesz > p_memsz, and any address computation that overflows
//   - PT_INTERP: a dynamic executable, which nothing here can run.
//     Silently ignoring it produced a process that jumped to an entry
//     point expecting an interpreter that never ran.
//
// Returns 1 on success and sets *out_entry to the ELF's entry point (a
// virtual address, ready to iretq/jmp to). Returns 0 on any error.
//
// **On failure the caller must destroy the address space**, with
// vmm_destroy_address_space(). A rejected FILE maps nothing (validation
// completes before the first mapping), but an out-of-memory failure
// partway through the mapping loop leaves earlier segments mapped, and
// this function does not unwind them -- destroying the address space
// frees every leaf page it owns, which is both simpler and what the
// callers already have to do for the stack and heap they map next.
// `out_image_end` receives the page-aligned address just past the
// highest PT_LOAD segment -- where the caller must start this process's
// heap (`struct sched_mm.heap_base`). May be NULL for a caller that
// arms no heap. Deriving it here rather than fixing it in the map is
// what removed the ceiling on how big a ring-3 image may be; see
// elf.c's ELF_IMAGE_END and kernel/uaddr.h.
int elf_load(uint64_t elf_phys_addr, uint64_t elf_size, uint64_t pml4_phys,
             uint64_t *out_entry, uint64_t *out_image_end);

#endif
