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
#include "sched_internal.h"
#include "vmm.h"
#include "process.h" // process_context_is_armed() -- see kernel_slot_runnable()
#include "win_input.h" // raw input to a ring-3 compositor // win_server_client_gone() -- see scheduler_on_exit()
#include "netdev.h"    // net_poll() -- the idle half of the receive path
#include "kfmt.h"      // klog_printf, vga_printf
#include "idt.h"     // isr_depth_get()/_set() -- the depth travels with kernel_rsp
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "gdt.h"
#include "tls.h"    // FS.base -- a thread pointer is per THREAD, see switch_to()
#include "vga.h"
#include "klog.h"
#include "clocksource.h" // CPU time is measured, not counted -- bill_current()
#include "clockevent.h"  // clockevent_reprogram() on every switch, clockevent_in_idle()
#include "random_hw.h"   // arch_rdtsc() -- off-CPU time, see context_load_globals()
#include "debug_console.h"
#include "ata.h"       // ata_idle() -- the idle work scheduler_idle() owns
#include "input.h"     // input_poll_sources() -- ditto, for a device with no IRQ
#include "string.h" // k_strlcpy -- proc_name_from_path()
#include "pmm.h"    // the table and the stack chunks

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

// "/bin/wm/demos/uidemo" -> "uidemo". A task manager column is a few
// characters wide, so the last component is the useful part and the
// path is not kept at all (see abi/proc_info.h).
void proc_name_from_path(char *dst, int cap, const char *path) {
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

// scheduler.h deliberately does not include fs.h, so struct sched_cwd
// spells its size as a literal. This is what stops the two drifting.
_Static_assert(sizeof(((struct sched_cwd *)0)->path) == FS_PATH_MAX,
               "struct sched_cwd::path must match FS_PATH_MAX");
_Static_assert(sizeof(((struct mmap_region *)0)->path) == FS_PATH_STORED_MAX,
               "mmap_region.path is a STORED path (fs.h): 32 per process, so it "
               "takes the smaller bound and mmap REFUSES anything longer");

// THE TABLE, sized from RAM in scheduler_init(): g_max_procs slots, of
// which g_slots_ready have a kernel stack, and g_slot_end bounds every scan
// (sched_internal.h, MAX_PROCS). NULL and 0 before init, so an early scan
// is an empty loop.
struct sched_process *procs;
int g_max_procs;
int g_slot_end;
int g_slots_ready;
static int g_guards_on;   // scheduler_guard_pages_init() has run

// A SLOT BEING BUILT IS CLAIMED, and only the allocators look. A spawn
// picks a slot and then reads the ELF from disk, which SLEEPS -- and the
// slot stayed SCHED_UNUSED meanwhile, so a second spawn in that window
// took the same one. Both filled it; the first child never existed and
// its descriptor table, holding its parent's pipe end, was never
// released, so the parent read forever (argv_test in block(pipe), about
// 1 suite run in 5). Linux's TASK_NEW is the same idea as a state; a
// flag only the allocators consult keeps every other scan of the table
// seeing "not a process" without auditing each one (procs[i].claimed).

// pid -> slot + 1, or 0. Kept by slot_claim() and slot_free() alone.
static uint16_t g_pid_slot[SCHED_PID_MAX];
static int g_last_pid;

// Still in use as a GROUP or a SESSION by a live or zombie slot: Linux
// keeps a `struct pid` alive for that, and handing the number out would
// put a new process into an old group.
static int pid_named(int pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != SCHED_UNUSED && (procs[i].pgid == pid || procs[i].sid == pid))
            return 1;
    return 0;
}

// The next free pid after the last one handed out, or 0. Interrupts off.
static int pid_alloc(void) {
    int pid = g_last_pid;
    for (int n = 0; n < SCHED_PID_MAX; n++) {
        if (++pid >= SCHED_PID_MAX) pid = SCHED_PID_RESERVED;
        if (g_pid_slot[pid] || pid_named(pid)) continue;
        g_last_pid = pid;
        return pid;
    }
    return 0;
}

// The next SCHED_SLOT_CHUNK slots' kernel stacks and ext parts, or 0.
// Under the preemption guard rather than with interrupts off: the frame
// allocator and the page split sleep on nothing, and a kernel-context
// spawner cannot park. g_slots_ready moves LAST, so no scan sees a slot
// without its stack.
static int slot_grow(void) {
    if (!procs || g_slots_ready >= g_max_procs) return 0;
    int n = g_max_procs - g_slots_ready;
    if (n > SCHED_SLOT_CHUNK) n = SCHED_SLOT_CHUNK;
    uint64_t kpages = ((uint64_t)n * sizeof(struct kstack) + 4095) / 4096;
    uint64_t epages = ((uint64_t)n * sizeof(struct sched_slot_ext) + 4095) / 4096;
    uint64_t kphys = pmm_alloc_contiguous(kpages, PMM_ZONE_ANY);
    if (!kphys) return 0;
    uint64_t ephys = pmm_alloc_contiguous(epages, PMM_ZONE_ANY);
    if (!ephys) { pmm_free_contiguous(kphys, kpages); return 0; }
    struct kstack *ks = (struct kstack *)(uintptr_t)kphys;   // identity-mapped
    struct sched_slot_ext *ex = (struct sched_slot_ext *)(uintptr_t)ephys;
    k_memset(ex, 0, epages * 4096);
    int base = g_slots_ready;
    for (int i = 0; i < n; i++) {
        procs[base + i].kstack = &ks[i];
        procs[base + i].ext = &ex[i];
        if (g_guards_on) kstack_guard_arm(&ks[i]);
    }
    g_slots_ready = base + n;
    return 1;
}

int slot_claim(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t f;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
        int slot = -1;
        for (int i = 0; i < g_slots_ready; i++) {
            if (procs[i].state == SCHED_UNUSED && !procs[i].claimed) {
                int pid = pid_alloc();   // the one place a pid is chosen
                if (!pid) break;
                procs[i].claimed = 1;
                procs[i].pid = pid;
                g_pid_slot[pid] = (uint16_t)(i + 1);
                if (i >= g_slot_end) g_slot_end = i + 1;   // before anyone can see it
                slot = i;
                break;
            }
        }
        __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
        if (slot >= 0) return slot;
        if (attempt) return -1;
        scheduler_preempt_disable();
        int grew = slot_grow();
        scheduler_preempt_enable();
        if (!grew) return -1;
    }
    return -1;
}

// Published (its state is no longer UNUSED) or abandoned: either way the
// claim is over.
void slot_unclaim(int slot) {
    if (slot < 0 || slot >= g_slots_ready) return;
    if (procs[slot].state == SCHED_UNUSED) slot_free(slot);   // abandoned
    procs[slot].claimed = 0;
}

// THE ONE WAY A SLOT BECOMES FREE: its pid goes with it, so no lookup can
// find a reused slot under the pid of what was there before.
void slot_free(int slot) {
    int pid = procs[slot].pid;
    if (pid > 0 && pid < SCHED_PID_MAX && g_pid_slot[pid] == slot + 1) g_pid_slot[pid] = 0;
    procs[slot].state = SCHED_UNUSED;
    procs[slot].pid = 0;
}

void scheduler_test_set_last_pid(int pid) { g_last_pid = pid; }

// The pid init holds, or 0 on a boot that has no init (nothing spawned
// it, or /bin/init is missing). Everything that treats pid 1 specially
// asks this rather than testing `pid == 1`, so a boot without an init
// behaves exactly as this kernel did before one existed -- rather than
// adopting orphans to a pid nobody is running and refusing to kill
// whatever happens to be in slot 0.
int g_init_pid = 0;

int scheduler_init_pid(void) { return g_init_pid; }
void scheduler_set_init_pid(int pid) { g_init_pid = pid; }

// When the CURRENT occupant of the CPU (a process, or the kernel
// context when current_index is -1) started running, in clocksource
// nanoseconds. bill_current() is the only thing that reads or moves it.
static uint64_t g_run_start_ns = 0;

int current_index = -1;    // -1 = kernel/shell in control, not
                                    // a scheduler-managed process
static int scheduler_armed = 0;
static int kernel_saved_isr_depth = 0; // the kernel slot's isr_depth_get()
static int kernel_preempt_depth = 0;   // and its scheduler_preempt_depth()
static uint64_t kernel_offcpu_tsc, kernel_left_tsc; // and its off-CPU time
static struct kernel_context kernel_kctx; // and where the kernel context
                                          // itself is parked -- rip 0
                                          // until it has been left once
const struct kernel_context *scheduler_kernel_kctx(void) { return &kernel_kctx; }
static uint64_t kernel_saved_rsp = 0; // refreshed every tick that finds
                                        // current_index == -1
volatile int alive_count = 0;

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

// Where the rotation last stopped: 0..MAX_PROCS-1 for a process slot,
// ROT_KERNEL for the kernel context. Deliberately separate from
// current_index, which still means exactly what it always did (-1
// whenever a scheduler-managed process is NOT the thing running) --
// syscall.c depends on that through scheduler_current_pid(), and
// conflating the two would change every M8-M15 exit path.
int rotation_pos = ROT_KERNEL;

// --- thread groups ---------------------------------------------------
//
// A thread is a slot whose leader is somebody else. Everything a
// PROCESS owns is read through the leader's slot, so there is one copy
// of it however many threads share it -- which is the difference
// between this and two processes that happen to share a page table.

int is_thread(int idx) {
    return procs[idx].tgid != procs[idx].pid;
}

int pid_slot(int pid) {
    if (pid < 1 || pid >= SCHED_PID_MAX) return -1;
    int s = (int)g_pid_slot[pid] - 1;
    // Checked against the slot: a lookup from an interrupt or the stopped
    // debugger may race a claim, and must miss rather than misname.
    return s >= 0 && procs[s].pid == pid ? s : -1;
}

// The slot holding what this group shares. Falls back to `idx` for a
// group whose leader is already gone -- unreachable while a thread runs
// (the group dies as a unit) and it keeps every caller here total.
int leader_index(int idx) {
    int lead = pid_slot(procs[idx].tgid);
    return lead < 0 ? idx : lead;
}

uint64_t kernel_stack_top(int idx) {
    return kstack_top(procs[idx].kstack);
}

uint64_t kernel_stack_base(int idx) {
    return kstack_base(procs[idx].kstack);
}

// Called wherever a stack starts a new life. The trapframe the spawn
// path has just written at the top is what `reserve_top` protects.
void kstack_arm_slot(int idx) {
    kstack_arm(procs[idx].kstack, TRAPFRAME_WORDS * 8, &procs[idx].kstack_peak);
}

// The canary check. Deliberately fatal rather than a log line: the
// canary being gone means something has already written outside its
// stack, so every piece of state this scheduler is about to act on is
// suspect -- and carrying on is exactly how the original bug presented,
// as a fault in an innocent process several context switches later.
static void kstack_verify(int idx) {
    if (kstack_canary_ok(procs[idx].kstack)) return;
    klog_printf("KERNEL STACK OVERFLOW: slot %d (pid %d, \"%s\") overran its "
                "%d-byte stack -- canary at %lx destroyed\n",
                idx, procs[idx].pid, procs[idx].name, PROC_KSTACK_SIZE,
                kernel_stack_base(idx));
    vga_printf("KERNEL STACK OVERFLOW: pid %d (\"%s\") overran its kernel stack\n",
               procs[idx].pid, procs[idx].name);
    // There is no panic() to call -- the panic machinery lives in the
    // fault handler, and going through it is what buys the function
    // name, the registers and the stack scan. `ud2` is the cheapest way
    // in, and the line above says what it really was.
    __asm__ volatile ("ud2");
}

uint64_t scheduler_kstack_base(int idx) {
    if (idx < 0 || idx >= g_slots_ready) return 0;
    return kernel_stack_base(idx);
}

int scheduler_slot_end(void) { return g_slot_end; }

// Unmaps the guard page below every kernel stack. Called from
// kernel_main() AFTER paging_enforce_wx(), which rewrites every PDE and
// would otherwise put the huge page back.
void scheduler_guard_pages_init(void) {
    int ok = 0;
    for (int i = 0; i < g_slots_ready; i++) {
        if (kstack_guard_arm(procs[i].kstack)) ok++;
    }
    g_guards_on = 1;   // every later chunk arms its own
    klog_printf("sched: %d/%d kernel-stack guard pages armed (%d KiB stacks); "
                "the limit is %d processes, stacks allocated %d at a time\n",
                ok, g_slots_ready, PROC_KSTACK_SIZE / 1024, g_max_procs, SCHED_SLOT_CHUNK);
}

// Which slot's guard page contains `addr`, or -1. The fault reporter
// asks, so a page fault on a guard page is reported as what it is
// instead of as an anonymous #PF in the middle of the kernel.
int scheduler_kstack_guard_slot(uint64_t addr) {
    for (int i = 0; i < g_slots_ready; i++) {
        if (kstack_guard_contains(procs[i].kstack, addr)) return i;
    }
    return -1;
}

// LINUX'S SHAPE (kernel/fork.c, set_max_threads()): process structures may
// use at most an eighth of RAM. Floored at one chunk, so a small machine
// keeps what toy-os always had; capped so pids can never run out
// (SCHED_PID_MAX is four times the ceiling: own pid, pgid, sid, spare).
static int limit_from_ram(void) {
    uint64_t ram = pmm_total_frames() * pmm_frame_size();
    uint64_t per = sizeof(struct sched_process) + sizeof(struct kstack) +
                   sizeof(struct sched_slot_ext);
    uint64_t n = ram / 8 / per;
    if (n < SCHED_SLOT_CHUNK) n = SCHED_SLOT_CHUNK;
    if (n > SCHED_PROCS_CEILING) n = SCHED_PROCS_CEILING;
    return (int)n;
}

void scheduler_init(void) {
    g_max_procs = limit_from_ram();
    uint64_t pages = ((uint64_t)g_max_procs * sizeof(struct sched_process) + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    if (!phys) {   // a smaller table beats no scheduler
        g_max_procs = SCHED_SLOT_CHUNK;
        pages = ((uint64_t)g_max_procs * sizeof(struct sched_process) + 4095) / 4096;
        phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    }
    procs = (struct sched_process *)(uintptr_t)phys;
    if (procs) k_memset(procs, 0, pages * 4096);
    g_slot_end = 0;
    g_slots_ready = 0;
    slot_grow();   // the first chunk now, so boot keeps a guaranteed 64
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
// returns the runnable one at the best level that has run least. Falls back to ROT_KERNEL when nothing
// else is runnable, which is the pre-rotation behaviour: a tick that
// finds no ready process resumes the kernel, exactly as before.
//
// `start` may be -1 (nothing was running); the +MAX_PROCS+1 term keeps
// the modulo positive for it.
// A process's scheduling priority, by pid. The picker above is the
// only reader; these are the only writers.
int scheduler_set_priority(int pid, int value) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    procs[idx].prio = (int8_t)value;
    return 0;
}

int scheduler_get_priority(int pid, int *value) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    *value = procs[idx].prio;
    return 0;
}

// A WAKE THAT OUTRANKS WHAT IS RUNNING asks for a switch on the way out
// of the trap it happened in (scheduler_trap_exit()) -- Linux's
// TIF_NEED_RESCHED. Only a STRICTLY better level: a wake at the same
// level would preempt on every interrupt and give nothing back. The one
// exception is a context parked MID-CALL that has run less than what is
// running -- see wake_slot().
int g_need_resched;

// The level a wake has to beat: the running process's, or the kernel
// slot's default when the kernel context is what runs.
static int running_prio(void) {
    return current_index >= 0 ? procs[current_index].prio : 0;
}

static int runnable_at(int idx) {
    return procs[idx].state == SCHED_READY && !procs[idx].stopped;
}

// WHO RUNS NEXT WITHIN A LEVEL IS WHOEVER HAS RUN LEAST -- CFS's rule,
// with the strict levels above it kept. Two tie-breaks this replaced
// (a preempted process first, Windows'; a context woken mid-call first,
// NEXT_BUDDY's) each held alone and together had no bound: a disk-bound
// thread and the kernel context handed the CPU to each other on every
// IRQ and every other READY process waited forever (docs/decisions.md,
// "Within a level, whoever has run least runs next"). Both fall out of
// this one: a preempted or long-parked context has used less.
//
// The kernel context takes part at level 0 with its own figure, which
// its idle hlt does not advance -- idle is not running (Linux's idle
// task is no CFS entity at all).
uint64_t kernel_vruntime;
uint64_t g_min_vruntime;      // the pack's floor; never goes back
static int g_kernel_halting;         // inside scheduler_idle_halt()

static uint64_t vr_credit_ns(void);  // one slice -- see g_timeslice_ms

static uint64_t *vr_of(int rot) {
    return rot == ROT_KERNEL ? &kernel_vruntime : &procs[rot].vruntime;
}

// A context arriving -- woken, spawned, or the kernel after idling --
// goes no further back than one slice behind the pack. Without the
// floor, a process that slept an hour is owed an hour (CFS's
// place_entity()).
void vr_place(int rot) {
    uint64_t c = vr_credit_ns();
    uint64_t floor = g_min_vruntime > c ? g_min_vruntime - c : 0;
    if (*vr_of(rot) < floor) *vr_of(rot) = floor;
}

int find_next_runnable(int start) {
    // THE BEST LEVEL PRESENT, first. Strict priority between levels and
    // least-run-first within one (vruntime, above), which keeps equals
    // fair while letting a woken driver in ahead of the desktop. The kernel's own slot sits at the default level, so it
    // is not starved by ordinary processes and IS outranked by a
    // driver -- which is the point.
    int best = 127;
    for (int idx = 0; idx < MAX_PROCS; idx++)
        if (runnable_at(idx) && procs[idx].prio < best) best = procs[idx].prio;
    if (kernel_slot_runnable() && 0 < best) best = 0;

    // Scanned in rotation order from `start`, so among equals the next
    // one along wins, which is the old round-robin.
    int pick = ROT_KERNEL, found = 0;
    uint64_t pv = 0;
    // Positions 0 .. MAX_PROCS-1 are slots and MAX_PROCS is the kernel's;
    // `start` may be ROT_KERNEL, -1, or past an end that has since moved.
    int npos = MAX_PROCS + 1;
    int from = (start < 0 || start >= MAX_PROCS) ? MAX_PROCS : start;
    for (int i = 1; i <= npos; i++) {
        int pos = (from + i) % npos;
        int idx = pos == MAX_PROCS ? ROT_KERNEL : pos;
        if (idx == ROT_KERNEL) {
            if (!kernel_slot_runnable() || best != 0) continue;
            vr_place(ROT_KERNEL);   // idling left it behind: owed one slice, not the idle
        } else if (!runnable_at(idx) || procs[idx].prio != best) {
            // STOPPED IS CHECKED HERE AND NOWHERE ELSE (runnable_at()).
            // One picker means one place suspension has to be honoured.
            continue;
        }
        uint64_t v = *vr_of(idx);
        // `found`, not `pick < 0`: ROT_KERNEL is negative, so choosing the
        // kernel must not read as having chosen nothing.
        if (!found || v < pv) { pick = idx; pv = v; found = 1; }
    }
    if (!found) return ROT_KERNEL;
    if (pv > g_min_vruntime) g_min_vruntime = pv;
    return pick;
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

void trace_sched(const char *what, int idx) {
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
                    "while cur=%d (tgid=%d)\n", where, i, procs[i].pid, procs[i].name,
                    current_index, procs[i].tgid);
        scheduler_trace_dump();
        return;
    }
}

// A PROCESS THAT HAS NEVER RUN CANNOT HAVE SAVED A CONTEXT, so it gets
// one built by hand: rsp at its trapframe, rip at the interrupt
// epilogue's pops. The first restore therefore lands exactly where a
// resume always used to -- Linux plants `ret_from_fork` on a fresh
// kernel stack for the same reason. The callee-saved registers are
// zeroed and immediately overwritten by those pops.
void proc_start_context(int slot, const uint64_t *tf) {
    struct kernel_context *k = &procs[slot].kctx;
    k_memset(k, 0, sizeof *k);
    k->rsp = (uint64_t)tf;
    k->rip = (uint64_t)&isr_resume_frame;
    // Its own, not the spawner's: a slot is reused, and both of these
    // describe a context that does not exist yet.
    procs[slot].isr_depth = 0;
    procs[slot].preempt_depth = 0;
    procs[slot].parked_in_kernel = 0;
    procs[slot].offcpu_tsc = 0;
    procs[slot].left_tsc = 0;
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
    *(from >= 0 ? &procs[from].left_tsc : &kernel_left_tsc) = arch_rdtsc();
}

static void context_load_globals(int to) {
    isr_depth_set(to >= 0 ? procs[to].isr_depth : kernel_saved_isr_depth);
    g_preempt_depth = to >= 0 ? procs[to].preempt_depth : kernel_preempt_depth;
    uint64_t *left = to >= 0 ? &procs[to].left_tsc : &kernel_left_tsc;
    uint64_t *off  = to >= 0 ? &procs[to].offcpu_tsc : &kernel_offcpu_tsc;
    uint64_t now = arch_rdtsc();
    if (*left && now > *left) *off += now - *left;
    *left = 0;
}

uint64_t scheduler_offcpu_tsc(void) {
    return current_index >= 0 ? procs[current_index].offcpu_tsc : kernel_offcpu_tsc;
}

// THE TIME SLICE IS A DEADLINE, NOT A TICK COUNT (kernel.timeslice_ms).
// Every switch starts a new one; a timer event that finds it over, with
// somebody else runnable, rotates. On the periodic path that is checked
// once a tick, so a slice shorter than a tick is a tick -- which at the
// old 100 Hz is exactly the rotate-every-tick this replaced.
static uint32_t g_timeslice_ms = SCHED_TIMESLICE_DEFAULT_MS;
static uint64_t g_slice_start_ns, g_slice_end_ns;

static void slice_restart(void) {
    g_slice_start_ns = clocksource_now_ns();
    g_slice_end_ns = g_slice_start_ns + (uint64_t)g_timeslice_ms * 1000000ull;
}

// A DEADLINE WAKE PREEMPTS AN EQUAL. A process whose own timeout came
// due runs NOW rather than when the busy one's slice ends -- once that
// one has had WAKE_GRAN_NS of its slice (CFS's wakeup granularity);
// short of it, the slice is cut to it. Without this a deadline kept to
// the microsecond was followed by up to a slice of waiting for the CPU.
// ONLY for deadlines: an interrupt-driven wake keeps the strictly-better
// rule in wake_slot() (docs/decisions/kernel.md, "A wake preempts only
// from a better level"), since one armed deadline cannot be "every
// interrupt".
#define WAKE_GRAN_NS 1000000ull

static uint64_t vr_credit_ns(void) { return (uint64_t)g_timeslice_ms * 1000000ull; }

// Has `i` run less than what is running now, by more than a
// granularity? The in-progress slice counts -- the running one has
// been using the CPU since g_run_start_ns.
static int vr_behind_running(int i) {
    uint64_t now = clocksource_now_ns();
    uint64_t run = now > g_run_start_ns ? now - g_run_start_ns : 0;
    uint64_t cur = current_index >= 0 ? procs[current_index].vruntime + run
                 : kernel_vruntime + (g_kernel_halting ? 0 : run);
    return procs[i].vruntime + WAKE_GRAN_NS < cur;
}

static void wake_preempt(int i) {
    if (current_index < 0 && clockevent_in_idle()) return; // the idle path hands over already
    if (procs[i].prio != running_prio()) return;           // better prio has its own rule
    vr_place(i);
    if (!vr_behind_running(i)) return;                     // it has had its share
    uint64_t gran_end = g_slice_start_ns + WAKE_GRAN_NS;
    if (clocksource_now_ns() >= gran_end) {
        g_need_resched = 1;
    } else if (gran_end < g_slice_end_ns) {
        g_slice_end_ns = gran_end;
    }
}

uint32_t scheduler_timeslice_ms(void) { return g_timeslice_ms; }

int scheduler_set_timeslice_ms(uint32_t ms) {
    if (ms < SCHED_TIMESLICE_MIN_MS || ms > SCHED_TIMESLICE_MAX_MS) return 0;
    g_timeslice_ms = ms;
    return 1;
}

int scheduler_any_ready(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (i != current_index && runnable_at(i)) return 1;
    return 0;
}

int scheduler_kernel_running(void) { return current_index < 0; }

void switch_to(int from, int idx) {

    check_one_running("switch_to");
    trace_sched("switch_to", idx);
    kstack_verify(idx);   // before trusting anything else about this slot
    // A slot selected before anything gave it somewhere to resume. Was
    // a zero RSP handed to isr_common; now it is a jump to address 0,
    // which is no more diagnosable, so it is still checked here.
    if (!procs[idx].kctx.rip) {
        klog_printf("SLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                    idx, procs[idx].pid, procs[idx].state, procs[idx].name);
        vga_printf("\nSLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                   idx, procs[idx].pid, procs[idx].state, procs[idx].name);
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
    slice_restart();
    clockevent_reprogram();   // a new slice, and the tick back if it was stopped
    process_context_restore_noirq(&procs[idx].kctx, 1);
}

// The ROT_KERNEL counterpart to switch_to(): hand the CPU back to the
// kernel context. No CR3 switch, no RSP0 repoint and no FP restore --
// see the ROT_KERNEL comment above for why the kernel needs none of the
// three. Kept as its own function purely so both callers (the tick and
// the exit path) state the same thing once.
void switch_to_kernel(int from) {
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
    slice_restart();
    clockevent_reprogram();
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

int scheduler_mark_current_ready(void) {
    int pid = scheduler_current_pid();
    if (pid <= 0 || current_index < 0) return 0;
    procs[current_index].ready = 1;
    return 1;
}

int scheduler_max_procs(void) { return g_max_procs; }
int scheduler_live_count(void) { return alive_count; }

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

void bill_current(void) {
    uint64_t now = clocksource_now_ns();
    if (now > g_run_start_ns) {
        uint64_t slice = now - g_run_start_ns;
        if (current_index >= 0) {
            procs[current_index].cpu_ns += slice;
            procs[current_index].vruntime += slice;
            g_proc_ns += slice;
        } else {
            g_kernel_ns += slice;
            if (!g_kernel_halting) kernel_vruntime += slice;
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

// The rotation, shared by the timer and by SYS_YIELD. They are the same
// operation now that neither one is where billing happens -- see
// bill_current() above.
static void scheduler_rotate(uint64_t *regs);

// Whether a timer event should rotate: the slice is over -- or the
// kernel context is IDLING with a process ready, which should run now
// rather than when the idle loop's slice would have ended.
static int timer_wants_rotation(uint64_t now) {
    if (current_index < 0 && clockevent_in_idle()) return scheduler_any_ready();
    return g_need_resched || now >= g_slice_end_ns;
}

void scheduler_tick(uint64_t *regs) {
    // BEFORE the rotation, and outside scheduler_rotate()'s armed
    // check: a sleeper's deadline has nothing to do with whether the
    // scheduler is currently rotating, and a woken process wants to be
    // eligible for the switch this very tick rather than the next one.
    uint64_t now = clocksource_now_ns();
    scheduler_wake_timers(now);
    if (timer_wants_rotation(now)) scheduler_rotate(regs);
}

void scheduler_timer_event(uint64_t *regs, uint64_t now) {
    scheduler_wake_timers(now);
    if (timer_wants_rotation(now)) scheduler_rotate(regs);
}

uint64_t scheduler_next_event_ns(void) {
    uint64_t next = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED || !procs[i].wake_at_ns) continue;
        if (!next || procs[i].wake_at_ns < next) next = procs[i].wake_at_ns;
    }
    // The slice's end matters only with somebody to hand over to. A
    // process always has one -- the kernel context is a participant.
    if (current_index >= 0 || scheduler_any_ready())
        if (!next || g_slice_end_ns < next) next = g_slice_end_ns;
    return next;
}

// SYS_YIELD's entry into the same rotation. Distinct from the tick only
// so the call sites read honestly; the accounting difference that used
// to justify two paths is gone.
// A YIELD GOES TO THE BACK of its level: its vruntime is moved up to the
// most anyone there has run, so every peer goes first -- the old
// "SCHED_COMPAT_YIELD" behaviour of CFS. Without it the least-run rule
// hands a yielder straight back the CPU it just gave up, since a
// process that only yields has run almost nothing (cputime_test).
void scheduler_yield(uint64_t *regs) {
    if (current_index >= 0) {
        int lvl = procs[current_index].prio;
        uint64_t top = procs[current_index].vruntime;
        for (int i = 0; i < MAX_PROCS; i++)
            if (i != current_index && runnable_at(i) && procs[i].prio == lvl &&
                procs[i].vruntime > top)
                top = procs[i].vruntime;
        if (lvl == 0 && kernel_slot_runnable() && kernel_vruntime > top) top = kernel_vruntime;
        procs[current_index].vruntime = top;
    }
    scheduler_rotate(regs);
}

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
    // A syscall reaches here too (scheduler_trap_exit(), SYS_YIELD), and
    // under a trap gate it arrives with IF SET: a tick nested between
    // "state = READY" and switch_to()'s save resumes a torn context.
    sched_switch_begin();


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
        slice_restart();   // asked again a slice from now, not in a storm
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
        slice_restart();
        return;
    }

    // Consumed only by a rotation that can happen: one refused above
    // leaves it for the next trap rather than losing the wake.
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
    int idx = pid_slot(pid);
    if (idx < 0) return 0;
    return &procs[idx];
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
// In one-shot mode the timer is armed for the earliest deadline
// (scheduler_next_event_ns()), so a sleep ends on time; on the periodic
// path its resolution is one tick.
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

        // SYS_SLEEP asked for exactly this and returns 0. Anything else
        // was waiting for an EVENT that did not come, so its handler
        // has to look again and decide -- it is the only code that
        // knows whether an empty queue at the deadline is a timeout or
        // a spurious wake.
        //
        // NOT for a context parked MID-CALL: its kernel_rsp is the
        // trapframe of a call still in progress, and its own C code
        // re-tests the condition on return -- the same rule
        // scheduler_wake_n() keeps.
        if (!procs[i].parked_in_kernel) {
            uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
            tf[TF_RAX] = procs[i].wait_chan == SCHED_CHAN_TIMER
                         ? 0 : (uint64_t)(int64_t)SYS_RETRY;
        }
        procs[i].wake_at_ns = 0;
        procs[i].state = SCHED_READY;
        wake_preempt(i);   // places it, too
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
    return scheduler_block_kernel_until(chan, reason, 0);
}

int scheduler_block_kernel_until(const void *chan, int reason, uint64_t wake_at_ns) {
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
    procs[idx].wake_at_ns = wake_at_ns;   // 0 = unbounded
    procs[idx].wait_reason = reason;
    current_index = -1;

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel(idx);
    else switch_to(idx, next);

    // Resumed, on our own stack, with every frame below us intact.
    procs[idx].parked_in_kernel = 0;
    return 1;
}

// One BLOCKED slot, made READY and answered -- the half of a wake that
// acts on a parked process, shared by every way of choosing one.
static void wake_slot(int i, int64_t value) {
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
    vr_place(i);
    if (procs[i].prio < running_prio()) g_need_resched = 1;
    // MID-CALL, AT THE SAME LEVEL: it is usually holding the filesystem
    // lock, so it cuts in -- but only while it has run less than what it
    // would preempt. That bound is what the unconditional version lacked.
    else if (procs[i].parked_in_kernel && procs[i].prio == running_prio() &&
             vr_behind_running(i))
        g_need_resched = 1;
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
        wake_slot(i, value);
        woken++;
    }
    return woken;
}

int scheduler_wake_one(const void *chan) {
    if (!chan) return 0;
    int pick = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED || procs[i].wait_chan != chan) continue;
        if (pick < 0 || procs[i].prio < procs[pick].prio) pick = i;
    }
    if (pick < 0) return 0;
    wake_slot(pick, 0);
    return procs[pick].pid;
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
    return current_index < 0 ? 0 : procs[current_index].pid;
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
    int s = pid_slot(pid);
    if (s < 0 || procs[s].state == SCHED_UNUSED) return 0;
    return procs[s].tgid;
}

int scheduler_exec_path(int pid, char *out, unsigned cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    int s = pid_slot(pid);
    if (s < 0) return 0;
    struct sched_process *p = &procs[s];
    if (p->state == SCHED_UNUSED) return 0;
    // REFUSE rather than truncate: callers match on this string to
    // decide which program a window belongs to, and a shortened path
    // matches the wrong one.
    if (k_strlcpy(out, p->ext->exec_path, cap) >= cap) { out[0] = '\0'; return 0; }
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
    return &procs[leader_index(current_index)].ext->cwd;
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
    int slot = pid_slot(pid);
    if (slot < 0) return 0;
    if (procs[slot].state == SCHED_UNUSED || procs[slot].state == SCHED_ZOMBIE)
        return 0;
    if (is_thread(slot)) return 0;
    return &procs[slot].mm;
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
    int slot = pid_slot(pid);
    if (slot < 0 || procs[slot].state == SCHED_UNUSED) return 0;
    if (is_thread(slot)) return 0;
    return procs[slot].ppid == parent_pid;
}

int scheduler_pid_valid(int pid) {
    int s = pid_slot(pid);
    return s >= 0 && procs[s].state != SCHED_UNUSED;
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

void scheduler_demo_run(void) {
    int a = spawn_from_fs("/bin/counter_a", NULL, 0, -1, -1, -1, 0, 0, 0);
    int b = spawn_from_fs("/bin/counter_b", NULL, 0, -1, -1, -1, 0, 0, 0);
    if (a < 0 || b < 0) {
        vga_write("schedtest: failed to spawn one or both counter processes --\n");
        vga_write("were /bin/counter_a and /bin/counter_b seeded onto disk.img?\n");
        vga_write("(see the Makefile's `seed` target)\n");
        if (a >= 0) { slot_free(a); alive_count--; }   // spawn_from_fs() returns a SLOT
        if (b >= 0) { slot_free(b); alive_count--; }
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
        if (!a_done && scheduler_poll(procs[a].pid, &a_code) == SCHED_POLL_EXITED) a_done = 1;
        if (!b_done && scheduler_poll(procs[b].pid, &b_code) == SCHED_POLL_EXITED) b_done = 1;
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
    // the platter rather than waiting for the next barrier. Through
    // ata.c, which skips the round if the drive is busy.
    ata_idle();
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

// Interrupts off around each bill: a tick that rotates bills too, and
// two interleaved bill_current()s would charge one slice twice.
void scheduler_idle_halt(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    bill_current();          // what the kernel ran up to here IS charged...
    g_kernel_halting = 1;    // ...the halt is not (see kernel_vruntime)
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
    clockevent_idle_halt();
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    bill_current();
    g_kernel_halting = 0;
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

// --- the rewound-syscall window (see struct sched_proc.syscall_reissue) --

int scheduler_syscall_reissue_pending(int pid) {
    int s = pid_slot(pid);
    return s < 0 ? 0 : procs[s].syscall_reissue;
}

void scheduler_syscall_entered(int pid) {
    int s = pid_slot(pid);
    if (s >= 0) procs[s].syscall_reissue = 0;
}
