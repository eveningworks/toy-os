#ifndef RING3_TEST_H
#define RING3_TEST_H

// Runs a minimal ring-3 demonstration: maps a small user-accessible
// code+data+stack region (see paging.h), drops to ring 3 via a manual
// iretq, and runs a tiny hand-encoded program that writes a marker value
// into its own mapped memory and then deliberately executes a privileged
// instruction (hlt). Ring 3 code cannot execute hlt, so the CPU faults;
// the kernel's exception handler catches it and prints diagnostics --
// the marker value (proving the ring-3 code really ran and could write
// to its own memory) and the CS register from the fault (proving it was
// genuinely running at ring 3 when blocked).
//
// This intentionally does not return. There's no syscall/process
// teardown yet to recover from the fault and resume the shell -- that's
// the natural next step. For now this is a one-shot proof that the
// paging + GDT/TSS + ring-3 plumbing actually works and actually
// enforces isolation.
void ring3_test_run(void);

#endif
