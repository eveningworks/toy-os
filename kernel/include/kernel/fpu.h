#ifndef FPU_H
#define FPU_H

#include <stdint.h>

// x87/SSE state: turning it on, and saving/restoring it across a task
// switch. Lives in arch/x86_64 because every line of it is a control
// register or an FXSAVE -- a different CPU would need all of it
// rewritten (see kernel/README.md).
//
// **Who is allowed to use floating point here, and why it's only ring 3.**
// The kernel and everything in apps/ stay compiled with `-mno-sse
// -mno-sse2 -mno-mmx`; only userland/'s ring-3 ELFs get float. That's
// the same split Linux and Windows both use -- Linux builds its kernel
// with those exact flags and makes kernel-side SIMD an explicitly
// bracketed `kernel_fpu_begin()`/`kernel_fpu_end()` region (AES-NI,
// RAID6), and Windows requires the equivalent
// KeSaveExtendedProcessorState()/KeRestoreExtendedProcessorState()
// around any kernel-mode FP. Neither runs a kernel that uses FP freely.
//
// The reason that split is worth copying rather than just conservative:
// with SSE enabled and `-mno-sse` dropped, GCC emits XMM registers in
// ORDINARY code -- struct copies and inlined memcpy included, not just
// code that mentions a `float`. An interrupt can land on any
// instruction, so a kernel compiled that way would need an FXSAVE on
// the interrupt path itself, on every single vector. Keeping the
// kernel FP-free means state only has to move when the scheduler
// actually swaps ring-3 processes, which is thousands of times rarer
// and is a place that already exists (scheduler.c's switch_to()).
//
// **Eager, not lazy.** The textbook trick is to set CR0.TS, let the
// first FP instruction after a switch fault with #NM, and restore state
// only for processes that actually use it. Both mainstream kernels
// abandoned that: it's what CVE-2018-3665 (Lazy FP State Restore)
// exploited to read another task's registers, and Linux deleted its
// lazy path outright in 4.14. FXRSTOR is on the order of 100 cycles on
// anything modern, against a 100 Hz tick here, so the trade isn't even
// close. CR0.TS stays 0 and every switch restores unconditionally.

// FXSAVE/FXRSTOR's fixed area: 512 bytes, and the instruction #GPs on
// anything less than 16-byte alignment -- hence the matching attribute
// on every buffer declared with this (see struct sched_process).
#define FPU_STATE_SIZE  512
#define FPU_STATE_ALIGN 16

// Enables SSE and turns the FPU on for real: clears CR0.EM (so FP
// instructions execute instead of raising #UD), sets CR0.MP and CR0.NE,
// and sets CR4.OSFXSR + CR4.OSXMMEXCPT (so FXSAVE/FXRSTOR exist and SIMD
// exceptions arrive as #XF rather than the ancient PIC FERR path).
// Also captures the pristine post-FNINIT state that fpu_init_state()
// hands out below.
//
// Returns 1 on success, 0 if the CPU somehow reports no FXSR/SSE2 --
// which cannot happen on anything that reached long mode (both are
// architecturally mandatory on x86-64), so a 0 here means the CPUID
// read itself is wrong, not that a real machine lacks SSE. Checked
// rather than assumed because the cost is four instructions once at
// boot.
//
// Call once from kernel_main(), before any ring-3 process can run.
int fpu_init(void);

// 1 once fpu_init() has succeeded. Anything that would touch FP state
// before that point (there shouldn't be any) can check.
int fpu_enabled(void);

// Writes a freshly-initialized FP state into `area` (FPU_STATE_SIZE
// bytes, 16-byte aligned) -- what a brand-new process starts with:
// control word 0x037F, MXCSR 0x1F80, no live registers, no pending
// exceptions.
//
// Copies the template fpu_init() captured with a real FNINIT+FXSAVE
// rather than hand-assembling the 512-byte layout. Hand-assembling is
// how you end up with an MXCSR_MASK of zero, which makes a later
// FXRSTOR #GP on bits the CPU would otherwise have accepted -- a fault
// at a completely unrelated moment, in another process.
void fpu_init_state(void *area);

// FXSAVE/FXRSTOR straight through. `area` must be FPU_STATE_SIZE bytes
// and 16-byte aligned in both cases.
void fpu_save(void *area);
void fpu_restore(const void *area);

#endif
