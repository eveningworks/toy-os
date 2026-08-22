// Pseudo-terminals -- see kernel/pty.h for what one is and why there is
// no /dev/ptmx.
//
// This file is a tty DRIVER and nothing else. Everything a person thinks
// of as terminal behaviour -- canonical mode, echo, what Ctrl-C means --
// is ldisc.c's and is shared with the physical console; what is here is
// the plumbing that makes "the other end of an fd" a place bytes can
// come from and go to.
#include "pty.h"
#include "tty_internal.h"
#include "scheduler.h"
#include "syscall_abi.h" // SYS_RETRY -- the value a woken reader gets
#include "string.h"
#include <stddef.h>

#define PTY_OUT_MAX 4096 // what the terminal has emitted and the master
                         // has not read yet. PIPE_BUF_SIZE, because it
                         // is the same job.

struct pty {
    int used;
    int masters, slaves; // reference counts per END, as pipe.c has
    struct tty *t;

    // The output ring: bytes the terminal emitted, waiting for the
    // master to read them. A ring rather than the write-through tty0
    // uses, because there IS a second process here and it may be behind.
    char out[PTY_OUT_MAX];
    unsigned out_head, out_tail;
};

static struct pty g_ptys[PTY_MAX];

static struct pty *at(int idx) {
    if (idx < 0 || idx >= PTY_MAX) return NULL;
    return g_ptys[idx].used ? &g_ptys[idx] : NULL;
}

static unsigned out_used(const struct pty *p) {
    return (p->out_head - p->out_tail) % PTY_OUT_MAX;
}

static unsigned out_space(const struct pty *p) {
    return PTY_OUT_MAX - 1 - out_used(p); // one slot reserved: head==tail is empty
}

static void out_push(struct pty *p, const char *src, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        unsigned next = (p->out_head + 1) % PTY_OUT_MAX;
        if (next == p->out_tail) return; // full -- see pty.h on who drops
        p->out[p->out_head] = src[i];
        p->out_head = next;
    }
}

// The tty driver hook. **MAY RUN IN THE KEYBOARD IRQ** -- no, not on a
// pty; but it may run inside tty_input() from a master write, which is
// an ordinary syscall, and the discipline calls it for ECHO from
// wherever input arrived. So it keeps to the same restraint as the
// console's: fill a buffer, wake, and nothing else.
static void pty_output_hook(struct tty *t, const char *buf, unsigned len) {
    struct pty *p = tty_driver_data(t);
    if (!p || !p->used) return;
    out_push(p, buf, len);
    scheduler_wake(pty_out_wait_chan((int)(p - g_ptys)), SYS_RETRY);
}

static const struct tty_driver pty_driver = {
    .name = "pty",
    .output = pty_output_hook,
};

int pty_create(int owner_pid) {
    for (int i = 0; i < PTY_MAX; i++) {
        if (g_ptys[i].used) continue;
        struct pty *p = &g_ptys[i];
        k_memset(p, 0, sizeof *p);
        p->used = 1;
        p->t = tty_create(&pty_driver, p);
        if (!p->t) { p->used = 0; return -1; } // TTY_MAX reached first
        p->masters = 1;
        p->slaves = 1;
        tty_set_owner(p->t, owner_pid);
        return i;
    }
    return -1;
}

struct tty *pty_tty(int idx) {
    struct pty *p = at(idx);
    return p ? p->t : NULL;
}

int pty_valid(int idx) { return at(idx) != NULL; }

// The last close of EITHER end wakes the other, which is the rule that
// makes end-of-file arrive rather than being waited for forever: a
// reader parked on an end nobody will ever write to again has to be told
// so, and a wake is the only thing that can tell it. The pty itself
// survives until both counts are zero -- a master must still be able to
// drain what a dead shell already printed.
static void maybe_release(struct pty *p, int idx) {
    if (p->masters > 0 || p->slaves > 0) return;
    tty_destroy(p->t);
    p->t = NULL;
    p->used = 0;
    (void)idx;
}

void pty_close_master(int idx) {
    struct pty *p = at(idx);
    if (!p || p->masters <= 0) return;
    if (--p->masters == 0) scheduler_wake(tty_wait_chan(p->t), SYS_RETRY);
    maybe_release(p, idx);
}

void pty_close_slave(int idx) {
    struct pty *p = at(idx);
    if (!p || p->slaves <= 0) return;
    if (--p->slaves == 0) scheduler_wake(pty_out_wait_chan(idx), SYS_RETRY);
    maybe_release(p, idx);
}

int pty_master_open(int idx) { struct pty *p = at(idx); return p ? p->masters > 0 : 0; }
int pty_slave_open(int idx)  { struct pty *p = at(idx); return p ? p->slaves  > 0 : 0; }

// The pty's OUTPUT side, which is a different queue from the terminal's
// input and therefore a different channel. Using the ring's own address
// rather than the pty's: a slave reader parks on tty_wait_chan(), and
// two waiters on one address would wake each other for the wrong reason
// -- scheduler.h's whole point.
const void *pty_out_wait_chan(int idx) {
    if (idx < 0 || idx >= PTY_MAX) return NULL;
    return &g_ptys[idx].out_head;
}

int64_t pty_master_read(int idx, char *dst, uint32_t len) {
    struct pty *p = at(idx);
    if (!p || !dst) return 0;

    unsigned n = 0;
    while (n < len && p->out_tail != p->out_head) {
        dst[n++] = p->out[p->out_tail];
        p->out_tail = (p->out_tail + 1) % PTY_OUT_MAX;
    }
    if (n) {
        // Space freed: a slave writer parked on a full buffer can go.
        scheduler_wake(pty_out_wait_chan(idx), SYS_RETRY);
        return (int64_t)n;
    }
    // Empty. Which of the two empties is it?
    return p->slaves > 0 ? -1 : 0;
}

int64_t pty_master_write(int idx, const char *src, uint32_t len) {
    struct pty *p = at(idx);
    if (!p || !src) return 0;
    if (p->slaves <= 0) return 0; // nothing left to type at

    // Byte at a time THROUGH THE DISCIPLINE, which is the whole point: a
    // 0x03 written here is recognised as INTR by the same code that
    // recognises it from the physical keyboard, and a newline ends a
    // canonical line here exactly as Enter does there.
    for (uint32_t i = 0; i < len; i++) tty_input(p->t, (uint8_t)src[i], 0);
    return (int64_t)len;
}

int64_t pty_slave_write(int idx, const char *src, uint32_t len) {
    struct pty *p = at(idx);
    if (!p || !src) return 0;
    if (p->masters <= 0) return 0; // discarded and reported; see pty.h
    if (out_space(p) < len) return -1; // would block -- park and retry

    out_push(p, src, len);
    scheduler_wake(pty_out_wait_chan(idx), SYS_RETRY);
    return (int64_t)len;
}
