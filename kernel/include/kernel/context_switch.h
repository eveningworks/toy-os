#ifndef CONTEXT_SWITCH_H
#define CONTEXT_SWITCH_H

#include <stdint.h>

// A minimal setjmp/longjmp-style saved kernel execution context, and
// **the scheduler's one suspend shape** -- every context this kernel
// parks, whether preempted in ring 3 or blocked half-way through a
// syscall, is parked by saving one of these and resumed by restoring
// it. That is Linux's `__switch_to_asm` and NT's `SwapContext`: swap
// the kernel stack, and a task preempted in userspace is not special --
// its trapframe simply sits at the base of its own kernel stack and the
// resume unwinds back out to the epilogue that will iretq from it.
//
// The one thing that cannot be saved is a context that has never run.
// Linux plants `ret_from_fork` on the fresh stack for this; here a new
// process gets a hand-built context whose rsp is its trapframe and
// whose rip is isr.asm's `isr_resume_frame`, so the first restore lands
// straight in the interrupt epilogue's pops. See scheduler.c's
// kctx_for_trapframe().
//
// It also still serves process_run_ring3() (see process.h), which drops
// into ring 3 and gets control back from deep inside a different call
// stack, the same way an ordinary function call returns to its caller.
//
// The return address is captured directly into `rip` at save time,
// rather than left on the stack for restore to read later via `ret` --
// anything process_run_ring3() does *after* the save call (its own
// further pushes, e.g. building the iretq frame) legitimately reuses
// that now-"freed" stack slot, so relying on it still holding the
// original value later is unsafe. Restoring jumps to the saved `rip`
// directly instead.
struct kernel_context {
    uint64_t rsp; // stack pointer as it will be immediately AFTER the
                  // original process_context_save() call returns
    uint64_t rbx, rbp, r12, r13, r14, r15; // callee-saved per the SysV ABI
    uint64_t rip; // return address, captured directly (see above)
};

// Saves the current context. Returns 0 on this, the "normal" call.
// If control is later transferred here again via
// process_context_restore(), this SAME call "returns" a second time,
// with whatever value was passed to process_context_restore() -- exactly
// like setjmp()/longjmp().
// `returns_twice` is not decoration: without it GCC is entitled to
// assume the code after this call runs once, and at -O2 that is a
// live-range assumption about every local held across it. setjmp
// carries the same attribute for the same reason.
int process_context_save(struct kernel_context *ctx)
    __attribute__((returns_twice));

// Restores a previously saved context: the matching
// process_context_save() call "returns" again with `value`. Never
// returns to its own caller.
void process_context_restore(struct kernel_context *ctx, int value)
    __attribute__((noreturn));

// The same, without the `sti`. What the SCHEDULER resumes through: the
// context being restored was suspended with interrupts in the state it
// wants, and the iretq it eventually unwinds to restores IF from its
// own frame. See context_switch.asm.
void process_context_restore_noirq(struct kernel_context *ctx, int value)
    __attribute__((noreturn));

// isr.asm's interrupt epilogue, reachable by name: point RSP at a
// trapframe and return through it. The address of `isr_resume_frame`
// (the same tail, minus the RSP load) is what a hand-built context's
// rip holds -- see the note above.
void isr_return_to(uint64_t *regs) __attribute__((noreturn));
void isr_resume_frame(void);

// Start running `entry(arg)` on `stack_top`, and never come back here.
//
// A context can only be RESUMED while the frames it saved are still
// live -- process_context_restore() puts RSP back inside them, and a
// caller that has since returned has handed that memory to whatever ran
// next (see the note above on why `rip` is captured as data). So a
// second context cannot be a second save point in one call chain: it
// needs its own stack, and this is what puts it there.
//
// `entry` must never return. There is nothing to return to.
void process_context_enter(void *stack_top, void (*entry)(void *), void *arg)
    __attribute__((noreturn));

#endif
