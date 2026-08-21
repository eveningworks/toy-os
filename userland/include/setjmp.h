#ifndef ULIB_SETJMP_H
#define ULIB_SETJMP_H

// C's <setjmp.h>: save a return point, jump back to it later.
//
// NOTHING IN toy-os USES THIS. It is here because ported C code does --
// it is how a parser unwinds out of a deep recursion and how a
// decompressor reports a corrupt stream -- and because there is no way
// to write it in C at all: it is eight registers and a stack pointer.
//
// WHAT IT SAVES: the callee-saved set the SysV AMD64 ABI requires a
// function to preserve (rbx, rbp, r12-r15), plus rsp and the return
// address. Nothing else needs saving precisely BECAUSE the ABI says a
// call may clobber the rest -- setjmp() is a call, so its caller has
// already accepted that.
//
// WHAT IT DOES NOT SAVE: the x87/SSE state. That is deliberate and it
// matches glibc and musl: MXCSR and the x87 control word are also
// callee-saved by the ABI in the sense that a function must restore
// them before returning, so a longjmp back through a frame that changed
// them is already the caller's bug. Saving them would cost an FXSAVE
// (512 bytes, alignment-constrained) in every setjmp.
//
// THE TRAP, and it is C's rather than this implementation's: a local
// variable that is not `volatile` and was modified between setjmp() and
// longjmp() has an INDETERMINATE value after the jump, because it may
// live in a register the jump restored. This is the single most common
// way setjmp code is wrong, in any C program anywhere.
#include <stddef.h>

// rbx, rbp, r12, r13, r14, r15, rsp, rip -- eight 64-bit words.
typedef unsigned long jmp_buf[8];

// Returns 0 when called directly, and the value passed to longjmp()
// when returning from one.
int  setjmp(jmp_buf env);
// Never returns. A `val` of 0 is turned into 1, as C requires: setjmp()
// must never appear to return 0 a second time, or the caller cannot
// tell the two paths apart.
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
