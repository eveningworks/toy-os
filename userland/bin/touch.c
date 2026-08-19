// touch -- create an empty file if it does not exist.
//
// No syscall of its own: SYS_OPEN with O_WRITE|O_CREAT is exactly this,
// and adding a second way to create a file would be a second place for
// the creation rules to live. It does NOT update an existing file's
// timestamp -- coreutils' touch does, this filesystem has no call for
// it, and quietly doing nothing under a familiar name is worse than not
// claiming the behaviour.
#include "rt/sys.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("touch <file> [file...]");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], SYS_O_WRITE | SYS_O_CREAT);
        if (fd < 0) {
            cmd_fail("touch", argv[i]);
            failed = 1;
        } else {
            sys_close(fd);
        }
    }
    return failed;
}
