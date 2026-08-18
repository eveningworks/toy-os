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
#include "vmm.h"
#include "pmm.h"
#include "elf.h"
#include "elf_run.h"
#include "process.h" // process_context_is_armed() -- see kernel_slot_runnable()
#include "win_events.h" // win_events_reset() at spawn -- see scheduler_spawn()
#include "win_server.h"
#include "syscall.h" // syscall_process_kill_cleanup()
#include "win_input.h" // raw input to a ring-3 compositor // win_server_client_gone() -- see scheduler_on_exit()
#include "pipe.h"      // pipe_close_writer() when a piped child exits
#include "kstack.h"    // the guard page, canary and poison fill
#include "kfmt.h"      // klog_printf, vga_printf
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "fs.h"
#include "gdt.h"
#include "fpu.h"
#include "vga.h"
#include "klog.h"
#include "strace_internal.h"
#include "uaddr.h"
#include "clocksource.h" // CPU time is measured, not counted -- bill_current()
#include "debug_console.h"
#include "ata_cache.h" // the idle work scheduler_idle() owns
#include "string.h" // k_strlcpy -- proc_name_from_path()
#include <stddef.h>

// Defined in idt.c; isr.asm's isr_common epilogue reloads rsp from this
// immediately before popping registers and iretq'ing. isr_dispatch sets
// it to `regs` (no-op) at the top of every call; only this file ever
// overrides it to something else.
extern uint64_t g_next_kernel_rsp;

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
    // What this process is parked on while SCHED_BLOCKED (one of
    // scheduler.h's SCHED_WAIT_*), and the value its blocking syscall
    // will return once woken. Both meaningless in any other state.
    int wait_reason;
    uint64_t pml4_phys;
    uint64_t kernel_rsp; // this process's saved trapframe pointer --
                          // valid whenever state != SCHED_UNUSED
    int exit_code;        // valid only once state == SCHED_ZOMBIE
    // Pipe index this process's stdout goes to, or -1 for the console.
    // Lives here rather than in the fd table because fd 1 has always
    // been a hardcoded console in SYS_WRITE -- see scheduler.h.
    int stdout_pipe;

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
    // syscall_reset_heap(), which only elf_run.c's legacy blocking
    // loader calls -- so a SCHEDULER-spawned process, which is every GUI
    // app and everything `gui spawn` starts, had no heap armed and
    // SYS_SBRK returned -1 for it unconditionally. Nothing noticed
    // because nothing spawned had ever asked for memory. A ring-3
    // compositor asks for a whole screen of back buffer on its first
    // line (M41 stage 4b), which is how this surfaced.
    struct sched_heap heap;
    // This process's x87/SSE registers while it isn't the one running.
    // 16-byte aligned because FXSAVE/FXRSTOR #GP otherwise -- see fpu.h,
    // including why only ring-3 processes need one of these at all.
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
};

static struct sched_process procs[MAX_PROCS];
// When the CURRENT occupant of the CPU (a process, or the kernel
// context when current_index is -1) started running, in clocksource
// nanoseconds. bill_current() is the only thing that reads or moves it.
static uint64_t g_run_start_ns = 0;

static int current_index = -1;    // -1 = kernel/shell in control, not
                                    // a scheduler-managed process
static int scheduler_armed = 0;
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
static int find_next_runnable(int start) {
    for (int i = 1; i <= MAX_PROCS + 1; i++) {
        int idx = (start + i + MAX_PROCS + 1) % (MAX_PROCS + 1);
        if (idx == ROT_KERNEL) {
            if (kernel_slot_runnable()) return ROT_KERNEL;
            continue;
        }
        if (procs[idx].state == SCHED_READY) return idx;
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
static void switch_to(int idx) {
    kstack_verify(idx);   // before trusting anything else about this slot
    fpu_restore(procs[idx].fpu);
    g_next_kernel_rsp = procs[idx].kernel_rsp;
    vmm_switch_address_space(procs[idx].pml4_phys);
    gdt_set_kernel_stack(kernel_stack_top(idx));
    procs[idx].state = SCHED_RUNNING;
    current_index = idx;
    rotation_pos = idx;
}

// The ROT_KERNEL counterpart to switch_to(): hand the CPU back to the
// kernel context. No CR3 switch, no RSP0 repoint and no FP restore --
// see the ROT_KERNEL comment above for why the kernel needs none of the
// three. Kept as its own function purely so both callers (the tick and
// the exit path) state the same thing once.
static void switch_to_kernel(void) {
    current_index = -1;
    rotation_pos = ROT_KERNEL;
    g_next_kernel_rsp = kernel_saved_rsp;
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
// see docs/decisions.md). `args` is the same optional, space-separated
// argument string elf_run_from_fs() takes (NULL/"" for none -- both
// `schedtest` counters still pass NULL, unaffected by this parameter's
// addition) -- laid out via elf_build_argv_on_stack() (elf_run.h) into
// this process's own stack page, the same layout elf_run_from_fs() uses
// for a legacy-blocking process, so a scheduler-managed one gets a real
// argv[0]/argc too instead of the rdi=rsi=0/bare-top-of-page RSP this
// function used to synthesize unconditionally. Returns the slot index
// (>= 0) or -1 on any failure (no free slot, missing/unreadable file,
// `args` too long to fit the one stack page, or the same allocation
// failures every other ELF-loading path already handles the same way).
static int spawn_from_fs(const char *path, const char *args, int stdout_pipe) {
    int slot = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED) { slot = i; break; }
    }
    if (slot < 0) return -1;

    uint32_t size = 0;
    const char *data = fs_read(path, &size);
    if (!data) return -1;
    uint64_t elf_phys = (uint64_t)(uintptr_t)data; // no copy needed -- see elf_run.c's top comment

    uint64_t as = vmm_create_address_space();
    if (!as) return -1;

    // Same one-line hook elf_run_from_fs() has -- a no-op unless the
    // shell's `strace` armed tracing, which keeps the mechanism
    // process-creation-path-agnostic rather than tied to the blocking
    // loader (see kernel/proc/strace.c).
    strace_claim(as);

    uint64_t entry = 0;
    // On failure the address space is destroyed rather than leaked --
    // it owns whatever elf_load() mapped before giving up, and every
    // failure path below this point owes the same cleanup. This used to
    // be a bare `return -1`, leaking the PML4, every page table under
    // it and every segment frame.
    if (!elf_load(elf_phys, size, as, &entry)) {
        vmm_destroy_address_space(as);
        return -1;
    }

    // The TOP page is where argv is laid out and where RSP starts; the
    // rest are mapped below it so the stack has somewhere to grow.
    uint64_t stack_phys = 0;
    for (int pg = 0; pg < UADDR_STACK_PAGES; pg++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) { vmm_destroy_address_space(as); return -1; }
        uint64_t va = UADDR_STACK_VADDR - (uint64_t)pg * 4096;
        if (!vmm_map_user_page(as, va, frame)) {
            // The frame is not mapped, so destroying the address space
            // will not reclaim it -- free it here, then let the address
            // space take everything that IS mapped.
            pmm_free_frame(frame);
            vmm_destroy_address_space(as);
            return -1;
        }
        if (pg == 0) stack_phys = frame;
    }

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, UADDR_STACK_VADDR, path, args,
                                  &argc, &argv, &user_rsp)) {
        vmm_destroy_address_space(as);
        return -1;
    }

    // Synthesize this process's very first trapframe, at the top of its
    // own dedicated kernel stack -- laid out exactly like a real one
    // isr_common would have saved, so the ordinary epilogue can launch
    // it the first time exactly the same way it resumes it later.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0; // r15..rax start at 0
    // rdi/rsi stay 0: argc/argv reach the process on its STACK now, in
    // the SysV layout elf_build_argv_on_stack() built and
    // userland/crt0.asm reads (user_rsp below points at argc). They used
    // to be seeded here for a C _start that took them as parameters.
    (void)argc; (void)argv;
    tf[TF_VECTOR]  = 0; // unused -- epilogue discards vector+error_code
    tf[TF_ERRCODE] = 0; //          via `add rsp, 16` without reading them
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    tf[TF_RSP]     = user_rsp;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    kstack_arm_slot(slot);
    // A pristine FP state, not whatever the previous tenant of this
    // slot left behind -- slots get reused (scheduler_poll() reaps back
    // to SCHED_UNUSED), and inheriting the last process's registers
    // would be both wrong and an information leak between processes.
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_reason = 0;
    procs[slot].stdout_pipe = stdout_pipe;
    // Reset, not inherited: slots are reused, and a reaped process's
    // name and CPU time showing up on its successor would be a
    // reporting bug that looks like a scheduling one.
    proc_name_from_path(procs[slot].name, sizeof procs[slot].name, path);
    procs[slot].cpu_ns = 0;
    // Armed here, at creation, rather than by a separate "set up this
    // process's heap" call the way the legacy loader does it: an init
    // step reachable by only one entry point is a bug waiting for a
    // second entry point, and this one already had that bug -- nothing
    // in the spawn path ever armed a heap, so SYS_SBRK refused every
    // scheduled process.
    procs[slot].heap.brk = UADDR_HEAP_BASE;
    procs[slot].heap.mapped_end = UADDR_HEAP_BASE;
    procs[slot].state      = SCHED_READY;
    alive_count++;
    return slot;
}

int scheduler_proc_info(int index, struct proc_info *out) {
    if (!out || index < 0 || index >= MAX_PROCS) return 0;

    struct sched_process *p = &procs[index];

    out->pid = 0;
    out->state = PROC_STATE_UNUSED;
    out->cpu_ns = 0;
    out->mem_bytes = 0;
    out->exit_code = 0;
    out->reserved = 0;
    out->name[0] = '\0';

    if (p->state == SCHED_UNUSED) return 1; // a real answer: slot empty

    // pid is slot + 1 throughout this file -- 0 is "no process".
    out->pid = index + 1;
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
        case SCHED_BLOCKED: out->state = PROC_STATE_BLOCKED; break;
        case SCHED_ZOMBIE:  out->state = PROC_STATE_ZOMBIE;  break;
        default:            out->state = PROC_STATE_UNUSED;  break;
    }
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
static void bill_current(void) {
    uint64_t now = clocksource_now_ns();
    if (current_index >= 0 && now > g_run_start_ns) {
        procs[current_index].cpu_ns += now - g_run_start_ns;
    }
    // Reset unconditionally, including when the KERNEL context was
    // running: its time belongs to nobody, and leaving the old start
    // in place would hand the next process everything the kernel just
    // spent.
    g_run_start_ns = now;
}

// The rotation, shared by the 100Hz timer and by SYS_YIELD. They are
// the same operation now that neither one is where billing happens --
// see bill_current() above.
static void scheduler_rotate(uint64_t *regs);

void scheduler_tick(uint64_t *regs) { scheduler_rotate(regs); }

// SYS_YIELD's entry into the same rotation. Distinct from the tick only
// so the call sites read honestly; the accounting difference that used
// to justify two paths is gone.
void scheduler_yield(uint64_t *regs) { scheduler_rotate(regs); }

// Depth, not a flag: sections nest, and an inner one must not re-enable
// preemption an outer one is relying on. See api/scheduler.h.
static int g_preempt_depth;

void scheduler_preempt_disable(void) { g_preempt_depth++; }

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
        if (current_index < 0) kernel_saved_rsp = (uint64_t)regs;
        return;
    }

    // A CRITICAL SECTION IS OPEN -- resume exactly what was interrupted,
    // the same treatment (and for the same reason) as the armed legacy
    // process above: switching away would let another caller re-enter
    // code that is holding shared state. See scheduler_preempt_disable()
    // in api/scheduler.h for what holds this and why. The slice is
    // already billed above, so accounting is unaffected.
    if (g_preempt_depth > 0) {
        if (current_index < 0) kernel_saved_rsp = (uint64_t)regs;
        return;
    }

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
    } else {
        kernel_saved_rsp = (uint64_t)regs;
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
        switch_to_kernel();
        return;
    }

    switch_to(next);
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
// merely slow. syscall.c's SYS_READ_KEY comment has the full autopsy:
// it worked for exactly one keystroke and then hung, because
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
int scheduler_block_current(uint64_t *regs, int reason) {
    if (current_index < 0) return 0;

    bill_current(); // this slice ends here -- see bill_current()
    int idx = current_index;
    procs[idx].kernel_rsp = (uint64_t)regs;
    kstack_verify(idx);
    fpu_save(procs[idx].fpu);
    procs[idx].state = SCHED_BLOCKED;
    procs[idx].wait_reason = reason;
    current_index = -1;

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel();
    else switch_to(next);
    return 1;
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
int scheduler_wake(int reason, int64_t value) {
    int woken = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED) continue;
        if (procs[i].wait_reason != reason) continue;

        // The saved trapframe's RAX slot IS the syscall's return value:
        // isr_common's epilogue pops it straight into the register the
        // ring-3 caller reads. Writing it here is what makes waking a
        // process and answering its syscall the same act.
        uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
        tf[TF_RAX] = (uint64_t)value;
        procs[i].state = SCHED_READY;
        woken++;
    }
    return woken;
}

void scheduler_on_exit(int code) {
    if (current_index < 0) return; // defensive; shouldn't happen

    // SCHED_ZOMBIE, not SCHED_UNUSED -- see this file's comment on that
    // enum value. The slot (and its exit_code) stays held until whoever
    // spawned it calls scheduler_poll().
    procs[current_index].state = SCHED_ZOMBIE;
    procs[current_index].exit_code = code;
    alive_count--;

    // Tell the window server to drop anything this client still owned.
    // Here rather than at reap: a zombie's windows must come off the
    // screen the moment it dies, not whenever someone gets round to
    // polling it -- otherwise a crashed client leaves a window that
    // draws stale pixels and answers no input. A no-op when no server
    // is registered, which is every non-GUI boot.
    win_server_client_gone(current_index + 1);

    // A parent blocked in SYS_WAITPID has to hear about this. Waking
    // every child-waiter rather than only this one's parent is the
    // same "name the event, not the waiter" rule the wait reasons
    // follow -- each woken parent re-checks its own child and parks
    // again if it was somebody else's that exited.
    scheduler_wake(SCHED_WAIT_CHILD, SYS_RETRY);

    // Closing the write end is what turns the parent's blocking read
    // into EOF rather than an indefinite wait. Done here, at exit,
    // because a process that dies without closing its own stdout is
    // the normal case, not an error.
    if (procs[current_index].stdout_pipe >= 0) {
        pipe_close_writer(procs[current_index].stdout_pipe);
        procs[current_index].stdout_pipe = -1;
    }

    bill_current(); // the exiting process's last slice
    current_index = -1;

    // Continue the rotation from the slot that just exited (which is
    // still what rotation_pos holds), rather than restarting at slot 0
    // -- same fairness the tick above gets, and it means the kernel's
    // position is reached normally instead of being skipped on an exit.
    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) {
        switch_to_kernel();
        return;
    }
    switch_to(next);
}

int scheduler_current_pid(void) {
    return current_index < 0 ? 0 : current_index + 1;
}

struct sched_heap *scheduler_current_heap(void) {
    // NULL means "the kernel context is running", which for SYS_SBRK is
    // the legacy elf_run.c process -- not "this process has no heap".
    // Every slot gets one at creation.
    if (current_index < 0) return 0;
    return &procs[current_index].heap;
}

int scheduler_spawn(const char *path, const char *args) {
    return scheduler_spawn_piped(path, args, -1);
}

int scheduler_spawn_piped(const char *path, const char *args, int pipe_idx) {
    int slot = spawn_from_fs(path, args, pipe_idx);
    if (slot < 0) return 0;

    // Clear any events left over from the previous tenant of this slot.
    // Doing it at spawn rather than at reap is what makes this the only
    // lifecycle call the scheduler owes the windowing layer: a recycled
    // pid can't inherit stale events if the queue is emptied before the
    // new process can ever look at it.
    win_events_reset(slot + 1);
    return slot + 1; // 1-based pid (see scheduler.h)
}

int scheduler_stdout_pipe(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return -1;
    if (procs[pid - 1].state == SCHED_UNUSED) return -1;
    return procs[pid - 1].stdout_pipe;
}

int scheduler_pid_valid(int pid) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    return procs[pid - 1].state != SCHED_UNUSED;
}

int scheduler_kill(int pid, int exit_code) {
    if (pid < 1 || pid > MAX_PROCS) return 0;
    int slot = pid - 1;

    // Killing the CURRENT process would have to switch away and never
    // come back, which is scheduler_on_exit()'s job and reached through
    // SYS_EXIT. Refused rather than half-implemented: the caller here is
    // the window manager, which is never the process it is killing.
    if (slot == current_index) return 0;

    if (procs[slot].state != SCHED_READY && procs[slot].state != SCHED_BLOCKED)
        return 0; // unused, already a zombie, or running (handled above)

    procs[slot].state = SCHED_ZOMBIE;
    procs[slot].exit_code = exit_code;
    alive_count--;

    // Exactly the teardown scheduler_on_exit() does, and for the same
    // reasons -- see its comments. A killed client's windows must come
    // off the screen now rather than at reap, or a dead process leaves a
    // window drawing stale pixels and answering no input.
    win_server_client_gone(pid);
    scheduler_wake(SCHED_WAIT_CHILD, SYS_RETRY);
    if (procs[slot].stdout_pipe >= 0) {
        pipe_close_writer(procs[slot].stdout_pipe);
        procs[slot].stdout_pipe = -1;
    }

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
    int a = spawn_from_fs("/bin/counter_a", NULL, -1);
    int b = spawn_from_fs("/bin/counter_b", NULL, -1);
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
    // Raw input to a ring-3 compositor. Silent unless one is registered
    // AND no ring-0 presentation layer is -- see win_input.c. It lives
    // here because this is the kernel's one owner of idle work, so
    // every waiting loop feeds the desktop without knowing it does.
    win_input_poll();
    // Write back a quiet disk cache (ata_cache.h). This is the "and an
    // idle timer" half of the flush policy -- the dirty-line threshold
    // bounds how much can accumulate, this bounds how LONG it can sit
    // there, so a machine nobody is touching ends up with its writes on
    // the platter rather than waiting for the next barrier.
    atac_idle();
}
