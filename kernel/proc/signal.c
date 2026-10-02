// Signal policy: sending, and the one place a signal is ACTED on.
//
// Read kernel/signal.h first -- it states the idea this file exists to
// implement (sending and acting are different moments) and why. What is
// here is the consequence of that split, plus the small amount of policy
// stage 0-2 of docs/signals-design.md actually has.
//
// WHAT HANDLERS ADDED, and it is the second half of this file. Until
// stage 3 of docs/signals-design.md there were two dispositions and the
// whole policy was "SIGCHLD is ignored, stop and continue suspend and
// resume, everything else terminates" -- one function, no table. A
// handler makes delivery a THIRD thing: build a frame on the user
// stack, point the trapframe at ring-3 code, and arrange for the
// process to come back through SYS_SIGRETURN.
//
// **THE ONE THING THAT HAS TO BE EXACTLY RIGHT is putting the process
// back the way it was.** Everything in push_signal_frame() below is in
// service of that, and the places it deliberately does NOT restore
// verbatim (CS, SS, the privileged RFLAGS bits) are called out where
// they happen.
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
#include "errno.h"
#include "uaddr.h"  // UADDR_STACK_* -- where a frame is allowed to land
#include "gdt.h"    // SEL_USER_CODE/SEL_USER_DATA -- reimposed on sigreturn
#include "crash_report.h" // a core-dumping signal's report

// Does `sig`'s DEFAULT action terminate the process?
//
// TWO entries differ, and both are signals the system raises on its own
// rather than at anybody's request: SIGCHLD on every child exit and
// SIGWINCH on every window drag. Ignoring them by default is what makes
// them safe to send to programs that have never heard of them. Unix made
// the same call, for the same reason.
static int default_terminates(int sig) {
    return sig != SIGCHLD && sig != SIGWINCH &&
           !SIGNAL_STOPS(sig) && !SIGNAL_CONTINUES(sig);
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
    // process has not installed a handler for, does nothing at all --
    // there is no handler for it to run and no default to fall back on.
    // Reported as delivered, because it was: the action was "nothing".
    //
    // **THE TEST IS "IS THERE A HANDLER", NOT "IS IT IGNORED", and the
    // difference was invisible until something sent one of these.** This
    // asked `!scheduler_signal_ignored()`, which is the exact inverse:
    // it dropped the signal for a process that had installed a HANDLER
    // and let one through for a process that had explicitly set SIG_IGN
    // (where scheduler_signal_raise() then dropped it anyway, so the
    // second half was merely wasted work). Only SIGCHLD and SIGWINCH
    // reach this branch -- stop and continue return above, and everything
    // else terminates -- and nothing sent a SIGCHLD until the scheduler
    // started doing it on every child death, so the inversion sat here
    // unexercised. Found by the first check that asked a handler to run.
    if (!default_terminates(sig)) {
        struct k_sigaction act;
        if (!scheduler_signal_action(pid, sig, &act) ||
            !SIG_IS_HANDLER(act.handler))
            return scheduler_pid_alive(pid);
    }

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

// --- the handler path -------------------------------------------------

// RFLAGS bits ring 3 is allowed to choose for itself on sigreturn.
//
// Carry, parity, adjust, zero, sign, direction and overflow -- the
// condition codes, which are part of the interrupted computation and
// must come back exactly. Everything ELSE is the kernel's: IOPL would
// hand out port access, IF would let a process run with interrupts
// disabled, TF would single-step it, NT/RF/VM are nothing a program
// here has any business setting. Linux keeps the same list, by the same
// reasoning, in its `FIX_EFLAGS`.
#define RFLAGS_USER_MASK 0x0CD5ULL
// Bit 1 is reserved and always set; IF must be on or the process
// resumes with interrupts disabled and the machine stops.
#define RFLAGS_FORCED    0x0202ULL

// Where a signal frame may legally live: inside this process's own
// stack region, whole. Checked BEFORE the copy rather than relying on
// vmm_copy_to_user() to fault, because "the copy failed" cannot tell a
// stack that has run out from a stack pointer that has been aimed at
// somebody else's mapping -- and the second one is the case worth
// refusing loudly.
//
// **AGAINST THE FLOOR, NOT THE CURRENT BOTTOM.** The stack grows on
// demand now, so the mapped bottom moves down as a process runs, and
// testing against it would refuse a perfectly legal frame on any
// process that had grown -- the deeper the call chain, the more likely
// the refusal, which is the opposite of what a signal is for. The floor
// is what does not move. A frame landing on a reserved-but-unmapped
// page is fine: vmm_copy_to_user() walks through the same fault hook
// that grows the stack, so the page arrives on the way in.
static int frame_fits(uint64_t sp) {
    return sp >= UADDR_STACK_FLOOR &&
           sp + sizeof(struct sigframe) <= UADDR_STACK_VADDR + 4096;
}

// Build the frame and point `regs` at the handler. Returns 1 if the
// process is now set up to run its handler, 0 if it could not be -- in
// which case the caller must fall back to the default action, which is
// what Linux's force_sigsegv() does and for the same reason: a process
// whose stack cannot hold a frame cannot be told about anything.
//
// `restartable` says the interrupted trap is a syscall that has NOT RUN
// YET, which is the only situation in which SA_RESTART means anything.
static int push_signal_frame(int pid, int sig, const struct k_sigaction *act,
                             uint64_t *regs, int restartable) {
    struct sigframe f;

    for (int i = 0; i < SIGFRAME_TF_SLOTS; i++) f.regs[i] = regs[i];

    // WHAT THE INTERRUPTED SYSCALL SEES WHEN THE HANDLER IS DONE, and
    // this is the whole of SA_RESTART. Applied to the SAVED copy, not to
    // the live frame, so it takes effect at sigreturn -- after the
    // handler has run, which is the order POSIX describes.
    //
    // Rewinding RIP re-executes `int $0x80` with RAX still holding the
    // syscall number (nothing has written a return value into this frame
    // -- idt.c delivers before the dispatcher runs), so the call is made
    // again from scratch. Without the flag the call fails instead, with
    // the -EINTR abi/errno.h has always promised and nothing had ever
    // been in a position to observe.
    if (restartable == SIG_AT_SYSCALL_ENTRY) {
        if (act->flags & SA_RESTART) f.regs[SCHED_TF_RIP] -= SYSCALL_INSN_LEN;
        else f.regs[SCHED_TF_RAX] = (uint64_t)(int64_t)-EINTR;
    } else if (restartable == SIG_BEFORE_REISSUE) {
        // RIP already sits ON the `int $0x80` (the wake put it there).
        // SA_RESTART leaves it to execute; -EINTR must STEP OVER it, or
        // the handler returns straight into the call it was meant to
        // have failed. Either way the window is closed.
        if (!(act->flags & SA_RESTART)) {
            f.regs[SCHED_TF_RIP] += SYSCALL_INSN_LEN;
            f.regs[SCHED_TF_RAX] = (uint64_t)(int64_t)-EINTR;
        }
        scheduler_syscall_entered(pid);
    }

    f.restorer_ret = act->restorer;
    f.blocked      = scheduler_signal_blocked(pid);
    f.sig          = sig;
    f.magic        = SIGFRAME_MAGIC;

    // THE RED ZONE FIRST, then the frame, then the alignment -- in that
    // order, because each one moves the pointer and getting the order
    // wrong silently corrupts a leaf function's locals.
    uint64_t sp = regs[SCHED_TF_RSP] - SIGFRAME_RED_ZONE;
    sp -= sizeof f;

    // SysV says RSP+8 is 16-byte aligned at function entry -- the state
    // a `call` leaves. So the frame's own address must be 8 mod 16, and
    // a handler that gets this wrong faults on the first SSE spill in
    // anything it calls, which points nowhere near here. This kernel has
    // already paid for that once, in crt0.
    sp = (sp & ~15ULL) - 8;

    if (!frame_fits(sp)) {
        klog_printf(KLOG_ERR "signal: pid %d cannot take SIG%s -- no room at rsp 0x%lx\n",
                    pid, signal_name(sig), sp);
        return 0;
    }
    if (!vmm_copy_to_user(vmm_current_pml4(), sp, &f, sizeof f)) return 0;

    // BLOCK THE SIGNAL BEING HANDLED. POSIX's default, and load bearing
    // rather than tidy: without it a signal arriving during its own
    // handler re-enters it, and a 4-page user stack does not survive
    // many rounds of that (kernel/uaddr.h).
    scheduler_signal_set_blocked(pid, f.blocked | (1u << sig));

    regs[SCHED_TF_RSP] = sp;
    regs[SCHED_TF_RIP] = act->handler;
    regs[SCHED_TF_RDI] = (uint64_t)sig; // the handler's one argument
    // DF CLEAR at function entry is part of the same ABI as the stack
    // alignment above: a handler calling memcpy() with it set copies
    // backwards. TF clear so a handler is not entered mid-single-step.
    regs[SCHED_TF_RFLAGS] &= ~0x500ULL;
    return 1;
}

// Restores the frame `push_signal_frame()` built, or returns 0 if there
// is not a valid one to restore. Called only by sys_sigreturn().
int signal_restore_frame(int pid, uint64_t *regs) {
    // The handler was entered with RSP pointing at the frame, and its
    // `ret` popped the restorer address off the front -- so the frame
    // starts one word BELOW where the restorer is now standing. No
    // search and no scan: the restorer is three instructions and pushes
    // nothing (userland/rt/sigtramp.asm), which is what makes this
    // arithmetic rather than a guess.
    uint64_t sp = regs[SCHED_TF_RSP] - 8;
    struct sigframe f;

    if (!frame_fits(sp)) return 0;
    if (!vmm_copy_from_user(vmm_current_pml4(), &f, sp, sizeof f)) return 0;
    if (f.magic != SIGFRAME_MAGIC) return 0;

    // EVERY GENERAL REGISTER, RIP AND RSP COME BACK VERBATIM -- that is
    // the entire job, and a curated subset would be a list to get wrong.
    for (int i = 0; i <= SCHED_TF_RAX; i++) regs[i] = f.regs[i];
    regs[SCHED_TF_RIP] = f.regs[SCHED_TF_RIP];
    regs[SCHED_TF_RSP] = f.regs[SCHED_TF_RSP];

    // AND THESE THREE DO NOT, because this frame lives on the USER
    // STACK and a program can scribble it. Reimposing the selectors and
    // masking RFLAGS means the worst a corrupted frame can do is fault
    // in ring 3, which is where a program's mistakes belong -- rather
    // than resume with a ring-0 CS, IOPL 3, or interrupts disabled.
    regs[SCHED_TF_CS]     = SEL_USER_CODE;
    regs[SCHED_TF_SS]     = SEL_USER_DATA;
    regs[SCHED_TF_RFLAGS] = (f.regs[SCHED_TF_RFLAGS] & RFLAGS_USER_MASK) |
                            RFLAGS_FORCED;

    // scheduler_signal_set_blocked() forces SIGKILL and SIGSTOP out of
    // whatever this says, so a frame edited to make the process
    // unkillable does not.
    scheduler_signal_set_blocked(pid, f.blocked);
    return 1;
}

// --- acting on a signal -----------------------------------------------

// The default action, once it is known that nothing else applies.
// Terminates `pid`, from whichever of the two situations it is in.
// The signals whose default action DUMPS CORE on Linux (signal(7)'s
// "Core"). A process killed by one gets the crash report a fault would
// have written, so `kill -SEGV` and abort() leave the same evidence.
int signal_dumps_core(int sig) {
    return sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGABRT;
}

static void do_default_action(int pid, int sig, uint64_t *regs) {
    // INIT CANNOT DIE OF A DEFAULT ACTION, and the guard has to be here
    // as well as in scheduler_kill(): when the victim is the RUNNING
    // process the branch below takes SYS_EXIT's path instead, which
    // never asks. That is Linux's SIGNAL_UNKILLABLE, and it keeps the
    // same split -- init still CATCHES anything it installs a handler
    // for (delivery has already run one by the time this is reached);
    // what it cannot do is die of a signal it has not.
    if (pid == scheduler_init_pid()) {
        klog_printf("signal: init (pid %d) discarded SIG%s -- no handler\n",
                    pid, signal_name(sig));
        return;
    }

    int code = SIGNAL_EXIT_BASE + sig;
    klog_printf("signal: pid %d terminated by SIG%s\n", pid, signal_name(sig));
    // Only for the RUNNING process: the report reads its address space
    // and its trap frame, which a process the scheduler switched away
    // from does not have to hand.
    if (regs && signal_dumps_core(sig) && pid == scheduler_current_pid()) {
        char what[32];
        k_snprintf(what, sizeof what, "Killed by SIG%s", signal_name(sig));
        crash_report_write(what, regs, 0, 0);
    }

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

int signal_deliver_pending(int pid, uint64_t *regs, int at_syscall_entry) {
    // The common case, and it must stay this cheap: this runs at the end
    // of every trap taken from ring 3 -- every syscall and every timer
    // tick of every process.
    //
    // TAKEN BEFORE THE SIGSUSPEND MASK IS UNWOUND, because what is
    // deliverable is decided against the mask the caller asked to wait
    // under. Unwinding first would hide the very signal that ended the
    // wait.
    int sig = scheduler_signal_take(pid);

    // A SIGSUSPEND THAT GOT THIS FAR IS OVER. Reaching here means this
    // process is the one about to run, and a process inside sigsuspend
    // is parked -- so the only way to be armed AND current is to have
    // just been woken.
    //
    // The mask goes back in TWO STEPS, which is Linux's saved_sigmask
    // and is not tidiness: push_signal_frame() records the CURRENT mask
    // as the one sigreturn restores, so the pre-suspend mask is
    // installed across that call and the handler's own mask is put back
    // after it. The handler therefore runs under what the caller asked
    // to suspend with, and the mask it had before the call is restored
    // when the handler returns -- which is what POSIX describes.
    uint32_t saved = 0, suspend_mask = 0;
    int was_suspended = scheduler_sigsuspend_take(pid, &saved);
    if (was_suspended) {
        suspend_mask = scheduler_signal_blocked(pid);
        scheduler_signal_set_blocked(pid, saved);
    }

    if (!sig) return 0; // the wait ended with nothing to act on; mask is back

    struct k_sigaction act;
    if (!scheduler_signal_action(pid, sig, &act)) return 0;

    // A HANDLER CAN ONLY BE GIVEN TO THE PROCESS THAT IS ABOUT TO RUN,
    // because building the frame writes its user stack and this code
    // stands in whichever address space the CPU is in. When the
    // scheduler switched away mid-trap the target is somebody else, so
    // the bit goes back and the signal is delivered at that process's
    // OWN next trap -- which it always reaches, exactly as the pending
    // set was designed for. Killing does not need this because
    // scheduler_kill() never touches the victim's address space.
    if (SIG_IS_HANDLER(act.handler)) {
        if (pid != scheduler_current_pid()) {
            scheduler_signal_raise(pid, sig);
            return 0;
        }
        if (push_signal_frame(pid, sig, &act, regs, at_syscall_entry)) {
            // The frame now carries `saved` as its restore-on-sigreturn
            // mask; give the HANDLER the one the caller suspended under.
            if (was_suspended)
                scheduler_signal_set_blocked(pid, suspend_mask | (1u << sig));
            return 1;
        }
        // Fell through: no room for a frame. The process is told the
        // only way left.
    }

    do_default_action(pid, sig, regs);
    return 1;
}

int signal_deliver_fault(int pid, int sig, uint64_t *regs) {
    if (pid <= 0 || pid != scheduler_current_pid()) return 0;

    struct k_sigaction act;
    if (!scheduler_signal_action(pid, sig, &act)) return 0;
    if (!SIG_IS_HANDLER(act.handler)) return 0;

    // A FAULT INSIDE THE FAULT'S OWN HANDLER IS THE END OF THE LINE, and
    // the blocked mask already knows: push_signal_frame() set this bit
    // on the way in, so finding it set means the handler faulted. Linux
    // calls this force_sig and resets the disposition to SIG_DFL; the
    // effect here is the same and the reasoning is the one that matters
    // -- re-entering a handler that just faulted produces a loop that
    // ends in a stack overflow rather than a diagnosis.
    if (scheduler_signal_blocked(pid) & (1u << sig)) {
        klog_printf("signal: pid %d faulted inside its own SIG%s handler\n",
                    pid, signal_name(sig));
        return 0;
    }

    // NOT RESTARTABLE, and the distinction is the point: a fault is the
    // instruction itself failing, so there is no call to re-run and
    // rewinding RIP would re-execute the faulting instruction forever.
    // The frame puts the process back exactly where it faulted, which is
    // what lets a handler that FIXES the cause simply return.
    if (!push_signal_frame(pid, sig, &act, regs, 0)) return 0;

    klog_printf("signal: pid %d took SIG%s at rip 0x%lx -- handler at 0x%lx\n",
                pid, signal_name(sig), regs[SCHED_TF_RIP], act.handler);
    return 1;
}
