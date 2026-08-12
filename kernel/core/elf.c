// A minimal ELF64 loader: PT_LOAD segments only, no relocations, no
// dynamic linking, no section/symbol-table parsing. See elf.h.
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include <stddef.h>

#define EI_NIDENT 16

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define PT_LOAD    1
#define ET_EXEC    2
#define EM_X86_64  62
#define ELFCLASS64 2

#define PF_X 1 // executable
#define PF_W 2 // writable

#define PAGE_SIZE 4096ULL

// Loads one PT_LOAD segment, page by page. A segment's file/memory
// bounds don't have to be page-aligned (p_memsz can exceed p_filesz for
// .bss, and p_vaddr itself might not start on a page boundary), so each
// page is handled individually: allocate + zero a fresh frame, copy
// whichever bytes of *this page* fall within the segment's file-backed
// range, leave the rest zeroed, then map it.
//
// Maps every page in this segment with the SAME writable/executable
// bits, taken from the segment's own p_flags (PF_W/PF_X) rather than
// the old blanket present+writable+user every segment used to get --
// see vmm.c's vmm_map_user_page_flags(). This only does anything real
// once userland/link.ld actually puts .text/.rodata/.data in separate
// page-aligned PT_LOAD segments with distinct p_flags; a single merged
// RWX segment (the old default) would make every page executable
// regardless of this code.
static int load_segment(uint8_t *elf_base, const struct elf64_phdr *ph, uint64_t pml4_phys) {
    int writable = (ph->p_flags & PF_W) != 0;
    int executable = (ph->p_flags & PF_X) != 0;
    uint64_t vaddr_start = ph->p_vaddr & ~(PAGE_SIZE - 1);
    uint64_t vaddr_end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uint64_t seg_file_start = ph->p_vaddr;
    uint64_t seg_file_end = ph->p_vaddr + ph->p_filesz;

    for (uint64_t page_va = vaddr_start; page_va < vaddr_end; page_va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return 0;

        uint8_t *dst = (uint8_t *)(uintptr_t)frame;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) dst[i] = 0;

        uint64_t page_end = page_va + PAGE_SIZE;
        uint64_t copy_start = page_va > seg_file_start ? page_va : seg_file_start;
        uint64_t copy_end = page_end < seg_file_end ? page_end : seg_file_end;

        if (copy_start < copy_end) {
            uint64_t file_off = ph->p_offset + (copy_start - ph->p_vaddr);
            uint64_t dst_off = copy_start - page_va;
            uint64_t len = copy_end - copy_start;
            for (uint64_t b = 0; b < len; b++) dst[dst_off + b] = elf_base[file_off + b];
        }

        if (!vmm_map_user_page_flags(pml4_phys, page_va, frame, writable, executable)) return 0;
    }

    return 1;
}

int elf_load(uint64_t elf_phys_addr, uint64_t pml4_phys, uint64_t *out_entry) {
    uint8_t *base = (uint8_t *)(uintptr_t)elf_phys_addr;
    struct elf64_ehdr *eh = (struct elf64_ehdr *)base;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != ELFCLASS64) return 0;
    if (eh->e_type != ET_EXEC) return 0; // static, non-PIE only
    if (eh->e_machine != EM_X86_64) return 0;

    struct elf64_phdr *phdrs = (struct elf64_phdr *)(base + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (!load_segment(base, ph, pml4_phys)) return 0;
    }

    *out_entry = eh->e_entry;
    return 1;
}
