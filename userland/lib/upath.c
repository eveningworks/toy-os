// See upath.h. Lifted verbatim out of userland/lib/tosh.c when
// /bin/strace became the second caller; the reasoning in the comments
// is the shell's, kept because it is what the behaviour is FOR.
#include "lib/upath.h"
#include "rt/sys.h"
#include "errno.h"

// The same order and spirit as the kernel shell's (apps/shell_path.c):
// /bin, then /usr/bin, then /tests.
static const char *const PATH_DIRS[] = { "/bin", "/usr/bin", "/tests" };
#define PATH_DIR_COUNT (int)(sizeof(PATH_DIRS) / sizeof(PATH_DIRS[0]))

static void scopy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int upath_find_program(const char *name, char *out, int cap) {
    if (!name || !out || cap <= 1) return 0;
    for (const char *p = name; *p; p++) {
        if (*p == '/') { scopy(out, name, cap); return 1; }
    }
    for (int i = 0; i < PATH_DIR_COUNT; i++) {
        int n = 0;
        for (const char *d = PATH_DIRS[i]; *d && n < cap - 2; d++) out[n++] = *d;
        out[n++] = '/';
        for (const char *c = name; *c && n < cap - 1; c++) out[n++] = *c;
        out[n] = '\0';
        // Probing by opening is the only test available: there is no
        // stat syscall yet. A directory would open too, but PATH
        // entries holding a directory named like a command is not a
        // case worth carrying code for.
        int fd = sys_open(out, 0);
        if (fd >= 0) { sys_close(fd); return 1; }
        // ENOENT is the ordinary answer -- keep looking. Anything else
        // is about this PROCESS, not about this candidate: EMFILE means
        // the next probe cannot succeed either, so continuing would
        // walk the whole PATH to arrive at a wrong conclusion.
        if (sys_errno() != ENOENT) return -1;
    }
    return 0;
}

int upath_dir_count(void) {
    return (int)(sizeof PATH_DIRS / sizeof PATH_DIRS[0]);
}

const char *upath_dir(int index) {
    if (index < 0 || index >= upath_dir_count()) return 0;
    return PATH_DIRS[index];
}
