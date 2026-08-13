#include "ring3_test.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "idt.h"
#include "vga.h"
#include "klog.h"
#include <stddef.h>

// Physical address of the data frame, kept around so the fault-hook can
// read the marker back out after the fault. A physical address stays a
// valid pointer regardless of which process's CR3 is loaded, since every
// address space shares the kernel's identity-mapped view of the low
// 4GiB (see vmm.h) -- this is exactly the property that lets the fault
// handler keep working no matter what was running when the fault hit.
static uint64_t data_phys = 0;

static void on_ring3_fault(uint64_t vector, uint64_t error_code, uint64_t cs, uint64_t cr2) {
    (void)vector; (void)error_code; (void)cr2;

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("\nRing 3 isolation confirmed:\n");
    vga_write("  Fault CS was "); vga_write_hex(cs);
    vga_write(" -- RPL bits = "); vga_write_dec((uint32_t)(cs & 3));
    vga_write(", i.e. the faulting code was genuinely\n  running at ring 3 (user mode), not ring 0.\n\n");

    uint32_t marker = *(volatile uint32_t *)(uintptr_t)data_phys;
    vga_write("  Marker written by ring-3 code before the fault: ");
    vga_write_hex(marker);
    vga_write("\n");
    if (marker == 0xDEADBEEF) {
        vga_write("  -> matches 0xdeadbeef: the ring-3 code executed in its own\n");
        vga_write("     private address space and wrote to its own mapped memory\n");
        vga_write("     *before* hlt correctly trapped as a privilege violation.\n\n");
        klog_write("ring3_test: fault confirmed at ring 3, marker matched\n");
    } else {
        vga_write("  -> unexpected value -- something didn't run as intended.\n\n");
        klog_write("ring3_test: fault confirmed at ring 3, marker DID NOT match\n");
    }

    vmm_switch_address_space(vmm_kernel_pml4_phys());
    vga_write("(switched CR3 back to the kernel's own address space)\n\n");

    vga_write("This still halts here on purpose -- NOT because recovery doesn't\n");
    vga_write("exist any more (see `crashtest`, which now recovers cleanly): this\n");
    vga_write("test drops to ring 3 with its own raw, manual iretq instead of\n");
    vga_write("going through process_run_ring3(), so there's nowhere for the\n");
    vga_write("kernel to recover TO -- see process.h. Restart the OS (or the\n");
    vga_write("QEMU window) to use the shell again.\n");

    klog_write("ring3_test: halting after fault diagnostics\n");
}

void ring3_test_run(void) {
    vga_write("Allocating code+data+stack frames from the physical allocator...\n");

    uint64_t code_phys = pmm_alloc_frame();
    data_phys = pmm_alloc_frame();
    uint64_t stack_phys = pmm_alloc_frame();

    if (!code_phys || !data_phys || !stack_phys) {
        vga_write("ring3_test: pmm_alloc_frame() failed -- out of physical memory?\n");
        return;
    }

    vga_write("Creating a private address space for this process...\n");
    uint64_t as = vmm_create_address_space();
    if (!as) {
        vga_write("ring3_test: vmm_create_address_space() failed\n");
        return;
    }

    uint64_t code_vaddr  = VMM_USER_BASE + 0x0000;
    uint64_t data_vaddr  = VMM_USER_BASE + 0x1000;
    uint64_t stack_vaddr = VMM_USER_BASE + 0x2000;

    // The code page needs executable=1 explicitly (vmm_map_user_page()'s
    // plain wrapper defaults to non-executable, correct for data_vaddr/
    // stack_vaddr below but wrong for actual machine code -- see vmm.h).
    // Not writable either: nothing here self-modifies its own code.
    if (!vmm_map_user_page_flags(as, code_vaddr, code_phys, 0, 1) ||
        !vmm_map_user_page(as, data_vaddr, data_phys) ||
        !vmm_map_user_page(as, stack_vaddr, stack_phys)) {
        vga_write("ring3_test: vmm_map_user_page()/vmm_map_user_page_flags() failed\n");
        return;
    }

    vga_write("  code:  phys "); vga_write_hex(code_phys);
    vga_write("  -> virt "); vga_write_hex(code_vaddr); vga_putc('\n');
    vga_write("  data:  phys "); vga_write_hex(data_phys);
    vga_write("  -> virt "); vga_write_hex(data_vaddr); vga_putc('\n');
    vga_write("  stack: phys "); vga_write_hex(stack_phys);
    vga_write("  -> virt "); vga_write_hex(stack_vaddr); vga_putc('\n');
    vga_write("(phys and virt differ -- this is real address translation,\n");
    vga_write(" not the identity-mapping shortcut the original Milestone 8 relied on)\n\n");

    // Write the code bytes and patch in the address the ring-3 code will
    // actually use -- the VIRTUAL address, since that's all it can see
    // through its own (private) page tables. Still done via the kernel's
    // own identity-mapped view of these physical frames, since CR3
    // hasn't switched to the process yet.
    uint8_t *code_page = (uint8_t *)(uintptr_t)code_phys;
    uint8_t *data_page = (uint8_t *)(uintptr_t)data_phys;
    for (size_t i = 0; i < 4096; i++) data_page[i] = 0;

    // Hand-encoded x86-64 machine code (no toolchain needed for a program
    // this small):
    //   48 BB <8 bytes>      movabs rbx, <patched below: data_vaddr>
    //   C7 03 EF BE AD DE    mov dword [rbx], 0xDEADBEEF
    //   F4                   hlt   -- privileged; ring 3 cannot execute this
    static const uint8_t program[] = {
        0x48, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
        0xC7, 0x03, 0xEF, 0xBE, 0xAD, 0xDE,
        0xF4,
    };
    for (size_t i = 0; i < sizeof(program); i++) code_page[i] = program[i];
    for (int i = 0; i < 8; i++) code_page[2 + i] = (uint8_t)(data_vaddr >> (8 * i));

    idt_set_ring3_fault_hook(on_ring3_fault);

    vga_write("Switching CR3 to the process's private address space...\n");
    vmm_switch_address_space(as);

    vga_write("Dropping to ring 3 (iretq)...\n");
    klog_write("ring3_test: about to iretq to user mode in a private address space\n");

    uint64_t user_rip = code_vaddr;
    uint64_t user_rsp = stack_vaddr + 4096;

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

    // Unreachable: iretq transferred control to ring 3, and that code's
    // hlt faults before it could ever return here.
    vga_write("ring3_test: unexpectedly returned -- this should be unreachable\n");
}
