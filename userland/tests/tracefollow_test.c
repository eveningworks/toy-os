// strace -f's kernel half (docs/trace-design.md stage 3): two traces at
// once, and SPAWN_TRACE_FOLLOW taking a traced program's children --
// spawned and forked -- into the same ring.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which shaped every check:
//   - "the followed ring has a grandchild's records" passes a kernel that
//     follows EVERY trace, so a second ring runs the same program without
//     FOLLOW and must hold no grandchild pid and no grandchild path;
//   - "two traces work" passes one ring receiving both programs, so each
//     ring must hold its own child's pid and never the other's;
//   - "live reaches 0" passes a header nobody writes, so the tracer also
//     samples it while the orphaned grandchild still runs: it must be
//     non-zero AFTER the child is reaped, and the orphan's exit must
//     still arrive.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "trace_abi.h"
#include "lib/utest.h"

#define RING_BYTES (64 * 1024)
#define MAX_RECS   4096
#define SELF       "/tests/tracefollow_test"
#define LEAF_PATH  "/tracefollow/leaf"
#define FORK_PATH  "/tracefollow/forked"
#define SLOW_PATH  "/tracefollow/slow"

struct ring {
    char name[32];
    struct trace_ring_hdr *h;
    struct trace_rec recs[MAX_RECS];
    int n;
};
static struct ring g_a, g_b;

// --- the traced side -----------------------------------------------------------

static int leaf(const char *path, int ms) {
    if (ms) sys_sleep_ms(ms);
    sys_open(path, 0);
    sys_exit(0);
}

// A spawned grandchild it waits for, a forked one it waits for, then an
// orphan that outlives it.
static int parent(void) {
    int code = 0;
    int pid = sys_spawn(SELF, "leaf", -1);
    if (pid > 0) sys_waitpid(pid, &code);
    int f = sys_fork();
    if (f == 0) leaf(FORK_PATH, 0);
    if (f > 0) sys_waitpid(f, &code);
    sys_spawn(SELF, "slow", -1);
    sys_exit(0);
}

// --- the tracer ------------------------------------------------------------------

static int ring_open(struct ring *r, const char *tag) {
    snprintf(r->name, sizeof r->name, "tracefollow.%s.%d", tag, sys_getpid());
    int fd = sys_shm_open(r->name, RING_BYTES, SHM_CREATE | SHM_EXCL);
    void *p = fd >= 0
        ? sys_mmap(0, RING_BYTES, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0) : 0;
    if (fd >= 0) sys_close(fd);
    if (!p || p == (void *)-1) return 0;
    r->h = p;
    r->h->magic = TRACE_RING_MAGIC;
    r->h->nrec = (RING_BYTES - TRACE_HDR_SIZE) / TRACE_REC_SIZE;
    return 1;
}

static void drain(struct ring *r) {
    const char *base = (const char *)r->h + TRACE_HDR_SIZE;
    while (r->h->tail != r->h->head) {
        const struct trace_rec *x =
            (const struct trace_rec *)(base + (r->h->tail % r->h->nrec) * TRACE_REC_SIZE);
        if (r->n < MAX_RECS) r->recs[r->n++] = *x;
        r->h->tail++;
    }
}

static int spawn_traced(struct ring *r, unsigned extra) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = "parent";
    o.flags = SPAWN_TRACE | extra;
    o.trace_ring = r->name;
    return sys_spawn_opts(SELF, &o);
}

static int has_pid(const struct ring *r, int pid) {
    for (int i = 0; i < r->n; i++) if (r->recs[i].pid == pid) return 1;
    return 0;
}

// The pid whose open() carried `path`, or 0.
static int opener(const struct ring *r, const char *path) {
    size_t len = strlen(path);
    for (int i = 0; i < r->n; i++) {
        const struct trace_rec *x = &r->recs[i];
        if (x->nr == SYS_OPEN && x->kind == TRACE_ENTRY && x->blob_len == len &&
            !memcmp(x->blob, path, len))
            return x->pid;
    }
    return 0;
}

static int exited(const struct ring *r, int pid) {
    for (int i = 0; i < r->n; i++)
        if (r->recs[i].pid == pid && r->recs[i].nr == SYS_EXIT && r->recs[i].kind == TRACE_NORETURN)
            return 1;
    return 0;
}

static int contiguous(const struct ring *r) {
    for (int i = 0; i < r->n; i++) if (r->recs[i].seq != (uint32_t)i) return 0;
    return r->n > 0;
}

// The pids a record of `nr`'s exit returned, in order: spawn's and fork's.
static int nth_ret(const struct ring *r, int pid, int nr, int nth) {
    for (int i = 0; i < r->n; i++) {
        const struct trace_rec *x = &r->recs[i];
        if (x->pid == pid && x->nr == nr && x->kind == TRACE_EXIT && x->ret > 0 && nth-- == 0)
            return (int)x->ret;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "parent")) return parent();
    if (argc > 1 && !strcmp(argv[1], "leaf")) return leaf(LEAF_PATH, 0);
    if (argc > 1 && !strcmp(argv[1], "slow")) return leaf(SLOW_PATH, 400);

    utest_begin("tracefollow_test", "strace -f: following children, two traces at once",
                UTEST_VERDICT_FILE);
    int ok = ring_open(&g_a, "a") && ring_open(&g_b, "b");
    utest_check(ok, "both rings are created and mapped");
    if (!ok) return utest_end();

    struct sys_spawn_opts bad;
    sys_spawn_opts_init(&bad);
    bad.args = "leaf";
    bad.flags = SPAWN_TRACE_FOLLOW;
    int r = sys_spawn_opts(SELF, &bad);
    utest_checkf(r < 0 && sys_errno() == EINVAL, "FOLLOW without a trace is refused (%d)", r);

    int pa = spawn_traced(&g_a, SPAWN_TRACE_FOLLOW);
    int pb = spawn_traced(&g_b, 0);
    utest_checkf(pa > 0 && pb > 0, "a second trace starts while the first runs (%d, %d)", pa, pb);
    if (pa <= 0 || pb <= 0) {
        sys_shm_unlink(g_a.name);
        sys_shm_unlink(g_b.name);
        return utest_end();
    }

    int code = 0, done_a = 0, done_b = 0;
    uint32_t live_after_reap = 0;
    for (int spins = 0; spins < 5000; spins++) {
        drain(&g_a);
        drain(&g_b);
        if (!done_a && sys_waitpid_nohang(pa, &code) == pa) {
            done_a = 1;
            live_after_reap = g_a.h->live;
        }
        if (!done_b && sys_waitpid_nohang(pb, &code) == pb) done_b = 1;
        if (done_a && done_b && !g_a.h->live && !g_b.h->live) break;
        sys_sleep_ms(2);
    }
    drain(&g_a);
    drain(&g_b);
    sys_shm_unlink(g_a.name);
    sys_shm_unlink(g_b.name);

    utest_checkf(contiguous(&g_a) && contiguous(&g_b),
                 "each ring is numbered 0.. with no gap (%d, %d records)", g_a.n, g_b.n);
    utest_check(has_pid(&g_a, pa) && !has_pid(&g_a, pb) && has_pid(&g_b, pb) && !has_pid(&g_b, pa),
                "each ring holds its own program and never the other's");

    int leaf_a = nth_ret(&g_a, pa, SYS_SPAWN, 0);
    utest_checkf(leaf_a > 0 && opener(&g_a, LEAF_PATH) == leaf_a,
                 "FOLLOWED: the spawned grandchild's open() is in the ring, as pid %d", leaf_a);
    int fork_a = nth_ret(&g_a, pa, SYS_FORK, 0);
    utest_checkf(fork_a > 0 && opener(&g_a, FORK_PATH) == fork_a && exited(&g_a, fork_a),
                 "FOLLOWED: the forked child's open() and exit are in the ring, as pid %d", fork_a);
    int slow_a = nth_ret(&g_a, pa, SYS_SPAWN, 1);
    utest_checkf(live_after_reap > 0, "the child reaped, the ring still has %u writer(s) left",
                 live_after_reap);
    utest_checkf(slow_a > 0 && opener(&g_a, SLOW_PATH) == slow_a && exited(&g_a, slow_a),
                 "...the orphan that outlived it is traced to its exit (pid %d)", slow_a);
    utest_checkf(!g_a.h->live && !g_b.h->live, "...and live fell to 0 in both (%u, %u)",
                 g_a.h->live, g_b.h->live);

    int leaf_b = nth_ret(&g_b, pb, SYS_SPAWN, 0);
    utest_checkf(leaf_b > 0 && !has_pid(&g_b, leaf_b) && !opener(&g_b, LEAF_PATH) &&
                 !opener(&g_b, FORK_PATH),
                 "NOT FOLLOWED: the same program's children write nothing (pid %d absent)", leaf_b);
    return utest_end();
}
