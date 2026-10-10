// SYS_FS_CHECK and /bin/fsck: the kernel's consistency check, asked for
// from ring 3 by a path on the volume.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a syscall that ignored its
// path and always checked the root would pass every check on "/" -- so
// /tmp (a ramfs) and a RELATIVE path into it must come back named as
// /tmp. One that refused every repair would pass the -ENOTSUP and
// -EROFS checks, so the root's repair must succeed. And a /bin/fsck that
// printed without asking would still exit 0, so its status is compared
// against a broken invocation's.
//
// FSCK_PROGRESS and FSCK_STOP are asked about a pass ANOTHER THREAD is
// in, which is the only way they are ever used: a progress read that
// waited for the volume's lock would only ever see an idle volume, and
// a stop that was ignored would let the pass end with 0 -- so the pass
// must be SEEN running, by name, and must come back ECANCELED.
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "rt/sys.h"
#include "lib/utest.h"

static struct fs_check_result r;

static int check(const char *path, unsigned flags) {
    memset(&r, 0xAA, sizeof r);
    return sys_fs_check(path, flags, &r);
}

// A pass on a second thread, and what it returned.
static struct fs_check_result g_bg;
static int g_bg_rc, g_bg_err;
static volatile int g_bg_done;

static void *bg_check(void *flags) {
    g_bg_rc = sys_fs_check("/", (unsigned)(unsigned long)flags, &g_bg);
    g_bg_err = g_bg_rc ? errno : 0;
    __atomic_store_n(&g_bg_done, 1, __ATOMIC_RELEASE);
    return 0;
}

// Starts one and waits, bounded, until a progress read sees it running.
static int start_bg(pthread_t *th, unsigned flags, struct fs_check_progress *p) {
    g_bg_done = 0;
    if (pthread_create(th, 0, bg_check, (void *)(unsigned long)flags)) return 0;
    for (int i = 0; i < 5000 && !__atomic_load_n(&g_bg_done, __ATOMIC_ACQUIRE); i++) {
        if (sys_fs_check_progress("/", p) == 0 && p->running && p->stages) return 1;
        sys_sleep_ms(1);
    }
    return 0;
}

static int run_fsck(const char *args) {
    int pid = sys_spawn("/bin/fsck", args, -1);
    int code = -1;
    if (pid < 0 || sys_waitpid(pid, &code) < 0) return -1;
    return code;
}

int main(void) {
    utest_begin("fscheck_test", "SYS_FS_CHECK and /bin/fsck", UTEST_VERDICT_FILE);

    utest_check(check("/", 0) == 0, "the root checks");
    utest_check(!strcmp(r.point, "/") && !strcmp(r.fstype, "tfs3"), "...and is named tfs3 on /");
    utest_check(r.records_used > 0 && r.blocks_referenced > 0, "...with records and blocks counted");
    utest_check(r.reclaimed == 0 && r.marked_allocated == 0 && r.pointers_cleared == 0,
                "...and a read-only pass changed nothing");

    utest_check(check("/tmp", 0) == 0 && !strcmp(r.point, "/tmp") && !strcmp(r.fstype, "ramfs"),
                "a path names ITS volume, not the root");
    utest_check(sys_chdir("/tmp") == 0 && check(".", 0) == 0 && !strcmp(r.point, "/tmp"),
                "a relative path resolves against the cwd");
    sys_chdir("/");

    utest_check(check("/tmp", FSCK_REPAIR) == -1 && errno == ENOTSUP,
                "a repair a backend cannot do is ENOTSUP");
    if (check("/boot", 0) == 0 && !strcmp(r.point, "/boot"))
        utest_check(check("/boot", FSCK_REPAIR) == -1 && errno == EROFS,
                    "a repair on the read-only /boot is EROFS");
    utest_check(check("/", 0x80) == -1 && errno == EINVAL, "an unknown flag is EINVAL");
    utest_check(sys_fs_check("/", 0, (struct fs_check_result *)8) == -1 && errno == EFAULT,
                "a bad result pointer is EFAULT");
    utest_check(check("/", FSCK_REPAIR) == 0 && !strcmp(r.point, "/"),
                "a repair of the root runs");

    struct fs_check_progress p;
    utest_check(sys_fs_check_progress("/", &p) == 0 && !p.running, "an idle volume reports no pass running");
    utest_check(sys_fs_check_stop("/") == -1 && errno == ESRCH, "...and a stop there is ESRCH");
    utest_check(sys_fs_check("/", FSCK_PROGRESS | FSCK_REPAIR, &r) == -1 && errno == EINVAL,
                "a progress read mixed with a check is EINVAL");

    pthread_t th;
    int seen = start_bg(&th, 0, &p);
    utest_check_detail(seen, "a read-only pass is SEEN running from another thread",
                       "it finished, or never started, before a progress read caught it");
    if (seen) {
        utest_check(p.stages == 3 && !strcmp(p.names[0], "Walk every file") && !p.repair,
                    "...with tfs3's three stages, by name, and not a repair");
        unsigned long long first = (unsigned long long)p.stage << 32 | p.done;
        struct fs_check_progress q;
        sys_sleep_ms(20);
        int again = sys_fs_check_progress("/", &q) == 0;
        utest_check(!again || !q.running || ((unsigned long long)q.stage << 32 | q.done) >= first,
                    "...and a later read is no earlier");
        utest_check(sys_fs_check_stop("/") == 0, "...and a stop is taken");
    }
    pthread_join(th, 0);
    if (seen) {
        utest_check(g_bg_rc == -1 && g_bg_err == ECANCELED, "...and the pass returns ECANCELED");
        utest_check(sys_fs_check_progress("/", &p) == 0 && !p.running, "...and is over");
    }

    seen = start_bg(&th, FSCK_REPAIR, &p);
    utest_check_detail(seen, "a repair is seen running too", "it was not caught running");
    if (seen) {
        utest_check(p.repair == 1, "...and says it is a repair");
        utest_check(sys_fs_check_stop("/") == -1 && errno == EBUSY, "...and refuses a stop with EBUSY");
    }
    pthread_join(th, 0);
    if (seen) utest_check(g_bg_rc == 0, "...and runs to the end");

    // The root was just repaired, so it is clean: 0, not 1 or 4.
    utest_check(run_fsck("") == 0, "/bin/fsck exits 0 on a clean root");
    utest_check(run_fsck("/tmp") == 0, "...and checks the volume it is given");
    utest_check(run_fsck("-r /tmp") == 8, "...and 8 when the kernel refuses the check");
    utest_check(run_fsck("--frob") == 2, "...and 2 for a usage error");
    return utest_end();
}
