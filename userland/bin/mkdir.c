// mkdir -- create a directory. The kernel shell has had this as a
// builtin since before there were processes; this is the ring-3 half,
// over SYS_MKDIR (abi/syscall_abi.h).
//
// Relative paths work because the CWD IS THE KERNEL'S: `mkdir docs` run
// from /tmp creates /tmp/docs because the kernel joined it, not because
// a shell rewrote the argument on the way past. See sys.h's chdir().
#include "lib/cmd.h"
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("mkdir <dir> [dir...]");
        return 1;
    }
    int failed = 0;
    // Every argument is attempted even after one fails -- the coreutils
    // behaviour, and the one that makes `mkdir a b c` useful when `b`
    // already exists.
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            cmd_fail("mkdir", argv[i]);
            failed = 1;
        }
    }
    return failed;
}
