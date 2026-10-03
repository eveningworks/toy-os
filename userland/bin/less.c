// less -- a pager. Shows one screenful at a time and lets you move
// through it.
//
// THE PAGING IS NOT HERE. It moved to userland/lib/upager.c when
// /bin/doc wanted the same thing, and that file carries the design --
// where the keys come from, why width is measured in display columns,
// the alternate screen. What is left here is the part that is `less`
// rather than "a pager": deciding whether the text is a file or stdin,
// and holding it.
//
// THE BUFFER STAYS ON THIS SIDE OF THE SEAM. The whole input is held in
// memory so it can be scrolled BACKWARD -- a pipe cannot be rewound, so
// a pager that supports going back has no choice but to keep what it
// has seen. (That is the difference between `less` and `more`: more is
// forward-only and needs no buffer at all.) Static rather than malloc'd
// because it is a fixed ceiling either way and a ring-3 stack has a
// 2 KiB frame budget; bigger than this is truncated WITH A MESSAGE
// rather than silently cut, since a pager that quietly drops the end of
// a file is worse than one that says it did.
#include "rt/sys.h"
#include "lib/upager.h"
#include "lib/uargs.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#define LESS_MAX_BYTES (256 * 1024)

static char g_buf[LESS_MAX_BYTES];
static int  g_len = 0;
static int  g_truncated = 0;

static void read_all(int fd) {
    while (g_len < LESS_MAX_BYTES) {
        int64_t n = read(fd, g_buf + g_len, (size_t)(LESS_MAX_BYTES - g_len));
        if (n == SYS_RETRY) continue;   // pipe not ready; ask again
        if (n <= 0) break;              // 0 is EOF on a file or a pipe
        g_len += (int)n;
    }
    if (g_len >= LESS_MAX_BYTES) g_truncated = 1;
}

static const struct uargs_prog PROG = {
    .name = "less",
    .usage = "[FILE]",
    .summary = "Page through FILE, or through standard input -- `dmesg | less`.",
    .more = upager_keys,   // the pager owns its keymap; one list, not two
};

int main(int argc, char **argv) {
    int fd = 0;   // stdin by default, so `cmd | less` works
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc > 1) return uargs_error(&PROG, "one file at a time");
    if (a.argc == 1) {
        argv[1] = a.argv[0];
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            char msg[160];
            snprintf(msg, sizeof msg, "less: cannot open %s: %s\n",
                     argv[1], sys_strerror(sys_errno()));
            sys_eprint(msg);
            return 1;
        }
    }

    read_all(fd);
    if (fd > 0) close(fd);

    return upager_run(g_buf, g_len, "less", g_truncated) < 0 ? 1 : 0;
}
