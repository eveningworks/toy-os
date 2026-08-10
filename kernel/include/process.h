#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

// Runs ring-3 code at `entry` (in address space `pml4_phys`, with the
// given user stack) and returns when that code calls the exit syscall
// (see syscall.c) with whatever exit code it passed, OR when its ring-3
// code faults and idt.c catches it -- in the fault case the return value
// is PROCESS_CRASHED, not a real exit code (see process_context_is_armed()
// below). Behaves like an ordinary (if unusual) function call from the
// caller's point of view -- see context_switch.h for how that's
// actually implemented, since there's no scheduler to hand control back
// through yet.
//
// ring3_test.c/elf_test.c are the one exception: they drop to ring 3
// with their own raw, manual iretq instead of calling this function, so
// their deliberate faults have nowhere to recover TO
// (process_context_is_armed() stays false the whole time) and still
// halt the machine exactly as before -- see idt.c's fault handler.
//
// Only one process can be "in flight" through this at a time (there's a
// single shared saved context, not a stack of them) -- fine for now
// since nothing here runs concurrently, but worth knowing if this ever
// needs to nest.
int process_run_ring3(uint64_t pml4_phys, uint64_t entry, uint64_t user_rsp);

// The sentinel process_run_ring3() returns when its ring-3 code faulted
// instead of exiting cleanly -- never a real exit code (context_switch.h's
// +1 exit-code bias means a real exit(N) always returns N, N >= a
// caller-chosen int with no reserved values, but nothing ever produces
// -2 through that path, so it's an unambiguous sentinel in practice).
#define PROCESS_CRASHED (-2)

// True if a process_run_ring3() call is currently "in flight" -- i.e.
// its ring-3 code is running and hasn't exited or been recovered from
// yet. idt.c's fault handler checks this before attempting recovery via
// process_context_recover(): ring3_test.c/elf_test.c drop to ring 3
// with their own raw, manual iretq (see process_run_ring3()'s comment
// above), so nothing is armed during their fault -- recovering would
// jump into whatever unrelated call last used this same shared context,
// not somewhere safe.
int process_context_is_armed(void);

// Recovers into the currently-armed process_run_ring3() call with a real
// exit code, exactly as if its ring-3 code had called the exit syscall
// with `code` (the +1 bias from context_switch.h is applied internally).
// Never returns. Only call this when process_context_is_armed() is true
// -- syscall.c's SYS_EXIT handler is the only caller.
void process_context_exit(int code) __attribute__((noreturn));

// Recovers into the currently-armed process_run_ring3() call as
// PROCESS_CRASHED, exactly as if its ring-3 code had called the exit
// syscall -- reached from idt.c's fault handler instead of syscall.c's
// SYS_EXIT. Never returns. Only call this when process_context_is_armed()
// is true.
void process_context_recover(void) __attribute__((noreturn));

#endif
