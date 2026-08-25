// umount -- detach a filesystem.
//
// REFUSES rather than forcing, and the three refusals are the whole
// design: the root cannot be unmounted (there would be nothing left to
// resolve a path against), a filesystem with an open file on it is
// EBUSY, and one with another filesystem mounted underneath it is EBUSY
// too. Linux has `-l` (lazy) and `-f` (force) for the first two; both
// exist because a network filesystem can hang, which nothing here can,
// and both leave a window where a process holds a file on a filesystem
// that is gone. Not implemented, and that is a decision rather than an
// omission.
//
// The volume is FLUSHED before the mount is forgotten -- the kernel
// does that (SYS_UMOUNT), because after the slot is cleared there is no
// owner left to report a failed flush to.
#include "rt/sys.h"
#include "lib/cmd.h"

static const char *USAGE = "umount <mountpoint>";

int main(int argc, char **argv) {
    if (argc != 2) { cmd_usage(USAGE); return 1; }
    if (sys_umount(argv[1]) < 0) {
        cmd_fail("umount", argv[1]);
        return 1;
    }
    return 0;
}
