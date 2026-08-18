#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "proc_info.h" // struct proc_info -- scheduler_proc_info() below

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
// How many ring-3 processes can exist at once.
//
// Was 4, which was not a design decision so much as a number nobody had
// revisited: each slot embeds an 8 KiB kernel stack and 512 bytes of FPU
// state, so the whole table is about 8.8 KiB per process. At 4 that was
// 35 KiB and the desktop stopped launching anything after four windows;
// at 64 it is ~560 KiB against a kernel .bss already 13 MiB (the GUI back
// buffer), which is a rounding error for sixteen times the headroom.
//
// **Anything sizing a per-process table must use this**, not a literal.
// win_server.c did carry its own 4 with a comment saying "MAX_PROCS
// (scheduler.c)", which is a copy waiting to be forgotten.
#define SCHED_MAX_PROCS 64

void scheduler_init(void);

// Called from idt.c's isr_dispatch for every timer tick (vector 32),
// after pit_handle_irq()+EOI. `regs` is the saved-register pointer for
// whatever was interrupted -- exactly the pointer isr_common's epilogue
// will resume from via g_next_kernel_rsp, unless this call decides to
// switch it to somewhere else. No-op if the scheduler hasn't been armed
// (see scheduler_demo_run()).
void scheduler_tick(uint64_t *regs);

// SYS_YIELD's entry into the same rotation. Identical to
// scheduler_tick() except that it charges the caller NO cpu time:
// a tick is a unit of elapsed time and a yield elapses microseconds, so
// billing one here bills time that never passed. Reusing scheduler_tick()
// for this made every polling app report a clamped 100% -- see
// scheduler_rotate()'s comment.
void scheduler_yield(uint64_t *regs);

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

// --- the running process's heap ---------------------------------------
//
// SYS_SBRK's per-process state, reached by the syscall layer rather than
// held there. `brk` is what the process sees; `mapped_end` is how far
// pages have actually been allocated behind it, which is only ever a
// whole number of pages ahead of or equal to `brk`.
//
// Kept as a struct handed out by pointer, not as a get/set pair,
// because sbrk has to read both and advance both together -- two
// accessors would make a partially-applied grow representable.
struct sched_heap {
    uint64_t brk;
    uint64_t mapped_end;
};

// The heap of the process currently on the CPU, or NULL when the kernel
// context is running (current_index == -1) -- which is the legacy
// elf_run.c case, where syscall.c's own single-slot heap still applies.
// Every scheduler-spawned process has one armed from the moment it is
// created, so the NULL means "not a scheduled process", never "this
// process has no heap".
struct sched_heap *scheduler_current_heap(void);

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
// pid (> 0), or 0 on any setup failure (no free slot -- SCHED_MAX_PROCS
// -- missing/invalid ELF, or arguments too long to fit the process's
// one stack page). Unlike elf_run_from_fs(), does not block: the
// process runs preemptively alongside whatever called this, and the
// caller must poll scheduler_poll(pid, ...) to learn when it finishes
// and collect its exit code. This is the public, non-blocking
// counterpart to elf_run_from_fs() the roadmap's Terminal async-spawn
// item needed -- see apps/wm/wm.c's wm_run() poll loop (Milestone 1
// phase 4b) for the first real caller.
int scheduler_spawn(const char *path, const char *args);

// Same, but the child's stdout (fd 1) is redirected into `pipe_idx`
// (pipe.h) instead of the console. -1 means "the console", i.e.
// identical to scheduler_spawn().
//
// This is what lets one process read another's output -- the thing a
// terminal fundamentally does and that nothing here could do before.
// The redirection is per-process state rather than an fd-table entry
// because fd 1 has always been a hardcoded console in SYS_WRITE; see
// syscall.c.
int scheduler_spawn_piped(const char *path, const char *args, int pipe_idx);

// The pipe `pid`'s stdout is redirected into, or -1 for the console.
// Called from SYS_WRITE to decide where fd 1 goes.
int scheduler_stdout_pipe(int pid);

// Whether `pid` names a live or reaped-pending process started by
// scheduler_spawn*(). For SYS_WAITPID's validation.
int scheduler_pid_valid(int pid);

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

// What a blocked process is waiting for. One flat namespace rather than
// a per-subsystem one, so a wake site never has to know which flavour
// of waiter it is answering -- it names the EVENT that happened and
// every process parked on it wakes.
#define SCHED_WAIT_EVENT 1 // a window/input event for this process
#define SCHED_WAIT_PIPE  2 // data (or EOF) on a pipe this process reads
#define SCHED_WAIT_CHILD 3 // a spawned child of this process exited

// Parks the calling process until scheduler_wake() names its `reason`,
// and hands the CPU to whatever is next. `regs` must be the syscall
// handler's own trapframe pointer.
//
// A blocking syscall in this kernel MUST go through this rather than
// waiting in place with interrupts on -- that was tried, and hangs
// after one event because g_next_kernel_rsp isn't reentrant (see
// syscall.c's SYS_READ_KEY comment and scheduler.c's own writeup here).
//
// Returns 1 if the caller was parked, in which case the syscall handler
// must return WITHOUT setting a return value: the wake writes it into
// the saved trapframe. Returns 0 if the caller has no slot to park in
// (kernel code, or the legacy process_run_ring3() path) -- callers must
// treat that as "fall back to non-blocking", not as an error to ignore.
// Terminate `pid` from OUTSIDE it -- what a force-quit does to a wedged
// process. Returns 1 if it killed something, 0 if the pid was invalid,
// already dead, or is the process currently running (this is for
// killing somebody ELSE; a process ending itself uses SYS_EXIT).
//
// The teardown is scheduler_on_exit()'s, minus the part that only makes
// sense for the running process: the slot becomes a zombie holding
// `exit_code`, the window server drops the client's windows, anyone
// blocked in waitpid is woken, and its stdout pipe's write end is
// closed so a reader sees EOF instead of hanging forever. What it does
// NOT do is switch away, because the caller is not the victim.
//
// A killed process is a zombie like any other and still has to be
// reaped -- see scheduler_poll(). The WM reaps the ones it launched.
// How big each per-process kernel stack is, in KiB -- exported only so
// the fault reporter can say what was overrun without a second copy of
// the number. The definition lives in scheduler.c beside the guard-page
// machinery it belongs to.
// --- the kernel-stack debug surface (`kstack` at the shell) ----------
//
// Everything here is diagnostic: none of it is on a hot path, and the
// kernel does not act on any of it. It exists because all three
// questions below were hand-rolled as throwaway probes during the
// overflow hunt that produced the guard pages, and the next session
// should not have to write them again.
#define SCHED_KSTACK_NAME_MAX 32
#define SCHED_KSTACK_SYSCALL_MAX 64

struct sched_kstack_info {
    int slot, pid, state, wait_reason;
    uint32_t size;       // the stack's capacity in bytes
    uint32_t used;       // high-water mark: how deep it has EVER been
    int canary_ok;       // is the magic at the stack's base intact?
    uint64_t base;       // lowest address of the stack itself
    uint64_t guard;      // the unmapped page below it
    uint64_t kernel_rsp; // where a resume would iretq from
    // ...and what it would iretq INTO, if kernel_rsp points somewhere
    // sane. `frame_ok` is 0 when it does not, which is itself a finding
    // rather than a reason to dereference it.
    int frame_ok;
    uint64_t rip, cs, rsp, ss;
    char name[SCHED_KSTACK_NAME_MAX];
};

// Fills `out` for one slot. Returns 0 for a bad index only -- an UNUSED
// slot is a successful report with state == 0, the same convention
// SYS_PROC_INFO follows so enumeration skips rather than stops.
int scheduler_kstack_info(int idx, struct sched_kstack_info *out);

// The same, for the LEGACY loader's kernel stack (process.c), which has
// no scheduler slot and is the stack a `run` or `config set` typed at
// the physical shell actually runs on. Reported through this header
// rather than process.h because apps/ may not include a kernel-internal
// one -- and leaving it out would omit the only stack whoever is
// reading the report is standing on. `slot` and `pid` come back -1.
int scheduler_kstack_legacy(struct sched_kstack_info *out);

// Per-syscall stack-depth accounting: off by default, and effectively
// free when off. Turning it ON clears the table.
void scheduler_kstack_track_set(int on);
int scheduler_kstack_track_get(void);
uint32_t scheduler_kstack_syscall_peak(int nr);
void scheduler_kstack_track_syscall(int nr); // called at syscall exit

int scheduler_kstack_kib(void);

// The lowest address of a slot's kernel stack -- the first mapped word
// above its guard page. The fault reporter starts its stack scan here
// after an overflow, since RSP itself is on the unmapped guard.
uint64_t scheduler_kstack_base(int idx);

// Unmaps the guard page below every per-process kernel stack, so an
// overflow faults instead of overwriting the neighbouring slot. Call
// once at boot, AFTER paging_enforce_wx() (which rewrites every PDE).
void scheduler_guard_pages_init(void);

// Which process slot's guard page contains `addr`, or -1 if none. The
// fault reporter asks, so a stack overflow is named as one rather than
// printed as an anonymous #PF somewhere in the kernel.
int scheduler_kstack_guard_slot(uint64_t addr);

int scheduler_kill(int pid, int exit_code);

// Fills `out` with a report on process-table slot `index`
// (0 .. scheduler_max_procs()-1). An EMPTY slot is a successful call
// reporting pid 0, not a failure -- a caller enumerating the table
// should skip it, not stop. Returns 0 only for a bad index or a NULL
// pointer.
//
// Indexed by SLOT rather than by pid so a caller can walk the whole
// table without knowing which pids exist, which is exactly what a task
// manager does. See abi/proc_info.h for the fields and for why cpu_ticks
// is a cumulative total rather than a percentage.
int scheduler_proc_info(int index, struct proc_info *out);

// How many slots that table has. The bound for the loop above.
int scheduler_max_procs(void);

int scheduler_block_current(uint64_t *regs, int reason);

// Wakes every process blocked on `reason`, handing each `value` as its
// blocking syscall's return value. Returns the number woken; 0 just
// means nobody was waiting.
//
// Safe from an interrupt handler, and deliberately limited to make that
// true: it only flips state and writes an already-saved trapframe, and
// never touches g_next_kernel_rsp, so the woken process runs at the
// next ordinary tick rather than being switched to from inside an IRQ.
int scheduler_wake(int reason, int64_t value);

// The kernel's own "while I have nothing else to do" work, in ONE
// place. Call it from any loop that is waiting rather than working;
// it never blocks, never spins and never sleeps -- the `hlt` (or the
// frame, or the disk step) stays the caller's business.
//
// What it owns today is the serial debug console, and the reason it
// exists is Milestone 41. Every GUI test tool drives the desktop over
// that console, and the console has never had an owner: it was polled
// from whichever loop happened to be running -- the physical shell's
// key wait, apps/wm/wm.c's event loop, a long `cat`, the demo's timer.
// The WM's copy is the one that matters, because when the WM becomes a
// ring-3 process (stage 4) ring 0 loses that loop, and with it the wire
// all 271 GUI checks arrive on -- silently, and in the direction that
// reads as "the test tools are broken". Naming the work here means the
// WM's departure deletes a CALL, not the capability.
//
// Deliberately NOT called from the timer tick, however tempting: a
// dispatched command can run a whole shell command (`sh cat big`),
// which blocks on the filesystem, and an interrupt handler must not.
// Nothing is lost by waiting for a normal context -- COM1's receive is
// interrupt-driven into a ring buffer (kernel/kernel/serial.h), so the
// bytes are already safe; only the line assembly is deferred.
//
// Console UPKEEP is not here on purpose (vga_cursor_tick(),
// vga_present()): those belong to whoever owns the screen, and the GUI
// desktop owns it while it is up. This is idle work that is safe
// wherever the kernel is idle.
void scheduler_idle(void);

// ---- critical sections that must not be preempted -------------------
//
// Nests (a depth counter), so an inner section cannot re-enable
// preemption an outer one was relying on. While the depth is non-zero
// scheduler_tick() still BILLS the slice that ended -- accounting stays
// honest -- it simply does not rotate.
//
// WHY THIS EXISTS. The kernel context is a scheduler participant, and a
// ring-3 process is preemptible inside a syscall, so two callers can be
// interleaved anywhere. That is fine for code with no shared state and
// silently fatal for code with some -- and the filesystem has some:
// kernel/fs/tfs3.c parses directories, inodes and data through
// module-level scratch buffers (g_blk, g_ptr_blk), so a ring-3app
// reading a file while the WM was mid-lookup overwrote the block the
// WM was reading. The WM then reported perfectly good files as missing
// or unreadable, intermittently, with no error anywhere -- the desktop
// dropping cursor shapes on roughly one boot in three under KVM.
//
// vfs.c holds this across every backend call for that reason. It is
// deliberately a general primitive rather than an fs-specific flag: the
// hazard is "shared state plus preemption", and the filesystem is
// simply where this project met it first.
//
// THE TRAP: an unbalanced disable() hangs the machine, since nothing
// will ever rotate again. Pair them on every path out, including the
// failure ones.
void scheduler_preempt_disable(void);
void scheduler_preempt_enable(void);

#endif
