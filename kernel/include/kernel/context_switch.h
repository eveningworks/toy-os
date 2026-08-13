#ifndef CONTEXT_SWITCH_H
#define CONTEXT_SWITCH_H

#include <stdint.h>

// A minimal setjmp/longjmp-style saved kernel execution context. There's
// no scheduler yet -- this exists purely so process_run_ring3() (see
// process.h) can drop into ring 3 and get control back later from deep
// inside a completely different call stack (the syscall handler, running
// on the TSS's kernel stack after an int 0x80 from ring 3), the same way
// an ordinary function call returns to its caller.
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
int process_context_save(struct kernel_context *ctx);

// Restores a previously saved context: the matching
// process_context_save() call "returns" again with `value`. Never
// returns to its own caller.
void process_context_restore(struct kernel_context *ctx, int value)
    __attribute__((noreturn));

#endif
