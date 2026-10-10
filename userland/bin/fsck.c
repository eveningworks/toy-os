// fsck -- check a mounted filesystem, and repair what can be repaired
// without guessing.
//
// THE CHECK IS THE KERNEL'S (SYS_FS_CHECK); this prints it. It walks the
// backend's live state and repairs through its journal, which is why it
// is not a ring-3 checker over the raw device -- abi/mount_abi.h has the
// reasoning, and fs.h's fs_check() what a repair will and will not fix.
//
// The exit status is e2fsck's, so a script can tell the cases apart:
// 0 clean, 1 problems found and all of them fixed, 4 problems left,
// 8 the check itself failed, 32 stopped by Ctrl-C. 2 is a usage error,
// as for every lib/uargs program.
//
// The call blocks for the whole pass, so it runs on a second thread and
// this one draws the stage line (e2fsck -C) from FSCK_PROGRESS -- on a
// terminal only, so a captured stderr does not fill with repaints.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/uargs.h"

#define FSCK_CLEAN     0
#define FSCK_CORRECTED 1
#define FSCK_LEFT      4
#define FSCK_FAILED    8
#define FSCK_CANCELED  32

#define REDRAW_MS 200

static int g_repair;

static const struct uargs_opt OPTS[] = {
    { "repair", 'r', 0, "also fix what can be fixed without guessing", &g_repair, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "fsck",
    .usage = "[-r] [PATH]",
    .summary = "Check the mounted filesystem holding PATH (default /), in the kernel,\n"
               "and with -r also reclaim leaked blocks and fix what is safely fixable.",
    .opts = OPTS,
    .notes = "`fsck repair` is the same as `fsck -r`.\n"
             "Exit status: 0 clean, 1 all problems fixed, 4 problems left, 8 the check failed.",
};

static int problems(const struct fs_check_result *r) {
    return r->leaked || r->referenced_but_free || r->double_allocated || r->out_of_range;
}

static const char *g_path;
static struct fs_check_result g_r;
static int g_rc, g_err;
static volatile int g_done, g_interrupted;

static void *worker(void *arg) {
    (void)arg;
    g_rc = sys_fs_check(g_path, g_repair ? FSCK_REPAIR : 0, &g_r);
    g_err = g_rc ? errno : 0;
    __atomic_store_n(&g_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void on_interrupt(int sig) {
    (void)sig;
    g_interrupted = 1;
}

// "fsck: stage 2 of 3, compare the allocation maps: 18 of 28 groups (64%)",
// over the last one; `width` is that one's length, so a shorter line
// leaves no tail.
static void draw_line(int *width) {
    struct fs_check_progress p;
    if (sys_fs_check_progress(g_path, &p) || !p.running || !p.stages) return;
    unsigned st = p.stage < FSCK_STAGES_MAX ? p.stage : 0;
    char line[128], name[32];
    snprintf(name, sizeof name, "%s", p.names[st]);
    if (name[0] >= 'A' && name[0] <= 'Z') name[0] = (char)(name[0] - 'A' + 'a');
    int n = snprintf(line, sizeof line, "fsck: stage %u of %u, %s", st + 1, p.stages, name);
    if (p.total && n > 0 && n < (int)sizeof line) {
        unsigned done = p.done > p.total ? p.total : p.done;
        snprintf(line + n, sizeof line - (size_t)n, ": %u of %u %s (%u%%)", done, p.total, p.units[st],
                 (unsigned)((unsigned long long)done * 100 / p.total));
    }
    int len = (int)strlen(line);
    char out[192];   // ONE write: stderr is unbuffered, and fprintf would make it one per byte
    int n2 = snprintf(out, sizeof out, "\r%s%*s", line, *width > len ? *width - len : 0, "");
    if (n2 > 0) write(2, out, (size_t)(n2 < (int)sizeof out ? n2 : (int)sizeof out - 1));
    *width = len;
}

// Runs the check on a worker and waits for it, drawing and passing a
// Ctrl-C on as FSCK_STOP. 0 or -1 with errno, as sys_fs_check() would.
static int run_check(void) {
    // THE PASS HOLDS THE VOLUME, AND A PAGE OF THIS PROGRAM OR ITS
    // LIBRARIES NOT YET READ IN IS READ FROM IT: a Ctrl-C whose handler
    // or stop call is still on disk would wait for the very pass it ends.
    // Run both once first; with nothing running the stop is -ESRCH.
    struct fs_check_progress warm;
    sys_fs_check_progress(g_path, &warm);
    sys_fs_check_stop(g_path);
    on_interrupt(0);
    g_interrupted = 0;

    // THE WORKER IS BORN WITH SIGINT BLOCKED, so only this thread takes
    // it: the kernel builds a signal frame on the main stack alone, and
    // a handled signal reaching another thread ends the program (bugs.md).
    signal(SIGINT, on_interrupt);
    sigset_t intr, old;
    sigemptyset(&intr);
    sigaddset(&intr, SIGINT);
    sigprocmask(SIG_BLOCK, &intr, &old);
    pthread_t th;
    int made = pthread_create(&th, 0, worker, 0) == 0;
    sigprocmask(SIG_SETMASK, &old, 0);
    if (!made) {   // no thread: no line, same answer
        worker(0);
        signal(SIGINT, SIG_DFL);
        errno = g_err;
        return g_rc;
    }
    int tty = isatty(2), width = 0, told = 0;
    while (!__atomic_load_n(&g_done, __ATOMIC_ACQUIRE)) {
        usleep(REDRAW_MS * 1000);
        // Asked every redraw until this pass ends: ESRCH is a pass not
        // begun yet, EBUSY someone else's repair ahead of it, and a 0 may
        // have stopped another program's check rather than this one.
        if (g_interrupted && g_repair && !told) {
            fprintf(stderr, "%sfsck: a repair cannot be stopped part way\n", width ? "\n" : "");
            width = 0;
            told = 1;
        } else if (g_interrupted && !g_repair) {
            sys_fs_check_stop(g_path);
        }
        if (tty) draw_line(&width);
    }
    pthread_join(th, 0);
    signal(SIGINT, SIG_DFL);
    if (width) {
        char blank[160];
        int n = snprintf(blank, sizeof blank, "\r%*s\r", width < 150 ? width : 150, "");
        if (n > 0) write(2, blank, (size_t)n);
    }
    errno = g_err;
    return g_rc;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    int at = 0;
    // The kernel shell's builtin took the word; its messages still say it.
    if (a.argc > at && !strcmp(a.argv[at], "repair")) { g_repair = 1; at++; }
    const char *path = a.argc > at ? a.argv[at++] : "/";
    if (a.argc > at) return uargs_error(&PROG, "unexpected argument '%s'", a.argv[at]);

    g_path = path;
    if (run_check() != 0) {
        int e = errno;
        if (e == ECANCELED) {
            fprintf(stderr, "fsck: %s: stopped part way; nothing was changed\n", path);
            return FSCK_CANCELED;
        }
        if (e == ENOTSUP)
            fprintf(stderr, "fsck: %s: this filesystem can be checked but not repaired\n", path);
        else if (e == EROFS)
            fprintf(stderr, "fsck: %s: mounted read-only, so it cannot be repaired\n", path);
        else
            fprintf(stderr, "fsck: %s: %s\n", path, strerror(e));
        return FSCK_FAILED;
    }

    printf("fsck: %s %s on %s\n", g_repair ? "checked and repaired" : "checked (read-only)",
           g_r.fstype, g_r.point);
    printf("  records in use:        %u\n", g_r.records_used);
    printf("  blocks referenced:     %u\n", g_r.blocks_referenced);
    printf("  leaked (unreferenced): %u\n", g_r.leaked);
    printf("  referenced but free:   %u\n", g_r.referenced_but_free);
    printf("  double-allocated:      %u\n", g_r.double_allocated);
    printf("  out-of-range pointers: %u\n", g_r.out_of_range);

    if (g_repair) {
        printf("  -- repaired --\n");
        printf("  blocks reclaimed:      %u (%u KB)\n", g_r.reclaimed, g_r.reclaimed * 4);
        printf("  marked allocated:      %u\n", g_r.marked_allocated);
        printf("  pointers cleared:      %u\n", g_r.pointers_cleared);
    } else if (g_r.leaked || g_r.referenced_but_free || g_r.out_of_range) {
        printf("Run `fsck -r %s` to reclaim %u leaked block(s) (%u KB) and fix the rest.\n",
               path, g_r.leaked, g_r.leaked * 4);
    }

    if (g_r.double_allocated) {
        // Never repaired -- choosing which file keeps a shared block
        // destroys the other's data (fs.h).
        printf("WARNING: blocks claimed by more than one file. `fsck -r` will NOT fix\n"
               "this; delete one of the affected files to resolve it.\n");
    }

    if (!problems(&g_r)) {
        printf("fsck: clean.\n");
        return FSCK_CLEAN;
    }
    // A repair pass reports what it FOUND; it fixed all of it unless a
    // double allocation, which it never touches, was among it.
    return g_repair && !g_r.double_allocated ? FSCK_CORRECTED : FSCK_LEFT;
}
