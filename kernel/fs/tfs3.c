// TFS3 -- block groups, inodes, dirent blocks. Format spec:
// docs/tfs3-design.md; host mirror: tools/tfs3_writer.py (the two are
// kept in lockstep with tools/tfs3_writer.py, which reads and writes
// the same format from the host).
//
// STAGE B: probe/format/mount (with backup-superblock fallback) and
// the whole READ side are real; every mutating op fails honestly with
// one klog. The write path + 4-slot journal transactions are Stage C,
// fsck is Stage D -- see docs/roadmap.md's Milestone 15 and the
// the git history. Split into stages so each lands with `make verify`
// green, not because the boundaries are architectural.
//
// Everything on disk is VOLUME-relative: block b lives at sector
// sbi->vol.base_lba + b * T3_SPB, and this file only touches the disk
// through vol_read()/vol_write(). The volume is {0, blk_sector_count()}
// and STAYS that way even inside a partition, because the
// probe loop hands in a partition's extent instead and nothing here
// changes -- that seam is the point (see the design doc's "Volumes
// and partitions").
#include "clockevent.h" // the idle commit is due at a time
#include "fs.h"
#include "fs_ops.h"
#include "errno.h"   // fs_chmod returns a negative errno
#include "tfs3.h"
#include "ktime.h"
#include "caltime.h"
#include "mount.h" // MOUNT_MAX -- the mount limit this backend declares
#include "string.h"
#include "block.h"    // TFS3 talks to a BLOCK DEVICE, not to a disk --
                    // that is what lets a live image mount from RAM
#include "timer.h"          // pit_ticks() -- the idle commit
#include "storage_config.h" // storage.sync -- whether the barriers are real
#include "block_stat.h"     // the read counter lookup() brackets itself with
#include "clocksource.h"    // clocksource_now_ns()
#include "kfmt.h"           // klog_printf() -- the read-only transition says why
#include "klog.h"
#include "tz.h"
#include "heap.h"
#include "kpath.h"
#include "scheduler.h" // the inode locks: whose op, and the wake

// ---- format constants (docs/tfs3-design.md; tfs3_writer.py mirrors) ----

#define T3_BLOCK        4096u
#define T3_SPB          8u              // sectors per block
// The block layer's ADDRESSING unit, not the device's: a 4K-sector disk
// is still eight of these per block (block.h). Sub-block I/O below goes
// through blkdev_*_partial(), which is what keeps that true.
#define T3_SECTOR       512u
#define T3_SB_BLOCK     8u
#define T3_JH_BLOCK     9u
#define T3_GDT_BLOCKS   16u             // fixed -- positions derive without a superblock
#define T3_BPG          32768u          // blocks per group (one 4 KiB bitmap)

// The smallest a group may be. Every group is T3_BPG blocks except
// possibly the last, which may be SHORT -- ext2/3/4's rule, and what
// makes a filesystem smaller than 128 MiB possible at all. This floor is
// generous: one group's metadata is two bitmaps plus an inode table
// (~130 blocks at the default inode density), and a volume that cannot
// hold that plus a root directory is not a filesystem.
#define T3_MIN_GROUP_BLOCKS 512u
#define T3_INODE_SIZE   128u
#define T3_INODES_PER_SECTOR (T3_SECTOR / T3_INODE_SIZE)
#define T3_BACKUP_BLOCKS (T3_GDT_BLOCKS + 1)
#define T3_PTRS_PER_BLOCK (T3_BLOCK / 4u)

// THE ONE STATED MAXIMUM FILE SIZE. The format addresses 12 direct
// blocks plus one single, one double and one triple indirect table and
// nothing past that -- so an index beyond this made
// map_get_or_alloc_tables() take the triple branch anyway and index a
// 4096-byte table with a slot number it has not got, and a big enough
// offset wrapped when narrowed to the uint32_t block index. Checked at
// every door rather than in one of them: write, truncate, the stepped
// write, the two mapping helpers, and an inode's own recorded size,
// since a corrupt inode must not be able to drive the walk either.
#define T3_MAX_FILE_BLOCKS ((uint64_t)12u + T3_PTRS_PER_BLOCK + \
                            (uint64_t)T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK + \
                            (uint64_t)T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK * \
                            T3_PTRS_PER_BLOCK)
#define T3_MAX_FILE_SIZE   (T3_MAX_FILE_BLOCKS * (uint64_t)T3_BLOCK)

// Does [offset, offset+len) fit? Written so `offset + len` is never
// evaluated: at these magnitudes it is the addition that overflows.
static inline int t3_range_fits(uint64_t offset, uint64_t len) {
    return offset <= T3_MAX_FILE_SIZE && len <= T3_MAX_FILE_SIZE - offset;
}

// ---- the two journal geometries -------------------------------------------
//
// The journal is a fixed region between the superblock and the group
// descriptors, so its size is a LAYOUT choice, not a tunable: growing
// it pushes the GDT and group 0 outward, which moves every block group.
// Hence a version, and hence two complete constant sets rather than
// free parameters in the superblock.
//
// Keeping each version's geometry constant is what preserves the
// property the fixed-size GDT exists for -- a reader whose primary
// superblock is unreadable can still find the backups, because there
// are only two possible values of group0_start to try (see
// load_superblock()). A superblock DOES carry the journal/GDT/group0
// offsets, but only as fields a mount VALIDATES against the version's
// constants; they are never believed on their own.
//
// v1 (Milestone 15) held four 4 KiB slots, which is enough for every
// operation that touches at most two dirents and two inodes. A rename
// that MOVES a directory needs five (both dirents, the child's "..",
// and both parents' link counts) and so could not be expressed at all.
// v2 gives 32 -- 128 KiB of journal, a rounding error against a 9 GiB
// volume -- so slot pressure stops being a design constraint on
// operations for the foreseeable future. See docs/decisions.md on why
// this is not ext4's circular log.
#define T3_V1_JDATA     10u
#define T3_V1_JSLOTS     4u
#define T3_V1_GDT       14u
#define T3_V1_GROUP0    30u

#define T3_V2_JDATA     10u
#define T3_V2_JSLOTS    32u
#define T3_V2_GDT       (T3_V2_JDATA + T3_V2_JSLOTS)      // 42
#define T3_V2_GROUP0    (T3_V2_GDT + T3_GDT_BLOCKS)       // 58

#define T3_JSLOTS_MAX   T3_V2_JSLOTS

#define T3_VERSION      2u  // what format() writes
#define T3_VERSION_MIN  1u  // oldest version this kernel still mounts
#define T3_INO_ROOT     1u

#define T3_TYPE_FILE    0u
#define T3_TYPE_DIR     1u
#define T3_TYPE_SYMLINK 2u

// What a mode of zero means -- an inode written before this field
// existed, which is most of them on any disk that predates it.
//
// **0755 FOR EVERYTHING, INCLUDING PLAIN FILES, AND THE REASON IS A
// MEASUREMENT RATHER THAN A PREFERENCE.** It was 0644 for files, which
// is what a Unix umask of 022 produces -- and on real hardware that
// made dash unable to run ANY external command. The laptop's files are
// written by `remote.py sync` THROUGH THIS KERNEL rather than by the
// host seeder, so every one of them got 0644, and dash's exec path
// stats a candidate and refuses a file with no execute bit (EACCES).
// QEMU never showed it: there the seeder sets /bin to 0755.
//
// So the default is the permissive one. A single-user system with no
// login has no one to withhold execute FROM, and the alternative --
// a default that makes half the binaries on a machine unrunnable
// depending on how they got there -- is the "wrong answer is worse
// than an absent one" failure this field was added to avoid.
// `chmod` is what sets anything narrower, and the seeder still marks
// data files 0644 where it knows better.
#define T3_MODE_DEFAULT(type) ((void)(type), 0755u)

// Deeper than any caller can currently express (every fs.h caller
// holds FS_PATH_MAX=64 buffers), but the format has no path cap, so
// this backend's own working buffer is roomier on purpose.
#define T3_PATH_BUF     FS_PATH_MAX
#define T3_NAME_MAX     255

#define T3_SB_READ_RETRIES 3 // same transient-DMA-miss reasoning as TFS2's

// Default bytes-per-inode ratio for format(); tfs3_writer.py mirrors.
#define T3_BYTES_PER_INODE 16384u

// ---- state --------------------------------------------------------------

struct t3_vol {
    // THE DEVICE, not "the active device". TFS3 is handed its volume by
    // fs_ops.init() now, because with a mount table there is no single
    // active device it could correctly assume -- a TFS3 root reading
    // blk_active() while /boot is mounted reads the ESP. Linux's
    // super_block->s_bdev. The base_lba stays 0 in practice (the
    // partition window is in the device, block_part.c), and the field
    // survives because a flat volume seam is what makes a KTEST able to
    // point this backend at a slice of anything.
    const struct block_device *dev;
    uint32_t base_lba;
    uint32_t sector_count;
};

struct t3_gd { uint32_t free_blocks, free_inodes; };

// ---- PER-MOUNT STATE -----------------------------------------------
//
// Everything that describes ONE VOLUME lives in here. The mount table
// allocates one per mount (fs_ops.h's state_alloc/state_free) and every
// op is handed it; inside, it travels as `sbi`, Linux's name for a
// superblock's private info.
//
// WHAT MUST STAY TRUE: a per-volume field belongs IN here. One left
// outside is shared by every mount, and the symptom is one filesystem's
// metadata written onto another with no error anywhere. The scratch
// below is the deliberate exception, and says why.
#define T3_MAX_GROUPS (T3_GDT_BLOCKS * T3_BLOCK / 16u)
#define T3_NCACHE 16
#define T3_NCACHE_NAME 48

// THE RESOLVED-PATH CACHE. Sized 64 bytes because `fs.h`'s FS_PATH_MAX
// is 64, so every path arriving through the VFS fits; a longer one
// (T3_PATH_BUF is 256, for internal use) is simply not cached, which is
// correct and merely not fast. 16 entries at 72 bytes is ~1.2 KiB per
// mount, against a mount that costs ~7 KiB.
#define T3_LCACHE 16
#define T3_LCACHE_PATH 64

#define T3_RCACHE_LEVELS 3
#define TRIM_QUEUE 64

struct t3_state {
    struct t3_vol vol;
    int mounted;

    // THE VOLUME IS IN ERROR AND MAY NOT BE WRITTEN. Set when a
    // committed journal transaction could not be applied, and when a
    // replay left one outstanding: in both cases the journal names
    // blocks that a further write could reuse, and the next mount has
    // to be able to replay it. ext4's errors=remount-ro.
    int readonly;

    struct {
        uint8_t version;
        uint8_t flags;
        uint32_t total_blocks;
        uint32_t bpg, ipg, gc;
    } sb;

    // The active format version's geometry, set by
    // set_geometry_version() before anything reads a structure whose
    // position depends on it. All four are constants per version (see
    // the T3_V1_*/T3_V2_* sets above); they are variables only because
    // two versions are mountable. state_alloc() seeds them with v1's.
    uint32_t group0, gdt_block, jdata_block, jslots;

    uint32_t itb;              // inode-table blocks per group
    int mounted_from_backup;   // primary superblock was bad at mount; fsck repair restores it
    uint32_t meta_off;         // first data-ish block offset within a group (2 [+cksum] + itb)
    struct t3_gd *gd;          // free-count caches, sb.gc entries

    // In-RAM copies of every group's block/inode bitmap (sb.gc * 4 KiB
    // each -- ~284 KiB per cache on the 9 GiB image, the same order as
    // TFS2's static full-disk bitmap). RAM is the authority during
    // operation; changed blocks are written through, UNJOURNALED, with
    // the set-before-use / clear-after-persist ordering that makes a
    // crash cost a leak, never a double allocation -- TFS2's exact
    // discipline, inherited deliberately (see the journal section).
    uint8_t *bbm;
    uint8_t *ibm;
    uint32_t *rotor;           // per-group scan start (ext2's trick)

    // Dirty tracking for the write-through: one bit per group, per cache.
    uint8_t bbm_dirty[T3_MAX_GROUPS / 8];
    uint8_t ibm_dirty[T3_MAX_GROUPS / 8];
    uint8_t gdt_dirty[T3_MAX_GROUPS / 8];

    uint32_t jrn_seq;          // this volume's journal sequence number

    // Bumped by every block free. A read that dropped the mount's lock
    // for its device I/O compares it afterwards: if any block was freed
    // meanwhile, the file's mapping may have changed under the inode copy
    // it is walking, so it stops short (vol_read_run()).
    uint64_t free_gen;
    // Bumped by every INODE free: an overwrite that dropped the lock
    // cannot tell its file from a new one that reused the number.
    uint64_t ino_free_gen;
    // Bumped by every inode STAGED for update (txn_stage_inode()): an
    // overwrite whose copy is still current -- nothing staged meanwhile --
    // skips re-reading it, which on a 4 KiB random write is a disk read.
    uint64_t ino_gen;

    // ---- INODE LOCKS (fslock stage 4) ----
    //
    // One entry per lock HELD, never per inode: a table, not a cache, the
    // shape of a futex hash. An entry is an op's -- its pid and the depth
    // at which it holds the mount's lock -- and op_end releases the op's
    // entries when FS_OP's call returns, so no op unlocks by hand. Readers
    // take an inode SHARED, writers and namespace changes EXCLUSIVE (the
    // parent directory too). NO OP WAITS HOLDING ANOTHER: a busy lock
    // makes it release all of its own, wait with the volume lock dropped,
    // and be run again (t3_lock()) -- so there is no lock order to keep,
    // and no rename mutex, which exist in Linux because it holds and waits.
    struct {
        uint64_t ino;
        int pid;
        int depth;
        uint8_t excl;
        uint8_t used;
    } holds[64];
    char ilock_chan;           // an address to park on; any release wakes it

    // Tiny name-lookup cache (dir ino + name -> child ino): path walks
    // are the hot loop, and every component is otherwise a dirent scan.
    // Invalidated wholesale on any mutation -- cheap and obviously
    // correct; per-directory invalidation is a later refinement if it
    // ever shows up in profiles.
    struct {
        uint64_t dir, ino;
        uint8_t len;
        char name[T3_NCACHE_NAME];
    } ncache[T3_NCACHE];
    int ncache_next;

    // WHOLE PATH -> INODE, so a read or write does not walk the path
    // from the root again. `ncache` above caches ONE COMPONENT's
    // name->inode within a directory, which still leaves resolve()
    // reading an inode per component; this caches the whole answer.
    //
    // Measured before building: path resolution was a fifth to two
    // thirds of every block read this filesystem issued
    // (docs/pagecache-design.md).
    struct {
        char path[T3_LCACHE_PATH];  // normalized; "" = empty slot
        uint64_t ino;
    } lcache[T3_LCACHE];
    int lcache_next;

    // One-deep cache of the last-level pointer block being filled, so a
    // long sequential write patches it in RAM and writes it once per
    // 1024 data blocks instead of read-modify-writing 4 KiB per block.
    struct {
        uint32_t blk;   // 0 = empty
        int dirty;
        uint8_t buf[T3_BLOCK];
    } pcache;

    // ---- THE JOURNAL, and everything a call uses as scratch --------
    //
    // PER MOUNT since stage 2 of docs/fslock-design.md: two volumes'
    // operations must be able to run at once, so nothing one call
    // leaves here may be another volume's. Still ONE call per volume at
    // a time -- the mount's lock -- which is what lets the scratch be
    // per mount rather than per call. ~250 KiB, half of it journal.
    uint8_t txn_img[T3_JSLOTS_MAX][T3_BLOCK];   // staged images (see txn_commit)
    uint32_t txn_target[T3_JSLOTS_MAX];
    struct blk_io txn_io[T3_JSLOTS_MAX];        // the targets, as one batch
    int txn_count;
    int txn_credits;             // what txn_begin() promised; see txn_stage()
    int txn_deferred;            // batched: open across writes (txn_begin())
    uint64_t txn_staged_tick;    // when the deferred txn last grew

    struct {
        uint32_t blk;            // 0 = empty
        uint8_t buf[T3_BLOCK];
    } rcache[T3_RCACHE_LEVELS];  // read-side pointer tables (rcache_get())
    struct {
        uint32_t blk;            // 0 = empty
        int dirty;
        uint8_t buf[T3_BLOCK];
    } mcache[2];                 // write-walk mid/top tables (mcache_load())
    struct {
        uint32_t *v;
        uint32_t n, cap;
        int active;
        int overflow;
    } alog;                      // allocation rollback (alog_begin())
    struct blk_range trim_q[TRIM_QUEUE];
    int trim_n;

    uint8_t blk[T3_BLOCK];       // general block scratch
    uint8_t ptr_blk[T3_BLOCK];   // indirect-pointer scratch, kept apart from blk
    uint8_t dirblk[T3_BLOCK];    // tfs3_list's copy of a dirent block
    char norm_scratch[KPATH_SCRATCH_FOR(T3_PATH_BUF)];   // normalize()'s
    // ONE PATH BUFFER PER FUNCTION, never shared: these calls nest
    // (tfs3_is_dir -> lookup -> normalize) and none recurses.
    struct {
        char lookup_norm[T3_PATH_BUF];
        char split_parent_parent[T3_PATH_BUF];
        char tfs3_is_dir_norm[T3_PATH_BUF];
        char tfs3_exists_norm[T3_PATH_BUF];
        char tfs3_list_norm[T3_PATH_BUF];
        char tfs3_chmod_norm[T3_PATH_BUF];
        char tfs3_stat_norm[T3_PATH_BUF];
        char create_entry_inner_norm[T3_PATH_BUF];
        char tfs3_touch_norm[T3_PATH_BUF];
        char tfs3_mkdir_norm[T3_PATH_BUF];
        char tfs3_write_range_norm[T3_PATH_BUF];
        char tfs3_write_norm[T3_PATH_BUF];
        char tfs3_delete_norm[T3_PATH_BUF];
        char tfs3_link_norm[T3_PATH_BUF];
        char tfs3_link_newnorm[T3_PATH_BUF];
        char tfs3_rename_oldn[T3_PATH_BUF];
        char tfs3_rename_newn[T3_PATH_BUF];
        char tfs3_truncate_norm[T3_PATH_BUF];
        char tfs3_write_range_begin_norm[T3_PATH_BUF];
    } pb;

};

// The journal staging and the per-call scratch used to live here, at
// file scope; they are in struct t3_state now (see its last section).

// FLUSHES BOTH CACHES, and the resolved-path one rides here rather than
// getting its own entry point on purpose: the five call sites that
// already invalidate names -- create, delete, link, rename, unmount --
// are exactly the operations that can change which inode a path names.
// A second function would have to be added to all five, and the one
// that got missed would hand back a stale inode number, which is a read
// of somebody else's file.
static void ncache_flush(struct t3_state *sbi) {
    for (int i = 0; i < T3_NCACHE; i++) sbi->ncache[i].dir = 0;
    sbi->ncache_next = 0;
    for (int i = 0; i < T3_LCACHE; i++) sbi->lcache[i].path[0] = '\0';
    sbi->lcache_next = 0;
}

// ---- little-endian field access (hand-serialized on disk) ---------------

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

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
// HERE, not in read_block(), and that is the whole point. read_inode()
// reads ONE SECTOR (`vol_read_sectors(lba, 1, ...)`), not a block, so a
// block-level overlay missed exactly the read that mattered and the
// symptom did not change. This is the one function every read in this
// backend passes through, which is what makes the overlay complete
// rather than nearly complete.
//
// The staged image IS the current truth; that is what an open
// transaction means. Only the DEFERRED one is consulted -- an ordinary
// transaction opens, stages and commits with no read in between.
static int vol_read_sectors(struct t3_state *sbi, uint32_t lba, int count, void *buf) {
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
    // Partial: a one-sector metadata read (read_inode(), the superblock)
    // is less than a block on a 4K-sector disk. Whole blocks pass through.
    return blkdev_read_partial(sbi->vol.dev, sbi->vol.base_lba + lba, count, buf);
}

static void rcache_drop(struct t3_state *sbi);

// Stop writing this volume, and say so where a person can see it.
// The mount flag is the REPORT (df, mount); t3_state.readonly is the
// ENFORCEMENT, because at init() time -- a failed replay -- there is
// no mount entry yet to flag.
static void vol_go_readonly(struct t3_state *sbi, const char *why) {
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
                return vol_read_sectors(sbi, lba, count, buf);
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

// The write half, for OVERWRITES only (do_write()): a data run rewritten
// in place, with the lock dropped. Returns as vol_read_run() does.
//
// **IT DOES NOT DROP THE READ-SIDE POINTER CACHE**, which every other
// write does (vol_write_sectors()). That rule guards a block that WAS a
// pointer table being reused as data under its old cached number; an
// overwrite's targets are this file's live data blocks, found through its
// own tables, so none of them is a table anyone has cached. Dropping it
// anyway cost a table re-read per 4 KiB random overwrite: 30% of the
// ASUS's random-write rate.
static int vol_write_run(struct t3_state *sbi, uint32_t lba, int count, const void *buf) {
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

static int vol_write_sectors(struct t3_state *sbi, uint32_t lba, int count, const void *buf) {
    if (sbi->readonly) return 0;   // see t3_state.readonly -- one gate, not ten
    if (lba + (uint32_t)count > sbi->vol.sector_count) return 0;
    // ANY write drops the pointer-table read cache -- see rcache_get().
    // Here rather than in write_block() because a coalesced data run
    // goes straight to the device, and a block that was a pointer table
    // before it was freed and reused as data would otherwise still be
    // cached under its old number.
    rcache_drop(sbi);
    // Partial for the one-sector writes (the journal header, format's
    // superblock wipes): every one is under the mount's lock, which is
    // what the read-modify-write on a 4K-sector disk needs.
    return blkdev_write_partial(sbi->vol.dev, sbi->vol.base_lba + lba, count, buf);
}

static int read_block(struct t3_state *sbi, uint32_t blk, void *buf) {
    return vol_read_sectors(sbi, blk * T3_SPB, (int)T3_SPB, buf);
}

static int write_block(struct t3_state *sbi, uint32_t blk, const void *buf) {
    return vol_write_sectors(sbi, blk * T3_SPB, (int)T3_SPB, buf);
}

// ---- the read side's pointer-table cache -----------------------------
//
// THE READ HALF OF WHAT `pcache` DOES FOR WRITES, and the reason reads
// were the slow direction: block_for_index() re-read the indirect table
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
// hold what is on the device. write_block() therefore DROPS the cache
// -- every write, unconditionally, whether or not it targeted a
// pointer table. Anything cleverer needs to know which blocks are
// tables, and being wrong once means a read served from a stale table
// returns another file's data.

static void rcache_drop(struct t3_state *sbi) {
    for (int i = 0; i < T3_RCACHE_LEVELS; i++) sbi->rcache[i].blk = 0;
}

// Returns the cached image of pointer-table block `blk` at `level`,
// reading it if it is not already there. NULL on a device error.
static const uint8_t *rcache_get(struct t3_state *sbi, int level, uint32_t blk) {
    if (level < 0 || level >= T3_RCACHE_LEVELS || !blk) return NULL;
    if (sbi->rcache[level].blk == blk) return sbi->rcache[level].buf;
    if (!read_block(sbi, blk, sbi->rcache[level].buf)) {
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
    // The LAST group may be partial (see group_span()), so the volume
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
// with tfs3_writer.py's backup_groups().
static int backup_groups(uint32_t gc, uint32_t out[2]) {
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
        got = vol_read_sectors(sbi, T3_SB_BLOCK * T3_SPB, 1, sec);
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
        int n = backup_groups(gc, groups);
        for (int i = 0; i < n; i++) {
            uint32_t gbase = group0 + groups[i] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t blk = gbase + gspan - 1;
            if (!vol_read_sectors(sbi, blk * T3_SPB, 1, sec)) continue;
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
// with the geometry helpers where it belongs. See group_span() for what
// a partial group is and why the backup moved.
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g);

static int write_superblock_everywhere(struct t3_state *sbi) {
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
    int ok = write_block(sbi, T3_SB_BLOCK, sbi->blk);
    uint32_t groups[2];
    int n = backup_groups(sbi->sb.gc, groups);
    for (int i = 0; i < n; i++) {
        uint32_t blk = group_backup_block(sbi, groups[i]);
        if (blk && !write_block(sbi, blk, sbi->blk)) ok = 0;
    }
    return ok;
}

// Declared here because persist_superblock() above needs it and the
// geometry helpers below own it -- see group_span().
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g);

// ---- group / inode geometry ----------------------------------------------

static uint32_t group_base(struct t3_state *sbi, uint32_t g) { return sbi->group0 + g * T3_BPG; }

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
static uint32_t group_span(struct t3_state *sbi, uint32_t g) {
    uint32_t base = group_base(sbi, g);
    if (base >= sbi->sb.total_blocks) return 0;
    uint32_t rest = sbi->sb.total_blocks - base;
    return rest < T3_BPG ? rest : T3_BPG;
}

// Where group g's superblock/GDT backup lives: its LAST block, which for
// a partial group is the last block of the volume rather than a block
// past the end of it.
static uint32_t group_backup_block(struct t3_state *sbi, uint32_t g) {
    uint32_t span = group_span(sbi, g);
    return span ? group_base(sbi, g) + span - 1 : 0;
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
static int inode_pos(struct t3_state *sbi, uint64_t ino, uint32_t *out_lba, uint32_t *out_off) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t idx = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return 0;
    uint32_t table = group_base(sbi, g) + 2 + cksum_table_blocks(sbi);
    *out_lba = table * T3_SPB + idx / T3_INODES_PER_SECTOR;
    *out_off = (idx % T3_INODES_PER_SECTOR) * T3_INODE_SIZE;
    return 1;
}

struct t3_inode {
    uint8_t type;
    uint16_t links;
    uint64_t size;
    uint64_t created, modified;
    uint32_t ptrs[15]; // 12 direct + single + double + triple
    // **PERMISSION BITS, AND ZERO MEANS "NOT SET" RATHER THAN "NO
    // ACCESS".** Stored at offset 92, inside the range the checksum has
    // always covered (bytes 0..87 and 92..127) and which every version
    // of this format has written as zero. That is what makes this an
    // extension and not a format revision: an inode written by an older
    // kernel reads back mode 0, and an inode written with a mode still
    // validates against an older kernel's checksum, because that kernel
    // already folds 92..127 in. No version bump, no migration, and
    // T3_VERSION_MIN is untouched.
    //
    // A zero is answered with mode_default() at read time, so a disk
    // that predates this field behaves exactly as it did.
    uint16_t mode;
};

static int read_inode(struct t3_state *sbi, uint64_t ino, struct t3_inode *out) {
    uint32_t lba, off;
    if (!inode_pos(sbi, ino, &lba, &off)) return 0;
    uint8_t sec[T3_SECTOR];
    if (!vol_read_sectors(sbi, lba, 1, sec)) return 0;
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

static int block_for_index(struct t3_state *sbi, const struct t3_inode *node, uint32_t idx,
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
        if (!block_for_index(sbi, dir, b, &blk) || !blk || !read_block(sbi, blk, sbi->blk)) return 0;
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
        if (!read_inode(sbi, ino, &dir) || dir.type != T3_TYPE_DIR) return 0;
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

static int normalize(struct t3_state *sbi, const char *path, char *out /* T3_PATH_BUF */) {
    if (!path) return 0;
    struct kpath_scratch sc = { sbi->norm_scratch, sizeof sbi->norm_scratch };
    return k_path_normalize(path, out, T3_PATH_BUF, &sc) == 0 ? 0 : 1;
}

// resolve() + normalize() in one, the common op prologue.
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

// resolve(), THROUGH THE PATH CACHE. The cache sits here rather than in
// lookup() because every door into the filesystem walks a path and only
// the READ path went through lookup(): a write syscall re-walked from
// the root, reading a directory inode per component, every time.
// Measured at 34% of random 4 KiB write throughput on a
// three-component path, and sequential READ was already insensitive to
// path depth -- which is what named the asymmetry.
//
// Only resolutions that SUCCEEDED are cached (lcache_get), and
// ncache_flush() clears it on exactly the operations that can change
// which inode a path names.
static int resolve(struct t3_state *sbi, const char *norm, uint64_t *out_ino) {
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
static int t3_lock(struct t3_state *sbi, uint64_t ino, int excl) {
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
    char *const norm = sbi->pb.lookup_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;

    uint64_t reads0 = 0;
    blk_stat_get(BLK_STAT_READ, &reads0, NULL, NULL, NULL);
    uint64_t t0 = clocksource_now_ns();

    uint64_t ino = 0;
    int ok = resolve(sbi, norm, &ino);
    if (ok) {
        if (out_ino) *out_ino = ino;
        // STILL READ, never cached. The inode's CONTENTS change on
        // every write -- size, mtime, block pointers -- so caching the
        // struct would need invalidating on the hot path rather than on
        // the rare one. Only the path -> number mapping is cached, and
        // that changes solely when the namespace does.
        if (out_node) ok = read_inode(sbi, ino, out_node);
    }

    uint64_t reads1 = 0;
    blk_stat_get(BLK_STAT_READ, &reads1, NULL, NULL, NULL);
    g_lookup_calls++;
    g_lookup_reads += reads1 - reads0;
    g_lookup_ns += clocksource_now_ns() - t0;
    return ok;
}

// ---- allocation (RAM bitmaps, write-through, leak-safe ordering) ---------

static void mark_dirty(uint8_t *set, uint32_t g) { set[g >> 3] |= (uint8_t)(1u << (g & 7)); }
static int test_dirty(const uint8_t *set, uint32_t g) { return (set[g >> 3] >> (g & 7)) & 1; }
static void clear_dirty(uint8_t *set, uint32_t g) { set[g >> 3] &= (uint8_t)~(1u << (g & 7)); }

static int bbm_test(struct t3_state *sbi, uint32_t g, uint32_t i) { return (sbi->bbm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
static void bbm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v) {
    uint8_t *b = &sbi->bbm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    mark_dirty(sbi->bbm_dirty, g);
}
static int ibm_test(struct t3_state *sbi, uint32_t g, uint32_t i) { return (sbi->ibm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
static void ibm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v) {
    uint8_t *b = &sbi->ibm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    mark_dirty(sbi->ibm_dirty, g);
}

static void alog_push(struct t3_state *sbi, uint32_t blk); // rollback log, defined below with its story
static void map_cache_forget(struct t3_state *sbi, uint32_t blk); // pointer-table caches, defined below

// Highest usable data offset within group g (backup regions excluded).
static uint32_t group_data_end(struct t3_state *sbi, uint32_t g) {
    uint32_t groups[2];
    int n = backup_groups(sbi->sb.gc, groups);
    uint32_t span = group_span(sbi, g);
    for (int i = 0; i < n; i++) {
        if (groups[i] == g) {
            return span > T3_BACKUP_BLOCKS ? span - T3_BACKUP_BLOCKS : 0;
        }
    }
    return span;
}

// Try to allocate one specific block (the adjacent-first fast path).
static void trim_flush(struct t3_state *sbi);

static int alloc_block_at(struct t3_state *sbi, uint32_t blk) {
    trim_flush(sbi);
    if (blk < sbi->group0 + sbi->meta_off) return 0;
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc || i < sbi->meta_off || i >= group_data_end(sbi, g)) return 0;
    if (bbm_test(sbi, g, i)) return 0;
    bbm_set(sbi, g, i, 1);
    sbi->gd[g].free_blocks--;
    mark_dirty(sbi->gdt_dirty, g);
    alog_push(sbi, blk);
    return 1;
}

// Allocate one data block: `hint` (try hint+1's spirit: exactly that
// block) first, then rotor scan of the preferred group, then every
// other group. Returns the block number or 0.
static uint32_t alloc_block(struct t3_state *sbi, uint32_t prefer_group, uint32_t adjacent_to) {
    trim_flush(sbi);
    if (adjacent_to && alloc_block_at(sbi, adjacent_to + 1)) return adjacent_to + 1;
    if (prefer_group >= sbi->sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < sbi->sb.gc; n++) {
        uint32_t g = (prefer_group + n) % sbi->sb.gc;
        if (sbi->gd[g].free_blocks == 0) continue;
        uint32_t end = group_data_end(sbi, g);
        uint32_t start = sbi->rotor[g];
        if (start < sbi->meta_off || start >= end) start = sbi->meta_off;
        for (uint32_t k = 0; k < end - sbi->meta_off; k++) {
            uint32_t i = start + k;
            if (i >= end) i = sbi->meta_off + (i - end);
            if (!bbm_test(sbi, g, i)) {
                bbm_set(sbi, g, i, 1);
                sbi->gd[g].free_blocks--;
                mark_dirty(sbi->gdt_dirty, g);
                sbi->rotor[g] = i + 1;
                alog_push(sbi, group_base(sbi, g) + i);
                return group_base(sbi, g) + i;
            }
        }
    }
    return 0;
}

static void free_block_bit(struct t3_state *sbi, uint32_t blk) {
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc) return;
    if (!bbm_test(sbi, g, i)) return; // double-free guard -- fsck's problem, not a crash
    // NOT WHILE A READ IS AT THE DEVICE WITHOUT THE LOCK: the block could
    // be reallocated and written before that read lands, and it would
    // return another file's bytes. Linux's inode_dio_wait(), per mount.
    mount_io_drain(sbi);
    sbi->free_gen++;
    bbm_set(sbi, g, i, 0);
    sbi->gd[g].free_blocks++;
    mark_dirty(sbi->gdt_dirty, g);
    if (sbi->rotor[g] > i) sbi->rotor[g] = i;
    // THE CACHES ARE KEYED BY BLOCK NUMBER AND OUTLIVE ONE OPERATION,
    // so a freed table block must be forgotten here -- reallocating it
    // as something else would otherwise hand the next walk a stale
    // image, and the write that followed would land in the wrong file.
    map_cache_forget(sbi, blk);
}

static uint64_t alloc_inode(struct t3_state *sbi, uint32_t prefer_group) {
    if (prefer_group >= sbi->sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < sbi->sb.gc; n++) {
        uint32_t g = (prefer_group + n) % sbi->sb.gc;
        if (sbi->gd[g].free_inodes == 0) continue;
        for (uint32_t i = 0; i < sbi->sb.ipg; i++) {
            if (!ibm_test(sbi, g, i)) {
                ibm_set(sbi, g, i, 1);
                sbi->gd[g].free_inodes--;
                mark_dirty(sbi->gdt_dirty, g);
                return (uint64_t)g * sbi->sb.ipg + i;
            }
        }
    }
    return 0;
}

static void free_inode_bit(struct t3_state *sbi, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc || !ibm_test(sbi, g, i)) return;
    sbi->ino_free_gen++;
    ibm_set(sbi, g, i, 0);
    sbi->gd[g].free_inodes++;
    mark_dirty(sbi->gdt_dirty, g);
}

// Write every dirty bitmap block and GDT block through to disk.
// UNJOURNALED on purpose: called BEFORE the transaction that makes an
// allocation reachable (set-before-use) and AFTER the transaction
// that makes a free unreachable (clear-after-persist), so a crash at
// any point costs a leaked block/inode -- never a double allocation.
// TFS2's exact metadata-ordering rule, see docs/decisions.md.
static int flush_alloc_state(struct t3_state *sbi) {
    int ok = 1;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        if (test_dirty(sbi->bbm_dirty, g)) {
            if (!write_block(sbi, group_base(sbi, g), sbi->bbm + g * T3_BLOCK)) ok = 0;
            clear_dirty(sbi->bbm_dirty, g);
        }
        if (test_dirty(sbi->ibm_dirty, g)) {
            if (!write_block(sbi, group_base(sbi, g) + 1, sbi->ibm + g * T3_BLOCK)) ok = 0;
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
        if (!write_block(sbi, sbi->gdt_block + tb, sbi->blk)) ok = 0;
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

static void alog_begin(struct t3_state *sbi) { sbi->alog.n = 0; sbi->alog.active = 1; sbi->alog.overflow = 0; }

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

static void alog_commit(struct t3_state *sbi) { sbi->alog.active = 0; }

// Remove one block from the log: it just became referenced by a
// COMMITTED transaction (directory growth commits mid-operation), so
// an outer rollback must not free it out from under that reference.
static void alog_forget(struct t3_state *sbi, uint32_t blk) {
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
static void alog_cancel(struct t3_state *sbi) { sbi->alog.n = 0; sbi->alog.active = 0; }

static void alog_rollback(struct t3_state *sbi) {
    if (sbi->alog.overflow) {
        // Couldn't track everything -- leak honestly rather than free
        // a partial (possibly wrong) set. fsck reclaims.
        klog_write("tfs3: rollback log overflowed -- leaked blocks left for fsck\n");
    } else {
        for (uint32_t i = 0; i < sbi->alog.n; i++) free_block_bit(sbi, sbi->alog.v[i]);
        flush_alloc_state(sbi);
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

static void trim_flush(struct t3_state *sbi) {
    if (sbi->trim_n) blkdev_trim_ranges(sbi->vol.dev, sbi->trim_q, sbi->trim_n);
    sbi->trim_n = 0;
}

static void trim_run(struct t3_state *sbi, uint32_t first_blk, uint32_t count) {
    if (!count || !blkdev_trim_supported(sbi->vol.dev)) return;
    uint32_t lba = sbi->vol.base_lba + first_blk * T3_SPB, n = count * T3_SPB;
    for (int i = 0; i < sbi->trim_n; i++) {
        struct blk_range *q = &sbi->trim_q[i];
        if (q->lba + q->count == lba) { q->count += n; return; }
        if (lba + n == q->lba) { q->lba = lba; q->count += n; return; }
    }
    if (sbi->trim_n == TRIM_QUEUE) trim_flush(sbi);
    sbi->trim_q[sbi->trim_n++] = (struct blk_range){ lba, n };
}

// ---- journal: one fixed-size multi-block transaction ---------------------
//
// What goes THROUGH the journal: dirent blocks and inode-table blocks
// -- the structures whose torn write is NAMESPACE corruption. What
// deliberately does NOT: bitmap and GDT blocks (leak-safe ordering
// above; a torn bitmap costs a leak fsck reclaims), data blocks and
// indirect-pointer blocks (unreachable until the inode that points at
// them commits). This is narrower than the design doc's first sketch
// ("dirent + inode + bitmaps") and is a deliberate revision: bitmaps
// under the leak rule keeps every operation's transaction at <= 3
// slots with the same crash guarantees TFS2 gives, for half the
// journaled bytes. The doc's journal section records this.
//
// Commit discipline = persist_record()'s, generalized (see TFS2 for
// the two-barrier reasoning): stage images + committed header, FLUSH,
// write targets, FLUSH, clear header (no barrier -- a stale committed
// header just replays idempotently).

// A DEFERRED transaction is never reset here: it holds other operations'
// completed writes, and only txn_flush_deferred() -- which clears the
// flag first -- may commit or drop it. Every other caller that resets
// on failure is unwinding its OWN transaction, and after a refused
// txn_begin() it has none.
static void txn_reset(struct t3_state *sbi) {
    if (sbi->txn_deferred) return;
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
}

// Open a transaction that will stage at most `credits` DISTINCT blocks,
// jbd2's reservation discipline in miniature: an operation that cannot
// fit says so before it has changed anything, instead of discovering it
// halfway through when txn_stage() returns 0 and every caller has to
// unwind by hand. Returns 0 if this volume's journal is too small --
// which is the whole reason it exists, since a v1 image (four slots)
// genuinely cannot express a directory move. Callers turn that into a
// refusal with an explanation, not a corrupt half-operation.
// ---- the DEFERRED transaction (storage.sync = batched) ---------------
//
// Under `batched` a write stages its inode and does NOT commit; many
// writes then share one commit, which is what removes the barriers that
// are 56-69% of block time on real hardware. What is deferred is only
// the INODE UPDATE: do_write_inner() has already written the data
// blocks and the allocation bitmaps by the time it opens a transaction,
// so a crash loses a size update and leaks the blocks past it -- a leak
// `fsck` reclaims, not a journal that cannot be replayed.
//
// THE HAZARD IS txn_begin() ITSELF. It zeroes txn_count, so any
// operation that opened a transaction while one was deferred would
// silently discard every inode staged in it -- writes reported as
// succeeded, vanishing. So the commit is forced HERE, in the one
// function every transaction in this file goes through, rather than at
// the call sites: create, delete, rename, truncate and the rest are
// covered without being edited, and a new one cannot forget.
//
// The journal is PER MOUNT, so a deferred transaction is always this
// volume's own; no other mount can find it half-staged.
static int txn_commit(struct t3_state *sbi);

// Commit whatever is deferred on this mount. Safe to call with nothing
// open. Returns 0 only if the commit itself failed.
static int txn_flush_deferred(struct t3_state *sbi) {
    if (!sbi->txn_deferred) return 1;
    int count = sbi->txn_count, credits = sbi->txn_credits;
    sbi->txn_deferred = 0;
    // The allocation state the batch accumulated goes first, BEFORE the
    // commit that makes those blocks reachable -- do_write_inner() skips
    // it per write under `batched`, which is the whole saving.
    int ok = flush_alloc_state(sbi) && txn_commit(sbi);
    if (!ok) {
        // KEEP THE STAGED WORK. txn_commit() zeroed the count, but the
        // images are intact and the targets are either untouched or
        // left committed for replay, so the next flush can retry.
        // Dropping them here lost another process's completed writes
        // whenever an unrelated operation failed its journal write:
        // blocks allocated, inode never updated, a leak at fsck.
        sbi->txn_count = count;
        sbi->txn_credits = credits;
        sbi->txn_deferred = 1;
    }
    return ok;
}

static int txn_begin(struct t3_state *sbi, int credits) {
    // See the hazard note above: never discard staged work -- and a
    // flush that FAILS keeps it, so the new operation is refused rather
    // than staged on top of it.
    if (sbi->txn_deferred && !txn_flush_deferred(sbi)) return 0;
    sbi->txn_count = 0;
    sbi->txn_credits = 0;
    if (credits <= 0 || credits > (int)sbi->jslots) return 0;
    sbi->txn_credits = credits;
    return 1;
}

// Stage `blk`'s new image into the transaction. Returns a writable
// pointer to the staged 4 KiB image (pre-loaded from disk so callers
// patch in place), or 0 when full/read-failed. Staging the same block
// twice returns the same image.
static uint8_t *txn_stage(struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) {
        if (sbi->txn_target[i] == blk) return sbi->txn_img[i];
    }
    // Past the reservation is a bug in the CALLER's credit count, not
    // a runtime condition -- but refusing here keeps it a failed
    // operation rather than a torn one.
    if (sbi->txn_count >= sbi->txn_credits || sbi->txn_count >= (int)sbi->jslots) return 0;
    if (!read_block(sbi, blk, sbi->txn_img[sbi->txn_count])) return 0;
    sbi->txn_target[sbi->txn_count] = blk;
    return sbi->txn_img[sbi->txn_count++];
}

// The header's slot table has to move for v2: four v1 entries end at
// byte 44, which is exactly where v1 put the header checksum, so 32 of
// them would overwrite it. v2 starts the table at 16 and puts the
// checksum in the sector's last four bytes -- room for 60 slots before
// the two would meet again. The version decides which shape is
// written and read; nothing guesses.
#define T3_JH_V1_SLOTS_OFF  12u
#define T3_JH_V1_CKSUM_OFF  44u
#define T3_JH_V2_SLOTS_OFF  16u
#define T3_JH_V2_CKSUM_OFF  508u   // ON-DISK: never derive it from a sector size

static uint32_t jh_slots_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_SLOTS_OFF : T3_JH_V1_SLOTS_OFF;
}
static uint32_t jh_cksum_off(uint32_t version) {
    return version >= 2 ? T3_JH_V2_CKSUM_OFF : T3_JH_V1_CKSUM_OFF;
}

static int write_journal_header(struct t3_state *sbi, int commit) {
    uint8_t sec[T3_SECTOR];
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = jh_cksum_off(sbi->sb.version);
    k_memset(sec, 0, sizeof(sec));
    sec[0] = 'J'; sec[1] = 'R'; sec[2] = 'N'; sec[3] = '3';
    sec[4] = (uint8_t)commit;
    sec[5] = (uint8_t)sbi->txn_count;
    wr32(sec + 8, sbi->jrn_seq);
    for (int i = 0; i < sbi->txn_count; i++) {
        wr32(sec + slots + (uint32_t)i * 8, sbi->txn_target[i]);
        wr32(sec + slots + (uint32_t)i * 8 + 4, k_fnv1a(sbi->txn_img[i], T3_BLOCK));
    }
    wr32(sec + ck, k_fnv1a(sec, ck));
    return vol_write_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec);
}

// A journal barrier, or a no-op under `storage.sync = lazy`.
//
// BOTH of txn_commit()'s barriers go through here, because neither is
// the optional one: the first orders the journal against the targets so
// a crash mid-target-write can be replayed, the second orders the
// targets against clearing the commit flag so a crash cannot leave the
// journal saying "nothing to do" over work that never landed. Turning
// them off trades crash recoverability for throughput, which is ext4's
// `nobarrier` exactly -- see storage_config.c for what it costs and why
// the default is the other one.
//
// It still returns 1 when skipped. A caller must not read "no barrier
// was issued" as "the barrier failed": failure ABANDONS the
// transaction, and doing that on every write would be a filesystem that
// refuses to write at all.
static int txn_barrier(struct t3_state *sbi) {
    if (!storage_sync_strict()) return 1;
    return blkdev_flush(sbi->vol.dev);
}

// A COMMIT HAS THREE OUTCOMES, AND THE THIRD IS THE POINT.
//
// Past the commit point the journal is durable and WILL be replayed,
// so the blocks the operation allocated are live even though the
// operation failed -- freeing them hands a replayed inode pointers to
// space something else can take. Before the commit point it is the
// exact opposite: nothing landed, and not freeing them leaks. The two
// used to be one `return 0`, and every caller rolled back.
//
// jbd2 draws the same line, and answers it the same way
// (jbd2_journal_abort() plus errors=remount-ro): a filesystem that
// cannot apply its own commit stops writing, because the next mount's
// replay is the only thing that can still finish the job.
enum t3_commit {
    T3_COMMIT_ABORTED   = 0,   // nothing durable -- the caller must roll back
    T3_COMMIT_OK        = 1,
    T3_COMMIT_UNAPPLIED = 2,   // committed; replay owns these blocks now
};

// THE JOURNAL IMAGES IN AS FEW COMMANDS AS THE DEVICE TAKES. The
// journal's data blocks are contiguous on disk and txn_img is
// contiguous in memory, so a commit that wrote them one 4 KiB command at
// a time was paying per-command latency up to 32 times for one run.
static int write_journal_images(struct t3_state *sbi) {
    uint32_t per = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
    if (per == 0) per = 1;
    for (int i = 0; i < sbi->txn_count; ) {
        uint32_t k = (uint32_t)(sbi->txn_count - i);
        if (k > per) k = per;
        if (!vol_write_sectors(sbi, (sbi->jdata_block + (uint32_t)i) * T3_SPB,
                               (int)(k * T3_SPB), sbi->txn_img[i])) return 0;
        i += (int)k;
    }
    return 1;
}

// THE TARGETS ALL IN FLIGHT AT ONCE. They are scattered, independent,
// and fenced by the barriers either side, so their order among
// themselves never mattered -- which is exactly a batch
// (blkdev_submit_batch(); AHCI queues it with NCQ, others loop). In the
// mount rather than on the stack, for the frame budget.

static int write_targets(struct t3_state *sbi) {
    if (sbi->readonly) return 0;             // vol_write_sectors()'s gate
    rcache_drop(sbi);                         // ...and its cache rule
    for (int i = 0; i < sbi->txn_count; i++) {
        uint32_t lba = sbi->txn_target[i] * T3_SPB;
        if (lba + T3_SPB > sbi->vol.sector_count) return 0;
        sbi->txn_io[i] = (struct blk_io){ .lba = sbi->vol.base_lba + lba, .count = T3_SPB,
                                       .write = 1, .buf = sbi->txn_img[i] };
    }
    return blkdev_submit_batch(sbi->vol.dev, sbi->txn_io, sbi->txn_count);
}

static enum t3_commit txn_commit_raw(struct t3_state *sbi) {
    if (sbi->txn_count == 0) return T3_COMMIT_OK;
    sbi->jrn_seq++;
    if (!write_journal_images(sbi)) { txn_reset(sbi); return T3_COMMIT_ABORTED; }
    if (!write_journal_header(sbi, 1)) { txn_reset(sbi); return T3_COMMIT_ABORTED; }

    // BARRIER 1, and it is checked. The journal's whole guarantee is
    // that everything written before this point is on the platter, so
    // if the flush cannot say that, the targets below must NOT be
    // overwritten: a half-written target with no durable journal behind
    // it is unrecoverable, while abandoning the transaction here costs
    // nothing that was not already lost. blkdev_flush(sbi->vol.dev) returned void until
    // a write-back cache went in underneath (ata_cache.h) -- that is
    // where a deferred write's failure now surfaces.
    if (!txn_barrier(sbi)) {
        klog_write(KLOG_ERR "tfs3: journal barrier failed -- transaction abandoned, "
                   "targets untouched\n");
        txn_reset(sbi);
        return T3_COMMIT_ABORTED;
    }

    int ok = write_targets(sbi);

    // BARRIER 2: the targets must be durable before the commit flag is
    // cleared below, or a crash after clearing it loses the record of
    // work that never reached the disk. A failure here takes the same
    // path a failed target write does -- leave the header committed and
    // let replay finish the job next boot.
    if (!txn_barrier(sbi)) ok = 0;
    if (ok) {
        int saved = sbi->txn_count;
        sbi->txn_count = 0;
        write_journal_header(sbi, 0); // no barrier -- see the discipline note above
        sbi->txn_count = saved;
    } else {
        // Leave the header committed: replay finishes the job next
        // boot, same call replay_journal() in TFS2 makes.
        // Either a target write or barrier 2 -- both mean the same
        // thing here, and only the journal can finish it now.
        klog_write(KLOG_ERR "tfs3: transaction could not be applied -- left committed for replay\n");
    }
    txn_reset(sbi);
    return ok ? T3_COMMIT_OK : T3_COMMIT_UNAPPLIED;
}

// What every caller in this file actually wants: 1 on success, 0 on
// failure. The three-way answer is consumed HERE rather than at the
// sixteen call sites, which is what makes the rule impossible to
// forget -- the same argument txn_begin() makes for forcing a deferred
// flush in one place. A caller's own alog_rollback() then frees
// nothing, because there is nothing it may undo.
static int txn_commit(struct t3_state *sbi) {
    enum t3_commit r = txn_commit_raw(sbi);
    if (r == T3_COMMIT_UNAPPLIED) {
        alog_cancel(sbi);
        vol_go_readonly(sbi, "a committed transaction could not be applied");
    }
    return r == T3_COMMIT_OK;
}

// 1 if nothing is outstanding -- replayed, torn and discarded, or
// none to begin with. 0 when a COMMITTED transaction is still on the
// disk unapplied, which the caller must not mount writable over: a
// write could reuse a block the journal names, and the replay is the
// only thing that can still finish it.
static int replay_journal(struct t3_state *sbi) {
    uint8_t sec[T3_SECTOR];
    if (!vol_read_sectors(sbi, T3_JH_BLOCK * T3_SPB, 1, sec)) return 1;
    if (!(sec[0] == 'J' && sec[1] == 'R' && sec[2] == 'N' && sec[3] == '3')) return 1;
    uint32_t slots = jh_slots_off(sbi->sb.version), ck = jh_cksum_off(sbi->sb.version);
    if (k_fnv1a(sec, ck) != rd32(sec + ck)) return 1; // torn header = no transaction
    sbi->jrn_seq = rd32(sec + 8);
    if (!sec[4]) return 1; // not committed
    uint32_t count = sec[5];
    if (count == 0 || count > sbi->jslots) count = 0;

    int all_ok = (count > 0);
    for (uint32_t i = 0; i < count && all_ok; i++) {
        if (!read_block(sbi, sbi->jdata_block + i, sbi->txn_img[i])) all_ok = 0;
        else if (k_fnv1a(sbi->txn_img[i], T3_BLOCK) != rd32(sec + slots + i * 8 + 4)) all_ok = 0;
    }
    if (all_ok) {
        for (uint32_t i = 0; i < count && all_ok; i++) {
            if (!write_block(sbi, rd32(sec + slots + i * 8), sbi->txn_img[i])) all_ok = 0;
        }
        if (all_ok && !blkdev_flush(sbi->vol.dev)) {
            // Same rule as txn_commit()'s barrier 2: if the replayed
            // targets are not durable, do NOT clear the committed flag
            // below -- the next boot must replay them again.
            klog_write(KLOG_ERR "tfs3: journal replay barrier failed -- left committed "
                       "for next boot\n");
            return 0;
        }
        if (all_ok) {
            klog_write("tfs3: replayed a committed journal transaction (");
            klog_write_dec(count); klog_write(" blocks)\n");
        } else {
            klog_write(KLOG_ERR "tfs3: journal replay write failed -- left committed for next boot\n");
            return 0;
        }
    } else {
        klog_write("tfs3: discarded a torn journal transaction\n");
    }
    sbi->txn_count = 0;
    write_journal_header(sbi, 0);
    return 1;
}

// ---- inode staging (through the transaction) ------------------------------

static void pack_inode_into(uint8_t *p, const struct t3_inode *node) {
    k_memset(p, 0, T3_INODE_SIZE);
    p[0] = node->type;
    wr16(p + 2, node->links);
    wr64(p + 4, node->size);
    wr64(p + 12, node->created);
    wr64(p + 20, node->modified);
    for (int i = 0; i < 15; i++) wr32(p + 28 + i * 4, node->ptrs[i]);
    wr16(p + 92, node->mode);   // see struct t3_inode -- inside the checksum
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    wr32(p + 88, k_fnv1a(chk, sizeof(chk)));
}

// Patch inode `ino`'s 128 bytes inside its (journal-staged) table
// block. `node == 0` zeroes the slot -- a dead inode fails its
// checksum by design, the bitmap is the allocation authority.
static int txn_stage_inode(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node) {
    sbi->ino_gen++;   // see t3_state.ino_gen
    uint32_t lba, off;
    if (!inode_pos(sbi, ino, &lba, &off)) return 0;
    uint32_t blk = lba / T3_SPB;
    uint32_t within = (lba % T3_SPB) * T3_SECTOR + off;
    uint8_t *img = txn_stage(sbi, blk);
    if (!img) return 0;
    if (node) pack_inode_into(img + within, node);
    else k_memset(img + within, 0, T3_INODE_SIZE);
    return 1;
}

static uint64_t now_epoch(void) {
    struct rtc_time t;
    ktime_read(&t);
    return cal_rtc_to_epoch(&t);
}

// ---- block-map allocation (write side) ------------------------------------


// THE MIDDLE LEVELS OF THE WRITE WALK, cached the way `pcache` caches
// the leaf. Without this, map_get_or_alloc_tables() re-read the mid
// table from the device for EVERY block written past 4 MiB -- the same
// defect block_for_index() had on the read side, and the comment below
// the walk claimed the opposite ("one RMW per 1024 data blocks"), which
// described the intent rather than the code.
//
// Two entries because a triple-indirect walk touches two middle levels.
// Per mount, like the journal. Its flush and drop are folded into
// pcache's below, so every existing call site covers it.

static int mcache_flush(struct t3_state *sbi) {
    for (int i = 0; i < 2; i++) {
        if (sbi->mcache[i].blk && sbi->mcache[i].dirty) {
            if (!write_block(sbi, sbi->mcache[i].blk, sbi->mcache[i].buf)) return 0;
            sbi->mcache[i].dirty = 0;
        }
    }
    return 1;
}

static int mcache_load(struct t3_state *sbi, int level, uint32_t blk, int fresh) {
    if (sbi->mcache[level].blk == blk) return 1;
    if (sbi->mcache[level].blk && sbi->mcache[level].dirty) {
        if (!write_block(sbi, sbi->mcache[level].blk, sbi->mcache[level].buf)) return 0;
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
    return read_block(sbi, blk, sbi->mcache[level].buf);
}

static int pcache_flush(struct t3_state *sbi) {
    if (!mcache_flush(sbi)) return 0;
    if (sbi->pcache.blk && sbi->pcache.dirty) {
        if (!write_block(sbi, sbi->pcache.blk, sbi->pcache.buf)) return 0;
        sbi->pcache.dirty = 0;
    }
    return 1;
}

static int pcache_load(struct t3_state *sbi, uint32_t blk, int fresh) {
    if (sbi->pcache.blk == blk) return 1;
    if (!pcache_flush(sbi)) return 0;
    sbi->pcache.blk = blk;
    sbi->pcache.dirty = 0;
    if (fresh) { k_memset(sbi->pcache.buf, 0, T3_BLOCK); sbi->pcache.dirty = 1; return 1; }
    return read_block(sbi, blk, sbi->pcache.buf);
}

// Forget one block wherever it is cached. Clean entries only ever
// reach here: a dirty table belongs to the operation still running,
// which cannot be freeing its own table.
static void map_cache_forget(struct t3_state *sbi, uint32_t blk) {
    if (sbi && sbi->pcache.blk == blk) { sbi->pcache.blk = 0; sbi->pcache.dirty = 0; }
    if (!sbi) return;
    for (int i = 0; i < 2; i++)
        if (sbi->mcache[i].blk == blk) { sbi->mcache[i].blk = 0; sbi->mcache[i].dirty = 0; }
}

static void pcache_drop(struct t3_state *sbi) {
    sbi->pcache.blk = 0;
    sbi->pcache.dirty = 0;
    for (int i = 0; i < 2; i++) { sbi->mcache[i].blk = 0; sbi->mcache[i].dirty = 0; }
}

// Allocate (if needed) and return the pointer-table slot chain for
// file-block `idx`, allocating intermediate pointer blocks as it
// goes. Returns the existing-or-new data block number via *out_blk
// (0 if a fresh one must be allocated by the caller and recorded with
// map_set_block()). This walks with the pcache for the LEAF table.
static int map_get_or_alloc_tables(struct t3_state *sbi, struct t3_inode *node, uint32_t idx,
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
        table = alloc_block(sbi, prefer_group, 0);
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
            next = alloc_block(sbi, prefer_group, 0);
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

static void map_set_block(struct t3_state *sbi, struct t3_inode *node, uint32_t leaf_blk,
                          uint32_t leaf_slot, uint32_t data_blk) {
    if (!leaf_blk) {
        node->ptrs[leaf_slot] = data_blk; // direct
    } else {
        wr32(sbi->pcache.buf + leaf_slot * 4, data_blk);
        sbi->pcache.dirty = 1;
    }
}

// ---- write core ------------------------------------------------------------

// Write [offset, offset+len) into the file behind *node (whose inode
// number is `ino`), allocating as needed, then commit the updated
// inode through the journal. The order is the leak-safe one: bitmap
// state flushes BEFORE the inode transaction makes anything
// reachable. Returns 1/0.
// Can a partially-written block hold bytes worth preserving? The
// question is about the BLOCK, not about where the write starts.
//
// Asking `file_off >= size` looks equivalent and is catastrophically
// not: an APPEND starts exactly at size, so that test is true every
// time and zeroes the whole block, destroying what is already in it.
// The skip is only safe when the block is freshly allocated or BEGINS
// past end-of-file. Both write paths ask here -- the stepped one
// carried its own copy of the wrong test.
static int block_has_live_bytes(int fresh, uint32_t bi, uint64_t size) {
    return !fresh && (uint64_t)bi * T3_BLOCK < size;
}

static int stage_inode_update(struct t3_state *sbi, uint64_t ino, struct t3_inode *node);

static int do_write_inner(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t offset,
                          const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t prefer_group = (uint32_t)(ino / sbi->sb.ipg);
    uint32_t last_alloc = 0;
    uint32_t total = 0;

    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;

        uint32_t leaf_blk, leaf_slot, existing;
        if (!map_get_or_alloc_tables(sbi, node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing))
            return 0;
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = alloc_block(sbi, prefer_group, last_alloc);
            if (!blk) return 0;
            map_set_block(sbi, node, leaf_blk, leaf_slot, blk);
            fresh = 1;
        }
        last_alloc = blk;

        if (chunk == T3_BLOCK) {
            // Run coalescing, the write half of what took TFS2 from 18
            // to 25 MB/s: gather the CONTIGUOUS on-disk run of full
            // blocks this write covers (the adjacent-first allocator
            // makes long runs the common case) and issue it as one
            // multi-sector transfer straight from the caller's buffer
            // -- no bounce, no per-block ATA round trip. Measured on
            // tfs3 before this existed: 4.6 MB/s; the per-block loop
            // was the whole gap.
            uint32_t run = 1;
            uint32_t want = (len - total) / T3_BLOCK; // whole blocks left
            // One transfer must fit the ATA path's per-command cap
            // (128 sectors on DMA, 8 on PIO -- ask, don't assume,
            // same rule TFS2's batching follows).
            uint32_t cap = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                uint32_t nleaf, nslot, nexist;
                if (!map_get_or_alloc_tables(sbi, node, bi + run, prefer_group, &nleaf, &nslot, &nexist))
                    return 0;
                uint32_t nblk = nexist;
                if (!nblk) {
                    nblk = alloc_block(sbi, prefer_group, last_alloc);
                    if (!nblk) return 0;
                    map_set_block(sbi, node, nleaf, nslot, nblk);
                }
                if (nblk != last_alloc + 1) {
                    // Not contiguous: it's allocated and recorded, the
                    // next loop iteration will write it as its own run.
                    last_alloc = 0; // don't bias the next adjacency try
                    break;
                }
                last_alloc = nblk;
                run++;
            }
            if (!vol_write_sectors(sbi, blk * T3_SPB, (int)(run * T3_SPB), src + total)) return 0;
            total += run * T3_BLOCK;
        } else {
            if (!block_has_live_bytes(fresh, bi, node->size)) k_memset(sbi->blk, 0, T3_BLOCK);
            else if (!read_block(sbi, blk, sbi->blk)) return 0;
            k_memcpy(sbi->blk + within, src + total, chunk);
            if (!write_block(sbi, blk, sbi->blk)) return 0;
            total += chunk;
        }
    }
    // FLUSHED, NOT DROPPED. The next write syscall to this file walks
    // the same leaf and middle tables, and re-reading them was ~3 of
    // the ~6 metadata reads every 64 KiB write was issuing.
    if (!pcache_flush(sbi)) return 0;

    if (offset + len > node->size) node->size = offset + len;
    node->modified = now_epoch();
    return stage_inode_update(sbi, ino, node);
}

// The inode half of a write: land the updated inode through the journal
// (or the deferred transaction under `batched`), after the allocation
// state it depends on. Shared by do_write_inner() and do_overwrite().
static int stage_inode_update(struct t3_state *sbi, uint64_t ino, struct t3_inode *node) {
    // SET-BEFORE-USE, and under `batched` it rides the deferred commit
    // instead. Deferring is strictly SAFER than flushing per write: a
    // crash mid-batch then leaves the bitmap saying `free` and the
    // inode unchanged -- consistent -- where a per-write flush leaves
    // blocks marked used by an inode update that never landed, which is
    // the leak fsck exists to reclaim. txn_flush_deferred() keeps the
    // ordering by doing it before the commit.
    if (!storage_sync_batched() && !flush_alloc_state(sbi)) return 0;

    // BATCHED: stage the inode into a transaction that stays open, so
    // many writes share one commit. Re-staging the same inode block
    // returns the SAME image (txn_stage), which txn_stage_inode()
    // patches in place -- so a second write to the same file updates
    // the staged copy rather than needing a slot of its own.
    if (storage_sync_batched()) {
        if (!sbi->txn_deferred) {
            if (!txn_begin(sbi, (int)sbi->jslots)) return 0;
            sbi->txn_deferred = 1;
        }
        sbi->txn_staged_tick = pit_ticks();
        if (!txn_stage_inode(sbi, ino, node)) {
            // Out of slots: commit what is there and start again. The
            // retry cannot fail for the same reason, because the fresh
            // transaction is empty.
            if (!txn_flush_deferred(sbi)) return 0;
            if (!txn_begin(sbi, (int)sbi->jslots)) return 0;
            sbi->txn_deferred = 1;
            if (!txn_stage_inode(sbi, ino, node)) {
                sbi->txn_deferred = 0;   // a fresh transaction: nothing to keep
                txn_reset(sbi);
                return 0;
            }
        }
        return 1;   // durable at the next commit -- see txn_flush_deferred()
    }

    if (!txn_begin(sbi, 1)) return 0;
    if (!txn_stage_inode(sbi, ino, node)) { txn_reset(sbi); return 0; }
    return txn_commit(sbi);                       // the commit point: file grows atomically
}

// Is [offset, offset+len) entirely blocks the file already has, inside
// its size? Then the write allocates nothing and changes no pointer --
// the one shape that may drop the lock (do_overwrite()).
static int is_overwrite(struct t3_state *sbi, const struct t3_inode *node,
                        uint64_t offset, uint32_t len) {
    if (!len || offset + len > node->size) return 0;
    uint32_t first = (uint32_t)(offset / T3_BLOCK);
    uint32_t last = (uint32_t)((offset + len - 1) / T3_BLOCK);
    for (uint32_t bi = first; bi <= last; bi++) {
        uint32_t b;
        if (!block_for_index(sbi, node, bi, &b) || !b) return 0;
    }
    return 1;
}

// AN OVERWRITE IN PLACE, with the mount's lock dropped for each whole-
// block run (vol_write_run()) -- ext4's direct-I/O overwrite, which it
// runs under a SHARED inode lock for the same reason: it allocates
// nothing and changes no pointer, so there is no half-built state for
// another call to see. Partial blocks go through blk, under the lock.
//
// **WHAT IT MUST NOT DO IS COMMIT ITS INODE COPY**, which another call
// may have changed while the lock was down -- an append's size, or a
// truncate. So it re-reads the inode at the end and changes only the
// time. And after each gap: an inode freed anywhere on the volume means
// this number may now be somebody else's file, so the write fails; a
// block freed means this copy may be stale, so it re-reads and checks
// the rest of the range is still an overwrite, or fails.
static int do_overwrite(struct t3_state *sbi, uint64_t ino, struct t3_inode *node,
                        uint64_t offset, const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t freed = sbi->free_gen, ino_freed = sbi->ino_free_gen, staged = sbi->ino_gen;
    uint32_t total = 0;
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;
        uint32_t blk;
        if (!block_for_index(sbi, node, bi, &blk) || !blk) return 0;
        if (chunk == T3_BLOCK) {
            uint32_t run = 1;
            uint32_t want = (len - total) / T3_BLOCK;
            uint32_t cap = (uint32_t)blkdev_max_sectors_per_xfer(sbi->vol.dev) / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                uint32_t nxt;
                if (!block_for_index(sbi, node, bi + run, &nxt) || nxt != blk + run) break;
                run++;
            }
            int r = vol_write_run(sbi, blk * T3_SPB, (int)(run * T3_SPB), src + total);
            if (r <= 0) return 0;          // failed, or unmounted: sbi may be gone
            total += run * T3_BLOCK;
            if (sbi->ino_free_gen != ino_freed) return 0;
            if (sbi->free_gen != freed) {
                freed = sbi->free_gen;
                if (!read_inode(sbi, ino, node) || node->type != T3_TYPE_FILE) return 0;
                if (total < len && !is_overwrite(sbi, node, offset + total, len - total)) return 0;
            }
        } else {
            if (!read_block(sbi, blk, sbi->blk)) return 0;
            k_memcpy(sbi->blk + within, src + total, chunk);
            if (!write_block(sbi, blk, sbi->blk)) return 0;
            total += chunk;
        }
    }
    if (sbi->ino_gen != staged && (!read_inode(sbi, ino, node) || node->type != T3_TYPE_FILE))
        return 0;
    node->modified = now_epoch();
    return stage_inode_update(sbi, ino, node);
}

// The rollback shell: a runtime failure (refused write, out of space
// midway) frees everything this call allocated -- see the alloc-log
// comment. Only a CRASH is allowed to cost a leak.
static int do_write(struct t3_state *sbi, uint64_t ino, struct t3_inode *node, uint64_t offset,
                    const void *buf, uint32_t len) {
    // BEFORE anything is allocated: a refusal that came after would
    // leave the blocks behind for a write that never happened.
    if (!t3_range_fits(offset, len)) return 0;
    // NOTHING TO ROLL BACK, AND NO SHARED LOG TO HOLD across the gaps it
    // may open: alog is per mount, and another call would reset it.
    if (is_overwrite(sbi, node, offset, len)) return do_overwrite(sbi, ino, node, offset, buf, len);
    alog_begin(sbi);
    int ok = do_write_inner(sbi, ino, node, offset, buf, len);
    if (ok) {
        alog_commit(sbi);
    } else {
        pcache_drop(sbi);
        alog_rollback(sbi);
    }
    return ok;
}

// Free every data + pointer block behind *node (for truncate/delete),
// TRIMming as it goes, and leave the pointer fields zeroed. The
// caller must have ALREADY committed the inode/dirent transaction
// that makes these blocks unreachable -- clear-after-persist.
static void free_tree_level(struct t3_state *sbi, uint32_t table_blk, int depth);

static void free_all_blocks(struct t3_state *sbi, struct t3_inode *node) {
    uint32_t run_start = 0, run_len = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t blk = node->ptrs[i];
        if (blk) {
            free_block_bit(sbi, blk);
            if (run_len && blk == run_start + run_len) run_len++;
            else { trim_run(sbi, run_start, run_len); run_start = blk; run_len = 1; }
        }
        node->ptrs[i] = 0;
    }
    trim_run(sbi, run_start, run_len);
    if (node->ptrs[12]) { free_tree_level(sbi, node->ptrs[12], 0); node->ptrs[12] = 0; }
    if (node->ptrs[13]) { free_tree_level(sbi, node->ptrs[13], 1); node->ptrs[13] = 0; }
    if (node->ptrs[14]) { free_tree_level(sbi, node->ptrs[14], 2); node->ptrs[14] = 0; }
    trim_flush(sbi);
}

// depth 0: entries are data blocks; deeper: entries are tables.
// Recursion depth is bounded at 3 by the format itself. Uses a local
// table copy (kmalloc) instead of the shared scratch because levels
// nest.
static void free_tree_level(struct t3_state *sbi, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl) return; // leak rather than corrupt -- fsck reclaims
    if (read_block(sbi, table_blk, tbl)) {
        uint32_t run_start = 0, run_len = 0;
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(tbl + i * 4);
            if (!e) continue;
            if (depth == 0) {
                free_block_bit(sbi, e);
                if (run_len && e == run_start + run_len) run_len++;
                else { trim_run(sbi, run_start, run_len); run_start = e; run_len = 1; }
            } else {
                free_tree_level(sbi, e, depth - 1);
            }
        }
        trim_run(sbi, run_start, run_len);
    }
    free_block_bit(sbi, table_blk);
    trim_run(sbi, table_blk, 1);
    kfree(tbl);
}

// ---- partial truncation ----------------------------------------------------
//
// Shrinking has to obey the same ordering everything else here does:
// the inode that stops referencing a block must be DURABLE before that
// block's bit is freed, or a crash in between leaves a live file
// pointing at space the allocator is free to hand to someone else --
// the double allocation this filesystem's whole discipline exists to
// make impossible. (A leak in the other direction is fine; fsck
// reclaims it.)
//
// That splits truncation into two phases with a commit between them,
// and the phases can't both walk the pointer tables from disk, because
// phase one rewrites them. What saves it is that the tail being
// dropped is a clean cut: at every level, entries are either wholly
// dropped or wholly kept, EXCEPT for at most one straddling entry --
// so there is at most one partially-rewritten table per level, three
// in the deepest case. Keeping those three original images in memory
// (12 KiB) is enough for phase two to walk everything phase one
// detached.
struct t3_trunc {
    uint8_t *img[3];   // original image of the boundary table at each depth
    uint32_t blk[3];   // where it lives
    uint32_t from[3];  // table-relative index of the cut
    int used[3];
    int emptied[3];    // nothing kept -- the table block goes too
    uint32_t ptrs[15]; // the inode's original pointers
};

// Phase one at one level: zero every entry covering table-relative
// index >= `from`, write the table back, and record the original for
// phase two. Returns 1 if the table kept nothing (the caller drops the
// entry pointing at it), 0 otherwise -- including on any allocation or
// I/O failure, where keeping the tail attached is the safe answer.
static int trunc_detach(struct t3_state *sbi, struct t3_trunc *tr, uint32_t table_blk, int depth, uint32_t from) {
    if (depth < 0 || depth > 2 || tr->used[depth]) return 0; // one boundary per level, by construction
    uint8_t *orig = kmalloc(T3_BLOCK);
    if (!orig) return 0;
    if (!read_block(sbi, table_blk, orig)) { kfree(orig); return 0; }

    uint8_t *edit = kmalloc(T3_BLOCK);
    if (!edit) { kfree(orig); return 0; }
    k_memcpy(edit, orig, T3_BLOCK);

    uint32_t span = 1;
    for (int d = 0; d < depth; d++) span *= T3_PTRS_PER_BLOCK;

    int empty = 1, dirty = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(orig + i * 4);
        if (!e) continue;
        uint32_t start = i * span; // first file-block index this entry covers
        if (start + span <= from) { empty = 0; continue; }      // wholly kept
        if (start >= from) { wr32(edit + i * 4, 0); dirty = 1; } // wholly dropped
        else if (trunc_detach(sbi, tr, e, depth - 1, from - start)) { // straddles
            wr32(edit + i * 4, 0); dirty = 1;
        } else {
            empty = 0;
        }
    }
    // An emptied table is dropped whole by the caller, so writing it
    // would be a write to a block about to be freed.
    if (dirty && !empty && !write_block(sbi, table_blk, edit)) empty = 0;
    map_cache_forget(sbi, table_blk);   // edited behind the write caches' backs
    kfree(edit);

    tr->img[depth] = orig;
    tr->blk[depth] = table_blk;
    tr->from[depth] = from;
    tr->used[depth] = 1;
    tr->emptied[depth] = empty;
    return empty;
}

// Detach every block holding file-block index >= `first` from *node
// (in memory and, for boundary tables, on disk). The caller must
// commit the inode before calling trunc_free().
static void trunc_begin(struct t3_state *sbi, struct t3_trunc *tr, struct t3_inode *node, uint32_t first) {
    k_memset(tr, 0, sizeof(*tr));
    for (int i = 0; i < 15; i++) tr->ptrs[i] = node->ptrs[i];

    for (uint32_t i = first; i < 12; i++) node->ptrs[i] = 0;

    uint32_t base = 12, span = T3_PTRS_PER_BLOCK;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t slot = 12 + (uint32_t)lvl;
        uint32_t blk = node->ptrs[slot];
        if (blk) {
            if (first <= base) node->ptrs[slot] = 0;                 // whole level dropped
            else if (first < base + span &&
                     trunc_detach(sbi, tr, blk, lvl, first - base)) node->ptrs[slot] = 0;
        }
        base += span;
        span *= T3_PTRS_PER_BLOCK;
    }
}

// Phase two: return the detached blocks to the bitmap, walking the
// originals trunc_begin() kept. Every table read here is one phase one
// deliberately did NOT rewrite, so the disk still describes the
// subtree being freed.
static void trunc_free(struct t3_state *sbi, struct t3_trunc *tr, uint32_t first) {
    uint32_t run_start = 0, run_len = 0;
    for (uint32_t i = first; i < 12; i++) {
        uint32_t blk = tr->ptrs[i];
        if (!blk) continue;
        free_block_bit(sbi, blk);
        if (run_len && blk == run_start + run_len) run_len++;
        else { trim_run(sbi, run_start, run_len); run_start = blk; run_len = 1; }
    }
    trim_run(sbi, run_start, run_len);

    uint32_t base = 12, span = T3_PTRS_PER_BLOCK;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint32_t blk = tr->ptrs[12 + lvl];
        if (blk && first <= base) free_tree_level(sbi, blk, lvl); // frees the table too
        base += span;
        span *= T3_PTRS_PER_BLOCK;
    }

    for (int d = 2; d >= 0; d--) {
        if (!tr->used[d]) continue;
        uint32_t sp = 1;
        for (int k = 0; k < d; k++) sp *= T3_PTRS_PER_BLOCK;
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(tr->img[d] + i * 4);
            if (!e) continue;
            uint32_t start = i * sp;
            if (start < tr->from[d]) continue; // kept, or the next boundary down
            if (d == 0) { free_block_bit(sbi, e); trim_run(sbi, e, 1); }
            else free_tree_level(sbi, e, d - 1);
        }
        if (tr->emptied[d]) { free_block_bit(sbi, tr->blk[d]); trim_run(sbi, tr->blk[d], 1); }
        kfree(tr->img[d]);
        tr->img[d] = 0;
        tr->used[d] = 0;
    }
    trim_flush(sbi);
}

// ---- dirent editing (through the transaction) ------------------------------

// The transaction's view of a block, if it has one. Dirent editing
// must read through this: a rename stages an INSERT and then a REMOVE,
// and if the second read the block from disk it would compute its fold
// from record lengths the first had already changed -- swallowing the
// entry just written. (Found by reasoning about the same-directory
// case, where both edits land in one block.)
static const uint8_t *txn_peek(struct t3_state *sbi, uint32_t blk) {
    for (int i = 0; i < sbi->txn_count; i++) {
        if (sbi->txn_target[i] == blk) return sbi->txn_img[i];
    }
    return 0;
}

// Find room for a new dirent in `dir` and stage the patched block.
// Grows the directory by one block IN ITS OWN transaction first when
// needed (an empty extra dir block is harmless if the follow-up
// transaction never lands -- two consistent states, no 5-slot
// transaction; see the design doc's journal section).
static int dirent_insert(struct t3_state *sbi, uint64_t dir_ino, struct t3_inode *dir,
                         const char *name, uint32_t name_len, uint64_t child_ino) {
    uint32_t need = (7 + name_len + 3) & ~3u;
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            uint32_t used = e_ino ? ((7u + nl + 3u) & ~3u) : 0;
            if (rec_len - used >= need) {
                uint8_t *img = txn_stage(sbi, blk);
                if (!img) return 0;
                uint32_t new_off = off + used;
                if (e_ino) wr16(img + off + 4, (uint16_t)used);
                wr32(img + new_off, (uint32_t)child_ino);
                wr16(img + new_off + 4, (uint16_t)(rec_len - used));
                img[new_off + 6] = (uint8_t)name_len;
                k_memcpy(img + new_off + 7, name, name_len);
                return 1;
            }
            off += rec_len;
        }
    }

    // No room: grow the directory by one EMPTY block in its own
    // transaction, then stage the insertion into it as part of the
    // caller's transaction. The empty-first split is load-bearing:
    // an early version wrote the child's entry INTO the grow block,
    // which made the name visible one transaction before the child's
    // inode existed -- exactly the namespace-corruption window the
    // journal is for. An empty extra block, by contrast, is harmless
    // slack if the caller's transaction never lands.
    if (nblocks >= 12 + T3_PTRS_PER_BLOCK) return 0; // dirs stop at single-indirect scale
    if (sbi->txn_count != 0) return 0; // grow needs its own txn; callers stage after insert only
    uint32_t prefer_group = (uint32_t)(dir_ino / sbi->sb.ipg);
    uint32_t newblk = alloc_block(sbi, prefer_group, 0);
    if (!newblk) return 0;
    k_memset(sbi->blk, 0, T3_BLOCK);
    wr16(sbi->blk + 4, (uint16_t)T3_BLOCK); // one free entry spanning the block
    if (!write_block(sbi, newblk, sbi->blk)) { free_block_bit(sbi, newblk); return 0; }

    // Wire it into the map. Directs cover 12 blocks; past that the
    // single-indirect table gets the pointer (RMW, no pcache needed
    // at dir scale).
    if (nblocks < 12) {
        dir->ptrs[nblocks] = newblk;
    } else {
        uint32_t table = dir->ptrs[12];
        int fresh = 0;
        if (!table) {
            table = alloc_block(sbi, prefer_group, 0);
            if (!table) { free_block_bit(sbi, newblk); return 0; }
            dir->ptrs[12] = table;
            fresh = 1;
        }
        if (fresh) k_memset(sbi->ptr_blk, 0, T3_BLOCK);
        else if (!read_block(sbi, table, sbi->ptr_blk)) { free_block_bit(sbi, newblk); return 0; }
        wr32(sbi->ptr_blk + (nblocks - 12) * 4, newblk);
        if (!write_block(sbi, table, sbi->ptr_blk)) { free_block_bit(sbi, newblk); return 0; }
        map_cache_forget(sbi, table);
    }
    dir->size += T3_BLOCK;
    dir->modified = now_epoch();
    if (!flush_alloc_state(sbi)) return 0;
    // The grow commits on its own, which clears the caller's
    // reservation -- put it back, since the caller is still mid-
    // operation and about to stage into the fresh block below.
    int saved_credits = sbi->txn_credits;
    if (!txn_begin(sbi, 1)) return 0;
    if (!txn_stage_inode(sbi, dir_ino, dir)) { txn_reset(sbi); sbi->txn_credits = saved_credits; return 0; }
    if (!txn_commit(sbi)) { sbi->txn_credits = saved_credits; return 0; }
    sbi->txn_credits = saved_credits;
    // The grow just COMMITTED: its blocks are referenced by the
    // parent inode now, so they must survive any rollback of the
    // caller's still-pending transaction.
    alog_forget(sbi, newblk);
    if (dir->ptrs[12]) alog_forget(sbi, dir->ptrs[12]);

    // Now stage the actual insertion into the fresh block, in the
    // caller's transaction.
    uint8_t *img = txn_stage(sbi, newblk);
    if (!img) return 0;
    wr32(img, (uint32_t)child_ino);
    wr16(img + 4, (uint16_t)T3_BLOCK);
    img[6] = (uint8_t)name_len;
    k_memcpy(img + 7, name, name_len);
    return 1;
}

// Stage the removal of `name` from `dir` (ext2-style fold into the
// previous entry). Returns the removed entry's inode via *out_child,
// 0 on not-found/failure.
static int dirent_remove(struct t3_state *sbi, struct t3_inode *dir, const char *name,
                         uint32_t name_len, uint64_t *out_child) {
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0, prev_off = 0;
        int have_prev = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            if (e_ino && nl == name_len && k_memcmp(cur + off + 7, name, name_len) == 0) {
                uint8_t *img = txn_stage(sbi, blk);
                if (!img) return 0;
                if (have_prev) {
                    uint16_t p_len = rd16(img + prev_off + 4);
                    wr16(img + prev_off + 4, (uint16_t)(p_len + rec_len));
                } else {
                    wr32(img + off, 0);
                    img[off + 6] = 0;
                }
                *out_child = e_ino;
                return 1;
            }
            prev_off = off; have_prev = 1;
            off += rec_len;
        }
    }
    return 0;
}

// Split a normalized path into (parent inode, final component).
static int split_parent(struct t3_state *sbi, const char *norm, uint64_t *out_parent,
                        const char **out_name, uint32_t *out_len) {
    if (k_strcmp(norm, "/") == 0) return 0;
    const char *last = norm;
    for (const char *p = norm; *p; p++) if (*p == '/') last = p;
    uint32_t len = (uint32_t)k_strlen(last + 1);
    if (len == 0 || len > T3_NAME_MAX) return 0;
    char *const parent = sbi->pb.split_parent_parent; // per-function, see normalize()
    if (last == norm) { parent[0] = '/'; parent[1] = '\0'; }
    else {
        uint32_t plen = (uint32_t)(last - norm);
        if (plen >= T3_PATH_BUF) return 0;
        k_memcpy(parent, norm, plen);
        parent[plen] = '\0';
    }
    uint64_t pino = T3_INO_ROOT;
    if (k_strcmp(parent, "/") != 0 && !resolve(sbi, parent, &pino)) return 0;
    *out_parent = pino;
    *out_name = last + 1;
    *out_len = len;
    return 1;
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
    int ok = vol_write_sectors(sbi, T3_SB_BLOCK * T3_SPB, 1, zero);
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
        int n = backup_groups(gc, groups);
        for (int i = 0; i < n; i++) {
            uint32_t gbase = group0 + groups[i] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t blk = gbase + gspan - 1;
            if (!vol_write_sectors(sbi, blk * T3_SPB, 1, zero)) ok = 0;
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

    // Publish the geometry BEFORE anything uses it. group_span() and
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
    int nb = backup_groups(gc, groups);
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
    if (!write_block(sbi, T3_SB_BLOCK, sbi->blk)) return 0;

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
            int on = backup_groups(old_gc, old_groups);
            for (int i = 0; i < on; i++) {
                uint32_t gbase = old0 + old_groups[i] * T3_BPG;
                uint32_t gspan = vol_blocks - gbase;
                if (gspan > T3_BPG) gspan = T3_BPG;
                vol_write_sectors(sbi, (gbase + gspan - 1) * T3_SPB, 1, zero);
            }
        }
    }

    // Empty journal header + zeroed image slots.
    k_memset(sbi->blk, 0, T3_BLOCK);
    sbi->blk[0] = 'J'; sbi->blk[1] = 'R'; sbi->blk[2] = 'N'; sbi->blk[3] = '3';
    wr32(sbi->blk + jh_cksum_off(T3_VERSION), k_fnv1a(sbi->blk, jh_cksum_off(T3_VERSION)));
    if (!write_block(sbi, T3_JH_BLOCK, sbi->blk)) return 0;
    k_memset(sbi->blk, 0, T3_BLOCK);
    for (uint32_t i = 0; i < sbi->jslots; i++) {
        if (!write_block(sbi, sbi->jdata_block + i, sbi->blk)) return 0;
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
        if (!write_block(sbi, sbi->gdt_block + tb, sbi->blk)) return 0;
        // Backup GDT snapshots get the identical block.
        for (int j = 0; j < nb; j++) {
            uint32_t gbase = sbi->group0 + groups[j] * T3_BPG;
            uint32_t gspan = vol_blocks - gbase;
            if (gspan > T3_BPG) gspan = T3_BPG;
            uint32_t tail = gbase + gspan - 1;
            if (!write_block(sbi, tail - T3_GDT_BLOCKS + tb, sbi->blk)) return 0;
        }
    }

    // Per group: block bitmap, inode bitmap, zeroed inode table.
    for (uint32_t g = 0; g < gc; g++) {
        uint32_t base = group_base(sbi, g);
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
        if (!write_block(sbi, base, sbi->blk)) return 0;

        k_memset(sbi->blk, 0, T3_BLOCK);
        if (g == 0) sbi->blk[0] |= 0x03; // ino 0 (null) + ino 1 (root)
        if (!write_block(sbi, base + 1, sbi->blk)) return 0;

        // Zero the inode table: a zero inode fails its checksum on
        // purpose, and stale-but-valid inodes from a previous TFS3
        // format must not survive into this one.
        k_memset(sbi->blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < itb; i++) {
            if (!write_block(sbi, base + 2 + i, sbi->blk)) return 0;
        }
    }

    // Root inode (ino 1, group 0) + its dirent block (. and ..).
    {
        uint8_t sec[T3_SECTOR];
        uint32_t table_lba = (sbi->group0 + 2) * T3_SPB;
        if (!vol_read_sectors(sbi, table_lba, 1, sec)) return 0;
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
        if (!vol_write_sectors(sbi, table_lba, 1, sec)) return 0;

        k_memset(sbi->blk, 0, T3_BLOCK);
        wr32(sbi->blk + 0, T3_INO_ROOT); wr16(sbi->blk + 4, 12); sbi->blk[6] = 1; sbi->blk[7] = '.';
        wr32(sbi->blk + 12, T3_INO_ROOT); wr16(sbi->blk + 16, (uint16_t)(T3_BLOCK - 12)); sbi->blk[18] = 2;
        sbi->blk[19] = '.'; sbi->blk[20] = '.';
        if (!write_block(sbi, root_block, sbi->blk)) return 0;
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
        if (!write_block(sbi, gbase + gspan - 1, sbi->blk)) return 0;
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
        txn_flush_deferred(sbi);
        // A flush that failed here has nowhere left to retry: the state
        // it belongs to is about to go.
        sbi->txn_deferred = 0;
    }
    if (sbi->gd) { kfree(sbi->gd); sbi->gd = 0; }
    if (sbi->bbm) { kfree(sbi->bbm); sbi->bbm = 0; }
    if (sbi->ibm) { kfree(sbi->ibm); sbi->ibm = 0; }
    if (sbi->rotor) { kfree(sbi->rotor); sbi->rotor = 0; }
    ncache_flush(sbi);
    pcache_drop(sbi);
    txn_reset(sbi);
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
    if (!replay_journal(sbi))
        vol_go_readonly(sbi, "a committed journal transaction could not be replayed");

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
        if (!read_block(sbi, group_base(sbi, g), sbi->bbm + (size_t)g * T3_BLOCK) ||
            !read_block(sbi, group_base(sbi, g) + 1, sbi->ibm + (size_t)g * T3_BLOCK)) {
            klog_write(KLOG_ERR "tfs3: bitmap read failed -- not mounted\n");
            unmount_state(sbi);
            return -1;
        }
    }
    uint32_t bad_gd = 0;
    for (uint32_t tb = 0; tb <= (sbi->sb.gc - 1) / (T3_BLOCK / 16); tb++) {
        if (!read_block(sbi, sbi->gdt_block + tb, sbi->blk)) { kfree(sbi->gd); sbi->gd = 0; return -1; }
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
    if (!read_inode(sbi, T3_INO_ROOT, &root) || root.type != T3_TYPE_DIR) {
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
        if (!block_for_index(sbi, node, bi, &blk)) break;
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
                if (!block_for_index(sbi, node, bi + run, &nxt) || nxt != blk + run) break;
                run++;
            }
            // THE ONE PLACE THE LOCK MAY DROP: a whole-block run straight
            // into the caller's buffer (vol_read_run()). After it, a freed
            // block anywhere on the volume means this inode copy may no
            // longer describe the file -- stop SHORT and let the caller's
            // next call look it up again. The run itself is good: frees
            // wait for it (free_block_bit()).
            uint64_t freed = sbi->free_gen;
            int r = vol_read_run(sbi, blk * T3_SPB, (int)(run * T3_SPB), dst + total);
            if (r < 0) return total;       // unmounted meanwhile: sbi is gone
            if (!r) break;
            total += run * T3_BLOCK;
            if (sbi->free_gen != freed) break;
            continue;
        } else {
            if (!read_block(sbi, blk, sbi->blk)) break;
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
    char *const norm = sbi->pb.tfs3_is_dir_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    struct t3_inode node;
    uint64_t ino;
    if (!lookup(sbi, path, &ino, &node)) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    return node.type == T3_TYPE_DIR;
}

static int tfs3_exists(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_exists_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    uint64_t ino;
    return resolve(sbi, norm, &ino) ? 1 : 0;
}

static void tfs3_list(void *st, const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    struct t3_state *sbi = st;
    struct t3_inode dir;
    char *const norm = sbi->pb.tfs3_list_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, dir_path, norm)) return;
    uint64_t ino = T3_INO_ROOT;
    if (k_strcmp(norm, "/") != 0 && !resolve(sbi, norm, &ino)) return;
    if (!t3_lock(sbi, ino, 0)) return;
    if (!read_inode(sbi, ino, &dir) || dir.type != T3_TYPE_DIR) return;

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        // Copy the dirent block out of blk before per-child inode
        // reads reuse the scratch.
        uint8_t *const dirblk = sbi->dirblk;
        if (!block_for_index(sbi, &dir, b, &blk) || !blk || !read_block(sbi, blk, dirblk)) return;
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
                    if (read_inode(sbi, e_ino, &child)) {
                        uint32_t sz = child.type == T3_TYPE_DIR ? 0
                                     : (child.size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)child.size);
                        cb(name, sz, child.type == T3_TYPE_DIR);
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
    char *const norm = sbi->pb.tfs3_chmod_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return -ENOENT;
    if (k_strcmp(norm, "/") == 0) return -ENOENT;   // root has no entry
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) return -ENOENT;
    if (!t3_lock(sbi, ino, 1)) return -EIO;   // or run again -- see t3_lock()
    if (!read_inode(sbi, ino, &node)) return -ENOENT;
    if ((node.mode & 07777) == (mode & 07777)) return 0;  // already so
    node.mode = (uint16_t)(mode & 07777);
    // THROUGH THE JOURNAL, like every other inode change: a mode is a
    // metadata write and gets the same crash-safety as a rename.
    if (!txn_begin(sbi, 1)) return -EIO;
    if (!txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); return -EIO; }
    return txn_commit(sbi) ? 0 : -EIO;
}

static int tfs3_stat(void *st, const char *path, struct fs_stat_info *out) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_stat_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0; // root has no entry
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) return 0;
    if (!t3_lock(sbi, ino, 0)) return 0;
    if (!read_inode(sbi, ino, &node)) return 0;
    if (out) {
        out->ino = ino;             // a real inode number -- FS_CAP_INODES
        out->created = node.created; // stored as epoch natively -- FS_CAP_EPOCH_TIME
        out->modified = node.modified;
        // A zero on disk is an inode written before the field existed,
        // not a file nobody may touch -- see struct t3_inode.
        out->mode = node.mode ? node.mode : (uint16_t)T3_MODE_DEFAULT(node.type);
        out->nlink = node.links;
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
    int nb = backup_groups(sbi->sb.gc, groups);
    uint64_t total_data = 0, free_data = 0;
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        // The group's REAL extent -- the last one may be partial, and
        // using T3_BPG here made `df` report a 16 MiB volume as 127 MB.
        // A size that lies is worse than no size at all.
        uint32_t span = group_span(sbi, g);
        uint32_t data = span > sbi->meta_off ? span - sbi->meta_off : 0;
        for (int j = 0; j < nb; j++) if (groups[j] == g) data -= T3_BACKUP_BLOCKS;
        total_data += data;
        free_data += sbi->gd[g].free_blocks;
    }
    if (out_total) *out_total = total_data * T3_BLOCK;
    if (out_used) *out_used = (total_data - free_data) * T3_BLOCK;
    return 1;
}

// ---- mutating ops ---------------------------------------------------------

// Create a file or directory entry under an existing parent.
// Transaction: parent dirent block + child inode block (+ the child's
// own `.`/`..` dirent block is DATA, written before anything points
// at it) + parent inode block for mkdir's link-count bump. <= 3
// slots; directory growth runs as its own transaction inside
// dirent_insert() (see its comment).
static int create_entry_inner(struct t3_state *sbi, const char *path, uint8_t type, uint64_t *out_ino);

static int create_entry(struct t3_state *sbi, const char *path, uint8_t type, uint64_t *out_ino) {
    alog_begin(sbi);
    int ok = create_entry_inner(sbi, path, type, out_ino);
    if (ok) alog_commit(sbi); else alog_rollback(sbi);
    return ok;
}

static int create_entry_inner(struct t3_state *sbi, const char *path, uint8_t type, uint64_t *out_ino) {
    char *const norm = sbi->pb.create_entry_inner_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t existing;
    if (resolve(sbi, norm, &existing)) return 0; // caller decides what exists means
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(sbi, norm, &parent_ino, &name, &name_len)) return 0;
    if (!t3_lock(sbi, parent_ino, 1)) return 0;   // a namespace change: the parent
    struct t3_inode parent;
    if (!read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    uint32_t prefer_group = (uint32_t)(parent_ino / sbi->sb.ipg);
    uint64_t ino = alloc_inode(sbi, prefer_group);
    if (!ino) return 0;
    // Block allocations below roll back via the alloc log (armed by
    // the create_entry() shell); the inode bit is freed by hand on
    // each failure path since the log only tracks blocks.

    struct t3_inode node;
    k_memset(&node, 0, sizeof(node));
    node.type = type;
    node.links = (type == T3_TYPE_DIR) ? 2 : 1;
    node.created = node.modified = now_epoch();
    // A NEW inode records its mode explicitly rather than leaning on the
    // read-time default, so that a later chmod has somewhere to write
    // and so an inode's stored mode means what it says.
    node.mode = T3_MODE_DEFAULT(type);

    if (type == T3_TYPE_DIR) {
        // The child's own dirent block: plain data until the inode
        // transaction lands, so a direct (unjournaled) write is safe.
        uint32_t blk = alloc_block(sbi, prefer_group, 0);
        if (!blk) { free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
        k_memset(sbi->blk, 0, T3_BLOCK);
        wr32(sbi->blk, (uint32_t)ino); wr16(sbi->blk + 4, 12); sbi->blk[6] = 1; sbi->blk[7] = '.';
        wr32(sbi->blk + 12, (uint32_t)parent_ino); wr16(sbi->blk + 16, (uint16_t)(T3_BLOCK - 12));
        sbi->blk[18] = 2; sbi->blk[19] = '.'; sbi->blk[20] = '.';
        if (!write_block(sbi, blk, sbi->blk)) { free_inode_bit(sbi, ino); free_block_bit(sbi, blk); flush_alloc_state(sbi); return 0; }
        node.ptrs[0] = blk;
        node.size = T3_BLOCK;
    }

    if (!flush_alloc_state(sbi)) { free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; } // set-before-use

    // dirent block + the new inode + (for a directory) the parent's
    // link count.
    if (!txn_begin(sbi, 3)) { free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
    int ins = dirent_insert(sbi, parent_ino, &parent, name, name_len, ino);
    if (!ins) { txn_reset(sbi); free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
    if (!txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
    if (type == T3_TYPE_DIR) {
        parent.links++;
        parent.modified = node.created;
        if (!txn_stage_inode(sbi, parent_ino, &parent)) { txn_reset(sbi); free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
    }
    if (!txn_commit(sbi)) { free_inode_bit(sbi, ino); flush_alloc_state(sbi); return 0; }
    ncache_flush(sbi);
    if (out_ino) *out_ino = ino;
    return 1;
}

static int tfs3_touch(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_touch_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (resolve(sbi, norm, &ino)) {
        // Existing file: a no-op that succeeds; existing dir: refuse.
        // Matches tfs_touch()'s behavior exactly (incl. not bumping
        // `modified` -- see fs.h's fs_stat_info comment).
        if (!t3_lock(sbi, ino, 0)) return 0;
        if (!read_inode(sbi, ino, &node)) return 0;
        return node.type == T3_TYPE_FILE;
    }
    return create_entry(sbi, path, T3_TYPE_FILE, 0);
}

static int tfs3_mkdir(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_mkdir_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    if (resolve(sbi, norm, &ino)) return 0; // exists (file OR dir) -- refuse
    return create_entry(sbi, path, T3_TYPE_DIR, 0);
}

static int tfs3_write_range(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_range_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!read_inode(sbi, ino, &node) || node.type != T3_TYPE_FILE) return 0;
    if (len == 0) return 1;
    return do_write(sbi, ino, &node, offset, buf, len);
}

static int tfs3_write(void *st, const char *path, const char *data, int append) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!read_inode(sbi, ino, &node) || node.type != T3_TYPE_FILE) return 0;

    uint32_t len = (uint32_t)k_strlen(data);
    uint64_t start = node.size;
    if (!append && node.size > 0) {
        // Truncate: commit the emptied inode FIRST, then return the
        // old blocks -- tfs_write()'s detach-then-persist ordering,
        // expressed in TFS3 terms (clear-after-persist; a crash
        // between the two steps leaks, fsck reclaims).
        struct t3_inode old = node;
        k_memset(node.ptrs, 0, sizeof(node.ptrs));
        node.size = 0;
        node.modified = now_epoch();
        if (!txn_begin(sbi, 1)) return 0;
        if (!txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); return 0; }
        if (!txn_commit(sbi)) return 0;
        free_all_blocks(sbi, &old);
        flush_alloc_state(sbi);
        start = 0;
    } else if (!append) {
        start = 0;
    }
    if (len == 0) return 1;
    return do_write(sbi, ino, &node, start, data, len);
}

static int tfs3_delete(void *st, const char *path) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_delete_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;
    uint64_t ino, del_pino;
    const char *del_name;
    uint32_t del_len;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) return 0;
    if (!split_parent(sbi, norm, &del_pino, &del_name, &del_len)) return 0;
    if (!t3_lock(sbi, del_pino, 1) || !t3_lock(sbi, ino, 1)) return 0;
    if (!read_inode(sbi, ino, &node)) return 0;

    if (node.type == T3_TYPE_DIR) {
        // Empty means "nothing but . and .." -- the no-recursive-
        // delete policy, unchanged (docs/decisions.md).
        uint32_t nblocks = (uint32_t)((node.size + T3_BLOCK - 1) / T3_BLOCK);
        for (uint32_t b = 0; b < nblocks; b++) {
            uint32_t blk;
            if (!block_for_index(sbi, &node, b, &blk) || !blk || !read_block(sbi, blk, sbi->blk)) return 0;
            uint32_t off = 0;
            while (off + 8 <= T3_BLOCK) {
                uint32_t e_ino = rd32(sbi->blk + off);
                uint16_t rec_len = rd16(sbi->blk + off + 4);
                uint8_t nl = sbi->blk[off + 6];
                if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
                if (e_ino && !(nl == 1 && sbi->blk[off + 7] == '.') &&
                    !(nl == 2 && sbi->blk[off + 7] == '.' && sbi->blk[off + 8] == '.')) {
                    return 0; // not empty
                }
                off += rec_len;
            }
        }
    }

    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(sbi, norm, &parent_ino, &name, &name_len)) return 0;
    struct t3_inode parent;
    if (!read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    // dirent block + the inode + (for a directory) the parent's link
    // count.
    if (!txn_begin(sbi, 3)) return 0;
    uint64_t removed = 0;
    if (!dirent_remove(sbi, &parent, name, name_len, &removed) || removed != ino) {
        txn_reset(sbi);
        return 0;
    }

    int gone = 0;
    if (node.type == T3_TYPE_DIR || node.links <= 1) {
        // Last name: zero the inode; blocks are freed after commit.
        if (!txn_stage_inode(sbi, ino, 0)) { txn_reset(sbi); return 0; }
        gone = 1;
        if (node.type == T3_TYPE_DIR) {
            parent.links--;
            parent.modified = now_epoch();
            if (!txn_stage_inode(sbi, parent_ino, &parent)) { txn_reset(sbi); return 0; }
        }
    } else {
        // A hardlink remains -- just drop the count.
        node.links--;
        if (!txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); return 0; }
    }
    if (!txn_commit(sbi)) return 0;
    ncache_flush(sbi);

    if (gone) {
        // clear-after-persist: nothing references these anymore.
        free_all_blocks(sbi, &node);
        free_inode_bit(sbi, ino);
        flush_alloc_state(sbi);
    }
    return 1;
}

// The first OPTIONAL fs_ops op, gated by FS_CAP_HARDLINKS (the caps
// honesty check in vfs.c verifies the pair). Files only -- hardlinked
// directories turn the tree into a graph, refused by every real Unix
// filesystem for the same reason (see the design doc).
static int tfs3_link(void *st, const char *existing, const char *newpath) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_link_norm;
    char *const newnorm = sbi->pb.tfs3_link_newnorm; // see normalize()
    if (!sbi->mounted || !normalize(sbi, existing, norm) || !normalize(sbi, newpath, newnorm)) return 0;
    uint64_t ino, clash;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino) || !read_inode(sbi, ino, &node)) return 0;
    if (node.type != T3_TYPE_FILE) return 0;
    if (resolve(sbi, newnorm, &clash)) return 0; // target name taken
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(sbi, newnorm, &parent_ino, &name, &name_len)) return 0;
    if (!t3_lock(sbi, parent_ino, 1) || !t3_lock(sbi, ino, 1)) return 0;
    if (!read_inode(sbi, ino, &node)) return 0;   // under the lock
    struct t3_inode parent;
    if (!read_inode(sbi, parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    alog_begin(sbi); // dir growth inside dirent_insert can allocate
    if (!txn_begin(sbi, 2)) { alog_rollback(sbi); return 0; } // dirent block + the inode's link count
    if (!dirent_insert(sbi, parent_ino, &parent, name, name_len, ino)) { txn_reset(sbi); alog_rollback(sbi); return 0; }
    node.links++;
    if (!txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); alog_rollback(sbi); return 0; }
    if (!txn_commit(sbi)) { alog_rollback(sbi); return 0; }
    alog_commit(sbi);
    ncache_flush(sbi);
    return 1;
}

// Stage a change to an EXISTING entry's inode number (rename's ".."
// fixup). Same scan as dirent_remove(), different patch.
static int dirent_repoint(struct t3_state *sbi, struct t3_inode *dir, const char *name,
                          uint32_t name_len, uint64_t to) {
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!block_for_index(sbi, dir, b, &blk) || !blk) return 0;
        const uint8_t *cur = txn_peek(sbi, blk);
        if (!cur) {
            if (!read_block(sbi, blk, sbi->blk)) return 0;
            cur = sbi->blk;
        }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(cur + off);
            uint16_t rec_len = rd16(cur + off + 4);
            uint8_t nl = cur[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            if (e_ino && nl == name_len && k_memcmp(cur + off + 7, name, name_len) == 0) {
                uint8_t *img = txn_stage(sbi, blk);
                if (!img) return 0;
                wr32(img + off, (uint32_t)to);
                return 1;
            }
            off += rec_len;
        }
    }
    return 0;
}

// True if `child` names something at or below `parent` -- the check
// that stops `mv /a /a/b`, which would otherwise detach a whole
// subtree into a cycle referenced by nothing. Both are normalized
// absolute paths and this filesystem has no symlinks or hardlinked
// directories, so a string prefix IS the tree relation.
static int path_is_within(const char *parent, const char *child) {
    uint32_t plen = (uint32_t)k_strlen(parent);
    if (k_strcmp(parent, "/") == 0) return 1;
    if (k_strncmp(child, parent, plen) != 0) return 0;
    return child[plen] == '\0' || child[plen] == '/';
}

// Rename/move, in ONE journal transaction: the namespace never shows
// both names or neither. Refuses an existing destination (fs.h's
// contract -- an atomic replace is a bigger operation and a separate
// decision), a directory moved into its own subtree, and the root.
//
// Credit accounting is the interesting part, and it is why v2's larger
// journal exists. Worst case is a directory changing parents: both
// dirent blocks, the child's ".." block, and both parents' link
// counts -- five. Every other shape needs three or four, which is why
// a v1 image can still rename freely and only refuses that one case,
// with a message, instead of failing halfway.
static int tfs3_rename(void *st, const char *oldpath, const char *newpath) {
    struct t3_state *sbi = st;
    char *const oldn = sbi->pb.tfs3_rename_oldn;
    char *const newn = sbi->pb.tfs3_rename_newn; // see normalize()
    if (!sbi->mounted || !normalize(sbi, oldpath, oldn) || !normalize(sbi, newpath, newn)) return 0;
    if (k_strcmp(oldn, "/") == 0 || k_strcmp(newn, "/") == 0) return 0;
    if (k_strcmp(oldn, newn) == 0) return 1; // renaming to itself changes nothing

    uint64_t ino, clash;
    struct t3_inode node;
    if (!resolve(sbi, oldn, &ino) || !read_inode(sbi, ino, &node)) return 0;
    if (resolve(sbi, newn, &clash)) return 0; // destination taken
    if (node.type == T3_TYPE_DIR && path_is_within(oldn, newn)) return 0;

    uint64_t src_pino, dst_pino;
    const char *src_name, *dst_name;
    uint32_t src_len, dst_len;
    if (!split_parent(sbi, oldn, &src_pino, &src_name, &src_len)) return 0;
    if (!split_parent(sbi, newn, &dst_pino, &dst_name, &dst_len)) return 0;
    // All three, taken as a set: none is held while waiting for another
    // (t3_lock()), so there is no order to keep and no rename mutex.
    if (!t3_lock(sbi, src_pino, 1) || !t3_lock(sbi, dst_pino, 1) || !t3_lock(sbi, ino, 1))
        return 0;
    if (!read_inode(sbi, ino, &node)) return 0;   // under the lock

    struct t3_inode src_parent, dst_parent;
    if (!read_inode(sbi, src_pino, &src_parent) || src_parent.type != T3_TYPE_DIR) return 0;
    if (!read_inode(sbi, dst_pino, &dst_parent) || dst_parent.type != T3_TYPE_DIR) return 0;

    int same_parent = (src_pino == dst_pino);
    int is_dir = (node.type == T3_TYPE_DIR);
    int credits = same_parent ? 3 : 4;
    if (is_dir && !same_parent) credits++; // the ".." fixup block
    if (credits > (int)sbi->jslots) {
        klog_write("tfs3: this volume's journal is too small to move a directory "
                   "between parents (v1 format) -- reformat with `fsformat tfs3 confirm`\n");
        return 0;
    }

    // The name buffer must outlive src_parent/dst_parent being mutated
    // below, and split_parent() hands back pointers INTO oldn/newn,
    // which do outlive it -- but the same-parent case aliases the two
    // struct copies, so take that fork explicitly rather than editing
    // two stale copies of one directory.
    struct t3_inode *dstp = same_parent ? &src_parent : &dst_parent;

    alog_begin(sbi); // a destination directory can grow inside dirent_insert
    if (!txn_begin(sbi, credits)) { alog_rollback(sbi); return 0; }

    // Insert FIRST: only the insert can need to grow a directory, and
    // a grow commits its own transaction, which it can only do while
    // nothing else is staged.
    if (!dirent_insert(sbi, dst_pino, dstp, dst_name, dst_len, ino)) {
        txn_reset(sbi); alog_rollback(sbi); return 0;
    }
    uint64_t removed = 0;
    if (!dirent_remove(sbi, &src_parent, src_name, src_len, &removed) || removed != ino) {
        txn_reset(sbi); alog_rollback(sbi); return 0;
    }

    uint64_t now = now_epoch();
    if (is_dir && !same_parent) {
        if (!dirent_repoint(sbi, &node, "..", 2, dst_pino)) { txn_reset(sbi); alog_rollback(sbi); return 0; }
        src_parent.links--;
        dst_parent.links++;
    }
    src_parent.modified = now;
    dstp->modified = now;
    if (!txn_stage_inode(sbi, src_pino, &src_parent)) { txn_reset(sbi); alog_rollback(sbi); return 0; }
    if (!same_parent && !txn_stage_inode(sbi, dst_pino, &dst_parent)) {
        txn_reset(sbi); alog_rollback(sbi); return 0;
    }
    if (!txn_commit(sbi)) { alog_rollback(sbi); return 0; }
    alog_commit(sbi);
    ncache_flush(sbi);
    return 1;
}

// Set a file's size exactly. Growing is SPARSE -- the size moves and
// the new range reads as zeros, which read_range_impl() already
// handles, so a 1 GB truncate costs one inode write and no blocks.
// Shrinking commits the smaller size FIRST and frees afterwards
// (clear-after-persist): a crash in between costs leaked blocks that
// fsck reclaims, never a live file pointing at freed space.
static int tfs3_truncate(void *st, const char *path, uint64_t size) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_truncate_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(sbi, norm, &ino)) return 0;
    if (!t3_lock(sbi, ino, 1)) return 0;
    if (!read_inode(sbi, ino, &node)) return 0;
    if (node.type != T3_TYPE_FILE) return 0; // directories size themselves
    if (size > T3_MAX_FILE_SIZE) return 0;   // a sparse grow past what the format addresses
    if (node.size == size) return 1;

    struct t3_trunc tr;
    int shrinking = (size < node.size);
    if (shrinking) {
        // ZERO THE RETAINED TAIL, or a shrink and regrow hands the old
        // bytes back: the final partial block survives the truncate, and
        // both a regrow and a write into the gap read it whole. ext4
        // zeroes the partial page in ext4_block_truncate_page() for the
        // same reason; NTFS tracks a valid-data-length instead, which
        // needs an inode field this format has not got.
        //
        // BEFORE the size commit, as ext4 orders it. The cost is that a
        // failed truncate may already have discarded the bytes it was
        // asked to discard -- cheaper than the alternative, where a
        // failed zeroing leaves them exposed with the size already moved.
        uint32_t tail = (uint32_t)(size % T3_BLOCK);
        if (tail) {
            uint32_t tblk;
            if (!block_for_index(sbi, &node, (uint32_t)(size / T3_BLOCK), &tblk)) return 0;
            if (tblk) {
                if (!read_block(sbi, tblk, sbi->blk)) return 0;
                k_memset(sbi->blk + tail, 0, T3_BLOCK - tail);
                if (!write_block(sbi, tblk, sbi->blk)) return 0;
            }
        }
        uint32_t first = (uint32_t)((size + T3_BLOCK - 1) / T3_BLOCK);
        trunc_begin(sbi, &tr, &node, first);
        node.size = size;
        node.modified = now_epoch();
        if (!txn_begin(sbi, 1) || !txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); return 0; }
        if (!txn_commit(sbi)) return 0;
        // Durable: nothing reachable references the tail any more.
        trunc_free(sbi, &tr, first);
        flush_alloc_state(sbi);
        return 1;
    }

    node.size = size;
    node.modified = now_epoch();
    if (!txn_begin(sbi, 1) || !txn_stage_inode(sbi, ino, &node)) { txn_reset(sbi); return 0; }
    return txn_commit(sbi);
}

// Steppable write: one block per step() call, inode committed once on
// the final step -- so a crash mid-stream leaks fresh blocks and
// leaves the file at its old size, same contract the blocking path
// gives (fs.h: on FS_STEP_DONE size/mtime/metadata are updated).
// FIELD BY FIELD, never memcmp: the struct has padding (after `type`
// and `links`) that read_inode() does not write, so two reads of one
// unchanged inode compare unequal.
static int inode_same(const struct t3_inode *a, const struct t3_inode *b) {
    if (a->type != b->type || a->links != b->links || a->size != b->size ||
        a->created != b->created || a->modified != b->modified || a->mode != b->mode)
        return 0;
    for (int i = 0; i < 15; i++) if (a->ptrs[i] != b->ptrs[i]) return 0;
    return 1;
}

struct t3_write_step {
    uint64_t ino;
    struct t3_inode node;
    struct t3_inode orig;      // the inode as begin() read it -- see the step
    const uint8_t *src;
    uint64_t offset;
    uint32_t len, total;
    uint32_t last_alloc;
};

static void *tfs3_write_range_begin(void *st, const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct t3_state *sbi = st;
    char *const norm = sbi->pb.tfs3_write_range_begin_norm; // per-function, see normalize()
    if (!sbi->mounted || !normalize(sbi, path, norm)) return 0;
    if (!t3_range_fits(offset, len)) return 0;
    uint64_t ino;
    if (!resolve(sbi, norm, &ino)) {
        if (!create_entry(sbi, path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!t3_lock(sbi, ino, 1)) return 0;
    struct t3_write_step *step = kmalloc(sizeof(*step));
    if (!step) return 0;
    if (!read_inode(sbi, ino, &step->node) || step->node.type != T3_TYPE_FILE) { kfree(step); return 0; }
    step->orig = step->node;
    step->ino = ino;
    step->src = (const uint8_t *)buf;
    step->offset = offset;
    step->len = len;
    step->total = 0;
    step->last_alloc = 0;
    pcache_drop(sbi);
    return step;
}

static int tfs3_write_range_step(void *st, void *handle) {
    struct t3_state *sbi = st;
    struct t3_write_step *step = (struct t3_write_step *)handle;
    // A STREAM SPANS SYSCALLS, and a lock may not (a process dying mid-
    // stream would keep it), so each step locks the file on its own. What
    // that leaves open is another writer between two steps -- and this
    // stream commits its inode COPY at the end, which would silently undo
    // the other write. So it checks the inode is still the one it began
    // from, and FAILS rather than lose somebody's data. Blocks from its
    // earlier steps leak, as an abandoned stream's always have; fsck
    // reclaims them.
    if (!t3_lock(sbi, step->ino, 1)) return 2 /* FS_STEP_FAILED -- or run again */;
    struct t3_inode cur;
    if (!read_inode(sbi, step->ino, &cur) || !inode_same(&cur, &step->orig)) {
        kfree(step);
        return 2 /* FS_STEP_FAILED */;
    }
    // Rollback scope is THIS STEP only -- other fs operations
    // interleave between steps of an async write, so a whole-stream
    // log can't be kept armed. Earlier completed steps of an
    // abandoned stream leak by design (crash-shaped); fsck reclaims.
    alog_begin(sbi);
    if (step->total < step->len) {
        uint64_t file_off = step->offset + step->total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > step->len - step->total) chunk = step->len - step->total;

        uint32_t prefer_group = (uint32_t)(step->ino / sbi->sb.ipg);
        uint32_t leaf_blk, leaf_slot, existing;
        if (!map_get_or_alloc_tables(sbi, &step->node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing)) {
            pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2 /* FS_STEP_FAILED */;
        }
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = alloc_block(sbi, prefer_group, step->last_alloc);
            if (!blk) { pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2; }
            map_set_block(sbi, &step->node, leaf_blk, leaf_slot, blk);
            fresh = 1;
        }
        step->last_alloc = blk;

        int ok;
        if (chunk == T3_BLOCK) {
            ok = write_block(sbi, blk, step->src + step->total);
        } else {
            if (!block_has_live_bytes(fresh, bi, step->node.size)) {
                k_memset(sbi->blk, 0, T3_BLOCK);
            } else if (!read_block(sbi, blk, sbi->blk)) {
                pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2;
            }
            k_memcpy(sbi->blk + within, step->src + step->total, chunk);
            ok = write_block(sbi, blk, sbi->blk);
        }
        if (!ok) { pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2; }
        step->total += chunk;
        // LANDED BEFORE RETURNING: other ops run between two steps, and
        // one that drops the pointer cache (another stream's begin())
        // would discard this stream's block pointers with it.
        if (step->total < step->len) {
            if (!pcache_flush(sbi)) { pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2; }
            alog_commit(sbi);
            return 0 /* FS_STEP_PENDING */;
        }
    }

    // Final step: land the pointer cache, the allocation state, and
    // the inode -- the same commit point do_write() has.
    if (!pcache_flush(sbi)) { pcache_drop(sbi); alog_rollback(sbi); kfree(step); return 2; }
    if (step->offset + step->len > step->node.size) step->node.size = step->offset + step->len;
    step->node.modified = now_epoch();
    int ok = flush_alloc_state(sbi);
    if (ok) {
        ok = txn_begin(sbi, 1) && txn_stage_inode(sbi, step->ino, &step->node) && txn_commit(sbi);
    }
    if (ok) alog_commit(sbi); else alog_rollback(sbi);
    kfree(step);
    return ok ? 1 /* FS_STEP_DONE */ : 2;
}

// ---- fsck -----------------------------------------------------------------
//
// One pass: walk the namespace from the root, mark every reachable
// block and inode, then reconcile against the allocation bitmaps.
// Result-field mapping keeps fs_check_result's TFS2-era meanings:
// records_used = reachable inodes, leaked/referenced_but_free/
// double_allocated/out_of_range = blocks, exactly as fs.h documents.
// Repair semantics follow the same rules as TFS2's: reclaim leaks
// (and TRIM them), re-mark referenced-but-free, zero out-of-range
// pointers, NEVER resolve a double allocation. TFS3 additions: link
// counts verified against observed name counts (repaired on a repair
// pass), free-count caches recomputed, and a primary superblock that
// was bad at mount (we ran from a backup) is rewritten -- on repair
// only, never automatically (see the design doc's backup rules).

struct t3_fsck {
    uint8_t *breach;     // block reachability, gc*4096
    uint8_t *ireach;     // inode reachability, gc*4096
    uint8_t *names;      // observed name count per inode, u8 saturating
    struct fs_check_result *r;
    int repair;
    int link_mismatches;
};

static int fsck_block_ok(struct t3_state *sbi, uint32_t blk) {
    if (blk < sbi->group0 + sbi->meta_off) return 0;
    uint32_t g = (blk - sbi->group0) / T3_BPG;
    uint32_t i = (blk - sbi->group0) % T3_BPG;
    if (g >= sbi->sb.gc) return 0;
    if (i < sbi->meta_off || i >= group_data_end(sbi, g)) return 0;
    return 1;
}

// Mark one referenced block; counts out-of-range and doubles.
// Returns 1 if the block is usable (in range, first reference).
static int fsck_mark_block(struct t3_state *sbi, struct t3_fsck *fk, uint32_t blk) {
    if (!fsck_block_ok(sbi, blk)) { fk->r->out_of_range++; return 0; }
    uint32_t idx = blk - sbi->group0;
    uint32_t g = idx / T3_BPG, i = idx % T3_BPG;
    uint8_t *b = &fk->breach[(size_t)g * T3_BLOCK + (i >> 3)];
    if (*b & (1u << (i & 7))) { fk->r->double_allocated++; return 0; }
    *b |= (uint8_t)(1u << (i & 7));
    fk->r->blocks_referenced++;
    return 1;
}

// Walk one inode's whole block tree (data + pointer blocks). `zap`
// support: on a repair pass, out-of-range pointers found in the INODE
// itself are zeroed via a transaction; ones inside pointer blocks are
// zeroed in place (data-class blocks, unjournaled like all data).
static void fsck_walk_table(struct t3_state *sbi, struct t3_fsck *fk, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl || !read_block(sbi, table_blk, tbl)) { if (tbl) kfree(tbl); return; }
    int dirty = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(tbl + i * 4);
        if (!e) continue;
        if (!fsck_block_ok(sbi, e)) {
            fk->r->out_of_range++;
            if (fk->repair) { wr32(tbl + i * 4, 0); dirty = 1; fk->r->pointers_cleared++; }
            continue;
        }
        if (depth == 0) fsck_mark_block(sbi, fk, e);
        else if (fsck_mark_block(sbi, fk, e)) fsck_walk_table(sbi, fk, e, depth - 1);
    }
    if (dirty) write_block(sbi, table_blk, tbl);
    kfree(tbl);
}

static void fsck_walk_inode_blocks(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino, struct t3_inode *node) {
    int inode_dirty = 0;
    for (int i = 0; i < 12; i++) {
        if (!node->ptrs[i]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[i])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[i] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else {
            fsck_mark_block(sbi, fk, node->ptrs[i]);
        }
    }
    for (int p = 12; p <= 14; p++) {
        if (!node->ptrs[p]) continue;
        if (!fsck_block_ok(sbi, node->ptrs[p])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[p] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else if (fsck_mark_block(sbi, fk, node->ptrs[p])) {
            fsck_walk_table(sbi, fk, node->ptrs[p], p - 12);
        }
    }
    if (inode_dirty) {
        if (txn_begin(sbi, 1) && txn_stage_inode(sbi, ino, node)) txn_commit(sbi); else txn_reset(sbi);
    }
}

static void fsck_mark_ino(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg), i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return;
    fk->ireach[(size_t)g * T3_BLOCK + (i >> 3)] |= (uint8_t)(1u << (i & 7));
}

static int fsck_ino_reached(struct t3_state *sbi, struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / sbi->sb.ipg), i = (uint32_t)(ino % sbi->sb.ipg);
    if (g >= sbi->sb.gc) return 1;
    return (fk->ireach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
}

// Depth-capped DFS over the directory tree. 32 components is far past
// anything the 64-byte caller paths can even express today; a deeper
// tree gets a klog and an unwalked subtree (reported as leaks --
// wrong, but loudly wrong).
static void fsck_walk_dir(struct t3_state *sbi, struct t3_fsck *fk, uint64_t dir_ino, uint64_t parent_ino, int depth) {
    if (depth > 32) {
        klog_write("tfs3 fsck: directory nesting past 32 -- subtree not walked\n");
        return;
    }
    struct t3_inode dir;
    if (!read_inode(sbi, dir_ino, &dir) || dir.type != T3_TYPE_DIR) return;
    fk->r->records_used++;
    fsck_walk_inode_blocks(sbi, fk, dir_ino, &dir);

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk;
        if (!block_for_index(sbi, &dir, b, &blk)) continue;
        uint8_t *dirblk = kmalloc(T3_BLOCK);
        if (!dirblk) return;
        if (!blk || !read_block(sbi, blk, dirblk)) { kfree(dirblk); continue; }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(dirblk + off);
            uint16_t rec_len = rd16(dirblk + off + 4);
            uint8_t nl = dirblk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) {
                klog_write(KLOG_ERR "tfs3 fsck: corrupt dirent chain in inode ");
                klog_write_dec((uint32_t)dir_ino); klog_write("\n");
                break;
            }
            if (e_ino != 0 && nl > 0) {
                int is_dot = (nl == 1 && dirblk[off + 7] == '.');
                int is_dotdot = (nl == 2 && dirblk[off + 7] == '.' && dirblk[off + 8] == '.');
                uint64_t child = e_ino;
                if (child < (uint64_t)sbi->sb.gc * sbi->sb.ipg) {
                    uint8_t *nc = &fk->names[child];
                    if (*nc < 255) (*nc)++;
                }
                if (is_dot) {
                    if (child != dir_ino) klog_write("tfs3 fsck: `.` points away from its own directory\n");
                } else if (is_dotdot) {
                    if (child != parent_ino) klog_write("tfs3 fsck: `..` points away from the parent\n");
                } else {
                    struct t3_inode cn;
                    if (!read_inode(sbi, child, &cn)) {
                        klog_write(KLOG_ERR "tfs3 fsck: dirent -> inode ");
                        klog_write_dec((uint32_t)child);
                        klog_write(" whose checksum fails (not repaired -- deleting a name is data loss)\n");
                    } else if (fsck_ino_reached(sbi, fk, child)) {
                        // Already visited: fine for files (hardlink),
                        // never for dirs.
                        if (cn.type == T3_TYPE_DIR)
                            klog_write("tfs3 fsck: directory reachable by two names\n");
                    } else {
                        fsck_mark_ino(sbi, fk, child);
                        if (cn.type == T3_TYPE_DIR) {
                            fsck_walk_dir(sbi, fk, child, dir_ino, depth + 1);
                        } else {
                            fk->r->records_used++;
                            fsck_walk_inode_blocks(sbi, fk, child, &cn);
                        }
                    }
                }
            }
            off += rec_len;
        }
        kfree(dirblk);
    }
}

static int tfs3_check(void *st, int repair, struct fs_check_result *out) {
    struct t3_state *sbi = st;
    struct fs_check_result local;
    struct fs_check_result *r = out ? out : &local;
    k_memset(r, 0, sizeof(*r));
    if (!sbi->mounted) return 0;

    // LAND ANY DEFERRED COMMIT FIRST. fsck walks what is ON THE DISK,
    // and under `storage.sync = batched` an inode update can be sitting
    // in the journal staging buffer -- so the blocks it references look
    // allocated-but-unreferenced and get counted as LEAKED. They are
    // not: they are referenced by an inode that has not landed yet.
    // Reporting a healthy filesystem as leaking is exactly the kind of
    // false alarm that teaches people to ignore the checker.
    txn_flush_deferred(sbi);

    struct t3_fsck fk;
    k_memset(&fk, 0, sizeof(fk));
    fk.r = r;
    fk.repair = repair;
    size_t bmbytes = (size_t)sbi->sb.gc * T3_BLOCK;
    uint64_t total_inodes = (uint64_t)sbi->sb.gc * sbi->sb.ipg;
    fk.breach = kmalloc(bmbytes);
    fk.ireach = kmalloc(bmbytes);
    fk.names = kmalloc((size_t)total_inodes);
    if (!fk.breach || !fk.ireach || !fk.names) {
        klog_write("tfs3 fsck: not enough memory for the reachability maps -- not checked\n");
        if (fk.breach) kfree(fk.breach);
        if (fk.ireach) kfree(fk.ireach);
        if (fk.names) kfree(fk.names);
        return 0;
    }
    k_memset(fk.breach, 0, bmbytes);
    k_memset(fk.ireach, 0, bmbytes);
    k_memset(fk.names, 0, (size_t)total_inodes);

    fsck_mark_ino(sbi, &fk, T3_INO_ROOT);
    fsck_walk_dir(sbi, &fk, T3_INO_ROOT, T3_INO_ROOT, 0);

    // Reconcile blocks: reach map vs allocation bitmap, per group.
    // Metadata and backup regions are allocated-by-design and outside
    // the reach map, so only the data area is compared.
    for (uint32_t g = 0; g < sbi->sb.gc; g++) {
        uint32_t end = group_data_end(sbi, g);
        uint32_t free_b = 0;
        uint32_t leak_run_start = 0, leak_run_len = 0;
        for (uint32_t i = 0; i < T3_BPG; i++) {
            int alloc = bbm_test(sbi, g, i);
            if (i < sbi->meta_off || i >= end) continue; // format-owned
            int reach = (fk.breach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
            if (alloc && !reach) {
                r->leaked++;
                if (repair) {
                    bbm_set(sbi, g, i, 0);
                    r->reclaimed++;
                    uint32_t blk = group_base(sbi, g) + i;
                    if (leak_run_len && blk == leak_run_start + leak_run_len) leak_run_len++;
                    else { trim_run(sbi, leak_run_start, leak_run_len); leak_run_start = blk; leak_run_len = 1; }
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { bbm_set(sbi, g, i, 1); r->marked_allocated++; }
            }
            if (!bbm_test(sbi, g, i)) free_b++;
        }
        if (repair) { trim_run(sbi, leak_run_start, leak_run_len); trim_flush(sbi); }

        // Inode bitmap + free counts: reconcile, repair-only writes.
        uint32_t free_i = 0;
        for (uint32_t i = 0; i < sbi->sb.ipg; i++) {
            uint64_t ino = (uint64_t)g * sbi->sb.ipg + i;
            int alloc = ibm_test(sbi, g, i);
            int reach = (fk.ireach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
            if (g == 0 && i == 0) { free_i += !alloc; continue; } // ino 0 reserved
            if (alloc && !reach) {
                // An orphaned inode is the inode-space leak. Reported
                // through the same counter (they are the same failure
                // class); reclaimed on repair.
                r->leaked++;
                if (repair) {
                    ibm_set(sbi, g, i, 0);
                    r->reclaimed++;
                    if (txn_begin(sbi, 1) && txn_stage_inode(sbi, ino, 0)) txn_commit(sbi); else txn_reset(sbi);
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { ibm_set(sbi, g, i, 1); r->marked_allocated++; }
            }
            if (!ibm_test(sbi, g, i)) free_i++;
        }

        if (sbi->gd[g].free_blocks != free_b || sbi->gd[g].free_inodes != free_i) {
            if (repair) {
                sbi->gd[g].free_blocks = free_b;
                sbi->gd[g].free_inodes = free_i;
                mark_dirty(sbi->gdt_dirty, g);
            }
        }
    }

    // Link counts: observed names vs stored counts. A directory's
    // observed count from the walk is its own dirent + `.` + each
    // child's `..`, which is exactly the 2+subdirs rule -- so one
    // comparison covers both types.
    for (uint64_t ino = 1; ino < total_inodes; ino++) {
        if (!fsck_ino_reached(sbi, &fk, ino) && ino != T3_INO_ROOT) continue;
        if (!fk.names[ino] && ino != T3_INO_ROOT) continue;
        struct t3_inode node;
        if (!read_inode(sbi, ino, &node)) continue;
        // One rule covers files, dirs AND the root: names[] counted
        // every dirent pointing at the inode, including `.`/`..`. A
        // dir gets parent-entry + own-`.` + children's `..` = 2+subdirs;
        // the root lacks a parent entry but its own `..` points at
        // itself, which restores the same total. Files get their
        // hardlink count.
        uint16_t want = fk.names[ino];
        if (node.links != want && fk.names[ino] < 255) {
            fk.link_mismatches++;
            klog_write("tfs3 fsck: inode "); klog_write_dec((uint32_t)ino);
            klog_write(" links="); klog_write_dec(node.links);
            klog_write(" but "); klog_write_dec(want);
            klog_write(" name(s) observed");
            if (repair) {
                node.links = want;
                if (txn_begin(sbi, 1) && txn_stage_inode(sbi, ino, &node) && txn_commit(sbi)) klog_write(" -- repaired");
                else txn_reset(sbi);
            }
            klog_write("\n");
        }
    }

    if (repair) {
        flush_alloc_state(sbi);
        // A primary superblock that failed at mount (we're running
        // from a backup) gets rewritten now, deliberately here and
        // never automatically at mount -- see the design doc.
        if (sbi->mounted_from_backup) {
            if (write_superblock_everywhere(sbi)) {
                klog_write("tfs3 fsck: primary superblock restored from the mounted backup\n");
                sbi->mounted_from_backup = 0;
            }
        }
    }

    kfree(fk.breach);
    kfree(fk.ireach);
    kfree(fk.names);
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
    return txn_flush_deferred(sbi);
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
    if (pit_ticks() - sbi->txn_staged_tick < quiet) {
        clockevent_idle_wake_at_tick(sbi->txn_staged_tick + quiet);
        return;
    }
    g_idle_commits++;
    txn_flush_deferred(sbi);
}

const struct fs_ops tfs3_ops = {
    .name = "tfs3",
    // Format truths, not implementation status: inodes and epoch
    // timestamps are live already; hardlinks/symlinks are carried by
    // the format (link counts, type 2) with their ops still to come --
    // see fs.h's FS_CAP_* comment on exactly this distinction.
    .chmod = tfs3_chmod,
    .caps = FS_CAP_INODES | FS_CAP_HARDLINKS | FS_CAP_SYMLINKS | FS_CAP_EPOCH_TIME |
            FS_CAP_MODE,
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
};

// ---- the deferred allocation flush ----------------------------------------
//
// HERE RATHER THAN IN fs_test.c BECAUSE fsck CANNOT SEE THIS BUG.
// tfs3_check() compares the inode tree against the RAM bitmap
// (bbm_test()), so a batch whose bitmap never reached the disk looks
// perfectly clean until the next mount re-reads it -- an earlier version
// of this check called fs_check() and passed with the flush removed
// entirely. So it reads the bitmap block back off the device.
#include "ktest.h"
#include "scheduler.h" // scheduler_preempt_disable() -- nothing else may TRIM mid-test

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
            if (read_block(sbi, group_base(sbi, g), disk)) on_disk = (disk[bit >> 3] >> (bit & 7)) & 1u;
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
