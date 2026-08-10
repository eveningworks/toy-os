#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

// Milestone 16: a minimal preemptive round-robin scheduler for ring-3
// processes, layered on TOP of the M8-M15 process-isolation work
// without changing it. See scheduler.c for the full design writeup.
//
// Deliberately opt-in and self-contained: idt.c's timer branch always
// calls scheduler_tick(), but it's a true no-op unless
// scheduler_demo_run() has actually armed it -- every existing test
// command (ring3test, elftest, syscalltest, writetest, ptrtest,
// guitest) is completely unaffected, since none of them ever touch the
// scheduler's process table, and the disarmed no-op path never writes
// to g_next_kernel_rsp (see idt.c), so isr_common's epilogue resumes
// exactly what it always did.

// One-time setup. Call once from kernel_main, before apps_start().
void scheduler_init(void);

// Called from idt.c's isr_dispatch for every timer tick (vector 32),
// after pit_handle_irq()+EOI. `regs` is the saved-register pointer for
// whatever was interrupted -- exactly the pointer isr_common's epilogue
// will resume from via g_next_kernel_rsp, unless this call decides to
// switch it to somewhere else. No-op if the scheduler hasn't been armed
// (see scheduler_demo_run()).
void scheduler_tick(uint64_t *regs);

// Called from syscall.c's SYS_EXIT handler INSTEAD of the old
// process_context_restore(&g_process_ctx, ...) path, whenever
// scheduler_current_pid() is non-zero (i.e. the exiting process is
// scheduler-managed, not a legacy process_run_ring3() caller). Marks
// the current process UNUSED and hands its CPU slot to the next ready
// process, or back to whatever kernel code called scheduler_demo_run()
// if none are ready. Like the exit syscall itself, this doesn't return
// to its own caller in the normal sense -- control resumes somewhere
// else entirely once isr_common's epilogue runs.
void scheduler_on_exit(int code);

// Non-zero (1-based pid) if the syscall currently being handled came
// from a scheduler-managed process; 0 otherwise (legacy
// process_run_ring3 path, or no process at all -- e.g. a stray int
// 0x80 from kernel code, which should never happen but isn't this
// function's problem). Lets syscall.c pick the right exit path.
int scheduler_current_pid(void);

// Runs the M16 demo: spawns two small ring-3 counter programs
// (userland/counter_a.c, counter_b.c -- GRUB modules 5 and 6) and
// preemptively round-robins between them (100Hz timer slices) while
// the shell itself stays blocked waiting for both to exit, proving
// genuine concurrent, non-cooperative scheduling rather than the
// M8-M15 one-process-at-a-time model. Disarms the scheduler again
// before returning, so every other command behaves exactly as it did
// before M16 once this returns.
void scheduler_demo_run(void);

#endif
