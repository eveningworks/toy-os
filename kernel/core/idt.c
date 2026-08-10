#include "idt.h"
#include "vga.h"
#include "klog.h"
#include "pic.h"
#include "keyboard.h"
#include "i8042.h"
#include "timer.h"
#include "string.h"
#include "mouse.h"
#include "syscall.h"
#include "scheduler.h"
#include "vmm.h"
#include "process.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

#define KERNEL_CODE_SEGMENT 0x08

extern void idt_load(uint64_t idt_ptr_addr);

// Assembly ISR stub entry points (defined in isr.asm)
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void); extern void isr35(void);
extern void isr36(void); extern void isr37(void); extern void isr38(void); extern void isr39(void);
extern void isr40(void); extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void); extern void isr47(void);

// Syscall entry (int 0x80) -- see isr.asm's comment there for why it's
// separate from the vectors 0-47 above.
extern void isr128(void);

void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low = addr & 0xFFFF;
    idt[vector].selector = KERNEL_CODE_SEGMENT;
    idt[vector].ist = ist;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero = 0;
}

static void (*const isr_table[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static const char *exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FP exception", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Security", "Reserved",
};

static ring3_fault_hook_fn ring3_hook = 0;

// Read by isr.asm's isr_common epilogue, reloaded into rsp right before
// the pop+iretq sequence -- see scheduler.c's design comment for the
// full explanation. isr_dispatch resets this to `regs` (a no-op) at the
// top of every call; only scheduler.c ever points it elsewhere.
uint64_t g_next_kernel_rsp;

void idt_set_ring3_fault_hook(ring3_fault_hook_fn hook) {
    ring3_hook = hook;
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_table[i], 0, 0x8E); // present, ring0, 64-bit interrupt gate
    }

    // Syscall gate needs DPL=3 (0xEE, not 0x8E) -- otherwise ring-3 code
    // executing `int 0x80` gets a #GP instead of reaching the handler,
    // since a software interrupt's DPL is the *minimum* privilege
    // allowed to invoke it via the `int` instruction.
    idt_set_gate(128, isr128, 0, 0xEE);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    idt_load((uint64_t)&idtp);

    pic_remap();
    pit_init(100); // 100 Hz tick

    // unmask timer (IRQ0), keyboard (IRQ1), cascade (IRQ2, needed for
    // any slave-PIC line to reach the CPU), and mouse (IRQ12)
    for (uint8_t irq = 0; irq < 16; irq++) pic_set_mask(irq);
    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);
    pic_clear_mask(12);

    __asm__ volatile ("sti");
}

// Called from isr.asm's common stub with rdi = pointer to saved GP regs.
// Stack layout above saved regs (low->high addr): vector, error_code, then
// the CPU-pushed iretq frame (rip, cs, rflags, rsp, ss).
void isr_dispatch(uint64_t *regs) {
    uint64_t vector = regs[15];

    // Default: resume exactly what was interrupted. Only scheduler_tick()
    // below (and scheduler_on_exit(), called from syscall.c) ever
    // override this -- and only when the scheduler is armed. See
    // scheduler.c's design comment.
    g_next_kernel_rsp = (uint64_t)regs;

    if (vector == 32) {
        pit_handle_irq();
        pic_send_eoi(0);
        scheduler_tick(regs); // no-op unless scheduler_demo_run() armed it
    } else if (vector == 33) {
        // Keyboard and mouse share the 8042 data port, so both IRQs go
        // through the same routing poll -- see i8042.h.
        i8042_poll();
        pic_send_eoi(1);
    } else if (vector == 44) {
        i8042_poll();
        pic_send_eoi(12);
    } else if (vector == 128) {
        // Software interrupt from ring 3 (int 0x80) -- not a hardware
        // IRQ, so no PIC EOI. syscall_dispatch() may not return (see
        // syscall.h): the exit syscall jumps straight back into
        // whichever kernel code called process_run_ring3() instead of
        // rejoining isr_common's normal epilogue below.
        syscall_dispatch(regs);
    } else if (vector < 32) {
        uint64_t error_code = regs[16];
        uint64_t rip = regs[17];
        uint64_t cs = regs[18];
        uint64_t cr2 = 0;
        if (vector == 14) { // page fault: CR2 holds the faulting address
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        }

        // A ring-3 fault is recoverable -- tear the process down and
        // hand control to whatever runs next -- only if there's
        // actually somewhere to hand it TO: a scheduler-managed process
        // (scheduler_on_exit() already knows how to pick the next one),
        // or a process_run_ring3() call with its context still armed
        // (process.h). ring3_test.c/elf_test.c's deliberate faults are
        // neither -- they drop to ring 3 with their own raw, manual
        // iretq, not through process_run_ring3() -- so they still fall
        // through to the unconditional halt below, exactly as before:
        // there's nowhere for them to recover TO. A ring-0 (kernel-mode)
        // fault always falls through too -- a kernel bug staying fatal
        // is correct, not a gap this closes.
        int recoverable = (cs & 3) == 3 &&
                           (scheduler_current_pid() || process_context_is_armed());

        vga_set_color(VGA_WHITE, VGA_RED);
        vga_write("\n*** ");
        vga_write(recoverable ? "RING-3 PROCESS CRASHED: " : "KERNEL PANIC: ");
        vga_write(exception_names[vector]);
        vga_write(" ***\n");
        vga_write("RIP="); vga_write_hex(rip);
        vga_write("  CS="); vga_write_hex(cs);
        vga_write(" (ring "); vga_write_dec((uint32_t)(cs & 3)); vga_write(")\n");
        vga_write("error_code="); vga_write_hex(error_code);
        if (vector == 14) { vga_write("  CR2="); vga_write_hex(cr2); }
        vga_putc('\n');

        klog_write(recoverable ? "RING-3 CRASH: " : "PANIC: ");
        klog_write(exception_names[vector]);
        klog_write("\n");

        if ((cs & 3) == 3 && ring3_hook) {
            ring3_hook(vector, error_code, cs, cr2);
        }

        if (recoverable) {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            vga_write("Process torn down (address space + fds/heap/window\n");
            vga_write("state freed) -- ");
            vga_write(scheduler_current_pid()
                          ? "handing the CPU to the next ready process.\n\n"
                          : "returning control to whatever ran it.\n\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

            syscall_process_exit_cleanup(vmm_current_pml4());

            if (scheduler_current_pid()) {
                // Same "crashed, no real exit code to report" case
                // scheduler_on_exit()'s own comment already covers --
                // nothing consumes the code today either way.
                scheduler_on_exit(-1);
                return; // g_next_kernel_rsp now points elsewhere; isr_common's epilogue resumes it
            } else {
                process_context_recover(); // never returns
            }
        }

        for (;;) __asm__ volatile ("cli; hlt");
    } else if (vector < 48) {
        pic_send_eoi((uint8_t)(vector - 32));
    }
}
