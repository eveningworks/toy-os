// truncate -- set a file's length, growing or shrinking it.
//
// Shrinking is the interesting direction and none of the interest is
// here: the backend commits the pointer change BEFORE freeing the
// blocks, so a crash mid-truncate leaks blocks (fsck reclaims) rather
// than handing a live file's blocks to the next allocation. See
// docs/decisions.md's truncation entry.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/string.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        cmd_usage("truncate <file> <size>");
        return 1;
    }
    // Parsed here rather than passed through as text: the syscall takes
    // a number, and a program that forwards a string would be asking the
    // kernel to grow a parser it has no reason to have.
    unsigned long long size = 0;
    const char *p = argv[2];
    if (!*p) { cmd_usage("truncate <file> <size>"); return 1; }
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            sys_print("truncate: size must be a decimal byte count\n");
            return 1;
        }
        size = size * 10 + (unsigned long long)(*p - '0');
    }
    if (sys_truncate(argv[1], size) < 0) {
        cmd_fail("truncate", argv[1]);
        return 1;
    }
    return 0;
}
