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
#include <stdio.h>
#include <string.h>

#include "lib/utest.h"

int main(void) {
    utest_begin("fsgen_test", "SYS_FS_GENERATION", 0);

    unsigned long long g0 = sys_fs_generation();
    utest_check(g0 != 0, "non-zero with a filesystem mounted");

    // The control. Two reads with nothing in between must agree -- a
    // clock, a tick count or a read-side increment all fail here and
    // pass every other check in this file.
    utest_check(sys_fs_generation() == g0, "stable across two reads");

    // A read is not a mutation either: opening and closing a file that
    // already exists must not move it.
    int fd = sys_open("/tests/fsgen_test", 0);
    if (fd >= 0) sys_close(fd);
    utest_check(sys_fs_generation() == g0, "stable across a read-only open");

    // --- a real mutation ------------------------------------------
    fd = sys_open(utest_path(TMP_VOLATILE, "fsgen.tmp"), SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    utest_check(fd >= 0, "created a file to mutate with");
    if (fd >= 0) {
        const char *msg = "generation";
        sys_write(fd, msg, (int)strlen(msg));
        sys_close(fd);
    }

    unsigned long long g1 = sys_fs_generation();
    utest_check(g1 != g0, "moved after a write");
    utest_check(g1 > g0, "only ever increases");
    utest_check(sys_fs_generation() == g1, "stable again once the write is done");

    // Deleting is a mutation too, and it also cleans up after this run
    // -- usertest_run.py works against a copy, but `run fsgen_test` by
    // hand does not, and a test that litters is a test that changes the
    // next one's starting conditions.
    sys_unlink(utest_path(TMP_VOLATILE, "fsgen.tmp"));
    utest_check(sys_fs_generation() > g1, "moved after a delete");

    // --- SYS_FS_GENERATION_OF: the same, scoped to one directory ------
    // The half that makes it worth having is the one a global counter
    // fails: a write in ANOTHER directory must not move it. (A hash
    // collision could -- 1 in 256; under the default /tmp these two
    // names were checked not to share a bucket.)
    char a[96], b[96], fa[112];
    strcpy(a, utest_path(TMP_VOLATILE, "fsgenA"));
    strcpy(b, utest_path(TMP_VOLATILE, "fsgenB"));
    snprintf(fa, sizeof fa, "%s/f", a);
    sys_mkdir(a);
    sys_mkdir(b);
    long long ga = sys_fs_generation_of(a), gb = sys_fs_generation_of(b);
    utest_check(ga > 0 && gb > 0, "a directory's generation is non-zero");
    utest_check(sys_fs_generation_of(a) == ga, "a directory's generation is stable across reads");
    fd = sys_open(fa, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd >= 0) { sys_write(fd, "x", 1); sys_close(fd); }
    utest_check(sys_fs_generation_of(a) > ga, "moved after a create in that directory");
    utest_check(sys_fs_generation_of(b) == gb, "STILL after a create in another directory");
    long long gf = sys_fs_generation_of(fa);
    fd = sys_open(fa, SYS_O_WRITE | SYS_O_APPEND);
    if (fd >= 0) { sys_write(fd, "y", 1); sys_close(fd); }
    utest_check(sys_fs_generation_of(fa) > gf, "a file's own generation moved after a write to it");
    ga = sys_fs_generation_of(a);
    sys_unlink(fa);
    utest_check(sys_fs_generation_of(a) > ga, "moved after a delete in that directory");
    sys_unlink(a);
    sys_unlink(b);

    return utest_end();
}
