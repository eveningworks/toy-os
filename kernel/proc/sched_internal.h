#ifndef KERNEL_PROC_SCHED_INTERNAL_H
#define KERNEL_PROC_SCHED_INTERNAL_H

// The process table and the state around it, shared by the files that
// make up the scheduler and by nothing else -- the userland/wm/ pattern
// (docs/decisions.md), as kernel/tty/tty_internal.h does it. Split by
// concern, as Linux splits kernel/sched/core.c from fork.c, exit.c and
// signal.c:
//
//   scheduler.c   the table, picking, switching, blocking and waking
//   sched_fork.c  making a process: spawn, fork, threads, exec, `#!`
//   sched_exit.c  ending one: exit, kill, reaping, reparenting
//   sched_job.c   signal state, process groups, sessions, stop/continue
//   sched_info.c  what it reports: proc_info, the kstack surface, KTEST hooks
//
// **EVERYTHING HERE IS PROTECTED THE WAY IT WAS AS ONE FILE** -- by
// interrupts being off or the preemption guard, never by which file a
// reader lives in. A new reader outside these five uses api/scheduler.h.

#include "scheduler.h"
#include "kstack.h"
#include "fpu.h"
#include "fs.h"
#include "signal_abi.h"
#include "context_switch.h"
#include "proc_info.h"
#include <stddef.h>
#include <stdint.h>

// See api/scheduler.h -- one definition, shared with everything that
// sizes a table per process.
#define MAX_PROCS        SCHED_MAX_PROCS
// The per-process kernel stacks, one struct kstack each -- guard page,
// canary and poison fill all come from kernel/kstack.h, which the
// legacy loader (process.c) shares so the two cannot drift. It was 8
// KiB with none of it, inside struct sched_process, and the overflow
// that produced all this is in docs/decisions.md.
#define PROC_KSTACK_SIZE  KSTACK_BYTES
#define PROC_KSTACK_GUARD KSTACK_GUARD_BYTES

// isr_common's saved-register block, as an array of 22 uint64_t
// (176 bytes) -- see isr_dispatch's comment in idt.c for the layout.
// Index mapping (derived from isr_common's push order: rax first/pushed
// earliest -> highest address, r15 last/pushed latest -> lowest
// address, i.e. regs[0]):
//   0=r15 1=r14 2=r13 3=r12 4=r11 5=r10 6=r9 7=r8 8=rbp 9=rdi 10=rsi
//   11=rdx 12=rcx 13=rbx 14=rax 15=vector 16=error_code 17=rip 18=cs
//   19=rflags 20=rsp 21=ss
#define TRAPFRAME_WORDS 22
#define TF_RDI     9
#define TF_RSI     10
#define TF_RAX     14
#define TF_VECTOR  15
#define TF_ERRCODE 16
#define TF_RIP     17
#define TF_CS      18
#define TF_RFLAGS  19
#define TF_RSP     20
#define TF_SS      21

// SCHED_ZOMBIE (Milestone 1 phase 4b, docs/roadmap.md): a process that
// has exited but hasn't been scheduler_poll()'d yet. Previously
// scheduler_on_exit() freed a slot straight to SCHED_UNUSED and
// discarded the exit code (nothing consumed it -- schedtest's own wait
// loop only ever checked alive_count, never a specific process's
// result). scheduler_spawn()'s callers DO need that result (Terminal
// reporting `ls`'s exit code the same way the physical shell's `run`
// already does), so a zombie now holds its slot -- and its exit_code --
// until scheduler_poll() explicitly reaps it. Same two-step "exit,
// then a separate reap" shape a real OS's wait()/waitpid() has, scaled
// down to this kernel's single-poller-per-process use.
// SCHED_BLOCKED: parked in a syscall, waiting for something to happen,
// and NOT runnable until scheduler_wake() says so. See
// scheduler_block_current() below for why a blocking syscall in this
// kernel has to deschedule rather than wait in place.
enum sched_state { SCHED_UNUSED = 0, SCHED_READY, SCHED_RUNNING, SCHED_ZOMBIE, SCHED_BLOCKED };

struct sched_process {
    enum sched_state state;
    // What this process is parked on while SCHED_BLOCKED. The CHANNEL
    // is what a wake matches -- an address naming the object waited on
    // (see scheduler.h) -- and the REASON is a label carried purely so
    // the `kstack` debug surface can print a word instead of a pointer.
    // Nothing ever matches on the reason. Both meaningless in any other
    // state.
    const void *wait_chan;
    int wait_reason;
    uint64_t pml4_phys;
    uint64_t kernel_rsp; // this process's saved trapframe pointer --
                          // valid whenever state != SCHED_UNUSED
    // PARKED INSIDE KERNEL CODE, not at a syscall entry. The switch is
    // the same either way; the WAKE is not. A process parked at an
    // entry is answered by writing SYS_RETRY into its trapframe and
    // letting ring 3 ask again, and one parked mid-call is answered by
    // its own C code carrying on -- there is no ring-3 loop involved
    // and its trapframe is not a return value to be written.
    uint8_t parked_in_kernel;
    int preempt_depth;    // this context's scheduler_preempt_disable()
                          // nesting -- see g_preempt_depth
    uint64_t offcpu_tsc;  // TSC ticks spent switched away, summed --
                          // what a syscall timing subtracts, see
                          // scheduler_offcpu_tsc()
    uint64_t left_tsc;    // when it was last switched away; 0 = never
    int isr_depth;        // how deep this context is inside
                          // isr_dispatch() -- see idt.h's
                          // isr_depth_get(); travels with kernel_rsp
    struct kernel_context kctx; // WHERE THIS PROCESS IS PARKED. The one
                          // suspend shape: preempted in ring 3 or
                          // blocked mid-syscall, a context is saved and
                          // resumed the same way. A process that has
                          // never run gets a hand-built one -- see
                          // proc_start_context()
    int exit_code;        // valid only once state == SCHED_ZOMBIE

    // Who spawned this process, or 0 for "the kernel did" -- the
    // shell's `spawn`, `gui`, a KTEST. There was no parent link at all
    // before this, so there was no process TREE: nothing could ask
    // which processes are a given one's children, which is what an
    // init needs to reap orphans and what a `ps` needs to draw.
    //
    // Set once at spawn and changed only by reparent_children(), which
    // is not tidiness -- see its comment for the stale-pid bug it
    // exists to prevent.
    //
    // A THREAD'S PARENT IS ITS GROUP LEADER, and every walk over a
    // process's children skips threads -- see `tgid` below.
    int ppid;

    // --- threads: which group this slot belongs to -------------------
    //
    // The pid of the thread-group LEADER, which for an ordinary process
    // is its own pid. `tgid != pid` IS the definition of "this slot is
    // a thread", Linux's, and it is what every parent/child walk here
    // filters on.
    //
    // What follows the GROUP: the address space, the fd table (keyed by
    // CR3, so shared without anything being written here), the heap and
    // the cwd -- both read through the leader's slot, never copied --
    // the parent link, and the process group. What stays PER SLOT: the
    // kernel stack, the FP state, the trapframe, the signal disposition
    // table and the thread pointer below.
    int tgid;

    // FS.base for this thread: what %fs-relative addressing in ring 3
    // resolves against, so a `__thread` variable has somewhere to live.
    // Restored by switch_to(); 0 until SYS_SET_TLS names one.
    uint64_t fs_base;

    // Nobody will join this thread, so its exit frees the slot outright
    // rather than leaving a zombie for a join that is not coming.
    uint8_t detached;

    // --- signals (abi/signal_abi.h, kernel/signal.h) -----------------
    //
    // `pending` is a BITMASK, not a queue: two SIGINTs before delivery
    // are one SIGINT, which is what ordinary Unix signals do too.
    //
    // **A SET BIT MEANS THIS PROCESS HAS SOMETHING TO DELIVER**, and
    // it used to mean the stronger "must die" -- handlers are what
    // changed it, exactly as api/scheduler.h predicted they would. An
    // ignored signal is still dropped at arrival rather than queued, so
    // a set bit is never a no-op; but whether it terminates the process
    // or calls one of its own functions now needs `actions[]` below.
    //
    // What did NOT change is the one place that leaned on the old
    // reading: scheduler_block_current() still parks whatever it is
    // handed, because delivery happens at the syscall ENTRY a process
    // makes next and it therefore cannot reach that function with
    // anything pending. Its comment says so at length.
    uint32_t pending;

    // Signals blocked from delivery right now. THE ONLY THING THAT SETS
    // A BIT HERE IS ENTERING A HANDLER, and sigreturn is the only thing
    // that clears one -- there is no sigprocmask. POSIX blocks the
    // signal a handler is running for (that is what SA_NODEFER turns
    // off), and it is not politeness: without it, holding Ctrl-C down
    // re-enters the handler on every delivery and walks a 4-page user
    // stack straight into its guard page.
    uint32_t blocked;

    // What `blocked` was before a sigsuspend swapped it, and whether one
    // is in flight. On the PROCESS rather than on a kernel frame because
    // the wait does not always end where it began: a parked process is
    // woken by somebody else, and the signal that woke it is delivered at
    // a later trap. scheduler_sigsuspend_disarm() is the one restore.
    uint32_t sigsuspend_saved;
    uint8_t  sigsuspend_armed;

    // --- job control: STOPPED, and why it is not a state -------------
    //
    // A FLAG BESIDE THE STATE RATHER THAN A FIFTH `enum sched_state`,
    // and the blocked case is the whole argument. A process suspended
    // while parked on a pipe must come back to that pipe, so a real
    // state would have to remember which state it displaced and what
    // channel that state was waiting on -- bookkeeping for a transition
    // nothing here can even exercise, because this kernel has no
    // interruptible syscalls (docs/roadmap.md) to wake a blocked
    // process into a stop. As a flag it composes with all four states
    // for free: find_next_runnable() skips it, a wake still lands and
    // leaves the slot READY-but-stopped, and SIGCONT is one clear.
    //
    // Linux makes it a state (TASK_STOPPED) because it CAN wake an
    // interruptible sleeper to stop it promptly. When interruptible
    // syscalls land here, this is the decision to revisit.
    uint8_t stopped;

    // SCHEDULING PRIORITY, nice-style: LOWER runs first, 0 is the
    // default every process starts at, and the range is POSIX's
    // -20..19. Strict between levels, round-robin within one.
    //
    // **IT EXISTS FOR ONE MEASURED REASON.** A ring-3 driver is woken
    // by its device's interrupt and then WAITS ITS TURN: at a 10 ms
    // timeslice, behind the compositor and the mixer, that was measured
    // as ~17 ms of dead air 5.6 times a second on a USB audio endpoint
    // whose buffer holds 12 ms. An in-kernel driver never sees it
    // because it refills inside the interrupt handler.
    //
    // **STRICT PRIORITY CAN STARVE.** A busy process at a better level
    // will hold the CPU against everything below it -- there is no
    // ageing here and no budget, deliberately, because the only callers
    // are drivers that block on a wakeword within microseconds of being
    // run. A CPU-bound process must not be given one.
    int8_t prio;
    // A SYSCALL REWOUND TO BE RE-ISSUED, NOT RUN YET. Set when a signal
    // wakes this process out of a park (its RIP is put back on the
    // `int $0x80`), cleared when it next enters a syscall. A signal
    // delivered at ANY trap in between -- a tick landing on the one
    // instruction before the re-issue -- must treat the syscall as not
    // run: without this a handler ran there and the read then completed
    // normally, which read as "the interrupted read was not interrupted"
    // in about one full suite in two. See signal.c's push_signal_frame().
    uint8_t syscall_reissue;
    // Has the parent been told about this stop yet? SYS_WUNTRACED
    // reports a stop ONCE, exactly as POSIX does -- otherwise a shell
    // looping on waitpid() would be handed the same suspension forever
    // and could never get back to its prompt.
    uint8_t stop_reported;
    // Which signal stopped it, for SIGNAL_STOP_BASE + sig. Meaningless
    // unless `stopped`.
    int stop_sig;

    // WHAT EACH SIGNAL DOES TO THIS PROCESS: a table now, indexed by
    // signal number, because a disposition stopped being one bit the
    // moment it could be a function pointer with a restorer and flags
    // beside it. This replaced a `uint32_t ignored` bitmask, which was
    // the right shape while SIG_DFL and SIG_IGN were the only answers.
    //
    // Costs 768 bytes per slot -- 48 KB of BSS across all 64. Paid
    // rather than compressed (a handler list keyed by signal, say)
    // because indexing by signal number is what every reader wants and
    // this kernel has 64 slots, not 64 thousand.
    struct k_sigaction actions[SIGNAL_MAX + 1];
    // This process's group. Never 0 for a live slot: a child inherits
    // its spawner's, and one the kernel started leads its own.
    int pgid;

    // --- prepare_to_wait, and the lost wakeup it closes --------------
    //
    // **A WAKE ONLY FINDS A PROCESS THAT IS ALREADY BLOCKED.** Between
    // "is there anything to do?" and the park, a process is RUNNING, so
    // scheduler_wake() skips it and the wake is dropped -- the process
    // then parks forever. With interrupts off for the whole syscall
    // that window did not exist, which is why this had to be built
    // before the trap gate could land.
    //
    // Linux's answer is prepare_to_wait(): announce the wait BEFORE
    // testing the condition, so a wake in the window has something to
    // land on. Here that announcement is per PROCESS rather than a
    // wait-queue entry, which needs no allocation and no channel table.
    const void *arm_chan;   // the channel this process is about to wait on
    int64_t     arm_value;  // what a wake in the window carried
    int         armed_woken;// a wake arrived before the park -- do not park
    // THE SESSION, which is what a controlling terminal belongs to.
    // Inherited like `pgid`, and changed only by SYS_SETSID -- so
    // everything a shell starts stays in the shell's session and may
    // take the terminal from it, which is the rule POSIX states and the
    // reason a nested shell works at all. A process the kernel started
    // leads its own. Never 0 for a live slot.
    int sid;

    // SYS_NOTIFY_READY: this process has said it finished starting up.
    // The kernel attaches NO meaning to it and never acts on it -- it
    // is reported through scheduler_proc_info() and init is the only
    // reader (see abi/syscall_abi.h's entry). Reset at spawn like every
    // other per-tenant field, because a slot is reused and inheriting a
    // ready bit would report a service as up before it had run an
    // instruction.
    int ready;

    // When a SCHED_WAIT_TIMER sleeper wants to be woken, in
    // clocksource nanoseconds. Meaningless in any other state.
    uint64_t wake_at_ns;
    // Pipe index this process's stdout goes to, or -1 for the console.
    // Lives here rather than in the fd table because fd 1 has always
    // been a hardcoded console in SYS_WRITE -- see scheduler.h.

    // --- what a task manager needs to show (see api/proc_info.h) ------
    //
    // None of this existed: the table held state, page tables, a kernel
    // stack and FPU state, so "which process is this and what is it
    // costing" had no answer anywhere in the kernel.
    //
    // The program's name, from the spawn path's last component. Stored
    // rather than derived because the path is the caller's buffer and
    // does not outlive the call.
    char name[PROC_NAME_MAX];

    // The FULL spawn path ("/bin/wm/apps/notepad"), not just the last
    // component. `name` above is deliberately short because a task
    // manager column is, and that is the right call for DISPLAY -- but
    // a basename is not an identity: two programs in different
    // directories can share one.
    //
    // This is what the window server keys a window's application
    // identity on (see win_server.c's create_window). The alternative
    // was to trust a string each app declares about itself, and that
    // cannot be made safe: two apps declaring the same one silently
    // raise each other's windows, and no runtime check can tell that
    // apart from the legitimate case of two copies of ONE program,
    // which must share an identity. A path the kernel derives cannot be
    // misdeclared. Windows falls back to the executable for exactly
    // this, and Wayland's app_id is only trustworthy because a
    // compositor matches it against a .desktop FILE.
    char exec_path[FS_PATH_MAX];

    // Timer ticks this process has been the RUNNING one for. Cumulative
    // and monotonic; a percentage is the DIFFERENCE between two reads
    // divided by the ticks elapsed between them, which is the consumer's
    // job -- storing a percentage here would bake in a sampling interval
    // the kernel has no business choosing.
    uint64_t cpu_ns;   // measured, not counted -- see bill_current()
    // CPU consumed, for PICKING -- see find_next_runnable(). cpu_ns is the
    // report; this one is moved forward on a wake (vr_place()) so a long
    // sleeper cannot come back owed a monopoly.
    uint64_t vruntime;

    // This process's SYS_SBRK state: the break it can see, and how far
    // physical pages have actually been mapped behind it (separate,
    // because sbrk only maps on first crossing into a page).
    //
    // PER PROCESS rather than the file-global pair syscall.c used to
    // hold, and that was not a tidy-up: those globals are armed only by
    // syscall_reset_mm(), which only elf_run.c's legacy blocking
    // loader calls -- so a SCHEDULER-spawned process, which is every GUI
    // app and everything `gui spawn` starts, had no heap armed and
    // SYS_SBRK returned -1 for it unconditionally. Nothing noticed
    // because nothing spawned had ever asked for memory. A ring-3
    // compositor asks for a whole screen of back buffer on its first
    // line (M41 stage 4b), which is how this surfaced.
    struct sched_mm mm;
    // This process's current directory (scheduler.h's struct sched_cwd).
    // Armed at creation from whatever spawned it, so a child starts
    // where its parent was standing -- the property that makes
    // `mkdir docs` typed in a subdirectory mean the same thing to a
    // /bin program as to a shell builtin.
    struct sched_cwd cwd;
    // This process's x87/SSE registers while it isn't the one running.
    // 16-byte aligned because FXSAVE/FXRSTOR #GP otherwise -- see fpu.h,
    // including why only ring-3 processes need one of these at all.
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
};

// ---- shared state -----------------------------------------------------

// scheduler.c
extern struct kstack kstacks[MAX_PROCS];
extern struct sched_process procs[MAX_PROCS];
extern int g_init_pid;
extern int current_index;
extern volatile int alive_count;
extern int rotation_pos;
extern uint32_t kstack_peak[MAX_PROCS];
extern int g_need_resched;
extern uint64_t kernel_vruntime;
extern uint64_t g_min_vruntime;

void proc_name_from_path(char *dst, int cap, const char *path);
int slot_claim(void);
void slot_unclaim(int slot);
int is_thread(int idx);
int leader_index(int idx);
uint64_t kernel_stack_top(int idx);
uint64_t kernel_stack_base(int idx);
void kstack_arm_slot(int idx);
void vr_place(int rot);
int find_next_runnable(int start);
void trace_sched(const char *what, int idx);
void proc_start_context(int slot, const uint64_t *tf);
void switch_to(int from, int idx);
void switch_to_kernel(int from);
void bill_current(void);

// sched_fork.c
int spawn_from_fs(const char *path, const char *argvec, size_t argvec_len,
                  int stdout_desc, int stdin_desc, int stderr_desc,
                  const char *env, int want_pgid, uint64_t parent_pml4);

// sched_exit.c
void group_release_threads(int leader);
void reparent_children(int dead_pid);

// sched_job.c
void signal_state_reset(int slot);

// The kernel context's place in the rotation -- see scheduler.c, "THE
// KERNEL CONTEXT AS A ROTATION PARTICIPANT".
#define ROT_KERNEL MAX_PROCS

// **THE SWITCH CRITICAL SECTION RUNS WITH INTERRUPTS OFF, AND NOTHING
// TURNS THEM BACK ON** -- the `iretq` that completes the handover
// restores the INCOMING context's RFLAGS, so IF returns with the
// process it belongs to.
//
// The section runs from "this process stops being current" to the
// process_context_restore_noirq() that moves the CPU. It used to be
// far longer and far worse: a switch only NOMINATED the incoming
// trapframe, so the outgoing context then walked all the way back out
// to isr.asm's epilogue while `current_index` already named the
// incoming one -- and a tick landing in that gap had scheduler_tick()
// write the OUTGOING trapframe into the INCOMING slot, leaving the
// incoming process resumed at somebody else's frame for as long as it
// lived (runnable, making no progress). The gap is a few instructions
// now, but it is still a gap.
//
// An interrupt gate hid this: IF is clear for the whole syscall, so
// nothing could land there. A trap gate does not, which is what made it
// visible. Linux holds the runqueue lock with IRQs off across
// __schedule() for exactly this window.
static inline void sched_switch_begin(void) {
    __asm__ volatile ("cli" ::: "memory");
}

#endif
