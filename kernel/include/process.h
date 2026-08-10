#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

// Runs ring-3 code at `entry` (in address space `pml4_phys`, with the
// given user stack) and returns when that code calls the exit syscall
// (see syscall.c) with whatever exit code it passed. Behaves like an
// ordinary (if unusual) function call from the caller's point of view --
// see context_switch.h for how that's actually implemented, since there's
// no scheduler to hand control back through yet.
//
// If the ring-3 code never calls exit -- e.g. ring3_test.c's and
// elf_test.c's deliberate hlt, which faults instead -- this function
// never returns either, exactly as before; only code that actually
// calls the exit syscall benefits from this.
//
// Only one process can be "in flight" through this at a time (there's a
// single shared saved context, not a stack of them) -- fine for now
// since nothing here runs concurrently, but worth knowing if this ever
// needs to nest.
int process_run_ring3(uint64_t pml4_phys, uint64_t entry, uint64_t user_rsp);

#endif
