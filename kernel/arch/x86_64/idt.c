#include "idt.h"
#include "vga.h"
#include "klog.h"
#include "kfmt.h"
#include "pic.h"
#include "irq.h"
#include "lapic.h"
#include "keyboard.h"
#include "i8042.h"
#include "timer.h"
#include "clockevent.h" // the tick, whichever device is delivering it
#include "string.h"
#include "mouse.h"
#include "syscall.h"
#include "signal.h" // signal_deliver_pending()/_fault() -- see isr_dispatch
#include "signal_abi.h" // SIGSEGV/SIGILL/SIGFPE -- fault_signal() below
#include "scheduler.h"
#include "paging.h"   // paging_kernel_leaf() -- is this stack page mapped?
#include "vmm.h"
#include "reloc.h" // kernel_reloc_delta() -- a panic RIP is meaningless without it
#include "process.h"
#include "crash_report.h" // crash_report_write() -- the ring-3 report
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
// The MSI vectors (kernel/lapic.h's LAPIC_VECTOR_BASE..LAST) and the
// LAPIC's spurious vector.
extern void isr48(void); extern void isr49(void); extern void isr50(void); extern void isr51(void);
extern void isr52(void); extern void isr53(void); extern void isr54(void); extern void isr55(void);
extern void isr56(void); extern void isr57(void); extern void isr58(void); extern void isr59(void);
extern void isr60(void); extern void isr61(void); extern void isr62(void); extern void isr63(void);
extern void isr64(void);
extern void isr65(void);
extern void isr66(void);
extern void isr67(void);
extern void isr68(void);
extern void isr69(void);
extern void isr70(void);
extern void isr71(void);
extern void isr255(void);

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

static void (*const isr_table[72])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
    isr48, isr49, isr50, isr51, isr52, isr53, isr54, isr55,
    isr56, isr57, isr58, isr59, isr60, isr61, isr62, isr63,
    isr64, isr65, isr66, isr67, isr68, isr69, isr70, isr71,
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

// Where isr_common's epilogue should resume. **THE VALUE IS A LOCAL OF
// THE LIVE isr_dispatch() CALL; this only points at it.** A plain global
// holding the value cannot work once syscalls are preemptible: the
// wrapper reads it AFTER the body, and a body that switches away is
// resumed only once some other wrapper has restored its own saved copy
// -- so the read is not this call's value (the outermost restores the
// initial 0, which reached isr_common as `mov rsp, rax` with zero).
//
// The pointer travels with the context, like isr_depth beside it: the
// scheduler saves and restores it whenever it saves `kernel_rsp`, so a
// switch_to() on a resumed context writes into ITS dispatch's local and
// not a parked one's. scheduler.c reaches it through isr_resume_set().
static uint64_t *g_resume_slot;

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
    clockevent_tick(regs);
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

// 0xEE is an interrupt gate (IF clear for the whole syscall), 0xEF a
// trap gate. See the comment at the call site.
#define SYSCALL_GATE 0xEE

void idt_init(void) {
    for (int i = 0; i < 72; i++) {
        idt_set_gate(i, isr_table[i], 0, 0x8E); // present, ring0, 64-bit interrupt gate
    }

    // The LAPIC's spurious vector. Installed whether or not the LAPIC is
    // ever enabled: a gate costs nothing and a vector delivered without
    // one is a #GP on top of whatever already went wrong.
    idt_set_gate(LAPIC_SPURIOUS_VECTOR, isr255, 0, 0x8E);

    // The DOUBLE FAULT gate runs on IST slot 1 (gdt.c's df_stack). It
    // is the ONLY gate that needs one, and it needs it for one reason:
    // a kernel stack overflow. RSP walks onto a stack's guard page, the
    // push faults, and delivering that #PF has to push onto the same
    // broken stack -- so without a known-good stack to switch to, the
    // CPU triple-faults and the machine reboots with nothing printed.
    idt_set_gate(8, isr_table[8], 1, 0x8E);

    // Syscall gate needs DPL=3 (0xEF, not 0x8F) -- otherwise ring-3 code
    // executing `int 0x80` gets a #GP instead of reaching the handler,
    // since a software interrupt's DPL is the *minimum* privilege
    // allowed to invoke it via the `int` instruction.
    //
    // **STILL AN INTERRUPT GATE (0xEE), AND THE TRAP GATE (0xEF) IS ONE
    // LINE AWAY -- it was tried, and it is not ready.** The one bit is
    // whether IF survives the syscall: an interrupt gate clears it for
    // the whole call, so nothing (timer, keyboard, mouse) is serviced
    // until it returns. Measured: one write(2) holds the CPU 19.6 ms
    // that way, and compositor wake latency goes 2.3 ms -> 12.2 ms
    // under disk load.
    //
    // **FLIPPING IT CORRUPTS AN UNRELATED PROCESS'S RESUME STATE AND
    // PANICS THE KERNEL**, in 2 full-suite runs out of 3: a process
    // blocked in a syscall comes back at an unmapped RIP, then
    // isr_common double-faults in its push prologue with RSP=0 -- which
    // means isr_dispatch() returned 0. Three control runs at 0xEE on the
    // same tree are clean. Two win_input KTESTs also go red, and they
    // are the small half.
    //
    // The two win_input KTESTs that used to hold it are FIXED -- an
    // exit ran preemptible, so a tick inside one marked the exiting
    // slot READY over its ZOMBIE and left it RUNNING for good
    // (scheduler_on_exit). What holds the flip now is ONE test at 1 run
    // in 3: sched_test's "a scheduled process survives a legacy process
    // running alongside", against 3 in 3 passing at 0xEE on the same
    // tree. docs/roadmap-details.md has the evidence and the
    // instruments.
    // LOGGED, because which gate a boot is running decides whether a
    // syscall can be preempted -- and a build that did not pick up a
    // change to it looks exactly like the change not working.
    idt_set_gate(128, isr128, 0, SYSCALL_GATE);
    klog_printf("idt: syscall gate 0x%x (%s)\n", SYSCALL_GATE,
                SYSCALL_GATE == 0xEF ? "trap -- syscalls are preemptible"
                                     : "interrupt -- IF clear for the whole call");

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    idt_load((uint64_t)&idtp);

    pic_remap();

    irq_register_handler(0, timer_irq_handler);
    irq_register_handler(1, keyboard_irq_handler);
    irq_register_handler(12, mouse_irq_handler);

    // unmask timer (IRQ0), keyboard (IRQ1), cascade (IRQ2, needed for
    // any slave-PIC line to reach the CPU), and mouse (IRQ12)
    for (uint8_t irq = 0; irq < 16; irq++) pic_set_mask(irq);

    // The PIT takes the tick, which is what programs it and unmasks
    // IRQ0. Every machine boots on it; kernel_main() offers the LAPIC
    // timer later, once there is something to calibrate against.
    clockevent_init();
    irq_unmask(1);
    irq_unmask(2);
    irq_unmask(12);

    __asm__ volatile ("sti");
}

// How deep THIS context is inside isr_dispatch(). Saved and restored by
// the wrapper, per call on the C stack, and swapped by the scheduler
// when a context switch changes whose kernel stack this describes --
// the same treatment g_next_kernel_rsp gets, because it is the same
// kind of fact. A plain global counter leaked to 348 the moment
// syscalls became preemptible: the increment ran on one context and the
// decrement on another. isr_reset_depth() still covers the longjmp
// teardown, which restores nothing.
static volatile int g_isr_depth = 0;

// Spurious LAPIC interrupts. A handful over a boot is normal (a masked
// line racing an in-flight delivery); a rising count is a real symptom,
// which is why it is counted rather than ignored silently.
static volatile uint32_t g_spurious = 0;

uint32_t idt_spurious_count(void) { return g_spurious; }

// WHICH SIGNAL AN EXCEPTION IS, or 0 for one no program may catch.
//
// The three POSIX names that mean something on this CPU. #PF and #GP are
// both SIGSEGV -- Linux makes the same call, because from a program's
// point of view "I touched memory I may not touch" is one event and the
// distinction between a missing page and a bad segment is the kernel's
// business. Everything else -- #DF, machine check, the ones a ring-3
// program cannot raise at all -- has no signal on purpose: a name for
// something no handler could sensibly act on is a name that invites
// somebody to try.
//
// dispatch-ok: bounded by the CPU's exception vectors, and by the three
// of them that have a meaning in ring 3.
static int fault_signal(uint64_t vector) {
    switch (vector) {
    case 0:  return SIGFPE;  // #DE -- integer divide by zero
    case 6:  return SIGILL;  // #UD -- an instruction that is not one
    case 13: return SIGSEGV; // #GP
    case 14: return SIGSEGV; // #PF
    default: return 0;
    }
}

int isr_in_progress(void) {
    return g_isr_depth > 0;
}

void isr_reset_depth(void) {
    g_isr_depth = 0;
}

// The scheduler's half: a context switch changes whose kernel stack
// these describe, so both travel with kernel_rsp.
int isr_depth_get(void) { return g_isr_depth; }
void isr_depth_set(int d) { g_isr_depth = d; }

// Point isr_common's epilogue somewhere other than what it interrupted.
// THE WHOLE CONTEXT-SWITCH MECHANISM, and the only way to reach the
// live dispatch's resume local from outside this file.
void isr_resume_set(uint64_t rsp) {
    if (g_resume_slot) *g_resume_slot = rsp;
}
void *isr_resume_slot_get(void) { return (void *)g_resume_slot; }
void isr_resume_slot_set(void *slot) { g_resume_slot = (uint64_t *)slot; }

// **THE INCOMING CONTEXT'S PAIR IS INSTALLED LAST, AFTER isr_dispatch()
// HAS PUT ITS OWN BACK.** A switch nominates here instead of writing
// the globals, because the wrapper's tail below restores the OUTGOING
// stack's outer values -- right for that stack, wrong for the CPU,
// which is about to be on the incoming one. Install then nominate would
// mean the incoming context resumes describing somebody else.
//
// An interrupt gate hid it: a switch could only happen at a context's
// outermost dispatch, and whatever the globals then said was replaced
// by the next trap. A trap gate lets a preempted syscall resume INSIDE
// a dispatch, where the next isr_resume_set() writes through a stale
// slot -- into a dead frame belonging to another stack, so the switch
// that was asked for silently does not happen.
static uint64_t *g_pending_slot;
static int g_pending_depth;
static int g_pending;

void isr_context_defer(void *slot, int depth) {
    g_pending_slot = (uint64_t *)slot;
    g_pending_depth = depth;
    g_pending = 1;
}

// **WHAT A SWITCHED-AWAY CONTEXT MUST BE SAVED WITH IS THE OUTER PAIR,
// NOT THE LIVE ONE -- the dispatch doing the saving is itself
// abandoned.** A resume is `mov rsp, <trapframe>; iretq`: it lands
// wherever that frame says and never runs a single dispatch tail, so
// every dispatch entered after that frame was pushed is gone. Saving
// the live depth therefore resurrects a frame that no longer exists,
// and the next isr_resume_set() through it writes into dead stack --
// which is a switch that silently does not happen.
static uint64_t *g_outer_slot;
static int g_outer_depth;

// `regs` is the frame the context will be RESUMED at, and its CS is what
// says how much of the dispatch chain survives that resume. From ring 3
// there is none: the frame is the outermost one on that kernel stack, so
// the context must come back describing nothing. Saving the live pair --
// or this one's outer, which belongs to whatever ran before -- leaves it
// pointing at a frame on somebody else's stack, and the next
// isr_resume_set() through it writes where nothing will read: the switch
// is recorded in procs[] and never happens, so the scheduler reports a
// process as RUNNING that is not running at all.
void isr_context_outer(const uint64_t *regs, void **slot, int *depth) {
    if ((regs[18] /* cs */ & 3) == 3) { *slot = 0; *depth = 0; return; }
    *slot = (void *)g_outer_slot;
    *depth = g_outer_depth;
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
// `scan_from` overrides where the stack scan starts. 0 means "use RSP",
// which is right for every fault except a kernel stack OVERFLOW: there
// RSP is on the unmapped guard page, so the scan reads nothing and the
// one thing worth having -- the call chain that got too deep -- is
// exactly what is missing. For that case the caller passes the stack's
// BASE, i.e. the first mapped word above the guard, where the deepest
// frames are.
static void panic_report_context(uint64_t rip, const uint64_t *regs,
                                  uint64_t scan_from) {
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

    uint64_t rsp = scan_from ? scan_from : regs[20];
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
        // Every page, not just the first: a stack OVERFLOW leaves RSP on
        // an unmapped guard page, so the plausibility check above is
        // satisfied and the read still faults -- inside the panic
        // handler, which loses the report. That is not hypothetical; it
        // is what the first armed guard page did.
        uint64_t at = (uint64_t)(uintptr_t)&sp[i];
        if ((at & 0xFFF) < 8 || i == 0) {
            if (!(paging_kernel_leaf(at) & 1 /* PRESENT */)) {
                klog_printf("    (stack scan stopped: 0x%lx is not mapped -- "
                            "an overflow leaves RSP on a guard page)\n", at);
                break;
            }
        }
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

static void isr_dispatch_body(uint64_t *regs) {
    uint64_t vector = regs[15];

    // The default -- resume exactly what was interrupted -- is the
    // wrapper's initialiser now. Only scheduler_tick() (and
    // scheduler_on_exit(), called from syscall.c) ever override it, and
    // only when the scheduler is armed. See scheduler.c's design comment.

    // WHOSE SIGNALS MAY BE ACTED ON WHEN THIS TRAP FINISHES, or 0 for
    // "none, not now". Captured HERE, before anything runs, because both
    // halves of the answer are only true at this instant.
    //
    // The CS check is the whole safety argument, and it is worth stating
    // rather than leaving to be re-derived: if the interrupted frame was
    // RING 3, then the kernel was not part-way through anything on
    // anybody's behalf -- no half-taken lock, no heap call in flight --
    // and the process that was running is the one whose address space we
    // may now free. If it was ring 0, this trap landed inside kernel
    // work (possibly somebody else's kmalloc) and terminating anything
    // from here would be the classic interrupt-context corruption.
    //
    // scheduler_current_pid() is sampled now because a timer tick may
    // switch away before the end of this function; signal.c compares
    // against the value it finds later precisely to notice that.
    // 0 covers both "kernel code was interrupted" and "ring-3 code with
    // no scheduler slot" (the legacy process_run_ring3() loader), which
    // has no pid to signal.
    int sig_pid = (regs[18] /* cs */ & 3) ? scheduler_current_pid() : 0;

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
    } else if (vector >= 64 && vector < 72) {
        // An I/O APIC input above the ISA range -- a PCI INTx pin a
        // `_PRT` entry routed to GSI 16-23. Same table, one line each.
        irq_dispatch((uint8_t)(16 + vector - 64), regs);
    } else if (vector >= LAPIC_VECTOR_BASE && vector <= LAPIC_VECTOR_LAST) {
        // AN MSI. Acknowledged to the LAPIC, never to the 8259 -- a PIC
        // EOI here would clear an in-service bit belonging to whichever
        // line happened to be live, and the LAPIC's would stay set.
        lapic_dispatch_vector((uint8_t)vector, regs);
    } else if (vector == LAPIC_SPURIOUS_VECTOR) {
        // BY ARCHITECTURE, NO EOI. The LAPIC did not set an in-service
        // bit for a spurious interrupt, so acknowledging one would
        // retire somebody else's.
        g_spurious++;
    } else if (vector == 128) {
        // A PENDING SIGNAL BEATS THE SYSCALL, and this is not an
        // optimisation -- it is what makes interrupting a blocked
        // program reliable.
        //
        // The problem it solves: a process parked in read() is woken
        // with -EINTR so that it can reach a delivery point, and it
        // resumes in ring 3 a few instructions from its next syscall.
        // With delivery only at the END of a trap, whichever of "the
        // timer ticks" and "the process calls exit()" happened first
        // decided whether it died of the signal or exited 0 -- and the
        // exit path zombies the slot, so the pending bit is then
        // unreachable forever. It passed anyway, most of the time,
        // which is the worst way for a race to behave.
        //
        // Checking here makes the process's OWN next syscall the
        // delivery point, which it always reaches. Linux does the same
        // thing from the other direction: a fatal signal pending means
        // the syscall returns without doing anything.
        //
        // Here and not at the top of isr_dispatch, because a hardware
        // IRQ must reach its handler to be acknowledged to the PIC --
        // returning early from one would stop interrupts for the rest of
        // the boot. The timer's own delivery is the check at the bottom.
        //
        // **`_deliverable` RATHER THAN `_pending`, AND THE RETURN VALUE
        // IS CHECKED.** Both halves are the same bug, found the hour
        // handlers landed: `pending` counts signals BLOCKED by a running
        // handler, so a handler's own SYS_SIGRETURN took this branch,
        // delivered nothing, and was never dispatched -- the restorer
        // returned from an `int $0x80` that had done nothing and ran
        // into its own `ud2`. Falling through to the syscall when
        // nothing was delivered makes that unreachable rather than
        // merely fixed.
        //
        // THE `1` IS SA_RESTART, and this is the only call site that
        // may pass it: the syscall has not run yet, so the frame saved
        // here can be rewound over the `int $0x80` and the call made
        // again after the handler returns. See kernel/signal.h.
        if (sig_pid) scheduler_syscall_entered(sig_pid); // the re-issue window is closed
        if (sig_pid && scheduler_signal_deliverable(sig_pid) &&
            signal_deliver_pending(sig_pid, regs, SIG_AT_SYSCALL_ENTRY)) {
            sig_pid = 0; // acted on; the check at the bottom must not repeat it
        } else {
            // Software interrupt from ring 3 (int 0x80) -- not a hardware
            // IRQ, so no PIC EOI. syscall_dispatch() may not return (see
            // syscall.h): the exit syscall jumps straight back into
            // whichever kernel code called process_run_ring3() instead of
            // rejoining isr_common's normal epilogue below.
            syscall_dispatch(regs);
        }
    } else if (vector < 32) {
        uint64_t error_code = regs[16];
        uint64_t rip = regs[17];
        uint64_t cs = regs[18];
        uint64_t cr2 = 0;
        if (vector == 14) { // page fault: CR2 holds the faulting address
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

            // DEMAND PAGING, and it belongs here -- before any of the
            // reporting below -- because a heap page arriving on first
            // touch is not an exception, it is the normal way memory
            // gets mapped now (SYS_SBRK reserves and maps nothing, see
            // kernel/uaddr.h). Returning immediately retries the
            // faulting instruction with the page present.
            //
            // The handler answers only for an address inside the
            // faulting address space's own heap reservation; anything
            // else -- a wild pointer, the stack guard, a page past the
            // break -- falls through to the report exactly as before.
            // So this cannot swallow a real fault: it can only satisfy
            // one the process was entitled to.
            if ((cs & 3) == 3 && vmm_fault_in(vmm_current_pml4(), cr2, error_code)) return;
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

        // **A FAULT IS OFFERED TO THE PROCESS BEFORE IT IS FATAL TO IT.**
        // A ring-3 program with a handler installed for the signal this
        // exception maps to gets that handler instead of the teardown
        // below -- so `catching SIGSEGV` means what it means everywhere
        // else, and a program can log its own crash, or fix the cause
        // and simply return.
        //
        // Deliberately BEFORE the report: a program that catches a fault
        // on purpose is not crashing, and printing a panic-grade page of
        // registers every time it does would make the report worthless
        // for the case it exists to serve. signal.c logs one line.
        //
        // WHAT KEEPS THIS FROM BECOMING A LOOP is the blocked mask, not
        // a counter: entering the handler blocks the signal, so a fault
        // INSIDE it finds the bit set, gets no handler, and falls
        // through to everything below (signal.c's signal_deliver_fault).
        int fault_sig = fault_signal(vector);
        if (recoverable && fault_sig && scheduler_current_pid() &&
            signal_deliver_fault(scheduler_current_pid(), fault_sig, regs)) {
            return;        // resume into the handler; regs now points at it
        }

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

        // The KERNEL's own stacks have guard pages too (scheduler.c), so
        // the same question gets asked of them. Two ways in: the
        // ordinary one is a #PF whose CR2 lands in a slot's guard page;
        // the violent one is a #DF, raised because the push that would
        // report the #PF cannot itself happen -- RSP is already on the
        // broken stack. That second case is why the #DF gate runs on an
        // IST (idt_init()); without it the CPU triple-faults and the
        // machine reboots with nothing printed at all.
        int kstack_slot = -1;
        int kstack_legacy = 0;
        uint64_t guard_at = vector == 14 ? cr2 : (vector == 8 ? regs[20] : 0);
        if (vector == 14 || vector == 8) {
            kstack_slot = scheduler_kstack_guard_slot(guard_at);
            // The legacy loader's stack is not a scheduler slot, and it
            // is the one `run`/`config set` from the shell use -- so
            // asking only the scheduler would report the commonest case
            // as a bare "Double fault".
            if (kstack_slot < 0) kstack_legacy = process_kstack_guard_hit(guard_at);
        }
        int kstack_overflow = kstack_slot >= 0 || kstack_legacy;

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
            vga_printf("The user stack hit its limit: it may grow down to "
                       "0x%lx (top 0x%lx) and went past it.\n",
                       (uint64_t)UADDR_STACK_FLOOR,
                       (uint64_t)UADDR_STACK_VADDR + 4096);
            vga_printf("That is %lu KiB of stack -- shorten the call chain, "
                       "or move a big local off it.\n",
                       (unsigned long)((UADDR_STACK_VADDR + 4096
                                        - UADDR_STACK_FLOOR) / 1024));
        }
        if (kstack_overflow && kstack_legacy) {
            vga_printf("The LEGACY loader's kernel stack overflowed into its "
                       "guard page (a `run`/`config` from the shell).\n");
        } else if (kstack_overflow) {
            vga_printf("Process slot %d (pid %d) ran off the bottom of its "
                       "KERNEL stack into the guard page below it.\n",
                       kstack_slot, kstack_slot + 1);
            vga_printf("The call chain in the stack scan below is too deep "
                       "for one -- shorten it, or move a big local off it.\n");
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
                     kstack_overflow ? "Kernel stack overflow"
                                     : stack_overflow ? "Stack overflow"
                                                      : exception_names[vector]);
        if (kstack_overflow) {
            if (kstack_legacy) {
                klog_printf("  the LEGACY loader's %d KiB kernel stack overran "
                            "into its guard page -- the call chain below is "
                            "too deep for one\n", scheduler_kstack_kib());
            } else {
                klog_printf("  process slot %d (pid %d) overran its %d KiB "
                            "KERNEL stack into the guard page below it -- the "
                            "call chain below is too deep for one\n",
                            kstack_slot, kstack_slot + 1,
                            scheduler_kstack_kib());
            }
        }
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
        if ((cs & 3) == 0) {
            uint64_t scan_from = 0;
            if (kstack_legacy)         scan_from = process_kstack_base();
            else if (kstack_slot >= 0) scan_from = scheduler_kstack_base(kstack_slot);
            panic_report_context(rip, regs, scan_from);
        }

        // The console draws into a back buffer and normally publishes
        // from the keyboard's idle loop (see vga.h's vga_present()). A
        // kernel panic never reaches that loop -- it halts below -- so
        // without this the banner reporting the panic is the one thing
        // that never makes it to the screen.
        //
        // _force, because the ordinary present does nothing while a
        // compositor owns the screen -- which would hide every panic
        // that happens under a running desktop.
        vga_present_force();

        if ((cs & 3) == 3 && ring3_hook) {
            ring3_hook(vector, error_code, cs, cr2);
        }
        // THE CRASH REPORT, while the process's page tables are still
        // live and after a caught signal has had its chance above. A
        // kernel fault writes nothing here, on purpose.
        if (recoverable) {
            crash_report_write(kstack_overflow ? "Kernel stack overflow"
                                              : stack_overflow ? "Stack overflow"
                                                               : exception_names[vector],
                               regs, cr2, stack_overflow);
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
    }

    // THE ONE PLACE A SIGNAL IS ACTED ON: on the way back to ring 3,
    // which is where Unix delivers and for the same reason (see
    // kernel/signal.h). After the depth decrement, because this may not
    // return in the ordinary sense -- terminating the current process
    // hands the CPU to another one exactly as SYS_EXIT does, and leaving
    // the depth raised would make isr_in_progress() lie for the rest of
    // the boot.
    //
    // Nothing pending is one load and one branch, which is what it has
    // to be: this runs on every syscall and every timer tick.
    // 0, not 1: whatever trap this was, it is FINISHED. A syscall here
    // has already produced its result, and rewinding it would run it a
    // second time.
    // ...unless the trap landed on the one instruction between a signal
    // wake and the syscall it rewound: that syscall has NOT run, and the
    // delivery must say so or it runs after the handler as if never
    // interrupted.
    if (sig_pid)
        signal_deliver_pending(sig_pid, regs,
                               scheduler_syscall_reissue_pending(sig_pid) ? SIG_BEFORE_REISSUE
                                                                          : SIG_TRAP_DONE);
}

// WHERE isr_common RESUMES, ANSWERED PER CALL RATHER THAN PER MACHINE.
//
// The trap: `g_next_kernel_rsp` is one global, and isr_dispatch_body()
// sets it to its own `regs` on entry. With an INTERRUPT gate that is
// safe because nothing can nest -- but the moment a syscall runs with
// IF set, a timer IRQ lands inside it, overwrites the global, and the
// OUTER handler's epilogue reloads a frame that has already been
// popped. It resumes garbage. That bug was hit twice (a blocking
// SYS_READ_KEY, then the ring-3 GUI migration) and was routed around
// both times rather than fixed; idt.h's isr_in_progress() exists
// because of it.
//
// The state was never really global -- it was per-invocation state kept
// in a global. This wrapper gives each call its own copy on the C
// stack, so nesting is correct by construction and every scheduler call
// site keeps writing `g_next_kernel_rsp` exactly as before.
//
// A noreturn path (process_context_exit()/recover()) longjmps out and
// never restores the outer value. That is fine rather than merely
// tolerated: it abandons the C stack the outer frame lived on, so there
// is no outer epilogue left to resume, and the next dispatch sets the
// global on entry regardless. isr_reset_depth() covers the depth
// counter at the same landing point.
uint64_t isr_dispatch(uint64_t *regs) {
    // THE RESUME VALUE LIVES HERE, on this call's own frame, so nothing
    // any other dispatch does can change what this one returns.
    uint64_t resume = (uint64_t)regs;
    uint64_t *outer_slot = g_resume_slot;
    int outer_depth = g_isr_depth;
    uint64_t *outer_pub = g_outer_slot;
    int outer_depth_pub = g_outer_depth;
    g_resume_slot = &resume;
    g_isr_depth = outer_depth + 1;
    g_outer_slot = outer_slot;      // what THIS dispatch will put back
    g_outer_depth = outer_depth;
    g_pending = 0;   // a nomination from a context that longjmp'd away
    isr_dispatch_body(regs);
    // RESTORED, not decremented: these describe THIS kernel stack, and
    // the body may have switched to another one part-way through.
    g_resume_slot = outer_slot;
    g_isr_depth = outer_depth;
    g_outer_slot = outer_pub;
    g_outer_depth = outer_depth_pub;
    // AND ONLY NOW the incoming context's, if the body nominated one.
    // Nothing runs between here and the epilogue's `mov rsp, rax`.
    if (g_pending) {
        g_resume_slot = g_pending_slot;
        g_isr_depth = g_pending_depth;
        g_pending = 0;
    }
    // isr_common does `mov rsp, rax` with this, so a zero becomes RSP=0
    // and double-faults three instructions later with no walkable stack
    // to name it. Report it here instead -- same klog/vga/`ud2` shape as
    // scheduler.c's kstack_verify(), and for the same reason.
    if (!resume) {
        klog_printf("ISR RESUME IS ZERO: vec=%lu cs=%lx depth=%d pid=%d\n",
                    regs[15], regs[18], g_isr_depth,
                    scheduler_current_pid());
        vga_printf("\nISR RESUME IS ZERO: vec=%lu cs=%lx depth=%d pid=%d\n",
                   regs[15], regs[18], g_isr_depth, scheduler_current_pid());
        __asm__ volatile ("ud2");
    }
    return resume;
}
