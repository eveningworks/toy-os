// Ending a process: exit, kill, reaping a zombie, and handing orphans
// to init. Split out of scheduler.c; sched_internal.h has the map.

#include "sched_internal.h"
#include "futex.h"
#include "remote_log.h" // a session created from a socket is a REMOTE session
#include "win_role.h"
#include "syscall.h" // syscall_process_kill_cleanup()
#include "vmm.h"     // vmm_current_pml4() -- the exiting group's address space
#include "diag.h"     // diag_provider_gone() -- drop a dead service's name
#include "kfmt.h"      // klog_printf, vga_printf
#include "signal.h"     // signal_send() -- SIGCHLD to a parent, see notify_parent()
#include "klog.h"

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
    if (ppid < 1) return;
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
                who, reaper, procs[idx].pid, procs[idx].name, procs[idx].state, ppid);
    scheduler_trace_dump();
}

// Free a slot outright, keeping the live count honest whichever state
// it was in. A ZOMBIE has already been subtracted.
static void slot_release(int idx) {
    reap_audit(idx, "slot_release");
    if (procs[idx].state == SCHED_UNUSED) return;
    if (procs[idx].state != SCHED_ZOMBIE) alive_count--;
    slot_free(idx);
}

// Every OTHER thread of `leader`'s group stops existing.
//
// Freed outright rather than zombied: a thread is not waitable by
// anything outside its own process, so a corpse nobody can reap would
// hold its slot for the rest of the boot. Their kernel stacks are
// simply never resumed again, which is sound ONLY because no sibling is
// parked mid-call by now -- every caller asks group_defer_death() first,
// since a mid-call thread's frames may hold the disk or a mount lock.
void group_release_threads(int leader) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (i == leader) continue;
        if (procs[i].state == SCHED_UNUSED) continue;
        if (procs[i].tgid == procs[leader].pid) slot_release(i);
    }
}

static int in_group(int i, int leader) {
    return procs[i].state != SCHED_UNUSED && procs[i].state != SCHED_ZOMBIE &&
           (i == leader || procs[i].tgid == procs[leader].pid);
}

// How many of `leader`'s threads other than `except` are PARKED MID-CALL
// -- Linux's D state. READY counts: woken but not yet resumed, it is
// still inside those frames (scheduler_kill() says why).
static int group_parked(int leader, int except) {
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (i != except && in_group(i, leader) && procs[i].parked_in_kernel) n++;
    return n;
}

// **A THREAD PARKED MID-CALL IS NEVER FREED WHERE IT STANDS.** Its C
// frames may hold a mount's lock across a disk wait, or the disk's own
// lock, and freeing it orphans that lock: every later file call in the
// machine then waits forever (System Update killed mid-check froze the
// desktop that way). So the group dies the way Linux's exit_group()
// does it: every other thread gets a pending SIGKILL and leaves the
// kernel on its own, the address space stays until the last one has,
// and the leader waits as an unreapable zombie. Returns 1 when deferred.
static int group_defer_death(int leader, int except, int code) {
    if (!group_parked(leader, except)) return 0;
    if (!procs[leader].group_dying) {
        procs[leader].group_dying = 1;
        procs[leader].group_exit_code = code;   // the FIRST death's reason
        klog_printf("sched: pid %d dies once its threads leave the kernel (%d mid-call)\n",
                    procs[leader].pid, group_parked(leader, except));
    }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (i == except || !in_group(i, leader)) continue;
        procs[i].stopped = 0;   // it must be able to finish the call
        scheduler_signal_raise(procs[i].pid, SIGKILL);
    }
    return 1;
}

void scheduler_exit_group(int code) {
    if (current_index < 0) return;
    int me = current_index;
    int leader = leader_index(me);
    if (!group_defer_death(leader, me, code)) {
        // The last thread out of a deferred death reports the first one's
        // reason, and the whole teardown happens here.
        if (procs[leader].group_dying) code = procs[leader].group_exit_code;
        syscall_process_exit_cleanup(vmm_current_pml4());
        scheduler_on_exit(code);
        return;
    }

    // DEFERRED: leave without touching the address space a sibling is
    // still inside. A thread's slot simply goes; the leader's stays as the
    // unreapable zombie that holds the pid for the parent.
    scheduler_preempt_disable();
    if (me == leader) {
        if (procs[me].state != SCHED_ZOMBIE) alive_count--;
        procs[me].state = SCHED_ZOMBIE;
    } else {
        slot_release(me);
    }
    scheduler_preempt_enable();
    sched_switch_begin();
    bill_current();
    current_index = -1;
    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) { switch_to_kernel(me); return; }
    switch_to(me, next);
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
    procs[leader].group_dying = 0;   // reapable from here

    // A REMOTE SESSION ENDS WHEN ITS LEADER DOES, whether it said
    // goodbye or the link dropped -- a no-op for every other process,
    // and what stops the tray indicator outliving the connection.
    int pid = procs[leader].pid;
    remote_log_session_closed(pid);

    // Tell the window server to drop anything this client still owned.
    // Here rather than at reap: a zombie's windows must come off the
    // screen the moment it dies, not whenever someone gets round to
    // polling it -- otherwise a crashed client leaves a window that
    // draws stale pixels and answers no input. A no-op when no server
    // is registered, which is every non-GUI boot.
    win_server_client_died(pid, code);
    diag_provider_gone(pid);

    // Its children lose their parent before anything can reuse this
    // slot -- see reparent_children() for why that ordering matters.
    reparent_children(pid);

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
    scheduler_wake(scheduler_wait_chan_pid(procs[me].pid), SYS_RETRY);
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
    int slot = pid_slot(tid);
    if (slot < 0 || current_index < 0) return SCHED_POLL_INVALID;
    // ONLY WITHIN ONE PROCESS. A tid names any slot, so without this a program could join another program's thread and
    // free its slot.
    if (procs[slot].state == SCHED_UNUSED) return SCHED_POLL_INVALID;
    if (procs[slot].tgid != procs[current_index].tgid) return SCHED_POLL_INVALID;
    if (slot == current_index) return SCHED_POLL_INVALID; // joining itself
    if (!is_thread(slot)) return SCHED_POLL_INVALID;      // the leader is not joinable
    if (procs[slot].detached) return SCHED_POLL_INVALID;  // and neither is a detached one

    if (procs[slot].state == SCHED_ZOMBIE) {
        if (out_code) *out_code = procs[slot].exit_code;
        reap_audit(slot, "thread_poll");
        slot_free(slot); // reaped -- see scheduler_poll()
        return SCHED_POLL_EXITED;
    }
    return SCHED_POLL_RUNNING;
}

// Nobody will join `tid`, so let its exit free the slot. Applied to a
// thread that has ALREADY exited, this reaps it -- which is what makes
// detach-after-the-fact safe rather than a leak.
int scheduler_thread_detach(int tid) {
    if (tid < 1) return -EINVAL;
    if (current_index < 0) return -EPERM;
    int slot = pid_slot(tid);
    if (slot < 0 || procs[slot].state == SCHED_UNUSED) return -ESRCH;
    if (procs[slot].tgid != procs[current_index].tgid) return -ESRCH;
    if (!is_thread(slot)) return -EINVAL;
    if (procs[slot].detached) return -EINVAL;

    procs[slot].detached = 1;
    if (procs[slot].state == SCHED_ZOMBIE) { reap_audit(slot, "thread_detach"); slot_free(slot); }
    return 0;
}

// Called when a process dies, on both paths. Its children lose their
// parent, and the reason that MATTERS is not tidiness: pids are reused
// once they wrap. Leaving a child pointing at its dead parent's pid means
// that as soon as that pid is handed out again, the child claims to be
// the new process's child -- and a waitpid(-1)
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
void reparent_children(int dead_pid) {
    if (dead_pid <= 0) return;
    int heir = (g_init_pid != dead_pid) ? g_init_pid : 0;
    int zombie = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        // A THREAD IS NOT A CHILD. Its ppid names its leader for
        // display only, and by the time a leader dies its threads are
        // already gone -- adopting one out to init would hand init a
        // slot it can never reap.
        if (is_thread(i)) continue;
        if (procs[i].state != SCHED_UNUSED && procs[i].ppid == dead_pid) {
            procs[i].ppid = heir;
            zombie |= procs[i].state == SCHED_ZOMBIE;
        }
    }
    // An adopted ZOMBIE is one init can reap immediately, and it may be
    // parked in waitpid(-1) right now with no children of its own -- in
    // which case it was told "never" and is asleep on a timer instead.
    // Waking child-waiters here is what turns adoption into a reap
    // rather than a slot that frees at init's next poll. AN ADOPTED
    // CORPSE IS A CHILD'S DEATH as far as the heir can tell, so it gets
    // the whole notice -- the wakeword too, which is where init waits
    // (Linux's reparent_leader() -> do_notify_parent()).
    if (heir && zombie) notify_parent(heir);
    else if (heir) scheduler_wake(scheduler_wait_chan_pid(heir), SYS_RETRY);
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
    int s = pid_slot(pid);
    if (s < 0 || new_ppid < 0) return 0;
    if (new_ppid == pid) return 0;
    if (procs[s].state == SCHED_UNUSED) return 0;
    procs[s].ppid = new_ppid;
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
    if (parent_pid < 1) return SCHED_POLL_INVALID;

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
        if (procs[i].state == SCHED_ZOMBIE && !procs[i].group_dying) {
            if (out_pid) *out_pid = procs[i].pid;
            if (out_exit_code) *out_exit_code = procs[i].exit_code;
            reap_audit(i, "poll_any");
            slot_free(i); // reap, as scheduler_poll() does
            return SCHED_POLL_EXITED;
        }
    }
    return any_children ? SCHED_POLL_RUNNING : SCHED_POLL_INVALID;
}

int scheduler_kill(int pid, int exit_code) {
    int slot = pid_slot(pid);
    if (slot < 0) return 0;

    // A TID IS NOT SEPARATELY KILLABLE: the cleanup below destroys an
    // address space, and a thread's is its siblings'. Naming a thread
    // kills the process it belongs to, which is what kill(2) means and
    // what tkill(2) exists separately for.
    if (procs[slot].state != SCHED_UNUSED && is_thread(slot)) {
        slot = leader_index(slot);
        pid  = procs[slot].pid;
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

    // **PARKED MID-CALL, IT DIES ON THE WAY OUT, NOT HERE** -- Linux's D
    // state. Its C frames may hold the filesystem lock across a disk
    // wait, and zombifying it now abandons them with the lock taken:
    // every later file call in the machine then waits forever. So the
    // kill becomes a pending SIGKILL, delivered on its return to ring 3
    // once the call completes, and the exit reports this `exit_code`
    // (group_defer_death() keeps it).
    // READY COUNTS TOO: woken but not yet resumed, it is still inside
    // those frames -- and since a mid-call wake waits its turn (see
    // wake_slot()), that window is a slice long, not a few instructions.
    // AND THE SAME FOR ANY THREAD OF IT: a sibling parked mid-call holds
    // those frames just as the leader would (group_defer_death()).
    if (group_defer_death(slot, -1, exit_code)) return 1;

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

    remote_log_session_closed(pid);   // killed counts as ended

    // Exactly the teardown scheduler_on_exit() does, and for the same
    // reasons -- see its comments. A killed client's windows must come
    // off the screen now rather than at reap, or a dead process leaves a
    // window drawing stale pixels and answering no input.
    win_server_client_died(pid, exit_code);
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
    int slot = pid_slot(pid);
    if (slot < 0) return SCHED_POLL_INVALID;

    if (procs[slot].state == SCHED_ZOMBIE && procs[slot].group_dying)
        return SCHED_POLL_RUNNING;   // a thread is still leaving the kernel
    if (procs[slot].state == SCHED_ZOMBIE) {
        if (out_exit_code) *out_exit_code = procs[slot].exit_code;
        reap_audit(slot, "poll");
        slot_free(slot); // reap -- see scheduler.h's doc comment
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
