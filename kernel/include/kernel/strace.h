#ifndef KERNEL_STRACE_H
#define KERNEL_STRACE_H

#include <stdint.h>
#include <stddef.h>

// Syscall tracing. All of it is kernel-internal: `strace` is a RING-3
// PROGRAM now (userland/bin/strace.c) and asks for a trace through
// SYS_SPAWN's SPAWN_TRACE flag, so nothing under apps/ needs any of
// this and there is no api/ half any more. This header is on the
// kernel's include path only, per kernel/include/README.md. See
// kernel/proc/strace.c for the design.
//
// **THERE USED TO BE TWO HEADERS AND ONE OF THEM WAS `api/strace.h`**,
// carrying arm/disarm/count for the kernel shell's `strace` builtin.
// The builtin is gone -- ring 0 contains no applications -- so the
// audience split it existed for is gone with it, and one header is what
// is left. kapi.h no longer mentions tracing at all.

// Asks that the next process THIS process spawns be traced. SYS_SPAWN
// calls it when the message carries SPAWN_TRACE, and nothing else does.
//
// **SCOPED TO THE CALLER, WHICH IS WHAT MAKES IT RACE-FREE.** It used
// to be a bare global -- "the next process created anywhere" -- and a
// spawner preempted between arming and creating had its trace claimed
// by whoever else spawned in the window. Recording WHO armed it means
// somebody else's spawn cannot consume it; the arm is a promise to one
// process, and only that process's own next spawn can collect.
void strace_arm_for_current(void);

// Cancels an arm that was never consumed -- the binary did not exist,
// so no process was ever created. A no-op once a process has claimed
// it, since that process's own exit clears it (strace_release()).
void strace_disarm(void);

// Called once per new address space, from wherever a process is about
// to be created (kernel/proc/elf_run.c, kernel/proc/scheduler.c).
// Claims the arm if the process making it is the one that armed;
// otherwise a no-op. This is also where the trace's SINK is decided --
// see kernel/proc/strace.c.
void strace_claim(uint64_t pml4_phys);

// Called from syscall_process_exit_cleanup() -- stops tracing if this
// was the traced address space, so a later, unrelated process running
// under a recycled CR3 can't inherit the trace. Also where the
// "N syscalls traced" summary is printed, because this is the one
// moment the kernel knows the traced process is finished and still
// knows where its trace was going.
void strace_release(uint64_t pml4_phys);
// An exec'd process keeps its trace: the address space changed, the
// process did not.
void strace_rekey(uint64_t old_pml4, uint64_t new_pml4);

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
// spliced into the middle of it. strace_end_noreturn() closes the line
// as " = ?" for a handler that produces no return value to print: it
// was written for SYS_EXIT (which may never come back at all) and is
// also what a handler that PARKS its caller uses (SYS_WAIT_EVENT --
// the value is written into the saved trapframe by the eventual wake,
// long after this line would have been emitted). A blocking wait
// therefore reads as one " = ?" per park, followed by a fresh call
// line with the real result once the client re-enters the syscall.
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

// The name for a syscall number, or NULL if this kernel has none. The
// strace table is the one list of these; a second copy would drift.
// `kstack syscalls` reads it too, which is why it is not private to
// strace.c.
const char *strace_syscall_name(int nr);

#endif
