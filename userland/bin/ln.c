// ln -- a second NAME for one file. No -s: this filesystem's formats
// carry link counts or they do not, and neither carries symlinks yet.
//
// The capability check is the point of the command. On a volume whose
// format has no link counts (tfs2) the kernel answers EPERM, which is a
// permanent property of the VOLUME rather than of these two paths --
// so the message says so instead of leaving someone to wonder which of
// the names was wrong.
#include "rt/sys.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        cmd_usage("ln <existing> <newname>");
        return 1;
    }
    if (sys_link(argv[1], argv[2]) < 0) {
        if (sys_errno() == EPERM)
            sys_print("ln: this filesystem's format has no hardlinks\n");
        else
            cmd_fail("ln", argv[1]);
        return 1;
    }
    return 0;
}
