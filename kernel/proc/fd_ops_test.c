// The descriptor layer's dispatch: which ops each kind of stream has, and
// that a description's open and release reach the subsystem that owns it.
#include "ktest.h"
#include "syscalls.h"
#include "pipe.h"
#include "kfmt.h"

// WHAT EACH KIND CAN DO, as ring 3 sees it: read(), write(), lseek() and
// isatty(). A NULL slot is an answer (EBADF, ESPIPE, not a terminal), so
// an op lost in an edit changes behaviour without failing to compile --
// this is the table that notices.
static const struct {
    const struct fd_ops *ops;
    uint8_t read, write, seek, tty;
} kinds[] = {
    { &file_fd_ops,        1, 1, 1, 0 },
    { &pipe_read_fd_ops,   1, 0, 0, 0 },
    { &pipe_write_fd_ops,  0, 1, 0, 0 },
    { &shm_fd_ops,         0, 0, 0, 0 },
    { &socket_fd_ops,      1, 1, 0, 0 },
    { &console_fd_ops,     1, 1, 0, 1 },
    { &pty_master_fd_ops,  1, 1, 0, 1 },
    { &pty_slave_fd_ops,   1, 1, 0, 1 },
    { &tty_fd_ops,         1, 1, 0, 1 },
    { &klog_fd_ops,        0, 1, 0, 0 },
    { &applog_fd_ops,      0, 1, 0, 0 },
};

KTEST("fd", "every kind of stream reads, writes, seeks and is a terminal as it should") {
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        const struct fd_ops *o = kinds[i].ops;
        KTEST_ASSERT(o->name != NULL);
        int ok = !!o->read == kinds[i].read && !!o->write == kinds[i].write &&
                 !!o->seek == kinds[i].seek && !!o->tty == kinds[i].tty;
        if (!ok) klog_printf("fd_ops: \"%s\" has read=%d write=%d seek=%d tty=%d\n",
                             o->name, !!o->read, !!o->write, !!o->seek, !!o->tty);
        KTEST_ASSERT(ok);
        // Only a pty master's write is typed AT a terminal; anything else
        // flagged so could not be a child's stderr.
        KTEST_ASSERT_EQ(!!(o->flags & FD_OPS_WRITE_IS_INPUT), o == &pty_master_fd_ops);
    }
}

KTEST("fd", "a description's open and release reach the pipe it names") {
    int idx = pipe_create();
    if (idx < 0) KTEST_SKIP("no free pipe");
    int rd = fd_desc_alloc(&pipe_read_fd_ops, idx);
    int wd = fd_desc_alloc(&pipe_write_fd_ops, idx);
    KTEST_ASSERT(rd >= 0 && wd >= 0);
    KTEST_ASSERT(fd_desc_at(rd)->ops == &pipe_read_fd_ops);
    KTEST_ASSERT_EQ(fd_desc_at(rd)->pipe.idx, idx);    // open stored the index

    // The last reader goes: release must have closed the reading end, so
    // the pipe now answers a write the way a dead pipe does -- 0, which
    // the write op turns into EPIPE.
    fd_desc_unref(rd);
    KTEST_ASSERT(fd_desc_at(rd)->ops == NULL);
    char b = 'x';
    KTEST_ASSERT_EQ(pipe_write(idx, &b, 1), 0);

    fd_desc_unref(wd);
    KTEST_ASSERT(!pipe_valid(idx));                     // both ends gone: freed
}
