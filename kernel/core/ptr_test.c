#include "ptr_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "klog.h"

#define STACK_VADDR 0x8000200000ULL

void ptr_test_run(void) {
    // write_bad_test.elf is the FOURTH Multiboot2 module (index 3) --
    // see grub.cfg's four `module2` lines.
    struct multiboot_module_info mod;
    if (!multiboot_get_module(3, &mod)) {
        vga_write("ptrtest: no fourth Multiboot2 module found -- was\n");
        vga_write("userland/write_bad_test.elf added as a GRUB module?\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("ptrtest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("ptrtest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys || !vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("ptrtest: failed to set up the stack\n");
        return;
    }

    vga_write("Running a process that deliberately passes a kernel-only\n");
    vga_write("pointer (0x1000) to the write syscall...\n");
    klog_write("ptr_test: calling process_run_ring3()\n");

    int result = process_run_ring3(as, entry, STACK_VADDR + 4096);

    klog_write("ptr_test: process_run_ring3() returned\n");
    if (result == 1) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write("\nPointer validation confirmed: the write syscall correctly\n");
        vga_write("rejected the invalid pointer instead of reading kernel\n");
        vga_write("memory on the process's behalf.\n");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_write("\nBUG: the syscall accepted an invalid pointer! (result=");
        vga_write_dec((uint32_t)result);
        vga_write(")\n");
    }
}
