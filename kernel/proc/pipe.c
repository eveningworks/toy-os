// See pipe.h for the design and what this deliberately isn't.
#include "pipe.h"
#include "scheduler.h"
#include "syscalls.h"
#include "errno.h"
#include "klog.h"
#include "heap.h"
#include "vmm.h"
#include "signal.h"   // signal_send -- SIGPIPE on a dead pipe
#include "syscall_abi.h" // SYS_RETRY -- the wake value blocked readers see
#include "kslots.h"
#include <stddef.h>

struct pipe {
    char buf[PIPE_BUF_SIZE];
    int head;    // next byte to read
    int count;   // bytes buffered
    int readers;
    int writers;
};

// GROWN ON DEMAND (kslots.h): created from syscall context, where an
// allocation failure is an ordinary -1, and released without freeing,
// so process teardown never allocates and never frees.
static struct kslots g_pipes = KSLOTS_INIT(sizeof(struct pipe));

static struct pipe *at(int idx) { return kslots_at(&g_pipes, idx); }

int pipe_valid(int idx) { return at(idx) != NULL; }

// This pipe's wait channel -- see scheduler.h. The struct's own address,
// so a waiter and a waker name the same thing without a second table,
// and a write to one pipe cannot wake the readers of another.
const void *pipe_wait_chan(int idx) { return at(idx); }

int pipe_create(void) {
    int i = kslots_alloc(&g_pipes);   // zeroed: empty, head 0
    if (i < 0) return -1;
    struct pipe *p = at(i);
    p->readers = 1;
    p->writers = 1;
    return i;
}

void pipe_add_reader(int idx) { struct pipe *p = at(idx); if (p) p->readers++; }
void pipe_add_writer(int idx) { struct pipe *p = at(idx); if (p) p->writers++; }

// Frees the slot once nobody holds either end. Kept in one place so the
// two close paths can't disagree about when that is.
static void release_if_orphaned(int idx, struct pipe *p) {
    if (p->readers <= 0 && p->writers <= 0) kslots_free(&g_pipes, idx);
}

void pipe_close_reader(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    if (p->readers > 0) p->readers--;
    if (p->readers == 0) {
        // The mirror of the writer case below, and load-bearing now
        // that a full pipe PARKS its writer: the last reader going away
        // is what turns that block into "discarded, nobody is
        // listening". Without this wake, a writer parked on a full pipe
        // whose reader then died would wait forever for room that can
        // never be made -- a hang rather than the dropped write pipe.h
        // documents.
        scheduler_wake(p, SYS_RETRY);
    }
    release_if_orphaned(idx, p);
}

void pipe_close_writer(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    if (p->writers > 0) p->writers--;
    if (p->writers == 0) {
        // The last writer going away is what turns a blocking read into
        // EOF. A reader parked right now would otherwise wait forever
        // for data that can no longer arrive.
        scheduler_wake(p, SYS_RETRY);
    }
    release_if_orphaned(idx, p);
}

int64_t pipe_write(int idx, const char *src, uint32_t len) {
    struct pipe *p = at(idx);
    if (!p || !src) return 0;
    if (p->readers <= 0) return 0; // nobody will ever read it -- see pipe.h

    // ALL OR NOTHING. A write that does not fit takes NONE of the bytes
    // and reports "would block", so the caller can park and retry the
    // whole thing -- the mirror of pipe_read() returning -1 on an empty
    // pipe with a live writer.
    //
    // This used to take what fitted and report a short count, which is
    // a correct-looking answer that nothing in ring 3 acts on: no
    // program here loops on a short write, so a producer faster than
    // its reader silently LOST the remainder. A pipeline makes that
    // unavoidable rather than unlucky, since the reader is another
    // process that may not have been scheduled yet.
    //
    // Atomicity is affordable because pipe_fd_write() CLAMPS a pipe
    // write to PIPE_BUF_SIZE, so any one write fits once the pipe drains
    // and this cannot deadlock on a request too big to ever satisfy.
    // POSIX guarantees exactly this for writes up to PIPE_BUF, and for
    // the same reason. The clamp used to be free -- SYS_WRITE_MAX was
    // under PIPE_BUF_SIZE -- and stopped being when the cap was raised;
    // see api/pipe.h.
    if (len > (uint32_t)(PIPE_BUF_SIZE - p->count)) return -1; // would block

    int64_t written = 0;
    while ((uint32_t)written < len) {
        p->buf[(p->head + p->count) % PIPE_BUF_SIZE] = src[written];
        p->count++;
        written++;
    }

    // Wake any parked reader. Harmless when none is: scheduler_wake()
    // returns 0 and does nothing.
    if (written > 0) scheduler_wake(p, SYS_RETRY);
    return written;
}

int64_t pipe_read(int idx, char *dst, uint32_t len) {
    struct pipe *p = at(idx);
    if (!p || !dst) return 0;

    if (p->count == 0) {
        // Empty. The answer depends entirely on whether more can ever
        // arrive -- see pipe.h on why conflating these two is the bug
        // that makes a terminal think a running program has exited.
        if (p->writers > 0) return -1; // would block
        return 0;                      // EOF
    }

    int64_t n = 0;
    while ((uint32_t)n < len && p->count > 0) {
        dst[n] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->count--;
        n++;
    }

    // Draining makes room, so a WRITER parked on a full pipe can now
    // proceed. Readers and writers share THIS PIPE's channel and this
    // wakes both -- which is correct rather than merely tolerable:
    // every waiter re-runs its syscall and re-parks if it is still not
    // ready, so a spurious wake costs a syscall and never a wrong
    // answer. What it no longer does is disturb the readers of OTHER
    // pipes, which is what a shared category channel did.
    if (n > 0) scheduler_wake(p, SYS_RETRY);
    return n;
}

// --- a pipe as a pair of descriptors ---------------------------------

// Returns 1 if the caller was PARKED (its syscall has no return value
// yet -- the wake writes it), 0 otherwise. That is the one thing the
// dispatcher still needs to know, so it is the return value rather than
// another out-parameter.
static int pipe_fd_read(struct syscall_ctx *c, struct open_file *f,
                        uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int pipe_idx = f->pipe.idx;
    // pipe_read() fills a kernel buffer, which is then handed out -- it
    // cannot write into the user pointer itself once SMAP is on
    // (vmm.h). len is capped at SYS_WRITE_MAX by the caller. Heap
    // rather than stack: see fd_fd_bounce_alloc().
    char *kbuf = fd_bounce_alloc(&len);
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
        klog_write(KLOG_ERR "syscall: read() rejected -- invalid buffer pointer\n");
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

// A write to a pipe WRITE end -- which after this change is simply
// what fd 1 refers to in a process whose parent redirected it. There
// is no per-process "stdout pipe" any more: the redirection lives in
// the descriptor table, where dup2 can also put it.
// Returns 1 if the caller was PARKED, like pipe_fd_read() -- a full
// pipe blocks its writer now rather than taking what fits.
static int pipe_fd_write(struct syscall_ctx *c, struct open_file *f,
                         uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
    uint64_t pml4 = c->pml4;
    int pipe_idx = f->pipe.idx;
    // CLAMPED TO THE PIPE BUFFER, and this is load-bearing rather than
    // tidy. pipe_write() is ALL-OR-NOTHING: a write that does not fit
    // takes none of the bytes and parks the writer to retry the whole
    // thing (api/pipe.h). That is only safe while one write can always
    // eventually fit -- which held for free while SYS_WRITE_MAX was
    // 1024 against a 4096-byte pipe, and stopped holding the moment the
    // cap became 64 KiB: a writer would park on a request the pipe can
    // never satisfy and never wake. A DEADLOCK, not a slow path.
    //
    // Clamping keeps POSIX's guarantee too -- a write up to PIPE_BUF is
    // atomic -- and costs the caller nothing, because libsys's
    // sys_write() loops on a short count.
    if (len > PIPE_BUF_SIZE) len = PIPE_BUF_SIZE;

    char *kbuf = fd_bounce_alloc(&len);
    if (!kbuf) { regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }

    int blocked = 0;
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        // Atomic against the reader, for the reason pipe_fd_read()
        // gives above: a wake that fires between "it is full" and
        // "park" is lost, and with both ends able to sleep that is a
        // deadlock rather than a delay.
        scheduler_preempt_disable();
        int64_t n = pipe_write(pipe_idx, kbuf, (uint32_t)len);
        // **A DEAD PIPE IS A FAILURE, NOT A SHORT WRITE**, and reporting
        // it as one WEDGED THE MACHINE. pipe_write() answers 0 when no
        // reader is left; this used to hand that straight back, where
        // libsys's sys_write() reads a zero as "wrote nothing, try the
        // rest" and loops -- so `echo abc | no_such_command`, one typo,
        // spun a process at 100% CPU forever and the whole guest stopped
        // answering (docs/bugs.md had the reproduction).
        //
        // POSIX: raise SIGPIPE and fail EPIPE. A process that has not
        // caught or ignored it dies here, which is what makes a pipeline
        // whose reader went away collapse instead of spin; one that HAS
        // -- every shell does, around its own pipelines -- sees the
        // errno and can act.
        //
        // `len > 0` is what separates this from an honest zero-length
        // write, which is not an error and must stay one.
        if (n == 0 && len > 0) {
            signal_send(scheduler_current_pid(), SIGPIPE);
            regs[14] = (uint64_t)(int64_t)-EPIPE;
        } else if (n >= 0) {
            regs[14] = (uint64_t)n; // all of it, or an honest zero
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

static void pipe_fd_open(struct open_file *f, int idx) { f->pipe.idx = idx; }
// Closing the LAST reader or writer is what turns the other end's wait
// into EOF or EPIPE -- pipe_close_reader()/_writer() wake it.
static void pipe_fd_release_r(struct open_file *f) { pipe_close_reader(f->pipe.idx); }
static void pipe_fd_release_w(struct open_file *f) { pipe_close_writer(f->pipe.idx); }

const struct fd_ops pipe_read_fd_ops = {
    .name = "pipe", .read = pipe_fd_read,
    .open = pipe_fd_open, .release = pipe_fd_release_r,
};
const struct fd_ops pipe_write_fd_ops = {
    .name = "pipe", .write = pipe_fd_write,
    .open = pipe_fd_open, .release = pipe_fd_release_w,
};

int sys_pipe(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(int) * 2)) {
        klog_write(KLOG_ERR "syscall: pipe() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        int idx = pipe_create();
        int rd = idx >= 0 ? fd_desc_alloc(&pipe_read_fd_ops, idx) : -1;
        int wd = rd  >= 0 ? fd_desc_alloc(&pipe_write_fd_ops, idx) : -1;
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
            klog_write(KLOG_ERR "syscall: pipe() failed -- no free pipe or fd\n");
            c->regs[14] = (uint64_t)(int64_t)(idx < 0 ? -ENFILE : -EMFILE);
        } else {
            int out[2] = { rfd, wfd };
            // Validated above BEFORE the fds existed; a copy that still
            // fails is reported rather than pretending two numbers landed.
            c->regs[14] = vmm_copy_to_user(pml4, c->a0, out, sizeof out) ? 0 : (uint64_t)(int64_t)-EFAULT;
        }
    }
    return 0;
}
