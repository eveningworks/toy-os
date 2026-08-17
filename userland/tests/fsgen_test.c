// SYS_FS_GENERATION from ring 3 -- a self-checking diagnostic, run by
// tools/usertest_run.py as well as by hand (`run fsgen_test`).
//
// The counter exists so the desktop can ask "has anything changed?"
// once per frame without listing a directory (Milestone 41 stage 4a;
// see abi/syscall_abi.h). So the contract has two halves, and the
// SECOND one is the half a plausible implementation gets wrong:
//
//   * it MOVES when the filesystem is mutated -- otherwise a cache
//     built on it goes stale and the live reload silently dies;
//   * it is STABLE when nothing happened. A counter wired to the timer,
//     or bumped by the read itself, satisfies the first half perfectly
//     and makes the desktop re-read its whole app directory every
//     frame -- which is the exact cost the counter was introduced to
//     avoid, and it would look like a working feature.
//
// Hence every mutation check here is paired with a no-op check. Asking
// "did it change?" alone would pass against a free-running counter.
#include "rt/sys.h"
#include "lib/string.h"

static int failures;

static void check(int ok, const char *what) {
    sys_print(ok ? "  ok   " : "  FAIL ");
    sys_print(what);
    sys_print("\n");
    if (!ok) failures++;
}

int main(void) {
    sys_print("fsgen_test: SYS_FS_GENERATION\n");

    unsigned long long g0 = sys_fs_generation();
    check(g0 != 0, "non-zero with a filesystem mounted");

    // The control. Two reads with nothing in between must agree -- a
    // clock, a tick count or a read-side increment all fail here and
    // pass every other check in this file.
    check(sys_fs_generation() == g0, "stable across two reads");

    // A read is not a mutation either: opening and closing a file that
    // already exists must not move it.
    int fd = sys_open("/tests/fsgen_test", 0);
    if (fd >= 0) sys_close(fd);
    check(sys_fs_generation() == g0, "stable across a read-only open");

    // --- a real mutation ------------------------------------------
    fd = sys_open("/tmp/fsgen.tmp", SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    check(fd >= 0, "created a file to mutate with");
    if (fd >= 0) {
        const char *msg = "generation";
        sys_write(fd, msg, (int)strlen(msg));
        sys_close(fd);
    }

    unsigned long long g1 = sys_fs_generation();
    check(g1 != g0, "moved after a write");
    check(g1 > g0, "only ever increases");
    check(sys_fs_generation() == g1, "stable again once the write is done");

    // Deleting is a mutation too, and it also cleans up after this run
    // -- usertest_run.py works against a copy, but `run fsgen_test` by
    // hand does not, and a test that litters is a test that changes the
    // next one's starting conditions.
    sys_unlink("/tmp/fsgen.tmp");
    check(sys_fs_generation() > g1, "moved after a delete");

    if (failures == 0) sys_print("fsgen_test: all checks passed\n");
    else sys_print("fsgen_test: FAILURES\n");
    return failures == 0 ? 0 : 1;
}
