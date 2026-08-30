#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

void multiboot_set_info(uint64_t addr);

// Prints a summary of the memory map (from the Multiboot2 mmap tag) and
// basic lower/upper memory info to the VGA console.
void multiboot_print_meminfo(void);

struct multiboot_mmap_region {
    uint64_t base;
    uint64_t length;
    uint32_t type; // 1 = available (RAM); anything else is reserved/unusable
};

// Calls cb(region) once for every entry in the Multiboot2 memory map tag.
// Used by pmm.c to find which physical memory is actually usable RAM --
// see paging.h/pmm.h for why the kernel needs to know that. No-op if the
// mmap tag isn't present.
void multiboot_mmap_foreach(void (*cb)(const struct multiboot_mmap_region *region));

// The command line GRUB was given for this kernel, or 0 if the
// bootloader supplied no command-line tag -- which is the normal case
// for this repo's grub.cfg, so callers must handle 0 rather than
// expecting an empty string. First user: `nokaslr` (see reloc.h).
const char *multiboot_cmdline(void);

// The ACPI Root System Description Pointer, as GRUB found it: tag 15
// (ACPI 2.0+, has an XSDT) if present, else tag 14 (ACPI 1.0, RSDT
// only). Returns 0 when neither tag is there, which is the answer on a
// machine with no ACPI at all -- and also on a bootloader that does not
// pass it, which is why kernel/acpi/acpi.c falls back to scanning the
// BIOS area rather than treating 0 as "no ACPI".
//
// The RSDP is COPIED INTO the tag by GRUB, so this points into the
// multiboot info block (which pmm_init() reserves), not at firmware
// memory. *out_bytes is the tag's payload size: 20 for a v1 RSDP, 36
// for v2.
const void *multiboot_acpi_rsdp(uint32_t *out_bytes);

struct multiboot_module_info {
    uint64_t start; // physical address (inclusive)
    uint64_t end;   // physical address (exclusive)
    int found;
};

// Finds the Nth Multiboot2 module tag (type 3, 0-indexed in the order
// GRUB reports them -- matching the order of `module2` lines in
// grub.cfg) -- the file(s) GRUB loads into memory alongside the kernel.
// Used by elf_test.c and syscall_test.c to find their userland ELF
// binaries. Returns 1 if found (out->found mirrors the return value),
// 0 otherwise.
int multiboot_get_module(int index, struct multiboot_module_info *out);

// The physical range occupied by the Multiboot2 info structure itself
// (the tag list boot.asm's argument points at -- NOT the modules'
// content, which multiboot_get_module() covers separately). GRUB places
// this wherever it likes, same as modules -- see pmm.c's pmm_init(),
// which reserves this range for the same reason it reserves module
// ranges: pmm_alloc_frame() must never hand out memory multiboot_*()
// still needs to read (e.g. multiboot_get_module(), called again after
// a process has already allocated frames -- see scheduler.c). Returns 1
// and fills *out_start/*out_end if multiboot_set_info() has been
// called, 0 otherwise.
int multiboot_get_info_range(uint64_t *out_start, uint64_t *out_end);

struct framebuffer_info {
    uint64_t addr;
    uint32_t pitch;    // bytes per row
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type;     // 0 = indexed, 1 = RGB, 2 = EGA text
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
    int found;
};

// Fills *out from the Multiboot2 framebuffer info tag (type 8).
// Returns 1 if found, 0 otherwise (out->found mirrors the return value).
int multiboot_get_framebuffer(struct framebuffer_info *out);

#endif
