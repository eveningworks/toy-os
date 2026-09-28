// kdfiled -- writes to disk the files a debugger sent with `remote put`.
//
// The kernel debugger STAGES them (kernel/debug/kdebug_files.c): while
// the machine is stopped it may only copy bytes into RAM, never touch the
// filesystem. This is the other half, a process with ordinary locking,
// woken once the machine runs again. kdfile_abi.h is the contract.
//
// Each file is written beside its target and RENAMED into place, and the
// previous version is kept as <stem>.old -- kernel.bin becomes kernel.old,
// the file grub.cfg's rescue entry boots -- ONCE PER BOOT per path, so a
// second put before a reboot cannot rotate away the kernel that is
// actually running. A read-only mount (/boot is one) is remounted
// read-write for the write and read-only again after.
#include "rt/sys.h"
#include "kdfile_abi.h"
#include "query_abi.h"
#include "mount_abi.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static char g_buf[65536];

// Paths already rotated to <stem>.old in this boot (this process lives
// for the whole boot).
#define ROTATED_MAX 8
static char g_rotated[ROTATED_MAX][KDFILE_PATH_MAX];
static int g_nrotated;

static int rotated(const char *path) {
    for (int i = 0; i < g_nrotated; i++)
        if (strcmp(g_rotated[i], path) == 0) return 1;
    return 0;
}

// The read-only mount holding `path`, if any: its point and device.
static int ro_mount_of(const char *path, char *point, char *device) {
    struct query_fsinfo fs;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; sys_query_record(QUERY_FSINFO, (unsigned)i, &fs, sizeof fs) > 0; i++) {
        if (!(fs.flags & QUERY_FS_MOUNTED)) continue;
        size_t n = strlen(fs.point);
        int under = strncmp(path, fs.point, n) == 0 && (n == 1 || path[n] == '/');
        if (!under || n < best_len) continue;
        best_len = n;
        best = (fs.flags & QUERY_FS_RDONLY) && fs.device[0];
        snprintf(point, 64, "%s", fs.point);
        snprintf(device, 64, "%s", fs.device);
    }
    return best == 1;
}

// NEVER LEAVES THE POINT UNMOUNTED: a mount that fails puts the other
// mode back. A umount that fails (busy, or already gone) is not fatal --
// the mount after it says whether the point ended up as asked.
static int remount(const char *point, const char *device, int rdonly) {
    struct mount_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.source, sizeof req.source, "%s", device);
    snprintf(req.point, sizeof req.point, "%s", point);
    if (rdonly) req.flags |= SYS_MNT_RDONLY;
    sys_umount(point);
    if (sys_mount(&req) == 0) return 0;
    req.flags ^= SYS_MNT_RDONLY;
    sys_mount(&req);
    return -1;
}

// "<dir>/<stem>.old" for "<dir>/<stem>.<ext>" (or "<dir>/<name>.old").
static void backup_name(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    char *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash + 1)) *dot = 0;
    size_t n = strlen(out);
    snprintf(out + n, cap - n, ".old");
}

static int write_one(int handle, const struct kdfile_info *info) {
    char tmp[KDFILE_PATH_MAX + 8], old[KDFILE_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.kdnew", info->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    for (uint64_t off = 0; off < info->size;) {
        struct kdfile_read r = { handle, sizeof g_buf, off, (uint64_t)(uintptr_t)g_buf };
        int64_t n = sys_kdfile(KDFILE_READ, (uint64_t)(uintptr_t)&r, 0);
        if (n <= 0 || write(fd, g_buf, (size_t)n) != n) {
            int e = n < 0 ? -errno : -EIO;
            close(fd);
            unlink(tmp);
            return e;
        }
        off += (uint64_t)n;
    }
    fsync(fd);
    close(fd);
    // rename() never replaces, so the target is moved or removed first.
    int first = !rotated(info->path);
    backup_name(info->path, old, sizeof old);
    if (first) {
        unlink(old);
        rename(info->path, old);   // absent the first time: fine
    } else {
        unlink(info->path);        // the .old already holds the booted one
    }
    if (rename(tmp, info->path) < 0) {
        int e = -errno;
        if (first) rename(old, info->path);   // put the running one back
        unlink(tmp);
        return e;
    }
    if (first && g_nrotated < ROTATED_MAX)
        snprintf(g_rotated[g_nrotated++], KDFILE_PATH_MAX, "%s", info->path);
    return 0;
}

int main(void) {
    for (;;) {
        struct kdfile_info info;
        // The wrapper returns -1 and sets errno, never -ENODEV itself --
        // a test on -ENODEV spun here on every boot without kdebug=.
        int64_t h = sys_kdfile(KDFILE_TAKE, (uint64_t)(uintptr_t)&info, 0);
        if (h < 0 && (errno == ENODEV || errno == ENOSYS))
            return 0;                 // no debugger armed: nothing to serve
        if (h < 0) return 1;          // anything else: the service restarts it
        if (h == 0) continue;         // woken: ask again

        char point[64], device[64];
        int ro = ro_mount_of(info.path, point, device);
        int rc = ro && remount(point, device, 0) < 0 ? -EACCES : 0;   // no EROFS here
        if (!rc) rc = write_one((int)h, &info);
        if (ro) remount(point, device, 1);
        sys_kdfile(KDFILE_DONE, (uint64_t)h, (uint64_t)(int64_t)rc);
    }
}
