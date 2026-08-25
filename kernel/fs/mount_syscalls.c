// SYS_MOUNT and SYS_UMOUNT -- the ring-3 half of the mount table.
//
// Beside mount.c the way partition_syscall.c sits beside partition.c,
// and for the same reason: this is the mount table's userland surface,
// and it is the only file here that touches user memory. It converts
// and validates; mount.c decides.
//
// The ABI's reasoning -- why the source is a partition NUMBER rather
// than a device path, and why there is no options string -- is in
// abi/mount_abi.h.
#include "mount.h"
#include "mount_abi.h"
#include "syscall_table.h"
#include "syscalls.h"
#include "errno.h"
#include "block.h"
#include "fs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "vmm.h"

// Turns a refusal reason into an errno. mount_add() answers in ENGLISH
// because the string is what a person needs; this is the same fact for
// a program, and the mapping lives in one place rather than being
// re-derived at each call.
//
// The reasons are matched by their leading words rather than by an enum
// because the string is the primary artefact -- an enum would be a
// second thing to keep in step, and a mismatch would silently degrade
// to a generic error rather than failing loudly.
static int why_to_errno(const char *why) {
    if (!why || !why[0]) return -EINVAL;
    if (k_strstr(why, "already mounted")) return -EBUSY;
    if (k_strstr(why, "in use") || k_strstr(why, "still open")) return -EBUSY;
    if (k_strstr(why, "underneath")) return -EBUSY;
    if (k_strstr(why, "not a directory")) return -ENOTDIR;
    if (k_strstr(why, "table is full")) return -ENOSPC;
    if (k_strstr(why, "nothing recognises")) return -ENODEV;
    if (k_strstr(why, "no such filesystem on")) return -ENODEV;
    return -EINVAL;
}

// A source names a PARTITION NUMBER on the boot disk, or nothing at all
// when the filesystem type needs no volume. Anything else is refused
// rather than guessed at -- this OS has no /dev, and accepting a
// device-looking path that resolved to nothing would be a mount that
// silently attached the wrong thing.
static int source_device(const char *src, const struct block_device **out) {
    *out = NULL;
    if (!src || !src[0]) return 1;   // no volume; the fstype must supply one
    int n = 0;
    for (const char *p = src; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (*p - '0');
        if (n > 128) return 0;
    }
    if (n < 1) return 0;
    *out = mount_partition_device(n);
    return *out != NULL;
}

int sys_mount(struct syscall_ctx *c) {
    struct mount_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // A struct arriving from ring 3 is UNTRUSTED DATA: a caller that
    // filled a field without a NUL would otherwise have every k_str*
    // below run off the end of the copy.
    req.source[MOUNT_SOURCE_MAX - 1] = '\0';
    req.fstype[MOUNT_FSTYPE_MAX - 1] = '\0';
    req.point[MOUNT_POINT_MAX - 1] = '\0';

    const struct block_device *dev = NULL;
    // A named type that needs no volume (ramfs) legitimately has an
    // empty source; a numeric source that names no partition is an
    // error whatever the type.
    if (!source_device(req.source, &dev)) {
        klog_printf("mount: no partition named \"%s\"\n", req.source);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    unsigned flags = 0;
    if (req.flags & SYS_MNT_RDONLY) flags |= MNT_RDONLY;

    const char *why = "";
    if (!mount_add(dev, req.fstype[0] ? req.fstype : NULL, req.point, flags, &why)) {
        klog_printf("mount: %s -- %s\n", req.point, why);
        c->regs[14] = (uint64_t)(int64_t)why_to_errno(why);
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}

int sys_umount(struct syscall_ctx *c) {
    char point[MOUNT_POINT_MAX];
    if (!vmm_copy_string_from_user(c->pml4, point, c->a0, sizeof(point))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    const char *why = "";
    if (!mount_remove(point, &why)) {
        klog_printf("umount: %s -- %s\n", point, why);
        c->regs[14] = (uint64_t)(int64_t)why_to_errno(why);
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}
