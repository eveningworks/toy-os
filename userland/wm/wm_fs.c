// See wm_fs.h.
#include "wm/wm_fs.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>

// Enough for any directory this WM reads: /usr/wm/desktop (nine
// entries), /usr/share/cursors (a handful of themes), and whatever the
// file picker is pointed at. A directory larger than this is truncated
// by SYS_LISTDIR itself, which is why callers treat a full result as
// "there may be more" rather than as the whole answer.
#define WM_FS_MAX_ENTRIES 64

int wm_fs_list(const char *dir, struct sys_dirent *out, int max) {
    if (!dir || !out || max <= 0) return -1;
    return sys_listdir(dir, out, max);
}

// Splits `path` into its parent directory and last component, so a path
// can be looked up in its parent's listing. Written here rather than
// reached for from kpath.h because the answer needed is a pair of
// buffers, not a normalised path.
//
// Returns 0 for a path with no last component (the root itself), which
// every caller treats as "exists, is a directory" without asking.
static int split_parent(const char *path, char *dir, unsigned dcap,
                         const char **leaf) {
    if (!path || !*path) return 0;
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') n--; // ignore a trailing slash
    int slash = -1;
    for (int i = n - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }
    if (slash < 0 || n == 0) return 0;
    *leaf = path + slash + 1;
    if (!**leaf) return 0; // the path was just "/"
    unsigned dl = (unsigned)(slash == 0 ? 1 : slash);
    if (dl + 1 > dcap) return 0;
    for (unsigned i = 0; i < dl; i++) dir[i] = path[i];
    dir[dl] = '\0';
    if (dl == 0) { dir[0] = '/'; dir[1] = '\0'; }
    return 1;
}

// The one lookup all three predicates below share: find `path`'s entry
// in its parent's listing. Returns 0 if the path is missing.
static int stat_entry(const char *path, struct sys_dirent *out) {
    char dir[64];
    const char *leaf = 0;
    if (!split_parent(path, dir, sizeof dir, &leaf)) return 0;

    // STATIC, not on the stack. A ring-3 stack is 16 KiB with ONE 4 KiB
    // guard page below it, and WM_FS_MAX_ENTRIES entries of `struct sys_dirent` is
    // 5,216 bytes -- a frame that large does not merely overflow, it
    // steps clean OVER the guard into unmapped space, which is the
    // Stack Clash shape. Found by -Wframe-larger-than the day it was
    // added to USERLAND_CFLAGS. Safe here: the WM is one event loop and
    // this does not recurse.
    static struct sys_dirent ents[WM_FS_MAX_ENTRIES];
    int n = sys_listdir(dir, ents, WM_FS_MAX_ENTRIES);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i].name, leaf) == 0) {
            if (out) *out = ents[i];
            return 1;
        }
    }
    return 0;
}

int wm_fs_exists(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return 1; // the root
    return stat_entry(path, 0);
}

int wm_fs_is_dir(const char *path) {
    if (path && path[0] == '/' && path[1] == '\0') return 1;
    struct sys_dirent e;
    if (!stat_entry(path, &e)) return 0;
    return e.is_dir != 0;
}

uint32_t wm_fs_size(const char *path) {
    struct sys_dirent e;
    if (!stat_entry(path, &e)) return 0;
    return e.is_dir ? 0 : e.size;
}

uint32_t wm_fs_read_into(const char *path, void *buf, uint32_t cap) {
    if (!path || !buf || cap == 0) return 0;

    // REFUSE an oversized file rather than truncating -- see wm_fs.h.
    // Checked before opening so the failure costs no I/O, and because a
    // partial read leaves the caller with something that parses.
    uint32_t size = wm_fs_size(path);
    if (size == 0 || size > cap) return 0;

    int fd = sys_open(path, 0); // 0 = read-only, see SYS_O_WRITE
    if (fd < 0) return 0;

    uint32_t got = 0;
    while (got < size) {
        int64_t r = sys_read(fd, (uint8_t *)buf + got, size - got);
        if (r <= 0) break; // 0 is EOF, negative is an error; both stop
        got += (uint32_t)r;
    }
    sys_close(fd);

    // A short read is a failed read here. The caller asked for a whole
    // file and every one of them parses what it gets.
    return got == size ? got : 0;
}
