// TFS3 -- block groups, inodes, dirent blocks. Format spec:
// docs/tfs3-design.md; host mirror: tools/tfs3_writer.py, which reads and
// writes the same format from the host and is kept in lockstep with it.
//
// This file is the volume, its caches, the superblock, the READ path and
// the inode locks, probe/format/mount and the read ops. The write side,
// the journal, allocation and fsck are beside it -- tfs3_internal.h has
// the map.
//
// Everything on disk is VOLUME-relative: block b lives at sector
// sbi->vol.base_lba + b * T3_SPB, and the backend only touches the disk
// through the vol_* helpers below. The volume is {0, blk_sector_count()}
// and STAYS that way even inside a partition, because the
// probe loop hands in a partition's extent instead and nothing here
// changes -- that seam is the point (see the design doc's "Volumes
// and partitions").
#include "clockevent.h" // the idle commit is due at a time
#include "errno.h"   // fs_chmod returns a negative errno
#include "tfs3.h"
#include "tfs3_internal.h"
#include "ktime.h"
#include "caltime.h"
#include "mount.h" // MOUNT_MAX -- the mount limit this backend declares
#include "string.h"
#include "storage_config.h" // storage.sync -- whether the barriers are real
#include "block_stat.h"     // the read counter lookup() brackets itself with
#include "clocksource.h"    // clocksource_now_ns()
#include "kfmt.h"           // klog_printf() -- the read-only transition says why
#include "klog.h"
#include "heap.h"
#include "scheduler.h" // the inode locks: whose op, and the wake

// The journal staging and the per-call scratch used to live here, at
// file scope; they are in struct t3_state now (see its last section).

// FLUSHES BOTH CACHES, and the resolved-path one rides here rather than
// getting its own entry point on purpose: the call sites that already
// invalidate names -- delete, link, rename, unmount -- are exactly the
// operations that can change which inode a path names. NOT CREATE: both
// caches hold only names that resolved, and adding a name changes none
// of them; its flush cost every create in a directory a walk from the
// root.
// A second function would have to be added to all five, and the one
// that got missed would hand back a stale inode number, which is a read
// of somebody else's file.
void t3_ncache_flush(struct t3_state *sbi) {
    for (int i = 0; i < T3_NCACHE; i++) sbi->ncache[i].dir = 0;
    sbi->ncache_next = 0;
    for (int i = 0; i < T3_LCACHE; i++) sbi->lcache[i].path[0] = '\0';
    sbi->lcache_next = 0;
}

// ---- volume I/O ----------------------------------------------------------

// A STAGED SECTOR IS READ FROM THE TRANSACTION, NOT FROM THE DISK.
//
// Under `storage.sync = batched` a transaction stays open across
// writes, so the newest image of an inode block lives in txn_img
// while the disk still holds the previous one. A reader going to the
// disk sees a STALE inode -- the old size, the old block pointers --
// and that is not subtle: `diskbench`'s read pass failed outright with
// a short read, because fs_size() reported the size the file had before
// writes that had already returned success.
//
// HERE, not in t3_read_block(), and that is the whole point. t3_read_inode()
// reads ONE SECTOR (`t3_vol_read_sectors(lba, 1, ...)`), not a block, so a
// block-level overlay missed exactly the read that mattered and the
// symptom did not change. This is the one function every read in this
// backend passes through, which is what makes the overlay complete
// rather than nearly complete.
//
// The staged image IS the current truth; that is what an open
// transaction means. Only the DEFERRED one is consulted -- an ordinary
// transaction opens, stages and commits with no read in between.
int t3_vol_read_sectors(struct t3_state *sbi, uint32_t lba, int count, void *buf) {
    if (lba + (uint32_t)count > sbi->vol.sector_count) return 0;
    if (sbi->txn_deferred) {
        for (int i = 0; i < sbi->txn_count; i++) {
            uint32_t base = sbi->txn_target[i] * T3_SPB;
            if (lba < base || lba + (uint32_t)count > base + T3_SPB) continue;
            k_memcpy(buf, sbi->txn_img[i] + (lba - base) * T3_SECTOR,
                     (uint32_t)count * T3_SECTOR);
            return 1;
        }
    }
    // Partial: a one-sector metadata read (t3_read_inode(), the superblock)
    // is less than a block on a 4K-sector disk. Whole blocks pass through.
    return blkdev_read_partial(sbi->vol.dev, sbi->vol.base_lba + lba, count, buf);
}


// Stop writing this volume, and say so where a person can see it.
// The mount flag is the REPORT (df, mount); t3_state.readonly is the
// ENFORCEMENT, because at init() time -- a failed replay -- there is
// no mount entry yet to flag.
void t3_vol_go_readonly(struct t3_state *sbi, const char *why) {
    if (sbi->readonly) return;
    sbi->readonly = 1;
    klog_printf("tfs3: %s -- the volume is read-only until the next boot "
                "replays its journal\n", why);
    mount_force_readonly(sbi->vol.dev, why);
}

// A FILE-DATA run, read with the mount's lock DROPPED when that is safe
// (mount.h's mount_io_begin()), so the rest of the volume is not held
// up for the length of a disk transfer. Returns 1 read, 0 failed, and
// -1 when the mount went away while unlocked -- the caller must then
// return at once WITHOUT touching sbi, which is freed.
//
// Everything the gap needs is copied out first; nothing per-mount is
// touched in it. A range a deferred transaction has STAGED is read
// from the staging instead, which is per-mount state, so under the lock.
static int vol_read_run(struct t3_state *sbi, uint32_t lba, int count, void *buf) {
    if (lba + (uint32_t)count > sbi->vol.sector_count) return 0;
    if (sbi->txn_deferred) {
        for (int i = 0; i < sbi->txn_count; i++) {
            uint32_t base = sbi->txn_target[i] * T3_SPB;
            if (lba < base + T3_SPB && lba + (uint32_t)count > base)
                return t3_vol_read_sectors(sbi, lba, count, buf);
        }
    }
    const struct block_device *dev = sbi->vol.dev;
    uint32_t at = sbi->vol.base_lba + lba;
    struct mount_io g;
    if (!mount_io_begin(sbi, &g)) return blkdev_read_sectors(dev, at, count, buf);
    int ok = blkdev_read_sectors(dev, at, count, buf);
    if (!mount_io_end(&g)) return -1;
    return ok;
}

// A POINTER TABLE read the same way -- for a table only this op can reach:
// its file is past its commit, locked, and its blocks not yet freed, so
// nobody writes the table while the lock is dropped (free_all_blocks()).
// Returns as vol_read_run() does.
int t3_read_block_unlocked(struct t3_state *sbi, uint32_t blk, void *buf) {
    return vol_read_run(sbi, blk * T3_SPB, (int)T3_SPB, buf);
}

// The write half, for OVERWRITES only (do_write()): a data run rewritten
// in place, with the lock dropped. Returns as vol_read_run() does.
//
// **IT DOES NOT DROP THE READ-SIDE POINTER CACHE**, which every other
// write does (t3_vol_write_sectors()). That rule guards a block that WAS a
// pointer table being reused as data under its old cached number; an
// overwrite's targets are this file's live data blocks, found through its
// own tables, so none of them is a table anyone has cached. Dropping it
// anyway cost a table re-read per 4 KiB random overwrite: 30% of the
// ASUS's random-write rate.
int t3_vol_write_run(struct t3_state *sbi, uint32_t lba, int count, const void *buf) {
    if (sbi->readonly) return 0;
    if (lba + (uint32_t)count > sbi->vol.sector_count) return 0;
    const struct block_device *dev = sbi->vol.dev;
    uint32_t at = sbi->vol.base_lba + lba;
    struct mount_io g;
    if (!mount_io_begin(sbi, &g)) return blkdev_write_sectors(dev, at, count, buf);
    int ok = blkdev_write_sectors(dev, at, count, buf);
    if (!mount_io_end(&g)) return -1;
    return ok;
}

int t3_vol_write_sectors(struct t3_state *sbi, uint32_t lba, int count, const void *buf) {
    if (sbi->readonly) return 0;   // see t3_state.readonly -- one gate, not ten
    if (lba + (uint32_t)count > sbi->vol.sector_count) return 0;
    // ANY write drops the pointer-table read cache -- see rcache_get().
    // Here rather than in t3_write_block() because a coalesced data run
    // goes straight to the device, and a block that was a pointer table
    // before it was freed and reused as data would otherwise still be
    // cached under its old number.
    t3_rcache_drop(sbi);
    // Partial for the one-sector writes (the journal header, format's
    // superblock wipes): every one is under the mount's lock, which is
    // what the read-modify-write on a 4K-sector disk needs.
    return blkdev_write_partial(sbi->vol.dev, sbi->vol.base_lba + lba, count, buf);
}

int t3_read_block(struct t3_state *sbi, uint32_t blk, void *buf) {
    return t3_vol_read_sectors(sbi, blk * T3_SPB, (int)T3_SPB, buf);
}

int t3_write_block(struct t3_state *sbi, uint32_t blk, const void *buf) {
    return t3_vol_write_sectors(sbi, blk * T3_SPB, (int)T3_SPB, buf);
}

// ---- the read side's pointer-table cache -----------------------------
//
// THE READ HALF OF WHAT `pcache` DOES FOR WRITES, and the reason reads
// were the slow direction: t3_block_for_index() re-read the indirect table
// from the device for EVERY 4 KiB block, so a sequential read of a file
// past the double-indirect boundary issued three commands per block --
// measured at 13189 reads and 93689 sectors for 4096 blocks of data,
// a 2.9x sector amplification the write path had not had since run
// coalescing landed.
//
// ONE ENTRY PER LEVEL, not one entry total. A walk touches top, then
// mid, then leaf, and a single slot would evict the level above on
// every step -- turning three reads per block into three misses per
// block, which is what it already did.
//
// PER MOUNT: block numbers are volume-relative, so a cache shared by two
// mounts served one volume's table for the other's block (it was only
// dropped on writes).
//
// THE INVARIANT, AND IT IS THE WHOLE SAFETY ARGUMENT: an entry may only
// hold what is on the device. t3_write_block() therefore DROPS the cache
// -- every write, unconditionally, whether or not it targeted a
// pointer table. Anything cleverer needs to know which blocks are
// tables, and being wrong once means a read served from a stale table
// returns another file's data.

void t3_rcache_drop(struct t3_state *sbi) {
    for (int i = 0; i < T3_RCACHE_LEVELS; i++) sbi->rcache[i].blk = 0;
}

// Returns the cached image of pointer-table block `blk` at `level`,
// reading it if it is not already there. NULL on a device error.
static const uint8_t *rcache_get(struct t3_state *sbi, int level, uint32_t blk) {
    if (level < 0 || level >= T3_RCACHE_LEVELS || !blk) return NULL;
    if (sbi->rcache[level].blk == blk) return sbi->rcache[level].buf;
    if (!t3_read_block(sbi, blk, sbi->rcache[level].buf)) {
        sbi->rcache[level].blk = 0;
        return NULL;
    }
    sbi->rcache[level].blk = blk;
    return sbi->rcache[level].buf;
}

// The whole of the device we were handed. "Flat" means volume-relative
// with no offset of our own -- the partition window, if there is one,
// lives in the device (block_part.c) and TFS3 never learns of it.
static void set_flat_volume(struct t3_state *sbi, const struct block_device *dev) {
    sbi->vol.dev = dev;
    sbi->vol.base_lba = 0;
    sbi->vol.sector_count = blkdev_sector_count(dev);
}

// ---- superblock ----------------------------------------------------------

// Point the geometry globals at one version's constant set. Returns 0
// for a version this kernel doesn't know, which is how an image from a
// future format is refused rather than misread.
static int set_geometry_version(struct t3_state *sbi, uint32_t v) {
    if (v == 1) {
        sbi->group0 = T3_V1_GROUP0; sbi->gdt_block = T3_V1_GDT;
        sbi->jdata_block = T3_V1_JDATA; sbi->jslots = T3_V1_JSLOTS;
        return 1;
    }
    if (v == 2) {
        sbi->group0 = T3_V2_GROUP0; sbi->gdt_block = T3_V2_GDT;
        sbi->jdata_block = T3_V2_JDATA; sbi->jslots = T3_V2_JSLOTS;
        return 1;
    }
    return 0;
}

// group0_start for a version, without disturbing the active geometry
// -- load_superblock()'s backup search needs it before it knows which
// version it's looking at.
static uint32_t group0_for_version(uint32_t v) {
    return v == 1 ? T3_V1_GROUP0 : T3_V2_GROUP0;
}

// Parses+validates one superblock sector into sbi->sb, and (on success)
// switches the geometry globals to its version. Returns 1 valid.
//
// The offsets at 28/32/36 are the version's own constants written down
// -- validated, never believed: a superblock that disagrees with its
// declared version's layout is corrupt, not a differently-shaped
// filesystem, because the layout is what makes the backups findable
// when this sector is the thing that's unreadable.
static int parse_superblock(struct t3_state *sbi, const uint8_t *sec) {
    if (!(sec[0] == 'T' && sec[1] == 'F' && sec[2] == 'S' && sec[3] == '3')) return 0;
    uint32_t version = sec[4];
    if (version < T3_VERSION_MIN || version > T3_VERSION) return 0;
    if (k_fnv1a(sec, 44) != rd32(sec + 44)) return 0;
    uint32_t total_blocks = rd32(sec + 8);
    uint32_t bpg = rd32(sec + 12);
    uint32_t ipg = rd32(sec + 16);
    uint32_t gc = rd32(sec + 20);
    uint32_t group0 = rd32(sec + 24);
    if (bpg != T3_BPG || group0 != group0_for_version(version)) return 0;
    if (version >= 2) {
        if (rd32(sec + 28) != T3_V2_JDATA || rd32(sec + 32) != T3_V2_JSLOTS ||
            rd32(sec + 36) != T3_V2_GDT) return 0;
    }
    if (gc == 0 || ipg == 0 || ipg > T3_BPG) return 0;
    if ((uint64_t)total_blocks * T3_SPB > (uint64_t)sbi->vol.sector_count) return 0; // claims more volume than exists
    // The LAST group may be partial (see t3_group_span()), so the volume
    // has to contain every group's START, not every group's full extent.
    if (group0 + (uint64_t)(gc - 1) * bpg >= total_blocks) return 0;
    if (!set_geometry_version(sbi, version)) return 0;
    sbi->sb.version = (uint8_t)version;
    sbi->sb.flags = sec[5];
    sbi->sb.total_blocks = total_blocks;
    sbi->sb.bpg = bpg;
    sbi->sb.ipg = ipg;
    sbi->sb.gc = gc;
    return 1;
}

// Backup-region groups for a given group count -- keep in lockstep
// with tfs3_writer.py's t3_backup_groups().
int t3_backup_groups(uint32_t gc, uint32_t out[2]) {
    if (gc == 0) return 0;
    if (gc == 1) { out[0] = 0; return 1; }
    if (gc == 2) { out[0] = 1; return 1; }
    out[0] = 1; out[1] = gc - 1; return 2;
}

// Reads and validates the superblock, primary first, backups second
// (their positions derive from the VOLUME size alone -- the whole
// reason the descriptor table is a fixed 16 blocks). Returns 1
// mounted-ready (sbi->sb filled), 0 no valid superblock anywhere, -1
// primary unreadable (refuse -- never treat a failing disk as blank).
static int load_superblock(struct t3_state *sbi, int loud) {
    uint8_t sec[T3_SECTOR];
    int got = 0;
    for (int i = 0; i < T3_SB_READ_RETRIES && !got; i++) {
        got = t3_vol_read_sectors(sbi, T3_SB_BLOCK * T3_SPB, 1, sec);
    }
    if (!got) return -1;
    if (parse_superblock(sbi, sec)) { sbi->mounted_from_backup = 0; return 1; }

    // Primary readable but invalid -- try the backups, derived from
    // the volume size (see docs/tfs3-spec.md "Superblock backups").
    // Which version wrote the disk is exactly what the unreadable
    // sector would have said, so try each version's group0_start:
    // there are only two, both compile-time constants, and a backup
    // that parses under the wrong one is rejected by
    // parse_superblock()'s own group0 check.
    uint32_t vol_blocks = sbi->vol.sector_count / T3_SPB;
    for (uint32_t v = T3_VERSION; v >= T3_VERSION_MIN; v--) {
        uint32_t group0 = group0_for_version(v);
        if (vol_blocks <= group0) continue;
        // Ceiling, matching format(): a volume of one partial group has
        // one group, not zero.
        uint32_t gc = (vol_blocks - group0 + T3_BPG - 1) / T3_BPG;
        uint32_t groups[2];
        int n = t3_backup_groups(gc, groups);
        for (int i = 0; i < n; i++) {
            uint32_t gbase = group0 + groups[i] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t blk = gbase + gspan - 1;
            if (!t3_vol_read_sectors(sbi, blk * T3_SPB, 1, sec)) continue;
            if (parse_superblock(sbi, sec) && sbi->sb.version == v) {
                sbi->mounted_from_backup = 1;
                if (loud) {
                    klog_write(KLOG_ERR "tfs3: primary superblock invalid -- mounted from the backup in group ");
                    klog_write_dec(groups[i]);
                    klog_write(" (run `fsck repair` to restore the primary)\n");
                }
                return 1;
            }
        }
    }
    return 0;
}

// Serialize sbi->sb and write it to the primary AND every backup slot --
// fsck repair's restore path, and never called at mount.
// Forward-declared: used by the superblock writer just below, defined
// with the geometry helpers where it belongs. See t3_group_span() for what
// a partial group is and why the backup moved.
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g);

int t3_write_superblock_everywhere(struct t3_state *sbi) {
    k_memset(sbi->blk, 0, T3_BLOCK);
    sbi->blk[0] = 'T'; sbi->blk[1] = 'F'; sbi->blk[2] = 'S'; sbi->blk[3] = '3';
    sbi->blk[4] = sbi->sb.version; sbi->blk[5] = sbi->sb.flags;
    wr32(sbi->blk + 8, sbi->sb.total_blocks);
    wr32(sbi->blk + 12, sbi->sb.bpg);
    wr32(sbi->blk + 16, sbi->sb.ipg);
    wr32(sbi->blk + 20, sbi->sb.gc);
    wr32(sbi->blk + 24, sbi->group0);
    if (sbi->sb.version >= 2) {
        wr32(sbi->blk + 28, sbi->jdata_block);
        wr32(sbi->blk + 32, sbi->jslots);
        wr32(sbi->blk + 36, sbi->gdt_block);
    }
    wr32(sbi->blk + 44, k_fnv1a(sbi->blk, 44));
    int ok = t3_write_block(sbi, T3_SB_BLOCK, sbi->blk);
    uint32_t groups[2];
    int n = t3_backup_groups(sbi->sb.gc, groups);
    for (int i = 0; i < n; i++) {
        uint32_t blk = group_backup_block(sbi, groups[i]);
        if (blk && !t3_write_block(sbi, blk, sbi->blk)) ok = 0;
    }
    return ok;
}

// Declared here because persist_superblock() above needs it and the
// geometry helpers below own it -- see t3_group_span().
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g);

// ---- group / inode geometry ----------------------------------------------

uint32_t t3_group_base(struct t3_state *sbi, uint32_t g) { return sbi->group0 + g * T3_BPG; }

// How many blocks group `g` ACTUALLY has.
//
// Every group is T3_BPG except possibly the LAST, which is short when
// the volume does not divide evenly into groups -- exactly what ext2/3/4
// allow, and for the same reason: without it the smallest filesystem
// that can exist is one whole group (128 MiB at a 4 KiB block), which
// made a 4 MiB live image impossible and the ISO carrying it ten times
// larger than its contents.
//
// A partial group is otherwise an ORDINARY group: same bitmap, same
// metadata layout, same arithmetic. The only difference is where it
// ends, which is why this is the single place that answers it -- every
// caller that used T3_BPG as "the size of a group" now asks here, and a
// caller that means "the STRIDE between groups" still says T3_BPG.
uint32_t t3_group_span(struct t3_state *sbi, uint32_t g) {
    uint32_t base = t3_group_base(sbi, g);
    if (base >= sbi->sb.total_blocks) return 0;
    uint32_t rest = sbi->sb.total_blocks - base;
    return rest < T3_BPG ? rest : T3_BPG;
}

// Where group g's superblock/GDT backup lives: its LAST block, which for
// a partial group is the last block of the volume rather than a block
// past the end of it.
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g) {
    uint32_t span = t3_group_span(sbi, g);
    return span ? t3_group_base(sbi, g) + span - 1 : 0;
}

static uint32_t cksum_table_blocks(struct t3_state *sbi) {
    // Feature bit 0: per-group data-block checksum table (format-time
    // choice; algorithm deferred to Milestone 16). We don't verify it
    // yet, but the geometry must account for the region either way.
    return (sbi->sb.flags & 1u) ? (T3_BPG * 4u + T3_BLOCK - 1) / T3_BLOCK : 0;
}

static void derive_geometry(struct t3_state *sbi) {
    sbi->itb = sbi->sb.ipg * T3_INODE_SIZE / T3_BLOCK;
    sbi->meta_off = 2 + cksum_table_blocks(sbi) + sbi->itb;
}

// Sector (volume-relative LBA) holding inode `ino`, plus its offset
// within that sector.
int t3_inode_pos(struct t3_state *sbi, uint64_t ino, uint32_t *out_lba, uint32_t *out_off) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t idx = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return 0;
    uint32_t table = t3_group_base(sbi, g) + 2 + cksum_table_blocks(sbi);
    *out_lba = table * T3_SPB + idx / T3_INODES_PER_SECTOR;
    *out_off = (idx % T3_INODES_PER_SECTOR) * T3_INODE_SIZE;
    return 1;
}


static int read_inode_uncached(struct t3_state *sbi, uint64_t ino, struct t3_inode *out);

void t3_icache_drop(struct t3_state *sbi) {
    for (int i = 0; i < T3_ICACHE; i++) sbi->icache[i].ino = 0;
    sbi->icache_next = 0;
}

void t3_icache_forget(struct t3_state *sbi, uint64_t ino) {
    for (int i = 0; i < T3_ICACHE; i++)
        if (sbi->icache[i].ino == ino) sbi->icache[i].ino = 0;
}

int t3_read_inode(struct t3_state *sbi, uint64_t ino, struct t3_inode *out) {
    for (int i = 0; ino && i < T3_ICACHE; i++) {
        if (sbi->icache[i].ino == ino) {
            *out = sbi->icache[i].node;
            return 1;
        }
    }
    int ok = read_inode_uncached(sbi, ino, out);
    if (ok && ino && out->type == T3_TYPE_DIR) {
        int s = sbi->icache_next;
        sbi->icache_next = (sbi->icache_next + 1) % T3_ICACHE;
        sbi->icache[s].ino = ino;
        sbi->icache[s].node = *out;
    }
    return ok;
}

static int read_inode_uncached(struct t3_state *sbi, uint64_t ino, struct t3_inode *out) {
    uint32_t lba, off;
    if (!t3_inode_pos(sbi, ino, &lba, &off)) return 0;
    uint8_t sec[T3_SECTOR];
    if (!t3_vol_read_sectors(sbi, lba, 1, sec)) return 0;
    const uint8_t *p = sec + off;
    // Checksum covers bytes 0-87 and 92-127 (everything but itself).
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    if (k_fnv1a(chk, sizeof(chk)) != rd32(p + 88)) return 0;
    out->type = p[0];
    out->links = rd16(p + 2);
    out->size = rd64(p + 4);
    if (out->size > T3_MAX_FILE_SIZE) return 0; // corrupt: past what the format addresses
    out->created = rd64(p + 12);
    out->modified = rd64(p + 20);
    for (int i = 0; i < 15; i++) out->ptrs[i] = rd32(p + 28 + i * 4);
    out->mode = rd16(p + 92);
    return 1;
}

// ---- block map (read side: full direct/single/double/triple walk) -------

// Data-block number for file-block index `idx`, through *out_blk: 0
// there means a HOLE. The RETURN value is the third outcome -- 0 when
// the walk could not be completed at all, i.e. a pointer-table read
// that failed.
//
// THREE OUTCOMES, NOT TWO, and that is the whole point of the shape.
// Answering 0 for a failed table read as well as for a hole made
// read_range_impl() supply zeros for an I/O error and report the read
// as a success: fabricated data delivered as fact. A caller that
// cannot tell them apart cannot report the difference either.
//
// LEVELS ARE NUMBERED FROM THE LEAF (0 = the table holding data-block
// numbers), so a single- and a triple-indirect walk agree about which
// slot a leaf goes in. Numbering from the top instead would put the
// leaf at a different level per depth and evict it on every step.
#define T3_RC_LEAF 0
#define T3_RC_MID  1
#define T3_RC_TOP  2

// A zero table pointer is a hole in the chain, not a failure -- so it
// has to be tested BEFORE rcache_get(), which answers NULL to both.
#define T3_WALK(level, tbl, dst)                                               \
    do {                                                                       \
        if (!(tbl)) return 1;               /* hole: *out_blk stays 0 */       \
        (dst) = rcache_get(sbi, (level), (tbl));                               \
        if (!(dst)) return 0;               /* the table could not be read */  \
    } while (0)

int t3_block_for_index(struct t3_state *sbi, const struct t3_inode *node, uint32_t idx,
                           uint32_t *out_blk) {
    *out_blk = 0;
    // Past what the format addresses. A hole rather than an error:
    // every write door refuses such an offset, so no live inode can
    // name one, and the read path clamps to the size regardless.
    if ((uint64_t)idx >= T3_MAX_FILE_BLOCKS) return 1;
    if (idx < 12) { *out_blk = node->ptrs[idx]; return 1; }
    idx -= 12;
    const uint8_t *leaf, *mid_tbl, *top;
    if (idx < T3_PTRS_PER_BLOCK) {
        T3_WALK(T3_RC_LEAF, node->ptrs[12], leaf);
        *out_blk = rd32(leaf + idx * 4);
        return 1;
    }
    idx -= T3_PTRS_PER_BLOCK;
    if (idx < T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK) {
        T3_WALK(T3_RC_MID, node->ptrs[13], mid_tbl);
        uint32_t mid = rd32(mid_tbl + (idx / T3_PTRS_PER_BLOCK) * 4);
        T3_WALK(T3_RC_LEAF, mid, leaf);
        *out_blk = rd32(leaf + (idx % T3_PTRS_PER_BLOCK) * 4);
        return 1;
    }
    idx -= T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK;
    T3_WALK(T3_RC_TOP, node->ptrs[14], top);
    uint32_t hi = rd32(top + (idx / (T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK)) * 4);
    T3_WALK(T3_RC_MID, hi, mid_tbl);
    uint32_t rem = idx % (T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK);
    uint32_t mid = rd32(mid_tbl + (rem / T3_PTRS_PER_BLOCK) * 4);
    T3_WALK(T3_RC_LEAF, mid, leaf);
    *out_blk = rd32(leaf + (rem % T3_PTRS_PER_BLOCK) * 4);
    return 1;
}

// ---- path resolution ------------------------------------------------------

// Walk one directory's dirent chain looking for `name` (len bytes, no
// NUL requirement). Returns the child inode or 0. `dir_ino` is only
// for the name cache; pass 0 to bypass caching (e.g. during repair).
static uint64_t dir_lookup(struct t3_state *sbi, uint64_t dir_ino, const struct t3_inode *dir,
                           const char *name, uint32_t name_len) {
    if (dir_ino && name_len < T3_NCACHE_NAME) {
        for (int i = 0; i < T3_NCACHE; i++) {
            if (sbi->ncache[i].dir == dir_ino && sbi->ncache[i].len == name_len &&
                k_memcmp(sbi->ncache[i].name, name, name_len) == 0) {
                return sbi->ncache[i].ino;
            }
        }
    }
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!t3_block_for_index(sbi, dir, b, &blk) || !blk || !t3_read_block(sbi, blk, sbi->blk)) return 0;
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(sbi->blk + off);
            uint16_t rec_len = rd16(sbi->blk + off + 4);
            uint8_t nl = sbi->blk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break; // corrupt chain -- stop, don't loop
            if (e_ino != 0 && nl == name_len &&
                k_memcmp(sbi->blk + off + 7, name, name_len) == 0) {
                if (dir_ino && name_len < T3_NCACHE_NAME) {
                    int s = sbi->ncache_next;
                    sbi->ncache_next = (sbi->ncache_next + 1) % T3_NCACHE;
                    sbi->ncache[s].dir = dir_ino;
                    sbi->ncache[s].ino = e_ino;
                    sbi->ncache[s].len = (uint8_t)name_len;
                    k_memcpy(sbi->ncache[s].name, name, name_len);
                }
                return e_ino;
            }
            off += rec_len;
        }
    }
    return 0;
}

// Resolve a normalized absolute path to an inode number. Returns 1
// with *out_ino set, 0 if any component is missing. Purely lexical
// today -- symlink following (with its hop cap) is deliberately not
// implemented yet, see the design doc's Symlinks section; a symlink
// encountered mid-path simply fails the lookup.
static int resolve_walk(struct t3_state *sbi, const char *norm, uint64_t *out_ino) {
    uint64_t ino = T3_INO_ROOT;
    const char *p = norm;
    if (*p == '/') p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);
        if (len == 0 || len > T3_NAME_MAX) return 0;
        struct t3_inode dir;
        if (!t3_read_inode(sbi, ino, &dir) || dir.type != T3_TYPE_DIR) return 0;
        ino = dir_lookup(sbi, ino, &dir, start, len);
        if (!ino) return 0;
        if (*p == '/') p++;
    }
    *out_ino = ino;
    return 1;
}

// Backend-internal normalization, per fs_ops.h's contract. kpath's
// lexical normalize is exactly this job.
// **EVERY PATH BUFFER IS PER MOUNT, ONE PER FUNCTION** (t3_state.pb):
// one call runs per volume at a time (the mount's lock), which is what
// lets them outlive the call. Never a shared stack with a depth
// counter: these calls nest (tfs3_is_dir -> lookup -> normalize) and
// none recurses into itself, so separate buffers need no push/pop and
// cannot leak a slot down an early return.

int t3_normalize(struct t3_state *sbi, const char *path, char *out /* T3_PATH_BUF */) {
    if (!path) return 0;
    struct kpath_scratch sc = { sbi->norm_scratch, sizeof sbi->norm_scratch };
    return k_path_normalize(path, out, T3_PATH_BUF, &sc) == 0 ? 0 : 1;
}

// t3_resolve() + t3_normalize() in one, the common op prologue.
// WHAT PATH RESOLUTION COSTS, counted rather than reasoned about.
//
// Every fs_*(path, ...) call resolves from the root: one uncached inode
// read per component, plus a directory scan the 16-entry `ncache` only
// sometimes absorbs. That is plainly a per-syscall cost in the code,
// and its SIZE was unknown -- an attempt to infer it by comparing path
// depths disagreed with itself, because block allocation layout varies
// between runs more than depth costs (docs/pagecache-design.md).
//
// So it is measured at the source: bracket the resolve with the block
// layer's own read counter and the delta is exactly the disk traffic
// this lookup caused, with nothing attributed by argument.
static uint64_t g_lookup_calls;
static uint64_t g_lookup_reads;
static uint64_t g_lookup_ns;

void tfs3_lookup_stats(uint64_t *calls, uint64_t *reads, uint64_t *ns) {
    if (calls) *calls = g_lookup_calls;
    if (reads) *reads = g_lookup_reads;
    if (ns)    *ns    = g_lookup_ns;
}

// A CACHED RESOLUTION, or 0. Only ever holds paths that RESOLVED: a
// negative result is deliberately not cached, because the thing that
// would make it wrong -- a create at that path -- is common, and one
// stale "no such file" is worse than every miss it would have saved.
static uint64_t lcache_get(struct t3_state *sbi, const char *norm) {
    for (int i = 0; i < T3_LCACHE; i++) {
        if (sbi->lcache[i].path[0] &&
            k_strcmp(sbi->lcache[i].path, norm) == 0) return sbi->lcache[i].ino;
    }
    return 0;
}

static void lcache_put(struct t3_state *sbi, const char *norm, uint64_t ino) {
    if (k_strlen(norm) >= T3_LCACHE_PATH) return;  // longer than the ABI allows
    for (int i = 0; i < T3_LCACHE; i++) {
        if (sbi->lcache[i].path[0] && k_strcmp(sbi->lcache[i].path, norm) == 0) {
            sbi->lcache[i].ino = ino;
            return;
        }
    }
    int s = sbi->lcache_next;
    sbi->lcache_next = (sbi->lcache_next + 1) % T3_LCACHE;
    k_strlcpy(sbi->lcache[s].path, norm, T3_LCACHE_PATH);
    sbi->lcache[s].ino = ino;
}

// t3_resolve(), THROUGH THE PATH CACHE. The cache sits here rather than in
// lookup() because every door into the filesystem walks a path and only
// the READ path went through lookup(): a write syscall re-walked from
// the root, reading a directory inode per component, every time.
// Measured at 34% of random 4 KiB write throughput on a
// three-component path, and sequential READ was already insensitive to
// path depth -- which is what named the asymmetry.
//
// Only resolutions that SUCCEEDED are cached (lcache_get), and
// t3_ncache_flush() clears it on exactly the operations that can change
// which inode a path names.
int t3_resolve(struct t3_state *sbi, const char *norm, uint64_t *out_ino) {
    uint64_t ino = lcache_get(sbi, norm);
    if (ino) { *out_ino = ino; return 1; }
    if (!resolve_walk(sbi, norm, &ino)) return 0;
    lcache_put(sbi, norm, ino);
    *out_ino = ino;
    return 1;
}

// ---- inode locks (see t3_state.holds) ------------------------------------

// Is `ino` held by ANOTHER op in a way that excludes this request?
static int t3_conflict(struct t3_state *sbi, uint64_t ino, int excl, int me) {
    for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++) {
        if (!sbi->holds[i].used || sbi->holds[i].ino != ino || sbi->holds[i].pid == me) continue;
        if (excl || sbi->holds[i].excl) return 1;
    }
    return 0;
}

// Release every lock `me` holds at `depth` or deeper -- one op's, and the
// ops nested inside it -- and wake anyone waiting on any of them.
static void t3_release(struct t3_state *sbi, int me, int depth) {
    int any = 0;
    for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++) {
        if (sbi->holds[i].used && sbi->holds[i].pid == me && sbi->holds[i].depth >= depth) {
            sbi->holds[i].used = 0;
            any = 1;
        }
    }
    if (any) {
        scheduler_wake(&sbi->ilock_chan, 0);
        mount_locks_released();
    }
}

// Take `ino` for this op, shared or exclusive. Returns nonzero when held
// (or when it need not be), 0 when the op must RETURN AT ONCE -- touching
// nothing -- because:
//   * it was busy: this op's locks were released, it waited with the
//     volume lock dropped, and FS_OP will run it again from the top; or
//   * the mount went away while it waited (sbi is freed); or
//   * it was busy and this op cannot wait (nested, or under exclusion)
//     and wanted it EXCLUSIVE -- the op fails. A SHARED request there
//     proceeds without the lock: readers already tolerate a concurrent
//     writer (read_range_impl()'s free_gen), and a nested read is an
//     fs_list() callback's stat.
// So every call site is `if (!t3_lock(...)) return <failure>;` BEFORE it
// has changed anything.
int t3_lock(struct t3_state *sbi, uint64_t ino, int excl) {
    int depth = mount_op_depth(sbi);
    if (depth <= 0) return 1;      // a scratch state: nothing else can reach it
    int me = scheduler_current_pid();
    if (!t3_conflict(sbi, ino, excl, me)) {
        for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++) {
            if (sbi->holds[i].used) continue;
            sbi->holds[i].ino = ino;
            sbi->holds[i].pid = me;
            sbi->holds[i].depth = depth;
            sbi->holds[i].excl = (uint8_t)(excl != 0);
            sbi->holds[i].used = 1;
            return 1;
        }
        // A FULL TABLE is waited out like a busy lock: 64 locks held at
        // once means that many ops mid-flight, and one will finish.
    }
    if (!mount_can_wait(sbi)) return excl ? 0 : 1;
    t3_release(sbi, me, depth);
    if (mount_wait(sbi, &sbi->ilock_chan) > 0) mount_op_restart(sbi);
    return 0;
}

// TEST SEAM: how `ino` is held right now, by anyone -- 2 exclusive, 1
// shared, 0 not at all. tfs3_test.c asks it from inside a device
// callback, i.e. in the middle of an op's unlocked gap.
int tfs3_test_lock_mode(void *st, uint64_t ino) {
    struct t3_state *sbi = st;
    int mode = 0;
    for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++) {
        if (!sbi->holds[i].used || sbi->holds[i].ino != ino) continue;
        int m = sbi->holds[i].excl ? 2 : 1;
        if (m > mode) mode = m;
    }
    return mode;
}

static void tfs3_op_end(void *st) {
    struct t3_state *sbi = st;
    t3_release(sbi, scheduler_current_pid(), mount_op_depth(sbi));
}

// Held by OTHER ops -- what fs_exclusive_begin() waits out. Its own are
// not in its way (a nested exclusion inside an op holding some).
static int tfs3_locks_held(void *st) {
    struct t3_state *sbi = st;
    int me = scheduler_current_pid(), n = 0;
    for (unsigned i = 0; i < sizeof sbi->holds / sizeof sbi->holds[0]; i++)
        if (sbi->holds[i].used && sbi->holds[i].pid != me) n++;
    return n;
}

static int lookup(struct t3_state *sbi, const char *path, uint64_t *out_ino, struct t3_inode *out_node) {
    char *const norm = sbi->pb.lookup_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;

    uint64_t reads0 = 0;
    blk_stat_get(BLK_STAT_READ, &reads0, NULL, NULL, NULL);
    uint64_t t0 = clocksource_now_ns();

    uint64_t ino = 0;
    int ok = t3_resolve(sbi, norm, &ino);
    if (ok) {
        if (out_ino) *out_ino = ino;
        // STILL READ, never cached. The inode's CONTENTS change on
        // every write -- size, mtime, block pointers -- so caching the
        // struct would need invalidating on the hot path rather than on
        // the rare one. Only the path -> number mapping is cached, and
        // that changes solely when the namespace does.
        if (out_node) ok = t3_read_inode(sbi, ino, out_node);
    }

    uint64_t reads1 = 0;
    blk_stat_get(BLK_STAT_READ, &reads1, NULL, NULL, NULL);
    g_lookup_calls++;
    g_lookup_reads += reads1 - reads0;
    g_lookup_ns += clocksource_now_ns() - t0;
    return ok;
}

// ---- probe / format / init -------------------------------------------------

// A PROBE MUST NOT DISTURB A MOUNT, and this one used to. fs_ops.h has
// always said "detection only, no side effects beyond the read", and
// that was true in effect while probing only ever happened BEFORE
// anything was mounted -- set_flat_volume() writes sbi->vol, which is the
// mounted volume.
//
// Real mount points made it false the same day: mounting /boot probes
// every backend against the ESP, so a TFS3 already serving `/` had its
// volume repointed at partition 2 and the root went silently empty --
// `df` still reported the right numbers (they come from the cached
// superblock) while every path lookup failed. Saved and restored, which
// is cheaper than a second superblock reader and keeps the one that is
// tested.
static int tfs3_probe(void *st, const struct block_device *dev) {
    struct t3_state *sbi = st;
    if (!dev) return 0;
    struct t3_vol saved = sbi->vol;
    set_flat_volume(sbi, dev);
    int r = load_superblock(sbi, 0);
    if (sbi->mounted) sbi->vol = saved;
    return r;
}

// Erase every location the probe recognizes: the primary superblock
// sector AND both backups (positions derive from the volume size, the
// same way load_superblock()'s fallback finds them). See fs_ops.h's
// wipe contract for the mounted-a-corpse story that made this an op.
static int tfs3_wipe_inner(void *st, const struct block_device *dev) {
    struct t3_state *sbi = st;
    if (!dev) return 1;
    set_flat_volume(sbi, dev);
    uint8_t zero[T3_SECTOR];
    k_memset(zero, 0, sizeof(zero));
    int ok = t3_vol_write_sectors(sbi, T3_SB_BLOCK * T3_SPB, 1, zero);
    uint32_t vol_blocks = sbi->vol.sector_count / T3_SPB;
    // EVERY version's backup positions, not just the mounted one's --
    // the disk being wiped may have been written by either, and this
    // runs before (or instead of) a mount, so there is nothing to ask.
    // Leaving one version's backups behind is exactly the seance the
    // wipefs rule exists to prevent.
    for (uint32_t v = T3_VERSION_MIN; v <= T3_VERSION; v++) {
        uint32_t group0 = group0_for_version(v);
        if (vol_blocks <= group0) continue;
        // Ceiling, matching format(): a volume of one partial group has
        // one group, not zero.
        uint32_t gc = (vol_blocks - group0 + T3_BPG - 1) / T3_BPG;
        uint32_t groups[2];
        int n = t3_backup_groups(gc, groups);
        for (int i = 0; i < n; i++) {
            uint32_t gbase = group0 + groups[i] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t blk = gbase + gspan - 1;
            if (!t3_vol_write_sectors(sbi, blk * T3_SPB, 1, zero)) ok = 0;
        }
    }
    return ok;
}

// Kernel-side format, kept in lockstep with tfs3_writer.py's
// cmd_format() -- one description of the layout, two writers of it.
static int tfs3_format_inner(void *st, const struct block_device *dev) {
    struct t3_state *sbi = st;
    if (!dev) return 0;
    set_flat_volume(sbi, dev);

    // A fresh filesystem is always the newest version.
    set_geometry_version(sbi, T3_VERSION);
    sbi->sb.version = T3_VERSION;

    uint32_t vol_blocks = sbi->vol.sector_count / T3_SPB;
    // The floor is the METADATA, not a whole group. A group needs its
    // two bitmaps, its inode table and somewhere to put the root
    // directory; beyond that a PARTIAL last group is fine, exactly as
    // ext2/3/4 allow -- which is what lets a 16 MiB live image exist
    // instead of a 129 MiB one. (`meta` is computed just below, so this
    // checks the generous version of the same thing: one group's
    // metadata cannot exceed a few hundred blocks.)
    if (vol_blocks <= sbi->group0 + T3_MIN_GROUP_BLOCKS) {
        klog_write("tfs3: volume too small even for one partial group -- not formatting\n");
        return 0;
    }
    // CEILING: a volume that ends mid-group still has that group.
    uint32_t gc = (vol_blocks - sbi->group0 + T3_BPG - 1) / T3_BPG;
    if (gc > T3_GDT_BLOCKS * T3_BLOCK / 16) gc = T3_GDT_BLOCKS * T3_BLOCK / 16;
    uint32_t ipg = T3_BPG * T3_BLOCK / T3_BYTES_PER_INODE;
    if (ipg > T3_BPG) ipg = T3_BPG;
    ipg = (ipg / T3_INODES_PER_SECTOR / T3_SPB) * T3_INODES_PER_SECTOR * T3_SPB; // whole table blocks
    uint32_t itb = ipg * T3_INODE_SIZE / T3_BLOCK;
    uint32_t meta = 2 + itb; // no checksum-table feature at format time (flags = 0)

    // Publish the geometry BEFORE anything uses it. t3_group_span() and
    // everything built on it read sbi->sb, and during a format that still
    // held the PREVIOUS volume's numbers (or zeros on a fresh boot) --
    // so every group's span came out wrong and the format failed with
    // no clue as to why. The superblock image below is written from
    // these same values, so there is one source rather than two.
    sbi->sb.total_blocks = vol_blocks;
    sbi->sb.bpg = T3_BPG;
    sbi->sb.ipg = ipg;
    sbi->sb.gc = gc;

    uint32_t groups[2];
    int nb = t3_backup_groups(gc, groups);
    uint64_t now = 0;
    {
        struct rtc_time t;
        ktime_read(&t);
        now = cal_rtc_to_epoch(&t);
    }

    // Superblock image (one sector's worth, zero-padded to a block by
    // the caller writes below).
    uint8_t sb[T3_SECTOR];
    k_memset(sb, 0, sizeof(sb));
    sb[0] = 'T'; sb[1] = 'F'; sb[2] = 'S'; sb[3] = '3';
    sb[4] = T3_VERSION; sb[5] = 0;
    wr32(sb + 8, vol_blocks);
    wr32(sb + 12, T3_BPG);
    wr32(sb + 16, ipg);
    wr32(sb + 20, gc);
    wr32(sb + 24, sbi->group0);
    wr32(sb + 28, sbi->jdata_block);
    wr32(sb + 32, sbi->jslots);
    wr32(sb + 36, sbi->gdt_block);
    wr32(sb + 44, k_fnv1a(sb, 44));

    k_memset(sbi->blk, 0, T3_BLOCK);
    k_memcpy(sbi->blk, sb, T3_SECTOR);
    if (!t3_write_block(sbi, T3_SB_BLOCK, sbi->blk)) return 0;

    // The wipefs rule applied WITHIN this format: an older version's
    // backup superblocks sit where this version's layout never writes,
    // so leaving them lets a later reader whose primary is damaged
    // mount a corpse with the wrong geometry -- the same seance
    // fs_ops.h's wipe contract describes, one format version apart
    // instead of one filesystem apart.
    {
        uint8_t zero[T3_SECTOR];
        k_memset(zero, 0, sizeof(zero));
        for (uint32_t v = T3_VERSION_MIN; v < T3_VERSION; v++) {
            uint32_t old0 = group0_for_version(v);
            if (vol_blocks <= old0) continue;
            uint32_t old_gc = (vol_blocks - old0 + T3_BPG - 1) / T3_BPG;
            uint32_t old_groups[2];
            int on = t3_backup_groups(old_gc, old_groups);
            for (int i = 0; i < on; i++) {
                uint32_t gbase = old0 + old_groups[i] * T3_BPG;
                uint32_t gspan = vol_blocks - gbase;
                if (gspan > T3_BPG) gspan = T3_BPG;
                t3_vol_write_sectors(sbi, (gbase + gspan - 1) * T3_SPB, 1, zero);
            }
        }
    }

    // Empty journal header + zeroed image slots.
    k_memset(sbi->blk, 0, T3_BLOCK);
    sbi->blk[0] = 'J'; sbi->blk[1] = 'R'; sbi->blk[2] = 'N'; sbi->blk[3] = '3';
    wr32(sbi->blk + t3_jh_cksum_off(T3_VERSION), k_fnv1a(sbi->blk, t3_jh_cksum_off(T3_VERSION)));
    if (!t3_write_block(sbi, T3_JH_BLOCK, sbi->blk)) return 0;
    k_memset(sbi->blk, 0, T3_BLOCK);
    for (uint32_t i = 0; i < sbi->jslots; i++) {
        if (!t3_write_block(sbi, sbi->jdata_block + i, sbi->blk)) return 0;
    }

    // Group descriptor table: free-count caches, checksummed each.
    // Built one block at a time (a full table is 64 KiB, bigger than
    // any scratch this kernel keeps around).
    uint32_t root_block = sbi->group0 + meta; // first data block of group 0
    for (uint32_t tb = 0; tb < T3_GDT_BLOCKS; tb++) {
        k_memset(sbi->blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= gc) break;
            // The last group's span, not T3_BPG -- a partial group has
            // fewer free blocks and the GDT cache is what the allocator
            // trusts.
            uint32_t gbase = sbi->group0 + g * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t free_b = gspan > meta ? gspan - meta : 0;
            uint32_t free_i = ipg;
            for (int j = 0; j < nb; j++) {
                if (groups[j] == g) free_b -= T3_BACKUP_BLOCKS;
            }
            if (g == 0) { free_b -= 1; free_i -= 2; } // root dirent block; ino 0+1
            uint8_t *e = sbi->blk + i * 16;
            wr32(e, free_b);
            wr32(e + 4, free_i);
            wr32(e + 12, k_fnv1a(e, 12));
        }
        if (!t3_write_block(sbi, sbi->gdt_block + tb, sbi->blk)) return 0;
        // Backup GDT snapshots get the identical block.
        for (int j = 0; j < nb; j++) {
            uint32_t gbase = sbi->group0 + groups[j] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t tail = gbase + gspan - 1;
            if (!t3_write_block(sbi, tail - T3_GDT_BLOCKS + tb, sbi->blk)) return 0;
        }
    }

    // Per group: block bitmap, inode bitmap, zeroed inode table.
    for (uint32_t g = 0; g < gc; g++) {
        uint32_t base = t3_group_base(sbi, g);
        k_memset(sbi->blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < meta; i++) sbi->blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        uint32_t gspan = vol_blocks - base;
        if (gspan > T3_BPG) gspan = T3_BPG;

        int is_backup = 0;
        for (int j = 0; j < nb; j++) if (groups[j] == g) is_backup = 1;
        if (is_backup) {
            for (uint32_t i = gspan - T3_BACKUP_BLOCKS; i < gspan; i++)
                sbi->blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        }

        // Everything past the volume's end is marked USED, permanently.
        // That is what makes a partial group need no special case
        // anywhere else: the bitmap is still a full T3_BPG bits, the
        // allocator still reads it the same way, and the blocks that do
        // not exist are simply never free. Miss this and the allocator
        // hands out a block past the end of the device, which fails as a
        // refused write somewhere far away from the cause.
        for (uint32_t i = gspan; i < T3_BPG; i++)
            sbi->blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        if (g == 0) {
            uint32_t i = meta; // the root dirent block
            sbi->blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
        if (!t3_write_block(sbi, base, sbi->blk)) return 0;

        k_memset(sbi->blk, 0, T3_BLOCK);
        if (g == 0) sbi->blk[0] |= 0x03; // ino 0 (null) + ino 1 (root)
        if (!t3_write_block(sbi, base + 1, sbi->blk)) return 0;

        // Zero the inode table: a zero inode fails its checksum on
        // purpose, and stale-but-valid inodes from a previous TFS3
        // format must not survive into this one.
        k_memset(sbi->blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < itb; i++) {
            if (!t3_write_block(sbi, base + 2 + i, sbi->blk)) return 0;
        }
    }

    // Root inode (ino 1, group 0) + its dirent block (. and ..).
    {
        uint8_t sec[T3_SECTOR];
        uint32_t table_lba = (sbi->group0 + 2) * T3_SPB;
        if (!t3_vol_read_sectors(sbi, table_lba, 1, sec)) return 0;
        uint8_t *p = sec + T3_INO_ROOT * T3_INODE_SIZE;
        k_memset(p, 0, T3_INODE_SIZE);
        p[0] = T3_TYPE_DIR;
        wr16(p + 2, 2);
        wr64(p + 4, T3_BLOCK);
        wr64(p + 12, now);
        wr64(p + 20, now);
        wr32(p + 28, root_block);
        uint8_t chk[124];
        k_memcpy(chk, p, 88);
        k_memcpy(chk + 88, p + 92, 36);
        wr32(p + 88, k_fnv1a(chk, sizeof(chk)));
        if (!t3_vol_write_sectors(sbi, table_lba, 1, sec)) return 0;

        k_memset(sbi->blk, 0, T3_BLOCK);
        wr32(sbi->blk + 0, T3_INO_ROOT); wr16(sbi->blk + 4, 12); sbi->blk[6] = 1; sbi->blk[7] = '.';
        wr32(sbi->blk + 12, T3_INO_ROOT); wr16(sbi->blk + 16, (uint16_t)(T3_BLOCK - 12)); sbi->blk[18] = 2;
        sbi->blk[19] = '.'; sbi->blk[20] = '.';
        if (!t3_write_block(sbi, root_block, sbi->blk)) return 0;
    }

    // Backup superblock copies, byte-identical to the primary.
    k_memset(sbi->blk, 0, T3_BLOCK);
    k_memcpy(sbi->blk, sb, T3_SECTOR);
    for (int j = 0; j < nb; j++) {
        // The group's REAL last block. A partial last group ends before
        // sbi->group0 + (g+1)*T3_BPG, and writing the backup superblock
        // past the end of the volume failed the entire format with
        // nothing anywhere to say why.
        uint32_t gbase = sbi->group0 + groups[j] * T3_BPG;
        uint32_t gspan = vol_blocks - gbase;
        if (gspan > T3_BPG) gspan = T3_BPG;
        if (!t3_write_block(sbi, gbase + gspan - 1, sbi->blk)) return 0;
    }

    blkdev_flush(sbi->vol.dev); // one barrier so the whole format is durable before init() re-reads it
    klog_write("tfs3: formatted a fresh tfs3 filesystem (");
    klog_write_dec(gc); klog_write(" groups, ");
    klog_write_dec(ipg); klog_write(" inodes/group)\n");
    return 1;
}

static void unmount_state(struct t3_state *sbi) {
    // Anything still staged has to land before the state it describes
    // is freed -- after this the journal could not be replayed against
    // a mount that no longer exists.
    if (sbi->txn_deferred) {
        t3_txn_flush_deferred(sbi);
        // A flush that failed here has nowhere left to retry: the state
        // it belongs to is about to go.
        sbi->txn_deferred = 0;
    }
    if (sbi->gd) { kfree(sbi->gd); sbi->gd = 0; }
    if (sbi->bbm) { kfree(sbi->bbm); sbi->bbm = 0; }
    if (sbi->ibm) { kfree(sbi->ibm); sbi->ibm = 0; }
    if (sbi->rotor) { kfree(sbi->rotor); sbi->rotor = 0; }
    t3_ncache_flush(sbi);
    t3_icache_drop(sbi);
    t3_pcache_drop(sbi);
    t3_txn_reset(sbi);
}

static int tfs3_init(void *st, const struct block_device *dev, uint64_t size_bytes) {
    struct t3_state *sbi = st;
    // A volume's capacity is the volume's; only a backend that lives
    // in memory has a size to be told (fs_ops.h).
    (void)size_bytes;
    sbi->mounted = 0;
    unmount_state(sbi);
    if (!dev) {
        // tfs3 has no RAM-only mode of its own -- that is ramfs's job
        // now (kernel/fs/ramfs.c), and vfs.c's policy is what chooses
        // between us. Reaching here without a disk means something
        // asked anyway; say so and mount NOTHING.
        //
        // -1, not 0: 0 would mean "mounted, but not persistent", which
        // is exactly the fiction this return value was carrying before
        // ramfs existed -- vfs.c reported an active backend while
        // sbi->mounted stayed 0 and every fs_* call failed. See fs_ops.h.
        klog_write(KLOG_ERR "tfs3: no disk -- cannot mount\n");
        return -1;
    }
    set_flat_volume(sbi, dev);
    if (load_superblock(sbi, 1) != 1) {
        klog_write("tfs3: no valid superblock (primary or backup) -- not mounted\n");
        return -1;
    }
    if (sbi->sb.flags != 0) {
        // Feature bits this kernel doesn't implement yet (e.g. the
        // per-block checksum table's write half). Refusing beats
        // mounting read-write and silently rotting the feature's
        // state -- the capabilities-must-not-lie rule, applied to a
        // format.
        klog_write("tfs3: superblock declares feature bits this kernel doesn't support -- not mounted\n");
        return -1;
    }
    derive_geometry(sbi);
    // Before anything reads the structures a crash may have
    // half-written -- and a transaction it could not apply takes the
    // volume read-only rather than being mounted over.
    if (!t3_replay_journal(sbi))
        t3_vol_go_readonly(sbi, "a committed journal transaction could not be replayed");
    t3_icache_drop(sbi);   // replay rewrote inode tables behind any cache

    sbi->gd = kmalloc(sizeof(struct t3_gd) * sbi->sb.gc);
    sbi->bbm = kmalloc((size_t)sbi->sb.gc * T3_BLOCK);
    sbi->ibm = kmalloc((size_t)sbi->sb.gc * T3_BLOCK);
    sbi->rotor = kmalloc(sizeof(uint32_t) * sbi->sb.gc);
    if (!sbi->gd || !sbi->bbm || !sbi->ibm || !sbi->rotor) { unmount_state(sbi); return -1; }
    k_memset(sbi->bbm_dirty, 0, sizeof(sbi->bbm_dirty));
    k_memset(sbi->ibm_dirty, 0, sizeof(sbi->ibm_dirty));
    k_memset(sbi->gdt_dirty, 0, sizeof(sbi->gdt_dirty));
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        sbi->rotor[g] = sbi->meta_off;
        if (!t3_read_block(sbi, t3_group_base(sbi, g), sbi->bbm + (size_t)g * T3_BLOCK) ||
            !t3_read_block(sbi, t3_group_base(sbi, g) + 1, sbi->ibm + (size_t)g * T3_BLOCK)) {
            klog_write(KLOG_ERR "tfs3: bitmap read failed -- not mounted\n");
            unmount_state(sbi);
            return -1;
        }
    }
    uint32_t bad_gd = 0;
    for (uint32_t tb = 0; tb <= (sbi->sb.gc - 1) / (T3_BLOCK / 16); tb++) {
        if (!t3_read_block(sbi, sbi->gdt_block + tb, sbi->blk)) { kfree(sbi->gd); sbi->gd = 0; return -1; }
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= sbi->sb.gc) break;
            const uint8_t *e = sbi->blk + i * 16;
            if (k_fnv1a(e, 12) != rd32(e + 12)) {
                // A descriptor is a cache -- a bad one doesn't block
                // the mount, it blocks trusting the cache. Count it
                // as fully-used so nothing over-promises; fsck
                // (Stage D) recomputes and repairs. Log the first few
                // only -- a wiped-and-reused disk fails ALL of them,
                // and 71 identical lines helped nobody.
                static const uint32_t GD_LOG_CAP = 4;
                if (bad_gd < GD_LOG_CAP) {
                    klog_write(KLOG_ERR "tfs3: group descriptor "); klog_write_dec(g);
                    klog_write(" failed its checksum -- treating its free counts as 0 until fsck\n");
                } else if (bad_gd == GD_LOG_CAP) {
                    klog_write(KLOG_ERR "tfs3: ...more group descriptors failed -- run fsck\n");
                }
                bad_gd++;
                sbi->gd[g].free_blocks = 0;
                sbi->gd[g].free_inodes = 0;
            } else {
                sbi->gd[g].free_blocks = rd32(e);
                sbi->gd[g].free_inodes = rd32(e + 4);
            }
        }
    }

    // Sanity-check the root before declaring victory.
    struct t3_inode root;
    if (!t3_read_inode(sbi, T3_INO_ROOT, &root) || root.type != T3_TYPE_DIR) {
        klog_write(KLOG_ERR "tfs3: root inode invalid -- not mounted\n");
        unmount_state(sbi);
        return -1;
    }

    sbi->mounted = 1;
    klog_write("tfs3: mounted (v");
    klog_write_dec(sbi->sb.version); klog_write(", ");
    klog_write_dec(sbi->sb.gc); klog_write(" groups, ");
    klog_write_dec(sbi->sb.ipg); klog_write(" inodes/group, ");
    klog_write_dec(sbi->jslots); klog_write(" journal slots)\n");
    return 1;
}

// ---- read-side ops ---------------------------------------------------------

static uint32_t read_range_impl(struct t3_state *sbi, const struct t3_inode *node, uint64_t offset,
                                 void *buf, uint32_t len) {
    if (offset >= node->size) return 0;
    uint64_t avail = node->size - offset;
    if ((uint64_t)len > avail) len = (uint32_t)avail;

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;
        uint32_t blk;
        // A TABLE READ THAT FAILED IS NOT A HOLE. Coming back short is
        // the only honest answer; zeros here would be invented bytes
        // reported as a successful read.
        if (!t3_block_for_index(sbi, node, bi, &blk)) break;
        if (!blk) {
            // A hole reads as zeros (the format allows them even
            // though the Stage C writer never creates one).
            k_memset(dst + total, 0, chunk);
        } else if (chunk == T3_BLOCK) {
            // Run coalescing, the mirror of the write path's: gather
            // the contiguous on-disk run of whole blocks this read
            // covers and issue it as ONE transfer straight into the
            // caller's buffer -- no bounce through blk, no per-block
            // command. The write side has done this since TFS2; the
            // read side issued a command per 4 KiB until now.
            uint32_t run = 1;
            uint32_t want = (len - total) / T3_BLOCK;
            uint32_t cap = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                // A HOLE ENDS THE RUN, and so does any block that is not
                // the next one: both would make this transfer read
                // sectors the caller did not ask for.
                uint32_t nxt;
                if (!t3_block_for_index(sbi, node, bi + run, &nxt) || nxt != blk + run) break;
                run++;
            }
            // THE ONE PLACE THE LOCK MAY DROP: a whole-block run straight
            // into the caller's buffer (vol_read_run()). After it, a freed
            // block anywhere on the volume means this inode copy may no
            // longer describe the file -- stop SHORT and let the caller's
            // next call look it up again. The run itself is good: frees
            // wait for it (t3_free_block_bit()).
            uint64_t freed = sbi->free_gen;
            int r = vol_read_run(sbi, blk * T3_SPB, (int)(run * T3_SPB), dst + total);
            if (r < 0) return total;       // unmounted meanwhile: sbi is gone
            if (!r) break;
            total += run * T3_BLOCK;
            if (sbi->free_gen != freed) break;
            continue;
        } else {
            if (!t3_read_block(sbi, blk, sbi->blk)) break;
            k_memcpy(dst + total, sbi->blk + within, chunk);
        }
        total += chunk;
    }
    return total;
}

static uint64_t tfs3_size(void *st, const char *path) {
    struct t3_state *sbi = st;
    struct t3_inode node;
    uint64_t ino;
    if (!lookup(sbi, path, &ino, &node) || node.type != T3_TYPE_FILE) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    return node.size;
}

static uint32_t tfs3_read_range(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    struct t3_inode node;
    uint64_t ino;
    if (!lookup(sbi, path, &ino, &node) || node.type != T3_TYPE_FILE) return 0;
    // SHARED, and held across read_range_impl()'s unlocked gap: a writer
    // of this file waits for the read, so a read sees whole writes.
    if (!t3_lock(sbi, ino, 0)) return 0;
    return read_range_impl(sbi, &node, offset, buf, len);
}

struct t3_read_step {
    uint64_t ino;              // locked shared per step -- see t3_lock()
    struct t3_inode node;
    uint8_t *dst;
    uint64_t offset;
    uint32_t len, total;
};

static void *tfs3_read_range_begin(void *st, const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    struct t3_inode node;
    uint64_t ino;
    if (!lookup(sbi, path, &ino, &node) || node.type != T3_TYPE_FILE) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    if (offset >= node.size) len = 0;
    else {
        uint64_t avail = node.size - offset;
        if ((uint64_t)len > avail) len = (uint32_t)avail;
    }
    struct t3_read_step *step = kmalloc(sizeof(*step));
    if (!step) return 0;
    step->ino = ino;
    step->node = node;
    step->dst = (uint8_t *)buf;
    step->offset = offset;
    step->len = len;
    step->total = 0;
    return step;
}

static int tfs3_read_range_step(void *st, void *handle, uint32_t *out_total) {
    struct t3_state *sbi = st;
    struct t3_read_step *step = (struct t3_read_step *)handle;
    // Per STEP, never across steps: a lock held between two syscalls
    // would outlive a process that died mid-stream.
    if (!t3_lock(sbi, step->ino, 0)) return 0 /* FS_STEP_PENDING: run again */;
    if (step->total < step->len) {
        uint32_t got = read_range_impl(sbi, &step->node, step->offset + step->total,
                                       step->dst + step->total,
                                       // one block per step, same
                                       // pacing contract as TFS2
                                       T3_BLOCK - (uint32_t)((step->offset + step->total) % T3_BLOCK) <= step->len - step->total
                                           ? T3_BLOCK - (uint32_t)((step->offset + step->total) % T3_BLOCK)
                                           : step->len - step->total);
        if (got == 0) {
            if (out_total) *out_total = step->total;
            kfree(step);
            return 2 /* FS_STEP_FAILED */;
        }
        step->total += got;
        if (step->total < step->len) {
            if (out_total) *out_total = step->total;
            return 0 /* FS_STEP_PENDING */;
        }
    }
    if (out_total) *out_total = step->total;
    kfree(step);
    return 1 /* FS_STEP_DONE */;
}

static int tfs3_is_dir(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_is_dir_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    struct t3_inode node;
    uint64_t ino;
    if (!lookup(sbi, path, &ino, &node)) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    return node.type == T3_TYPE_DIR;
}

static int tfs3_exists(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_exists_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    uint64_t ino;
    return t3_resolve(sbi, norm, &ino) ? 1 : 0;
}

static void tfs3_list(void *st, const char *dir_path, fs_list_cb cb, void *ctx) {
    struct t3_state *sbi = st;
    struct t3_inode dir;
    char *const norm = sbi->pb.tfs3_list_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, dir_path, norm)) return;
    uint64_t ino = T3_INO_ROOT;
    if (k_strcmp(norm, "/") != 0 && !t3_resolve(sbi, norm, &ino)) return;
    if (!t3_lock(sbi, ino, 0)) return;
    if (!t3_read_inode(sbi, ino, &dir) || dir.type != T3_TYPE_DIR) return;

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        // Copy the dirent block out of blk before per-child inode
        // reads reuse the scratch.
        uint8_t *const dirblk = sbi->dirblk;
        if (!t3_block_for_index(sbi, &dir, b, &blk) || !blk || !t3_read_block(sbi, blk, dirblk)) return;
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(dirblk + off);
            uint16_t rec_len = rd16(dirblk + off + 4);
            uint8_t nl = dirblk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            if (e_ino != 0 && nl > 0) {
                char name[T3_NAME_MAX + 1];
                k_memcpy(name, dirblk + off + 7, nl);
                name[nl] = '\0';
                // `.`/`..` are real entries on disk but not "direct
                // children" in fs_list()'s contract -- skip them so
                // ls output matches the other backend.
                if (!(name[0] == '.' && (nl == 1 || (nl == 2 && name[1] == '.')))) {
                    struct t3_inode child;
                    if (t3_read_inode(sbi, e_ino, &child)) {
                        uint32_t sz = child.type == T3_TYPE_DIR ? 0
                                     : (child.size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)child.size);
                        cb(ctx, name, sz, child.type == T3_TYPE_DIR);
                    }
                }
            }
            off += rec_len;
        }
    }
}

// Permission bits only -- the VFS has already masked the type off.
static int tfs3_chmod(void *st, const char *path, uint16_t mode) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_chmod_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return -ENOENT;
    if (k_strcmp(norm, "/") == 0) return -ENOENT;   // root has no entry
    uint64_t ino;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) return -ENOENT;
    if (!t3_lock(sbi, ino, 1)) return -EIO;   // or run again -- see t3_lock()
    if (!t3_read_inode(sbi, ino, &node)) return -ENOENT;
    if ((node.mode & 07777) == (mode & 07777)) return 0;  // already so
    node.mode = (uint16_t)(mode & 07777);
    // THROUGH THE JOURNAL, like every other inode change: a mode is a
    // metadata write and gets the same crash-safety as a rename.
    if (!t3_txn_begin(sbi, 1)) return -EIO;
    if (!t3_txn_stage_inode(sbi, ino, &node)) { t3_txn_reset(sbi); return -EIO; }
    return t3_txn_commit(sbi) ? 0 : -EIO;
}

static int tfs3_stat(void *st, const char *path, struct fs_stat_info *out) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_stat_norm; // per-function, see t3_normalize()
    if (!sbi->mounted || !t3_normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0; // root has no entry
    uint64_t ino;
    struct t3_inode node;
    if (!t3_resolve(sbi, norm, &ino)) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    if (!t3_read_inode(sbi, ino, &node)) return 0;
    if (out) {
        out->ino = ino;             // a real inode number -- FS_CAP_INODES
        out->created = node.created; // stored as epoch natively -- FS_CAP_EPOCH_TIME
        out->modified = node.modified;
        // A zero on disk is an inode written before the field existed,
        // not a file nobody may touch -- see struct t3_inode.
        out->mode = node.mode ? node.mode : (uint16_t)T3_MODE_DEFAULT(node.type);
        out->nlink = node.links;
        out->is_dir = node.type == T3_TYPE_DIR;
        out->size = node.type == T3_TYPE_FILE ? node.size : 0;
    }
    return 1;
}

static int tfs3_disk_usage(void *st, uint64_t *out_used, uint64_t *out_total) {
    struct t3_state *sbi = st;
    if (!sbi->mounted) {
        if (out_used) *out_used = 0;
        if (out_total) *out_total = 0;
        return 1;
    }
    // Same contract as fs.h: usable data space only, metadata excluded.
    uint32_t groups[2];
    int nb = t3_backup_groups(sbi->sb.gc, groups);
    uint64_t total_data = 0, free_data = 0;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        // The group's REAL extent -- the last one may be partial, and
        // using T3_BPG here made `df` report a 16 MiB volume as 127 MB.
        // A size that lies is worse than no size at all.
        uint32_t span = t3_group_span(sbi, g);
        uint32_t data = span > sbi->meta_off ? span - sbi->meta_off : 0;
        for (int j = 0; j < nb; j++) if (groups[j] == g) data -= T3_BACKUP_BLOCKS;
        total_data += data;
        free_data += sbi->gd[g].free_blocks;
    }
    if (out_total) *out_total = total_data * T3_BLOCK;
    if (out_used) *out_used = (total_data - free_data) * T3_BLOCK;
    return 1;
}

// ---- per-mount state: allocate, activate, free ----------------------
//
// format() and wipe() are handed a device and repoint this state at it.
// That used to be catastrophic -- `mkfs` on a second disk made the
// mounted root read the wrong volume, twice taking /bin with it -- and
// is now merely local: the mount table hands those two a SCRATCH state,
// so there is nothing of anybody's to restore.
static void *tfs3_state_alloc(void) {
    struct t3_state *st = kmalloc(sizeof *st);
    if (!st) return NULL;
    k_memset(st, 0, sizeof *st);
    // v1's geometry, which is what the declarations used to carry: a
    // mount overwrites all four in set_geometry_version(), and a probe
    // reads them before it knows the version.
    st->group0 = T3_V1_GROUP0;
    st->gdt_block = T3_V1_GDT;
    st->jdata_block = T3_V1_JDATA;
    st->jslots = T3_V1_JSLOTS;
    return st;
}

static void tfs3_state_free(void *st) {
    struct t3_state *sbi = st;
    if (!sbi) return;
    unmount_state(sbi);          // the caches hanging off it, and the read buffer
    // AN OP PARKED ON AN INODE LOCK must hear the mount go, or it sleeps
    // on this freed address forever; it wakes to a changed generation.
    scheduler_wake(&sbi->ilock_chan, 0);
    kfree(sbi);
}

// Land a deferred transaction. Ordered BEFORE the device flush by
// fs_sync(), because committing after the barrier would leave the very
// thing being made durable behind it.
static int tfs3_sync(void *st) {
    struct t3_state *sbi = st;
    if (!sbi) return 1;
    return t3_txn_flush_deferred(sbi);
}

// BOUNDS HOW LONG A DEFERRED COMMIT CAN SIT. Without this, `batched`
// lands a transaction only when another one opens, a second mount
// becomes active, `sync` runs, or the volume unmounts -- so a machine
// that writes a file and is then left alone could hold that inode
// update indefinitely. The dirty-slot ceiling bounds how MUCH
// accumulates; this bounds how long, which is the same pair
// `atac_idle()` already implements for the sector cache.
// COUNTED, because a test cannot otherwise tell an idle commit from
// somebody else's. Anything that opens a transaction commits the
// deferred one on its way past, and the desktop is always writing --
// so "a commit happened after waiting" is true whether or not this
// function does anything at all. That is exactly how the first version
// of the test for it passed with the idle path disabled.
static uint64_t g_idle_commits;

uint64_t tfs3_idle_commits(void) { return g_idle_commits; }

static void tfs3_idle(void *st) {
    struct t3_state *sbi = st;
    if (!sbi->txn_deferred) return;
    uint32_t quiet = storage_writeback_ticks();
    if (coarse_ticks() - sbi->txn_staged_tick < quiet) {
        clockevent_idle_wake_at_tick(sbi->txn_staged_tick + quiet);
        return;
    }
    g_idle_commits++;
    t3_txn_flush_deferred(sbi);
}

const struct fs_ops tfs3_ops = {
    .name = "tfs3",
    // Format truths, not implementation status: inodes and epoch
    // timestamps are live already; hardlinks/symlinks are carried by
    // the format (link counts, type 2) with their ops still to come --
    // see fs.h's FS_CAP_* comment on exactly this distinction.
    .chmod = tfs3_chmod,
    .caps = FS_CAP_INODES | FS_CAP_HARDLINKS | FS_CAP_SYMLINKS | FS_CAP_EPOCH_TIME |
            FS_CAP_MODE | FS_CAP_REPLACE,
    .volume_relative = 1, // all I/O is volume-relative through vol_read/vol_write -- mountable from a partition
    // MORE THAN ONCE, because every per-volume field is in struct
    // t3_state and every op is handed its own. Two is what an
    // installer needs (its own root, plus the target it is writing);
    // the ceiling is the mount table's, not this backend's.
    .max_mounts = MOUNT_MAX,
    .state_alloc = tfs3_state_alloc,
    .state_free = tfs3_state_free,
    .op_end = tfs3_op_end,
    .locks_held = tfs3_locks_held,
    .probe = tfs3_probe,
    .wipe = tfs3_wipe_inner,
    .format = tfs3_format_inner,
    .init = tfs3_init,
    .touch = tfs3_touch,
    .write = tfs3_write,
    .mkdir = tfs3_mkdir,
    .del = tfs3_delete,
    .size = tfs3_size,
    .read_range = tfs3_read_range,
    .write_range = tfs3_write_range,
    .write_range_begin = tfs3_write_range_begin,
    .write_range_step = tfs3_write_range_step,
    .read_range_begin = tfs3_read_range_begin,
    .read_range_step = tfs3_read_range_step,
    .rename = tfs3_rename,
    .truncate = tfs3_truncate,
    .is_dir = tfs3_is_dir,
    .exists = tfs3_exists,
    .list = tfs3_list,
    .stat = tfs3_stat,
    .disk_usage = tfs3_disk_usage,
    .sync = tfs3_sync,
    .idle = tfs3_idle,
    .check = tfs3_check,
    .link = tfs3_link, // optional op, paired with FS_CAP_HARDLINKS above
    .rename_replace = tfs3_rename_replace, // FS_CAP_REPLACE
};

// ---- the deferred allocation flush ----------------------------------------
//
// HERE RATHER THAN IN fs_test.c BECAUSE fsck CANNOT SEE THIS BUG.
// tfs3_check() compares the inode tree against the RAM bitmap
// (t3_bbm_test()), so a batch whose bitmap never reached the disk looks
// perfectly clean until the next mount re-reads it -- an earlier version
// of this check called fs_check() and passed with the flush removed
// entirely. So it reads the bitmap block back off the device.
#include "ktest.h"

KTEST("fs", "deleting a big file discards it in ONE trim call, not one per run") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    const struct mount *rm = mount_root();
    if (!rm || rm->fs != &tfs3_ops) KTEST_SKIP("the root is not TFS3");
    if (!blkdev_trim_supported(rm->dev)) KTEST_SKIP("the disk does not TRIM");

    // 8 MiB: direct blocks, the single-indirect table and its data, and a
    // double-indirect subtree -- six runs to the old per-run code.
    const char *path = "/.ktest_trimq";
    fs_delete(path);
    static char buf[65536];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 13u + 5u);
    int wrote = 1;
    for (uint32_t off = 0; wrote && off < 8u * 1024 * 1024; off += sizeof buf)
        wrote = fs_write_range(path, off, buf, sizeof buf);
    int synced = wrote && fs_sync(0);

    // Nothing else may TRIM between the two reads -- the lock, not the
    // preemption guard, since a TRIM in flight may be asleep holding it.
    fs_exclusive_begin();
    uint64_t before = 0, after = 0, sectors0 = 0, sectors1 = 0;
    blk_stat_get(BLK_STAT_TRIM, &before, &sectors0, NULL, NULL);
    int deleted = fs_delete(path) && fs_sync(0);
    blk_stat_get(BLK_STAT_TRIM, &after, &sectors1, NULL, NULL);
    fs_exclusive_end();

    KTEST_ASSERT(wrote == 1);
    KTEST_ASSERT(synced == 1);
    KTEST_ASSERT(deleted == 1);
    KTEST_ASSERT_EQ((int64_t)(after - before), 1);
    // ...and that one call covered the whole file (8 MiB is 16384 sectors,
    // plus its pointer tables).
    KTEST_ASSERT(sectors1 - sectors0 >= 16384);
}

KTEST("fs", "a batched commit lands the allocation bitmap on disk") {
    if (!fs_is_persistent()) KTEST_SKIP("RAM-only boot, no disk");
    const struct mount *rm = mount_root();
    if (!rm || rm->fs != &tfs3_ops) KTEST_SKIP("the root is not TFS3");

    int was_strict = storage_sync_strict(), was_batched = storage_sync_batched();
    storage_config_set_mode_for_test(1, 1);   // barriers real, commits deferred

    const char *path = "/.ktest_batchbm";
    fs_delete(path);
    static char buf[8192];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (char)(i * 7u + 1u);
    int wrote = fs_write_range(path, 0, buf, sizeof buf);
    int synced = wrote && fs_sync(0);

    // THE ROOT'S OWN STATE, under the fs lock the VFS would hold: the
    // lookup below walks the same scratch buffers every fs_* call does.
    uint32_t blk = 0, on_disk = 2;
    struct t3_state *sbi = rm->state;
    uint64_t ino = 0;
    struct t3_inode node;
    fs_exclusive_begin();
    if (synced && sbi && sbi->mounted && lookup(sbi, path, &ino, &node)) {
        blk = node.ptrs[0];
        if (blk) {
            uint32_t g = (blk - sbi->group0) / T3_BPG;
            uint32_t bit = (blk - sbi->group0) % T3_BPG;
            static uint8_t disk[T3_BLOCK];
            if (t3_read_block(sbi, t3_group_base(sbi, g), disk)) on_disk = (disk[bit >> 3] >> (bit & 7)) & 1u;
        }
    }
    fs_exclusive_end();
    storage_config_set_mode_for_test(was_strict, was_batched);

    KTEST_ASSERT(wrote == 1);
    KTEST_ASSERT(synced == 1);
    KTEST_ASSERT(blk != 0);
    // THE LOAD-BEARING ONE: the block the file uses is marked allocated
    // in the bitmap AS THE DEVICE HOLDS IT, not as RAM believes it.
    KTEST_ASSERT_EQ((int64_t)on_disk, 1);

    fs_delete(path);
}
