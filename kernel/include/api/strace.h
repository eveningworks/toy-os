#ifndef STRACE_H
#define STRACE_H

#include <stdint.h>

// The app-facing half of syscall tracing -- what the shell's `strace`
// command (apps/shell_sys.c's cmd_strace()) needs and nothing more.
// The tracing itself is kernel-internal: see
// kernel/include/kernel/strace_internal.h for the dispatcher/loader
// hooks, and kernel/proc/strace.c for the implementation and the
// design comment.
//
// Usage is a strict arm -> run -> report cycle:
//
//     strace_arm();                   // the NEXT process launched is traced
//     elf_run_from_fs(path, args);    // ... which happens here
//     strace_call_count();            // how many syscalls it made
//     strace_disarm();                // safety net if nothing launched
//
// Arming is deliberately "the next process," not "this process ID" --
// the caller can't know the traced process's address space until
// elf_run_from_fs() has already created it, and there is no PID to
// name until then either.

// Arms tracing for the next process whose address space gets set up
// (elf_run_from_fs(), or the scheduler's own spawn path). Re-arming
// while already armed is harmless -- it stays armed.
void strace_arm(void);

// Cancels an arm that was never consumed -- e.g. the binary didn't
// exist, so no process was ever created. A no-op if tracing was
// already claimed by a process, since that process's own exit clears
// it (see strace_release() in the internal header).
void strace_disarm(void);

// How many syscalls the most recently traced process made. Reset to 0
// each time an arm is claimed, so it's read after the traced process
// returns, not before.
uint64_t strace_call_count(void);

// The name for a syscall number, or NULL if this kernel has none. The
// strace table is the one list of these; a second copy would drift.
const char *strace_syscall_name(int nr);

#endif
