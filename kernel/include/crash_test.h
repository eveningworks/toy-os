#ifndef CRASH_TEST_H
#define CRASH_TEST_H

// crash_test_run(): loads and runs userland/crash_test.c (see grub.cfg's
// module2 lines) -- a ring-3 program that deliberately faults (a
// wild-pointer write) to exercise the new fault-recovery path (see
// idt.c's fault handler / process.h's process_context_is_armed()). Unlike
// ring3test/elftest, this one RETURNS: process_run_ring3() comes back
// with PROCESS_CRASHED (process.h) instead of hanging the machine, and
// the shell keeps running afterward -- that's the whole point of this
// test existing.
void crash_test_run(void);

#endif
