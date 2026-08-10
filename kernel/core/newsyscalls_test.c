#include "newsyscalls_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "klog.h"

#define STACK_VADDR 0x8000200000ULL

void newsyscalls_test_run(void) {
    // newsyscalls_test.elf is the ELEVENTH Multiboot2 module (index 10)
    // -- see grub.cfg's module2 lines: hello(0), exit_test(1),
    // write_test(2), write_bad_test(3), gui_test(4), counter_a(5),
    // counter_b(6), echo(7), win_test(8), file_test(9),
    // newsyscalls_test(10).
    struct multiboot_module_info mod;
    if (!multiboot_get_module(10, &mod)) {
        vga_write("newsyscallstest: no eleventh Multiboot2 module found\n");
        vga_write("-- was userland/newsyscalls_test.elf added as a GRUB\n");
        vga_write("module? (see grub.cfg's eleventh `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("newsyscallstest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("newsyscallstest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("newsyscallstest: out of physical memory for the stack\n");
        return;
    }
    if (!vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("newsyscallstest: failed to map the stack page\n");
        return;
    }

    vga_write("Running the process -- it exercises SYS_UNLINK, SYS_LISTDIR,\n");
    vga_write("SYS_GETTIME, and SYS_YIELD in turn (see\n");
    vga_write("userland/newsyscalls_test.c):\n\n");
    klog_write("newsyscalls_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    klog_write("newsyscalls_test: process_run_ring3() returned\n");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\nProcess finished. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_write("\n\n(exit code 0 means every phase passed -- see\n");
    vga_write("userland/newsyscalls_test.c)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}
