#ifndef KERNEL_STRACE_H
#define KERNEL_STRACE_H

#include <stdint.h>

// Syscall tracing, the kernel half: naming the traced process at its
// spawn and writing its syscalls as RECORDS into the tracer's ring
// (abi/trace_abi.h). Decoding them is /bin/strace's. Kernel-internal --
// see kernel/proc/strace.c for the design.

// SYS_SPAWN's SPAWN_TRACE_RING: the shm index of a ring the caller
// created and that is shaped right, or a negative errno.
int strace_ring_check(const char *name, int pid);

// Asks that the next process THIS process spawns be traced into the ring
// at `ring_idx` (from strace_ring_check()). Scoped to the caller: only
// its own next spawn can collect the arm.
void strace_arm_for_current(int ring_idx);

// Cancels the CALLER'S arm if no process claimed it (the spawn failed).
// Another process's arm is left alone: a spawn can sleep, and every
// spawn ends in this call.
void strace_disarm(void);

// Called once per new address space (elf_run.c, sched_fork.c). Claims
// the arm if the process making it is the one that armed.
void strace_claim(uint64_t pml4_phys);

// At process exit: stops tracing if this was the traced address space,
// so a later process under a recycled CR3 cannot inherit it.
void strace_release(uint64_t pml4_phys);
// An exec'd process keeps its trace: the address space changed, the
// process did not.
void strace_rekey(uint64_t old_pml4, uint64_t new_pml4);

// Is the CURRENTLY running address space being traced? One global read
// and a compare -- what an untraced process pays per syscall.
int strace_active(void);

// The dispatcher's hooks, for a traced call only. strace_wait_for_room()
// comes first: with no room for the call's two records it rewinds the
// call and sleeps, returns 1, and the dispatcher must return at once.
// strace_begin() writes the ENTRY record before the handler runs (so it
// sees the arguments as passed); one of the three ends writes the exit:
// strace_end() for a value, strace_end_resumed() for a call that parked
// and has been woken (the value the wake wrote), strace_end_noreturn()
// for SYS_EXIT / SYS_THREAD_EXIT.
int strace_wait_for_room(uint64_t *regs);
void strace_begin(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
void strace_end(uint64_t nr, uint64_t rax);
void strace_end_noreturn(uint64_t nr);
void strace_end_resumed(uint64_t nr, uint64_t rax);

// The name for a syscall number, or NULL if this kernel has none. The
// syscall table is the one list of these; `kstack syscalls` reads it.
const char *strace_syscall_name(int nr);

#endif
