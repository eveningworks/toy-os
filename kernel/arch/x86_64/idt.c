#include "idt.h"
#include "vga.h"
#include "klog.h"
#include "kfmt.h"
#include "pic.h"
#include "irq.h"
#include "keyboard.h"
#include "i8042.h"
#include "timer.h"
#include "string.h"
#include "mouse.h"
#include "syscall.h"
#include "scheduler.h"
#include "vmm.h"
#include "reloc.h" // kernel_reloc_delta() -- a panic RIP is meaningless without it
#include "process.h"
#include "uaddr.h" // uaddr_is_stack_guard() -- naming a stack overflow as one
#include "ksyms.h" // a panic names the function, not just an address
#include "version.h" // TOYOS_VERSION_FULL -- a photographed panic identifies its build
#include "clocksource.h" // uptime, so a panic says WHEN

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

// The three hardware-IRQ handlers this kernel has always had, now
// registered through irq.c's generic table (see irq.h's top comment)
// instead of being hardcoded branches in isr_dispatch() below -- one
// uniform dispatch path for every IRQ, this file just wires up which
// function handles which line, the same way the pic_clear_mask() calls
// in idt_init() already wire up which lines are even unmasked.
static void timer_irq_handler(uint64_t *regs) {
    pit_handle_irq();
    scheduler_tick(regs); // no-op unless scheduler_demo_run() armed it
}

static void keyboard_irq_handler(uint64_t *regs) {
    (void)regs;
    // Keyboard and mouse share the 8042 data port, so both IRQs go
    // through the same routing poll -- see i8042.h.
    i8042_poll();
}

static void mouse_irq_handler(uint64_t *regs) {
    (void)regs;
    i8042_poll();
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
    pit_init(PIT_HZ);

    irq_register_handler(0, timer_irq_handler);
    irq_register_handler(1, keyboard_irq_handler);
    irq_register_handler(12, mouse_irq_handler);

    // unmask timer (IRQ0), keyboard (IRQ1), cascade (IRQ2, needed for
    // any slave-PIC line to reach the CPU), and mouse (IRQ12)
    for (uint8_t irq = 0; irq < 16; irq++) pic_set_mask(irq);
    pic_clear_mask(0);
    pic_clear_mask(1);
    pic_clear_mask(2);
    pic_clear_mask(12);

    __asm__ volatile ("sti");
}

// See idt.h's doc comment. Not itself part of the reentrancy hazard --
// just a plain counter -- but see isr_reset_depth() for the one case
// (process.c's longjmp-style process teardown) where a decrement below
// never runs and this needs forcing back to 0 from outside.
static volatile int g_isr_depth = 0;

int isr_in_progress(void) {
    return g_isr_depth > 0;
}

void isr_reset_depth(void) {
    g_isr_depth = 0;
}

// Called from isr.asm's common stub with rdi = pointer to saved GP regs.
// Stack layout above saved regs (low->high addr): vector, error_code, then
// the CPU-pushed iretq frame (rip, cs, rflags, rsp, ss).
extern char __ktext_start[];
extern char __ktext_end[];

// What a panic needs to be diagnosable from a pasted log rather than a
// photograph, printed to the SERIAL log (klog) where it can be copied.
//
// **The relocation offset is the load-bearing line.** The kernel moves
// itself to a random base at boot, so a raw RIP means nothing on its
// own -- resolving one used to mean scrolling back to the boot banner
// and doing the subtraction by hand. It prints the link-time address
// here, ready to paste into `addr2line -f -e build/kernel.bin`.
//
// The backtrace is a STACK SCAN, not a frame-pointer walk: this kernel
// builds at -O2, which omits frame pointers, so an RBP chain would be
// fiction. Scanning for values that land inside .text overreports --
// stale return addresses from earlier calls are still down there -- and
// that is the honest trade, because the alternative is nothing at all.
// Read the list as candidates, most recent first, not as a call chain.
static void panic_report_context(uint64_t rip, const uint64_t *regs) {
    uint64_t delta = kernel_reloc_delta();
    uint64_t tstart = (uint64_t)(uintptr_t)__ktext_start;
    uint64_t tend   = (uint64_t)(uintptr_t)__ktext_end;

    if (delta) {
        klog_printf("  kernel relocated +0x%lx -- link-time RIP = 0x%lx\n",
                     delta, rip - delta);
    }
    // The name in the LOG too, not only on screen: the log is what gets
    // pasted into a report, and an address alone was the whole problem.
    uint32_t rip_off = 0;
    const char *rip_sym = ksyms_lookup(rip, &rip_off);
    if (rip_sym) klog_printf("  in %s+0x%x\n", rip_sym, rip_off);
    klog_printf("  resolve with: addr2line -f -e build/kernel.bin 0x%lx\n",
                 rip - delta);

    uint64_t rsp = regs[20];
    // Only walk a stack that could plausibly be one. A wild RSP is
    // exactly what some faults leave behind, and faulting again inside
    // the panic handler loses the report entirely -- which is the one
    // outcome worse than a missing backtrace.
    if (rsp < 0x1000 || rsp >= 0x100000000ULL || (rsp & 7)) {
        klog_printf("  no backtrace: RSP=0x%lx is not a walkable stack\n", rsp);
        return;
    }

    klog_printf("  stack scan from RSP=0x%lx (candidates, newest first):\n", rsp);
    const uint64_t *sp = (const uint64_t *)(uintptr_t)rsp;
    int shown = 0;
    for (int i = 0; i < 128 && shown < 12; i++) {
        uint64_t v = sp[i];
        if (v < tstart || v >= tend) continue;
        uint32_t off = 0;
        const char *nm = ksyms_lookup(v, &off);
        if (nm) klog_printf("    [%d] %s+0x%x  (0x%lx)\n", i, nm, off, v - delta);
        else    klog_printf("    [%d] 0x%lx  -> 0x%lx\n", i, v, v - delta);
        shown++;
    }
    if (!shown) klog_printf("    (nothing in .text found on the stack)\n");
}

void isr_dispatch(uint64_t *regs) {
    uint64_t vector = regs[15];
    g_isr_depth++; // see idt.h's isr_in_progress() -- every normal-return
                    // path below must decrement this to match; a
                    // noreturn path (process_context_exit()/recover())
                    // instead relies on isr_reset_depth() at the one
                    // point that longjmp lands.

    // Default: resume exactly what was interrupted. Only scheduler_tick()
    // below (and scheduler_on_exit(), called from syscall.c) ever
    // override this -- and only when the scheduler is armed. See
    // scheduler.c's design comment.
    g_next_kernel_rsp = (uint64_t)regs;

    if (vector >= 32 && vector < 48) {
        // Every hardware IRQ (timer, keyboard, mouse, and -- once a
        // driver registers one -- anything else, a NIC chief among
        // them) goes through the same generic lookup-call-EOI path now,
        // instead of a hardcoded if/else chain with a special case per
        // line. See irq.h/irq.c for the registration table, and
        // idt_init() just above for which function is registered for
        // which line -- timer_irq_handler() (IRQ0) is the one that
        // still calls scheduler_tick(), just from inside its own
        // registered handler now rather than as a separate line here.
        irq_dispatch((uint8_t)(vector - 32), regs);
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

        // This block was eleven calls to print three lines before
        // kfmt.h existed. It formats into a stack buffer, which is fine
        // in an exception handler -- KFMT_LINE_MAX is 256 bytes and this
        // is the deepest the fault path ever goes.
        // A ring-3 #PF in the unmapped guard region below the user
        // stack is a stack overflow, and saying so is the whole point
        // of reserving that region: the fault happened either way, but
        // "Page fault, CR2=0x80001fc..." points at nothing, and the
        // last time a client outgrew its stack the symptom was exactly
        // that. Ring 0 is excluded because the kernel's own stacks are
        // elsewhere entirely (scheduler.c's per-process kstacks), so a
        // supervisor fault at this address is a wild pointer, not an
        // overflow, and mislabelling it would be worse than not
        // labelling it.
        int stack_overflow = vector == 14 && (cs & 3) == 3 &&
                              uaddr_is_stack_guard(cr2);

        vga_set_color(VGA_WHITE, VGA_RED);
        vga_printf("\n*** %s%s ***\n",
                    recoverable ? "RING-3 PROCESS CRASHED: " : "KERNEL PANIC: ",
                    stack_overflow ? "Stack overflow" : exception_names[vector]);
        // Name the function. The whole reason the symbol table is baked
        // into the image (linker.ld's .ksyms, tools/gen_syms.py): a bare
        // address is unreadable on the machine it happened on, because
        // the kernel relocated itself to a random base at boot.
        uint32_t sym_off = 0;
        const char *sym = ksyms_lookup(rip, &sym_off);
        if (sym) vga_printf("in %s+0x%x\n", sym, sym_off);
        vga_printf("RIP=0x%lx  CS=0x%lx (ring %lu)\n", rip, cs, cs & 3);
        if (vector == 14) {
            vga_printf("error_code=0x%lx  CR2=0x%lx\n", error_code, cr2);
        } else {
            vga_printf("error_code=0x%lx\n", error_code);
        }
        if (stack_overflow) {
            vga_printf("Ran off the bottom of the user stack "
                       "(0x%lx..0x%lx) into its guard page.\n",
                       (uint64_t)UADDR_STACK_BOTTOM,
                       (uint64_t)UADDR_STACK_VADDR + 4096);
        }

        // WHO was running, which is the line that would have pointed
        // straight at a WM/ring-3 interleaving bug this kernel spent a
        // session on. `pid 0` means the kernel context itself -- the
        // scheduler participant the desktop runs in, not "no process".
        int pid = scheduler_current_pid();
        if (pid) vga_printf("context: pid %d\n", pid);
        else     vga_printf("context: kernel context (no process)\n");

        // Registers. A wild pointer is usually sitting in one of them,
        // and on a #GP there is no CR2 to consult.
        // isr.asm pushes rax..r15 in that order and the stack grows
        // DOWN, so the last push (r15) is regs[0] and rax is regs[14].
        // Worth stating: the obvious reading of the push list is exactly
        // backwards, and six of these eight were wrong the first time.
        vga_printf("RAX=%lx RBX=%lx RCX=%lx RDX=%lx\n",
                    regs[14], regs[13], regs[12], regs[11]);
        vga_printf("RSI=%lx RDI=%lx RBP=%lx RSP=%lx\n",
                    regs[10], regs[9], regs[8], regs[20]);

        // Build and uptime, so a photograph identifies itself: matching a
        // panic against the wrong build is a whole wasted round trip.
        vga_printf("%s  up %lus\n", TOYOS_VERSION_FULL,
                    (unsigned long)(clocksource_now_ns() / 1000000000ULL));

        klog_printf("%s%s\n", recoverable ? "RING-3 CRASH: " : "PANIC: ",
                     stack_overflow ? "Stack overflow" : exception_names[vector]);
        // Everything above went to the SCREEN only, which is why a panic
        // used to arrive as a photograph. The serial log is where a
        // report can actually be pasted from, so it gets the same facts.
        klog_printf("  RIP=0x%lx  CS=0x%lx (ring %lu)  error_code=0x%lx\n",
                     rip, cs, cs & 3, error_code);
        if (vector == 14) klog_printf("  CR2=0x%lx\n", cr2);
        // Ring 0 only. A ring-3 fault's RIP is an address in some
        // userland ELF, so resolving it against the kernel image would
        // be confidently wrong -- and its stack is a user mapping this
        // has no business walking.
        if ((cs & 3) == 0) panic_report_context(rip, regs);

        // The console draws into a back buffer and normally publishes
        // from the keyboard's idle loop (see vga.h's vga_present()). A
        // kernel panic never reaches that loop -- it halts below -- so
        // without this the banner reporting the panic is the one thing
        // that never makes it to the screen.
        vga_present();

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
                g_isr_depth--; // matches this call's own increment above -- see isr_in_progress()
                scheduler_on_exit(-1);
                return; // g_next_kernel_rsp now points elsewhere; isr_common's epilogue resumes it
            } else {
                process_context_recover(); // never returns
            }
        }

        for (;;) __asm__ volatile ("cli; hlt");
    }

    g_isr_depth--; // matches this call's own increment above -- see
                    // isr_in_progress(). NOT reached if syscall_dispatch()
                    // above took the noreturn process_context_exit() path
                    // (legacy SYS_EXIT) -- isr_reset_depth() covers that.
}
