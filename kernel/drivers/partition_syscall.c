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
#include "mount.h"     // disk_is_in_use() asks the mount table
#include "scheduler.h" // the preemption guard around the write

// Is this disk one the machine is RUNNING FROM? True when it is the
// boot disk, or when any mount sits on it or on a partition of it.
// mount->dev is a partition device, so the parent is what to compare.
static int disk_is_in_use(const struct block_device *disk) {
    if (disk == blk_whole_disk() && fs_is_persistent()) return 1;
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->used || !m->dev) continue;
        const struct blk_entry *e = blk_device_by_name(blk_device_name(m->dev));
        if (e && (e->parent == disk || e->dev == disk)) return 1;
    }
    return 0;
}

int sys_mkpart(struct syscall_ctx *c) {
    struct mkpart_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    // WHICH DISK. Empty names the boot disk, which is all this syscall
    // could reach before; a name must resolve to a WHOLE DISK, because
    // a table written inside a partition describes windows into itself.
    const struct block_device *disk = NULL;
    req.device[sizeof(req.device) - 1] = '\0';
    if (req.device[0]) {
        const struct blk_entry *e = blk_device_by_name(req.device);
        if (!e) {
            klog_printf("mkpart: no such device \"%s\"\n", req.device);
            c->regs[14] = (uint64_t)(int64_t)-ENODEV;
            return 0;
        }
        if (e->parent != e->dev) {
            klog_printf("mkpart: \"%s\" is a partition, not a disk -- refused\n", req.device);
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }
        disk = e->dev;
    } else {
        disk = blk_whole_disk();
    }

    if (!disk || blkdev_sector_count(disk) == 0) {
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
    // ...and it asks about THIS disk, not "is anything mounted": with a
    // device field, repartitioning an empty second disk while the root
    // is mounted elsewhere is the installer's ordinary case, and making
    // it type `confirm` for a disk it is not touching teaches the word
    // to mean nothing. The boot disk still needs it whether or not
    // anything is mounted from it.
    if (disk_is_in_use(disk) && !(req.flags & MKPART_CONFIRM)) {
        klog_printf("mkpart: refused -- %s is in use and MKPART_CONFIRM was not set\n",
                    blk_device_name(disk));
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
    if (!partition_validate_on(disk, &tbl, &why)) {
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
    int ok = partition_write_table_of(disk, &tbl);
    scheduler_preempt_enable();

    if (!ok) {
        c->regs[14] = (uint64_t)(int64_t)-EIO;
        return 0;
    }

    // RE-READ THE TABLE, but only on a disk nothing is running from --
    // Linux's rule for BLKRRPART, and for its reason: handing out
    // windows over a filesystem in use is worse than making the caller
    // reboot. So the boot disk still takes effect at the next boot,
    // and an installer's target gets its `<disk>p<n>` devices at once,
    // which is what lets one program partition, format and mount.
    if (!disk_is_in_use(disk)) {
        int named = mount_rescan_disk(disk);
        klog_printf("mkpart: %s re-read -- %d partition(s) named\n",
                    blk_device_name(disk), named);
    }

    c->regs[14] = 0;
    return 0;
}
