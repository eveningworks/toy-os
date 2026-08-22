// The file-descriptor namespace, and every syscall that operates on one.
//
// One table for files, sockets and pipe ends, as in real Unix rather
// than a parallel table per kind: SYS_CLOSE and fd_release_all() only
// ever look at `used` and `owner_pml4`, so a new kind costs them
// nothing. Each entry records which process owns it BY CR3, which is
// what stops one process reading, writing or closing another's fds --
// the same isolation trick the legacy heap and window state use.
//
// SYS_WRITE and SYS_READ live here rather than with the filesystem
// because what they mostly do is ROUTE: a write goes to the console,
// to a pipe, or to a file depending only on the fd. The file-shaped
// ends of each are the two sys_do_*_file() helpers below.
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "heap.h"  // bounce buffers come from here, not the stack
#include "vga.h"
#include "keyboard.h"
#include "vmm.h"
#include "scheduler.h"
#include "pipe.h"
#include "tty.h" // the console's owner and foreground group
#include "fs.h"
#include "string.h"
#include <stddef.h>

// The table itself. Its TYPES are in syscalls.h, because SYS_OPEN
// (kernel/fs/fs_syscalls.c) and SYS_SPAWN (proc_syscalls.c) index it
// too; the storage is here, with the code that owns it.

struct open_file fd_desc[FD_DESC_MAX];

// One descriptor table per ADDRESS SPACE. A flat array scanned
// linearly: FD_SPACE_MAX is small, this is touched once per fd
// operation rather than per byte, and a hash would be a data structure
// pretending to be a design at this size.
struct fd_space {
    uint64_t pml4;      // 0 = free slot
    short    d[FD_MAX]; // d[fd] = index into fd_desc, or -1
};
static struct fd_space g_spaces[FD_SPACE_MAX];

// Which address space holds the console claim, so fd_release_all() can
// tell "this process is dying" from "some other process is". Keyed by
// CR3 like everything else here.
static uint64_t g_console_owner_pml4;

int fd_desc_alloc(enum fd_kind kind, int pipe_idx) {
    for (int i = 0; i < FD_DESC_MAX; i++) {
        if (fd_desc[i].refs) continue;
        fd_desc[i].refs = 1;
        fd_desc[i].kind = kind;
        if (kind == FD_KIND_PIPE_R || kind == FD_KIND_PIPE_W)
            fd_desc[i].pipe.idx = pipe_idx;
        return i;
    }
    return -1;
}

// The one place that decides a stream is really gone. Everything that
// closes an fd -- SYS_CLOSE, dup2 over an open descriptor, a process
// dying -- lands here, so a pipe end cannot be closed twice or leaked
// depending on which path got there.
void fd_desc_unref(int di) {
    if (di < 0 || di >= FD_DESC_MAX) return;
    struct open_file *f = &fd_desc[di];
    if (f->refs <= 0) return;
    if (--f->refs > 0) return; // somebody else still names it

    switch (f->kind) {
    case FD_KIND_PIPE_R: pipe_close_reader(f->pipe.idx); break;
    case FD_KIND_PIPE_W: pipe_close_writer(f->pipe.idx); break;
    default: break; // a file needs nothing: fs.c holds no per-open state
    }
    f->kind = FD_KIND_FILE;
}

static struct fd_space *space_find(uint64_t pml4) {
    for (int i = 0; i < FD_SPACE_MAX; i++)
        if (g_spaces[i].pml4 == pml4) return &g_spaces[i];
    return NULL;
}

int fd_space_open(uint64_t pml4) {
    if (!pml4) return -1;
    if (space_find(pml4)) return 0; // idempotent

    struct fd_space *sp = NULL;
    for (int i = 0; i < FD_SPACE_MAX; i++)
        if (!g_spaces[i].pml4) { sp = &g_spaces[i]; break; }
    if (!sp) {
        klog_write("fd: no free descriptor table -- too many address spaces\n");
        return -1;
    }

    sp->pml4 = pml4;
    for (int i = 0; i < FD_MAX; i++) sp->d[i] = -1;

    // The three a process starts with. They are descriptions like any
    // other, so a later dup2 can move them.
    int con = fd_desc_alloc(FD_KIND_CONSOLE, -1);
    int err = fd_desc_alloc(FD_KIND_KLOG, -1);
    if (con < 0 || err < 0) {
        if (con >= 0) fd_desc_unref(con);
        if (err >= 0) fd_desc_unref(err);
        sp->pml4 = 0;
        return -1;
    }
    fd_desc[con].refs++; // named twice: stdin and stdout
    sp->d[FD_STDIN]  = (short)con;
    sp->d[FD_STDOUT] = (short)con;
    sp->d[FD_STDERR] = (short)err;
    return 0;
}

// Every lookup goes through here, so a process that has not touched an
// fd yet still gets its standard three rather than an empty table --
// which matters because nothing calls fd_space_open() at spawn for the
// legacy loader.
static struct fd_space *space_get(uint64_t pml4) {
    struct fd_space *sp = space_find(pml4);
    if (sp) return sp;
    if (fd_space_open(pml4) < 0) return NULL;
    return space_find(pml4);
}

int fd_install(uint64_t pml4, int di) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || di < 0) return -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (sp->d[i] >= 0) continue;
        sp->d[i] = (short)di;
        return i;
    }
    return -1;
}

int fd_desc_index(uint64_t pml4, int fd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || fd < 0 || fd >= FD_MAX) return -1;
    int di = sp->d[fd];
    if (di < 0 || !fd_desc[di].refs) return -1;
    return di;
}

struct open_file *fd_get(uint64_t pml4, int fd) {
    int di = fd_desc_index(pml4, fd);
    return di < 0 ? NULL : &fd_desc[di];
}

int fd_close(uint64_t pml4, int fd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || fd < 0 || fd >= FD_MAX) return -1;
    int di = sp->d[fd];
    if (di < 0) return -1;
    sp->d[fd] = -1;
    fd_desc_unref(di);
    return 0;
}

int fd_dup2(uint64_t pml4, int oldfd, int newfd) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || newfd < 0 || newfd >= FD_MAX) return -1;
    int di = fd_desc_index(pml4, oldfd);
    if (di < 0) return -1;
    // POSIX: dup2(fd, fd) is a no-op and specifically does NOT close.
    // Getting that wrong destroys the stream it was asked to preserve.
    if (oldfd == newfd) return newfd;
    if (sp->d[newfd] >= 0) fd_desc_unref(sp->d[newfd]);
    fd_desc[di].refs++;
    sp->d[newfd] = (short)di;
    return newfd;
}

int fd_set_desc(uint64_t pml4, int fd, int di) {
    struct fd_space *sp = space_get(pml4);
    if (!sp || fd < 0 || fd >= FD_MAX) return -1;
    if (di < 0 || di >= FD_DESC_MAX || !fd_desc[di].refs) return -1;
    if (sp->d[fd] == di) return fd; // already there -- do not unref it
    if (sp->d[fd] >= 0) fd_desc_unref(sp->d[fd]);
    fd_desc[di].refs++;
    sp->d[fd] = (short)di;
    return fd;
}

void fd_inherit(uint64_t child, uint64_t parent) {
    if (!child) return;
    struct fd_space *ps = parent ? space_find(parent) : NULL;
    if (fd_space_open(child) < 0) return;
    if (!ps) return; // kernel-spawned: the standard three are right

    struct fd_space *cs = space_find(child);
    if (!cs) return;

    // ONLY THE STANDARD THREE, and this is the whole design rather than
    // a simplification.
    //
    // Unix hands a child everything not marked close-on-exec, and gets
    // away with it because the shell runs code IN the child (between
    // fork and exec) to close the pipe ends it must not keep. There is
    // no fork here and no CLOEXEC, so "inherit the whole table" means
    // every spawn silently hands the child every pipe end the parent
    // happens to hold -- and a pipeline can then NEVER see EOF, because
    // the reading stage is itself a writer of the pipe it is reading.
    // Observed exactly that way: two processes blocked forever, one
    // waiting for data and holding the write end that would have ended
    // it.
    //
    // 0/1/2 is also precisely what posix_spawn() and Windows'
    // STARTUPINFO pass by default, for the same reason: they are the
    // streams a child is *meant* to be given, and everything else is
    // the parent's private business.
    for (int i = 0; i <= FD_STDERR; i++) {
        if (cs->d[i] >= 0) { fd_desc_unref(cs->d[i]); cs->d[i] = -1; }
        int di = ps->d[i];
        if (di < 0 || !fd_desc[di].refs) continue;
        fd_desc[di].refs++;
        cs->d[i] = (short)di;
    }
}

// Returns 1 if the caller was PARKED (its syscall has no return value
// yet -- the wake writes it), 0 otherwise. That is the one thing the
// dispatcher still needs to know, so it is the return value rather than
// another out-parameter.
static __attribute__((noinline)) int
sys_do_read_pipe(uint64_t *regs, uint64_t pml4, int pipe_idx,
                 uint64_t buf_ptr, uint64_t len) {
    // pipe_read() fills a kernel buffer, which is then handed out -- it
    // cannot write into the user pointer itself once SMAP is on
    // (vmm.h). len is capped at SYS_WRITE_MAX by the caller. Heap
    // rather than stack, for the reason sys_do_write_console() gives.
    char *kbuf = kmalloc(SYS_WRITE_MAX);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }

    // CHECK AND PARK MUST BE ATOMIC AGAINST THE OTHER END.
    //
    // The kernel is preemptible, so without this the sequence "pipe is
    // empty -> park" can be interrupted between its two halves by the
    // writer, whose wake then fires while nobody is parked yet and is
    // LOST. That was survivable while only readers parked -- the next
    // write or the EOF woke them. It deadlocks now that a full pipe
    // parks its WRITER too: reader sleeps on an empty check it made
    // before the pipe filled, writer sleeps on a full one, and neither
    // will ever wake the other. Observed exactly that way: two blocked
    // processes and a pipe with data in it.
    scheduler_preempt_disable();
    int64_t n = pipe_read(pipe_idx, kbuf, (uint32_t)len);
    int blocked = 0;
    if (n >= 0 && !vmm_copy_to_user(pml4, buf_ptr, kbuf, (uint64_t)n)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (n >= 0) {
        regs[14] = (uint64_t)n; // bytes, or 0 for EOF
    } else if (!scheduler_block_current(regs, pipe_wait_chan(pipe_idx), SCHED_WAIT_PIPE)) {
        // Nowhere to park (kernel code or the legacy path). Report EOF
        // rather than spinning: a caller that cannot block must not be
        // told "try again forever".
        regs[14] = 0;
    } else {
        blocked = 1;
    }
    scheduler_preempt_enable();
    kfree(kbuf);
    return blocked;
}

// READING THE PHYSICAL CONSOLE FROM RING 3 -- fd 0.
//
// The counterpart of sys_do_write_console() below, and the reason
// /bin/tosh can exist: a ring-3 shell needs somewhere to read a line
// from, and until this there was nowhere. SYS_READ_KEY is the only
// other way in and is non-blocking BY REQUIREMENT (see its handler in
// win_syscalls.c), so a program wanting a keystroke had to spin.
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
//   - No translation. One byte per key, exactly the code
//     keyboard_try_getchar() returns -- every value this driver
//     produces fits in a byte, specials (KEY_ARROW_* et al, 0x91-0xA6)
//     included. An ANSI escape encoding belongs above a real TTY, not
//     baked in here where it could not be turned off.
//   - No per-terminal queue. There is ONE console, so the first ring-3
//     reader claims it (keyboard.h) and the kernel shell stands down
//     until that process dies.
static __attribute__((noinline)) int
sys_do_read_console(uint64_t *regs, uint64_t pml4, uint64_t buf_ptr, uint64_t len) {
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
    keyboard_claim_console(1);
    g_console_owner_pml4 = pml4;
    // ...and the console gains an OWNER and a foreground group, which is
    // what makes Ctrl-C mean anything (kernel/tty.h). Claimed on the
    // first read for the same reason the keyboard is: a process that
    // never reads the console must not take it.
    tty_set_console_owner(scheduler_current_pid());

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
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        regs[14] = got;
    }
    return 0;
}

SYSCALL_HANDLER sys_do_write_console(uint64_t *regs, uint64_t pml4, int kind,
                                      uint64_t buf_ptr, uint64_t len) {
    // len is capped at SYS_WRITE_MAX by the caller, so the
    // bounce buffer is always big enough. Copying first (rather
    // than reading through the user pointer as this used to) is
    // what SMAP requires -- see vmm.h.
    // From the HEAP, not the stack: a KiB of bounce buffer at the top of
    // a syscall is a KiB of a 16 KiB per-process kernel stack, and this
    // one sits above pipe_write() and the console. A write already
    // costs far more than an allocation. Same call as the one
    // etc_config_file.c makes, and for the same reason.
    char *kbuf = kmalloc(SYS_WRITE_MAX);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write("syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        const char *buf = kbuf;
        // KLOG (stderr's default) goes to the KERNEL LOG. It is a
        // separate DESCRIPTION rather than a test on the fd number,
        // which is what lets a process redirect stdout without
        // dragging its diagnostics along: folding the two corrupts
        // whatever the parent was trying to read, and is exactly why
        // Unix has two descriptors rather than one. The kernel log
        // reaches the serial console and `dmesg` no matter where
        // stdout went, so a GUI client with no terminal attached can
        // still say something a test can read.
        if (kind == FD_KIND_KLOG) {
            for (uint64_t i = 0; i < len; i++) klog_putc(buf[i]);
        } else {
            for (uint64_t i = 0; i < len; i++) vga_putc(buf[i]);
        }
        regs[14] = len; // bytes written, back via RAX
    }
    kfree(kbuf);
}

// A write to a pipe WRITE end -- which after this change is simply
// what fd 1 refers to in a process whose parent redirected it. There
// is no per-process "stdout pipe" any more: the redirection lives in
// the descriptor table, where dup2 can also put it.
// Returns 1 if the caller was PARKED, like sys_do_read_pipe() -- a full
// pipe blocks its writer now rather than taking what fits.
static __attribute__((noinline)) int
sys_do_write_pipe(uint64_t *regs, uint64_t pml4, int pipe_idx,
                  uint64_t buf_ptr, uint64_t len) {
    char *kbuf = kmalloc(SYS_WRITE_MAX); // heap -- see sys_do_write_console()
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }

    int blocked = 0;
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write("syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        // Atomic against the reader, for the reason sys_do_read_pipe()
        // gives above: a wake that fires between "it is full" and
        // "park" is lost, and with both ends able to sleep that is a
        // deadlock rather than a delay.
        scheduler_preempt_disable();
        int64_t n = pipe_write(pipe_idx, kbuf, (uint32_t)len);
        if (n >= 0) {
            regs[14] = (uint64_t)n; // all of it, or 0 for "no readers left"
        } else if (!scheduler_block_current(regs, pipe_wait_chan(pipe_idx), SCHED_WAIT_PIPE)) {
            // Nowhere to park -- kernel code, or the legacy loader.
            // Report 0 rather than spinning, for the same reason the
            // read side reports EOF there: a caller that cannot block
            // must not be told "try again forever".
            regs[14] = 0;
        } else {
            // Parked. libsys's sys_write() loops on SYS_RETRY, and the
            // retry re-sends the WHOLE buffer -- which is only correct
            // because pipe_write() is all-or-nothing, so nothing was
            // taken. A partial write here would duplicate those bytes
            // on the retry.
            blocked = 1;
        }
        scheduler_preempt_enable();
    }
    kfree(kbuf);
    return blocked;
}

SYSCALL_HANDLER sys_do_write_file(uint64_t *regs, uint64_t pml4, struct open_file *f,
                                   uint64_t buf_ptr, uint64_t len) {
    // fs_write_range(), NOT fs_write(). This used to copy into a
    // NUL-terminated scratch buffer and call fs_write(name, tmp, 1),
    // which treats its argument as a C STRING -- so a write containing
    // a zero byte stopped there, wrote only the prefix, AND RETURNED
    // `len` as if all of it had landed. Text files were unaffected and
    // everything binary was silently truncated: /bin/mkfiles asking for
    // 1207 bytes of a derived pattern got 82, because each of its two
    // chunks stopped at its own first zero.
    //
    // fs_write_range() takes an explicit length and treats the buffer
    // as raw bytes, which is what a write(2) means.
    //
    // Heap, not stack -- see sys_do_write_console(). This one sits
    // directly above the whole TFS3 journal and ATA path, which is the
    // deepest chain in the kernel.
    char *tmp = kmalloc(SYS_WRITE_MAX);
    if (!tmp) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return; }
    if (!vmm_copy_from_user(pml4, tmp, buf_ptr, len)) {
        klog_write("syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        kfree(tmp);
        return;
    }
    // AT THE FD'S POSITION, which is what write(2) means -- this used
    // to append unconditionally and ignore the position the read path
    // maintained, and a position that writes ignore is not a position
    // to seek. SYS_O_APPEND is how a caller asks for the old behaviour;
    // it re-reads the size on every write rather than trusting a cached
    // end, because that is what makes two appenders to one file
    // interleave whole writes instead of overwriting each other.
    //
    // Nothing that opens with SYS_O_TRUNC changes behaviour: position 0
    // of an emptied file is its end.
    uint64_t at = f->file.append ? fs_size(f->file.name) : f->file.pos;
    int ok = fs_write_range(f->file.name, at, tmp, (uint32_t)len);
    if (ok) f->file.pos = at + len;
    // The COUNT IS NOW HONEST. Reporting `len` unconditionally is what
    // let the truncation go unnoticed: every caller checked its return
    // value and every one of them was told it had succeeded.
    regs[14] = ok ? len : (uint64_t)(int64_t)-EIO;
    kfree(tmp);
}

SYSCALL_HANDLER sys_do_read_file(uint64_t *regs, uint64_t pml4, struct open_file *f,
                                  uint64_t buf_ptr, uint64_t len) {
    // fs_read_range() reports 0 both at EOF and on any error (fs.h says
    // so explicitly), which is exactly the behaviour wanted here -- a
    // file deleted mid-read by another shell should read as EOF, not
    // fabricate data or fault. It fills a KERNEL buffer which is then
    // copied out; handing it the user pointer directly is what SMAP
    // forbids (vmm.h). len is capped at SYS_WRITE_MAX by the caller.
    uint64_t off = f->file.pos;
    char *kbuf = kmalloc(SYS_WRITE_MAX);   // heap -- see sys_do_write_console()
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return; }
    uint32_t n = fs_read_range(f->file.name, off, kbuf, (uint32_t)len);
    if (!vmm_copy_to_user(pml4, buf_ptr, kbuf, n)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
        kfree(kbuf);
        return;
    }
    f->file.pos += n;
    regs[14] = n;
    kfree(kbuf);
}

int sys_write(struct syscall_ctx *c) {
    // ABI: RDI = fd (was the buffer pointer before SYS_OPEN/SYS_READ
    // existed -- see the "ABI NOTE" on SYS_WRITE in syscall_abi.h),
    // RSI = buffer pointer, RDX = length.
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    // Validate the buffer before touching it -- CR3 is still the
    // calling process's own page tables at this point (a syscall
    // doesn't switch address spaces), so without this check a
    // process could hand the kernel a kernel-only address (still
    // *present*, since PML4 entry 0 is shared with every process --
    // see vmm.h) and get the kernel, running at full privilege, to
    // read it on the process's behalf, even though the process
    // could never legally read that address itself.
    uint64_t pml4 = c->pml4;

    // ROUTED ON THE DESCRIPTION'S KIND, not on the fd number. That is
    // the whole point of the two-level table: after `dup2(pipe_w, 1)`
    // fd 1 IS a pipe, and nothing here needs to know that a
    // redirection happened. The `if (fd == 1)` this replaced could not
    // express that, which is why redirection used to be a field on the
    // process instead.
    struct open_file *f = fd_get(pml4, fd);
    if (!f) {
        klog_write("syscall: write() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }

    switch (f->kind) {
    case FD_KIND_CONSOLE:
    case FD_KIND_KLOG:
        sys_do_write_console(c->regs, pml4, f->kind, buf_ptr, len);
        break;
    case FD_KIND_PIPE_W:
        // The one write that can PARK its caller, so its return value
        // is this function's -- exactly as the pipe read is.
        return sys_do_write_pipe(c->regs, pml4, f->pipe.idx, buf_ptr, len);
    case FD_KIND_FILE:
        if (f->file.mode != FD_MODE_WRITE) {
            klog_write("syscall: write() rejected -- fd is read-only\n");
            c->regs[14] = (uint64_t)(int64_t)-EBADF;
        } else {
            sys_do_write_file(c->regs, pml4, f, buf_ptr, len);
        }
        break;
    default:
        // A socket fd (SYS_SEND is its only writer) or a pipe READ end
        // reaching here means the caller used the wrong syscall or the
        // wrong end. Rejected like any other bad fd rather than
        // silently treated as something it is not.
        klog_write("syscall: write() rejected -- wrong kind of fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        break;
    }
    return 0;
}

int sys_read(struct syscall_ctx *c) {
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    uint64_t pml4 = c->pml4;

    // Routed on the description's KIND, exactly as SYS_WRITE is, so
    // `dup2(pipe_r, 0)` makes fd 0 a pipe with nothing here changed.
    struct open_file *f = fd_get(pml4, fd);
    if (!f) {
        klog_write("syscall: read() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    switch (f->kind) {
    case FD_KIND_CONSOLE:
        // The physical console. Blocks, claims the keyboard, and never
        // reports EOF -- see sys_do_read_console().
        return sys_do_read_console(c->regs, pml4, buf_ptr, len);

    case FD_KIND_PIPE_R:
        // Reads from the pipe, and BLOCKS when it is empty with a
        // writer still alive. The one handler that can PARK its caller
        // and still be a plain read: its return value is this
        // function's.
        return sys_do_read_pipe(c->regs, pml4, f->pipe.idx, buf_ptr, len);

    case FD_KIND_FILE:
        if (f->file.mode != FD_MODE_READ) {
            klog_write("syscall: read() rejected -- fd is write-only\n");
            c->regs[14] = (uint64_t)(int64_t)-EBADF;
            break;
        }
        // fs_read_range(), NOT fs_read(). fs_read() reads the WHOLE
        // file into a kmalloc'd buffer, so streaming one cost (file
        // size) of disk reads per call -- /bin/lspci reading the 1.6MB
        // pci.ids in 1KB chunks turned that into ~2.6GB of reads and 35
        // seconds. With a range read it is ~0.6s.
        sys_do_read_file(c->regs, pml4, f, buf_ptr, len);
        break;

    default:
        // A socket fd (SYS_RECV is its only reader), a pipe WRITE end,
        // or the kernel-log description, which has no reader.
        klog_write("syscall: read() rejected -- wrong kind of fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        break;
    }
    return 0;
}

// --- position and identity -------------------------------------------
//
// The two calls a buffered stdio cannot be written without: where am I
// in this stream, and what KIND of stream is it. Both live here rather
// than with the filesystem because both answer questions about a
// DESCRIPTION, not about a path -- the same reason SYS_READ and
// SYS_WRITE are in this file.

int sys_lseek(struct syscall_ctx *c) {
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f) {
        klog_write("syscall: lseek() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    // ESPIPE, not EINVAL, and the name is a historical accident worth
    // keeping: POSIX spells "this stream has no position" this way for
    // a pipe, a socket and a terminal alike, and a libc's fseek() turns
    // exactly this code into the errno a program expects.
    if (f->kind != FD_KIND_FILE) {
        c->regs[14] = (uint64_t)(int64_t)-ESPIPE;
        return 0;
    }

    int64_t off = (int64_t)c->a1;
    uint64_t whence = c->a2;
    // SIGNED arithmetic all the way, in 64 bits, because SEEK_END with
    // a negative offset is the ordinary way to read a file's tail and
    // an unsigned base would wrap it into a seek past the end -- which
    // is legal, so nothing downstream would report it.
    int64_t base;
    switch (whence) {
    case SYS_SEEK_SET: base = 0; break;
    case SYS_SEEK_CUR: base = (int64_t)f->file.pos; break;
    case SYS_SEEK_END: base = (int64_t)fs_size(f->file.name); break;
    default:
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    int64_t want = base + off;
    // Before byte zero is the one result that is an ERROR rather than a
    // strange-but-legal position. Past the end is fine: fs_write_range()
    // zero-fills a gap and fs_read_range() reports 0 there, so both
    // halves already behave the way POSIX says a sparse seek behaves.
    if (want < 0) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    f->file.pos = (uint64_t)want;
    c->regs[14] = (uint64_t)want;
    return 0;
}

int sys_fstat(struct syscall_ctx *c) {
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f) {
        klog_write("syscall: fstat() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    if (!vmm_validate_user_range(c->pml4, c->a1, sizeof(struct sys_stat))) {
        klog_write("syscall: fstat() rejected -- invalid output pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    struct sys_stat out;
    k_memset(&out, 0, sizeof out);
    switch (f->kind) {
    case FD_KIND_FILE:
        out.flags |= SYS_STAT_SEEKABLE;
        // Deliberately NOT a call into fs_stat() for the timestamps:
        // this is the same path-keyed answer SYS_STAT gives, and
        // duplicating the conversion here is how the two would drift.
        // What an fd adds is the flags; for the rest, a caller that
        // wants an inode and civil timestamps has SYS_STAT and a name.
        out.size = fs_size(f->file.name);
        out.is_dir = 0; // SYS_OPEN refuses a directory, so an fd is never one
        break;
    case FD_KIND_CONSOLE:
        out.flags |= SYS_STAT_TTY;
        break;
    default:
        // A pipe end or a socket: no size, no position, no timestamps.
        // Zero is the honest answer rather than a lossy one -- there is
        // no length for a pipe to have, and the flags say so.
        break;
    }
    vmm_copy_to_user(c->pml4, c->a1, &out, sizeof out); // validated above
    c->regs[14] = 0;
    return 0;
}

int sys_close(struct syscall_ctx *c) {
    // Kind-agnostic, and refcount-aware: fd_close() drops the
    // DESCRIPTOR, and the description behind it is torn down only when
    // the last descriptor naming it goes. That is what makes the
    // shell's save/restore dance safe --
    //
    //     saved = dup(1); dup2(f, 1); spawn(...); dup2(saved, 1); close(saved);
    //
    // -- because closing `saved` must not close the stream it named.
    // Closing the LAST writer of a pipe is still what turns a blocked
    // reader's wait into EOF; that now happens inside fd_desc_unref().
    c->regs[14] = fd_close(c->pml4, (int)c->a0) < 0 ? (uint64_t)(int64_t)-EBADF : 0;
    return 0;
}

int sys_dup(struct syscall_ctx *c) {
    // Lowest free descriptor naming the same description, as POSIX.
    uint64_t pml4 = c->pml4;
    int di = fd_desc_index(pml4, (int)c->a0);
    if (di < 0) { c->regs[14] = (uint64_t)(int64_t)-EBADF; return 0; }
    fd_desc[di].refs++;
    int fd = fd_install(pml4, di);
    if (fd < 0) {
        fd_desc_unref(di); // undo: the table was full
        c->regs[14] = (uint64_t)(int64_t)-EMFILE;
    } else {
        c->regs[14] = (uint64_t)fd;
    }
    return 0;
}

int sys_dup2(struct syscall_ctx *c) {
    // fd_dup2() reports -1 for both "oldfd is not open" and "newfd is out
    // of range", which are the same answer to the caller: EBADF names a
    // descriptor that cannot be used, whichever of the two it was.
    int r = fd_dup2(c->pml4, (int)c->a0, (int)c->a1);
    c->regs[14] = r < 0 ? (uint64_t)(int64_t)-EBADF : (uint64_t)r;
    return 0;
}

int sys_socket(struct syscall_ctx *c) {
    // See syscall_abi.h's SYS_SOCKET doc comment -- domain/type are
    // reserved for future use and must be 0 for now, rejected
    // otherwise so a caller relying on a real value being honored
    // fails loudly today rather than silently once one exists.
    uint64_t pml4 = c->pml4;
    uint64_t domain = c->a0;
    uint64_t type = c->a1;
    if (domain != 0 || type != 0) {
        klog_write("syscall: socket() rejected -- nonzero domain/type (not supported yet)\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
    } else {
        int di = fd_desc_alloc(FD_KIND_SOCKET, -1);
        int fd = di >= 0 ? fd_install(pml4, di) : -1;
        if (fd < 0) {
            if (di >= 0) fd_desc_unref(di);
            klog_write("syscall: socket() rejected -- fd table full\n");
            c->regs[14] = (uint64_t)(int64_t)-EMFILE;
        } else {
            c->regs[14] = (uint64_t)fd;
        }
    }
    return 0;
}

// Both share one body -- same fd validation, same "no transport yet"
// outcome (see syscall_abi.h). Doesn't touch the caller's buffer at all
// (nothing is actually sent/received), so unlike SYS_WRITE/SYS_READ
// there's no buffer pointer to validate here -- only the fd itself.
// The direction is a PARAMETER rather than a re-test of the syscall
// number: with a table there is one entry per number, so a handler that
// asks which one it is has lost information the table already had.
static int send_recv(struct syscall_ctx *c, int is_send) {
    uint64_t pml4 = c->pml4;
    int fd = (int)c->a0;
    struct open_file *f = fd_get(pml4, fd);
    if (!f || f->kind != FD_KIND_SOCKET) {
        klog_write(is_send ? "syscall: send() rejected -- bad fd\n"
                                    : "syscall: recv() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
    } else {
        // A REAL socket fd, and no transport behind it. ENOSYS rather
        // than EBADF: the fd is fine and the caller did nothing wrong,
        // the call simply is not implemented -- which is a different
        // thing to tell a caller, and telling them apart is the point
        // of having codes at all. See syscall_abi.h.
        klog_write(is_send ? "syscall: send() -- no transport yet, failing\n"
                                    : "syscall: recv() -- no transport yet, failing\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOSYS;
    }
    return 0;
}

int sys_send(struct syscall_ctx *c) { return send_recv(c, 1); }
int sys_recv(struct syscall_ctx *c) { return send_recv(c, 0); }

int sys_pipe(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(int) * 2)) {
        klog_write("syscall: pipe() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        int idx = pipe_create();
        int rd = idx >= 0 ? fd_desc_alloc(FD_KIND_PIPE_R, idx) : -1;
        int wd = rd  >= 0 ? fd_desc_alloc(FD_KIND_PIPE_W, idx) : -1;
        int rfd = wd >= 0 ? fd_install(pml4, rd) : -1;
        int wfd = rfd >= 0 ? fd_install(pml4, wd) : -1;
        if (idx < 0 || rd < 0 || wd < 0 || rfd < 0 || wfd < 0) {
            // Unwind rather than leak. A half-made pipe with only
            // one end is worse than none: the caller cannot tell,
            // and would block forever on the end that is missing.
            if (rfd >= 0) fd_close(pml4, rfd); else if (rd >= 0) fd_desc_unref(rd);
            if (wd >= 0 && wfd < 0) fd_desc_unref(wd);
            if (idx >= 0) { pipe_close_reader(idx); pipe_close_writer(idx); }
            // ENFILE when the system-wide pipe table is out, EMFILE
            // when it was this process's descriptors -- the caller can
            // do something about the second and nothing about the first.
            klog_write("syscall: pipe() failed -- no free pipe or fd\n");
            c->regs[14] = (uint64_t)(int64_t)(idx < 0 ? -ENFILE : -EMFILE);
        } else {
            int out[2] = { rfd, wfd };
            vmm_copy_to_user(pml4, c->a0, out, sizeof out); // range validated above
            c->regs[14] = 1;
        }
    }
    return 0;
}

// Everything this process left open. Not part of its address space
// (fs.c and pipe.c are separate kernel resources, nothing about them is
// mapped into the process), so vmm_destroy_address_space() would not
// reclaim any of it.
void fd_release_all(uint64_t pml4_phys) {
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
    // Every descriptor this space holds, dropped through the same
    // refcounted path SYS_CLOSE uses. A pipe end must be RELEASED, not
    // just forgotten: a process that dies holding the last write end is
    // exactly how a reader learns there is no more output coming, and
    // dropping the reference silently would leave that reader blocked
    // forever on a dead writer. Sharing the unref path is also what
    // makes an INHERITED fd safe -- the parent's copy survives its
    // child dying.
    struct fd_space *sp = space_find(pml4_phys);
    if (sp) {
        for (int i = 0; i < FD_MAX; i++) {
            if (sp->d[i] < 0) continue;
            fd_desc_unref(sp->d[i]);
            sp->d[i] = -1;
        }
        sp->pml4 = 0; // the table itself is reusable now
    }
}
