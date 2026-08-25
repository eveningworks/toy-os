// The mounted filesystems, as a queryable FACT.
//
// Lives in kernel/fs/ because that is the subsystem that owns the
// answers -- the same rule that puts the memory provider in
// kernel/mm/mem_query.c and a display_driver in its card's file. There
// is no central table to edit.
//
// WHY THIS EXISTS AT ALL: `df` was a kernel builtin long after /bin/df
// was written, for one reason -- fs_backend_name() and
// fs_is_persistent() are kernel calls with no syscall behind them, so
// ring 3 could report the NUMBERS and not which filesystem they were
// about. userland/gui/system/about.c omitted a line for the same gap,
// with a comment saying so. One provider closes both.
//
// The usage numbers are here too, even though SYS_SYSINFO already
// carries them, so a caller reads ONE record. Two reads of a live
// filesystem can disagree -- a `used` from one moment beside a `total`
// from another is a number nobody can trust -- and /bin/df was doing
// exactly that as soon as it needed the name from somewhere else.
//
// IT IS A VECTOR NOW, one record per mount. That is what makes `df`
// and `/bin/mount` two formatters over one fact rather than two
// syscalls, and it is why neither needed a syscall of its own for
// listing. Root first, so a reader that asks only for record 0 gets
// exactly what the scalar version gave it.
#include "query.h"
#include "fs.h"
#include "mount.h"
#include "block.h"
#include "string.h"
#include <stddef.h>

static int fsinfo_count(void) {
    int n = mount_count();
    return n ? n : 1; // one empty record, so `df` can say "nothing mounted"
}

static int fsinfo_fill(int index, void *out) {
    struct query_fsinfo *f = out;
    k_memset(f, 0, sizeof *f);

    const struct mount *m = mount_at(index);
    if (!m) return index == 0 ? 1 : 0; // the not-mounted case: flags stay 0

    k_strlcpy(f->name, m->fs->name, sizeof f->name);
    k_strlcpy(f->point, m->point, sizeof f->point);
    if (m->dev) k_strlcpy(f->device, m->dev->name, sizeof f->device);
    f->flags |= QUERY_FS_MOUNTED;
    if (m->persistent) f->flags |= QUERY_FS_PERSISTENT;
    if (m->flags & MNT_RDONLY) f->flags |= QUERY_FS_RDONLY;
    if (m->point_len == 1) f->flags |= QUERY_FS_ROOT;

    // PER-MOUNT USAGE, which is the whole reason this became a vector:
    // fs_disk_usage() answers for the root, so asking it for /boot would
    // report the root's numbers under the ESP's name.
    //
    // THROUGH fs_mount_usage(), NOT `m->fs->disk_usage()` DIRECTLY. The
    // direct call is one line shorter and skips vfs.c's preemption
    // guard, and the backends are not re-entrant -- a provider read
    // preempted inside a FAT chain walk is exactly the corruption that
    // guard exists to stop. Correct as to WHICH volume because a
    // backend is mounted exactly once (fs_ops.max_mounts).
    uint64_t used = 0, total = 0;
    if (fs_mount_usage(m, &used, &total)) {
        f->used_bytes = used;
        f->total_bytes = total;
    }
    return 1;
}

// The NAME is deliberately absent from this list: every named field is
// 64 bits (struct query_field), so a string cannot be one. `config get
// fs.used_bytes` works; the name is read as part of the whole record.
static const struct query_field fsinfo_fields[] = {
    QUERY_FIELD(struct query_fsinfo, used_bytes,  QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_fsinfo, total_bytes, QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_fsinfo, flags,       QUERY_TYPE_U64),
};

static const struct query_provider fsinfo_provider = {
    .cls = QUERY_FSINFO,
    .name = "fs",
    .record_size = sizeof(struct query_fsinfo),
    .flags = QUERY_F_LIST,
    .count = fsinfo_count,
    .fill = fsinfo_fill,
    .fields = fsinfo_fields,
    .field_count = sizeof fsinfo_fields / sizeof fsinfo_fields[0],
};

void fs_query_init(void) {
    query_register(&fsinfo_provider);
}
