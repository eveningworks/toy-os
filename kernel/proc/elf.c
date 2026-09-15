// A minimal ELF64 loader: PT_LOAD segments only, no relocations, no
// dynamic linking, no section/symbol-table parsing. See elf.h.
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "uaddr.h" // where a segment is and is not allowed to land
#include "klog.h"
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
#define PT_INTERP  3
#define ET_EXEC    2
#define EM_X86_64  62
#define ELFCLASS64 2

#define PF_X 1 // executable
#define PF_W 2 // writable

#define PAGE_SIZE 4096ULL

// An upper bound on program headers. Real executables from this build
// have four; the cap exists so a corrupt e_phnum cannot walk the header
// loop for 65535 iterations off the end of a small file.
#define ELF_MAX_PHNUM 64

// Where a segment is allowed to land.
//
// The image links at 0x8000000000 (userland/rt/link.ld). A segment
// claiming an address at or past this ceiling would be mapped BEFORE
// the runner maps the stack, so the later mapping silently replaces the
// segment's pages -- or, worse, the segment's pages survive underneath
// and the process runs with its stack sitting on loader-controlled
// bytes. Neither faults; both are decided by a file the loader was
// handed.
//
// **THE CEILING IS THE STACK'S GUARD, NOT THE HEAP'S BASE.** It was
// UADDR_HEAP_BASE, a fixed 1 MiB above the image base, which made this
// check double as a hard limit on how big a ring-3 program could be --
// and userland/rt/link.ld carried a matching ASSERT so the failure was
// a link error rather than a boot-time refusal. The heap starts where
// the image ENDS now (elf_load's out_image_end below), so there is
// nothing between the image and the heap to collide with, and the first
// fixed thing above is the guard. An image that ran all the way up to
// it would leave its process no heap at all -- sbrk would refuse every
// call -- which is a useless binary rather than an unsafe one, and it
// is the file's own doing.
// ELF_IMAGE_BASE/ELF_IMAGE_END live in elf.h now -- spawn needs the
// base for AT_PHDR.

// a + b, refusing on unsigned overflow. Every bound below is computed
// from two file-controlled 64-bit values, so a wrapped sum would pass a
// <= check it should have failed -- the usual way a range test becomes
// a no-op.
static int add_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > (uint64_t)-1 - b) return 0;
    *out = a + b;
    return 1;
}

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
        uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
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

// Is this segment's file range inside the file, and its memory range
// inside the region a program image may occupy?
//
// Every value here comes from the file being loaded, so each is treated
// as hostile: this is the only thing standing between a malformed
// header and either a read past the end of the loader's buffer or a
// mapping placed over the stack.
static int segment_ok(const struct elf64_phdr *ph, uint64_t elf_size) {
    // .bss is p_memsz > p_filesz; the reverse is malformed, and would
    // make load_segment()'s copy range exceed the pages it maps.
    if (ph->p_filesz > ph->p_memsz) return 0;

    // An EMPTY segment maps nothing, so it gets no say in where it
    // would have gone. Every binary this build produces ends with one --
    // a third PT_LOAD at p_vaddr 0 with p_memsz 0, which ld emits for
    // the empty RW group -- and rejecting it on the address check below
    // refused every real executable while a hand-built test fixture
    // (which has no such segment) loaded perfectly. load_segment()
    // already no-ops on these: its page loop covers an empty range.
    if (ph->p_memsz == 0) return 1;

    // The file-backed bytes must be inside the buffer we were handed.
    // This is the defect that mattered most: without it,
    // load_segment()'s copy reads elf_base[p_offset + ...] straight past
    // the end of fs_read()'s allocation, out of identity-mapped physical
    // memory, and into a page it then maps into userland.
    uint64_t file_end;
    if (!add_ok(ph->p_offset, ph->p_filesz, &file_end)) return 0;
    if (file_end > elf_size) return 0;

    // The memory range must sit inside the image region -- above it are
    // the heap, the guard and the stack, which the runner maps after
    // this returns.
    uint64_t mem_end;
    if (!add_ok(ph->p_vaddr, ph->p_memsz, &mem_end)) return 0;
    if (ph->p_vaddr < ELF_IMAGE_BASE) return 0;
    if (mem_end > ELF_IMAGE_END) return 0;

    return 1;
}

int elf_load(uint64_t elf_phys_addr, uint64_t elf_size, uint64_t pml4_phys,
             uint64_t *out_entry, uint64_t *out_image_end,
             struct elf_dyn_info *out_dyn) {
    uint8_t *base = (uint8_t *)(uintptr_t)elf_phys_addr;

    if (out_dyn) {
        out_dyn->interp[0] = 0;
        out_dyn->phoff = 0;
        out_dyn->phnum = 0;
    }

    // Before the header is READ, not after -- dereferencing eh on a
    // shorter buffer is itself the bug.
    if (!base || elf_size < sizeof(struct elf64_ehdr)) return 0;

    struct elf64_ehdr *eh = (struct elf64_ehdr *)base;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return 0;
    if (eh->e_ident[4] != ELFCLASS64) return 0;
    if (eh->e_type != ET_EXEC) return 0; // static, non-PIE only
    if (eh->e_machine != EM_X86_64) return 0;

    // The header table is indexed with e_phentsize as the stride by
    // every other ELF reader; this one indexes a struct array instead,
    // so a file declaring a different stride would be silently
    // misparsed field by field. Refuse rather than reinterpret.
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM) return 0;

    // The table itself must be inside the file.
    uint64_t ph_bytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    uint64_t ph_end;
    if (!add_ok(eh->e_phoff, ph_bytes, &ph_end)) return 0;
    if (ph_end > elf_size) return 0;

    struct elf64_phdr *phdrs = (struct elf64_phdr *)(base + eh->e_phoff);

    // Validate EVERY header before mapping ANY of them, so a file that
    // is bad in its third segment does not leave the first two mapped
    // in a half-built address space. Rejection is then a pure function
    // of the file, with no side effects to unwind.
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];

        // A dynamic executable names its interpreter here. With no
        // out_dyn the caller cannot load one, and silently ignoring
        // the header produces a process that jumps to an entry point
        // expecting an interpreter that never ran -- so it stays a
        // refusal there (the legacy `run` loader, and the interpreter
        // load itself). The path must be inside the file and
        // NUL-terminated; anything else is a malformed file.
        if (ph->p_type == PT_INTERP) {
            uint64_t iend;
            if (!out_dyn) {
                klog_write(KLOG_ERR "elf: refused -- dynamic executable (PT_INTERP), use spawn\n");
                return 0;
            }
            if (ph->p_filesz == 0 ||
                ph->p_filesz > sizeof out_dyn->interp ||
                !add_ok(ph->p_offset, ph->p_filesz, &iend) ||
                iend > elf_size ||
                base[ph->p_offset + ph->p_filesz - 1] != '\0') {
                klog_write(KLOG_ERR "elf: refused -- bad PT_INTERP path\n");
                return 0;
            }
            k_memcpy(out_dyn->interp, base + ph->p_offset, ph->p_filesz);
            continue;
        }

        if (ph->p_type != PT_LOAD) continue;
        if (!segment_ok(ph, elf_size)) {
            klog_write(KLOG_ERR "elf: refused -- segment out of bounds\n");
            return 0;
        }
    }

    // The entry point must be inside the image too: it is loaded into
    // RIP for a ring-3 iretq, and an unmapped one faults immediately
    // while an address inside the stack would execute the stack.
    if (eh->e_entry < ELF_IMAGE_BASE || eh->e_entry >= ELF_IMAGE_END) return 0;

    // WHERE THE IMAGE ENDS, which is where the caller starts the heap.
    //
    // The highest page-aligned end of any PT_LOAD, not the last
    // segment's -- program headers are not required to be in address
    // order, and a loader that assumed they were would put the heap
    // underneath a segment it had just mapped. Empty segments are
    // skipped for the reason segment_ok() states: every binary this
    // build produces ends with a p_memsz 0 header at p_vaddr 0, and
    // taking its end into the maximum would be harmless while taking a
    // MINIMUM would not -- so this is written as the max it is.
    uint64_t image_end = ELF_IMAGE_BASE;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        // A failure here is out of memory, not a bad file -- the caller
        // destroys the address space, which frees whatever was mapped
        // before it (see elf.h).
        if (!load_segment(base, ph, pml4_phys)) return 0;

        if (ph->p_memsz == 0) continue;
        // segment_ok() has already refused anything that would overflow
        // here or land past ELF_IMAGE_END, so both sums are safe.
        uint64_t end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (end > image_end) image_end = end;
    }

    *out_entry = eh->e_entry;
    if (out_image_end) *out_image_end = image_end;
    if (out_dyn) {
        out_dyn->phoff = eh->e_phoff;
        out_dyn->phnum = eh->e_phnum;
    }
    return 1;
}
