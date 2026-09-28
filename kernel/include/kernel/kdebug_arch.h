#ifndef KDEBUG_ARCH_H
#define KDEBUG_ARCH_H

// The CPU half of the kernel debugger (kernel/arch/x86_64/kdebug_x86.c):
// everything that would be rewritten on another architecture. The
// protocol and the breakpoint bookkeeping (kernel/debug/) only ever see
// a trap frame through these calls.

#include <stdint.h>

// GDB's amd64 core registers, in `g` packet order: 16 GPRs, rip, then
// eflags and six segment selectors at 4 bytes each. A shorter `g` reply
// is legal -- GDB marks the rest (x87, SSE) unavailable.
#define KDB_NREGS 24
int      kdb_arch_reg_size(int n);
uint64_t kdb_arch_reg_get(const uint64_t *regs, int n);
void     kdb_arch_reg_set(uint64_t *regs, int n, uint64_t v);
// A PARKED context's registers (context_switch.h): rsp, rip and the
// callee-saved ones. Nothing else was saved, so *have is 0 for the rest
// and GDB is told they are unavailable rather than shown a guess.
struct kernel_context;
uint64_t kdb_arch_ctx_reg(const struct kernel_context *k, int n, int *have);
uint64_t kdb_arch_pc(const uint64_t *regs);
void     kdb_arch_set_pc(uint64_t *regs, uint64_t pc);

// Byte access through the CURRENT page tables, walked first: an address
// nothing maps is refused rather than faulted on. Both return how many
// bytes they managed, stopping at the first unmapped page. A write may
// patch read-only KERNEL text (a breakpoint); a read-only USER page is
// refused, because it may be copy-on-write shared with another process.
uint64_t kdb_arch_mem_read(uint64_t va, void *dst, uint64_t len);
uint64_t kdb_arch_mem_write(uint64_t va, const void *src, uint64_t len);
// The same in address space `cr3` -- another thread's (0: the current).
uint64_t kdb_arch_mem_read_in(uint64_t cr3, uint64_t va, void *dst, uint64_t len);
uint64_t kdb_arch_mem_write_in(uint64_t cr3, uint64_t va, const void *src, uint64_t len);

#define KDB_BREAK_INSN 0xCC
void kdb_arch_breakpoint(void);   // a compiled-in int3

// What a trap was, from its vector and the debug status register.
enum kdb_trap {
    KDB_TRAP_NONE,    // not the debugger's
    KDB_TRAP_BREAK,   // int3: the PC is one past it
    KDB_TRAP_STEP,    // a single step finished
    KDB_TRAP_HW,      // a DR0-3 slot fired; *slot says which
    KDB_TRAP_NMI,
};
enum kdb_trap kdb_arch_classify(uint64_t vector, int stepping, int *slot);

// The four debug-register slots. `type` is GDB's Z number (1 exec,
// 2 write, 4 access); x86 has no read-only watch, so no 3.
#define KDB_HW_SLOTS 4
struct kdb_hw {
    uint64_t addr;
    uint8_t  used, type, len;
};
int  kdb_arch_hw_valid(uint64_t addr, int type, int len);
void kdb_arch_hw_install(const struct kdb_hw *slots);  // DR0-3 + DR7
void kdb_arch_hw_disable(void);                        // DR7 = 0

// Leaving the debugger: RF so a hardware breakpoint at the PC does not
// fire again at once, and for a step TF with interrupts MASKED so the
// step lands on the next instruction rather than in the timer's ISR.
// step_done() puts IF back as it was.
void kdb_arch_resume(uint64_t *regs, int step);
void kdb_arch_step_done(uint64_t *regs);
int  kdb_arch_step_in_place(uint64_t *regs);   // 1: a step the CPU must not run (HLT), done

#endif
