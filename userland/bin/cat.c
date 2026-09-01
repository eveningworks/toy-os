// cat -- copy files (or stdin) to stdout.
//
// IT STREAMS IN CHUNKS RATHER THAN READING THE WHOLE FILE. The kernel
// shell's builtin read the file entire and printed it, which is fine
// for an /etc file and wrong for anything large -- and the ring-3 stack
// has a 2 KiB frame budget, so a whole-file buffer could not live on it
// anyway (CLAUDE.md's ring-3 frame budget). A fixed chunk in .bss makes
// the file size irrelevant.
//
// WITH NO ARGUMENTS IT READS fd 0, which is what makes `cmd | cat` and
// a plain `cat` fed from a pipe work. fd 0 BLOCKS (api/pipe.h), so an
// argument-less cat at the console waits for input, exactly as it does
// on a real system -- that is the behaviour, not a hang.
//
// A missing file does NOT stop the remaining ones: the failure is
// reported, the exit code goes non-zero, and the rest still print.
// coreutils does the same, and it is the difference between `cat a b c`
// telling you which one is missing and telling you only the first.
#include "lib/cmd.h"
#include <fcntl.h>
#include <unistd.h>

// One block. Bigger buys little -- the console sink is the slow part --
// and this is .bss either way.
#define CHUNK 1024

static char g_buf[CHUNK];

// Copies everything on `fd` to stdout. Returns 0 on success, 1 if a
// read failed partway (a short read is not a failure -- it is how a
// pipe delivers whatever is ready).
static int copy_out(int fd, const char *name) {
    for (;;) {
        int64_t n = read(fd, g_buf, sizeof g_buf);
        if (n == 0) return 0;   // EOF
        if (n < 0) {
            cmd_fail("cat", name);
            return 1;
        }
        write(1, g_buf, (size_t)n);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return copy_out(0, 0);

    int failed = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            cmd_fail("cat", argv[i]);
            failed = 1;
            continue;
        }
        if (copy_out(fd, argv[i])) failed = 1;
        close(fd);
    }
    return failed;
}
