// See pipe.h for the design and what this deliberately isn't.
#include "pipe.h"
#include "scheduler.h"
#include "syscall_abi.h" // SYS_RETRY -- the wake value blocked readers see
#include <stddef.h>

struct pipe {
    int used;
    char buf[PIPE_BUF_SIZE];
    int head;    // next byte to read
    int count;   // bytes buffered
    int readers;
    int writers;
};

// Statically allocated, like the event queues: these are written from
// syscall context and torn down from process teardown, and a fixed
// table keeps both paths free of allocation failure handling.
static struct pipe pipes[PIPE_MAX];

static struct pipe *at(int idx) {
    if (idx < 0 || idx >= PIPE_MAX) return NULL;
    struct pipe *p = &pipes[idx];
    return p->used ? p : NULL;
}

int pipe_valid(int idx) { return at(idx) != NULL; }

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (pipes[i].used) continue;
        pipes[i].used = 1;
        pipes[i].head = 0;
        pipes[i].count = 0;
        pipes[i].readers = 1;
        pipes[i].writers = 1;
        return i;
    }
    return -1;
}

void pipe_add_reader(int idx) { struct pipe *p = at(idx); if (p) p->readers++; }
void pipe_add_writer(int idx) { struct pipe *p = at(idx); if (p) p->writers++; }

// Frees the slot once nobody holds either end. Kept in one place so the
// two close paths can't disagree about when that is.
static void release_if_orphaned(struct pipe *p) {
    if (p->readers <= 0 && p->writers <= 0) {
        p->used = 0;
        p->count = 0;
        p->head = 0;
    }
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
        scheduler_wake(SCHED_WAIT_PIPE, SYS_RETRY);
    }
    release_if_orphaned(p);
}

void pipe_close_writer(int idx) {
    struct pipe *p = at(idx);
    if (!p) return;
    if (p->writers > 0) p->writers--;
    if (p->writers == 0) {
        // The last writer going away is what turns a blocking read into
        // EOF. A reader parked right now would otherwise wait forever
        // for data that can no longer arrive.
        scheduler_wake(SCHED_WAIT_PIPE, SYS_RETRY);
    }
    release_if_orphaned(p);
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
    // Atomicity is affordable because a single write is capped at
    // SYS_WRITE_MAX (1024) and the buffer is PIPE_BUF_SIZE (4096), so
    // any one write fits once the pipe drains and this cannot deadlock
    // on a request too big to ever satisfy. POSIX guarantees exactly
    // this for writes up to PIPE_BUF, and for the same reason.
    if (len > (uint32_t)(PIPE_BUF_SIZE - p->count)) return -1; // would block

    int64_t written = 0;
    while ((uint32_t)written < len) {
        p->buf[(p->head + p->count) % PIPE_BUF_SIZE] = src[written];
        p->count++;
        written++;
    }

    // Wake any parked reader. Harmless when none is: scheduler_wake()
    // returns 0 and does nothing.
    if (written > 0) scheduler_wake(SCHED_WAIT_PIPE, SYS_RETRY);
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
    // proceed. Readers and writers share SCHED_WAIT_PIPE and this wakes
    // both -- which is correct rather than merely tolerable: every
    // waiter re-runs its syscall and re-parks if it is still not ready,
    // so a spurious wake costs a syscall and never a wrong answer.
    if (n > 0) scheduler_wake(SCHED_WAIT_PIPE, SYS_RETRY);
    return n;
}
