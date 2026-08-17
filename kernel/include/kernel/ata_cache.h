#ifndef ATA_CACHE_H
#define ATA_CACHE_H

#include <stdint.h>

// A write-back sector cache, sitting directly under ata_read_sectors()
// and ata_write_sectors().
//
// WHY HERE AND NOT IN THE BLOCK LAYER
// -----------------------------------
// The block layer is the tidier home for it and it is the wrong one:
// TFS2 makes seven direct ata_* calls and partition.c three more, so a
// cache above them would sit beside two bypass paths. A bypass past a
// WRITE-BACK cache is not a missed optimisation, it is a correctness
// hole in both directions -- the bypassing reader sees a stale sector,
// and a later write-back of a dirty line overwrites what the bypassing
// writer put there. Both are silent.
//
// ata_read_sectors()/ata_write_sectors() are the one place every caller
// in this kernel funnels through, so caching there means no bypass path
// can exist to get wrong. The cost is that this lives in a driver
// rather than in a layer, and that a RAM-backed live image gets no
// cache -- which is right anyway: its "I/O" is already a memcpy.
//
// WHAT WRITE-BACK CHANGES, AND THE INVARIANT THAT MATTERS
// -------------------------------------------------------
// A write returns once the data is in RAM, so a drive that refuses it
// reports the failure LATER, at the flush that writes the line back.
// That is the whole risk of this feature: TFS3's journal is only safe
// because its two barriers mean "everything before this is on the
// platter", and a barrier that cannot fail cannot say otherwise.
//
// So atac_flush() RETURNS a status, ata_flush_now() and blk_flush()
// pass it up, and tfs3.c's txn_commit() checks it -- all three used to
// be `void`. A failed write-back keeps its line DIRTY rather than
// dropping it (losing the data is the one unrecoverable outcome) and
// says so, which lets the filesystem leave its journal committed for
// replay instead of proceeding as though the disk had agreed.
//
// TRANSFERS LARGER THAN ATAC_MAX_LINES SECTORS BYPASS THE CACHE, and a
// bypass is never a plain passthrough: overlapping dirty lines are
// written back first (so a raw read sees current data) and overlapping
// lines are then invalidated (so a raw write cannot later be clobbered
// by a stale write-back). Bulk file data therefore does not evict the
// metadata this cache exists to hold.
//
// RE-ENTRANCY: this keeps mutable state, so it inherits the rule the
// filesystem learned the hard way -- vfs.c holds
// scheduler_preempt_disable() across every backend call, which is what
// keeps two callers out of here at once. The g_busy guard below is the
// second line, for the one path that is not reached through vfs.c
// (atac_idle(), from scheduler_idle()).

// One line is one 512-byte sector. Per-sector granularity rather than
// per-4KiB-block: TFS3 issues 1-sector and 8-sector transfers, and a
// partition offset can make an 8-sector transfer unaligned to any
// larger unit, so a bigger line would need alignment reasoning that
// per-sector lines simply do not have.
#define ATAC_WAYS  4u
#define ATAC_SETS  128u
#define ATAC_LINES (ATAC_WAYS * ATAC_SETS)   // 512 sectors = 256 KiB

// The largest transfer served THROUGH the cache. TFS3's metadata is 1
// or 8 sectors; its bulk data runs reach 128, and those would evict the
// whole cache for data nobody re-reads.
#define ATAC_MAX_LINES 8

// Write-backs start once this many lines are dirty, so a burst of
// metadata writes cannot sit in RAM unboundedly.
#define ATAC_DIRTY_HIGH (ATAC_LINES / 4)

// ...and after this long with no new write, whichever comes first. In
// PIT ticks (100 Hz), so a second of quiet.
#define ATAC_IDLE_TICKS 100

// The device operations the cache drives. Registered by ata.c rather
// than called directly, so this file knows nothing about hardware and
// the recursion (cache -> driver -> cache) cannot happen.
struct atac_ops {
    int (*read)(uint32_t lba, int count, void *buf);
    int (*write)(uint32_t lba, int count, const void *buf);
    int (*flush)(void); // the DEVICE's own cache flush, after write-back
};

void atac_init(const struct atac_ops *ops);

// 1 if the cache is active. It is not until atac_init(), so every entry
// point below is a safe no-op before that -- which is what lets ata.c
// call them unconditionally.
int atac_enabled(void);
void atac_set_enabled(int on); // switching OFF flushes first

// The read/write path. Both return 1 on success, 0 on failure, exactly
// as the raw driver calls do.
int atac_read(uint32_t lba, int count, void *buf);
int atac_write(uint32_t lba, int count, const void *buf);

// Write back every dirty line, then flush the device. Returns 1 only if
// BOTH succeeded -- see the invariant above. A caller treating this as
// void is the bug this whole file has to avoid.
int atac_flush(void);

// Drops every clean line, writing dirty ones back first. For anything
// that changes what the sectors underneath MEAN -- reformatting, a
// backend switch -- where cached contents describe a filesystem that no
// longer exists. Returns atac_flush()'s status.
int atac_drop(void);

// Called from scheduler_idle(): writes back after ATAC_IDLE_TICKS of
// quiet. A no-op with nothing dirty, and it refuses to run
// re-entrantly, so an idle poll reached from inside a disk operation
// cannot start a second transfer on top of one already in flight.
void atac_idle(void);

// For `sync` and diagnostics. `dirty` is what a flush would have to
// write; `valid` is how much of the cache is populated.
struct atac_stats {
    uint32_t hits, misses, writes, writebacks, evictions;
    uint32_t dirty, valid;
    uint32_t flushes, failed_writebacks;
};
void atac_get_stats(struct atac_stats *out);

#endif
