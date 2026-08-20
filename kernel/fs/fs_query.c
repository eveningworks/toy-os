// The mounted filesystem, as a queryable FACT.
//
// Lives in kernel/fs/ because that is the subsystem that owns the
// answers -- the same rule that puts the memory provider in
// kernel/mm/mem_query.c and a display_driver in its card's file. There
// is no central table to edit.
//
// WHY THIS EXISTS AT ALL: `df` was a kernel builtin long after
// /bin/df was written, for one reason -- fs_backend_name() and
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
#include "query.h"
#include "fs.h"
#include "string.h"
#include <stddef.h>

static int fsinfo_count(void) { return 1; } // scalar

static int fsinfo_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_fsinfo *f = out;
    k_memset(f, 0, sizeof *f);

    const char *name = fs_backend_name();
    if (name && name[0]) {
        k_strlcpy(f->name, name, sizeof f->name);
        f->flags |= QUERY_FS_MOUNTED;
        if (fs_is_persistent()) f->flags |= QUERY_FS_PERSISTENT;
    }

    // Asked for unconditionally but only MEANINGFUL under
    // QUERY_FS_MOUNTED -- with nothing mounted the two counts are not
    // small, they are meaningless, and "0B used of 0B" reads as an
    // empty disk rather than as no disk. The flag is what lets a
    // formatter tell those apart; see /bin/df.
    uint64_t used = 0, total = 0;
    if (fs_disk_usage(&used, &total)) {
        f->used_bytes = used;
        f->total_bytes = total;
    }
    return 1;
}

// The NAME is deliberately absent from this list: every named field is
// 64 bits (struct query_field), so a string cannot be one. `config get
// fs.used_bytes` works; the name is read as part of the whole record.
// See the ABI comment on struct query_fsinfo.
static const struct query_field fsinfo_fields[] = {
    QUERY_FIELD(struct query_fsinfo, used_bytes,  QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_fsinfo, total_bytes, QUERY_TYPE_BYTES),
    QUERY_FIELD(struct query_fsinfo, flags,       QUERY_TYPE_U64),
};

static const struct query_provider fsinfo_provider = {
    .cls = QUERY_FSINFO,
    .name = "fs",
    .record_size = sizeof(struct query_fsinfo),
    .flags = 0, // scalar
    .count = fsinfo_count,
    .fill = fsinfo_fill,
    .fields = fsinfo_fields,
    .field_count = sizeof fsinfo_fields / sizeof fsinfo_fields[0],
};

void fs_query_init(void) {
    query_register(&fsinfo_provider);
}
