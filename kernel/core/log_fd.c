// The two log sinks as descriptors. Write-only: there is nothing to read
// back through a descriptor, and `dmesg` and `log` read their own stores.
#include "syscalls.h"
#include "errno.h"
#include "klog.h"
#include "applog.h"
#include "heap.h"
#include "vmm.h"

enum log_sink { SINK_KLOG, SINK_APPLOG };

static int log_write(struct syscall_ctx *c, enum log_sink sink,
                     uint64_t buf_ptr, uint64_t len) {
    uint64_t *regs = c->regs;
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
    if (!vmm_copy_from_user(c->pml4, kbuf, buf_ptr, len)) {
        klog_write(KLOG_ERR "syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        const char *buf = kbuf;
        if (sink == SINK_APPLOG) {
            // TAGGED BY WHO IS WRITING, which the kernel knows and the
            // program cannot lie about -- a tag a caller supplied would
            // be a tag a caller could forge, and the whole value of
            // `log -u toywm` is that it means what it says.
            struct proc_info info;
            const char *tag = "?";
            if (scheduler_proc_info(scheduler_current_pid() - 1, &info))
                tag = info.name;
            applog_write(tag, (const char *)buf, (uint32_t)len);
        } else {
            for (uint64_t i = 0; i < len; i++) klog_putc(buf[i]);
        }
        regs[14] = len; // bytes written, back via RAX
    }
    kfree(kbuf);
    return 0;
}

static int klog_fd_write(struct syscall_ctx *c, struct open_file *f,
                         uint64_t buf_ptr, uint64_t len) {
    (void)f;
    return log_write(c, SINK_KLOG, buf_ptr, len);
}

static int applog_fd_write(struct syscall_ctx *c, struct open_file *f,
                           uint64_t buf_ptr, uint64_t len) {
    (void)f;
    return log_write(c, SINK_APPLOG, buf_ptr, len);
}

// KLOG -- the stderr of a process started with no terminal --
// goes to the KERNEL LOG. A kind of DESCRIPTION rather than a
// test on the fd number, so it moves with dup2 like any other
// stream. It reaches the serial console and `dmesg`, so a
// service or a GUI client can still say something a test can
// read; journald is the same answer to the same question.
const struct fd_ops klog_fd_ops = { .name = "klog", .write = klog_fd_write };

// The application log (api/applog.h), tagged with the writing program's
// name. What a SERVICE's stdout is, so its ordinary output is captured
// and attributed instead of landing on a console nobody is reading.
const struct fd_ops applog_fd_ops = { .name = "log", .write = applog_fd_write };
