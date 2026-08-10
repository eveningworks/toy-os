#include "syscall_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

// Separate from USERLAND_MARKER_ADDR (used by hello.c/ring3_test's-style
// tests) since this test's stack lives in its own address space anyway
// and doesn't need to coordinate with anything else.
#define STACK_VADDR 0x8000200000ULL

void syscall_test_run(void) {
    // exit_test.elf is the SECOND Multiboot2 module (index 1) --
    // hello.elf (used by elftest) is index 0. See grub.cfg's two
    // `module2` lines.
    struct multiboot_module_info mod;
    if (!multiboot_get_module(1, &mod)) {
        vga_write("syscalltest: no second Multiboot2 module found -- was\n");
        vga_write("userland/exit_test.elf added as a GRUB module? (see\n");
        vga_write("grub.cfg's second `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("syscalltest: vmm_create_address_space() failed\n");
        return;
    }

    vga_write("Parsing ELF headers and loading PT_LOAD segments...\n");
    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("syscalltest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }
    vga_write("  entry point: "); vga_write_hex(entry); vga_putc('\n');

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) {
        vga_write("syscalltest: out of physical memory for the stack\n");
        return;
    }
    if (!vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("syscalltest: failed to map the stack page\n");
        return;
    }

    vga_write("Running the process (via a real int 0x80 exit syscall this\n");
    vga_write("time, not a deliberate fault)...\n");
    serial_write("syscall_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("syscall_test: process_run_ring3() returned\n");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\nThe process finished (via exit syscall, or a caught fault). Exit code: ");
    vga_write_exit_code(exit_code);
    vga_write("\n\nNo fault, no halt -- control came straight back here, same\n");
    vga_write("as an ordinary function call would, and the shell keeps\n");
    vga_write("running normally from this point on.\n");
}
