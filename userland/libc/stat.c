// <sys/stat.h>: mkdir(), and stat()/lstat()/fstat() over SYS_STAT
// and SYS_FSTAT.
#include <sys/stat.h>
#include "rt/sys.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include "caltime.h"   // cal_rtc_to_epoch -- shared with the kernel

int mkdir(const char *path, mode_t mode) {
    // Read and discarded rather than left unnamed: this filesystem has
    // no permission bits, so there is nothing for the mode to mean. See
    // the header for why that is an ignore and not a dishonour.
    (void)mode;
    // sys_mkdir() already returns 0 or -1 with sys_errno() set, which is
    // exactly POSIX's contract -- there is nothing to translate.
    return sys_mkdir(path);
}


// The block size reported to a caller sizing its I/O. Not asked of the
// filesystem: TFS3's block is 4096 and every other path here is a
// memory copy, so a constant is the truthful answer rather than a
// lookup that would always return the same number.
#define STAT_BLKSIZE 4096

static void fill(struct stat *st, const struct sys_stat *s) {
    memset(st, 0, sizeof *st);
    st->st_ino  = s->ino;
    st->st_size = (off_t)s->size;
    // **THE TYPE COMES FROM is_dir, NOT FROM THE MODE.** The kernel
    // reports permission bits only; the type is a separate fact, and
    // folding them together is this boundary's job rather than the
    // filesystem's. A symlink cannot be distinguished here because the
    // path was already resolved through it (see lstat).
    st->st_mode = (mode_t)(s->mode & 07777) | (s->is_dir ? S_IFDIR : S_IFREG);
    st->st_nlink = s->nlink ? s->nlink : 1;
    st->st_uid = st->st_gid = 0;      // single-user, not a placeholder
    st->st_mtime = (time_t)cal_rtc_to_epoch(&s->modified);
    st->st_ctime = (time_t)cal_rtc_to_epoch(&s->created);
    st->st_atime = st->st_mtime;      // no access time is recorded
    st->st_blksize = STAT_BLKSIZE;
    // 512-byte units, as every Unix reports regardless of the real
    // block size. Rounded UP: a one-byte file occupies a block.
    st->st_blocks = (blkcnt_t)((s->size + 511) / 512);
}

int stat(const char *path, struct stat *st) {
    if (!path || !st) { errno = EINVAL; return -1; }
    struct sys_stat s;
    int r = sys_stat(path, &s);
    if (r < 0) return -1;             // sys_stat set errno
    fill(st, &s);
    return 0;
}

// IDENTICAL TO stat(), deliberately -- see <sys/stat.h>. Path
// resolution here has no "do not follow the last link" mode, so a
// separate implementation would be the same code with a different name
// and a false promise.
int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st) {
    if (fd < 0 || !st) { errno = EINVAL; return -1; }
    struct sys_stat s;
    if (sys_fstat(fd, &s) < 0) return -1;
    fill(st, &s);
    // A pipe, a socket and a terminal are not regular files, and saying
    // so is what stops a caller seeking on one because S_ISREG was true.
    if (!(s.flags & SYS_STAT_SEEKABLE) && !s.is_dir)
        st->st_mode = (mode_t)((st->st_mode & 07777) |
                               ((s.flags & SYS_STAT_TTY) ? S_IFCHR : S_IFIFO));
    return 0;
}
