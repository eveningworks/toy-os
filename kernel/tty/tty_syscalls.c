// The terminal syscalls: make a pty, read and write its behaviour.
//
// Here rather than in kernel/proc/ because the subsystem that owns a
// facility owns its handlers -- the shape kernel/include/kernel/
// syscalls.h describes and syscall_table.c assembles. What is NOT here
// is reading and writing a pty, which is syscall_fd.c's: those are
// routed by fd KIND alongside files and pipes, and splitting them out
// would put half of "what does an fd do" in each of two files.
#include "syscalls.h"
#include "syscall_abi.h"
#include "tty.h"
#include "pty.h"
#include "errno.h"
#include "klog.h"
#include "scheduler.h"
#include "vmm.h"

// **ONE OWNERSHIP CHECK, IN tty.c, AND NOT A SECOND COPY HERE.** Who
// may move a foreground group is the terminal's rule
// (tty_set_fg_pgid()); repeating it at the syscall boundary is the kind
// of duplication that drifts -- CLAUDE.md's note on the context menu
// re-implementing wm_request_close() is the same mistake.

int sys_openpty(struct syscall_ctx *c) {
    struct openpty_msg msg;

    int idx = pty_create(scheduler_current_pid());
    if (idx < 0) {
        klog_write("syscall: openpty() -- no free terminal\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOSPC;
        return 0;
    }

    int mdesc = fd_desc_alloc(FD_KIND_TTY_MASTER, idx);
    int sdesc = mdesc >= 0 ? fd_desc_alloc(FD_KIND_TTY_SLAVE, idx) : -1;
    int mfd = mdesc >= 0 ? fd_install(c->pml4, mdesc) : -1;
    int sfd = sdesc >= 0 ? fd_install(c->pml4, sdesc) : -1;

    // **UNWIND COMPLETELY OR NOT AT ALL.** A half-made pty is worse than
    // none: the caller gets an error and one live descriptor it does not
    // know about, and the terminal slot is never freed. Each cleanup
    // below goes through the same fd_desc_unref()/fd_close() path an
    // ordinary close does, so the pty's own counts are decremented by
    // the code that always decrements them rather than by a second copy
    // here.
    if (mfd < 0 || sfd < 0) {
        if (sfd >= 0) fd_close(c->pml4, sfd); else if (sdesc >= 0) fd_desc_unref(sdesc);
        if (mfd >= 0) fd_close(c->pml4, mfd); else if (mdesc >= 0) fd_desc_unref(mdesc);
        klog_write("syscall: openpty() -- no free descriptor\n");
        c->regs[14] = (uint64_t)(int64_t)-EMFILE;
        return 0;
    }

    msg.master_fd = (int32_t)mfd;
    msg.slave_fd  = (int32_t)sfd;
    if (!vmm_copy_to_user(c->pml4, c->a0, &msg, sizeof msg)) {
        fd_close(c->pml4, sfd);
        fd_close(c->pml4, mfd);
        klog_write("syscall: openpty() rejected -- invalid pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}

int sys_tcgetattr(struct syscall_ctx *c) {
    struct tty *t = fd_tty(c->pml4, (int)(int32_t)c->a0);
    if (!t) { c->regs[14] = (uint64_t)(int64_t)-ENOTTY; return 0; }

    struct tty_termios tio;
    tty_get_termios(t, &tio);
    if (!vmm_copy_to_user(c->pml4, c->a1, &tio, sizeof tio)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}

int sys_tcsetattr(struct syscall_ctx *c) {
    struct tty *t = fd_tty(c->pml4, (int)(int32_t)c->a0);
    if (!t) { c->regs[14] = (uint64_t)(int64_t)-ENOTTY; return 0; }

    struct tty_termios tio;
    if (!vmm_copy_from_user(c->pml4, &tio, c->a1, sizeof tio)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // **NO PRIVILEGE CHECK, DELIBERATELY, AND IT IS NOT AN OVERSIGHT.**
    // A process that holds an fd for a terminal may change how that
    // terminal behaves -- which is POSIX's rule too (tcsetattr needs the
    // fd, not ownership). Holding the fd IS the capability here: fds are
    // per address space, and nothing hands one out that did not ask.
    // Moving a FOREGROUND GROUP is different and does check, because
    // that decides where somebody else's Ctrl-C lands.
    tty_set_termios(t, &tio);
    c->regs[14] = 0;
    return 0;
}
