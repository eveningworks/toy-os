#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

// Built as the ORIGINAL Milestone 16 (the old pre-v0.1.0 numbering used
// by CHANGELOG-archive.md, unrelated to docs/roadmap.md's current
// Milestone 16): a minimal preemptive round-robin scheduler for ring-3
// processes, layered on TOP of the M8-M15 process-isolation work
// without changing it. See scheduler.c for the full design writeup.
//
// Self-contained and safe by construction, not by staying off: idt.c's
// timer branch always calls scheduler_tick(), and it's continuously
// armed from scheduler_init() onward (Milestone 1 phase 4b generalized
// this from a demo-only flag -- see scheduler.c's top comment), but
// every existing test command (ring3test, elftest, syscalltest,
// writetest, ptrtest, guitest) and every legacy elf_run_from_fs()
// caller (`run`/`ls` from the physical shell) is still completely
// unaffected, since none of them ever touch the scheduler's process
// table -- an armed tick over an empty table never writes to
// g_next_kernel_rsp (see idt.c), so isr_common's epilogue resumes
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
// the current process SCHED_ZOMBIE (holding `code` for a later
// scheduler_poll() -- see scheduler.c's own comment on that enum
// value, not freed to SCHED_UNUSED immediately the way it used to be)
// and hands its CPU slot to the next ready process, or back to
// whatever kernel code called scheduler_spawn()/scheduler_demo_run()
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
// (userland/counter_a.c, counter_b.c) and preemptively round-robins
// between them (100Hz timer slices) while the shell itself stays
// blocked waiting for both to exit, proving genuine concurrent,
// non-cooperative scheduling rather than the M8-M15
// one-process-at-a-time model. The scheduler itself stays armed after
// this returns (it always is now -- see this header's top comment),
// unlike the old demo-only behavior; every other command still behaves
// exactly as it did before M16.
void scheduler_demo_run(void);

// Spawns `path` (optional whitespace-separated `args`, NULL/"" for
// none -- same convention as elf_run_from_fs()'s, see elf_run.h) as a
// new scheduler-managed process and returns IMMEDIATELY with a 1-based
// pid (> 0), or 0 on any setup failure (no free slot -- MAX_PROCS is 4
// -- missing/invalid ELF, or arguments too long to fit the process's
// one stack page). Unlike elf_run_from_fs(), does not block: the
// process runs preemptively alongside whatever called this, and the
// caller must poll scheduler_poll(pid, ...) to learn when it finishes
// and collect its exit code. This is the public, non-blocking
// counterpart to elf_run_from_fs() the roadmap's Terminal async-spawn
// item needed -- see apps/wm/wm.c's wm_run() poll loop (Milestone 1
// phase 4b) for the first real caller.
int scheduler_spawn(const char *path, const char *args);

// FS_STEP_*-shaped result for scheduler_poll() below (fs.h's enum
// fs_step_result was the direct precedent -- same "PENDING/DONE-ish,
// call again" shape, renamed since this isn't actually that type and a
// process can't "fail" the way a filesystem step can, only ever exit
// or not exist).
enum sched_poll_result {
    SCHED_POLL_RUNNING = 0, // still going -- call again later
    SCHED_POLL_EXITED  = 1, // just reaped: `*out_exit_code` is valid, pid is now free (SCHED_UNUSED)
    SCHED_POLL_INVALID = 2, // `pid` is out of range, or already reaped/never spawned
};

// Polls a process started via scheduler_spawn(pid > 0). Returns
// SCHED_POLL_RUNNING while it's still SCHED_READY/SCHED_RUNNING (does
// NOT block -- call this once per wm_run() frame, same shape as
// fs_write_range_step()/fs_read_range_step()), SCHED_POLL_EXITED the
// first time it's found SCHED_ZOMBIE (writing its exit code to
// `*out_exit_code` if non-NULL, and reaping the slot back to
// SCHED_UNUSED so a future scheduler_spawn() can reuse it -- a second
// scheduler_poll() call with the same `pid` after this returns
// SCHED_POLL_INVALID, not SCHED_POLL_EXITED again), or
// SCHED_POLL_INVALID for a `pid` that's out of range or was already
// reaped.
enum sched_poll_result scheduler_poll(int pid, int *out_exit_code);

#endif
