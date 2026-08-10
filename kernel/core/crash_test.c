#include "crash_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

#define STACK_VADDR 0x8000200000ULL

void crash_test_run(void) {
    // crash_test.elf is the TWELFTH Multiboot2 module (index 11) -- see
    // grub.cfg's module2 lines: hello(0), exit_test(1), write_test(2),
    // write_bad_test(3), gui_test(4), counter_a(5), counter_b(6),
    // echo(7), win_test(8), file_test(9), newsyscalls_test(10),
    // crash_test(11).
    struct multiboot_module_info mod;
    if (!multiboot_get_module(11, &mod)) {
        vga_write("crashtest: no twelfth Multiboot2 module found -- was\n");
        vga_write("userland/crash_test.elf added as a GRUB module? (see\n");
        vga_write("grub.cfg's twelfth `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("crashtest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("crashtest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("crashtest: out of physical memory for the stack\n");
        return;
    }
    if (!vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("crashtest: failed to map the stack page\n");
        return;
    }

    vga_write("Running the process -- it deliberately faults (a wild-\n");
    vga_write("pointer write). Watch for the kernel to catch it, tear the\n");
    vga_write("process down, and hand control straight back here instead\n");
    vga_write("of halting (see userland/crash_test.c):\n\n");
    serial_write("crash_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("crash_test: process_run_ring3() returned\n");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\nBack in the shell -- process_run_ring3() returned. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_write("\n\n(CRASHED here means the fault was caught and the\n");
    vga_write("process was torn down cleanly -- no reboot needed. See\n");
    vga_write("userland/crash_test.c and idt.c's fault handler.)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}
