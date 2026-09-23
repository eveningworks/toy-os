// Preemptive round-robin scheduler for ring-3 processes -- built as the
// ORIGINAL Milestone 16 (the old numbering in the git history, not
// docs/roadmap.md's current Milestone 16).
//
// DESIGN
// ------
// Every milestone through M15 ran at most one ring-3 process at a time,
// synchronously: process_run_ring3() (process.c) drops to ring 3 and
// gets control back only when that process calls the exit syscall,
// via a setjmp/longjmp-style save/restore of the CALLER's kernel
// context (g_process_ctx in process.c). That's a real, useful
// mechanism, but it's fundamentally a function call, not scheduling --
// nothing else can run while a process is "in flight" through it.
//
// This file adds honest preemptive multitasking on top, WITHOUT
// touching that mechanism: the two coexist, chosen per-syscall by
// whether the exiting process is scheduler-managed (see
// scheduler_current_pid(), used by syscall.c). Every one of the M8-M15
// test commands keeps using process_run_ring3() untouched and is
// provably unaffected (see below).
//
// The core trick: isr_common (isr.asm) already saves a process's full
// register state (15 GP regs + vector + error code + the CPU-pushed
// rip/cs/rflags/rsp/ss) onto whatever stack was active when the
// interrupt fired, calls isr_dispatch(regs) with a pointer to it, then
// -- unmodified through M15 -- just pops those same registers back off
// THE SAME stack and iretq's, resuming exactly what was interrupted.
//
// M16 generalizes that last step: isr_common now reloads rsp from a
// global, g_next_kernel_rsp (defined in idt.c), immediately before the
// pop+iretq sequence. isr_dispatch sets it to `regs` (i.e. a no-op --
// resume what was interrupted) at the very top of the function, for
// EVERY vector, unconditionally. Only scheduler_tick() (called for
// vector 32, the timer, and only when armed) or scheduler_on_exit()
// (called from syscall.c's SYS_EXIT handler) ever override it, to
// point at a DIFFERENT saved register block instead -- another
// process's, or back to whatever kernel code (the shell, blocked in
// scheduler_demo_run()'s wait loop) was running before any process got
// the CPU.
//
// This works uniformly for every case that matters here:
//   - Switching between two ring-3 processes: each gets its own
//     dedicated kernel stack (proc.kstack), used as the CPU's RSP0 (via
//     gdt_set_kernel_stack()) while that process is the one running --
//     so if it's interrupted, its register block lands on ITS OWN
//     stack, not shared with any other process. Switching processes is
//     just: point g_next_kernel_rsp at the other one's saved block,
//     switch CR3 (vmm_switch_address_space -- safe mid-ISR because
//     every process's PML4 shares kernel entry 0, see vmm.h), and
//     repoint RSP0 for next time.
//   - Launching a process for the FIRST time: its "saved register
//     block" is synthesized once, in spawn_from_fs() below, instead
//     of being the product of a real interrupt -- but it's laid out
//     identically (r15..rax zeroed, rip/cs/rflags/rsp/ss set to the
//     ELF's entry point and a fresh user stack), so isr_common's
//     ordinary epilogue can't tell the difference. This unifies "first
//     launch" and "resume after preemption" into one mechanism.
//   - Switching back to the kernel/shell (scheduler_demo_run()'s wait
//     loop) once no process is ready: kernel code is ring 0, so this is
//     a same-privilege interrupt return (iretq only restores
//     rip/cs/rflags, not rsp/ss, since there was no stack switch) --
//     the exact same "just point g_next_kernel_rsp elsewhere" trick
//     handles it too, using whatever real stack the shell's wait loop
//     was actually using when last interrupted (kernel_saved_rsp,
//     refreshed every tick that finds no process running).
//
// SAFETY FOR M8-M15, AND EVERYTHING SPAWNED NEITHER BY schedtest NOR
// Terminal's async run/ls (Milestone 1 phase 4b, docs/roadmap.md)
// ---------------------------------------------------------------------
// scheduler_armed is set true once, permanently, in scheduler_init()
// ("continuously armed" -- the roadmap item this generalizes from
// demo-only) rather than being flipped on/off around
// scheduler_demo_run()'s own wait loop the way it used to be. This is
// still safe for every M8-M15 test command and every legacy
// elf_run_from_fs() caller (`run`/`ls` from the physical shell) despite
// being permanently on: scheduler_tick() being armed only matters once
// something is actually in the process table (alive_count > 0) --
// find_next_ready() scanning an all-SCHED_UNUSED table always returns
// -1, so every tick that finds nothing ready just re-confirms
// g_next_kernel_rsp at whatever isr_dispatch's default already set it
// to (`regs`, i.e. a genuine no-op, byte-for-byte the same as the old
// disarmed early-return). scheduler_on_exit() is likewise only ever
// reached via syscall.c's `if (scheduler_current_pid())` guard, and
// scheduler_current_pid() returns 0 whenever current_index is -1 --
// which it always is unless something was actually spawned through
// this file's spawn_from_fs(). So every existing test command's exit
// path (the old process_context_restore(&g_process_ctx, ...) call in
// syscall.c) is completely untouched by this file, exactly as before --
// only the mechanism that used to keep it that way (a flag flipped
// off) changed to a different one (an empty table).
#include "scheduler.h"
#include "futex.h"
#include "mmap.h"   // a fork's view of the mmap arena
#include "shm.h"    // shm_process_gone() -- undoing a half-inherited arena, and an exec
#include "sound.h"  // sound_process_gone() -- an exec drops the stream
#include "syscalls.h" // the fd table: a child inherits its parent's descriptors
#include "remote_log.h" // a session created from a socket is a REMOTE session
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "auxv.h" // the dynamic handoff, see spawn_from_fs()
#include "elf_run.h"
#include "process.h" // process_context_is_armed() -- see kernel_slot_runnable()
#include "context_switch.h" // the ONE suspend shape -- see switch_to()
#include "win_role.h"
#include "syscall.h" // syscall_process_kill_cleanup()
#include "win_input.h" // raw input to a ring-3 compositor // win_server_client_gone() -- see scheduler_on_exit()
#include "diag.h"     // diag_provider_gone() -- drop a dead service's name
#include "netdev.h"    // net_poll() -- the idle half of the receive path
#include "pipe.h"      // pipe_close_writer() when a piped child exits
#include "kstack.h"    // the guard page, canary and poison fill
#include "kfmt.h"      // klog_printf, vga_printf
#include "idt.h"     // isr_depth_get()/_set() -- the depth travels with kernel_rsp
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "signal_abi.h" // the pending mask and the group each process carries
#include "signal.h"     // signal_send() -- SIGCHLD to a parent, see notify_parent()
#include "errno.h"     // -EINTR, what a signal makes a blocking syscall return
#include "fs.h"
#include "heap.h"
#include "gdt.h"
#include "fpu.h"
#include "tls.h"    // FS.base -- a thread pointer is per THREAD, see switch_to()
#include "vga.h"
#include "klog.h"
#include "strace.h"
#include "uaddr.h"
#include "clocksource.h" // CPU time is measured, not counted -- bill_current()
#include "debug_console.h"
#include "ata_cache.h" // the idle work scheduler_idle() owns
#include "input.h"     // input_poll_sources() -- ditto, for a device with no IRQ
#include "string.h" // k_strlcpy -- proc_name_from_path()
#include <stddef.h>

// The thread pointer belonging to the KERNEL CONTEXT -- which in
// practice means a ring-3 process the legacy elf_run.c loader is
// running, since ring 0 itself never reads %fs. See
// scheduler_set_tls().
static uint64_t kernel_fs_base;

// A SWITCH MOVES THE CPU ITSELF; NOTHING NOMINATES A FRAME ANY MORE.
// switch_to() saves the outgoing context and restores the incoming one
// on the spot (Linux's __switch_to_asm, NT's SwapContext), so a resumed
// context returns out through the dispatch it parked in and that
// dispatch's own epilogue iretqs from the frame it arrived on. What
// this replaced -- g_next_kernel_rsp, then a per-dispatch resume slot
// with a deferred (slot, depth) nomination beside it -- is gone with
// it; see docs/blocking-design.md.

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

static struct kstack kstacks[MAX_PROCS];
// The user stack's address and size, plus the guard region below it,
// come from uaddr.h -- this spawn path and elf_run.c's legacy loader
// build the SAME ring-3 layout, and used to say so in two places with
// nothing keeping them equal.

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

// "/bin/wm/demos/uidemo" -> "uidemo". A task manager column is a few
// characters wide, so the last component is the useful part and the
// path is not kept at all (see abi/proc_info.h).
static void proc_name_from_path(char *dst, int cap, const char *path) {
    if (cap <= 0) return;
    dst[0] = '\0';
    if (!path) return;

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    // A path ending in '/' leaves nothing; keep the whole thing rather
    // than reporting an empty name, which would read as a kernel bug.
    if (!*base) base = path;
    k_strlcpy(dst, base, (size_t)cap);
}


// Everything a slot's SIGNAL state has to forget before somebody else
// gets it -- pending bits, dispositions, and any suspension.
//
// ONE FUNCTION FOR THREE CALLERS (spawn, the KTEST slot fabricator, and
// the release beside it) because the file already predicted the failure
// mode in prose: "one of the two places would eventually be the one
// that got forgotten". Adding a third field made that concrete -- a
// stopped bit surviving into the next tenant is a process that never
// runs and gives no reason.
static void signal_state_reset(int slot);

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

// scheduler.h deliberately does not include fs.h, so struct sched_cwd
// spells its size as a literal. This is what stops the two drifting.
_Static_assert(sizeof(((struct sched_cwd *)0)->path) == FS_PATH_MAX,
               "struct sched_cwd::path must match FS_PATH_MAX");
_Static_assert(sizeof(((struct mmap_region *)0)->path) == FS_PATH_STORED_MAX,
               "mmap_region.path is a STORED path (fs.h): 32 per process, so it "
               "takes the smaller bound and mmap REFUSES anything longer");

static struct sched_process procs[MAX_PROCS];

// Defined further down, beside scheduler_kill() -- its other caller.
static void reparent_children(int dead_pid);

// The pid init holds, or 0 on a boot that has no init (nothing spawned
// it, or /bin/init is missing). Everything that treats pid 1 specially
// asks this rather than testing `pid == 1`, so a boot without an init
// behaves exactly as this kernel did before one existed -- rather than
// adopting orphans to a pid nobody is running and refusing to kill
// whatever happens to be in slot 0.
static int g_init_pid = 0;

int scheduler_init_pid(void) { return g_init_pid; }
void scheduler_set_init_pid(int pid) { g_init_pid = pid; }

// When the CURRENT occupant of the CPU (a process, or the kernel
// context when current_index is -1) started running, in clocksource
// nanoseconds. bill_current() is the only thing that reads or moves it.
static uint64_t g_run_start_ns = 0;

static int current_index = -1;    // -1 = kernel/shell in control, not
                                    // a scheduler-managed process
static int scheduler_armed = 0;
static int kernel_saved_isr_depth = 0; // the kernel slot's isr_depth_get()
static int kernel_preempt_depth = 0;   // and its scheduler_preempt_depth()
static struct kernel_context kernel_kctx; // and where the kernel context
                                          // itself is parked -- rip 0
                                          // until it has been left once
static uint64_t kernel_saved_rsp = 0; // refreshed every tick that finds
                                        // current_index == -1
static volatile int alive_count = 0;

// THE KERNEL CONTEXT AS A ROTATION PARTICIPANT
// --------------------------------------------
// Through Milestone 1 phase 4b the kernel context was not scheduled at
// all: it resumed only on a tick that found NOTHING ready, so any ready
// ring-3 process starved it completely until every one of them exited.
// That is what froze wm_run() for the whole lifetime of a spawned
// process -- the Terminal's async spawn only looked live because a
// process's output reaches the screen from inside its own SYS_WRITE
// handler (see userland/wm/wm.c's per-frame poll comment), not because the
// WM was drawing.
//
// The kernel now takes a position in the same round-robin cycle a
// process does, so wm_run() keeps drawing, routing input and polling
// while ring-3 processes run. It deliberately does NOT become a
// struct sched_process: it has no address space of its own (kernel code
// is correct under any process's CR3 -- every PML4 shares kernel entry
// 0, see vmm.h), no FP state worth saving (kernel and apps/ are built
// -mno-sse, see switch_to()'s comment), no kstack of its own (ring 0
// interrupting ring 0 doesn't switch stacks, so its trapframe lands on
// whatever kernel stack it was already using), and no slot to reap.
// All it needs is a position in the cycle and the saved trapframe
// pointer this file already kept for it.
#define ROT_KERNEL MAX_PROCS

// Where the rotation last stopped: 0..MAX_PROCS-1 for a process slot,
// ROT_KERNEL for the kernel context. Deliberately separate from
// current_index, which still means exactly what it always did (-1
// whenever a scheduler-managed process is NOT the thing running) --
// syscall.c depends on that through scheduler_current_pid(), and
// conflating the two would change every M8-M15 exit path.
static int rotation_pos = ROT_KERNEL;

// --- thread groups ---------------------------------------------------
//
// A thread is a slot whose leader is somebody else. Everything a
// PROCESS owns is read through the leader's slot, so there is one copy
// of it however many threads share it -- which is the difference
// between this and two processes that happen to share a page table.

static int is_thread(int idx) {
    return procs[idx].tgid != idx + 1;
}

// The slot holding what this group shares. Falls back to `idx` for a
// group whose leader is already gone -- unreachable while a thread runs
// (the group dies as a unit) and it keeps every caller here total.
static int leader_index(int idx) {
    int lead = procs[idx].tgid - 1;
    if (lead < 0 || lead >= MAX_PROCS) return idx;
    return lead;
}

static uint64_t kernel_stack_top(int idx) {
    return kstack_top(&kstacks[idx]);
}

static uint64_t kernel_stack_base(int idx) {
    return kstack_base(&kstacks[idx]);
}

// How deep each slot's stack has ever been, in bytes. Only ever grows
// within one process's life; reset when the slot starts a new one.
static uint32_t kstack_peak[MAX_PROCS];

// Called wherever a stack starts a new life. The trapframe the spawn
// path has just written at the top is what `reserve_top` protects.
static void kstack_arm_slot(int idx) {
    kstack_arm(&kstacks[idx], TRAPFRAME_WORDS * 8, &kstack_peak[idx]);
}

// The canary check. Deliberately fatal rather than a log line: the
// canary being gone means something has already written outside its
// stack, so every piece of state this scheduler is about to act on is
// suspect -- and carrying on is exactly how the original bug presented,
// as a fault in an innocent process several context switches later.
static void kstack_verify(int idx) {
    if (kstack_canary_ok(&kstacks[idx])) return;
    klog_printf("KERNEL STACK OVERFLOW: slot %d (pid %d, \"%s\") overran its "
                "%d-byte stack -- canary at %lx destroyed\n",
                idx, idx + 1, procs[idx].name, PROC_KSTACK_SIZE,
                kernel_stack_base(idx));
    vga_printf("KERNEL STACK OVERFLOW: pid %d (\"%s\") overran its kernel stack\n",
               idx + 1, procs[idx].name);
    // There is no panic() to call -- the panic machinery lives in the
    // fault handler, and going through it is what buys the function
    // name, the registers and the stack scan. `ud2` is the cheapest way
    // in, and the line above says what it really was.
    __asm__ volatile ("ud2");
}

// Unmaps the guard page below every kernel stack. Called from
// kernel_main() AFTER paging_enforce_wx(), which rewrites every PDE and
// would otherwise put the huge page back.
// --- the kernel-stack debug surface (`kstack` at the shell) ----------
//
// Three questions this answers, each of which was a hand-rolled
// throwaway probe during the overflow hunt that produced this file's
// guard pages:
//
//   1. how close is each process to the edge?   (the high-water mark)
//   2. what is a slot about to be resumed INTO? (the saved trapframe --
//      seeing cs=0 rip=0 on one is what named that bug, after three
//      wrong theories)
//   3. which SYSCALL is responsible for the depth? (below)
int scheduler_kstack_kib(void) { return PROC_KSTACK_SIZE / 1024; }

int scheduler_kstack_info(int idx, struct sched_kstack_info *out) {
    if (idx < 0 || idx >= MAX_PROCS || !out) return 0;
    k_memset(out, 0, sizeof *out);
    out->slot       = idx;
    out->pid        = idx + 1;
    out->state      = (int)procs[idx].state;
    out->size       = PROC_KSTACK_SIZE;
    out->base       = kernel_stack_base(idx);
    out->guard      = (uint64_t)&kstacks[idx].guard[0];
    out->kernel_rsp = procs[idx].kernel_rsp;
    out->wait_reason = procs[idx].wait_reason;
    k_strlcpy(out->name, procs[idx].name, sizeof out->name);
    if (procs[idx].state == SCHED_UNUSED) return 1;

    out->used = kstack_used(&kstacks[idx], &kstack_peak[idx]);
    out->canary_ok = kstack_canary_ok(&kstacks[idx]);

    // The saved trapframe, which is the thing a resume will iretq from.
    // Bounds-checked against this slot's own stack rather than trusted:
    // a kernel_rsp pointing anywhere else is itself the finding, and
    // dereferencing it would turn a diagnostic into a second fault.
    uint64_t rsp = procs[idx].kernel_rsp;
    if (rsp >= out->base && rsp + TRAPFRAME_WORDS * 8 <= kernel_stack_top(idx)) {
        const uint64_t *f = (const uint64_t *)(uintptr_t)rsp;
        out->rip = f[TF_RIP];
        out->cs  = f[TF_CS];
        out->rsp = f[TF_RSP];
        out->ss  = f[TF_SS];
        out->frame_ok = 1;
    }
    return 1;
}

int scheduler_kstack_legacy(struct sched_kstack_info *out) {
    if (!out) return 0;
    k_memset(out, 0, sizeof *out);
    out->slot = -1;
    out->pid  = -1;
    out->size = KSTACK_BYTES;
    out->used = process_kstack_used();
    out->base = process_kstack_base();
    out->canary_ok = process_kstack_canary_ok();
    k_strlcpy(out->name, "(legacy loader)", sizeof out->name);
    return 1;
}

// Per-syscall depth accounting. OFF by default and effectively free
// when off; when on, every syscall exit asks how deep this stack has
// ever been and attributes any GROWTH to the syscall that just ran.
//
// It attributes the PEAK, not this call's own usage, which is the
// honest thing a cheap implementation can say: the peak is a property
// of the stack, and the syscall recorded against it is the one that was
// running when it got that deep. Good enough to rank the expensive
// paths, which is the question worth asking.
static int g_kstack_track;
static uint32_t g_syscall_peak[SCHED_KSTACK_SYSCALL_MAX];

void scheduler_kstack_track_set(int on) {
    g_kstack_track = on ? 1 : 0;
    if (on) {
        for (int i = 0; i < SCHED_KSTACK_SYSCALL_MAX; i++) g_syscall_peak[i] = 0;
    }
}

int scheduler_kstack_track_get(void) { return g_kstack_track; }

uint32_t scheduler_kstack_syscall_peak(int nr) {
    if (nr < 0 || nr >= SCHED_KSTACK_SYSCALL_MAX) return 0;
    return g_syscall_peak[nr];
}

void scheduler_kstack_track_syscall(int nr) {
    if (!g_kstack_track) return;
    if (nr < 0 || nr >= SCHED_KSTACK_SYSCALL_MAX) return;

    uint32_t used, before;
    if (current_index >= 0) {
        before = kstack_peak[current_index];
        used = kstack_used(&kstacks[current_index], &kstack_peak[current_index]);
    } else if (process_context_is_armed()) {
        before = process_kstack_peak();
        // The LEGACY loader's process: no scheduler slot, but its
        // syscalls land on a real kernel stack all the same -- and it is
        // the deepest path measured so far (8680 bytes for `config set`
        // at the shell). Skipping it would leave the tracking blind to
        // exactly the case that first overflowed.
        used = process_kstack_used();
    } else {
        return;  // the kernel context itself -- not a per-process stack
    }
    // Attribute only a syscall that actually PUSHED the high-water
    // down. The mark is a property of the stack, not of a call, so
    // recording it unconditionally credits every later syscall with the
    // deepest one's number -- measured, and it made `write` look as
    // expensive as the setting write that really did it.
    if (used > before && used > g_syscall_peak[nr]) g_syscall_peak[nr] = used;
}

uint64_t scheduler_kstack_base(int idx) {
    if (idx < 0 || idx >= MAX_PROCS) return 0;
    return kernel_stack_base(idx);
}

void scheduler_guard_pages_init(void) {
    int ok = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (kstack_guard_arm(&kstacks[i])) ok++;
    }
    klog_printf("sched: %d/%d kernel-stack guard pages armed (%d KiB stacks, "
                "guards %lx..%lx)\n",
                ok, MAX_PROCS, PROC_KSTACK_SIZE / 1024,
                (uint64_t)&kstacks[0].guard[0],
                (uint64_t)&kstacks[MAX_PROCS - 1].guard[PROC_KSTACK_GUARD]);
}

// Which slot's guard page contains `addr`, or -1. The fault reporter
// asks, so a page fault on a guard page is reported as what it is
// instead of as an anonymous #PF in the middle of the kernel.
int scheduler_kstack_guard_slot(uint64_t addr) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (kstack_guard_contains(&kstacks[i], addr)) return i;
    }
    return -1;
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_PROCS; i++) procs[i].state = SCHED_UNUSED;
    current_index = -1;
    rotation_pos = ROT_KERNEL;
    // Permanently armed from here on -- see this file's top comment on
    // why that's safe with an empty process table. Was `= 0` (disarmed,
    // only scheduler_demo_run() ever flipped it) before Milestone 1
    // phase 4b generalized this from a one-off demo to a real,
    // continuously-available spawn mechanism.
    scheduler_armed = 1;
    kernel_saved_rsp = 0;
    kernel_saved_isr_depth = 0;
    k_memset(&kernel_kctx, 0, sizeof kernel_kctx);
    alive_count = 0;
}

// Whether the kernel context is a runnable participant right now.
//
// Normally it is -- that's the whole point of ROT_KERNEL. The one
// exception is the LEGACY BLOCKING PATH (process_run_ring3(),
// process.c), which runs a ring-3 process WITHOUT giving it a procs[]
// slot: from this file's point of view that process's trapframe simply
// IS "the kernel context" (current_index stays -1, so every tick stores
// its regs into kernel_saved_rsp). Rotating away from it and back would
// resume a ring-3 process under whatever CR3 and RSP0 the scheduler
// process left behind -- a foreign address space and a shared kernel
// stack -- so while one is in flight the kernel position drops out of
// the rotation entirely and this file behaves exactly as it did before,
// preserving every M8-M15 test command and every elf_run_from_fs()
// caller (`run`/`ls` from the physical shell) unchanged.
//
// process_context_is_armed() (process.h) is precisely the predicate "a
// blocking ring-3 process is in flight", so it is reused directly
// rather than tracking a second flag here that could drift out of
// agreement with it.
static int kernel_slot_runnable(void) {
    if (process_context_is_armed()) return 0;
    // Never select the kernel before a tick has captured a real
    // trapframe for it -- g_next_kernel_rsp = 0 would iretq into
    // nothing. Unreachable in practice (the kernel is always what's
    // running when the first spawn happens, so the very next tick saves
    // it before any switch away can occur), but this is a boot-critical
    // path and the check is one compare.
    return kernel_saved_rsp != 0;
}

// Scans the whole rotation -- all MAX_PROCS process slots PLUS the
// kernel's own position -- starting just after `start` (wrapping), and
// returns the first runnable one. Falls back to ROT_KERNEL when nothing
// else is runnable, which is the pre-rotation behaviour: a tick that
// finds no ready process resumes the kernel, exactly as before.
//
// `start` may be -1 (nothing was running); the +MAX_PROCS+1 term keeps
// the modulo positive for it.
// A process's scheduling priority, by pid. The picker above is the
// only reader; these are the only writers.
int scheduler_set_priority(int pid, int value) {
    int idx = pid - 1;
    if (idx < 0 || idx >= MAX_PROCS || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    procs[idx].prio = (int8_t)value;
    return 0;
}

int scheduler_get_priority(int pid, int *value) {
    int idx = pid - 1;
    if (idx < 0 || idx >= MAX_PROCS || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    *value = procs[idx].prio;
    return 0;
}

// A WAKE THAT OUTRANKS WHAT IS RUNNING asks for a switch on the way out
// of the trap it happened in (scheduler_trap_exit()) -- Linux's
// TIF_NEED_RESCHED. Only a STRICTLY better level: a wake at the same
// level would preempt on every interrupt and give nothing back.
static int g_need_resched;

// The level a wake has to beat: the running process's, or the kernel
// slot's default when the kernel context is what runs.
static int running_prio(void) {
    return current_index >= 0 ? procs[current_index].prio : 0;
}

static int runnable_at(int idx) {
    return procs[idx].state == SCHED_READY && !procs[idx].stopped;
}

// WHOM A WAKE PREEMPTED, per level, as a rotation index + 1 (0: nobody).
// It is served FIRST the next time its level runs -- the head of its
// queue, Windows' rule for a preempted thread, where a quantum that ran
// out goes to the tail. Without it the scan restarted after the
// preempting driver on every wake, so the slots following the driver's
// always won and one scanned late went unserved (soundd, 762 ms, which
// is a dropout).
#define PRIO_MIN (-20)
#define PRIO_LEVELS 40
static uint8_t g_preempted[PRIO_LEVELS];

static void note_preempted(int rot_idx, int prio) {
    if (prio >= PRIO_MIN && prio < PRIO_MIN + PRIO_LEVELS)
        g_preempted[prio - PRIO_MIN] = (uint8_t)(rot_idx + 1);
}

static int find_next_runnable(int start) {
    // THE BEST LEVEL PRESENT, first. Strict priority between levels and
    // round-robin within one, which is what keeps the rotation fair
    // among equals while letting a woken driver in ahead of the
    // desktop. The kernel's own slot sits at the default level, so it
    // is not starved by ordinary processes and IS outranked by a
    // driver -- which is the point.
    int best = 127;
    for (int idx = 0; idx < MAX_PROCS; idx++)
        if (runnable_at(idx) && procs[idx].prio < best) best = procs[idx].prio;
    if (kernel_slot_runnable() && 0 < best) best = 0;

    // Consulted once, used or not: a head that has since blocked or
    // exited is simply dropped.
    if (best >= PRIO_MIN && best < PRIO_MIN + PRIO_LEVELS) {
        int head = g_preempted[best - PRIO_MIN] - 1;
        g_preempted[best - PRIO_MIN] = 0;
        if (head == ROT_KERNEL && kernel_slot_runnable()) return ROT_KERNEL;
        if (head >= 0 && head < MAX_PROCS && runnable_at(head) &&
            procs[head].prio == best)
            return head;
    }

    for (int i = 1; i <= MAX_PROCS + 1; i++) {
        int idx = (start + i + MAX_PROCS + 1) % (MAX_PROCS + 1);
        if (idx == ROT_KERNEL) {
            if (kernel_slot_runnable() && best >= 0) return ROT_KERNEL;
            continue;
        }
        // STOPPED IS CHECKED HERE AND NOWHERE ELSE. One picker means
        // one place suspension has to be honoured -- see the field's
        // comment in struct sched_process for why it is a flag rather
        // than a state.
        if (runnable_at(idx) && procs[idx].prio == best) return idx;
    }
    return ROT_KERNEL;
}

// Hands the CPU to `idx`, including its floating-point registers.
//
// The FXRSTOR is unconditional and has no matching "was it dirty?"
// check -- that's the eager model fpu.h argues for. It also means a
// process can never observe another process's XMM/x87 contents, which
// the lazy alternative got wrong badly enough to become a CVE.
//
// Note there's no restore for the kernel side (current_index == -1):
// the kernel and apps/ are built `-mno-sse` and have no FP state to
// preserve. If that ever stops being true, this is one of the two
// places that has to grow a save (the other is scheduler_tick()'s
// outgoing branch), and the ISR path becomes a third -- see fpu.h.
// The kernel slot's half of "the resume frame and the ISR depth travel
// together". Every site that captured kernel_saved_rsp captured only
// half the context before this existed.
static void save_kernel_frame(uint64_t *regs) {
    kernel_saved_rsp = (uint64_t)regs;
}

// A ring of the last few scheduler transitions, for a state that cannot
// happen -- a process left SCHED_RUNNING while nothing is current, say.
// **CONSECUTIVE IDENTICAL ENTRIES COLLAPSE**, or five seconds of an idle
// rotation pushes out the transition that caused the trouble: the ring
// is short on purpose (see CLAUDE.md on a probe outrunning its log).
#define SCHED_TRACE_N 24
static struct sched_trace_ent {
    const char *what;
    int idx, cur, repeat, state;
} g_trace[SCHED_TRACE_N];
static unsigned g_trace_n;

static void trace_sched(const char *what, int idx) {
    if (g_trace_n) {
        struct sched_trace_ent *last = &g_trace[(g_trace_n - 1) % SCHED_TRACE_N];
        if (last->what == what && last->idx == idx && last->cur == current_index) {
            last->repeat++;
            return;
        }
    }
    struct sched_trace_ent *e = &g_trace[g_trace_n % SCHED_TRACE_N];
    e->what = what; e->idx = idx; e->cur = current_index; e->repeat = 0;
    e->state = (idx >= 0 && idx < MAX_PROCS) ? (int)procs[idx].state : -1;
    g_trace_n++;
}

void scheduler_trace_dump(void) {
    unsigned first = g_trace_n > SCHED_TRACE_N ? g_trace_n - SCHED_TRACE_N : 0;
    for (unsigned i = first; i < g_trace_n; i++) {
        struct sched_trace_ent *e = &g_trace[i % SCHED_TRACE_N];
        klog_printf("sched: %s idx=%d cur=%d state=%d (x%d)\n", e->what, e->idx,
                    e->cur, e->state, e->repeat + 1);
    }
}

// **AT MOST ONE SLOT IS SCHED_RUNNING, AND IT IS `current_index`.** The
// state is only reachable through switch_to(), only the rotation puts a
// process back to READY, and it only does that for the process that is
// current -- so a slot left RUNNING while somebody else is current is
// unschedulable for the rest of the boot. Checked at every switch, ONCE
// per boot: the report is the trace ring above, which is worth nothing
// if a flood has already pushed the cause out of it.
static int g_running_latched;

static void check_one_running(const char *where) {
    if (g_running_latched) return;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_RUNNING || i == current_index) continue;
        g_running_latched = 1;
        klog_printf("sched: INVARIANT at %s -- slot %d (pid %d, \"%s\") is RUNNING "
                    "while cur=%d (tgid=%d)\n", where, i, i + 1, procs[i].name,
                    current_index, procs[i].tgid);
        scheduler_trace_dump();
        return;
    }
}

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

// A PROCESS THAT HAS NEVER RUN CANNOT HAVE SAVED A CONTEXT, so it gets
// one built by hand: rsp at its trapframe, rip at the interrupt
// epilogue's pops. The first restore therefore lands exactly where a
// resume always used to -- Linux plants `ret_from_fork` on a fresh
// kernel stack for the same reason. The callee-saved registers are
// zeroed and immediately overwritten by those pops.
static void proc_start_context(int slot, const uint64_t *tf) {
    struct kernel_context *k = &procs[slot].kctx;
    k_memset(k, 0, sizeof *k);
    k->rsp = (uint64_t)tf;
    k->rip = (uint64_t)&isr_resume_frame;
    // Its own, not the spawner's: a slot is reused, and both of these
    // describe a context that does not exist yet.
    procs[slot].isr_depth = 0;
    procs[slot].preempt_depth = 0;
    procs[slot].parked_in_kernel = 0;
}

// WHO IS BEING SWITCHED AWAY FROM, PASSED IN RATHER THAN READ OFF
// current_index. Every caller but the tick clears current_index before
// it switches -- a blocking process is no longer current, and an
// exiting one is a zombie -- so reading it here would park the outgoing
// context in the KERNEL's slot and leave the real one unreachable.
// That mattered the moment a switch started saving anything; it did
// not before, when it only nominated a frame the caller had already
// written into procs[].
//
// -1 is the kernel context, which is a rotation participant like any
// other. An exiting process passes its own slot: the save is dead
// state, but the switch still has to HAPPEN.
static struct kernel_context *kctx_of(int from) {
    return from >= 0 ? &procs[from].kctx : &kernel_kctx;
}
static int *isr_depth_of(int from) {
    return from >= 0 ? &procs[from].isr_depth : &kernel_saved_isr_depth;
}

// **THE CPU MOVES HERE, not in an epilogue.** process_context_save()
// returns 0 on the way out and non-zero when this context is resumed,
// which is the entire switch: everything after the save runs on the
// outgoing stack and is abandoned by the restore, and everything the
// resumed context needs was installed for it by whoever resumed it.
//
// So the order matters and is not arbitrary: park OURSELVES first, then
// install the incoming context, then go. Installing first would run the
// rest of this function under the incoming process's CR3.
// Depth, not a flag: sections nest, and an inner one must not re-enable
// preemption an outer one is relying on. See api/scheduler.h.
//
// **AND IT TRAVELS WITH THE CONTEXT, exactly as isr_depth does.** A
// switch moves the CPU on the spot now, so a process that parks inside
// a guarded section parks WITH IT RAISED -- and as one global that
// left the whole machine unpreemptible for as long as somebody else
// was running. It presented as three tty tests failing and the suite
// taking 24s instead of 0.3s. docs/blocking-design.md names this as
// the fourth instance of "a global describing a per-context property",
// beside g_next_kernel_rsp, g_isr_depth and the old resume slot; this
// is it being cured the same way they were.
static int g_preempt_depth;

// The globals that describe the CURRENT context rather than the
// machine. Saved into the outgoing slot and reloaded from the incoming
// one on every switch, which is the whole of what makes them per
// context. kernel_* hold the kernel context's, since it has no slot.
static void context_save_globals(int from) {
    int *depth = isr_depth_of(from);
    int *preempt = from >= 0 ? &procs[from].preempt_depth : &kernel_preempt_depth;
    *depth = isr_depth_get();
    *preempt = g_preempt_depth;
}

static void context_load_globals(int to) {
    isr_depth_set(to >= 0 ? procs[to].isr_depth : kernel_saved_isr_depth);
    g_preempt_depth = to >= 0 ? procs[to].preempt_depth : kernel_preempt_depth;
}

static void switch_to(int from, int idx) {

    check_one_running("switch_to");
    trace_sched("switch_to", idx);
    kstack_verify(idx);   // before trusting anything else about this slot
    // A slot selected before anything gave it somewhere to resume. Was
    // a zero RSP handed to isr_common; now it is a jump to address 0,
    // which is no more diagnosable, so it is still checked here.
    if (!procs[idx].kctx.rip) {
        klog_printf("SLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                    idx, idx + 1, procs[idx].state, procs[idx].name);
        vga_printf("\nSLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                   idx, idx + 1, procs[idx].state, procs[idx].name);
        __asm__ volatile ("ud2");
    }

    if (process_context_save(kctx_of(from)) != 0) return;  // resumed: back

    context_save_globals(from);
    fpu_restore(procs[idx].fpu);
    // The thread pointer, and it has to be here: iretq reloads CS and SS
    // and leaves the hidden segment bases alone, so without this every
    // thread would read the last-scheduled thread's `__thread` storage.
    arch_set_fs_base(procs[idx].fs_base);
    vmm_switch_address_space(procs[idx].pml4_phys);
    gdt_set_kernel_stack(kernel_stack_top(idx));
    context_load_globals(idx);
    procs[idx].state = SCHED_RUNNING;
    current_index = idx;
    rotation_pos = idx;
    process_context_restore_noirq(&procs[idx].kctx, 1);
}

// The ROT_KERNEL counterpart to switch_to(): hand the CPU back to the
// kernel context. No CR3 switch, no RSP0 repoint and no FP restore --
// see the ROT_KERNEL comment above for why the kernel needs none of the
// three. Kept as its own function purely so both callers (the tick and
// the exit path) state the same thing once.
static void switch_to_kernel(int from) {
    check_one_running("to_kernel");
    trace_sched("to_kernel", -1);
    int was = from;
    current_index = -1;
    rotation_pos = ROT_KERNEL;
    // The LEGACY loader's thread pointer -- the same
    // two-owners-one-representation shape the heap and the cwd have
    // (scheduler.h): a process run by elf_run.c has no slot to keep one
    // in, and without this the last scheduled thread's %fs would still
    // be loaded when it resumed.
    arch_set_fs_base(kernel_fs_base);
    // ALREADY THE KERNEL -- the overwhelmingly common case, since a tick
    // with nothing spawned lands here every time. It used to nominate
    // the frame the tick had just captured, which was a no-op by a
    // longer route; now there is nothing to switch to.
    if (was < 0) return;
    // find_next_runnable()'s FALLBACK returns ROT_KERNEL without asking
    // kernel_slot_runnable(), so it can reach here before the kernel
    // context has ever been left -- and a restore then jumps to 0.
    // Measured not to fire under the interrupt gate; kept because the
    // fallback is genuinely unguarded.
    if (!kernel_kctx.rip) {
        klog_printf("KERNEL SLOT HAS NO SAVED CONTEXT (armed=%d)\n",
                    process_context_is_armed());
        vga_printf("\nKERNEL SLOT HAS NO SAVED CONTEXT (armed=%d)\n",
                   process_context_is_armed());
        __asm__ volatile ("ud2");
    }
    if (process_context_save(&procs[was].kctx) != 0) return; // resumed: back
    context_save_globals(was);
    context_load_globals(-1);
    process_context_restore_noirq(&kernel_kctx, 1);
}

// Loads a real ELF64 binary from the persistent filesystem as a fresh
// ring-3 process and marks it READY -- the scheduler's own counterpart
// to elf_run_from_fs() (elf_run.c), which does the same load but then
// blocks synchronously via process_run_ring3() instead of handing the
// process to this scheduler. Used to spawn both `schedtest` counter
// processes from /bin now that they're disk-hosted binaries rather
// than GRUB modules (this used to be spawn_from_module(int
// module_index), sourcing bytes via multiboot_get_module() -- replaced
// outright rather than kept alongside once nothing needed it anymore,
// see docs/decisions.md). `argvec` is the child's argument VECTOR in
// elf_build_argv_on_stack()'s blob form (NULL for argv = {path}; the
// string-taking wrappers below convert) -- laid out via that function into
// this process's own stack page, the same layout elf_run_from_fs() uses
// for a legacy-blocking process, so a scheduler-managed one gets a real
// argv[0]/argc too instead of the rdi=rsi=0/bare-top-of-page RSP this
// function used to synthesize unconditionally. Returns the slot index
// (>= 0) or -1 on any failure (no free slot, missing/unreadable file,
// `args` too long to fit the one stack page, or the same allocation
// failures every other ELF-loading path already handles the same way).
// THE IMAGE HALF OF A SPAWN, shared with exec (docs/fork-design.md):
// read the ELF, create the address space, load it and its interpreter,
// map the initial stack and lay argv/env/auxv out on it. On success
// `*out_as` holds an address space nothing else refers to yet; on
// failure nothing is held. `*out_image_end` is where the heap starts.
// --- `#!`, which is the LOADER's job and not the shell's ------------
//
// Linux does this in binfmt_script, and the reason to copy the position
// rather than the mechanism is that a script then runs the same way from
// `spawn`, from execve() and from either shell -- instead of from
// whichever one remembered to look for the line.
//
// Two limits, both Linux's: the line is read out of the first
// SHEBANG_MAX_LINE bytes (BINPRM_BUF_SIZE is 128 there), and a script
// whose interpreter is itself a script nests at most SHEBANG_MAX_DEPTH
// times, which is what stops `#!/x` in a file named `/x` looping the
// kernel. ONE optional argument after the interpreter, not a split word
// list -- every Unix does exactly this, and a shell writing
// `#!/bin/dash -e -x` gets `-e -x` as a single argument on all of them.
#define SHEBANG_MAX_LINE  128
#define SHEBANG_MAX_DEPTH 4

// Builds the interpreter's argument vector: [interp, arg?, script,
// caller's args after argv[0]]. argv[0] is the interpreter AS WRITTEN
// and the script's own path arrives as the next entry, which is how the
// interpreter learns what to open. Returns a kmalloc'd vector and its
// length, or 0 if it cannot fit -- refused, never truncated, the same
// rule elf_argv_from_string() follows.
static char *shebang_argv(const char *interp, const char *arg, const char *script,
                          const char *argvec, size_t argvec_len, size_t *out_len) {
    // The caller's args MINUS argv[0]: the interpreter replaces it.
    const char *rest = 0;
    size_t rest_len = 0;
    if (argvec && argvec_len) {
        size_t first = 0;
        while (first < argvec_len && argvec[first]) first++;
        if (first < argvec_len) {           // there is something after argv[0]
            rest = argvec + first + 1;
            rest_len = argvec_len - first - 1;
        }
    }

    size_t need = k_strlen(interp) + 1 + k_strlen(script) + 1 + rest_len;
    if (arg) need += k_strlen(arg) + 1;
    if (need > SPAWN_ARGS_MAX) return 0;

    char *out = kmalloc(need);
    if (!out) return 0;
    size_t n = 0;
    n += (size_t)k_strlcpy(out + n, interp, need - n) + 1;
    if (arg) n += (size_t)k_strlcpy(out + n, arg, need - n) + 1;
    n += (size_t)k_strlcpy(out + n, script, need - n) + 1;
    if (rest_len) { k_memcpy(out + n, rest, rest_len); n += rest_len; }
    *out_len = n;
    return out;
}

// Reads `path`'s first line and, if it is a `#!`, writes the interpreter
// into `interp` (cap `interp_cap`) and the optional single argument into
// `arg`. Returns 1 if this file is a script, 0 if it is not one (or the
// line is malformed, which is treated as "not a script" so the ordinary
// "not an ELF" error reaches the caller rather than a second one).
static int shebang_read(const char *path, char *interp, size_t interp_cap,
                        char *arg, size_t arg_cap) {
    char line[SHEBANG_MAX_LINE];
    uint32_t got = fs_read_range(path, 0, line, sizeof line);
    if (got < 3 || line[0] != '#' || line[1] != '!') return 0;

    size_t i = 2, n = got < sizeof line ? got : sizeof line;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t start = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' &&
           line[i] != '\n' && line[i] != '\r') i++;
    if (i == start || i - start >= interp_cap) return 0;
    k_memcpy(interp, line + start, i - start);
    interp[i - start] = 0;

    // The optional argument: everything left on the line, trimmed at
    // both ends, as ONE string.
    arg[0] = 0;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t astart = i;
    while (i < n && line[i] != '\n' && line[i] != '\r') i++;
    while (i > astart && (line[i - 1] == ' ' || line[i - 1] == '\t')) i--;
    if (i > astart && i - astart < arg_cap) {
        k_memcpy(arg, line + astart, i - astart);
        arg[i - astart] = 0;
    }
    return 1;
}

static int build_elf_image(const char *path, const char *argvec, size_t argvec_len,
                       const char *env, uint64_t *out_as, uint64_t *out_entry,
                       uint64_t *out_rsp, uint64_t *out_image_end) {
    // THE IMAGE IS READ INTO MEMORY THIS FUNCTION OWNS (fs_read_into),
    // never the backend's staging buffer: this runs in the kernel
    // context as well as under a syscall, and a ring-3 file read could
    // free that buffer under elf_load(). kmalloc memory is identity-
    // mapped, so elf_load() takes its address directly (elf_run.c).
    uint64_t fsz = fs_size(path);
    if (fsz == 0 || fsz > 0xFFFFFFFFu - 1) return 0;
    char *data = kmalloc((size_t)fsz + 1);
    if (!data) return 0;
    uint32_t size = fs_read_into(path, data, (uint32_t)fsz + 1);
    if (size == 0) { kfree(data); return 0; }
    uint64_t elf_phys = (uint64_t)(uintptr_t)data;

    uint64_t as = vmm_create_address_space();
    if (!as) { kfree(data); return 0; }

    // Same one-line hook elf_run_from_fs() has -- a no-op unless the
    // shell's `strace` armed tracing, which keeps the mechanism
    // process-creation-path-agnostic rather than tied to the blocking
    // loader (see kernel/proc/strace.c).
    strace_claim(as);

    uint64_t entry = 0, image_end = 0;
    struct elf_dyn_info dyn;
    // On failure the address space is destroyed rather than leaked --
    // it owns whatever elf_load() mapped before giving up, and every
    // failure path below this point owes the same cleanup. This used to
    // be a bare `return -1`, leaking the PML4, every page table under
    // it and every segment frame.
    if (!elf_load(elf_phys, size, as, &entry, &image_end, &dyn)) {
        vmm_destroy_address_space(as);
        kfree(data);
        return 0;
    }
    kfree(data);   // everything the process needs from it is in the address space now

    // A DYNAMIC executable: load the interpreter it names as a second
    // image and enter THAT (Linux's split -- the kernel's part in
    // dynamic linking ends here; /lib/ld-toy.so finishes the job in
    // ring 3 and jumps to the auxv's AT_ENTRY). The interpreter is a
    // fixed-base ET_EXEC at ELF_LDSO_BASE, so the same elf_load() and
    // the same bounds serve both images; the guard is the executable
    // growing up into it, which no real program here approaches.
    //
    uint64_t auxv[4][2];
    int auxc = 0;
    if (dyn.interp[0]) {
        if (image_end > ELF_LDSO_BASE) {
            klog_printf(KLOG_ERR "spawn: %s reaches %#lx, into the interpreter -- refused\n",
                        path, image_end);
            vmm_destroy_address_space(as);
            return 0;
        }
        uint64_t ifsz = fs_size(dyn.interp);
        char *idata = ifsz && ifsz < 0xFFFFFFFFu - 1 ? kmalloc((size_t)ifsz + 1) : 0;
        uint32_t isize = idata ? fs_read_into(dyn.interp, idata, (uint32_t)ifsz + 1) : 0;
        if (isize == 0) {
            klog_printf("spawn: interpreter %s missing\n", dyn.interp);
            if (idata) kfree(idata);
            vmm_destroy_address_space(as);
            return 0;
        }
        uint64_t ientry = 0, iend = 0;
        int iok = elf_load((uint64_t)(uintptr_t)idata, isize, as, &ientry, &iend, 0);
        kfree(idata);
        if (!iok) {
            klog_printf("spawn: interpreter %s did not load\n", dyn.interp);
            vmm_destroy_address_space(as);
            return 0;
        }
        if (iend > image_end) image_end = iend; // the heap starts after BOTH

        auxv[auxc][0] = AT_PHDR;  auxv[auxc][1] = ELF_IMAGE_BASE + dyn.phoff; auxc++;
        auxv[auxc][0] = AT_PHENT; auxv[auxc][1] = 56;         auxc++;
        auxv[auxc][0] = AT_PHNUM; auxv[auxc][1] = dyn.phnum;  auxc++;
        auxv[auxc][0] = AT_ENTRY; auxv[auxc][1] = entry;      auxc++;
        entry = ientry;
    }

    // The TOP page is where argv is laid out and where RSP starts; the
    // rest are this process's starting WORKING SET, so the common client
    // never takes a growth fault at all. Everything below them is
    // reserved address space that uheap_fault() maps on demand -- see
    // kernel/uaddr.h.
    uint64_t stack_phys = 0;
    for (int pg = 0; pg < UADDR_STACK_INIT_PAGES; pg++) {
        uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!frame) { vmm_destroy_address_space(as); return 0; }
        uint64_t va = UADDR_STACK_VADDR - (uint64_t)pg * 4096;
        if (!vmm_map_user_page(as, va, frame)) {
            // The frame is not mapped, so destroying the address space
            // will not reclaim it -- free it here, then let the address
            // space take everything that IS mapped.
            pmm_free_frame(frame);
            vmm_destroy_address_space(as);
            return 0;
        }
        if (pg == 0) stack_phys = frame;
    }

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, UADDR_STACK_VADDR, path, argvec, argvec_len, env,
                                  auxc ? auxv : 0, auxc,
                                  &argc, &argv, &user_rsp)) {
        vmm_destroy_address_space(as);
        return 0;
    }
    (void)argc; (void)argv; // argc/argv reach the process on its STACK
    *out_as = as;
    *out_entry = entry;
    *out_rsp = user_rsp;
    *out_image_end = image_end;
    return 1;
}

// The per-address-space half of a slot's memory state, from a freshly
// built image: the heap starts where the image ends (elf.c's
// out_image_end, which is what lets a ring-3 binary be any size), the
// stack has its initial working set, and no mmap region exists yet. A
// recycled slot still holds its previous owner's regions, and the
// frames behind them are long freed, so the wipe is load-bearing.
static void mm_reset(int slot, uint64_t image_end) {
    uint64_t heap_base = image_end > UADDR_HEAP_MIN_BASE
                              ? image_end : UADDR_HEAP_MIN_BASE;
    procs[slot].mm.heap_base    = heap_base;
    procs[slot].mm.brk          = heap_base;
    procs[slot].mm.stack_bottom = UADDR_STACK_INIT_BOTTOM;
    // Frees any list a previous occupant of this slot left behind:
    // the teardown path should have, and a slot recycled without one
    // must not leak. Idempotent, so both calling is correct.
    mmap_regions_reset(&procs[slot].mm);
}

// The loader every caller actually reaches: resolves `#!` first, then
// loads a real ELF. A script's interpreter may itself be a script, so
// this loops rather than recursing once -- bounded by SHEBANG_MAX_DEPTH.
static int build_image(const char *path, const char *argvec, size_t argvec_len,
                       const char *env, uint64_t *out_as, uint64_t *out_entry,
                       uint64_t *out_rsp, uint64_t *out_image_end) {
    char interp[SHEBANG_MAX_LINE], arg[SHEBANG_MAX_LINE];
    char *owned = 0;

    for (int depth = 0; shebang_read(path, interp, sizeof interp,
                                     arg, sizeof arg); depth++) {
        if (depth >= SHEBANG_MAX_DEPTH) { kfree(owned); return 0; }
        size_t len = 0;
        char *next = shebang_argv(interp, arg[0] ? arg : 0, path,
                                  argvec, argvec_len, &len);
        if (!next) { kfree(owned); return 0; }
        kfree(owned);               // the vector this one was built from
        owned = next;
        argvec = next;
        argvec_len = len;
        path = interp;
        // `path` now aliases `interp`, which the next pass overwrites --
        // safe only because shebang_argv() above has already copied it
        // into `owned` before that happens.
    }

    int rc = build_elf_image(path, argvec, argvec_len, env,
                             out_as, out_entry, out_rsp, out_image_end);
    kfree(owned);
    return rc;
}

static int spawn_from_fs(const char *path, const char *argvec, size_t argvec_len,
                          int stdout_desc, int stdin_desc, const char *env,
                          int want_pgid, uint64_t parent_pml4) {
    int slot = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint64_t as = 0, entry = 0, user_rsp = 0, image_end = 0;
    if (!build_image(path, argvec, argvec_len, env, &as, &entry, &user_rsp, &image_end))
        return -1;

    // Synthesize this process's very first trapframe, at the top of its
    // own dedicated kernel stack -- laid out exactly like a real one
    // isr_common would have saved, so the ordinary epilogue can launch
    // it the first time exactly the same way it resumes it later.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0; // r15..rax start at 0
    // rdi/rsi stay 0: argc/argv reach the process on its STACK, in the
    // SysV layout elf_build_argv_on_stack() built and crt0.asm reads
    // (user_rsp points at argc).
    tf[TF_VECTOR]  = 0; // unused -- epilogue discards vector+error_code
    tf[TF_ERRCODE] = 0; //          via `add rsp, 16` without reading them
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    tf[TF_RSP]     = user_rsp;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    // A pristine FP state, not whatever the previous tenant of this
    // slot left behind -- slots get reused (scheduler_poll() reaps back
    // to SCHED_UNUSED), and inheriting the last process's registers
    // would be both wrong and an information leak between processes.
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_chan = 0;
    procs[slot].wait_reason = 0;
    // A NEW PROCESS LEADS ITS OWN THREAD GROUP -- everything below this
    // line is per-group state, and it has exactly one member.
    procs[slot].tgid     = slot + 1;
    procs[slot].fs_base  = 0;
    procs[slot].detached = 0;

    // THE CHILD'S FILE DESCRIPTORS, built here because this is where
    // its address space first exists. It inherits the caller's whole
    // table (sharing every description, refcounted), which is what
    // lets a shell redirect a child by redirecting ITSELF around the
    // spawn -- the dance fork() normally exists to make possible:
    //
    //     saved = dup(1); dup2(f, 1); spawn(...); dup2(saved, 1);
    //
    // A kernel-context spawn has no table to inherit and the child
    // gets the standard three (console, console, kernel log).
    // THE PARENT IS WHOEVER IS EXECUTING, and that is CR3 -- not
    // procs[current_index]. The legacy blocking loader (`run` at the
    // physical shell) has its own address space and NO scheduler slot,
    // so a slot lookup answers "no parent" for it and its children
    // silently inherited nothing. The fd table is keyed by CR3
    // precisely so that path is not a special case; asking the
    // scheduler instead reintroduced the special case at the one site
    // that mattered.
    //
    // The parent is NAMED, never read off CR3. vmm_current_pml4() was
    // the parent here, and it was wrong in exactly one situation that
    // took an afternoon to find: a KERNEL-context caller (the shell's
    // bare-name spawn) runs with whatever address space the scheduler
    // last loaded, so the child inherited some arbitrary interrupted
    // process's terminal -- `ps` typed at the console printed into a
    // Terminal window. SYS_SPAWN passes its caller's pml4; a kernel
    // caller passes 0 and its child gets the standard three.
    fd_inherit(as, parent_pml4);
    if (stdout_desc >= 0) {
        // SYS_SPAWN's explicit stream overrides, which predate
        // inheritance and stay as the one-call shortcut. Applied
        // AFTER inheriting, so they win.
        fd_set_desc(as, FD_STDOUT, stdout_desc);
    }
    if (stdin_desc >= 0) fd_set_desc(as, FD_STDIN, stdin_desc);
    // The CALLER is the parent. 0 when the kernel context spawned this
    // -- scheduler_current_pid() returns 0 there, which is exactly the
    // "no parent" value, so this needs no special case.
    procs[slot].ppid = scheduler_current_tgid();
    // SET, NOT LEFT: a reused slot otherwise hands the next tenant its
    // last one's priority. Inherited, as posix_spawn does.
    procs[slot].prio = current_index >= 0 ? procs[current_index].prio : 0;
    // NOTHING IS PENDING AND NOTHING IS IGNORED for a fresh process --
    // reset rather than inherited, and both matter. A slot is reused, so
    // a leftover pending bit would kill the NEXT tenant on its first
    // instruction; and dispositions do not survive an exec on Unix
    // either (an ignored signal is the documented exception there, and
    // this kernel has no fork/exec pair to make that distinction from).
    signal_state_reset(slot);
    // Nothing has announced anything yet. See the field's comment: this
    // is the same slot-reuse hazard signal_state_reset() covers.
    procs[slot].ready = 0;
    // THE GROUP: what the caller asked for, else the spawner's, else a
    // group of this process's own. The third case is the kernel context
    // -- init, the demo, a KTEST -- which has no group to lend, and
    // leading its own is what keeps `pgid` non-zero for every live slot.
    if (want_pgid > 0) {
        procs[slot].pgid = want_pgid;              // join that group
    } else if (want_pgid == PGID_NEW) {
        procs[slot].pgid = slot + 1;               // lead one of its own
    } else {
        int parent_pgid = scheduler_pgid(procs[slot].ppid);
        procs[slot].pgid = parent_pgid > 0 ? parent_pgid : slot + 1;
    }
    // THE SESSION IS ALWAYS INHERITED -- there is no spawn-time way to
    // ask for a new one, and deliberately: POSIX creates a session with
    // setsid() in the child, and a shell's children MUST stay in the
    // shell's session or none of them could ever take the terminal.
    // A kernel-context spawn has no session to lend and leads its own.
    {
        int parent_sid = scheduler_sid(procs[slot].ppid);
        procs[slot].sid = parent_sid > 0 ? parent_sid : slot + 1;
    }
    // Reset, not inherited: slots are reused, and a reaped process's
    // name and CPU time showing up on its successor would be a
    // reporting bug that looks like a scheduling one.
    proc_name_from_path(procs[slot].name, sizeof procs[slot].name, path);
    k_strlcpy(procs[slot].exec_path, path ? path : "", sizeof procs[slot].exec_path);
    procs[slot].cpu_ns = 0;
    // Armed here, at creation, rather than by a separate "set up this
    // process's heap" call the way the legacy loader does it: an init
    // step reachable by only one entry point is a bug waiting for a
    // second entry point, and this one already had that bug -- nothing
    // in the spawn path ever armed a heap, so SYS_SBRK refused every
    // scheduled process.
    //
    // heap_base comes from the IMAGE rather than from a constant, which
    // is what lets a ring-3 binary be any size (elf.c's out_image_end).
    // The max() is belt and braces: elf_load() cannot report an end
    // below ELF_IMAGE_BASE, but a heap starting under the floor would be
    // a silent aliasing bug rather than a loud one.
    mm_reset(slot, image_end);
    // INHERITED, unlike the name and the CPU time above: the cwd is the
    // one piece of a parent's state a child is supposed to start with,
    // which is what makes `mkdir docs` from a shell standing in /tmp
    // create /tmp/docs rather than /docs. syscall_current_cwd() answers
    // for the kernel context too (the legacy loader's single slot), so
    // this needs no special case for a process the shell's `spawn`
    // started.
    k_strlcpy(procs[slot].cwd.path, scheduler_cwd(), sizeof procs[slot].cwd.path);

    // WHAT A REMOTE SESSION STARTS IS RECORDED, auditd's execve shape --
    // here rather than in sys_spawn() because every spawn funnels
    // through this function, including the ones a script makes. A local
    // session logs nothing: remote_log_session_of() answers 0 and this
    // costs one walk of eight slots.
    uint32_t rip = remote_log_session_of(procs[slot].sid);
    if (rip) {
        char line[QUERY_REMOTELOG_TEXT_MAX];
        // The path plus the first argument, which is what makes `ls
        // /boot` distinguishable from `ls`. The whole vector would not
        // fit and the interesting part is the front of it.
        const char *arg = 0;
        if (argvec && argvec_len) {   // the same walk shebang_argv() makes
            size_t first = 0;
            while (first < argvec_len && argvec[first]) first++;
            if (first + 1 < argvec_len) arg = argvec + first + 1;
        }
        if (arg) k_snprintf(line, sizeof line, "%s %s", path, arg);
        else     k_strlcpy(line, path, sizeof line);
        remote_log_record(QUERY_REMOTE_SPAWN, rip, slot + 1,
                          procs[slot].name, line);
    }

    procs[slot].state      = SCHED_READY;
    alive_count++;
    return slot;
}

// SCHED_WAIT_* (kernel-internal) -> PROC_WAIT_* (what ring 3 sees).
//
// A TRANSLATION RATHER THAN THE SAME NUMBERS TWICE, for the reason
// abi/proc_info.h gives: the two enumerations are allowed to diverge,
// and the states already do (they do not agree on BLOCKED). What keeps
// them in step is procinfo's "every wait reason is reported" KTEST,
// which walks every SCHED_WAIT_* and refuses PROC_WAIT_NONE -- so a
// sixth reason added to scheduler.h reddens a check instead of silently
// reporting as "not waiting for anything".
static uint32_t reported_wait_reason(int reason) {
    switch (reason) { // dispatch-ok: bounded by scheduler.h's SCHED_WAIT_* labels
    case SCHED_WAIT_EVENT: return PROC_WAIT_EVENT;
    case SCHED_WAIT_PIPE:  return PROC_WAIT_PIPE;
    case SCHED_WAIT_CHILD: return PROC_WAIT_CHILD;
    case SCHED_WAIT_TIMER: return PROC_WAIT_TIMER;
    case SCHED_WAIT_KEY:   return PROC_WAIT_KEY;
    case SCHED_WAIT_TTY:   return PROC_WAIT_TTY;
    case SCHED_WAIT_THREAD: return PROC_WAIT_THREAD;
    case SCHED_WAIT_NET:   return PROC_WAIT_NET;
    case SCHED_WAIT_FUTEX: return PROC_WAIT_FUTEX;
    case SCHED_WAIT_SIGNAL: return PROC_WAIT_SIGNAL;
    case SCHED_WAIT_LOCK:  return PROC_WAIT_LOCK;
    default:               return PROC_WAIT_NONE;
    }
}

int scheduler_mark_current_ready(void) {
    int pid = scheduler_current_pid();
    if (pid <= 0) return 0;
    procs[pid - 1].ready = 1;
    return 1;
}

int scheduler_proc_info(int index, struct proc_info *out) {
    if (!out || index < 0 || index >= MAX_PROCS) return 0;

    struct sched_process *p = &procs[index];

    out->pid = 0;
    out->state = PROC_STATE_UNUSED;
    out->cpu_ns = 0;
    out->mem_bytes = 0;
    out->exit_code = 0;
    out->ppid = p->ppid;
    out->pgid = p->pgid;
    out->tgid = 0;
    out->wait_reason = PROC_WAIT_NONE;
    out->ready = 0;
    out->name[0] = '\0';

    if (p->state == SCHED_UNUSED) return 1; // a real answer: slot empty

    // pid is slot + 1 throughout this file -- 0 is "no process".
    out->pid = index + 1;
    out->tgid = p->tgid;
    out->ready = p->ready ? 1u : 0u;
    out->cpu_ns = p->cpu_ns;
    out->exit_code = p->exit_code;
    k_strlcpy(out->name, p->name, sizeof out->name);

    // A zombie's address space is already gone, so asking for its memory
    // would report whatever now lives at that PML4 address. Report 0.
    if (p->state != SCHED_ZOMBIE) out->mem_bytes = vmm_user_bytes(p->pml4_phys);

    // SCHED_RUNNING is a state in its own right here, NOT just "READY
    // and current" -- a process asking this question about itself is in
    // it, which is why the first version reported the caller as "-":
    // it mapped READY and BLOCKED and let RUNNING fall to the default.
    // The `index == current_index` test is still wanted, because a
    // process can be READY and current between a tick and a switch.
    switch (p->state) {
        case SCHED_RUNNING: out->state = PROC_STATE_RUNNING; break;
        case SCHED_READY:   out->state = (index == current_index)
                                          ? PROC_STATE_RUNNING : PROC_STATE_READY; break;
        case SCHED_BLOCKED: out->state = PROC_STATE_BLOCKED;
                            out->wait_reason = reported_wait_reason(p->wait_reason);
                            break;
        case SCHED_ZOMBIE:  out->state = PROC_STATE_ZOMBIE;  break;
        default:            out->state = PROC_STATE_UNUSED;  break;
    }

    // STOPPED OUTRANKS WHATEVER IT IS STOPPED FROM. A suspended process
    // is still READY or BLOCKED underneath -- that is the point of the
    // flag -- but reporting "ready" for something the scheduler will
    // never pick is a lie of exactly the kind `ps` exists to prevent.
    // Only over the two live states: a zombie's flag is stale, and
    // scheduler_kill() clears it for the same reason.
    if (p->stopped && (out->state == PROC_STATE_READY ||
                       out->state == PROC_STATE_RUNNING ||
                       out->state == PROC_STATE_BLOCKED))
        out->state = PROC_STATE_STOPPED;
    return 1;
}

int scheduler_max_procs(void) { return MAX_PROCS; }

// CPU time is billed by MEASURING IT, not by counting ticks.
//
// The history is worth the paragraph, because the obvious
// implementation is the wrong one and this kernel shipped it twice.
// Billing was `cpu_ticks++` from the timer, and SYS_YIELD rescheduled
// through the same function -- so a yield, which elapses microseconds,
// charged a whole 10ms tick. A polling app yields about once per tick,
// so it billed itself 100 ticks a second against a 100Hz clock: a
// stable, entirely fake 100%, for every polling app simultaneously,
// which one CPU obviously cannot do. Charging only from the timer
// fixed the impossibility and replaced it with the opposite error --
// sampled accounting cannot see a process that runs for less than a
// tick, so a client drawing one frame every 10ms read 0%.
//
// Both errors have the same root: a TICK COUNT is not a DURATION. So
// the scheduler asks a clock (kernel/clocksource.h) how much time
// actually passed, and charges that. The PIT-backed source makes this
// no better than before; the TSC-backed one, registered once the CPU
// is calibrated, makes it exact to the nanosecond. Nothing here knows
// which is live, which is the point of the interface.
//
// The invariant: every path that stops running the current process
// calls bill_current() BEFORE changing current_index. Miss one and
// that slice is credited to whoever runs next.
// The same slices, totalled machine-wide, so a caller can divide rather
// than sum per-process percentages -- which miss a process that started
// and exited between two samples. Read through QUERY_CPULOAD.
static uint64_t g_proc_ns = 0;   // charged to some slot
static uint64_t g_kernel_ns = 0; // charged to nobody: the idle halt, or the text shell

static void bill_current(void) {
    uint64_t now = clocksource_now_ns();
    if (now > g_run_start_ns) {
        uint64_t slice = now - g_run_start_ns;
        if (current_index >= 0) {
            procs[current_index].cpu_ns += slice;
            g_proc_ns += slice;
        } else {
            g_kernel_ns += slice;
        }
    }
    // Reset unconditionally, including when the KERNEL context was
    // running: leaving the old start in place would hand the next
    // process everything the kernel just spent.
    g_run_start_ns = now;
}

void scheduler_cpu_time(uint64_t *proc_ns, uint64_t *kernel_ns) {
    // Billed first, so the slice in progress is not missing from the
    // answer -- without it a machine running ONE busy process reports
    // whatever it had at the last rotation, up to 10 ms stale.
    bill_current();
    if (proc_ns) *proc_ns = g_proc_ns;
    if (kernel_ns) *kernel_ns = g_kernel_ns;
}

// The rotation, shared by the 100Hz timer and by SYS_YIELD. They are
// the same operation now that neither one is where billing happens --
// see bill_current() above.
static void scheduler_rotate(uint64_t *regs);

void scheduler_tick(uint64_t *regs) {
    // BEFORE the rotation, and outside scheduler_rotate()'s armed
    // check: a sleeper's deadline has nothing to do with whether the
    // scheduler is currently rotating, and a woken process wants to be
    // eligible for the switch this very tick rather than the next one.
    scheduler_wake_timers(clocksource_now_ns());
    scheduler_rotate(regs);
}

// SYS_YIELD's entry into the same rotation. Distinct from the tick only
// so the call sites read honestly; the accounting difference that used
// to justify two paths is gone.
void scheduler_yield(uint64_t *regs) { scheduler_rotate(regs); }

void scheduler_trap_exit(uint64_t *regs) {
    if (g_need_resched) scheduler_rotate(regs);
}

void scheduler_preempt_disable(void) { g_preempt_depth++; }

int scheduler_preempt_depth(void) { return g_preempt_depth; }

void scheduler_preempt_enable(void) {
    if (g_preempt_depth > 0) g_preempt_depth--;
    // Clamped at 0 rather than allowed to go negative: an extra enable()
    // is a bug, but letting the count drift below zero would make the
    // NEXT legitimate disable() a no-op, turning a local mistake into a
    // silent loss of protection somewhere else entirely.
}

static void scheduler_rotate(uint64_t *regs) {
    if (!scheduler_armed) return;


    // UNCONDITIONALLY, and before anything can return early or switch:
    // this closes the slice that just ended, whoever owned it. Doing it
    // inside the `current_index >= 0` branch below was wrong in the one
    // case that matters -- when the KERNEL context was running there is
    // nobody to charge, but the start timestamp still has to move, and
    // leaving it stale handed the next process everything the kernel had
    // just spent. Measured: a process billed 9.51 SECONDS across a 300ms
    // window.
    bill_current();


    // A LEGACY BLOCKING PROCESS CANNOT BE PARKED, so while one is in
    // flight this file does not switch at all -- it resumes exactly
    // what was interrupted, which is what it did before the kernel
    // joined the rotation.
    //
    // process_run_ring3() runs a ring-3 process with NO procs[] entry:
    // from here it simply *is* "the kernel context", and its trapframe
    // lands in kernel_saved_rsp. There is nowhere to record its CR3 or
    // its RSP0, so switching away and back resumes it under whatever
    // address space the other process left loaded.
    //
    // kernel_slot_runnable() already refused to SELECT the kernel
    // position for that reason -- but find_next_runnable()'s fallback
    // returns ROT_KERNEL anyway when nothing else is runnable, which
    // reintroduced the exact case the guard existed to prevent. The
    // real fix is here: don't start the rotation at all.
    //
    // Found the hard way. Running `ls` from the debug console (the
    // legacy path) while a ring-3 GUI client was alive scheduled the
    // client, then resumed `ls` under the CLIENT's page tables --
    // reported as a bare "RING-3 CRASH: Page fault" in the client,
    // which is about as far from the actual cause as a symptom gets.
    // The window is narrow but entirely reachable: any `run` from the
    // physical shell while a client has a window open.
    if (process_context_is_armed()) {
        if (current_index < 0) save_kernel_frame(regs);
        return;
    }

    // A CRITICAL SECTION IS OPEN -- resume exactly what was interrupted,
    // the same treatment (and for the same reason) as the armed legacy
    // process above: switching away would let another caller re-enter
    // code that is holding shared state. See scheduler_preempt_disable()
    // in api/scheduler.h for what holds this and why. The slice is
    // already billed above, so accounting is unaffected.
    if (g_preempt_depth > 0) {
        if (current_index < 0) save_kernel_frame(regs);
        return;
    }

    // Consumed only by a rotation that can happen: one refused above
    // leaves it for the next trap rather than losing the wake.
    if (g_need_resched)
        note_preempted(current_index >= 0 ? current_index : ROT_KERNEL, running_prio());
    g_need_resched = 0;

    if (current_index >= 0) {
        // This process was the one running for the tick that just
        // fired. Counted here rather than at switch_to() time because
        // this is the only place that knows a whole tick elapsed under
        // it -- see abi/proc_info.h on why the total, not a percentage.
        procs[current_index].kernel_rsp = (uint64_t)regs;
        kstack_verify(current_index);  // it just stopped running -- check its stack
        // Paired with switch_to()'s FXRSTOR. Saved on the way out
        // whether or not the process has touched FP: "has it?" is
        // exactly the question the lazy scheme answered with CR0.TS,
        // and exactly the question that turned out to be dangerous to
        // answer (see fpu.h).
        fpu_save(procs[current_index].fpu);
        procs[current_index].state = SCHED_READY;
        trace_sched("rotate_out", current_index);
    } else {
        save_kernel_frame(regs);
    }

    // Rotate from wherever the cycle last stopped, not from
    // current_index -- those differ precisely when the kernel is the
    // thing running (current_index -1, rotation_pos ROT_KERNEL), which
    // is exactly the case that has to advance past the kernel's own
    // position instead of restarting the scan at slot 0 every tick.
    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) {
        // Either nothing else is runnable, or the kernel's turn came up
        // in the rotation. In the overwhelmingly common case (nothing
        // has been spawned at all) kernel_saved_rsp was just set to
        // `regs` above, so this stays the genuine no-op it always was.
        switch_to_kernel(current_index);
        return;
    }

    switch_to(current_index, next);
}

// BLOCKING SYSCALLS, AND WHY THEY DESCHEDULE RATHER THAN WAIT
// -----------------------------------------------------------
// Parks the calling process on `reason` and hands the CPU to whatever
// is next in the rotation. `regs` is the syscall's own trapframe --
// the same pointer isr_dispatch was handed -- so the process resumes
// from the instruction after its `int 0x80` when woken, with
// scheduler_wake()'s value already in RAX.
//
// The obvious implementation of a blocking syscall -- `sti`, then spin
// or `hlt` inside the handler until the thing you're waiting for
// arrives -- was tried in this kernel and is genuinely unsafe here, not
// merely slow. A blocking keyboard read was built that way once: it
// worked for exactly one keystroke and then hung, because
// g_next_kernel_rsp (idt.c) is a single global "where to resume"
// pointer. It is correct for the scheduler's own use but was never
// meant to be reentrant, so a nested IRQ handler overwrites it while
// the outer int-0x80 handler is still on the stack, and that outer
// handler's epilogue then resumes into a stale frame.
//
// Descheduling sidesteps that entirely instead of trying to make the
// global reentrant. Nothing nests: the handler does not wait, it
// RETURNS, through the ordinary isr_common epilogue, into a different
// entity -- exactly the switch scheduler_tick() and scheduler_on_exit()
// already perform, using machinery that is already proven. Interrupts
// stay off for the whole handler, as they always were.
//
// Returns 1 if the caller was parked (its syscall must then return
// WITHOUT touching regs[TF_RAX] -- the wake writes it), or 0 if the
// caller isn't a scheduler-managed process and therefore has no slot to
// park in (the legacy process_run_ring3() path, or kernel code). A 0
// return is not an error the caller may ignore: it means "you must fall
// back to non-blocking behaviour", because there is nowhere to put this
// process to sleep.
// The one global wait channel -- a deadline. Its CONTENTS are never
// read; only its address matters, which is the whole point of a
// channel. `char` rather than `int` so it is guaranteed its own
// distinct address. (There were two until terminals became objects; see
// scheduler.h.)
const char sched_chan_timer;

// A process's own channel, woken by ITS children when they exit. Using
// the slot's address means the channel is stable for as long as the
// slot is, and unique across processes without a second table.
//
// Note this is the slot's address, not the pid: a reaped slot that is
// reused hands the new occupant the same channel, which is correct --
// the old occupant is gone and cannot be waiting on it.
const void *scheduler_wait_chan_pid(int pid) {
    int idx = pid - 1;
    if (idx < 0 || idx >= MAX_PROCS) return 0;
    return &procs[idx];
}

// See scheduler.h for the two rules a caller must keep. Claims the
// first free slot; -1 when the table is full. Deliberately does NOT
// touch alive_count -- this slot holds no process, and counting it
// would make the scheduler believe there is one more thing to run.
int scheduler_test_park(uint64_t *tf, const void *chan, int reason) {
    if (!tf) return -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_UNUSED) continue;
        procs[i].state = SCHED_BLOCKED;
        procs[i].wait_chan = chan;
        procs[i].wait_reason = reason;
        procs[i].kernel_rsp = (uint64_t)tf;
        proc_start_context(i, tf);
        // A FABRICATED SLOT MUST LOOK LIKE A FRESH PROCESS, which is
        // exactly what spawn_from_fs() gives a real one. Slots are
        // reused, so without this a test that set a disposition leaves
        // it for whichever test claims the slot next -- and it presented
        // exactly that way: three signal tests failed because an earlier
        // one had left SIGINT ignored on the slot they happened to get.
        // Establishing the precondition in the fabricator beats each
        // test remembering to (ktest.h).
        signal_state_reset(i);
        procs[i].pgid = i + 1;
        procs[i].sid = i + 1;
        procs[i].prio = 0;
        return i;
    }
    return -1;
}

int scheduler_test_pick(int start, int preempted) {
    if (preempted >= 0) note_preempted(preempted, procs[preempted].prio);
    return find_next_runnable(start);
}

int scheduler_test_take_resched(void) {
    int r = g_need_resched;
    g_need_resched = 0;
    return r;
}

// Releases a slot parked above, in EITHER state: a woken one is READY,
// and refusing to free that was the bug this comment exists to stop --
// it would leave the scheduler a runnable slot whose trapframe is a
// dead stack local.
void scheduler_test_release(int idx) {
    if (idx < 0 || idx >= MAX_PROCS) return;
    if (procs[idx].state != SCHED_BLOCKED && procs[idx].state != SCHED_READY) return;
    procs[idx].state = SCHED_UNUSED;
    procs[idx].wait_chan = 0;
    procs[idx].kernel_rsp = 0;
    procs[idx].isr_depth = 0;
    procs[idx].preempt_depth = 0;
    procs[idx].parked_in_kernel = 0;
    k_memset(&procs[idx].kctx, 0, sizeof procs[idx].kctx);
    // Cleared on the way out as well as on the way in. Belt and braces
    // is not the reason: an UNUSED slot with a pending bit is a slot the
    // next real spawn would have to remember to clear, and one of the
    // two places would eventually be the one that got forgotten.
    signal_state_reset(idx);
}

// Reported as PROC_STATE_*, never the internal enum: abi/proc_info.h
// keeps those two enumerations deliberately separate (they do not even
// agree on the value of BLOCKED), and a test asserting on the internal
// one would silently start lying if it gained a state.
int scheduler_test_state(int idx) {
    if (idx < 0 || idx >= MAX_PROCS) return -1;
    // Same precedence scheduler_proc_info() applies, and for the same
    // reason -- a test asking a stopped slot's state must not be told
    // "ready" about something that will never be picked.
    if (procs[idx].stopped &&
        (procs[idx].state == SCHED_READY || procs[idx].state == SCHED_RUNNING ||
         procs[idx].state == SCHED_BLOCKED))
        return PROC_STATE_STOPPED;
    switch (procs[idx].state) { // dispatch-ok: bounded by enum sched_state
    case SCHED_RUNNING: return PROC_STATE_RUNNING;
    case SCHED_READY:   return PROC_STATE_READY;
    case SCHED_BLOCKED: return PROC_STATE_BLOCKED;
    case SCHED_ZOMBIE:  return PROC_STATE_ZOMBIE;
    default:            return PROC_STATE_UNUSED;
    }
}

_Static_assert(TF_RAX < SCHED_TF_SLOTS,
               "a test trapframe must be big enough to hold the RAX slot a wake writes");
_Static_assert(TF_RDI == SCHED_TF_RDI && TF_CS == SCHED_TF_CS &&
               TF_RFLAGS == SCHED_TF_RFLAGS && TF_RSP == SCHED_TF_RSP &&
               TF_SS == SCHED_TF_SS,
               "api/scheduler.h's SCHED_TF_* must match this file's TF_*");
_Static_assert(TF_VECTOR == SCHED_TF_VECTOR,
               "api/scheduler.h's SCHED_TF_VECTOR must match TF_VECTOR");
_Static_assert(TF_RIP == SCHED_TF_RIP,
               "api/scheduler.h's SCHED_TF_RIP must match TF_RIP");
_Static_assert(TRAPFRAME_WORDS == SIGFRAME_TF_SLOTS,
               "abi/signal_abi.h's SIGFRAME_TF_SLOTS must be a whole trapframe");
_Static_assert(TF_RAX == SCHED_TF_RAX,
               "scheduler.h's public RAX slot index must match the real trapframe layout");

const char *sched_wait_reason_name(int reason) {
    switch (reason) { // dispatch-ok: bounded by scheduler.h's SCHED_WAIT_* labels
    case SCHED_WAIT_EVENT: return "event";
    case SCHED_WAIT_PIPE:  return "pipe";
    case SCHED_WAIT_CHILD: return "child";
    case SCHED_WAIT_TIMER: return "timer";
    case SCHED_WAIT_KEY:   return "key";
    case SCHED_WAIT_TTY:   return "tty";
    case SCHED_WAIT_THREAD: return "join";
    case SCHED_WAIT_NET:   return "net";
    case SCHED_WAIT_FUTEX: return "futex";
    case SCHED_WAIT_SIGNAL: return "signal";
    case SCHED_WAIT_LOCK:  return "lock";
    default:               return "?";
    }
}

static int block_common(uint64_t *regs, const void *chan, int reason,
                        uint64_t wake_at_ns) {
    if (current_index < 0) return 0;

    // NOTE there is deliberately NO "refuse to park a process with a
    // signal pending" check here, and it was written, tested and taken
    // out again rather than never considered. The worry it answered --
    // a woken process re-entering the same blocking call before delivery
    // could run, forever -- is real, and is closed one layer up: a
    // pending signal is delivered at the SYSCALL ENTRY the process makes
    // next (idt.c), so it cannot reach this function at all. A positive
    // control confirmed the check reddened nothing, which made it a
    // guard no test could exercise and a comment claiming a mechanism
    // that was not the one doing the work.
    //
    // WHAT WOULD BRING IT BACK: syscalls that can be preempted
    // (docs/roadmap.md's "Interruptible syscalls"). Every gate is an
    // interrupt gate today, so a syscall runs with interrupts off and
    // nothing can raise a signal against a process part-way through one.

    // **A WAKE THAT ARRIVED WHILE WE WERE CHECKING.** The caller armed
    // before testing its condition, and scheduler_wake() found that arm
    // rather than a blocked process -- so there IS something to do and
    // parking now would sleep through it. Answer as a wake would have,
    // and let the caller loop round and look again.
    if (current_index >= 0 && procs[current_index].armed_woken &&
        procs[current_index].arm_chan == chan) {
        procs[current_index].armed_woken = 0;
        procs[current_index].arm_chan = 0;
        regs[TF_RAX] = (uint64_t)procs[current_index].arm_value;
        return 1;
    }
    if (current_index >= 0) procs[current_index].arm_chan = 0;

    sched_switch_begin();
    bill_current(); // this slice ends here -- see bill_current()
    int idx = current_index;
    procs[idx].kernel_rsp = (uint64_t)regs;
    kstack_verify(idx);
    fpu_save(procs[idx].fpu);
    procs[idx].state = SCHED_BLOCKED;
    trace_sched("block", idx);
    procs[idx].wait_chan = chan;
    // SET UNCONDITIONALLY, and 0 for an unbounded wait. It has to be
    // written on EVERY block rather than only where a deadline is
    // wanted: slots are recycled, so an inherited deadline in the past
    // makes wake_timers() release the process the instant it parks --
    // which, once that loop stopped filtering on SCHED_CHAN_TIMER,
    // turned every blocking wait on the machine into a spin. That is
    // what it did: the whole GUI suite failed at once.
    procs[idx].wake_at_ns = wake_at_ns;
    procs[idx].wait_reason = reason;
    current_index = -1;

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel(idx);
    else switch_to(idx, next);
    return 1;
}

// SYS_SLEEP's half of the block/wake pair: park until a DEADLINE rather
// than until an event.
//
// It is a separate entry point from scheduler_block_current() only
// because the deadline has to be recorded somewhere before the switch,
// and a wait reason cannot carry it: scheduler_wake() releases every
// process parked on a reason at once, which is right for "a pipe has
// data" and wrong for "it is 09:00" -- two sleepers almost never share
// an instant.
int scheduler_block_current(uint64_t *regs, const void *chan, int reason) {
    return block_common(regs, chan, reason, 0);
}

int scheduler_sleep_current(uint64_t *regs, uint64_t wake_at_ns) {
    return block_common(regs, SCHED_CHAN_TIMER, SCHED_WAIT_TIMER, wake_at_ns);
}

int scheduler_block_current_until(uint64_t *regs, const void *chan, int reason,
                                  uint64_t wake_at_ns) {
    return block_common(regs, chan, reason, wake_at_ns);
}

// The deadline-aware counterpart of scheduler_wake(), called once per
// timer tick. Returns how many sleepers it released.
//
// Same restraint as scheduler_wake() and for the same reason: this runs
// in the timer IRQ, so it only flips state and writes an already-saved
// trapframe. The woken process runs at the next ordinary rotation.
//
// The sleep's RESOLUTION is therefore one tick -- a process asking for
// 1 ms sleeps until the next tick, never less. That is deliberate:
// programming a one-shot timer per sleeper is a real tickless design
// and this kernel does not have one, so a caller gets no more precision
// than the clock this loop runs on.
int scheduler_wake_timers(uint64_t now_ns) {
    int woken = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED) continue;
        // A DEADLINE, NOT A CHANNEL, is what this loop acts on. It used
        // to require SCHED_CHAN_TIMER, which made "sleep for a while"
        // the only timed wait the kernel had; a bounded wait on an
        // EVENT -- a datagram that may never arrive -- is the other
        // one, and it is what stops a blocking receive being a hang.
        // Linux's schedule_timeout() is the same primitive: a wait
        // queue and a deadline, either of which may fire first.
        if (!procs[i].wake_at_ns) continue;   // parked with no deadline
        if (procs[i].wake_at_ns > now_ns) continue;

        uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
        // SYS_SLEEP asked for exactly this and returns 0. Anything else
        // was waiting for an EVENT that did not come, so its handler
        // has to look again and decide -- it is the only code that
        // knows whether an empty queue at the deadline is a timeout or
        // a spurious wake.
        tf[TF_RAX] = procs[i].wait_chan == SCHED_CHAN_TIMER
                     ? 0 : (uint64_t)(int64_t)SYS_RETRY;
        procs[i].wake_at_ns = 0;
        procs[i].state = SCHED_READY;
        woken++;
    }
    return woken;
}

// Wakes every process blocked on `reason`, giving each `value` as its
// blocking syscall's return value. Returns how many were woken (0 is
// perfectly normal -- an event with nobody waiting on it).
//
// Safe to call from an interrupt handler, which is the point: this only
// flips state and writes into an already-saved trapframe. It never
// touches g_next_kernel_rsp, so it cannot disturb whatever the
// interrupted context was going to resume into -- the woken process
// simply becomes eligible again and the next ordinary scheduler_tick()
// picks it up. That restraint is deliberate: an IRQ handler that tried
// to switch directly to the woken process is exactly the reentrancy
// this design exists to avoid.
int scheduler_wake(const void *chan, int64_t value) {
    return scheduler_wake_n(chan, value, 0);
}

// `max` waiters, or every one of them when it is 0. A bound exists
// because a futex has one: waking every waiter on a contended mutex so
// that all but one park again is the thundering herd this channel
// mechanism was built to avoid (see api/scheduler.h), and a lock's
// unlock wants exactly one.
// **PARK THE CURRENT CONTEXT MID-CALL, AND CARRY ON WHERE IT STOPPED.**
// The counterpart to scheduler_block_current(), and the reason a single
// suspend shape was worth building: that one parks at a syscall ENTRY
// and is answered by ring 3 asking again, so the C frames beneath it
// are thrown away. This one keeps them, so a caller six frames deep in
// a block walk can wait for a disk and resume on the next line with its
// locals intact.
//
// Returns 0 when there is nowhere to park -- the kernel context, a
// KTEST, the legacy loader, anything with no scheduler slot -- and the
// caller must fall back to polling rather than assume it slept.
//
// THE CALLER MUST ARM BEFORE IT TESTS ITS CONDITION, exactly as the
// entry-point version requires: a wake that lands between the test and
// the park is otherwise lost, and this one has no ring-3 retry loop
// underneath it to paper over that.
int scheduler_block_kernel(const void *chan, int reason) {
    if (current_index < 0) return 0;
    // **AND NOT WHILE THE PREEMPTION GUARD IS RAISED**, which is
    // Linux's "you cannot sleep holding a spinlock". The guard is what
    // makes the non-re-entrant filesystem safe (vfs.c's FS_OP), so a
    // context that slept inside one would let a second walker into
    // tfs3.c's module-level scratch buffers -- the exact corruption the
    // guard exists to prevent, reintroduced by the thing meant to
    // replace it. The caller falls back to polling, as it does for a
    // context with no slot at all.
    if (g_preempt_depth > 0) return 0;
    int idx = current_index;

    // A wake that arrived while we were checking -- see block_common().
    if (procs[idx].armed_woken && procs[idx].arm_chan == chan) {
        procs[idx].armed_woken = 0;
        procs[idx].arm_chan = 0;
        return 1;
    }
    procs[idx].arm_chan = 0;

    sched_switch_begin();
    bill_current();
    kstack_verify(idx);
    fpu_save(procs[idx].fpu);
    procs[idx].state = SCHED_BLOCKED;
    procs[idx].parked_in_kernel = 1;
    trace_sched("block_kernel", idx);
    procs[idx].wait_chan = chan;
    procs[idx].wake_at_ns = 0;   // unbounded -- see block_common()
    procs[idx].wait_reason = reason;
    current_index = -1;

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel(idx);
    else switch_to(idx, next);

    // Resumed, on our own stack, with every frame below us intact.
    procs[idx].parked_in_kernel = 0;
    return 1;
}

int scheduler_wake_n(const void *chan, int64_t value, int max) {
    int woken = 0;
    // **A NULL CHANNEL WAKES NOBODY.** scheduler_wait_chan_pid() answers
    // NULL for a pid outside the table, and 0 is one -- it is what
    // notify_parent() is handed for a process whose parent is "the
    // kernel", which is every program the debug console starts. Without
    // this the arm loop below then matches every process whose
    // arm_chan is 0, i.e. everything not currently armed, and stamps
    // armed_woken on all of them. Measured: 77 such calls in one
    // usertest run, each reporting 1-2 processes "woken" that nobody
    // had asked about.
    if (!chan) return 0;
    // **FIRST, ANYONE WHO HAS ARMED BUT NOT YET PARKED.** Such a process
    // is RUNNING, so the blocked scan below cannot see it and its wake
    // would be dropped -- the lost wakeup that made a preemptible
    // syscall unsafe. Recording it here lets block_common() decline to
    // park. It counts as woken, so a `max` of 1 is still honoured.
    for (int i = 0; i < MAX_PROCS && (!max || woken < max); i++) {
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        if (procs[i].arm_chan != chan || procs[i].armed_woken) continue;
        procs[i].armed_woken = 1;
        procs[i].arm_value   = value;
        woken++;
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (max && woken >= max) break;
        if (procs[i].state != SCHED_BLOCKED) continue;
        if (procs[i].wait_chan != chan) continue;

        // The saved trapframe's RAX slot IS the syscall's return value:
        // isr_common's epilogue pops it straight into the register the
        // ring-3 caller reads. Writing it here is what makes waking a
        // process and answering its syscall the same act.
        // ...but only for a process parked at a syscall ENTRY. One
        // parked mid-call resumes its own C frames and computes its
        // own answer, and its trapframe belongs to a call that has not
        // finished -- writing a return value into it would be
        // answering a question nobody asked.
        if (!procs[i].parked_in_kernel) {
            uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
            tf[TF_RAX] = (uint64_t)value;
        }
        // The wait is over, so its deadline is too. Leaving it set
        // would have the next timer tick "release" a process that is
        // already running -- overwriting the RAX of whatever syscall it
        // had reached by then.
        procs[i].wake_at_ns = 0;
        procs[i].state = SCHED_READY;
        if (procs[i].prio < running_prio()) g_need_resched = 1;
        woken++;
    }
    return woken;
}

// A CHILD HAS GONE: wake a parent parked in waitpid, and tell it.
//
// **ONE HELPER BECAUSE THERE ARE TWO DEATHS.** A process can leave
// through scheduler_on_exit() (it exited, or a signal terminated it
// while it was the running process) or through scheduler_kill()
// (somebody else ended it) -- and this file has already paid once for
// treating those as one path and once for treating them as two: the
// memory-freeing that only lived in the exit path leaked every kill for
// months. So the notification lives in exactly one function and both
// deaths call it, which is the only arrangement where adding a third
// kind of death cannot silently skip it. Linux funnels the same way,
// through exit_notify() -> do_notify_parent().
//
// THE WAKE AND THE SIGNAL ARE DIFFERENT MECHANISMS FOR DIFFERENT
// WAITERS, and both are needed. The wake releases a parent blocked in
// SYS_WAITPID on this specific child's channel -- that is the
// synchronous half, and it is what every waiter in this tree has used
// since blocking landed. SIGCHLD is the asynchronous half: it reaches a
// parent that is NOT in waitpid at all, which is the case a shell
// sitting at an idle prompt is in.
//
// **SIGCHLD COSTS NOTHING FOR A PARENT THAT NEVER ASKED FOR IT.**
// signal_send() drops a default-ignored signal with no handler
// installed before it reaches the pending set (signal.c), so every
// existing program -- init, the desktop, every GUI client -- pays one
// call and one compare per child death and is otherwise untouched. That
// is exactly why Unix made SIGCHLD's default "ignore": it is what lets
// the kernel send one on every exit without every program having to
// learn about it first.
//
// NOT SENT FOR A STOP OR A CONTINUE, deliberately. POSIX sends SIGCHLD
// for those too (absent SA_NOCLDSTOP), and toy-os does not: a stop is
// already reported to a waiter that asked, through SYS_WUNTRACED's
// SIGNAL_STOP_BASE (abi/signal_abi.h), which is the only consumer there
// is. Adding a second, asynchronous route to the same news would mean
// raising a pending bit from the keyboard IRQ that delivers Ctrl-Z, for
// a fact nothing reads. Revisit if something ever needs to hear about a
// suspension without asking.
static void notify_parent(int ppid) {
    scheduler_wake(scheduler_wait_chan_pid(ppid), SYS_RETRY);
    signal_send(ppid, SIGCHLD);
    // THE THIRD ROUTE, for a parent that is waiting on SEVERAL things at
    // once and so is parked on neither this child's channel nor in a
    // signal. A supervisor serving requests as well as reaping children
    // is exactly that (kernel/futex.h); it costs a compare for every
    // parent that never registered one.
    futex_note_ready(ppid);
}

// **WHO FREED A CHILD SOMEBODY WAS WAITING FOR.** docs/bugs.md's
// `block(child)` stall is a parent parked on a child that is no longer
// in the table, and the one thing a `ps` after the fact cannot say is
// which code path returned that slot. The legitimate reaper is the
// PARENT, through scheduler_poll()/_poll_any() -- and a parked parent
// is by definition not calling either. So any reap of a child whose
// parent is blocked waiting for one is the anomaly, reported where it
// happens with the caller's name.
//
// Cheap: one compare on a path that runs at most once per process
// death. Remove it when the stall has a cause.
static void reap_audit(int idx, const char *who) {
    int ppid = procs[idx].ppid;
    if (ppid < 1 || ppid > MAX_PROCS) return;
    // **A CHILD IS REAPED BY ITS PARENT, AND BY NOBODY ELSE.** Checking
    // the parent's STATE instead was the first version of this and it
    // never fired: the window is between the parent's poll and its
    // park, where it is still RUNNING, so "is the parent blocked?" is
    // false exactly when the damage is done. Who reaps is a fact that
    // does not depend on that timing.
    int reaper = scheduler_current_tgid();
    if (reaper == ppid) return;          // the parent itself: correct
    klog_printf(KLOG_ERR "REAP BY NON-PARENT: %s (pid %d) freed pid %d "
                "(\"%s\", state %d) whose parent is pid %d\n",
                who, reaper, idx + 1, procs[idx].name, procs[idx].state, ppid);
    scheduler_trace_dump();
}

// Free a slot outright, keeping the live count honest whichever state
// it was in. A ZOMBIE has already been subtracted.
static void slot_release(int idx) {
    reap_audit(idx, "slot_release");
    if (procs[idx].state == SCHED_UNUSED) return;
    if (procs[idx].state != SCHED_ZOMBIE) alive_count--;
    procs[idx].state = SCHED_UNUSED;
}

// Every OTHER thread of `leader`'s group stops existing.
//
// Freed outright rather than zombied: a thread is not waitable by
// anything outside its own process, so a corpse nobody can reap would
// hold its slot for the rest of the boot. Their kernel stacks are
// simply never resumed again -- the same thing scheduler_kill() does to
// a process parked mid-syscall, and sound for the same reason: a
// trapframe lives on its own slot's stack and nothing outside the slot
// points at it.
static void group_release_threads(int leader) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (i == leader) continue;
        if (procs[i].state == SCHED_UNUSED) continue;
        if (procs[i].tgid == leader + 1) slot_release(i);
    }
}

void scheduler_on_exit(int code) {
    if (current_index < 0) return; // defensive; shouldn't happen

    // **AN EXIT IS NOT INTERRUPTIBLE, and it is the state below that
    // says so rather than the teardown.** The slot is marked ZOMBIE
    // here and the switch happens at the bottom; a tick in between runs
    // the rotation, which saves the frame and marks the slot READY --
    // over the ZOMBIE. The process is then resumed part-way through
    // this function, `switch_to()` makes it RUNNING on the way in, and
    // the tail sets `current_index` to -1 and leaves it there: RUNNING
    // while nothing is current, which only the rotation could undo and
    // only for a process that is current. Unschedulable, unreapable,
    // and the parent never hears that it exited.
    //
    // The guard rather than interrupts-off, because the teardown below
    // releases descriptors and can reach the disk -- a wait that needs
    // the very interrupt a `cli` would hold off. Only a trap gate makes
    // this reachable at all: an interrupt gate clears IF for the whole
    // syscall, which is what hid it.
    scheduler_preempt_disable();

    // A PROCESS EXITS AS A WHOLE, whichever of its threads called
    // exit() -- POSIX's exit_group(), and not a choice: there is one
    // address space and the cleanup below is about to destroy it, so a
    // surviving thread would be resumed into unmapped memory.
    //
    // The status is reported on the LEADER's slot even when a thread is
    // what exited, because the leader's pid is what the parent waited
    // for. The calling thread's own slot is freed with its siblings'.
    int leader = leader_index(current_index);
    group_release_threads(leader);

    // SCHED_ZOMBIE, not SCHED_UNUSED -- see this file's comment on that
    // enum value. The slot (and its exit_code) stays held until whoever
    // spawned it calls scheduler_poll().
    if (procs[leader].state != SCHED_ZOMBIE) alive_count--;
    procs[leader].state = SCHED_ZOMBIE;
    procs[leader].exit_code = code;

    // A REMOTE SESSION ENDS WHEN ITS LEADER DOES, whether it said
    // goodbye or the link dropped -- a no-op for every other process,
    // and what stops the tray indicator outliving the connection.
    remote_log_session_closed(leader + 1);

    // Tell the window server to drop anything this client still owned.
    // Here rather than at reap: a zombie's windows must come off the
    // screen the moment it dies, not whenever someone gets round to
    // polling it -- otherwise a crashed client leaves a window that
    // draws stale pixels and answers no input. A no-op when no server
    // is registered, which is every non-GUI boot.
    win_server_client_gone(leader + 1);
    diag_provider_gone(leader + 1);

    // Its children lose their parent before anything can reuse this
    // slot -- see reparent_children() for why that ordering matters.
    reparent_children(leader + 1);

    // A parent blocked in SYS_WAITPID has to hear about this -- and
    // ONLY that parent. This used to wake every child-waiter in the
    // system, each to re-check its own children and park again; the
    // channel is the parent's slot, so an exit reaches exactly the
    // process that might care. The SIGCHLD beside it is for a parent
    // that is not waiting at all -- see notify_parent().
    notify_parent(procs[leader].ppid);

    // Handed straight to the switch, which holds interrupts off from
    // here -- so nothing can land between the two.
    scheduler_preempt_enable();

    // The write end that turns a parent's blocking read into EOF is
    // closed by fd_release_all() now, along with every other
    // descriptor this process held -- there is no separate
    // "stdout_pipe" to remember, because stdout is an ordinary
    // descriptor like the rest.

    sched_switch_begin();
    bill_current(); // the exiting process's last slice
    int gone = current_index;
    trace_sched("exit", current_index);
    current_index = -1;

    // Continue the rotation from the slot that just exited (which is
    // still what rotation_pos holds), rather than restarting at slot 0
    // -- same fairness the tick above gets, and it means the kernel's
    // position is reached normally instead of being skipped on an exit.
    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) {
        switch_to_kernel(gone);
        return;
    }
    switch_to(gone, next);
}

// --- threads ---------------------------------------------------------
//
// A thread is an ordinary slot with somebody else's `tgid`. It gets its
// own kernel stack, FP state, trapframe, signal table and thread
// pointer; it shares its leader's address space, and through the
// address space the fd table, because that is keyed by CR3 and never
// learned about pids at all (syscall_fd.c).
//
// What this is NOT is fork(): there is no copy of anything. The new
// thread starts at an address ring 3 named, on a stack ring 3
// allocated, which is clone(CLONE_VM|CLONE_FILES)'s shape rather than
// pthread_create()'s -- the library half lives in ring 3 where it
// belongs (userland/libc/pthread.c).
int scheduler_thread_create(uint64_t entry, uint64_t user_rsp, uint64_t arg,
                             uint64_t fs_base, int detached) {
    // The kernel context has no address space to share, and the legacy
    // loader has no slot to lead a group -- both are "not a process".
    if (current_index < 0) return -EPERM;
    int caller = current_index;
    int leader = leader_index(caller);

    if (!entry || !user_rsp) return -EFAULT;
    // THE STACK IS THE CALLER'S, so a bad pointer must fail HERE, where
    // the caller can see -EFAULT, rather than as a page fault on the new
    // thread's first push -- which would kill the whole process for a
    // mistake one call made. Validating also faults the page in, which
    // is what makes a freshly malloc'd stack usable: the heap is
    // demand-paged, so the memory the caller "has" is not mapped yet.
    if (!vmm_validate_user_range(procs[leader].pml4_phys, user_rsp - 64, 64))
        return -EFAULT;

    int slot = -1;
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == SCHED_UNUSED) { slot = i; break; }
    if (slot < 0) return -EAGAIN;

    // The same synthesized first trapframe a spawn builds, minus
    // everything about loading an image: this thread's code is already
    // mapped, because it is its creator's.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0;
    tf[TF_RDI]     = arg;   // the SysV first argument: void *arg
    tf[TF_VECTOR]  = 0;
    tf[TF_ERRCODE] = 0;
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    // **RSP % 16 == 8 AT ENTRY, NOT 0**, which is the same trap
    // crt0.asm documents and which this line got backwards for one
    // build. SysV states the rule at the CALLEE: a `call` has just
    // pushed 8 bytes, so a function begins with RSP % 16 == 8 and GCC
    // sizes its prologue from that. Hand it a 16-ALIGNED RSP and every
    // `movaps` it emits against a stack slot faults with a #GP.
    //
    // `(x & ~15) - 8`, not `(x - 8) & ~15` -- the second is always
    // 16-aligned, i.e. always the broken case. It passed every
    // thread test in the tree, because none of those workers used SSE;
    // the first GUI client to run one crashed on its first snprintf.
    tf[TF_RSP]     = (user_rsp & ~15ull) - 8;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = procs[leader].pml4_phys; // SHARED, not created
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_chan   = 0;
    procs[slot].wait_reason = 0;
    procs[slot].tgid     = leader + 1;
    procs[slot].fs_base  = fs_base;
    procs[slot].detached = detached ? 1 : 0;
    // Its parent is its leader, which is what makes `ps --tree` show a
    // thread under the process it belongs to. Every walk over a
    // process's CHILDREN skips threads, so this is a display fact and
    // never a wait() one.
    procs[slot].ppid     = leader + 1;
    procs[slot].pgid     = procs[leader].pgid;
    procs[slot].prio     = procs[caller].prio;
    signal_state_reset(slot);
    // DISPOSITIONS ARE INHERITED, which is as close to POSIX's
    // per-process disposition as a per-thread table gets: a thread
    // created after signal(SIGINT, h) runs the same handler its creator
    // would. `pending` is not inherited -- a signal raised before this
    // thread existed was not raised at it.
    for (int i = 0; i <= SIGNAL_MAX; i++)
        procs[slot].actions[i] = procs[caller].actions[i];
    procs[slot].ready   = 0;
    procs[slot].cpu_ns  = 0;
    k_strlcpy(procs[slot].name, procs[leader].name, sizeof procs[slot].name);
    k_strlcpy(procs[slot].exec_path, procs[leader].exec_path,
              sizeof procs[slot].exec_path);
    // The heap and the cwd belong to the GROUP and are read through the
    // leader (scheduler_current_mm/_cwd). Zeroed rather than copied, so
    // a reader that forgets gets an obvious 0 instead of a second copy
    // that drifts.
    k_memset(&procs[slot].mm, 0, sizeof procs[slot].mm);
    procs[slot].cwd.path[0] = '\0';
    procs[slot].state = SCHED_READY;
    alive_count++;
    return slot + 1;
}

// --- fork ------------------------------------------------------------
//
// What a fork copies is stated once, in docs/fork-design.md's table;
// this function is that table in order. Two things are not obvious
// from the table. THE FRAMES THE KERNEL HOLDS A PHYSICAL POINTER INTO
// ARE COPIED EAGERLY, not shared: a futex waiter is parked on its
// word's physical address and the wakeword is written by one, so if the
// parent un-shared such a page its waiters would be keyed to the frame
// the child now owns -- a lost wakeup. And the FP state is the
// caller's LIVE registers (the kernel is -mno-sse, so they are still in
// the CPU), which is what a fork means.
static int fork_inherits_borrowed(void *ctx, uint64_t va) {
    return mmap_inherits_at(ctx, va);
}

int scheduler_fork(const uint64_t *regs) {
    if (current_index < 0) return -EPERM; // the kernel context, or the legacy loader
    int caller = current_index;
    int leader = leader_index(caller);

    int slot = -1;
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == SCHED_UNUSED) { slot = i; break; }
    if (slot < 0) return -EAGAIN;

    uint64_t pinned[MAX_PROCS + 1];
    int npin = 0;
    uint64_t ww = futex_wakeword_phys(leader + 1);
    if (ww) pinned[npin++] = ww;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED || procs[i].tgid != leader + 1) continue;
        if (procs[i].wait_chan) pinned[npin++] = (uint64_t)(uintptr_t)procs[i].wait_chan;
    }
    struct vmm_fork_opts o = { pinned, npin, fork_inherits_borrowed, &procs[leader].mm };
    uint64_t as = vmm_fork_address_space(procs[leader].pml4_phys, &o);
    if (!as) return -ENOMEM;
    if (mmap_inherit_shm(as, &procs[leader].mm) < 0) {
        vmm_destroy_address_space(as);
        return -ENOMEM;
    }

    // The caller's trapframe, verbatim, on the child's own kernel stack
    // -- so the child resumes at the instruction after the `int $0x80`
    // with every register the parent had, except the one that tells
    // them apart.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TRAPFRAME_WORDS; i++) tf[i] = regs[i];
    tf[TF_RAX] = 0;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    proc_start_context(slot, tf);
    kstack_arm_slot(slot);
    fpu_save(procs[slot].fpu);
    procs[slot].wait_chan   = 0;
    procs[slot].wait_reason = 0;
    procs[slot].wake_at_ns  = 0;
    procs[slot].tgid     = slot + 1;
    procs[slot].fs_base  = procs[caller].fs_base;
    procs[slot].detached = 0;
    fd_clone(as, procs[leader].pml4_phys);
    procs[slot].ppid = leader + 1;
    procs[slot].prio = procs[caller].prio;
    signal_state_reset(slot);
    for (int i = 0; i <= SIGNAL_MAX; i++)
        procs[slot].actions[i] = procs[caller].actions[i];
    procs[slot].blocked = procs[caller].blocked;
    procs[slot].syscall_reissue = 0;
    procs[slot].ready   = 0;
    procs[slot].pgid    = procs[leader].pgid;
    // THE SESSION TOO. A fork that copied the group and not the session
    // put the child in session 0, so it could not take the terminal its
    // parent owned -- which is the whole point of having sessions.
    procs[slot].sid     = procs[leader].sid;
    k_strlcpy(procs[slot].name, procs[leader].name, sizeof procs[slot].name);
    k_strlcpy(procs[slot].exec_path, procs[leader].exec_path,
              sizeof procs[slot].exec_path);
    procs[slot].cpu_ns = 0;
    k_memcpy(&procs[slot].mm, &procs[leader].mm, sizeof procs[slot].mm);
    // ...which copied the region POINTER. Give the child its own, or
    // the two of them free one array twice.
    if (!mmap_clone_regions(&procs[slot].mm, &procs[leader].mm)) {
        procs[slot].state = SCHED_UNUSED;
        return -1;
    }
    k_memcpy(&procs[slot].cwd, &procs[leader].cwd, sizeof procs[slot].cwd);
    procs[slot].state = SCHED_READY;
    alive_count++;
    return slot + 1;
}

// --- exec ------------------------------------------------------------
//
// The caller's slot keeps its pid, parent, group, cwd and descriptors
// and gets a new image. THE NEW ADDRESS SPACE IS BUILT BEFORE THE OLD
// ONE IS TOUCHED, so a program that cannot be loaded is reported to a
// caller that still exists (POSIX: exec fails in place). Everything
// keyed by the old address space is either re-keyed (descriptors, the
// trace) or dropped through the same hooks an exit uses (shared
// mappings, the wakeword, windows, sound) -- an exec'd program has no
// idea it holds any of them. Returns 0 into a rewritten trapframe, or
// -errno with nothing changed.
int scheduler_exec(const char *path, const char *argvec, size_t argvec_len,
                   const char *env, uint64_t *regs) {
    if (current_index < 0) return -EPERM;
    int me = current_index;
    if (is_thread(me)) {
        // POSIX makes the exec'ing thread the leader, pid and all. Not
        // worth a second exit path: refuse, loudly.
        klog_printf(KLOG_ERR "exec: refused from thread %d -- only a process may exec\n", me + 1);
        return -EPERM;
    }
    uint64_t as = 0, entry = 0, user_rsp = 0, image_end = 0;
    if (!build_image(path, argvec, argvec_len, env, &as, &entry, &user_rsp, &image_end))
        return -ENOENT;

    uint64_t old = procs[me].pml4_phys;
    group_release_threads(me);
    strace_rekey(old, as);
    fd_rekey(old, as);
    proc_syscall_release(old);
    win_server_client_gone(me + 1);
    diag_provider_gone(me + 1);
    sound_process_gone(old);
    shm_process_gone(old);
    futex_wakeword_release(old);

    // A caught signal goes back to its default; an ignored one stays
    // ignored (POSIX). The pending set and the blocked mask are kept.
    for (int i = 0; i <= SIGNAL_MAX; i++)
        if (procs[me].actions[i].handler > SIG_IGN)
            procs[me].actions[i] = (struct k_sigaction){ 0, 0, 0, 0 };
    procs[me].syscall_reissue = 0;
    procs[me].ready = 0;
    procs[me].fs_base = 0;
    arch_set_fs_base(0);            // this return does not go through switch_to()
    fpu_init_state(procs[me].fpu);
    fpu_restore(procs[me].fpu);     // ...so the CPU's state is loaded here too
    mm_reset(me, image_end);
    proc_name_from_path(procs[me].name, sizeof procs[me].name, path);
    k_strlcpy(procs[me].exec_path, path ? path : "", sizeof procs[me].exec_path);

    // The caller's own trapframe, rewritten: it resumes at the new
    // image's entry with every register clear, as a spawn's first frame.
    for (int i = 0; i < TF_VECTOR; i++) regs[i] = 0;
    regs[TF_RIP]    = entry;
    regs[TF_CS]     = SEL_USER_CODE;
    regs[TF_RFLAGS] = 0x200;
    regs[TF_RSP]    = user_rsp;
    regs[TF_SS]     = SEL_USER_DATA;

    procs[me].pml4_phys = as;
    vmm_switch_address_space(as);   // CR3 first -- see vmm_destroy_address_space()
    vmm_destroy_address_space(old);
    return 0;
}

// One THREAD ends; the process does not.
//
// The leader is the exception, and deliberately: pthread_exit() from
// the initial thread would have to leave a zombie leader holding the
// tgid while its siblings ran on, with nothing in this kernel able to
// wait for that. It exits the PROCESS instead -- a divergence from
// POSIX, where the process survives until the last thread leaves.
void scheduler_on_thread_exit(int code) {
    if (current_index < 0) return;
    if (!is_thread(current_index)) { scheduler_on_exit(code); return; }
    scheduler_preempt_disable();   // as in scheduler_on_exit(), and for its reason

    int me = current_index;
    procs[me].exit_code = code;
    if (procs[me].detached) {
        slot_release(me);      // nobody is coming to reap it
    } else {
        procs[me].state = SCHED_ZOMBIE;
        alive_count--;
    }
    // Whoever is joining. Harmless when nobody is: a wake with no
    // waiter on the channel is a loop over the table finding nothing.
    scheduler_wake(scheduler_wait_chan_pid(me + 1), SYS_RETRY);
    scheduler_preempt_enable();

    sched_switch_begin();
    bill_current();
    current_index = -1;
    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) { switch_to_kernel(me); return; }
    switch_to(me, next);
}

// Reap `tid` if it is dead, and say so; otherwise say "not yet".
//
// A THREE-VALUED ANSWER rather than a blocking call, for the reason
// every other wait here is shaped this way: the caller (proc_syscalls.c)
// is what parks, because parking means writing the CALLER's trapframe
// and only a syscall handler holds one.
enum sched_poll_result scheduler_thread_poll(int tid, int *out_code) {
    if (tid < 1 || tid > MAX_PROCS) return SCHED_POLL_INVALID;
    if (current_index < 0) return SCHED_POLL_INVALID;
    int slot = tid - 1;
    // ONLY WITHIN ONE PROCESS. A tid is a slot index like any other, so
    // without this a program could join another program's thread and
    // free its slot.
    if (procs[slot].state == SCHED_UNUSED) return SCHED_POLL_INVALID;
    if (procs[slot].tgid != procs[current_index].tgid) return SCHED_POLL_INVALID;
    if (slot == current_index) return SCHED_POLL_INVALID; // joining itself
    if (!is_thread(slot)) return SCHED_POLL_INVALID;      // the leader is not joinable
    if (procs[slot].detached) return SCHED_POLL_INVALID;  // and neither is a detached one

    if (procs[slot].state == SCHED_ZOMBIE) {
        if (out_code) *out_code = procs[slot].exit_code;
        reap_audit(slot, "thread_poll");
        procs[slot].state = SCHED_UNUSED; // reaped -- see scheduler_poll()
        return SCHED_POLL_EXITED;
    }
    return SCHED_POLL_RUNNING;
}

// Nobody will join `tid`, so let its exit free the slot. Applied to a
// thread that has ALREADY exited, this reaps it -- which is what makes
// detach-after-the-fact safe rather than a leak.
int scheduler_thread_detach(int tid) {
    if (tid < 1 || tid > MAX_PROCS) return -EINVAL;
    if (current_index < 0) return -EPERM;
    int slot = tid - 1;
    if (procs[slot].state == SCHED_UNUSED) return -ESRCH;
    if (procs[slot].tgid != procs[current_index].tgid) return -ESRCH;
    if (!is_thread(slot)) return -EINVAL;
    if (procs[slot].detached) return -EINVAL;

    procs[slot].detached = 1;
    if (procs[slot].state == SCHED_ZOMBIE) { reap_audit(slot, "thread_detach"); procs[slot].state = SCHED_UNUSED; }
    return 0;
}

// The thread pointer this thread's %fs resolves against. Ring 3 owns
// the layout behind it entirely (userland/rt/tls.c); the kernel only
// remembers the number and reloads it on every switch.
int scheduler_set_tls(uint64_t base) {
    // A ring-3 process the LEGACY loader is running has no slot, and it
    // still needs a thread pointer -- every program does, since errno
    // is a `__thread` variable now. It goes in the kernel context's own
    // slot, which switch_to_kernel() reloads.
    if (current_index < 0) kernel_fs_base = base;
    else                   procs[current_index].fs_base = base;
    arch_set_fs_base(base); // whoever asked is running -- take effect now
    return 0;
}

int scheduler_current_pid(void) {
    return current_index < 0 ? 0 : current_index + 1;
}

// The PROCESS on the CPU, where scheduler_current_pid() is the THREAD.
// Equal for everything that is not a thread, which is why every caller
// that predates threads kept working -- and why the ones that mean "the
// process" (a window's owner, a terminal's owner, a child's parent,
// getpid) had to be moved over one at a time rather than in bulk.
int scheduler_current_tgid(void) {
    if (current_index < 0) return 0;
    return procs[current_index].tgid;
}

int scheduler_tgid(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    if (procs[pid - 1].state == SCHED_UNUSED) return 0;
    return procs[pid - 1].tgid;
}

int scheduler_exec_path(int pid, char *out, unsigned cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (pid < 1 || pid > MAX_PROCS) return 0;   // slot is pid - 1, as everywhere here
    struct sched_process *p = &procs[pid - 1];
    if (p->state == SCHED_UNUSED) return 0;
    // REFUSE rather than truncate: callers match on this string to
    // decide which program a window belongs to, and a shortened path
    // matches the wrong one.
    if (k_strlcpy(out, p->exec_path, cap) >= cap) { out[0] = '\0'; return 0; }
    return out[0] ? 1 : 0;
}

struct sched_mm *scheduler_current_mm(void) {
    // NULL means "the kernel context is running", which for SYS_SBRK is
    // the legacy elf_run.c process -- not "this process has no heap".
    // Every slot gets one at creation.
    if (current_index < 0) return 0;
    // THE LEADER'S, not this slot's: threads share one heap because they
    // share one address space, and a per-thread copy of `brk` would let
    // two sbrk()s hand out the same page.
    return &procs[leader_index(current_index)].mm;
}

// The KERNEL CONTEXT's own directory -- the legacy elf_run.c loader and
// anything the kernel shell starts. Same two-owners-one-representation
// shape SYS_SBRK's heap has (proc_syscalls.c's g_legacy_heap): the
// legacy path has no scheduler slot to keep this in, so it gets one of
// the same TYPE, reached through the same accessors, and the two cannot
// drift in behaviour.
static struct sched_cwd kernel_cwd = { "/" };

const char *scheduler_cwd(void) {
    struct sched_cwd *c = scheduler_current_cwd();
    return c ? c->path : kernel_cwd.path;
}

void scheduler_set_kernel_cwd(const char *path) {
    if (!path || path[0] != '/') return; // callers pass an already-resolved path
    k_strlcpy(kernel_cwd.path, path, sizeof kernel_cwd.path);
}

struct sched_cwd *scheduler_current_cwd(void) {
    // Same NULL convention as scheduler_current_mm(): "the kernel
    // context is running", i.e. the legacy loader's slot applies.
    if (current_index < 0) return 0;
    // The leader's, for scheduler_current_mm()'s reason: `cd` in one
    // thread moves the whole process, which is what chdir() means.
    return &procs[leader_index(current_index)].cwd;
}

// The memory map behind a given address space. Walks the table because the
// caller (a page fault, or a copy helper) has a pml4 and not a pid --
// and a ZOMBIE is skipped deliberately: its address space is already
// destroyed, so a fault naming it is a stale mapping, not a heap page.
struct sched_mm *scheduler_mm_for_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        // Threads share their leader's address space, so every one of
        // them would match -- and the first match must be the one slot
        // that owns the heap, or a growth fault in a thread would move
        // a `brk` nothing else reads.
        if (is_thread(i)) continue;
        if (procs[i].pml4_phys == pml4_phys) return &procs[i].mm;
    }
    return 0;
}

struct sched_mm *scheduler_mm_for_pid(int pid) {
    int slot = pid - 1;
    if (slot < 0 || slot >= MAX_PROCS) return 0;
    if (procs[slot].state == SCHED_UNUSED || procs[slot].state == SCHED_ZOMBIE)
        return 0;
    if (is_thread(slot)) return 0;
    return &procs[slot].mm;
}

int scheduler_spawn(const char *path, const char *args) {
    return scheduler_spawn_piped(path, args, -1);
}

int scheduler_spawn_piped(const char *path, const char *args, int pipe_idx) {
    // No environment. Kernel-side spawners (init, the demo) have none
    // to pass -- an environment is a ring-3 idea that the kernel only
    // ever relays.
    return scheduler_spawn_env(path, args, pipe_idx, 0);
}

int scheduler_spawn_env(const char *path, const char *args, int pipe_idx,
                         const char *env) {
    // THE STRING FORM ENDS HERE: split into the vector everything below
    // carries. On the heap, since SPAWN_ARGS_MAX does not fit a frame.
    char *vec = kmalloc(SPAWN_ARGS_MAX + FS_PATH_MAX);
    if (!vec) return 0;
    int pid = 0;
    size_t vec_len = 0;
    if (elf_argv_from_string(path, args, vec, SPAWN_ARGS_MAX + FS_PATH_MAX, &vec_len)) {
        // 0 = inherit the spawner's group, which is what every kernel-side
        // caller wants: init's services and the demo's counters belong with
        // whatever started them. Parent 0 too: a kernel-side caller's child
        // gets the standard three fds (see spawn_from_fs()'s fd_inherit).
        pid = scheduler_spawn_group(path, vec, vec_len, pipe_idx, -1, env, 0, 0);
    }
    kfree(vec);
    return pid;
}

int scheduler_spawn_group(const char *path, const char *argv, size_t argv_len,
                           int pipe_idx, int stdin_desc, const char *env, int pgid,
                           uint64_t parent_pml4) {
    int slot = spawn_from_fs(path, argv, argv_len, pipe_idx, stdin_desc, env, pgid,
                              parent_pml4);
    if (slot < 0) return 0;

    // Clear any events left over from the previous tenant of this slot.
    // Doing it at spawn rather than at reap is what makes this the only
    return slot + 1; // 1-based pid (see scheduler.h)
}

// The address space of a slot that is genuinely running, or 0. Kernel
// side only, and deliberately NOT part of struct proc_info: a CR3 is
// not something userland has any business seeing.
//
// A ZOMBIE answers 0 because its address space is already gone -- see
// scheduler_kill() -- and a caller auditing page tables must not walk
// freed ones.
uint64_t scheduler_slot_pml4(int slot) {
    if (slot < 0 || slot >= MAX_PROCS) return 0;
    if (procs[slot].state == SCHED_UNUSED || procs[slot].state == SCHED_ZOMBIE) return 0;
    return procs[slot].pml4_phys;
}

// **IS `pid` A CHILD OF `parent_pid`?** waitpid() needs this and did
// not have it: scheduler_poll() answers about ANY pid, so a caller that
// named something that was never its child was told RUNNING and parked
// on its OWN channel -- while that process's death woke its real
// parent's channel instead, and the waiter slept for the rest of the
// boot. POSIX answers ECHILD there, and pids RECYCLE here, so "the pid
// I spawned" and "the process in that slot now" are not the same
// question on a machine that has started fifty processes.
//
// A THREAD IS NOT A CHILD, the same rule scheduler_poll_any() keeps.
int scheduler_is_child_of(int pid, int parent_pid) {
    // **A CALLER WITH NO SLOT IS NOBODY'S PARENT.** The legacy loader
    // runs with current_index < 0, so scheduler_current_tgid() is 0 --
    // and init's ppid is 0 too, which made "is init a child of nobody?"
    // answer YES and let a wait through to a park that then refused.
    // Caught by the errno test asking for ECHILD and getting EPERM.
    if (parent_pid < 1) return 0;
    if (pid < 1 || pid > MAX_PROCS) return 0;
    int slot = pid - 1;
    if (procs[slot].state == SCHED_UNUSED) return 0;
    if (is_thread(slot)) return 0;
    return procs[slot].ppid == parent_pid;
}

int scheduler_pid_valid(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    return procs[pid - 1].state != SCHED_UNUSED;
}

// Called when a process dies, on both paths. Its children lose their
// parent, and the reason that MATTERS is not tidiness: a pid is a slot
// index plus one, and slots are reused. Leaving a child pointing at its
// dead parent's pid means that as soon as the slot is handed out again,
// the child claims to be the new process's child -- and a waitpid(-1)
// from that new process would hand it somebody else's corpse.
//
// They are adopted by INIT when there is one, and become parentless
// (ppid 0) when there is not -- which is every boot before init is
// spawned, and any boot where /bin/init is missing. Adoption is what
// makes an orphan reapable: a zombie is only ever reaped by somebody
// waiting for it, so a corpse whose parent is 0 holds its slot for the
// rest of the boot.
//
// Init adopting ITSELF is impossible (it has no parent to die), but
// init dying would hand its children to itself; the guard below keeps
// the tree acyclic whatever happens.
static void reparent_children(int dead_pid) {
    if (dead_pid <= 0) return;
    int heir = (g_init_pid != dead_pid) ? g_init_pid : 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        // A THREAD IS NOT A CHILD. Its ppid names its leader for
        // display only, and by the time a leader dies its threads are
        // already gone -- adopting one out to init would hand init a
        // slot it can never reap.
        if (is_thread(i)) continue;
        if (procs[i].state != SCHED_UNUSED && procs[i].ppid == dead_pid) {
            procs[i].ppid = heir;
        }
    }
    // An adopted ZOMBIE is one init can reap immediately, and it may be
    // parked in waitpid(-1) right now with no children of its own -- in
    // which case it was told "never" and is asleep on a timer instead.
    // Waking child-waiters here is what turns adoption into a reap
    // rather than a slot that frees at init's next poll.
    if (heir) scheduler_wake(scheduler_wait_chan_pid(heir), SYS_RETRY);
}

// Give `pid` a new parent. 0 means "no parent".
//
// The general form of what reparent_children() does to a dying
// process's children, and it exists as a public call because adoption
// is the other half of the same idea: stage 1 of docs/init-design.md
// has init adopt orphans instead of leaving them parentless, and that
// is this function with a different second argument.
//
// Refuses to make a process its own parent, which would make the tree
// a cycle and hang any walk of it.
int scheduler_reparent(int pid, int new_ppid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    if (new_ppid < 0 || new_ppid > MAX_PROCS) return 0;
    if (new_ppid == pid) return 0;
    if (procs[pid - 1].state == SCHED_UNUSED) return 0;
    procs[pid - 1].ppid = new_ppid;
    return 1;
}

// Reap any ONE dead child of `parent_pid`, which is what an init does
// all day and what waitpid(-1) exposes to ring 3.
//
// Three outcomes, and the third is the one a caller must not confuse
// with the second: EXITED reaped a child and filled both out-params,
// RUNNING means there are children and none has died yet, and INVALID
// means this process has NO children at all -- which is a permanent
// answer, where RUNNING is a "not yet". A caller that treats them alike
// either spins forever or gives up too early.
enum sched_poll_result scheduler_poll_any(int parent_pid, int *out_pid,
                                           int *out_exit_code) {
    if (parent_pid < 1 || parent_pid > MAX_PROCS) return SCHED_POLL_INVALID;

    int any_children = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED) continue;
        // A thread is not a child: wait() must never hand a process one
        // of its own threads, which is the rule Linux spells
        // __WNOTHREAD. scheduler_thread_poll() is how a thread is
        // collected.
        if (is_thread(i)) continue;
        if (procs[i].ppid != parent_pid) continue;
        any_children = 1;
        if (procs[i].state == SCHED_ZOMBIE) {
            if (out_pid) *out_pid = i + 1;
            if (out_exit_code) *out_exit_code = procs[i].exit_code;
            reap_audit(i, "poll_any");
            procs[i].state = SCHED_UNUSED; // reap, as scheduler_poll() does
            return SCHED_POLL_EXITED;
        }
    }
    return any_children ? SCHED_POLL_RUNNING : SCHED_POLL_INVALID;
}

// --- signals and process groups --------------------------------------
//
// The STATE only. What a signal means and when it is acted on is
// kernel/signal.c -- see api/scheduler.h for why the two are split.

// The slot behind `pid` if it is a live process, else NULL. A ZOMBIE is
// deliberately not live: it has no address space left to signal and
// nothing to interrupt.
static struct sched_process *live_slot(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    struct sched_process *p = &procs[pid - 1];
    if (p->state == SCHED_UNUSED || p->state == SCHED_ZOMBIE) return 0;
    return p;
}

uint64_t scheduler_pid_pml4(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->pml4_phys : 0;
}

int scheduler_pid_alive(int pid) {
    return live_slot(pid) != 0;
}

int scheduler_pgid(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->pgid : 0;
}

// **IS THIS PROCESS GROUP ORPHANED?** POSIX's definition, not a
// shorthand: a group is orphaned when no member has a parent that is
// both ALIVE, in a DIFFERENT group, and in the SAME SESSION. The parent
// being elsewhere in the session is what makes it possible for anyone
// to notice the group stopped and continue it.
//
// It exists because of what happens otherwise. A background read stops
// the reader with SIGTTIN, which is right while somebody could resume
// it -- and is a process stopped forever once nobody can. The shell in
// a closed terminal window is exactly that: reparented to init, its
// group's only outside parent gone. POSIX answers such a read with EIO
// instead, and tty.c's tty_check_background_read() is the caller.
int scheduler_pgid_orphaned(int pgid) {
    if (pgid < 1) return 0;
    int any = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        if (procs[i].pgid != pgid) continue;
        any = 1;
        int ppid = procs[i].ppid;
        if (ppid < 1 || ppid > MAX_PROCS) continue;
        const struct sched_process *par = &procs[ppid - 1];
        if (par->state == SCHED_UNUSED || par->state == SCHED_ZOMBIE) continue;
        // A parent inside the group cannot rescue it, and one in another
        // SESSION has no claim on this terminal.
        if (par->pgid == pgid) continue;
        if (par->sid != procs[i].sid) continue;
        return 0;
    }
    // A group with no members is not orphaned; it does not exist.
    return any;
}

int scheduler_pgid_live(int pgid) {
    if (pgid < 1) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        if (procs[i].pgid == pgid) return 1;
    }
    return 0;
}

// See the fields' comment. Announces that the caller is ABOUT to wait on
// `chan`, so a wake arriving before it parks is not lost. Every blocking
// syscall calls this BEFORE it tests its condition.
void scheduler_wait_arm(const void *chan) {
    if (current_index < 0) return;
    procs[current_index].arm_chan   = chan;
    procs[current_index].armed_woken = 0;
    procs[current_index].arm_value  = 0;
}

// Withdraws the announcement -- for a caller that decided not to wait
// after all, so a later unrelated wake does not make its NEXT park a
// no-op.
void scheduler_wait_disarm(void) {
    if (current_index < 0) return;
    procs[current_index].arm_chan = 0;
    procs[current_index].armed_woken = 0;
}

int scheduler_sid(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->sid : 0;
}

// Is `pgid` a group inside session `sid`? The second half of POSIX's
// tcsetpgrp() rule: naming a group in somebody ELSE's session must not
// work even from inside the right session.
int scheduler_sid_has_pgid(int sid, int pgid) {
    if (sid < 1 || pgid < 1) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        if (procs[i].sid == sid && procs[i].pgid == pgid) return 1;
    }
    return 0;
}

// Starts a new session: the caller leads it, leads a new process group
// of its own, and has NO controlling terminal (the caller's terminal
// keeps its old session, so this process can no longer move its
// foreground group -- which is the point).
//
// **REFUSED FOR A PROCESS GROUP LEADER**, as POSIX requires: the new
// session's id would collide with the group it already leads, leaving
// one number meaning two things. A caller that needs it forks first.
int scheduler_setsid(int pid) {
    struct sched_process *p = live_slot(pid);
    if (!p) return -ESRCH;
    if (p->pgid == pid) return -EPERM;   // already a group leader
    p->sid  = pid;
    p->pgid = pid;
    return pid;
}

// SPAWN_SETSID's half: make a just-spawned process lead a new session
// and a group of its own. Separate from scheduler_setsid() because that
// one REFUSES a group leader (POSIX's rule about one number meaning two
// things), and a fresh child spawned with PGID_NEW already leads a
// group -- this is creation, not a transition, so the rule does not
// apply. Safe to call before the child has run: it is READY, and a
// syscall cannot be preempted.
void scheduler_make_session_leader(int pid) {
    struct sched_process *p = live_slot(pid);
    if (!p) return;
    p->sid  = pid;
    p->pgid = pid;

    // **A SESSION IS REMOTE WHEN THE PROCESS THAT CREATED IT WAS READING
    // A SOCKET**, and this is the one place a session is born -- so the
    // fact is derived here rather than declared by anybody. telnetd is
    // handed its connection on fd 0 and then spawns a shell with
    // SPAWN_SETSID; a GUI Terminal does the same spawn with a pty in
    // front of it and is not remote. Nothing in ring 3 is trusted to
    // say, and the peer travels with the session so every later record
    // carries it (kernel/include/kernel/remote_log.h).
    if (p->ppid > 0) {
        uint32_t ip = fd_peer_ip(scheduler_pid_pml4(p->ppid));
        if (ip) remote_log_session_opened(pid, ip);
    }
}

int scheduler_setpgid(int pid, int pgid) {
    struct sched_process *p = live_slot(pid);
    if (!p || pgid < 1) return 0;
    // EITHER lead a group named after yourself, OR join one that exists.
    // Without the second half a typo drops a process into a group
    // nothing will ever signal, which is indistinguishable from Ctrl-C
    // being broken.
    if (pgid != pid && !scheduler_pgid_live(pgid)) return 0;
    p->pgid = pgid;
    return 1;
}

int scheduler_signal_ignored(int pid, int sig) {
    struct sched_process *p = live_slot(pid);
    if (!p || !SIGNAL_VALID(sig)) return 0;
    return p->actions[sig].handler == SIG_IGN;
}

int scheduler_signal_action(int pid, int sig, struct k_sigaction *out) {
    struct sched_process *p = live_slot(pid);
    if (!p || !SIGNAL_VALID(sig)) return 0;
    if (out) *out = p->actions[sig];
    return 1;
}

int scheduler_signal_set_action(int pid, int sig, const struct k_sigaction *act,
                                struct k_sigaction *old) {
    struct sched_process *p = live_slot(pid);
    if (!p || !SIGNAL_VALID(sig)) return -1;
    if (old) *old = p->actions[sig];
    if (!act) return 0;

    p->actions[sig] = *act;
    // A SENTINEL CARRIES NO RESTORER AND NO FLAGS, and normalising here
    // rather than trusting the caller is what makes the readback
    // honest: SIG_DFL with a stale restorer left in the struct would be
    // reported back as something that looks armed and is not.
    if (!SIG_IS_HANDLER(act->handler)) {
        p->actions[sig].restorer = 0;
        p->actions[sig].flags    = 0;
    }

    // AND DROP WHAT IS ALREADY PENDING, for SIG_IGN only. A process that
    // has just said "I do not want this signal" must not be acted on by
    // one that arrived a moment earlier. Installing a HANDLER does not
    // drop it -- there the pending signal is precisely what the caller
    // has just arranged to hear about, and dropping it would lose a
    // signal that was legitimately sent.
    if (act->handler == SIG_IGN) p->pending &= ~(1u << sig);
    return 0;
}

uint32_t scheduler_signal_pending(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->pending : 0;
}

uint32_t scheduler_signal_blocked(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->blocked : 0;
}

void scheduler_signal_set_blocked(int pid, uint32_t mask) {
    struct sched_process *p = live_slot(pid);
    // SIGKILL AND SIGSTOP CAN NEVER BE BLOCKED, the same rule that stops
    // them being ignored -- and enforced HERE rather than at the two
    // callers, because sigreturn restores this mask from a struct on the
    // USER STACK and a program that scribbles its own frame must not be
    // able to make itself unkillable.
    if (p) p->blocked = mask & ~((1u << SIGKILL) | (1u << SIGSTOP));
}

void scheduler_sigsuspend_arm(int pid, uint32_t saved) {
    struct sched_process *p = live_slot(pid);
    if (!p) return;
    p->sigsuspend_saved = saved;
    p->sigsuspend_armed = 1;
}

int scheduler_sigsuspend_take(int pid, uint32_t *saved) {
    struct sched_process *p = live_slot(pid);
    if (!p || !p->sigsuspend_armed) return 0;
    // STILL PARKED MEANS THE WAIT IS NOT OVER. The trap that parked the
    // process runs its own tail afterwards, where "armed" and "current"
    // are both true and nothing has happened yet -- unwinding there put
    // the pre-suspend mask straight back and left the process asleep
    // under it forever, which reads as the signal never arriving.
    if (p->state == SCHED_BLOCKED) return 0;
    p->sigsuspend_armed = 0;
    // HANDS THE MASK BACK RATHER THAN INSTALLING IT, because the two
    // callers want it at different moments: a sigsuspend returning with
    // nothing to deliver wants it now, and one whose signal has a
    // handler wants the HANDLER to run under the suspend mask and the
    // restore to happen at the sigreturn. Installing it here would block
    // the very signal that ended the wait -- which is exactly what a
    // shell does, since it suspends with everything else held off.
    if (saved) *saved = p->sigsuspend_saved;
    return 1;
}

int scheduler_sigsuspend_armed(int pid) {
    struct sched_process *p = live_slot(pid);
    return p ? p->sigsuspend_armed : 0;
}

const void *scheduler_sigsuspend_chan(int pid) {
    struct sched_process *p = live_slot(pid);
    // A FIELD INSIDE THE SLOT, NOT THE SLOT ITSELF -- `&procs[idx]` is
    // already taken: scheduler_wait_chan_pid() returns it, and that is
    // the channel a child's exit wakes with SYS_RETRY. Parking here on
    // the slot address made a sigsuspend return -4095 the moment any
    // child died, which reads exactly like the syscall being wrong.
    return p ? (const void *)&p->sigsuspend_armed : 0;
}

int scheduler_signal_deliverable(int pid) {
    struct sched_process *p = live_slot(pid);
    if (!p) return 0;
    uint32_t ready = p->pending & ~p->blocked;
    if (!ready) return 0;
    for (int i = 1; i <= SIGNAL_MAX; i++)
        if (ready & (1u << i)) return i;
    return 0;
}

int scheduler_signal_take(int pid) {
    int sig = scheduler_signal_deliverable(pid);
    if (!sig) return 0;
    // ONE BIT, NOT THE WHOLE SET, and that changed with handlers. It
    // used to clear everything on the reasoning that acting on any
    // signal terminated the process, so the rest could never be acted
    // on -- true then, and false the moment a handler can run and
    // return. Each pending signal now gets its own delivery, the next
    // one at the sigreturn that ends this one.
    struct sched_process *p = live_slot(pid);
    p->pending &= ~(1u << sig);
    return sig;
}

// --- job control ------------------------------------------------------

static void signal_state_reset(int slot) {
    procs[slot].pending       = 0;
    procs[slot].blocked       = 0;
    procs[slot].sigsuspend_saved = 0;
    procs[slot].sigsuspend_armed = 0;
    for (int i = 0; i <= SIGNAL_MAX; i++)
        procs[slot].actions[i] = (struct k_sigaction){ 0, 0, 0, 0 };
    procs[slot].stopped       = 0;
    procs[slot].stop_reported = 0;
    procs[slot].stop_sig      = 0;
}

int scheduler_stop(int pid, int sig) {
    struct sched_process *p = live_slot(pid);
    if (!p) return 0;

    // ALREADY STOPPED IS A SUCCESS WITH NOTHING TO DO, and deliberately
    // does not re-arm the report: two Ctrl-Zs on one job are one
    // suspension, so a shell must not be told about it twice. Same
    // reasoning as the pending mask being a set rather than a queue.
    if (p->stopped) return 1;

    p->stopped       = 1;
    p->stop_sig      = sig;
    p->stop_reported = 0;

    // NOTHING IS DESCHEDULED HERE, and it does not need to be. If the
    // target is some other process, the picker already will not choose
    // it. If the target is the CURRENT process -- which is the common
    // case for Ctrl-Z, since the job being suspended is usually the one
    // running -- it keeps the CPU until the next timer tick and is then
    // never picked again. That is at most one tick of extra execution,
    // observable by nobody but the process itself, and it is what lets
    // this function be safe to call from the keyboard IRQ: it only
    // flips a byte, exactly the restraint scheduler_wake() keeps.
    //
    // A blocked process stays blocked. Its wake will still land and
    // still write its trapframe; the slot simply becomes
    // READY-and-stopped rather than runnable, so the syscall finishes
    // the moment somebody continues it.

    // The parent may be parked in SYS_WAITPID, and a suspension is
    // news it asked for if it passed SYS_WUNTRACED. Same channel and
    // same value an exit uses -- SYS_RETRY means "look again", and
    // looking again is exactly what finds the stop.
    scheduler_wake(scheduler_wait_chan_pid(p->ppid), SYS_RETRY);
    return 1;
}

int scheduler_continue(int pid) {
    struct sched_process *p = live_slot(pid);
    if (!p || !p->stopped) return 0;

    p->stopped  = 0;
    p->stop_sig = 0;
    // The report is dropped along with the stop it described: a
    // suspension nobody heard about before it ended is not something a
    // shell should be told about afterwards, because by then it is
    // false.
    p->stop_reported = 0;
    return 1;
}

int scheduler_stopped(int pid) {
    struct sched_process *p = live_slot(pid);
    return p && p->stopped;
}

int scheduler_stop_report(int pid) {
    struct sched_process *p = live_slot(pid);
    if (!p || !p->stopped || p->stop_reported) return 0;
    p->stop_reported = 1;
    return p->stop_sig;
}

int scheduler_stop_report_any(int parent_pid, int *out_pid) {
    if (parent_pid < 1) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (is_thread(i)) continue; // not a child -- see scheduler_poll_any()
        if (procs[i].ppid != parent_pid) continue;
        int sig = scheduler_stop_report(i + 1);
        if (sig) {
            if (out_pid) *out_pid = i + 1;
            return sig;
        }
    }
    return 0;
}

int scheduler_signal_raise(int pid, int sig) {
    struct sched_process *p = live_slot(pid);
    if (!p || !SIGNAL_VALID(sig)) return 0;

    // DROPPED, not queued -- POSIX's rule for SIG_IGN, and the thing
    // that makes `pending != 0` mean "must die" with no policy lookup.
    // Still a successful delivery: the caller asked for an action and
    // the action was "nothing".
    if (p->actions[sig].handler == SIG_IGN && !SIGNAL_UNIGNORABLE(sig)) return 1;

    p->pending |= (1u << sig);

    // A PARKED PROCESS HAS TO BE WOKEN TO BE ACTED ON, because delivery
    // only happens on the way back to ring 3 and a blocked process is
    // not on its way anywhere.
    //
    // **IT IS WOKEN BY REWINDING ITS SYSCALL, NOT BY FAILING IT**, and
    // this is the one thing handlers changed out here rather than in
    // signal.c. It used to write -EINTR into the saved RAX and let the
    // call return; that reaches a delivery point just as reliably, and
    // it puts the events in the wrong ORDER -- ring 3 sees the call fail
    // FIRST and runs the handler at some later trap, by which time
    // there is nothing left to restart and SA_RESTART cannot exist.
    //
    // Rewinding RIP over the two bytes of `int $0x80` means the process
    // re-enters the kernel at the same syscall with its arguments
    // untouched (RAX still holds the number -- nothing has written a
    // return value into this frame). idt.c's syscall-entry check sees
    // the pending signal there and delivers BEFORE the call runs, which
    // is the moment POSIX describes and the only moment at which
    // "restart it" and "fail it with -EINTR" are both still available.
    // Linux reaches the same place from the other end, with a
    // -ERESTARTSYS its blocking primitives return.
    //
    // The vector check is not paranoia: only a syscall can block, so a
    // frame that says otherwise is one this code does not understand,
    // and failing the call is the safe answer for it.
    //
    // Safe from an interrupt handler, and limited to make that true --
    // exactly the restraint scheduler_wake() keeps: this only flips
    // state and writes an already-saved trapframe, and never touches
    // g_next_kernel_rsp. Freeing the victim's address space here would
    // mean calling the heap from the keyboard IRQ.
    // A SIGSUSPEND SLEEPER IS WOKEN ONLY BY A SIGNAL ITS MASK LETS
    // THROUGH. Every other wait can take a spurious wake and simply
    // re-park; this one RETURNS on it, so waking it for a signal it
    // asked to block would hand the caller an -EINTR POSIX says it must
    // not see.
    if (p->state == SCHED_BLOCKED && p->wait_reason == SCHED_WAIT_SIGNAL &&
        (p->blocked & (1u << sig)))
        return 1;

    // **A CONTEXT PARKED MID-CALL IS NOT INTERRUPTIBLE, which is Linux's
    // TASK_UNINTERRUPTIBLE and for the same reason: the thing it is
    // waiting for is a device, not a person.** Neither answer below
    // applies to it -- rewinding RIP would re-issue a syscall that has
    // not finished, and -EINTR would be written into a trapframe its C
    // frames are still going to return through. The signal stays
    // pending and is delivered where every other one is, on the way
    // back to ring 3 once the call completes.
    if (p->state == SCHED_BLOCKED && p->parked_in_kernel) return 1;

    if (p->state == SCHED_BLOCKED) {
        uint64_t *tf = (uint64_t *)(uintptr_t)p->kernel_rsp;
        // **SIGSUSPEND IS THE ONE WAIT THAT MUST NOT BE REWOUND.** Its
        // whole contract is "return -EINTR once a signal arrives"; a
        // re-issue re-parks it with the same mask, and the caller never
        // reaches the line that reads what its handler set -- a shell's
        // wait loop hangs there and looks like a lost wakeup. Linux
        // spells the same exception ERESTARTNOHAND.
        if (tf[TF_VECTOR] == 0x80 && p->wait_reason != SCHED_WAIT_SIGNAL) {
            tf[TF_RIP] -= SYSCALL_INSN_LEN; p->syscall_reissue = 1;
        } else {
            tf[TF_RAX] = (uint64_t)(int64_t)-EINTR;
        }
        p->state = SCHED_READY;
        p->wait_chan = 0;
        p->wake_at_ns = 0;   // the wait is over; see scheduler_wake()
    }
    return 1;
}

int scheduler_kill(int pid, int exit_code) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    int slot = pid - 1;

    // A TID IS NOT SEPARATELY KILLABLE: the cleanup below destroys an
    // address space, and a thread's is its siblings'. Naming a thread
    // kills the process it belongs to, which is what kill(2) means and
    // what tkill(2) exists separately for.
    if (procs[slot].state != SCHED_UNUSED && is_thread(slot)) {
        slot = procs[slot].tgid - 1;
        pid  = slot + 1;
    }

    // Killing the CURRENT process would have to switch away and never
    // come back, which is scheduler_on_exit()'s job and reached through
    // SYS_EXIT. Refused rather than half-implemented: the caller here is
    // the window manager, which is never the process it is killing.
    if (slot == current_index) return 0;

    // Killing init would leave every orphan unreapable and nothing
    // supervising anything, so it is refused -- Linux's rule (SIGKILL
    // to pid 1 is discarded), for the same reason. Refused by ASKING
    // which pid init holds rather than testing `pid == 1`, so a boot
    // with no init leaves slot 0 an ordinary process.
    if (pid == g_init_pid) return 0;

    if (procs[slot].state != SCHED_READY && procs[slot].state != SCHED_BLOCKED)
        return 0; // unused, already a zombie, or running (handled above)

    // A STOPPED PROCESS IS STILL KILLABLE, which is the reason SIGKILL
    // is exempt from suspension everywhere: a job suspended by Ctrl-Z
    // must not be unkillable until somebody resumes it. The flag is
    // cleared here so the zombie it becomes does not report as stopped.
    procs[slot].stopped       = 0;
    procs[slot].stop_reported = 0;

    // Its threads go with it, for scheduler_on_exit()'s reason.
    group_release_threads(slot);

    procs[slot].state = SCHED_ZOMBIE;
    procs[slot].exit_code = exit_code;
    alive_count--;

    remote_log_session_closed(slot + 1);   // killed counts as ended

    // Exactly the teardown scheduler_on_exit() does, and for the same
    // reasons -- see its comments. A killed client's windows must come
    // off the screen now rather than at reap, or a dead process leaves a
    // window drawing stale pixels and answering no input.
    win_server_client_gone(pid);
    diag_provider_gone(pid);   // ...and any diagnostic name it held
    notify_parent(procs[slot].ppid);

    // The victim's memory goes NOW, not at reap. A zombie exists to
    // hold an exit code for whoever waits on it; holding an entire
    // address space as well is just a leak, and reaping never freed it
    // either -- scheduler_poll() only marks the slot unused. This is
    // the same split Linux makes (exit_mm() drops the mm at death, the
    // task_struct lingers), and without it every kill lost the victim's
    // ELF pages, stack, heap and window buffer for the rest of the
    // boot.
    //
    // AFTER win_server_client_gone() above, which unmaps this process's
    // window buffers from the compositor and clears the compositor role
    // if this was the desktop -- both of those reach into address
    // spaces and must happen while this one still exists.
    syscall_process_kill_cleanup(procs[slot].pml4_phys);
    procs[slot].pml4_phys = 0; // nothing may follow this pointer again
    reparent_children(pid);

    // No switch: the victim is not the process running, so the CPU is
    // already somewhere valid. If it was READY it simply never gets
    // picked again; if it was BLOCKED, find_next_runnable() skips
    // zombies exactly as it skipped it before.
    return 1;
}

enum sched_poll_result scheduler_poll(int pid, int *out_exit_code) {
    if (pid < 1 || pid > MAX_PROCS) return SCHED_POLL_INVALID;
    int slot = pid - 1;

    if (procs[slot].state == SCHED_ZOMBIE) {
        if (out_exit_code) *out_exit_code = procs[slot].exit_code;
        reap_audit(slot, "poll");
        procs[slot].state = SCHED_UNUSED; // reap -- see scheduler.h's doc comment
        return SCHED_POLL_EXITED;
    }
    // SCHED_BLOCKED counts as RUNNING: a process parked in a blocking
    // syscall is very much alive, and a poller (wm_run()'s per-frame
    // check) that saw anything else would conclude it had died and
    // release the window slot out from under it.
    if (procs[slot].state == SCHED_READY || procs[slot].state == SCHED_RUNNING ||
        procs[slot].state == SCHED_BLOCKED) {
        return SCHED_POLL_RUNNING;
    }
    return SCHED_POLL_INVALID; // SCHED_UNUSED -- bad pid, or already reaped
}

void scheduler_demo_run(void) {
    int a = spawn_from_fs("/bin/counter_a", NULL, 0, -1, -1, 0, 0, 0);
    int b = spawn_from_fs("/bin/counter_b", NULL, 0, -1, -1, 0, 0, 0);
    if (a < 0 || b < 0) {
        vga_write("schedtest: failed to spawn one or both counter processes --\n");
        vga_write("were /bin/counter_a and /bin/counter_b seeded onto disk.img?\n");
        vga_write("(see the Makefile's `seed` target)\n");
        if (a >= 0) { procs[a].state = SCHED_UNUSED; alive_count--; }
        if (b >= 0) { procs[b].state = SCHED_UNUSED; alive_count--; }
        return;
    }

    vga_write("Spawned two ring-3 counter processes (A and B). The scheduler\n");
    vga_write("is continuously armed (Milestone 1 phase 4b) -- the timer\n");
    vga_write("(100Hz) will now preemptively switch between them without\n");
    vga_write("either ever yielding voluntarily. Output below is interleaved\n");
    vga_write("DIRECTLY by each process's own write syscall, not narrated by\n");
    vga_write("the kernel:\n\n");
    klog_write("scheduler: demo spawned, waiting for both processes to exit\n");

    // Just a wait loop now, not an arm/disarm pair -- see this file's
    // top comment on why permanently-armed is safe. Both processes are
    // reaped here via scheduler_poll() rather than reaching into procs[]
    // directly, same public API a real caller (Terminal) uses -- this
    // demo is otherwise the one place still allowed to busy-wait
    // (blocking the physical shell for the demo's duration is the
    // whole point, see scheduler.h's doc comment).
    int a_code = 0, b_code = 0;
    int a_done = 0, b_done = 0;
    while (!a_done || !b_done) {
        __asm__ volatile ("hlt");
        if (!a_done && scheduler_poll(a + 1, &a_code) == SCHED_POLL_EXITED) a_done = 1;
        if (!b_done && scheduler_poll(b + 1, &b_code) == SCHED_POLL_EXITED) b_done = 1;
    }

    vga_write("\n\nBoth processes exited. Scheduler stays armed -- every other\n");
    vga_write("command behaves exactly as it did before M16 (see this file's\n");
    vga_write("top comment on why an empty process table makes that safe).\n");
    klog_write("scheduler: demo complete\n");
}

// See scheduler.h: the kernel's idle work, in one place instead of in
// whichever loop happened to be running. Adding a second thing here
// means it becomes live in every waiting loop at once -- which is the
// point, and also the risk, so the bar is the same one the rest of this
// repo holds: it must be safe wherever the kernel is idle.
void scheduler_idle(void) {
    debug_console_poll();
    // Input devices that have no interrupt of their own. Empty unless
    // something registered a poll() -- the PS/2 pair does not, being
    // IRQ-driven, so this costs a loop over two NULLs on a machine with
    // no other input hardware. See kernel/include/kernel/input.h.
    input_poll_sources();
    // Raw input to a ring-3 compositor. Silent unless one holds the role
    // -- see win_input.c. It lives
    // here because this is the kernel's one owner of idle work, so
    // every waiting loop feeds the desktop without knowing it does.
    win_input_poll();
    // Write back a quiet disk cache (ata_cache.h). This is the "and an
    // idle timer" half of the flush policy -- the dirty-line threshold
    // bounds how much can accumulate, this bounds how LONG it can sit
    // there, so a machine nobody is touching ends up with its writes on
    // the platter rather than waiting for the next barrier.
    atac_idle();
    // ...and the same half of the policy for a FILESYSTEM that is
    // holding a journal transaction open (storage.sync = batched). The
    // slot ceiling bounds how much accumulates; this bounds how long,
    // so a machine that wrote a file and was then left alone does not
    // hold that inode update indefinitely.
    fs_idle();
    // Received frames, and the protocols above them. The NIC's own
    // interrupt only queues a frame (kernel/drivers/net/net.c); this is
    // where ARP gets answered and an echo request becomes a reply, so a
    // machine with nothing to do still behaves like a host on the
    // network. Costs one compare when no card is registered.
    net_poll();
}

// --- the rewound-syscall window (see struct sched_proc.syscall_reissue) --

int scheduler_syscall_reissue_pending(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    return procs[pid - 1].syscall_reissue;
}

void scheduler_syscall_entered(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return;
    procs[pid - 1].syscall_reissue = 0;
}
