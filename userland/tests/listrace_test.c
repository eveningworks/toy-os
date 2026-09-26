// TWO LISTINGS AT ONCE MUST EACH GET THEIR OWN DIRECTORY.
//
// SYS_LISTDIR kept its per-call state (the output buffer, the count, the
// path) in file-scope globals, on the grounds that syscalls never ran
// concurrently. They do: a walk that finds an inode busy waits with the
// mount lock DROPPED (mount_wait()), and another process's listing runs
// in the gap and re-arms every global. Measured at boot: init's listing
// of /etc/services.d came back holding the desktop's app entries, and
// the desktop's came back empty, 1 boot in 10. fs_list() carries a
// context now (Linux's dir_context), and this is the check.
//
// THE FIXTURE FORCES THE GAP rather than waiting for a cold boot to hit
// it: one thread keeps creating and deleting a file in A, so A's inode
// is often held EXCLUSIVELY and a listing of A has to wait -- lock
// dropped -- while a second thread lists B. Each lister clears its
// buffer before every call, so a listing whose entries went elsewhere
// shows up as a wrong count or a foreign name, not as stale content.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: nothing, if the waits happen
// -- which is why the fixture reports how many of A's listings ran
// while the churn thread was mid-cycle, and fails if it cannot show the
// two listers overlapped at all.
//
// It must be SPAWNED, not `run` -- threads need a scheduler slot.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "lib/utest.h"

#define A_DIR "/var/tmp/lr.a"
#define B_DIR "/var/tmp/lr.b"
#define N_FILES 16
#define MAX_ENTS 32

static volatile int g_stop;
static volatile unsigned g_churn, g_b_lists, g_b_bad;
static volatile unsigned g_b_listing;   // B is inside sys_listdir right now
static char g_b_first_bad[80];

static int make_dir_of(const char *dir, char prefix) {
    mkdir(dir, 0755);
    for (int i = 0; i < N_FILES; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s/%c%02d", dir, prefix, i);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) return 0;
        close(fd);
    }
    return 1;
}

static void remove_dir_of(const char *dir, char prefix) {
    for (int i = 0; i < N_FILES; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s/%c%02d", dir, prefix, i);
        unlink(p);
    }
    unlink(A_DIR "/achurn");
    rmdir(dir);
}

// A listing is right when it has exactly the directory's own names.
// `extra` allows A's churn file, which may or may not exist at the time.
static int listing_ok(const struct sys_dirent *e, int n, char prefix, int extra,
                      char *why, int why_cap) {
    if (n < N_FILES || n > N_FILES + extra) {
        snprintf(why, why_cap, "%d entries, wanted %d%s", n, N_FILES, extra ? "+" : "");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (e[i].name[0] != prefix) {
            snprintf(why, why_cap, "foreign name '%.40s'", e[i].name);
            return 0;
        }
    }
    return 1;
}

static void *churn(void *arg) {
    (void)arg;
    while (!g_stop) {
        int fd = open(A_DIR "/achurn", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) close(fd);
        unlink(A_DIR "/achurn");
        g_churn++;
    }
    return 0;
}

static struct sys_dirent g_b_ents[MAX_ENTS];

static void *lister_b(void *arg) {
    (void)arg;
    while (!g_stop) {
        memset(g_b_ents, 0, sizeof g_b_ents);
        g_b_listing = 1;
        int n = sys_listdir(B_DIR, g_b_ents, MAX_ENTS);
        g_b_listing = 0;
        char why[80];
        if (!listing_ok(g_b_ents, n, 'b', 0, why, sizeof why)) {
            if (!g_b_bad) snprintf(g_b_first_bad, sizeof g_b_first_bad, "%s", why);
            g_b_bad++;
        }
        g_b_lists++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = 3;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    utest_begin("listrace_test", "two directory listings at once, each its own",
                UTEST_VERDICT_FILE);

    utest_check(make_dir_of(A_DIR, 'a') && make_dir_of(B_DIR, 'b'),
                "made two directories of 16 files each");

    pthread_t tc, tb;
    int started = pthread_create(&tc, NULL, churn, NULL) == 0 &&
                  pthread_create(&tb, NULL, lister_b, NULL) == 0;
    utest_check(started, "started the churn thread and the second lister");

    static struct sys_dirent ents[MAX_ENTS];
    unsigned a_lists = 0, a_bad = 0, overlapped = 0;
    char a_first_bad[80] = "";
    unsigned long long end = sys_monotonic_ns() + (unsigned long long)secs * 1000000000ull;
    while (started && sys_monotonic_ns() < end) {
        memset(ents, 0, sizeof ents);
        int n = sys_listdir(A_DIR, ents, MAX_ENTS);
        if (g_b_listing) overlapped++;
        char why[80];
        if (!listing_ok(ents, n, 'a', 1, why, sizeof why)) {
            if (!a_bad) snprintf(a_first_bad, sizeof a_first_bad, "%s", why);
            a_bad++;
        }
        a_lists++;
    }
    g_stop = 1;
    if (started) { pthread_join(tc, NULL); pthread_join(tb, NULL); }
    remove_dir_of(A_DIR, 'a');
    remove_dir_of(B_DIR, 'b');

    // The fixture reached the code: both listed, the churn ran, and the
    // two listers were inside the call together at least sometimes.
    utest_checkf(a_lists > 0 && g_b_lists > 0 && g_churn > 0 && overlapped > 0,
                 "listed A %u and B %u times against %u churn cycles, %u overlapping",
                 a_lists, g_b_lists, g_churn, overlapped);
    utest_checkf(a_bad == 0, "every listing of A was A's own: %u wrong%s%s",
                 a_bad, a_bad ? ", first: " : "", a_first_bad);
    utest_checkf(g_b_bad == 0, "every listing of B was B's own: %u wrong%s%s",
                 g_b_bad, g_b_bad ? ", first: " : "", g_b_first_bad);
    return utest_end();
}
