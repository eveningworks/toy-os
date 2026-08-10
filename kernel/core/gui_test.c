#include "gui_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

#define STACK_VADDR 0x8000200000ULL

void gui_test_run(void) {
    // gui_test.elf is the FIFTH Multiboot2 module (index 4) -- see
    // grub.cfg's five `module2` lines.
    struct multiboot_module_info mod;
    if (!multiboot_get_module(4, &mod)) {
        vga_write("guitest: no fifth Multiboot2 module found -- was\n");
        vga_write("userland/gui_test.elf added as a GRUB module?\n");
        return;
    }

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("guitest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("guitest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys || !vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("guitest: failed to set up the stack\n");
        return;
    }

    vga_write("Handing the real screen to a ring-3 process. It'll fill the\n");
    vga_write("screen with a color; press any key to cycle colors, 'q' to\n");
    vga_write("return here.\n");
    serial_write("gui_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("gui_test: process_run_ring3() returned\n");

    // The process just drew directly over the real screen -- clear it
    // back to a normal console before printing anything further.
    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Back from the ring-3 GUI process. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_write("\n\nThis was modal -- the process had the whole real screen to\n");
    vga_write("itself while it ran. It's not a window inside the kernel-space\n");
    vga_write("window manager (try 'gui'); that's a separate, so-far-untouched\n");
    vga_write("piece of this project. See apps/README.md.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}
