// TFS3 allocation: the RAM bitmaps and their write-through, the
// rollback log that frees what a failed op allocated, TRIM, and the
// write side of the block map. Split out of tfs3.c; tfs3_internal.h has
// the map.

#include "tfs3_internal.h"
#include "mount.h" // MOUNT_MAX -- the mount limit this backend declares
#include "string.h"
#include "klog.h"
#include "heap.h"

// ---- allocation (RAM bitmaps, write-through, leak-safe ordering) ---------

void t3_mark_dirty(uint8_t *set, uint32_t g) { set[g >> 3] |= (uint8_t)(1u << (g & 7)); }
static int test_dirty(const uint8_t *set, uint32_t g) { return (set[g >> 3] >> (g & 7)) & 1; }
static void clear_dirty(uint8_t *set, uint32_t g) { set[g >> 3] &= (uint8_t)~(1u << (g & 7)); }

int t3_bbm_test(struct t3_state *sbi, uint32_t g, uint32_t i) { return (sbi->bbm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
void t3_bbm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v) {
    uint8_t *b = &sbi->bbm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    t3_mark_dirty(sbi->bbm_dirty, g);
}
int t3_ibm_test(struct t3_state *sbi, uint32_t g, uint32_t i) { return (sbi->ibm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
void t3_ibm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v) {
    uint8_t *b = &sbi->ibm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    t3_mark_dirty(sbi->ibm_dirty, g);
}

static void alog_push(struct t3_state *sbi, uint32_t blk); // rollback log, defined below with its story

// Highest usable data offset within group g (backup regions excluded).
uint32_t t3_group_data_end(struct t3_state *sbi, uint32_t g) {
    uint32_t groups[2];
    int n = t3_backup_groups(sbi->sb.gc, groups);
    uint32_t span = t3_group_span(sbi, g);
    for (int i = 0; i < n; i++) {
        if (groups[i] == g) {
            return span > T3_BACKUP_BLOCKS ? span - T3_BACKUP_BLOCKS : 0;
        }
    }
    return span;
}

// Try to allocate one specific block (the adjacent-first fast path).

static int alloc_block_at(struct t3_state *sbi, uint32_t blk) {
    t3_trim_flush(sbi);
    if (blk < sbi->group0 + sbi->meta_off) return 0;
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc || i < sbi->meta_off || i >= t3_group_data_end(sbi, g)) return 0;
    if (t3_bbm_test(sbi, g, i)) return 0;
    t3_bbm_set(sbi, g, i, 1);
    sbi->gd[g].free_blocks--;
    t3_mark_dirty(sbi->gdt_dirty, g);
    alog_push(sbi, blk);
    return 1;
}

// Allocate one data block: `hint` (try hint+1's spirit: exactly that
// block) first, then rotor scan of the preferred group, then every
// other group. Returns the block number or 0.
uint32_t t3_alloc_block(struct t3_state *sbi, uint32_t prefer_group, uint32_t adjacent_to) {
    t3_trim_flush(sbi);
    if (adjacent_to && alloc_block_at(sbi, adjacent_to + 1)) return adjacent_to + 1;
    if (prefer_group >= sbi->sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < sbi->sb.gc; n++) {
        uint32_t g = (prefer_group + n) % sbi->sb.gc;
        if (sbi->gd[g].free_blocks == 0) continue;
        uint32_t end = t3_group_data_end(sbi, g);
        uint32_t start = sbi->rotor[g];
        if (start < sbi->meta_off || start >= end) start = sbi->meta_off;
        for (uint32_t k = 0; k < end - sbi->meta_off; k++) {
            uint32_t i = start + k;
            if (i >= end) i = sbi->meta_off + (i - end);
            if (!t3_bbm_test(sbi, g, i)) {
                t3_bbm_set(sbi, g, i, 1);
                sbi->gd[g].free_blocks--;
                t3_mark_dirty(sbi->gdt_dirty, g);
                sbi->rotor[g] = i + 1;
                alog_push(sbi, t3_group_base(sbi, g) + i);
                return t3_group_base(sbi, g) + i;
            }
        }
    }
    return 0;
}

void t3_free_block_bit(struct t3_state *sbi, uint32_t blk) {
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc) return;
    if (!t3_bbm_test(sbi, g, i)) return; // double-free guard -- fsck's problem, not a crash
    // NOT WHILE A READ IS AT THE DEVICE WITHOUT THE LOCK: the block could
    // be reallocated and written before that read lands, and it would
    // return another file's bytes. Linux's inode_dio_wait(), per mount.
    mount_io_drain(sbi);
    sbi->free_gen++;
    t3_bbm_set(sbi, g, i, 0);
    sbi->gd[g].free_blocks++;
    t3_mark_dirty(sbi->gdt_dirty, g);
    if (sbi->rotor[g] > i) sbi->rotor[g] = i;
    // THE CACHES ARE KEYED BY BLOCK NUMBER AND OUTLIVE ONE OPERATION,
    // so a freed table block must be forgotten here -- reallocating it
    // as something else would otherwise hand the next walk a stale
    // image, and the write that followed would land in the wrong file.
    t3_map_cache_forget(sbi, blk);
}

uint64_t t3_alloc_inode(struct t3_state *sbi, uint32_t prefer_group) {
    if (prefer_group >= sbi->sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < sbi->sb.gc; n++) {
        uint32_t g = (prefer_group + n) % sbi->sb.gc;
        if (sbi->gd[g].free_inodes == 0) continue;
        for (uint32_t i = 0; i < sbi->sb.ipg; i++) {
            if (!t3_ibm_test(sbi, g, i)) {
                t3_ibm_set(sbi, g, i, 1);
                sbi->gd[g].free_inodes--;
                t3_mark_dirty(sbi->gdt_dirty, g);
                return (uint64_t)g * sbi->sb.ipg + i;
            }
        }
    }
    return 0;
}

void t3_free_inode_bit(struct t3_state *sbi, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc || !t3_ibm_test(sbi, g, i)) return;
    sbi->ino_free_gen++;
    t3_ibm_set(sbi, g, i, 0);
    sbi->gd[g].free_inodes++;
    t3_mark_dirty(sbi->gdt_dirty, g);
}

// Write every dirty bitmap block and GDT block through to disk.
// UNJOURNALED on purpose: called BEFORE the transaction that makes an
// allocation reachable (set-before-use) and AFTER the transaction
// that makes a free unreachable (clear-after-persist), so a crash at
// any point costs a leaked block/inode -- never a double allocation.
// TFS2's exact metadata-ordering rule, see docs/decisions.md.
int t3_flush_alloc_state(struct t3_state *sbi) {
    int ok = 1;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        if (test_dirty(sbi->bbm_dirty, g)) {
            if (!t3_write_block(sbi, t3_group_base(sbi, g), sbi->bbm + g * T3_BLOCK)) ok = 0;
            clear_dirty(sbi->bbm_dirty, g);
        }
        if (test_dirty(sbi->ibm_dirty, g)) {
            if (!t3_write_block(sbi, t3_group_base(sbi, g) + 1, sbi->ibm + g * T3_BLOCK)) ok = 0;
            clear_dirty(sbi->ibm_dirty, g);
        }
    }
    // GDT blocks: rebuild each dirty block from the RAM free counts.
    // Backup GDT snapshots are deliberately NOT refreshed (stale by
    // design -- fsck recomputes counts anyway; see the design doc).
    for (uint32_t tb = 0; tb < T3_GDT_BLOCKS; tb++) {
        int dirty = 0;
        for (uint32_t i = 0; i < T3_BLOCK / 16 && !dirty; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g < sbi->sb.gc && test_dirty(sbi->gdt_dirty, g)) dirty = 1;
        }
        if (!dirty) continue;
        k_memset(sbi->blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= sbi->sb.gc) break;
            uint8_t *e = sbi->blk + i * 16;
            wr32(e, sbi->gd[g].free_blocks);
            wr32(e + 4, sbi->gd[g].free_inodes);
            wr32(e + 12, k_fnv1a(e, 12));
            clear_dirty(sbi->gdt_dirty, g);
        }
        if (!t3_write_block(sbi, sbi->gdt_block + tb, sbi->blk)) ok = 0;
    }
    return ok;
}

// ---- allocation rollback log ---------------------------------------------
//
// "Crash = leak, fsck reclaims" is the CRASH story; a runtime failure
// (a refused write, out of space midway) is not allowed to leak --
// TFS2's failure paths reattach/free what they took, and the fault-
// injection KTESTs enforce exactly that by running fsck after the
// failure tests. So every blocking mutation logs what it allocates
// and frees it all if the operation fails. Single-threaded kernel:
// one active log, armed around one operation at a time. The steppable
// write path arms it PER STEP only (other fs ops interleave between
// steps, and a global log would swallow their allocations); blocks
// from earlier, completed steps of an abandoned stepped write do
// still leak -- that path is crash-shaped by nature, and fsck's
// reclaim is the designed answer there.

void t3_alog_begin(struct t3_state *sbi) { sbi->alog.n = 0; sbi->alog.active = 1; sbi->alog.overflow = 0; }

static void alog_push(struct t3_state *sbi, uint32_t blk) {
    if (!sbi->alog.active || sbi->alog.overflow) return;
    if (sbi->alog.n == sbi->alog.cap) {
        uint32_t ncap = sbi->alog.cap ? sbi->alog.cap * 2 : 64;
        uint32_t *nv = kmalloc(ncap * sizeof(uint32_t));
        if (!nv) { sbi->alog.overflow = 1; return; }
        if (sbi->alog.v) { k_memcpy(nv, sbi->alog.v, sbi->alog.n * sizeof(uint32_t)); kfree(sbi->alog.v); }
        sbi->alog.v = nv;
        sbi->alog.cap = ncap;
    }
    sbi->alog.v[sbi->alog.n++] = blk;
}

void t3_alog_commit(struct t3_state *sbi) { sbi->alog.active = 0; }

// Remove one block from the log: it just became referenced by a
// COMMITTED transaction (directory growth commits mid-operation), so
// an outer rollback must not free it out from under that reference.
void t3_alog_forget(struct t3_state *sbi, uint32_t blk) {
    for (uint32_t i = 0; i < sbi->alog.n; i++) {
        if (sbi->alog.v[i] == blk) {
            sbi->alog.v[i] = sbi->alog.v[--sbi->alog.n];
            return;
        }
    }
}

// Drop the log without freeing any of it: a committed transaction
// names these blocks now, so they are live even though the operation
// that allocated them failed.
void t3_alog_cancel(struct t3_state *sbi) { sbi->alog.n = 0; sbi->alog.active = 0; }

void t3_alog_rollback(struct t3_state *sbi) {
    if (sbi->alog.overflow) {
        // Couldn't track everything -- leak honestly rather than free
        // a partial (possibly wrong) set. fsck reclaims.
        klog_write("tfs3: rollback log overflowed -- leaked blocks left for fsck\n");
    } else {
        for (uint32_t i = 0; i < sbi->alog.n; i++) t3_free_block_bit(sbi, sbi->alog.v[i]);
        t3_flush_alloc_state(sbi);
    }
    sbi->alog.active = 0;
}

// TRIM freed blocks, best-effort -- parity with TFS2's free_block(): the
// block is free either way, a refused TRIM must not fail the delete.
//
// QUEUED, MERGED, AND SENT AS ONE LIST, because the device's cost is per
// COMMAND: a 512 MiB delete issued a TRIM per run, 313 of them, 89 ms
// with nothing else able to run. A delete's runs are mostly adjacent (a
// pointer table sits next to the data it maps), so they merge to a
// handful, and the block layer packs 64 to a command.
//
// THE QUEUE MUST BE EMPTY BEFORE ANY BLOCK IS ALLOCATED: a discard
// arriving after a freed block was reused and written destroys the new
// data. So every freeing operation flushes before it returns, and both
// allocators flush first as well, in case one did not.

void t3_trim_flush(struct t3_state *sbi) {
    if (sbi->trim_n) blkdev_trim_ranges(sbi->vol.dev, sbi->trim_q, sbi->trim_n);
    sbi->trim_n = 0;
}

void t3_trim_run(struct t3_state *sbi, uint32_t first_blk, uint32_t count) {
    if (!count || !blkdev_trim_supported(sbi->vol.dev)) return;
    uint32_t lba = sbi->vol.base_lba + first_blk * T3_SPB, n = count * T3_SPB;
    for (int i = 0; i < sbi->trim_n; i++) {
        struct blk_range *q = &sbi->trim_q[i];
        if (q->lba + q->count == lba) { q->count += n; return; }
        if (lba + n == q->lba) { q->lba = lba; q->count += n; return; }
    }
    if (sbi->trim_n == TRIM_QUEUE) t3_trim_flush(sbi);
    sbi->trim_q[sbi->trim_n++] = (struct blk_range){ lba, n };
}

// ---- block-map allocation (write side) ------------------------------------


// THE MIDDLE LEVELS OF THE WRITE WALK, cached the way `pcache` caches
// the leaf. Without this, t3_map_get_or_alloc_tables() re-read the mid
// table from the device for EVERY block written past 4 MiB -- the same
// defect t3_block_for_index() had on the read side, and the comment below
// the walk claimed the opposite ("one RMW per 1024 data blocks"), which
// described the intent rather than the code.
//
// Two entries because a triple-indirect walk touches two middle levels.
// Per mount, like the journal. Its flush and drop are folded into
// pcache's below, so every existing call site covers it.

static int mcache_flush(struct t3_state *sbi) {
    for (int i = 0; i < 2; i++) {
        if (sbi->mcache[i].blk && sbi->mcache[i].dirty) {
            if (!t3_write_block(sbi, sbi->mcache[i].blk, sbi->mcache[i].buf)) return 0;
            sbi->mcache[i].dirty = 0;
        }
    }
    return 1;
}

static int mcache_load(struct t3_state *sbi, int level, uint32_t blk, int fresh) {
    if (sbi->mcache[level].blk == blk) return 1;
    if (sbi->mcache[level].blk && sbi->mcache[level].dirty) {
        if (!t3_write_block(sbi, sbi->mcache[level].blk, sbi->mcache[level].buf)) return 0;
    }
    sbi->mcache[level].blk = blk;
    sbi->mcache[level].dirty = 0;
    if (fresh) {
        // A FRESH TABLE IS DIRTY IMMEDIATELY, so it lands on disk even
        // if nothing else writes into it -- the write the old walk did
        // unconditionally for the same reason.
        k_memset(sbi->mcache[level].buf, 0, T3_BLOCK);
        sbi->mcache[level].dirty = 1;
        return 1;
    }
    return t3_read_block(sbi, blk, sbi->mcache[level].buf);
}

int t3_pcache_flush(struct t3_state *sbi) {
    if (!mcache_flush(sbi)) return 0;
    if (sbi->pcache.blk && sbi->pcache.dirty) {
        if (!t3_write_block(sbi, sbi->pcache.blk, sbi->pcache.buf)) return 0;
        sbi->pcache.dirty = 0;
    }
    return 1;
}

static int pcache_load(struct t3_state *sbi, uint32_t blk, int fresh) {
    if (sbi->pcache.blk == blk) return 1;
    if (!t3_pcache_flush(sbi)) return 0;
    sbi->pcache.blk = blk;
    sbi->pcache.dirty = 0;
    if (fresh) { k_memset(sbi->pcache.buf, 0, T3_BLOCK); sbi->pcache.dirty = 1; return 1; }
    return t3_read_block(sbi, blk, sbi->pcache.buf);
}

// Forget one block wherever it is cached. Clean entries only ever
// reach here: a dirty table belongs to the operation still running,
// which cannot be freeing its own table.
void t3_map_cache_forget(struct t3_state *sbi, uint32_t blk) {
    if (sbi && sbi->pcache.blk == blk) { sbi->pcache.blk = 0; sbi->pcache.dirty = 0; }
    if (!sbi) return;
    for (int i = 0; i < 2; i++)
        if (sbi->mcache[i].blk == blk) { sbi->mcache[i].blk = 0; sbi->mcache[i].dirty = 0; }
}

void t3_pcache_drop(struct t3_state *sbi) {
    sbi->pcache.blk = 0;
    sbi->pcache.dirty = 0;
    for (int i = 0; i < 2; i++) { sbi->mcache[i].blk = 0; sbi->mcache[i].dirty = 0; }
}

// Allocate (if needed) and return the pointer-table slot chain for
// file-block `idx`, allocating intermediate pointer blocks as it
// goes. Returns the existing-or-new data block number via *out_blk
// (0 if a fresh one must be allocated by the caller and recorded with
// t3_map_set_block()). This walks with the pcache for the LEAF table.
int t3_map_get_or_alloc_tables(struct t3_state *sbi, struct t3_inode *node, uint32_t idx,
                                    uint32_t prefer_group,
                                    uint32_t *out_leaf_blk, uint32_t *out_leaf_slot,
                                    uint32_t *out_existing) {
    if ((uint64_t)idx >= T3_MAX_FILE_BLOCKS) return 0;
    if (idx < 12) {
        *out_leaf_blk = 0; // direct -- lives in the inode itself
        *out_leaf_slot = idx;
        *out_existing = node->ptrs[idx];
        return 1;
    }
    idx -= 12;
    uint32_t l1 = T3_PTRS_PER_BLOCK;
    uint32_t l2 = l1 * l1;
    int ptr_index;         // 12 single, 13 double, 14 triple
    uint32_t rem = idx;
    if (idx < l1) { ptr_index = 12; }
    else if (idx - l1 < l2) { ptr_index = 13; rem = idx - l1; }
    else { ptr_index = 14; rem = idx - l1 - l2; }

    // Top-level table.
    uint32_t table = node->ptrs[ptr_index];
    int fresh = 0;
    if (!table) {
        table = t3_alloc_block(sbi, prefer_group, 0);
        if (!table) return 0;
        node->ptrs[ptr_index] = table;
        fresh = 1;
    }
    if (ptr_index == 12) {
        if (!pcache_load(sbi, table, fresh)) return 0;
        *out_leaf_blk = table;
        *out_leaf_slot = rem;
        *out_existing = rd32(sbi->pcache.buf + rem * 4);
        return 1;
    }

    // Middle level(s), through mcache -- so a long write reads each
    // one ONCE and patches it in RAM, which is what the old comment
    // here already claimed was happening. It was not: the read and the
    // write-back were both inside this loop, so every data block past
    // 4 MiB paid a table round trip.
    uint32_t levels = (ptr_index == 13) ? 1 : 2;
    uint32_t divisors[2] = { l1, 1 };
    if (levels == 2) { divisors[0] = l2; divisors[1] = l1; }
    for (uint32_t d = 0; d < levels; d++) {
        uint32_t slot = rem / divisors[d];
        rem = rem % divisors[d];
        if (!mcache_load(sbi, (int)d, table, fresh)) return 0;
        uint32_t next = rd32(sbi->mcache[d].buf + slot * 4);
        int next_fresh = 0;
        if (!next) {
            next = t3_alloc_block(sbi, prefer_group, 0);
            if (!next) return 0;
            wr32(sbi->mcache[d].buf + slot * 4, next);
            sbi->mcache[d].dirty = 1;
            next_fresh = 1;
        }
        table = next;
        fresh = next_fresh;
    }
    if (!pcache_load(sbi, table, fresh)) return 0;
    *out_leaf_blk = table;
    *out_leaf_slot = rem;
    *out_existing = rd32(sbi->pcache.buf + rem * 4);
    return 1;
}

void t3_map_set_block(struct t3_state *sbi, struct t3_inode *node, uint32_t leaf_blk,
                          uint32_t leaf_slot, uint32_t data_blk) {
    if (!leaf_blk) {
        node->ptrs[leaf_slot] = data_blk; // direct
    } else {
        wr32(sbi->pcache.buf + leaf_slot * 4, data_blk);
        sbi->pcache.dirty = 1;
    }
}
