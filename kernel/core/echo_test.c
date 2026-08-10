#include "echo_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "syscall.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

#define STACK_VADDR 0x8000200000ULL

// Well clear of both the tiny ELF's own segments (near VMM_USER_BASE)
// and the stack above -- SYS_SBRK grows upward from here one page at a
// time as echo.c asks for more (see syscall.c).
#define HEAP_VADDR 0x8000100000ULL

void echo_test_run(void) {
    // echo.elf is the EIGHTH Multiboot2 module (index 7) -- see
    // grub.cfg's module2 lines: hello(0), exit_test(1), write_test(2),
    // write_bad_test(3), gui_test(4), counter_a(5), counter_b(6),
    // echo(7).
    struct multiboot_module_info mod;
    if (!multiboot_get_module(7, &mod)) {
        vga_write("echotest: no eighth Multiboot2 module found -- was\n");
        vga_write("userland/echo.elf added as a GRUB module? (see grub.cfg's\n");
        vga_write("eighth `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("echotest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("echotest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("echotest: out of physical memory for the stack\n");
        return;
    }
    if (!vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("echotest: failed to map the stack page\n");
        return;
    }

    // Arm SYS_SBRK for this process before it starts -- see syscall.h.
    // No pages are actually allocated here; echo.c's first sbrk() call
    // is what triggers the first page to be mapped.
    syscall_reset_heap(as, HEAP_VADDR);

    vga_write("Running echo -- type to see it echoed back by the process\n");
    vga_write("itself (via SYS_READ_KEY + SYS_WRITE), Esc to quit:\n\n");
    serial_write("echo_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("echo_test: process_run_ring3() returned\n");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\nProcess finished. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_putc('\n');
}
