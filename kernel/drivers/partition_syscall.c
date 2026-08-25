// SYS_MKPART -- the ring-3 half of writing a partition table.
//
// Beside partition.c the way partition_query.c is, and for the same
// reason: this is the partition subsystem's own userland surface, not
// the filesystem's. It sits ABOVE partition.c (it converts and checks;
// partition.c encodes) and it is the only file here that touches user
// memory.
//
// The ABI's reasoning -- why this takes a table description rather than
// exposing a raw sector write -- is in abi/partition_abi.h.
#include "partition.h"
#include "partition_abi.h"
#include "syscall_table.h"
#include "syscalls.h"
#include "errno.h"
#include "block.h"
#include "fs.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "vmm.h"
#include "scheduler.h" // the preemption guard around the write

int sys_mkpart(struct syscall_ctx *c) {
    struct mkpart_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    if (!blk_present() || blk_disk_sector_count() == 0) {
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return 0;
    }

    // THE SPEED BUMP, AND WHAT IT IS NOT. There is no privilege model
    // in this kernel, so this cannot be a permission check -- any
    // process can set the flag. What it stops is the accident: a
    // program that meant to READ the table and passed a zeroed request
    // does not silently repartition the disk it is running from.
    // abi/partition_abi.h says the same thing out loud so that a
    // future session adding uids knows this is the place to put a real
    // check.
    if (fs_is_persistent() && !(req.flags & MKPART_CONFIRM)) {
        klog_write("mkpart: refused -- a persistent filesystem is mounted and MKPART_CONFIRM was not set\n");
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }

    if (req.count < 1 || req.count > MKPART_MAX_ENTRIES ||
        (req.kind != MKPART_KIND_MBR && req.kind != MKPART_KIND_GPT)) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    // STATIC, not a stack local: struct partition_table is ~1.5 KB
    // against a 1 KB kernel frame budget. Safe because vfs.c's FS_OP()
    // preemption guard does not cover this path, so the guard is taken
    // explicitly below -- and because writing a partition table is a
    // once-in-a-boot operation nobody races.
    static struct partition_table tbl;
    k_memset(&tbl, 0, sizeof(tbl));
    tbl.kind = (req.kind == MKPART_KIND_GPT) ? PART_TABLE_GPT : PART_TABLE_MBR;
    tbl.entry_count = (int)req.count;

    for (uint32_t i = 0; i < req.count; i++) {
        const struct mkpart_entry *src = &req.entries[i];
        struct partition_entry *dst = &tbl.entries[i];

        // A count of zero would underflow the inclusive end below, so
        // it is refused HERE rather than being allowed to become an
        // end LBA of start-1 that partition_validate() would then read
        // as "ends before it starts" -- a real refusal, but naming the
        // wrong cause.
        if (src->sectors == 0) {
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }

        dst->gpt_lba_start = src->start_lba;
        dst->gpt_lba_end = src->start_lba + src->sectors - 1; // GPT's end is INCLUSIVE
        dst->mbr_lba_start = (uint32_t)src->start_lba;
        dst->mbr_num_sectors = (uint32_t)src->sectors;
        dst->mbr_type = src->mbr_type;

        // An MBR field is 32 bits. Refuse rather than truncate -- a
        // truncated start LBA is a partition somewhere else entirely.
        if (tbl.kind == PART_TABLE_MBR &&
            (src->start_lba + src->sectors) > 0xFFFFFFFFull) {
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }

        // k_strlcpy, not k_strncpy: the name comes from ring 3 and may
        // arrive without a NUL. Truncation here is the right call
        // rather than a refusal -- a too-long label is cosmetic, and
        // GPT's own 36-code-unit limit means every tool truncates it.
        k_strlcpy(dst->gpt_name, src->name, sizeof(dst->gpt_name));
    }

    const char *why = "";
    if (!partition_validate(&tbl, &why)) {
        klog_printf("mkpart: refused -- %s\n", why);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    partition_fill_defaults(&tbl);

    // The same preemption guard vfs.c's FS_OP() takes, for the same
    // reason: partition.c reuses module-level scratch through
    // blk_disk_*, and a ring-3 process is preemptible inside a syscall,
    // so an interleaved filesystem read would fight this for the disk
    // driver's buffers mid-table-write. Cheaper to hold it than to
    // reason about which half of a GPT write is safe to be preempted in.
    scheduler_preempt_disable();
    int ok = partition_write_table(&tbl);
    scheduler_preempt_enable();

    c->regs[14] = ok ? 0 : (uint64_t)(int64_t)-EIO;
    return 0;
}
