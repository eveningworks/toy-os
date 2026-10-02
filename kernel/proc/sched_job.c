// A process's signal state, its process group and session, and job
// control's stop and continue. Delivery itself is signal.c's; this is the
// per-slot state it reads. Split out of scheduler.c; sched_internal.h
// has the map.

#include "sched_internal.h"
#include "syscalls.h" // the fd table: a child inherits its parent's descriptors
#include "remote_log.h" // a session created from a socket is a REMOTE session
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees

// --- signals and process groups --------------------------------------
//
// The STATE only. What a signal means and when it is acted on is
// kernel/signal.c -- see api/scheduler.h for why the two are split.

// The slot behind `pid` if it is a live process, else NULL. A ZOMBIE is
// deliberately not live: it has no address space left to signal and
// nothing to interrupt.
static struct sched_process *live_slot(int pid) {
    int s = pid_slot(pid);
    if (s < 0) return 0;
    struct sched_process *p = &procs[s];
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
        int ps = pid_slot(procs[i].ppid);
        if (ps < 0) continue;
        const struct sched_process *par = &procs[ps];
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

// Everything a slot's SIGNAL state has to forget before somebody else
// gets it -- pending bits, dispositions, and any suspension.
//
// ONE FUNCTION FOR THREE CALLERS (spawn, the KTEST slot fabricator, and
// the release beside it) because the file already predicted the failure
// mode in prose: "one of the two places would eventually be the one
// that got forgotten". Adding a third field made that concrete -- a
// stopped bit surviving into the next tenant is a process that never
// runs and gives no reason.
void signal_state_reset(int slot) {
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
        int sig = scheduler_stop_report(procs[i].pid);
        if (sig) {
            if (out_pid) *out_pid = procs[i].pid;
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
        vr_place((int)(p - procs));
        p->wait_chan = 0;
        p->wake_at_ns = 0;   // the wait is over; see scheduler_wake()
    }
    return 1;
}
