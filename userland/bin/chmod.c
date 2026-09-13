// chmod -- change a file's permission bits.
//
// **OCTAL ONLY, no `u+x` symbolic form.** The symbolic syntax is a small
// parser plus a umask to resolve `+w` against, and this system has no
// umask that anything consults (<sys/stat.h>). A half-implemented `+x`
// that ignored the mask would be the quietly-wrong-under-a-familiar-name
// failure `touch` avoids by not claiming to update timestamps.
//
// It exists because the mode had no way to change: a file was created
// with the default and stayed there forever, which made the field a
// seeded constant rather than a fact about the file.
#include "lib/cmd.h"
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

// Returns the mode, or -1. Rejects anything that is not three or four
// octal digits -- a REFUSAL rather than a guess, because `chmod 8 f`
// and `chmod rwx f` both mean the caller expected something this does
// not do, and silently applying a misread number is the worst outcome.
static int parse_mode(const char *s) {
    if (!s || !*s) return -1;
    int v = 0, n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '7') return -1;
        v = v * 8 + (*s - '0');
    }
    if (n < 1 || n > 4) return -1;
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        cmd_usage("chmod <octal-mode> <file> [file...]");
        return 1;
    }
    int mode = parse_mode(argv[1]);
    if (mode < 0) {
        fprintf(stderr, "chmod: %s: not an octal mode (try 755 or 0644)\n",
                argv[1]);
        return 1;
    }
    int failed = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], (mode_t)mode) != 0) {
            // ENOTSUP is worth naming rather than folding into a generic
            // failure: it means the FILESYSTEM cannot store a mode, not
            // that this file is special, and the caller's next move is
            // different (there is nothing to retry).
            if (errno == ENOTSUP)
                fprintf(stderr, "chmod: %s: this filesystem stores no "
                                "permission bits\n", argv[i]);
            else
                cmd_fail("chmod", argv[i]);
            failed = 1;
        }
    }
    return failed;
}
