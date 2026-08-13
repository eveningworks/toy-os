#ifndef STRACE_INTERNAL_H
#define STRACE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

// Kernel-internal half of syscall tracing -- the hooks the syscall
// dispatcher and the ELF loaders call. Apps get only api/strace.h
// (arm/disarm/count); this is on the kernel's include path only, per
// kernel/include/README.md. See kernel/proc/strace.c for the design.

// Called once per new address space, from wherever a process is about
// to be created (kernel/proc/elf_run.c, kernel/proc/scheduler.c). If
// an arm is pending (api/strace.h's strace_arm()), this address space
// becomes the traced one and the arm is consumed; otherwise a no-op.
void strace_claim(uint64_t pml4_phys);

// Called from syscall_process_exit_cleanup() -- stops tracing if this
// was the traced address space, so a later, unrelated process running
// under a recycled CR3 can't inherit the trace.
void strace_release(uint64_t pml4_phys);

// Is the CURRENTLY running address space (CR3) being traced? One read
// of a global plus a compare -- what an untraced process pays per
// syscall.
int strace_active(void);

// The three dispatcher hooks, all no-ops unless strace_active().
// strace_begin() formats "name(args...)" into an internal line buffer
// BEFORE the handler runs (so it sees the arguments as passed, not as
// the handler left them); strace_end() appends " = <ret>" and emits
// the whole line at once, after the handler is done -- which is why a
// traced write()'s own output appears above its trace line rather than
// spliced into the middle of it. strace_end_noreturn() is for
// SYS_EXIT, the one handler that may never come back.
void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
void strace_end(uint64_t nr, uint64_t rax);
void strace_end_noreturn(void);

// The pure formatting core, exposed for kernel/proc/strace_test.c.
// Writes "name(args...)" (no return value, no newline) into `out` and
// returns its length. `pml4_phys` is the address space user pointers
// belong to -- pass 0 to format without dereferencing any of them,
// which is what makes this callable from a test with no live process.
size_t strace_format_call(char *out, size_t cap, uint64_t nr,
                           uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t pml4_phys);

// Formats " = <ret>" for `nr`'s return convention (decimal for most,
// hex for the one syscall that returns a pointer). Returns its length.
size_t strace_format_ret(char *out, size_t cap, uint64_t nr, uint64_t rax);

#endif
