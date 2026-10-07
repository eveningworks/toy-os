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
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/utest.h"

static struct fs_check_result r;

static int check(const char *path, unsigned flags) {
    memset(&r, 0xAA, sizeof r);
    return sys_fs_check(path, flags, &r);
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

    // The root was just repaired, so it is clean: 0, not 1 or 4.
    utest_check(run_fsck("") == 0, "/bin/fsck exits 0 on a clean root");
    utest_check(run_fsck("/tmp") == 0, "...and checks the volume it is given");
    utest_check(run_fsck("-r /tmp") == 8, "...and 8 when the kernel refuses the check");
    utest_check(run_fsck("--frob") == 2, "...and 2 for a usage error");
    return utest_end();
}
