// fsync_test -- the fsync(2) syscall from ring 3.
//
// WHY IT IS A /tests PROGRAM AND NOT ONLY A KTEST. The KTESTs beside
// `storage.sync` exercise fs_sync_path() inside the kernel, which is
// the half that commits. This exercises the half a PROGRAM sees: an fd
// goes in, so the descriptor lookup, the kind check and the errno on a
// bad one are all on the ring-3 side of the boundary and nothing in the
// kernel can vouch for them.
//
// It deliberately does NOT try to prove durability. A crash is the only
// thing that could, and this machine has no way to stage one; what a
// program can check is that the call succeeds on a real file, refuses
// what it should, and leaves the bytes readable.
#include "rt/sys.h"
#include "lib/utest.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define PATH "/var/tmp/fsync_test.bin"  /* durability needs a real device */
#define N 8192

int main(void) {
    utest_begin("fsync_test", "fsync and fdatasync over SYS_FSYNC",
                UTEST_VERDICT_FILE);

    sys_unlink(PATH);
    int fd = sys_open(PATH, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { utest_check(0, "create the fixture"); return utest_end(); }

    static char buf[N];
    for (int i = 0; i < N; i++) buf[i] = (char)('a' + (i % 26));
    utest_check(sys_write(fd, buf, N) == N, "wrote the fixture");

    // The call under test. 0 is success; anything negative is an errno.
    utest_check(fsync(fd) == 0, "fsync on an open file returns 0");
    // fdatasync is the same syscall here -- there is no cheaper subset,
    // because what a deferred write holds back IS the inode. Checked
    // rather than assumed: it is a separate name in <unistd.h> and a
    // wrapper pointed at the wrong number would still compile.
    utest_check(fdatasync(fd) == 0, "fdatasync too");

    sys_close(fd);

    // A CLOSED fd IS A BAD ONE. This is the check that only ring 3 can
    // make -- the kernel's own tests call fs_sync_path() and never go
    // near a descriptor table.
    utest_check(fsync(fd) < 0, "fsync on a closed fd fails");
    utest_check(fsync(-1) < 0, "...and so does a negative one");

    // fd 1 is the console, not a file. fsync must refuse a kind it
    // cannot make durable rather than silently reporting success.
    utest_check(fsync(1) < 0, "fsync on a non-file fd fails");

    // AND THE BYTES SURVIVED IT. A sync that corrupted what it was
    // flushing would satisfy every check above.
    fd = sys_open(PATH, 0);
    utest_check(fd >= 0, "reopened after fsync");
    static char back[N];
    memset(back, 0, sizeof back);
    utest_check(sys_read(fd, back, N) == N, "read it all back");
    utest_check(memcmp(back, buf, N) == 0, "and every byte matches");
    sys_close(fd);

    sys_unlink(PATH);
    return utest_end();
}
