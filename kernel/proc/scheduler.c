// Preemptive round-robin scheduler for ring-3 processes -- built as the
// ORIGINAL Milestone 16 (the old numbering in CHANGELOG-archive.md, not
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
#include "win_server.h" // win_server_client_gone() -- see scheduler_on_exit()
#include "pipe.h"      // pipe_close_writer() when a piped child exits
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "fs.h"
#include "gdt.h"
#include "fpu.h"
#include "vga.h"
#include "klog.h"
#include "strace_internal.h"
#include "uaddr.h"
#include <stddef.h>

// Defined in idt.c; isr.asm's isr_common epilogue reloads rsp from this
// immediately before popping registers and iretq'ing. isr_dispatch sets
// it to `regs` (no-op) at the top of every call; only this file ever
// overrides it to something else.
extern uint64_t g_next_kernel_rsp;

#define MAX_PROCS        4
#define PROC_KSTACK_SIZE 8192
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
    // This process's x87/SSE registers while it isn't the one running.
    // 16-byte aligned because FXSAVE/FXRSTOR #GP otherwise -- see fpu.h,
    // including why only ring-3 processes need one of these at all.
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(FPU_STATE_ALIGN)));
    uint8_t kstack[PROC_KSTACK_SIZE] __attribute__((aligned(16)));
};

static struct sched_process procs[MAX_PROCS];
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
// handler (see apps/wm/wm.c's per-frame poll comment), not because the
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
    return (uint64_t)&procs[idx].kstack[PROC_KSTACK_SIZE];
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
    if (!elf_load(elf_phys, as, &entry)) return -1;

    // The TOP page is where argv is laid out and where RSP starts; the
    // rest are mapped below it so the stack has somewhere to grow.
    uint64_t stack_phys = 0;
    for (int pg = 0; pg < UADDR_STACK_PAGES; pg++) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        uint64_t va = UADDR_STACK_VADDR - (uint64_t)pg * 4096;
        if (!vmm_map_user_page(as, va, frame)) return -1;
        if (pg == 0) stack_phys = frame;
    }

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, UADDR_STACK_VADDR, path, args,
                                  &argc, &argv, &user_rsp)) {
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
    // A pristine FP state, not whatever the previous tenant of this
    // slot left behind -- slots get reused (scheduler_poll() reaps back
    // to SCHED_UNUSED), and inheriting the last process's registers
    // would be both wrong and an information leak between processes.
    fpu_init_state(procs[slot].fpu);
    procs[slot].wait_reason = 0;
    procs[slot].stdout_pipe = stdout_pipe;
    procs[slot].state      = SCHED_READY;
    alive_count++;
    return slot;
}

void scheduler_tick(uint64_t *regs) {
    if (!scheduler_armed) return;

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

    if (current_index >= 0) {
        procs[current_index].kernel_rsp = (uint64_t)regs;
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

    int idx = current_index;
    procs[idx].kernel_rsp = (uint64_t)regs;
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
