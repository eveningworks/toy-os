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

// driver-none: the syscall half of partition.c

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

// Hand-serialized little-endian, the same convention partition.c uses:
// this kernel builds on-disk structures byte by byte rather than casting
// a struct over them.
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

// WHICH DISK. Empty names the boot disk, which is all these syscalls
// could reach before; a name must resolve to a WHOLE DISK, because both
// operations write structures that describe a whole one. Sets the
// caller's errno and returns NULL on a refusal.
static const struct block_device *resolve_disk(const char *name, struct syscall_ctx *c) {
    const struct block_device *disk;
    if (name[0]) {
        const struct blk_entry *e = blk_device_by_name(name);
        if (!e) {
            klog_printf("partition: no such device \"%s\"\n", name);
            c->regs[14] = (uint64_t)(int64_t)-ENODEV;
            return NULL;
        }
        if (e->parent != e->dev) {
            klog_printf("partition: \"%s\" is a partition, not a disk -- refused\n", name);
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return NULL;
        }
        disk = e->dev;
    } else {
        disk = blk_whole_disk();
    }
    if (!disk || blkdev_sector_count(disk) == 0) {
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return NULL;
    }
    return disk;
}

int sys_mkpart(struct syscall_ctx *c) {
    struct mkpart_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.device[sizeof(req.device) - 1] = '\0';

    const struct block_device *disk = resolve_disk(req.device, c);
    if (!disk) return 0;   // resolve_disk() set the errno

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

        // The role decides the GPT type GUID and, where MBR has an
        // equivalent, the type byte. An unknown role is refused rather
        // than quietly becoming data: a partition typed wrongly is one
        // the boot scan will offer as a root.
        if (!partition_type_guid((enum partition_role)src->role, dst->gpt_type_guid)) {
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }
        if (src->role == MKPART_ROLE_ESP && !src->mbr_type) dst->mbr_type = 0xEF;

        dst->gpt_lba_start = src->start_lba;
        dst->gpt_lba_end = src->start_lba + src->sectors - 1; // GPT's end is INCLUSIVE
        dst->mbr_lba_start = (uint32_t)src->start_lba;
        dst->mbr_num_sectors = (uint32_t)src->sectors;
        if (src->mbr_type) dst->mbr_type = src->mbr_type;

        // ON MBR, THE ESP-ROLE PARTITION IS THE ACTIVE ONE. It holds
        // grub.cfg and the kernel, so it is what a BIOS should chain to
        // -- and a disk with nothing marked active is one a number of
        // firmwares refuse outright. Derived from the role rather than
        // given its own request field: there is exactly one sensible
        // answer, and a caller free to mark the wrong partition would
        // only be free to make an unbootable disk.
        if (tbl.kind == PART_TABLE_MBR && src->role == MKPART_ROLE_ESP)
            dst->mbr_active = 1;

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

// ---- SYS_INSTALL_BOOT ------------------------------------------------

// 512, and it is the BLOCK layer's sector rather than the partition
// code's -- every offset patched below is defined against a 512-byte
// boot sector by the BIOS, not by anything this kernel chose.
#define BOOT_SECTOR_SIZE INSTALL_BOOT_SECTOR_BYTES

// Where a core image goes on this disk: on GPT the BIOS boot partition,
// which is a partition with no filesystem in it that exists for exactly
// this; on MBR the gap before the first partition, which is where GRUB
// has always put it.
//
// NEITHER IS A GUESS. The GPT case matches a type GUID; the MBR case
// ends the window at the earliest partition's start LBA, so a disk
// somebody else partitioned from sector 1 reports no room and is
// refused rather than written into. Returns 0 when there is nowhere to
// put it, which is the honest answer for a disk nobody partitioned for
// booting.
static int bios_boot_window(const struct block_device *disk,
                            uint32_t *out_lba, uint32_t *out_sectors) {
    // STATIC, not a stack local: struct partition_table is ~1.5 KB
    // against a 1 KB kernel frame budget. Safe for the same reason the
    // table write below is -- the preemption guard is taken around it.
    static struct partition_table tbl;
    if (!partition_read_table_of(disk, &tbl)) return 0;

    // MBR HAS NO BIOS-BOOT TYPE, so core.img goes in the GAP between
    // the boot sector and the first partition -- the layout GRUB has
    // used on MBR disks forever. This is DERIVED from the table, not
    // guessed at: the window ends where the earliest partition begins,
    // so a disk somebody else partitioned tightly reports a gap too
    // small and is refused rather than written into.
    if (tbl.kind == PART_TABLE_MBR) {
        uint32_t first = 0xFFFFFFFFu;
        for (int i = 0; i < tbl.entry_count; i++)
            if (tbl.entries[i].mbr_lba_start < first)
                first = tbl.entries[i].mbr_lba_start;
        if (first == 0xFFFFFFFFu || first < 2) return 0;
        *out_lba = 1;
        *out_sectors = first - 1;
        return 1;
    }
    if (tbl.kind != PART_TABLE_GPT) return 0;

    uint8_t want[16];
    if (!partition_type_guid(PART_ROLE_BIOS_BOOT, want)) return 0;

    for (int i = 0; i < tbl.entry_count; i++) {
        const struct partition_entry *pe = &tbl.entries[i];
        if (k_memcmp(pe->gpt_type_guid, want, 16) != 0) continue;
        if (pe->gpt_lba_end < pe->gpt_lba_start) return 0;
        uint64_t n = pe->gpt_lba_end - pe->gpt_lba_start + 1;  // GPT's end is INCLUSIVE
        if (pe->gpt_lba_start > 0xFFFFFFFFull || n > 0xFFFFFFFFull) return 0;
        *out_lba = (uint32_t)pe->gpt_lba_start;
        *out_sectors = (uint32_t)n;
        return 1;
    }
    return 0;
}

int sys_install_boot(struct syscall_ctx *c) {
    struct install_boot_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof(req))) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    req.device[sizeof(req.device) - 1] = '\0';

    const struct block_device *disk = resolve_disk(req.device, c);
    if (!disk) return 0;   // resolve_disk() set the errno

    if (disk_is_in_use(disk) && !(req.flags & INSTALL_BOOT_CONFIRM)) {
        klog_printf("install_boot: refused -- %s is in use and INSTALL_BOOT_CONFIRM was not set\n",
                    blk_device_name(disk));
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }

    // EXACTLY one sector. A boot sector is not a file that can be short:
    // 511 bytes would leave the last byte of the 0x55AA signature
    // whatever was there before.
    if (req.boot_size != INSTALL_BOOT_SECTOR_BYTES) {
        klog_printf("install_boot: refused -- boot image is %u bytes, not %u\n",
                    (unsigned)req.boot_size, INSTALL_BOOT_SECTOR_BYTES);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    uint32_t core_lba = 0, core_room = 0;
    if (!bios_boot_window(disk, &core_lba, &core_room)) {
        klog_printf("install_boot: %s has no BIOS boot partition\n", blk_device_name(disk));
        c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        return 0;
    }

    uint64_t core_sectors = (req.core_size + BOOT_SECTOR_SIZE - 1) / BOOT_SECTOR_SIZE;
    // At least two: the first sector is the one the boot sector loads,
    // and the block list it carries describes the REST. A one-sector
    // core image has no rest and its patched count would be zero.
    if (core_sectors < 2 || core_sectors > core_room) {
        klog_printf("install_boot: refused -- core image is %u sector(s), room for %u\n",
                    (unsigned)core_sectors, core_room);
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    // STATIC for the same frame-budget reason as the table above, and
    // safe for the same one: the preemption guard is held across the
    // whole write.
    static uint8_t boot[BOOT_SECTOR_SIZE];
    static uint8_t sec[BOOT_SECTOR_SIZE];

    scheduler_preempt_disable();
    int ok = 1;

    if (!vmm_copy_from_user(c->pml4, boot, req.boot_img, INSTALL_BOOT_SECTOR_BYTES)) {
        scheduler_preempt_enable();
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    // THE DISK'S OWN BYTES SURVIVE. 0x1b8-0x200 is the disk signature
    // and the partition table; a boot sector written over them boots and
    // describes an empty disk.
    if (!blkdev_read_sectors(disk, 0, 1, sec)) {
        klog_write("install_boot: could not read the target's boot sector\n");
        scheduler_preempt_enable();
        c->regs[14] = (uint64_t)(int64_t)-EIO;
        return 0;
    }
    k_memcpy(boot + 0x1B8, sec + 0x1B8, BOOT_SECTOR_SIZE - 0x1B8);
    // ...and where to find the core image.
    wr64(boot + 0x5C, core_lba);

    // The core image, a sector at a time -- it is tens of KiB and this
    // is a 16 KiB kernel stack. The block list patch lands in the FIRST
    // sector, so it is applied as that sector goes past.
    for (uint64_t i = 0; i < core_sectors && ok; i++) {
        uint32_t want = BOOT_SECTOR_SIZE;
        uint64_t left = req.core_size - i * BOOT_SECTOR_SIZE;
        if (left < want) { k_memset(sec, 0, BOOT_SECTOR_SIZE); want = (uint32_t)left; }
        if (!vmm_copy_from_user(c->pml4, sec, req.core_img + i * BOOT_SECTOR_SIZE, want)) {
            scheduler_preempt_enable();
            c->regs[14] = (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        if (i == 0) {
            wr64(sec + 0x1F4, core_lba + 1);              // where the rest is...
            wr16(sec + 0x1FC, (uint16_t)(core_sectors - 1)); // ...and how much
        }
        if (!blkdev_write_sectors(disk, core_lba + (uint32_t)i, 1, sec)) ok = 0;
    }

    // THE BOOT SECTOR LAST. A power cut partway then leaves a disk whose
    // old boot sector is intact rather than one pointing at a core image
    // that was never finished -- the same publish-last discipline
    // partition_write_table() follows with the protective MBR.
    if (ok && !blkdev_write_sectors(disk, 0, 1, boot)) ok = 0;
    if (ok) blkdev_flush(disk);

    scheduler_preempt_enable();

    if (!ok) {
        klog_write("install_boot: write failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
        return 0;
    }
    klog_printf("install_boot: %s -- boot sector written, core image at LBA %u (%u sectors)\n",
                blk_device_name(disk), core_lba, (unsigned)core_sectors);
    c->regs[14] = 0;
    return 0;
}
