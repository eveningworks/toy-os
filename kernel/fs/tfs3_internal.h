#ifndef KERNEL_FS_TFS3_INTERNAL_H
#define KERNEL_FS_TFS3_INTERNAL_H

// TFS3's on-disk format and per-mount state, shared by the files that
// make up the backend and by nothing else -- the userland/wm/ pattern
// (docs/decisions.md). Split along ext4's lines:
//
//   tfs3.c          volume I/O, caches, superblock, the read path, inode
//                   locks, probe/format/mount, the read ops, mount state
//   tfs3_alloc.c    bitmaps, the rollback log, TRIM, write-side block map
//   tfs3_journal.c  the transaction, the deferred transaction, inode staging
//   tfs3_write.c    write core, truncation, dirents, the mutating ops
//   tfs3_fsck.c     fsck
//
// **EVERY FUNCTION HERE RUNS UNDER THE MOUNT'S LOCK**, exactly as it did
// as one file -- kernel/fs/CLAUDE.md has the rules; which file a helper
// lives in changes none of them. Helpers shared across files carry a
// t3_ prefix, since they are kernel-global now.

#include "fs.h"
#include "block.h"
#include "kpath_buf.h" // KPATH_SCRATCH_FOR -- t3_state's path scratch
#include <stddef.h>
#include <stdint.h>

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
// t3_map_get_or_alloc_tables() take the triple branch anyway and index a
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
    // Bumped by every inode STAGED for update (t3_txn_stage_inode()): an
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
    // name->inode within a directory, which still leaves t3_resolve()
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
    int txn_credits;             // what t3_txn_begin() promised; see t3_txn_stage()
    int txn_deferred;            // batched: open across writes (t3_txn_begin())
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
    } alog;                      // allocation rollback (t3_alog_begin())
    struct blk_range trim_q[TRIM_QUEUE];
    int trim_n;

    uint8_t blk[T3_BLOCK];       // general block scratch
    uint8_t ptr_blk[T3_BLOCK];   // indirect-pointer scratch, kept apart from blk
    uint8_t dirblk[T3_BLOCK];    // tfs3_list's copy of a dirent block
    char norm_scratch[KPATH_SCRATCH_FOR(T3_PATH_BUF)];   // t3_normalize()'s
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

// ---- little-endian field access (hand-serialized on disk) ---------------

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

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

// ---- shared helpers ---------------------------------------------------

// tfs3.c
int t3_backup_groups(uint32_t gc, uint32_t out[2]);
int t3_block_for_index(struct t3_state *sbi, const struct t3_inode *node, uint32_t idx,
                       uint32_t *out_blk);
uint32_t t3_group_base(struct t3_state *sbi, uint32_t g);
uint32_t t3_group_span(struct t3_state *sbi, uint32_t g);
int t3_inode_pos(struct t3_state *sbi, uint64_t ino, uint32_t *out_lba, uint32_t *out_off);
void t3_ncache_flush(struct t3_state *sbi);
int t3_normalize(struct t3_state *sbi, const char *path, char *out /* T3_PATH_BUF */);
void t3_rcache_drop(struct t3_state *sbi);
int t3_read_block(struct t3_state *sbi, uint32_t blk, void *buf);
int t3_read_inode(struct t3_state *sbi, uint64_t ino, struct t3_inode *out);
int t3_resolve(struct t3_state *sbi, const char *norm, uint64_t *out_ino);
int t3_lock(struct t3_state *sbi, uint64_t ino, int excl);
void t3_vol_go_readonly(struct t3_state *sbi, const char *why);
int t3_vol_read_sectors(struct t3_state *sbi, uint32_t lba, int count, void *buf);
int t3_vol_write_run(struct t3_state *sbi, uint32_t lba, int count, const void *buf);
int t3_vol_write_sectors(struct t3_state *sbi, uint32_t lba, int count, const void *buf);
int t3_write_block(struct t3_state *sbi, uint32_t blk, const void *buf);
int t3_write_superblock_everywhere(struct t3_state *sbi);

// tfs3_alloc.c
uint32_t t3_alloc_block(struct t3_state *sbi, uint32_t prefer_group, uint32_t adjacent_to);
uint64_t t3_alloc_inode(struct t3_state *sbi, uint32_t prefer_group);
void t3_alog_begin(struct t3_state *sbi);
void t3_alog_cancel(struct t3_state *sbi);
void t3_alog_commit(struct t3_state *sbi);
void t3_alog_forget(struct t3_state *sbi, uint32_t blk);
void t3_alog_rollback(struct t3_state *sbi);
void t3_bbm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v);
int t3_bbm_test(struct t3_state *sbi, uint32_t g, uint32_t i);
int t3_flush_alloc_state(struct t3_state *sbi);
void t3_free_block_bit(struct t3_state *sbi, uint32_t blk);
void t3_free_inode_bit(struct t3_state *sbi, uint64_t ino);
uint32_t t3_group_data_end(struct t3_state *sbi, uint32_t g);
void t3_ibm_set(struct t3_state *sbi, uint32_t g, uint32_t i, int v);
int t3_ibm_test(struct t3_state *sbi, uint32_t g, uint32_t i);
void t3_map_cache_forget(struct t3_state *sbi, uint32_t blk);
int t3_map_get_or_alloc_tables(struct t3_state *sbi, struct t3_inode *node, uint32_t idx,
                               uint32_t prefer_group, uint32_t *out_leaf_blk,
                               uint32_t *out_leaf_slot, uint32_t *out_existing);
void t3_map_set_block(struct t3_state *sbi, struct t3_inode *node, uint32_t leaf_blk,
                      uint32_t leaf_slot, uint32_t data_blk);
void t3_mark_dirty(uint8_t *set, uint32_t g);
void t3_pcache_drop(struct t3_state *sbi);
int t3_pcache_flush(struct t3_state *sbi);
void t3_trim_flush(struct t3_state *sbi);
void t3_trim_run(struct t3_state *sbi, uint32_t first_blk, uint32_t count);

// tfs3_journal.c
uint32_t t3_jh_cksum_off(uint32_t version);
uint64_t t3_now_epoch(void);
int t3_replay_journal(struct t3_state *sbi);
int t3_txn_begin(struct t3_state *sbi, int credits);
int t3_txn_commit(struct t3_state *sbi);
int t3_txn_flush_deferred(struct t3_state *sbi);
void t3_txn_reset(struct t3_state *sbi);
uint8_t *t3_txn_stage(struct t3_state *sbi, uint32_t blk);
int t3_txn_stage_inode(struct t3_state *sbi, uint64_t ino, const struct t3_inode *node);

// tfs3_write.c
int t3_stage_inode_update(struct t3_state *sbi, uint64_t ino, struct t3_inode *node);
int tfs3_delete(void *st, const char *path);
int tfs3_link(void *st, const char *existing, const char *newpath);
int tfs3_mkdir(void *st, const char *path);
int tfs3_rename(void *st, const char *oldpath, const char *newpath);
int tfs3_touch(void *st, const char *path);
int tfs3_truncate(void *st, const char *path, uint64_t size);
int tfs3_write(void *st, const char *path, const char *data, int append);
int tfs3_write_range(void *st, const char *path, uint64_t offset, const void *buf,
                     uint32_t len);
void *tfs3_write_range_begin(void *st, const char *path, uint64_t offset,
                             const void *buf, uint32_t len);
int tfs3_write_range_step(void *st, void *handle);

// tfs3_fsck.c
int tfs3_check(void *st, int repair, struct fs_check_result *out);

#endif
