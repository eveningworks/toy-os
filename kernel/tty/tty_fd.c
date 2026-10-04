// Terminals as descriptors: the machine console, both ends of a pty, and
// a terminal named by index (the serial debug console's). The fd_ops
// syscall_fd.c dispatches through for each; the terminal objects are
// tty.c's and pty.c's.
#include "syscalls.h"
#include "syscall_abi.h"
#include "klog.h"
#include "heap.h"
#include "vga.h"
#include "keyboard.h"
#include "vmm.h"
#include "pty.h"
#include "tty.h"

// Which address space holds the console claim, so tty_fd_process_gone()
// can tell "this process is dying" from "some other process is". Keyed
// by CR3 like the descriptor tables.
static uint64_t g_console_owner_pml4;

static int console_read(uint64_t *regs, uint64_t pml4, uint64_t buf_ptr, uint64_t len);

// --- pseudo-terminals ------------------------------------------------
//
// Four paths, because the two ENDS do opposite things: a write to the
// master is INPUT (it goes through the line discipline as if typed) and
// a write to the slave is OUTPUT. Each pairs a non-blocking call in
// pty.c/tty.c with the same check-and-park dance pipe.c's ops use, and
// for the same reason -- the kernel is preemptible, so "empty -> park"
// must be atomic against the other end or the wake fires with nobody
// parked yet and is LOST.

static int pty_master_fd_read(struct syscall_ctx *c, struct open_file *f,
                              uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int idx = f->pty.idx, nonblock = f->nonblock;
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }

    scheduler_preempt_disable();
    int64_t n = pty_master_read(idx, kbuf, (uint32_t)len);
    int blocked = 0;
    if (n >= 0 && !vmm_copy_to_user(pml4, buf_ptr, kbuf, (uint64_t)n)) {
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (n >= 0) {
        regs[14] = (uint64_t)n; // bytes, or 0 for EOF (no slave left)
    } else if (nonblock) {
        // ASKED NOT TO BLOCK. -EAGAIN is not "no more data" -- 0 already
        // means that, and a terminal emulator told the wrong one would
        // decide its shell had exited.
        regs[14] = (uint64_t)(int64_t)-EAGAIN;
    } else if (!scheduler_block_current(regs, pty_out_wait_chan(idx), SCHED_WAIT_TTY)) {
        regs[14] = 0; // nowhere to park -- EOF beats spinning
    } else {
        blocked = 1;
    }
    scheduler_preempt_enable();
    kfree(kbuf);
    return blocked;
}

// Is the terminal behind `f` gone for good? A pty slave's is when its
// master has closed -- a read then answers end of file; a tty_fd_ops
// one's when its session has been hung up -- it then reads the console.
static int tty_desc_hung_up(const struct open_file *f) {
    if (f->ops == &pty_slave_fd_ops) return !pty_master_open(f->pty.idx);
    return tty_generation(tty_at(f->tty.idx)) != f->tty.gen;
}

// A read from a terminal that is not the console: a pty slave, or a
// terminal named by index (tty_fd_ops). One path, because everything
// that differs between them is whether the far end is still there.
static int tty_fd_read(struct syscall_ctx *c, struct open_file *f,
                       uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int nonblock = f->nonblock;
    struct tty *t = f->ops == &pty_slave_fd_ops ? pty_tty(f->pty.idx) : tty_at(f->tty.idx);
    if (!t) { regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }
    // HUNG UP BEFORE ANYTHING IS TAKEN: an old session's reader must not
    // claim the terminal or eat the input that is now somebody else's.
    // It reads the MACHINE CONSOLE instead -- what it had before this
    // terminal existed, as its writes do (tty_fd_write()), so a shell
    // `sh spawn` started keeps working on the keyboard.
    if (f->ops == &tty_fd_ops && tty_desc_hung_up(f))
        return console_read(regs, pml4, buf_ptr, len);

    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }

    // CLAIMED ON THE FIRST READ, which is the physical console's rule
    // in console_read() below. The process that OPENED the pty is a
    // terminal emulator and never reads the slave; the one that reads it
    // is the shell, and the shell is who needs to move the foreground
    // group. Claiming at open instead made tcsetpgrp() answer -EPERM to
    // the only process that had any business calling it.
    //
    // UNDER THE GUARD, together with the read itself: the hung-up check
    // above could be passed, the caller preempted, and the session hung
    // up before it continued -- and a leftover reader would then claim
    // the terminal and take the line typed for the NEXT command.
    scheduler_preempt_disable();
    if (f->ops == &tty_fd_ops && tty_desc_hung_up(f)) {
        scheduler_preempt_enable();
        kfree(kbuf);
        return console_read(regs, pml4, buf_ptr, len);
    }
    if (!tty_owner(t)) tty_set_owner(t, scheduler_current_tgid());

    // The same rule as the physical console one screen up, and it has to
    // be both: a Terminal window is a terminal, so a `&` job started in
    // one competes for its keyboard exactly as it would for tty0's.
    {
        int bg = tty_check_background_read(t);
        if (bg) {
            scheduler_preempt_enable();
            regs[14] = (uint64_t)(int64_t)(bg > 0 ? SYS_RETRY : bg);
            kfree(kbuf);
            return 0;
        }
    }

    unsigned n = tty_read(t, kbuf, (unsigned)len);
    int blocked = 0;
    if (n && !vmm_copy_to_user(pml4, buf_ptr, kbuf, (uint64_t)n)) {
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (n) {
        regs[14] = (uint64_t)n;
    } else if (tty_eof_pending(t)) {
        // Ctrl-D on an empty line. A zero-length read, which is what END
        // OF INPUT means to a program -- and the only reason `cat` with
        // no arguments can ever finish.
        regs[14] = 0;
    } else if (nonblock && !tty_desc_hung_up(f)) {
        regs[14] = (uint64_t)(int64_t)-EAGAIN;
    } else if (tty_desc_hung_up(f)) {
        // END OF FILE, and this is the one case that distinguishes a pty
        // slave from the physical console: a console has no end of input
        // because the keyboard is always there, but a terminal WINDOW
        // can be closed, and a shell inside it has to be told rather
        // than parked forever on a master that will never write again.
        regs[14] = 0;
    } else if (!scheduler_block_current(regs, tty_wait_chan(t), SCHED_WAIT_KEY)) {
        // Nowhere to park: the legacy `run` loader has no slot. A pty
        // slave says EOF; the serial terminal says "not yet", as the
        // console does, because its input is still coming.
        regs[14] = f->ops == &tty_fd_ops ? (uint64_t)SYS_RETRY : 0;
    } else {
        blocked = 1;
    }
    scheduler_preempt_enable();
    kfree(kbuf);
    return blocked;
}

static int pty_master_fd_write(struct syscall_ctx *c, struct open_file *f,
                               uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int idx = f->pty.idx;
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        kfree(kbuf);
        return 0;
    }
    // NEVER BLOCKS. This is input, and input is dropped when the queue
    // is full exactly as a keystroke at the physical keyboard is -- a
    // terminal emulator that parked here would stop painting while its
    // shell was busy, which is the opposite of what a terminal is for.
    int64_t n = pty_master_write(idx, kbuf, (uint32_t)len);
    regs[14] = (uint64_t)n; // len, or 0 when no terminal is left
    kfree(kbuf);
    return 0;
}

static int pty_slave_fd_write(struct syscall_ctx *c, struct open_file *f,
                              uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int idx = f->pty.idx;
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        kfree(kbuf);
        return 0;
    }

    scheduler_preempt_disable();
    int64_t n = pty_slave_write(idx, kbuf, (uint32_t)len);
    int blocked = 0;
    if (n >= 0) {
        regs[14] = (uint64_t)n; // len, or 0 when no master remains
    } else if (!scheduler_block_current(regs, pty_out_wait_chan(idx), SCHED_WAIT_TTY)) {
        regs[14] = 0;
    } else {
        blocked = 1;
    }
    scheduler_preempt_enable();
    kfree(kbuf);
    return blocked;
}

// A program's output to a terminal named by index. Never parks: the
// serial driver queues and drops rather than waiting (serial.c's trap).
//
// **A HUNG-UP SESSION'S DESCRIPTOR BECOMES A CONSOLE ONE**, not an
// error: writes go to the machine console, reads come from it, and
// fd_tty() names tty0. That is what a job left running by a debug-
// console command had before this terminal existed, and it keeps such a
// job's output out of the NEXT command's reply -- the torn-reply problem
// the port split removed. Linux answers EIO here; tools/ansi_cursor_test.py
// screenshots such a job's VGA output, and tools/stdin_test.py types at
// a tosh started this way.
static int tty_fd_write(struct syscall_ctx *c, struct open_file *f,
                        uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        kfree(kbuf);
        return 0;
    }
    struct tty *t = tty_desc_hung_up(f) ? tty_console() : tty_at(f->tty.idx);
    tty_output(t ? t : tty_console(), kbuf, (unsigned)len);
    regs[14] = len;
    kfree(kbuf);
    return 0;
}

// READING THE PHYSICAL CONSOLE FROM RING 3 -- fd 0.
//
// The counterpart of console_fd_write() below, and the reason
// /bin/tosh can exist: a ring-3 shell needs somewhere to read a line
// from, and until this there was nowhere: the non-blocking key read
// that preceded it (SYS_READ_KEY, retired) made a program wanting a
// keystroke spin.
//
// Blocking here is safe for the reason the pipe path above is safe: the
// handler does not WAIT, it PARKS and returns. keyboard.c's ring_push()
// is the wake site.
//
// THREE THINGS THIS DELIBERATELY DOES NOT DO, all of them the TTY
// milestone's job (docs/roadmap.md):
//   - No line discipline. Bytes arrive as typed; there is no cooked
//     mode, no editing and no echo, so the reader echoes what it reads.
//     That is a program in raw mode, which is what a shell wanting its
//     own editor asks for anyway.
//   - No translation here. The terminal below already made every key
//     bytes -- a character is one Latin-1 byte, a special key an ANSI
//     sequence (tty_input(), api/termkey.h).
//   - No per-terminal queue. There is ONE console, so the first ring-3
//     reader claims it (keyboard.h) and the kernel shell stands down
//     until that process dies.
static int console_read(uint64_t *regs, uint64_t pml4, uint64_t buf_ptr, uint64_t len) {
    if (len == 0) { regs[14] = 0; return 0; }

    // A DESKTOP OWNING THE SCREEN OWNS THE KEYBOARD WITH IT. Park
    // without popping anything and without claiming: win_input.c drains
    // this same ring to feed the compositor, so taking a key here would
    // make keystrokes vanish from the desktop at random. The reader
    // simply gets nothing until the desktop exits, which is exactly what
    // the kernel shell behind the desktop already does.
    if (keyboard_compositor_owns()) {
        if (scheduler_block_current(regs, tty_wait_chan(tty_console()), SCHED_WAIT_KEY)) return 1;
        regs[14] = (uint64_t)SYS_RETRY;
        return 0;
    }

    // Claiming on the FIRST read rather than at spawn: a process that
    // never reads the console must not silence the kernel shell, and
    // there is no other moment the kernel could learn the difference.
    //
    // **AND ONLY ON THE FIRST, which this used to say and not do.** The
    // claim ran on EVERY read, so tty_set_console_owner() re-pointed the
    // owner and the FOREGROUND GROUP at whoever had just called -- which
    // was invisible while a shell was the only thing that ever read the
    // console, and is a keystroke thief the moment a second process
    // does: a background job's first read made it the foreground group,
    // so the next key went to it instead of the prompt. Found by `cat &`
    // eating the `j` of the command typed after it.
    //
    // A DEAD OWNER IS NOT AN OWNER. tty_fd_process_gone() clears the claim
    // when the owning address space goes, so this normally only sees an
    // empty console -- but a claim by a pid whose slot has since been
    // reused would be worse than none, so it is checked rather than
    // assumed.
    int console_owner = tty_console_owner();
    if (!console_owner || !scheduler_pid_alive(console_owner)) {
        keyboard_claim_console(1);
        g_console_owner_pml4 = pml4;
        // ...and the console gains an OWNER and a foreground group,
        // which is what makes Ctrl-C mean anything (kernel/tty.h).
        tty_set_console_owner(scheduler_current_tgid());
    }

    // **A BACKGROUND READER IS STOPPED, NOT SERVED.** Two processes
    // reading one keyboard is not untidy -- which of them gets a given
    // key is a race, so a `&` job that reads would take keystrokes out
    // of the shell's prompt at random. See tty_check_background_read().
    {
        int bg = tty_check_background_read(tty_console());
        if (bg) {
            // SYS_RETRY rather than parking: the caller is suspended
            // now, so it re-enters this read when somebody continues
            // it -- by which time it may be the foreground group and
            // the read simply works. A negative answer is -EIO, for a
            // process that ignores SIGTTIN and so cannot be stopped.
            regs[14] = (uint64_t)(int64_t)(bg > 0 ? SYS_RETRY : bg);
            return 0;
        }
    }

    // Whoever waits for a key is also who flushes the screen -- the
    // console draws into a back buffer and the ring-0 reader's idle loop
    // is what normally presents it (vga.h). That loop is now suspended
    // on this reader's behalf, so this is the moment "output is
    // finished, we are waiting for a human" is true. Cheap when nothing
    // changed.
    vga_present();

    char kbuf[SYS_WRITE_MAX < 64 ? SYS_WRITE_MAX : 64];
    uint64_t want = len < sizeof kbuf ? len : sizeof kbuf;
    // THE TERMINAL DECIDES WHAT IS READABLE, not this function. In raw
    // mode that is every byte that has arrived; in canonical mode it is
    // whole lines only, and a half-typed one reads as nothing -- which
    // is why this asks the terminal rather than draining a ring.
    uint64_t got = tty_read(tty_console(), kbuf, (unsigned)want);

    if (got == 0) {
        // Nothing queued. Park rather than report EOF: a console has no
        // end of file, and 0 would tell a shell its input had closed.
        if (scheduler_block_current(regs, tty_wait_chan(tty_console()), SCHED_WAIT_KEY)) return 1;
        // Nowhere to park -- kernel code, or the legacy loader's single
        // slot. Answer "nothing yet" the only way a non-blocking caller
        // can be answered, rather than lying about end of input.
        regs[14] = (uint64_t)SYS_RETRY;
        return 0;
    }

    if (!vmm_copy_to_user(pml4, buf_ptr, kbuf, got)) {
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        regs[14] = got;
    }
    return 0;
}

// The PHYSICAL console, not whatever sink the kernel shell has installed
// for its own output (vga.h's vga_putc_console()).
static int console_fd_write(struct syscall_ctx *c, struct open_file *f,
                            uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    (void)f;
    // len is capped at SYS_WRITE_MAX by the caller, so the
    // bounce buffer is always big enough. Copying first (rather
    // than reading through the user pointer as this used to) is
    // what SMAP requires -- see vmm.h.
    // From the HEAP, not the stack: a KiB of bounce buffer at the top of
    // a syscall is a KiB of a 16 KiB per-process kernel stack, and this
    // one sits above pipe_write() and the console. A write already
    // costs far more than an allocation. Same call as the one
    // etc_config_file.c makes, and for the same reason.
    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        for (uint64_t i = 0; i < len; i++) vga_putc_console(((const char *)kbuf)[i]);
        regs[14] = len; // bytes written, back via RAX
    }
    kfree(kbuf);
    return 0;
}

static int console_fd_read(struct syscall_ctx *c, struct open_file *f,
                           uint64_t buf_ptr, uint64_t len) {
    (void)f;
    return console_read(c->regs, c->pml4, buf_ptr, len);
}

static struct tty *console_fd_tty(struct open_file *f) { (void)f; return tty_console(); }
static struct tty *pty_fd_tty(struct open_file *f) { return pty_tty(f->pty.idx); }
// HUNG UP: no terminal at all for termios and job control, as Linux's
// EIO. Reads and writes fall back to the console, but a leftover job
// re-applying the serial line's saved settings must not land them on
// tty0 -- its erase character is DEL, not '\b'.
static struct tty *tty_fd_tty(struct open_file *f) {
    return tty_desc_hung_up(f) ? NULL : tty_at(f->tty.idx);
}

static void pty_fd_open(struct open_file *f, int idx) { f->pty.idx = idx; }
static void tty_fd_open(struct open_file *f, int idx) {
    f->tty.idx = idx;
    f->tty.gen = tty_generation(tty_at(idx));
}
// A pty end. The LAST close of one end wakes anything parked on the
// other, which is what makes end-of-file arrive rather than being waited
// for forever; pty.c owns that rule, as pipe.c does for a pipe.
static void pty_master_fd_release(struct open_file *f) { pty_close_master(f->pty.idx); }
static void pty_slave_fd_release(struct open_file *f)  { pty_close_slave(f->pty.idx); }

// The physical console. Reads block, claim the keyboard and never report
// EOF -- see console_read().
const struct fd_ops console_fd_ops = {
    .name = "console",
    .read = console_fd_read, .write = console_fd_write,
    .tty = console_fd_tty,
};
// What the terminal has emitted -- a program's output and the
// discipline's echo, in order -- and, written, what is typed AT it.
const struct fd_ops pty_master_fd_ops = {
    .name = "pty master",
    .read = pty_master_fd_read, .write = pty_master_fd_write,
    .release = pty_master_fd_release, .open = pty_fd_open,
    .tty = pty_fd_tty,
    .flags = FD_OPS_WRITE_IS_INPUT,
};
// What was typed at it -- whole lines in canonical mode -- and a
// program's output, which CAN park when the master is behind.
const struct fd_ops pty_slave_fd_ops = {
    .name = "pty slave",
    .read = tty_fd_read, .write = pty_slave_fd_write,
    .release = pty_slave_fd_release, .open = pty_fd_open,
    .tty = pty_fd_tty,
};
// A terminal by TTY INDEX -- the serial debug console's. Same rules as a
// slave; a hung-up session reads and writes the machine console.
const struct fd_ops tty_fd_ops = {
    .name = "tty",
    .read = tty_fd_read, .write = tty_fd_write,
    .open = tty_fd_open,
    .tty = tty_fd_tty,
};

// The console claim is dropped with the address space that made it.
void tty_fd_process_gone(uint64_t pml4_phys) {
    // The console claim is not an fd, but it is the same kind of thing:
    // a kernel resource keyed by address space that nothing else would
    // reclaim. Releasing it here is what brings the kernel shell's
    // prompt back when a ring-3 shell exits OR crashes -- both reach
    // this hook, which is why the claim is not tracked in the reader.
    if (g_console_owner_pml4 == pml4_phys) {
        g_console_owner_pml4 = 0;
        keyboard_claim_console(0);
        // The foreground group goes with it. A dead owner's group left
        // in front would point the next Ctrl-C at whatever reused those
        // slots.
        tty_set_console_owner(0);
    }
}

void tty_fd_rekey(uint64_t old_pml4, uint64_t new_pml4) {
    if (g_console_owner_pml4 == old_pml4) g_console_owner_pml4 = new_pml4;
}
