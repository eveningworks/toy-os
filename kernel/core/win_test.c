#include "win_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "multiboot.h"
#include "vga.h"
#include "serial.h"

#define STACK_VADDR 0x8000200000ULL

void win_test_run(void) {
    // win_test.elf is the NINTH Multiboot2 module (index 8) -- see
    // grub.cfg's module2 lines: hello(0), exit_test(1), write_test(2),
    // write_bad_test(3), gui_test(4), counter_a(5), counter_b(6),
    // echo(7), win_test(8).
    struct multiboot_module_info mod;
    if (!multiboot_get_module(8, &mod)) {
        vga_write("wintest: no ninth Multiboot2 module found -- was\n");
        vga_write("userland/win_test.elf added as a GRUB module? (see\n");
        vga_write("grub.cfg's ninth `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("wintest: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("wintest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys || !vmm_map_user_page(as, STACK_VADDR, stack_phys)) {
        vga_write("wintest: failed to set up the stack\n");
        return;
    }

    vga_write("Handing a private pixel buffer (not the real screen) to a\n");
    vga_write("ring-3 process -- the kernel composites it into a real,\n");
    vga_write("chrome'd window (title bar + close button, drawn but not\n");
    vga_write("yet mouse-clickable -- there's no mouse syscall). Any key\n");
    vga_write("cycles its color, Esc or 'q' returns here.\n\n");
    serial_write("win_test: calling process_run_ring3()\n");

    int exit_code = process_run_ring3(as, entry, STACK_VADDR + 4096);

    serial_write("win_test: process_run_ring3() returned\n");

    // Same as gui_test_run(): the window's content was drawn straight
    // onto the real screen by SYS_WIN_PRESENT (kernel-side), so this
    // console needs a fresh screen before printing anything further.
    vga_clear();
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Back from the ring-3 window process. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_write("\n\nThe process only ever drew into its own private buffer --\n");
    vga_write("it never had the real framebuffer mapped into its address\n");
    vga_write("space at all (see SYS_WIN_CREATE/SYS_WIN_PRESENT in\n");
    vga_write("syscall_abi.h). Still modal, though, same as guitest: not a\n");
    vga_write("window inside the kernel-space window manager ('gui'). See\n");
    vga_write("README's \"Ideas for what's next\".\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}
