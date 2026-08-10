#include "write_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

#define STACK_VADDR 0x8000200000ULL

void write_test_run(void) {
    // write_test.elf is the THIRD Multiboot2 module (index 2) --
    // hello.elf is index 0 (elftest), exit_test.elf is index 1
    // (syscalltest). See grub.cfg's three `module2` lines.
    struct multiboot_module_info mod;
    if (!multiboot_get_module(2, &mod)) {
        vga_write("writetest: no third Multiboot2 module found -- was\n");
        vga_write("userland/write_test.elf added as a GRUB module? (see\n");
        vga_write("grub.cfg's third `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("writetest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("writetest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("writetest: out of physical memory for the stack\n");
        return;
    }
    if (!vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("writetest: failed to map the stack page\n");
        return;
    }

    vga_write("Running the process -- the next line of output comes from\n");
    vga_write("ring 3 itself, via a real write syscall:\n");
    serial_write("write_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("write_test: process_run_ring3() returned\n");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\n(that line above was printed by the ring-3 process itself)\n");
    vga_write("Process exited cleanly. Exit code: ");
    vga_write_dec((uint32_t)exit_code);
    vga_putc('\n');
}
