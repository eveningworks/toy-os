#include "elf_test.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"
#include "idt.h"
#include "multiboot.h"
#include "userland_contract.h"
#include "vga.h"
#include "serial.h"
#include <stddef.h>

// Where the test's private stack lives -- chosen well clear of
// USERLAND_MARKER_ADDR and of wherever the tiny ELF's own segments land
// (both close to VMM_USER_BASE), so there's no risk of overlap.
#define ELF_TEST_STACK_VADDR 0x8000200000ULL

static void on_elf_fault(uint64_t vector, uint64_t error_code, uint64_t cs, uint64_t cr2) {
    (void)vector; (void)error_code; (void)cr2;

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("\nELF-loaded ring-3 program ran and faulted as expected:\n");
    vga_write("  CS = "); vga_write_hex(cs);
    vga_write(" (ring "); vga_write_dec((uint32_t)(cs & 3)); vga_write(")\n");

    // The fault handler runs with whatever CR3 was active when the
    // fault happened -- we haven't switched back to the kernel's own
    // address space yet, so this is still the process's private one,
    // and USERLAND_MARKER_ADDR (mapped there, separately from the ELF's
    // own segments) is readable exactly as the ring-3 code left it.
    uint32_t marker = *(volatile uint32_t *)USERLAND_MARKER_ADDR;
    vga_write("  Marker written by the ELF binary: "); vga_write_hex(marker);
    vga_write("\n");
    if (marker == 0xC0FFEE) {
        vga_write("  -> matches 0xc0ffee: a real compiled+linked ELF64 binary was\n");
        vga_write("     loaded via its program headers and executed correctly.\n\n");
        serial_write("elf_test: fault confirmed at ring 3, marker matched\n");
    } else {
        vga_write("  -> unexpected value -- something didn't run as intended.\n\n");
        serial_write("elf_test: fault confirmed at ring 3, marker DID NOT match\n");
    }

    vmm_switch_address_space(vmm_kernel_pml4_phys());
    vga_write("(switched CR3 back to the kernel's own address space)\n\n");

    vga_write("This still halts here for the same reason ring3test does (see its\n");
    vga_write("comment): this test drops to ring 3 with its own raw, manual iretq\n");
    vga_write("instead of going through process_run_ring3(), so there's nowhere\n");
    vga_write("for the kernel to recover TO -- crash recovery does exist now for\n");
    vga_write("processes that DO go through it (see `crashtest`). Restart the OS\n");
    vga_write("(or the QEMU window) to use the shell again.\n");

    serial_write("elf_test: halting after fault diagnostics\n");
}

void elf_test_run(void) {
    struct multiboot_module_info mod;
    if (!multiboot_get_module(0, &mod)) {
        vga_write("elftest: no Multiboot2 module found -- was userland/hello.elf\n");
        vga_write("added as a GRUB module? (see grub.cfg's `module2` line)\n");
        return;
    }
    vga_write("Found ELF module: "); vga_write_hex(mod.start);
    vga_write(" - "); vga_write_hex(mod.end); vga_putc('\n');

    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("elftest: vmm_create_address_space() failed\n");
        return;
    }

    vga_write("Parsing ELF headers and loading PT_LOAD segments...\n");
    uint64_t entry = 0;
    if (!elf_load(mod.start, as, &entry)) {
        vga_write("elftest: elf_load() failed -- bad ELF, or out of memory\n");
        return;
    }
    vga_write("  entry point: "); vga_write_hex(entry); vga_putc('\n');

    uint64_t marker_phys = pmm_alloc_frame();
    uint64_t stack_phys = pmm_alloc_frame();
    if (!marker_phys || !stack_phys) {
        vga_write("elftest: out of physical memory for marker/stack pages\n");
        return;
    }
    for (size_t i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)marker_phys)[i] = 0;

    if (!vmm_map_user_page(as, USERLAND_MARKER_ADDR, marker_phys) ||
        !vmm_map_user_page(as, ELF_TEST_STACK_VADDR, stack_phys)) {
        vga_write("elftest: failed to map marker/stack pages\n");
        return;
    }

    idt_set_ring3_fault_hook(on_elf_fault);

    vga_write("Switching CR3 and dropping to ring 3 at the ELF's entry point...\n");
    serial_write("elf_test: about to iretq into a loaded ELF binary\n");
    vmm_switch_address_space(as);

    uint64_t user_rip = entry;
    uint64_t user_rsp = ELF_TEST_STACK_VADDR + 4096;

    __asm__ volatile (
        "mov %0, %%rax\n\t"
        "push %%rax\n\t"          // SS
        "push %1\n\t"             // RSP
        "pushfq\n\t"              // RFLAGS
        "orq $0x200, (%%rsp)\n\t" // make sure IF is set
        "mov %2, %%rax\n\t"
        "push %%rax\n\t"          // CS
        "push %3\n\t"             // RIP
        "iretq\n\t"
        :
        : "i"(SEL_USER_DATA), "r"(user_rsp), "i"(SEL_USER_CODE), "r"(user_rip)
        : "rax", "memory"
    );

    vga_write("elf_test: unexpectedly returned -- this should be unreachable\n");
}
