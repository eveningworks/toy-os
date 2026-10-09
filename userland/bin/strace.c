// /bin/strace -- run a program and print the syscalls it makes.
//
// THE KERNEL RECORDS, THIS DECODES (docs/trace-design.md): the program
// is spawned with SPAWN_TRACE and a ring this process created, the
// kernel writes an entry and an exit record per syscall into it
// (abi/trace_abi.h), and lib/utrace.h turns them into lines -- FreeBSD's
// ktrace and kdump in one program. Filtering (-e), counting (-c) and a
// file (-o) are ring 3's for free: they are things a reader does to a
// stream it already has.
//
// THE TRACE GOES TO STDERR, as real strace's does, so `strace foo |
// grep x` greps foo's output and never the trace -- fd 2 is the terminal
// whenever there is one (and the kernel log under /bin/spawn, which is
// where a background trace belongs).
//
// A FULL RING STALLS THE TRACEE, never drops a record: the kernel
// re-issues the call until this has drained. So this drains promptly --
// it sleeps on its wakeword, which the kernel bumps per record -- and a
// slow output only slows the program being traced.
#include "rt/sys.h"
#include "syscall_abi.h"
#include "trace_abi.h"
#include "lib/upath.h"
#include "lib/uargs.h"
#include "lib/utrace.h"
#include <stdio.h>
#include <string.h>

#define RING_BYTES (64 * 1024)   // header + 511 records
#define MAX_ARGV   64

static const char *g_out_path, *g_filter;
static int g_count, g_follow;

static const struct uargs_opt OPTS[] = {
    { "output",  'o', "FILE", "write the trace to FILE instead of stderr", 0, &g_out_path },
    { "trace",   'e', "LIST", "only these syscalls: trace=open,read or open,read", 0, &g_filter },
    { "summary", 'c', 0,      "count calls and errors per syscall, and print only that table", &g_count, 0 },
    { "follow",  'f', 0,      "trace PROGRAM's children too, each line marked [pid N]", &g_follow, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "strace",
    .usage = "[-c] [-f] [-o FILE] [-e LIST] PROGRAM [ARG]...",
    .summary = "Run PROGRAM and print every syscall it makes: its arguments as the\n"
               "kernel saw them and what it returned. Exits with PROGRAM's status.",
    .opts = OPTS,
    .notes = "The trace goes to stderr; PROGRAM's own output is left alone. Without -f\n"
             "its children are not traced; with it, strace waits until the last of\n"
             "them has exited too.",
    .first_operand_ends_options = 1,
};

static int g_out = 2;
static unsigned char g_want[1024];   // -e: syscall numbers to show; empty = all
static int g_filtering;
static unsigned g_calls[1024], g_errors[1024];

static void emit(void *ctx, const char *line) {
    (void)ctx;
    char buf[400];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    sys_write(g_out, buf, (size_t)n);
}

static int parse_filter(const char *s) {
    if (!strncmp(s, "trace=", 6)) s += 6;
    while (*s) {
        char name[32];
        size_t n = strcspn(s, ",");
        if (n == 0 || n >= sizeof name) return uargs_error(&PROG, "bad -e list near '%s'", s);
        memcpy(name, s, n);
        name[n] = 0;
        int nr = utrace_lookup(name);
        if (nr < 0 || nr >= (int)sizeof g_want) return uargs_error(&PROG, "no syscall named '%s'", name);
        g_want[nr] = 1;
        s += n + (s[n] == ',');
    }
    g_filtering = 1;
    return 0;
}

static struct utrace_printer g_pr;

static void take(const struct trace_rec *r) {
    if (g_filtering && (r->nr >= sizeof g_want || !g_want[r->nr])) return;
    if (g_count) {
        if (r->nr < sizeof g_calls / sizeof g_calls[0]) {
            if (r->kind == TRACE_ENTRY) g_calls[r->nr]++;
            else if (utrace_failed(r)) g_errors[r->nr]++;
        }
        return;
    }
    utrace_feed(&g_pr, r);
}

static void drain(struct trace_ring_hdr *h) {
    const char *base = (const char *)h + TRACE_HDR_SIZE;
    while (h->tail != h->head) {
        const struct trace_rec *r =
            (const struct trace_rec *)(base + (h->tail % h->nrec) * TRACE_REC_SIZE);
        take(r);
        h->tail++;
    }
}

// Linux's -c table, without its time columns: the records carry none.
static void summary(void) {
    char line[96];
    unsigned calls = 0, errors = 0;
    emit(0, "calls  errors syscall");
    emit(0, "------ ------ ----------------");
    // Most-called first; a selection sort over a table this small.
    static unsigned char done[1024];
    for (;;) {
        int best = -1;
        for (int i = 0; i < 1024; i++)
            if (g_calls[i] && !done[i] && (best < 0 || g_calls[i] > g_calls[best])) best = i;
        if (best < 0) break;
        done[best] = 1;
        const char *name = utrace_name(best);
        if (g_errors[best])
            snprintf(line, sizeof line, "%6u %6u %s", g_calls[best], g_errors[best], name ? name : "?");
        else
            snprintf(line, sizeof line, "%6u        %s", g_calls[best], name ? name : "?");
        emit(0, line);
        calls += g_calls[best];
        errors += g_errors[best];
    }
    emit(0, "------ ------ ----------------");
    snprintf(line, sizeof line, "%6u %6u total", calls, errors);
    emit(0, line);
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 1) return uargs_error(&PROG, "a PROGRAM to run");
    if (g_filter && parse_filter(g_filter)) return UARGS_USAGE;

    char path[UPATH_MAX];
    int found = upath_find_program(a.argv[0], path, sizeof path);
    if (found <= 0) {
        fprintf(stderr, "strace: %s: no such program\n", a.argv[0]);
        return 1;
    }
    if (g_out_path) {
        g_out = sys_open(g_out_path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
        if (g_out < 0) {
            fprintf(stderr, "strace: %s: %s\n", g_out_path, sys_strerror(sys_errno()));
            return 1;
        }
    }

    // THE RING: ours, so the kernel accepts it (only a ring the tracer
    // created can be named) and gone when we unlink it.
    char ring[32];
    snprintf(ring, sizeof ring, "strace.%d", sys_getpid());
    int fd = sys_shm_open(ring, RING_BYTES, SHM_CREATE | SHM_EXCL);
    void *p = fd >= 0
        ? sys_mmap(0, RING_BYTES, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0) : 0;
    if (fd >= 0) sys_close(fd);
    if (!p || p == (void *)-1) {
        fprintf(stderr, "strace: no memory for the trace ring\n");
        return 1;
    }
    struct trace_ring_hdr *h = p;
    h->magic = TRACE_RING_MAGIC;
    h->nrec = (RING_BYTES - TRACE_HDR_SIZE) / TRACE_REC_SIZE;

    static volatile uint32_t word;
    sys_wakeword(&word);
    utrace_printer_init(&g_pr, emit, 0);

    char *cargv[MAX_ARGV + 1];
    int n = a.argc < MAX_ARGV ? a.argc : MAX_ARGV;
    cargv[0] = path;
    for (int i = 1; i < n; i++) cargv[i] = a.argv[i];
    cargv[n] = 0;

    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = cargv;
    o.env = environ;
    o.flags = SPAWN_TRACE | (g_follow ? SPAWN_TRACE_FOLLOW : 0);
    o.trace_ring = ring;
    int pid = sys_spawn_opts(path, &o);
    if (pid <= 0) {
        fprintf(stderr, "strace: %s: %s\n", path, sys_strerror(sys_errno()));
        sys_shm_unlink(ring);
        return 1;
    }

    if (g_follow) g_pr.main_pid = pid;

    // DONE IS THE CHILD REAPED AND, with -f, NOTHING LEFT WRITING: a
    // grandchild can outlive it, and its records still belong here. The
    // kernel keeps `live`; Linux strace -f likewise waits for every tracee.
    int code = 0, reaped = 0;
    for (;;) {
        uint32_t seen = word;
        drain(h);
        // SYS_RETRY ("still running") is negative too: only -1 is an error.
        if (!reaped) {
            int r = sys_waitpid_nohang(pid, &code);
            reaped = r == pid || r == -1;
        }
        if (reaped && (!g_follow || h->live == 0)) break;
        sys_futex_wait(&word, seen, 100);
    }
    drain(h);
    sys_shm_unlink(ring);

    if (g_count) {
        summary();
    } else {
        utrace_flush(&g_pr);
        char line[48];
        snprintf(line, sizeof line, "+++ exited with %d +++", code);
        emit(0, line);
    }
    return code;
}
