// rm -- delete a file, or an EMPTY directory.
//
// Named rm rather than unlink because that is the word people type, and
// SYS_UNLINK is what serves both: fs_delete() refuses a non-empty
// directory, so there is no -r here and nothing pretends otherwise.
#include "rt/sys.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        cmd_usage("rm <path> [path...]");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        // SYS_UNLINK is one of the syscalls whose failure value is 0
        // rather than a negative errno (see abi/syscall_abi.h on why
        // flipping those is its own change), so there is no code to
        // report -- say what it means instead of inventing one.
        if (!sys_unlink(argv[i])) {
            sys_print("rm: ");
            sys_print(argv[i]);
            sys_print(": no such file, or a non-empty directory\n");
            failed = 1;
        }
    }
    return failed;
}
