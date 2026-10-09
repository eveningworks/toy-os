// strace's RECORD RING (abi/trace_abi.h, docs/trace-design.md stage 1):
// this program traces ITSELF, run as `tracering_test child`, into a one-page
// ring it drains slowly, and checks what came back.
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which shaped every check:
//   - "records arrived" passes a kernel that wrote the same slot over
//     and over, so the sequence numbers must be contiguous from 0;
//   - a ring of 31 drained only once full MUST stall the child, so the
//     stall count has to be non-zero AND nothing may be missing -- a
//     kernel that dropped on a full ring fails the count of getpid() pairs;
//   - the parked waitpid() is checked against the pid spawn() returned,
//     a value nothing else in the trace carries.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "trace_abi.h"
#include "lib/utest.h"

#define RING_BYTES 4096           // header + 31 records: small on purpose
#define GETPIDS    120
#define MAX_RECS   1024
#define BAD_PATH   "/trace-test/no-such-path"

static struct trace_rec g_recs[MAX_RECS];
static int g_n;

static int child(void) {
    sys_open(BAD_PATH, 0);
    sys_write(1, "trace-me\n", 9);
    int pid = sys_spawn("/bin/sleep", "1", -1);   // long enough that waitpid parks
    int code = 0;
    if (pid > 0) sys_waitpid(pid, &code);
    for (int i = 0; i < GETPIDS; i++) sys_getpid();
    sys_exit(0);
}

static void drain(struct trace_ring_hdr *h) {
    const char *base = (const char *)h + TRACE_HDR_SIZE;
    while (h->tail != h->head) {
        const struct trace_rec *r =
            (const struct trace_rec *)(base + (h->tail % h->nrec) * TRACE_REC_SIZE);
        if (g_n < MAX_RECS) g_recs[g_n++] = *r;
        h->tail++;
    }
}

static const struct trace_rec *find(int nr, int kind, int from) {
    for (int i = from; i < g_n; i++)
        if (g_recs[i].nr == nr && g_recs[i].kind == kind) return &g_recs[i];
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child();

    utest_begin("tracering_test", "strace's record ring", UTEST_VERDICT_FILE);
    char name[32];
    snprintf(name, sizeof name, "tracering_test.%d", sys_getpid());
    int fd = sys_shm_open(name, RING_BYTES, SHM_CREATE | SHM_EXCL);
    void *p = fd >= 0
        ? sys_mmap(0, RING_BYTES, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0) : 0;
    if (fd >= 0) sys_close(fd);
    int mapped = p && p != (void *)-1;   // mmap's MAP_FAILED
    utest_check(mapped, "the ring is created and mapped");
    if (!mapped) return utest_end();
    struct trace_ring_hdr *h = p;
    h->magic = TRACE_RING_MAGIC;
    h->nrec = (RING_BYTES - TRACE_HDR_SIZE) / TRACE_REC_SIZE;

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.args = "child";
    o.flags = SPAWN_TRACE;
    o.trace_ring = name;
    int pid = sys_spawn_opts("/tests/tracering_test", &o);
    utest_checkf(pid > 0, "the traced child spawns (%d)", pid);
    if (pid <= 0) { sys_shm_unlink(name); return utest_end(); }

    // NOT DRAINED UNTIL THE CHILD HAS STALLED, so it must stall whatever
    // the machine's speed -- a drain on a timer raced it, and under load
    // the child lost (0 stalls). After that, drained whenever it is full.
    int code = 0, done = 0;
    for (int spins = 0; spins < 20000 && !done; spins++) {
        done = sys_waitpid_nohang(pid, &code) == pid;
        if (done || (h->stalls > 0 && h->head - h->tail >= h->nrec - 1)) drain(h);
        else sys_sleep_ms(2);
    }
    drain(h);
    sys_shm_unlink(name);

    int contiguous = g_n > 0;
    for (int i = 0; i < g_n; i++) if (g_recs[i].seq != (uint32_t)i) contiguous = 0;
    utest_checkf(contiguous, "%d records, numbered 0.. with no gap", g_n);

    const struct trace_rec *op = find(SYS_OPEN, TRACE_ENTRY, 0);
    utest_check(op && op->blob_arg == 0 && op->blob_len == strlen(BAD_PATH) &&
                !memcmp(op->blob, BAD_PATH, op->blob_len),
                "open's entry carries the path, copied at the call");
    const struct trace_rec *ope = find(SYS_OPEN, TRACE_EXIT, 0);
    utest_checkf(ope && ope->ret < 0, "...and its exit the error (%lld)", ope ? (long long)ope->ret : 0);

    const struct trace_rec *wr = find(SYS_WRITE, TRACE_ENTRY, 0);
    utest_check(wr && wr->a[0] == 1 && wr->a[2] == 9 && wr->blob_arg == 1 &&
                wr->blob_len == 9 && !memcmp(wr->blob, "trace-me\n", 9),
                "write's entry carries fd, length and the bytes");

    const struct trace_rec *sp = find(SYS_SPAWN, TRACE_EXIT, 0);
    const struct trace_rec *wres = find(SYS_WAITPID, TRACE_RESUMED, 0);
    utest_check(wres != 0, "the waitpid that parked has a RESUMED exit, not \"= ?\"");
    int reaped = 0;
    for (int i = 0; sp && i < g_n; i++)
        if (g_recs[i].nr == SYS_WAITPID && g_recs[i].kind != TRACE_ENTRY && g_recs[i].ret == sp->ret)
            reaped = 1;
    utest_checkf(sp && sp->ret > 0 && reaped,
                 "a waitpid exit returns the pid spawn returned (%lld)", sp ? (long long)sp->ret : 0);

    int pairs = 0;
    for (int i = 0; i + 1 < g_n; i++)
        if (g_recs[i].nr == SYS_GETPID && g_recs[i].kind == TRACE_ENTRY &&
            g_recs[i + 1].nr == SYS_GETPID && g_recs[i + 1].kind == TRACE_EXIT &&
            g_recs[i + 1].ret == pid)
            pairs++;
    utest_checkf(pairs == GETPIDS, "every getpid() has its entry and exit, in order (%d of %d)",
                 pairs, GETPIDS);
    utest_checkf(h->stalls > 0, "the ring filled and the child waited (%u stalls), losing nothing",
                 h->stalls);
    utest_check(g_n > 0 && g_recs[g_n - 1].nr == SYS_EXIT && g_recs[g_n - 1].kind == TRACE_NORETURN,
                "the last record is the exit, which never returns");
    return utest_end();
}
