// mv -- rename, or move between directories. One SYS_RENAME.
//
// The one refusal worth knowing about is not this program's: moving a
// DIRECTORY across parents needs five journal credits, and a v1 TFS3
// image has four slots, so that single case is refused on an old volume
// while every other move works (fs.h, and CLAUDE.md's journal-credits
// rule). It arrives here as EIO.
#include "rt/sys.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        cmd_usage("mv <source> <dest>");
        return 1;
    }
    if (sys_rename(argv[1], argv[2]) < 0) {
        cmd_fail("mv", argv[1]);
        return 1;
    }
    return 0;
}
