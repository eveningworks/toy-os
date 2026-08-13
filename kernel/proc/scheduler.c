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
#include "fs.h"
#include "gdt.h"
#include "vga.h"
#include "klog.h"
#include "strace_internal.h"
#include <stddef.h>

// Defined in idt.c; isr.asm's isr_common epilogue reloads rsp from this
// immediately before popping registers and iretq'ing. isr_dispatch sets
// it to `regs` (no-op) at the top of every call; only this file ever
// overrides it to something else.
extern uint64_t g_next_kernel_rsp;

#define MAX_PROCS        4
#define PROC_KSTACK_SIZE 8192
#define PROC_USTACK_VADDR 0x8000200000ULL // same fixed vaddr in every
                                            // process's OWN address
                                            // space -- no collision,
                                            // since each is private.

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
enum sched_state { SCHED_UNUSED = 0, SCHED_READY, SCHED_RUNNING, SCHED_ZOMBIE };

struct sched_process {
    enum sched_state state;
    uint64_t pml4_phys;
    uint64_t kernel_rsp; // this process's saved trapframe pointer --
                          // valid whenever state != SCHED_UNUSED
    int exit_code;        // valid only once state == SCHED_ZOMBIE
    uint8_t kstack[PROC_KSTACK_SIZE] __attribute__((aligned(16)));
};

static struct sched_process procs[MAX_PROCS];
static int current_index = -1;    // -1 = kernel/shell in control, not
                                    // a scheduler-managed process
static int scheduler_armed = 0;
static uint64_t kernel_saved_rsp = 0; // refreshed every tick that finds
                                        // current_index == -1
static volatile int alive_count = 0;

static uint64_t kernel_stack_top(int idx) {
    return (uint64_t)&procs[idx].kstack[PROC_KSTACK_SIZE];
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_PROCS; i++) procs[i].state = SCHED_UNUSED;
    current_index = -1;
    // Permanently armed from here on -- see this file's top comment on
    // why that's safe with an empty process table. Was `= 0` (disarmed,
    // only scheduler_demo_run() ever flipped it) before Milestone 1
    // phase 4b generalized this from a one-off demo to a real,
    // continuously-available spawn mechanism.
    scheduler_armed = 1;
    kernel_saved_rsp = 0;
    alive_count = 0;
}

// Scans all MAX_PROCS slots starting just after `start` (wrapping),
// returns the first READY one, or -1 if none. `start` being -1 (no
// current process) is handled correctly by the modular arithmetic --
// it just starts the scan at slot 0.
static int find_next_ready(int start) {
    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (start + i + MAX_PROCS) % MAX_PROCS;
        if (procs[idx].state == SCHED_READY) return idx;
    }
    return -1;
}

static void switch_to(int idx) {
    g_next_kernel_rsp = procs[idx].kernel_rsp;
    vmm_switch_address_space(procs[idx].pml4_phys);
    gdt_set_kernel_stack(kernel_stack_top(idx));
    procs[idx].state = SCHED_RUNNING;
    current_index = idx;
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
static int spawn_from_fs(const char *path, const char *args) {
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

    uint64_t stack_phys = pmm_alloc_frame();
    if (!stack_phys) return -1;
    if (!vmm_map_user_page(as, PROC_USTACK_VADDR, stack_phys)) return -1;

    uint64_t argc = 0, argv = 0, user_rsp = 0;
    if (!elf_build_argv_on_stack(stack_phys, PROC_USTACK_VADDR, path, args,
                                  &argc, &argv, &user_rsp)) {
        return -1;
    }

    // Synthesize this process's very first trapframe, at the top of its
    // own dedicated kernel stack -- laid out exactly like a real one
    // isr_common would have saved, so the ordinary epilogue can launch
    // it the first time exactly the same way it resumes it later.
    uint64_t *tf = (uint64_t *)(kernel_stack_top(slot) - TRAPFRAME_WORDS * 8);
    for (int i = 0; i < TF_VECTOR; i++) tf[i] = 0; // r15..rax start at 0
    tf[TF_RDI]     = argc; // argc/argv -- same ABI process_run_ring3_args() uses
    tf[TF_RSI]     = argv;
    tf[TF_VECTOR]  = 0; // unused -- epilogue discards vector+error_code
    tf[TF_ERRCODE] = 0; //          via `add rsp, 16` without reading them
    tf[TF_RIP]     = entry;
    tf[TF_CS]      = SEL_USER_CODE;
    tf[TF_RFLAGS]  = 0x200; // IF set
    tf[TF_RSP]     = user_rsp;
    tf[TF_SS]      = SEL_USER_DATA;

    procs[slot].pml4_phys  = as;
    procs[slot].kernel_rsp = (uint64_t)tf;
    procs[slot].state      = SCHED_READY;
    alive_count++;
    return slot;
}

void scheduler_tick(uint64_t *regs) {
    if (!scheduler_armed) return;

    if (current_index >= 0) {
        procs[current_index].kernel_rsp = (uint64_t)regs;
        procs[current_index].state = SCHED_READY;
    } else {
        kernel_saved_rsp = (uint64_t)regs;
    }

    int next = find_next_ready(current_index);
    if (next < 0) {
        // Nothing ready -- resume the kernel/shell context. In the
        // overwhelmingly common case (scheduler disarmed, or armed but
        // nothing has been spawned yet) kernel_saved_rsp already equals
        // `regs`, so this is a genuine no-op.
        current_index = -1;
        g_next_kernel_rsp = kernel_saved_rsp;
        return;
    }

    switch_to(next);
}

void scheduler_on_exit(int code) {
    if (current_index < 0) return; // defensive; shouldn't happen

    // SCHED_ZOMBIE, not SCHED_UNUSED -- see this file's comment on that
    // enum value. The slot (and its exit_code) stays held until whoever
    // spawned it calls scheduler_poll().
    procs[current_index].state = SCHED_ZOMBIE;
    procs[current_index].exit_code = code;
    alive_count--;
    current_index = -1;

    int next = find_next_ready(-1);
    if (next < 0) {
        g_next_kernel_rsp = kernel_saved_rsp;
        return;
    }
    switch_to(next);
}

int scheduler_current_pid(void) {
    return current_index < 0 ? 0 : current_index + 1;
}

int scheduler_spawn(const char *path, const char *args) {
    int slot = spawn_from_fs(path, args);
    return slot < 0 ? 0 : slot + 1; // 0 = failure, else 1-based pid (see scheduler.h)
}

enum sched_poll_result scheduler_poll(int pid, int *out_exit_code) {
    if (pid < 1 || pid > MAX_PROCS) return SCHED_POLL_INVALID;
    int slot = pid - 1;

    if (procs[slot].state == SCHED_ZOMBIE) {
        if (out_exit_code) *out_exit_code = procs[slot].exit_code;
        procs[slot].state = SCHED_UNUSED; // reap -- see scheduler.h's doc comment
        return SCHED_POLL_EXITED;
    }
    if (procs[slot].state == SCHED_READY || procs[slot].state == SCHED_RUNNING) {
        return SCHED_POLL_RUNNING;
    }
    return SCHED_POLL_INVALID; // SCHED_UNUSED -- bad pid, or already reaped
}

void scheduler_demo_run(void) {
    int a = spawn_from_fs("/bin/counter_a", NULL);
    int b = spawn_from_fs("/bin/counter_b", NULL);
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
