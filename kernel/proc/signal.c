// Signal policy: sending, and the one place a signal is ACTED on.
//
// Read kernel/signal.h first -- it states the idea this file exists to
// implement (sending and acting are different moments) and why. What is
// here is the consequence of that split, plus the small amount of policy
// stage 0-2 of docs/signals-design.md actually has.
//
// HOW LITTLE POLICY THERE IS, and why that is deliberate: with no
// user-space handlers there are exactly two dispositions, so the whole
// policy is "SIGCHLD is ignored, stop and continue suspend and resume,
// everything else terminates". That is a complete and useful system --
// it is where Ctrl-C and Ctrl-Z both work -- and it fits in one
// function. Handlers (stage 3 of docs/signals-design.md) are what turn
// this file into something bigger; until then, resisting the temptation
// to build the table they would need is the point.
//
// THE ONE SURPRISE, AND IT IS POSIX'S TOO: a SIGTERM to a STOPPED
// process does nothing visible. The bit is set and delivery happens on
// the way back to ring 3, which a suspended process does not reach --
// so it dies when somebody continues it, and not before. SIGKILL is the
// exception, here as on Linux, because it never went through the
// pending set in the first place.
#include "signal.h"
#include "signal_abi.h"
#include "scheduler.h"
#include "syscall.h" // syscall_process_exit_cleanup() -- the current process's death
#include "vmm.h"
#include "ksignal.h" // signal_name(), for the log line
#include "klog.h"
#include "kfmt.h"

// Does `sig`'s DEFAULT action terminate the process?
//
// One entry differs and it is the one that matters: SIGCHLD's default is
// to be ignored, which is what makes it safe to send on every child exit
// without every existing program having to learn about it. Unix made the
// same call, and for the same reason.
static int default_terminates(int sig) {
    return sig != SIGCHLD && !SIGNAL_STOPS(sig) && !SIGNAL_CONTINUES(sig);
}

int signal_send(int pid, int sig) {
    if (!SIGNAL_VALID(sig)) return 0;

    // SIGKILL IS IMMEDIATE, AND THAT IS THE WHOLE POINT OF IT. Everything
    // else waits for the target to reach a return to ring 3 -- which a
    // process spinning in its own loop does reach (the timer preempts
    // it), but which one wedged inside a kernel path might not. Force
    // Quit has to work on the wedged case, so SIGKILL does not go
    // through the pending machinery at all.
    //
    // Safe here and not from an interrupt: scheduler_kill() frees the
    // victim's address space, which calls the heap. The caller is always
    // a syscall or kernel code (SYS_KILL, Task Manager, the WM); the
    // INTR key deliberately sends SIGINT instead.
    if (sig == SIGKILL) {
        // A process SIGKILLing itself cannot be torn down from inside
        // its own syscall -- scheduler_kill() refuses the current
        // process on purpose, because the teardown would pull CR3 out
        // from under the caller. Fall through to the pending path, which
        // delivers on the way back to ring 3 a few instructions later.
        // SIGKILL cannot be ignored, so the bit is guaranteed to survive.
        if (pid != scheduler_current_pid())
            return scheduler_kill(pid, SIGNAL_EXIT_BASE + SIGKILL);
    }

    // STOP AND CONTINUE ACT HERE, AT SEND TIME, and never reach the
    // pending set. Suspending flips one byte of scheduler state -- it
    // allocates nothing, frees nothing and unmaps nothing -- so unlike
    // a termination there is no reason to defer it to the target's
    // return to ring 3, and every reason not to: deferring would mean
    // teaching `pending` a second meaning, and "a set bit means this
    // process must die" is the invariant the whole design rests on.
    //
    // The consequence worth stating: a stop is delivered to a process
    // that never reaches ring 3 again -- one wedged inside a kernel
    // path -- exactly as reliably as to a running one, which is more
    // than SIGTERM can say. SIGSTOP is unignorable for the same reason
    // SIGKILL is (abi/signal_abi.h).
    if (SIGNAL_STOPS(sig)) {
        if (!SIGNAL_UNIGNORABLE(sig) && scheduler_signal_ignored(pid, sig))
            return scheduler_pid_alive(pid);
        return scheduler_stop(pid, sig);
    }
    if (SIGNAL_CONTINUES(sig)) {
        // NOT conditional on it having been stopped: SIGCONT to a
        // running process is a successful delivery that does nothing,
        // which is what POSIX says and what a shell's `bg` relies on
        // when it races a job that woke up on its own.
        scheduler_continue(pid);
        return scheduler_pid_alive(pid);
    }

    // A signal whose default action is to be ignored, and which the
    // process has not asked to hear about, does nothing at all -- there
    // is no handler for it to run. Reported as delivered, because it
    // was: the action was "nothing".
    if (!default_terminates(sig) && !scheduler_signal_ignored(pid, sig))
        return scheduler_pid_alive(pid);

    return scheduler_signal_raise(pid, sig);
}

int signal_send_group(int pgid, int sig) {
    if (pgid < 1 || !SIGNAL_VALID(sig)) return 0;

    // BY SLOT, not by pid, and taking the pid from the report: a slot's
    // index is not its pid once slots are reused, and assuming otherwise
    // is a mistake this tree has already made once (see /bin/kill).
    //
    // Snapshotting is unnecessary even though signal_send() can
    // terminate a process mid-walk: killing a process only ever empties
    // its own slot, and a slot already passed cannot come back.
    int reached = 0;
    struct proc_info info;
    for (int i = 0; i < scheduler_max_procs(); i++) {
        if (!scheduler_proc_info(i, &info)) continue;
        if (info.pid == 0) continue;
        if (scheduler_pgid(info.pid) != pgid) continue;
        if (signal_send(info.pid, sig)) reached++;
    }
    return reached;
}

void signal_deliver_pending(int pid) {
    // The common case, and it must stay this cheap: this runs at the end
    // of every trap taken from ring 3 -- every syscall and every timer
    // tick of every process.
    if (!scheduler_signal_pending(pid)) return;

    int sig = scheduler_signal_take(pid);
    if (!sig) return;

    // EVERY PENDING SIGNAL TERMINATES, and the invariant that makes that
    // true is stated on `pending` in scheduler.c: an ignored signal is
    // dropped at arrival rather than queued, and there are no handlers
    // for anything else to mean.
    int code = SIGNAL_EXIT_BASE + sig;
    klog_printf("signal: pid %d terminated by SIG%s\n", pid, signal_name(sig));

    if (pid == scheduler_current_pid()) {
        // The process about to be resumed IS the one being killed, so
        // this is exactly SYS_EXIT's situation and takes SYS_EXIT's
        // path: release the kernel-side bookkeeping, drop the address
        // space (safe -- the process is leaving anyway), then hand the
        // CPU to whatever is next. Does not return in the ordinary
        // sense; isr_common resumes wherever scheduler_on_exit() pointed.
        syscall_process_exit_cleanup(vmm_current_pml4());
        scheduler_on_exit(code);
        return;
    }

    // The scheduler switched away during this same trap, so the target
    // is no longer the running process -- which is precisely the case
    // scheduler_kill() is built for.
    //
    // EXCEPT THAT CR3 MAY STILL BE THE VICTIM'S, and this cost a real
    // leak before it was noticed: switch_to_kernel() moves the CPU back
    // to the kernel context WITHOUT changing CR3, because every address
    // space shares the kernel's mappings and the kernel context does not
    // care which one it is standing in. So "not the running process" and
    // "not the loaded address space" are different questions, and
    // syscall_process_kill_cleanup() -- which can only ask the second --
    // refused the teardown and logged it, leaving the victim a zombie
    // with its ELF pages, stack, heap and window buffer still allocated.
    //
    // Moving CR3 to the kernel's first is safe for exactly the reason
    // the tick did not bother to: nothing is executing in the victim's
    // address space. It is not safe when a PROCESS is running there,
    // which is the case the branch above handles.
    uint64_t victim = scheduler_pid_pml4(pid);
    if (victim && victim == vmm_current_pml4())
        vmm_switch_address_space(vmm_kernel_pml4_phys());
    scheduler_kill(pid, code);
}
