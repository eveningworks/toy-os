// What the scheduler reports about itself: proc_info for `ps`, the
// kernel-stack debug surface behind `kstack`, and the hooks the KTESTs
// reach the table through. Split out of scheduler.c; sched_internal.h
// has the map.

#include "sched_internal.h"
#include "vmm.h"
#include "process.h" // process_context_is_armed() -- see kernel_slot_runnable()
#include "string.h" // k_strlcpy -- proc_name_from_path()

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
    out->pid        = procs[idx].pid;
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

// --- what `ps` and `kstack` read ------------------------------------

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
    case SCHED_WAIT_DISK:  return "disk";
    default:               return "?";
    }
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
    case SCHED_WAIT_DISK:  return PROC_WAIT_DISK;
    default:               return PROC_WAIT_NONE;
    }
}

int scheduler_pid_slot(int pid) { return pid_slot(pid); }

int scheduler_slot_pid(int slot) {
    if (slot < 0 || slot >= MAX_PROCS || procs[slot].state == SCHED_UNUSED) return 0;
    return procs[slot].pid;
}

int scheduler_proc_info_pid(int pid, struct proc_info *out) {
    int s = pid_slot(pid);
    return s >= 0 && scheduler_proc_info(s, out) && out->pid == pid;
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

    out->pid = p->pid;   // 0 is "no process"; never derived from `index`
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

// --- KTEST hooks -----------------------------------------------------

// See scheduler.h for the two rules a caller must keep. Claims the
// first free slot; -1 when the table is full. Deliberately does NOT
// touch alive_count -- this slot holds no process, and counting it
// would make the scheduler believe there is one more thing to run.
int scheduler_test_park(uint64_t *tf, const void *chan, int reason) {
    if (!tf) return -1;
    int i = slot_claim();
    if (i >= 0) {
        procs[i].state = SCHED_BLOCKED;
        slot_unclaim(i);   // published: its state is no longer UNUSED
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
        procs[i].pgid = procs[i].pid;
        procs[i].sid = procs[i].pid;
        procs[i].tgid = procs[i].pid;   // a process; scheduler_test_make_thread() changes that
        procs[i].group_dying = 0;
        // NOTHING REAL TO TEAR DOWN: a kill that is not deferred runs the
        // full teardown, and a stale address space or parent here would
        // be some earlier process's.
        procs[i].pml4_phys = 0;
        procs[i].ppid = 0;
        procs[i].prio = 0;
        procs[i].vruntime = g_min_vruntime;
        procs[i].parked_in_kernel = 0;   // a mid-call park is asked for, never inherited
        return i;
    }
    return -1;
}

void scheduler_test_make_thread(int idx, int leader) {
    if (idx < 0 || idx >= MAX_PROCS || leader < 0 || leader >= MAX_PROCS) return;
    procs[idx].tgid = procs[leader].pid;
}

int  scheduler_test_slot_claim(void)     { return slot_claim(); }
void scheduler_test_slot_unclaim(int s)  { slot_unclaim(s); }

void scheduler_test_park_deadline(int idx, uint64_t wake_at_ns, int in_kernel) {
    if (idx < 0 || idx >= MAX_PROCS || procs[idx].state != SCHED_BLOCKED) return;
    procs[idx].wake_at_ns = wake_at_ns;
    procs[idx].parked_in_kernel = in_kernel ? 1 : 0;
}

int scheduler_test_pick(int start) { return find_next_runnable(start); }

void scheduler_test_set_vruntime(int idx, uint64_t v) {
    if (idx == -1) kernel_vruntime = v;
    else if (idx >= 0 && idx < MAX_PROCS) procs[idx].vruntime = v;
}

uint64_t scheduler_test_vruntime(int idx) {
    if (idx == -1) return kernel_vruntime;
    if (idx == -2) return g_min_vruntime;
    return (idx >= 0 && idx < MAX_PROCS) ? procs[idx].vruntime : 0;
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
    slot_free(idx);
    procs[idx].wait_chan = 0;
    procs[idx].kernel_rsp = 0;
    procs[idx].isr_depth = 0;
    procs[idx].preempt_depth = 0;
    procs[idx].parked_in_kernel = 0;
    procs[idx].group_dying = 0;
    procs[idx].tgid = 0;
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
