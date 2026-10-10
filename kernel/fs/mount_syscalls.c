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
#include "kpath_buf.h" // kpath_get(): a path may not be a kernel local

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
    if (k_strstr(why, "cannot be unmounted")) return -EBUSY;   // the root, as Linux answers
    if (k_strstr(why, "not a directory")) return -ENOTDIR;
    if (k_strstr(why, "table is full")) return -ENOSPC;
    if (k_strstr(why, "nothing recognises")) return -ENODEV;
    if (k_strstr(why, "no such filesystem on")) return -ENODEV;
    return -EINVAL;
}

// A source names a DEVICE (`ahci0p1`, block.h's table), or a partition
// NUMBER on the boot disk, or nothing at all when the filesystem type
// needs no volume. Anything else is refused rather than guessed at --
// this OS has no /dev, and accepting a device-looking path that resolved
// to nothing would be a mount that silently attached the wrong thing.
//
// THE NUMBER IS THE OLDER FORM AND IS KEPT, but it can only ever mean a
// partition of the ROOT's disk, which stopped being the only disk when
// every driver started enumerating. A number is what `parttable` prints
// and what every existing script passes; a name is the only way to reach
// the second disk at all.
static int source_device(const char *src, const struct block_device **out) {
    *out = NULL;
    if (!src || !src[0]) return 1;   // no volume; the fstype must supply one

    const struct blk_entry *e = blk_device_by_name(src);
    if (e) { *out = e->dev; return 1; }

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

// SYS_MKFS -- put an empty filesystem on ONE partition.
//
// The installer's operation, and the reason it is not `fsformat` with an
// argument: fsformat reformats the volume this machine is RUNNING FROM,
// unmounting everything and re-probing after. Writing a filesystem onto
// some OTHER partition must disturb nothing at all, so a target that is
// mounted is REFUSED rather than unmounted for the caller.
int sys_mkfs(struct syscall_ctx *c) {
    struct mkfs_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // Untrusted, exactly as sys_mount() says below: a field without a
    // NUL would run every k_str* off the end of the copy.
    req.source[MOUNT_SOURCE_MAX - 1] = '\0';
    req.fstype[MOUNT_FSTYPE_MAX - 1] = '\0';

    if (!(req.flags & MKFS_CONFIRM)) {
        klog_write(KLOG_ERR "mkfs: refused without MKFS_CONFIRM\n");
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }

    // A VOLUME, NEVER "whatever is active". An empty source means
    // nothing here -- there is no filesystem to format without one --
    // so it is an error rather than a fallback to the boot disk.
    const struct block_device *dev = NULL;
    if (!req.source[0] || !source_device(req.source, &dev) || !dev) {
        klog_printf("mkfs: no partition named \"%s\"\n", req.source);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    const struct fs_ops *target = mount_backend_named(req.fstype);

    // BUSY COVERS TWO CASES, and the second is the surprising one: a
    // backend keeps its volume in module-level state (see
    // fs_format_device), so formatting a second TFS3 partition while a
    // TFS3 root is mounted repoints the backend under the running
    // system. Both are -EBUSY rather than one being -EIO, because the
    // caller's move is the same for both -- do it from a live boot.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->used) continue;
        if (m->dev == dev) {   // the target itself -- see fs_format_device
            c->regs[14] = (uint64_t)(int64_t)-EBUSY;
            return 0;
        }
    }

    if (!target) {
        klog_printf("mkfs: no filesystem type \"%s\"\n", req.fstype);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    if (!fs_format_device(dev, req.fstype)) {
        c->regs[14] = (uint64_t)(int64_t)-EIO;
        return 0;
    }
    klog_printf("mkfs: %s formatted as %s\n", req.source, req.fstype);
    c->regs[14] = 0;
    return 0;
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
    // MiB in the ABI, bytes below: the unit hop happens once, here,
    // rather than at each of mount_add()'s callers.
    uint64_t size_bytes = (uint64_t)req.size_mib * 1024 * 1024;
    if (!mount_add(dev, req.fstype[0] ? req.fstype : NULL, req.point, flags,
                   size_bytes, &why)) {
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

// SYS_FS_CHECK -- the consistency check of the volume holding a path,
// run here against the mounted backend (abi/mount_abi.h says why it is
// not a ring-3 checker). Holds that mount's lock for the whole pass, so
// every other call on the volume waits it out, as it did when `fsck`
// was a kernel-shell builtin -- all but FSCK_PROGRESS and FSCK_STOP,
// which ask about that pass and take no lock (fs_ops.h).
int sys_fs_check(struct syscall_ctx *c) {
    uint64_t f = c->a1;
    if ((f & ~(uint64_t)(FSCK_REPAIR | FSCK_PROGRESS | FSCK_STOP)) ||
        ((f & (FSCK_PROGRESS | FSCK_STOP)) && f != FSCK_PROGRESS && f != FSCK_STOP)) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int rc = resolve_user_path(c->pml4, c->a0, path);
    if (!rc && f == FSCK_PROGRESS) {
        struct fs_check_progress p;
        rc = fs_check_progress_at(path, &p);
        if (!rc && !vmm_copy_to_user(c->pml4, c->a2, &p, sizeof p)) rc = -EFAULT;
    } else if (!rc && f == FSCK_STOP) {
        rc = fs_check_stop_at(path);
    } else if (!rc) {
        struct fs_check_result r;
        k_memset(&r, 0, sizeof r);
        rc = fs_check_at(path, (int)(f & FSCK_REPAIR), &r);
        if (!rc && !vmm_copy_to_user(c->pml4, c->a2, &r, sizeof r)) rc = -EFAULT;
    }
    kpath_put(path);
    c->regs[14] = (uint64_t)(int64_t)rc;
    return 0;
}
