// truncate -- set a file's length, growing or shrinking it.
//
// Shrinking is the interesting direction and none of the interest is
// here: the backend commits the pointer change BEFORE freeing the
// blocks, so a crash mid-truncate leaks blocks (fsck reclaims) rather
// than handing a live file's blocks to the next allocation. See
// docs/decisions.md's truncation entry.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <string.h>

#define USAGE "truncate <file> <size>  |  truncate -s <size> <file>"

// Parsed here rather than passed through as text: the syscall takes a
// number, and a program that forwards a string would be asking the
// kernel to grow a parser it has no reason to have. Returns 0 on
// anything that is not a plain decimal byte count -- no suffixes, so
// `-s 4K` is REFUSED rather than read as 4.
static int parse_size(const char *p, unsigned long long *out) {
    if (!*p) return 0;
    unsigned long long v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (unsigned long long)(*p - '0');
    }
    *out = v;
    return 1;
}

int main(int argc, char **argv) {
    // Two spellings: this system's <file> <size>, and the -s <size>
    // <file> every other truncate takes. Accepting both because the
    // muscle memory is universal and the positional form is what every
    // existing caller here types.
    const char *path = 0, *size_arg = 0;
    if (argc == 3 && strcmp(argv[1], "-s") != 0) {
        path = argv[1];
        size_arg = argv[2];
    } else if (argc == 4 && strcmp(argv[1], "-s") == 0) {
        size_arg = argv[2];
        path = argv[3];
    } else {
        cmd_usage(USAGE);
        return 1;
    }

    unsigned long long size = 0;
    if (!parse_size(size_arg, &size)) {
        sys_print("truncate: size must be a decimal byte count\n");
        return 1;
    }
    if (sys_truncate(path, size) < 0) {
        cmd_fail("truncate", path);
        return 1;
    }
    return 0;
}
