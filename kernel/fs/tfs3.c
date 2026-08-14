// TFS3 -- block groups, inodes, dirent blocks. Format spec:
// docs/tfs3-design.md; host mirror: tools/tfs3_writer.py (the two are
// kept in lockstep the way tfs.c and tfs2_writer.py are).
//
// STAGE B: probe/format/mount (with backup-superblock fallback) and
// the whole READ side are real; every mutating op fails honestly with
// one klog. The write path + 4-slot journal transactions are Stage C,
// fsck is Stage D -- see docs/roadmap.md's Milestone 15 and the
// CHANGELOG. Split into stages so each lands with `make verify`
// green, not because the boundaries are architectural.
//
// Everything on disk is VOLUME-relative: block b lives at sector
// g_vol.base_lba + b * T3_SPB, and this file only touches the disk
// through vol_read()/vol_write(). Today the volume is the flat disk
// ({0, ata_sector_count()}); when partition mounting arrives the
// probe loop hands in a partition's extent instead and nothing here
// changes -- that seam is the point (see the design doc's "Volumes
// and partitions").
#include "fs.h"
#include "fs_ops.h"
#include "tfs3.h"
#include "string.h"
#include "ata.h"
#include "klog.h"
#include "tz.h"
#include "heap.h"
#include "kpath.h"

// ---- format constants (docs/tfs3-design.md; tfs3_writer.py mirrors) ----

#define T3_BLOCK        4096u
#define T3_SPB          8u              // sectors per block
#define T3_SB_BLOCK     8u
#define T3_JH_BLOCK     9u
#define T3_JDATA_BLOCK  10u
#define T3_JSLOTS       4u
#define T3_GDT_BLOCK    14u
#define T3_GDT_BLOCKS   16u             // fixed -- positions derive without a superblock
#define T3_GROUP0       30u
#define T3_BPG          32768u          // blocks per group (one 4 KiB bitmap)
#define T3_INODE_SIZE   128u
#define T3_INODES_PER_SECTOR (ATA_SECTOR_SIZE / T3_INODE_SIZE)
#define T3_BACKUP_BLOCKS (T3_GDT_BLOCKS + 1)
#define T3_PTRS_PER_BLOCK (T3_BLOCK / 4u)

#define T3_VERSION      1u
#define T3_INO_ROOT     1u

#define T3_TYPE_FILE    0u
#define T3_TYPE_DIR     1u
#define T3_TYPE_SYMLINK 2u

// Deeper than any caller can currently express (every fs.h caller
// holds FS_PATH_MAX=64 buffers), but the format has no path cap, so
// this backend's own working buffer is roomier on purpose.
#define T3_PATH_BUF     256
#define T3_NAME_MAX     255

#define T3_SB_READ_RETRIES 3 // same transient-DMA-miss reasoning as tfs.c's

// Default bytes-per-inode ratio for format(); tfs3_writer.py mirrors.
#define T3_BYTES_PER_INODE 16384u

// ---- state --------------------------------------------------------------

struct t3_vol {
    uint32_t base_lba;
    uint32_t sector_count;
};

struct t3_gd { uint32_t free_blocks, free_inodes; };

static struct t3_vol g_vol;
static int g_mounted = 0;

static struct {
    uint8_t flags;
    uint32_t total_blocks;
    uint32_t bpg, ipg, gc;
} g_sb;

static uint32_t g_itb;        // inode-table blocks per group
static int g_mounted_from_backup = 0; // primary superblock was bad at mount; fsck repair restores it
static uint32_t g_meta_off;   // first data-ish block offset within a group (2 [+cksum] + itb)
static struct t3_gd *g_gd = 0; // free-count caches, g_sb.gc entries

// In-RAM copies of every group's block/inode bitmap (g_sb.gc * 4 KiB
// each -- ~284 KiB per cache on the 9 GiB image, the same order as
// tfs.c's static full-disk bitmap). RAM is the authority during
// operation; changed blocks are written through, UNJOURNALED, with
// the set-before-use / clear-after-persist ordering that makes a
// crash cost a leak, never a double allocation -- TFS2's exact
// discipline, inherited deliberately (see the journal section).
static uint8_t *g_bbm = 0;
static uint8_t *g_ibm = 0;
static uint32_t *g_rotor = 0; // per-group scan start (ext2's trick)

// Dirty tracking for the write-through: one bit per group, per cache.
#define T3_MAX_GROUPS (T3_GDT_BLOCKS * T3_BLOCK / 16u)
static uint8_t g_bbm_dirty[T3_MAX_GROUPS / 8];
static uint8_t g_ibm_dirty[T3_MAX_GROUPS / 8];
static uint8_t g_gdt_dirty[T3_MAX_GROUPS / 8];

// Journal transaction staging (up to 4 block images -- see the
// design doc's journal section and txn_commit() below).
static uint8_t g_txn_img[T3_JSLOTS][T3_BLOCK];
static uint32_t g_txn_target[T3_JSLOTS];
static int g_txn_count = 0;
static uint32_t g_jrn_seq = 0;

// Tiny name-lookup cache (dir ino + name -> child ino): path walks
// are the hot loop, and every component is otherwise a dirent scan.
// Invalidated wholesale on any mutation -- cheap and obviously
// correct; per-directory invalidation is a later refinement if it
// ever shows up in profiles.
#define T3_NCACHE 16
#define T3_NCACHE_NAME 48
static struct {
    uint64_t dir, ino;
    uint8_t len;
    char name[T3_NCACHE_NAME];
} g_ncache[T3_NCACHE];
static int g_ncache_next = 0;

static void ncache_flush(void) {
    for (int i = 0; i < T3_NCACHE; i++) g_ncache[i].dir = 0;
    g_ncache_next = 0;
}

// One block of scratch for everything on this (single-threaded)
// kernel -- same convention as tfs.c's g_io_scratch.
static uint8_t g_blk[T3_BLOCK];
static uint8_t g_ptr_blk[T3_BLOCK]; // indirect-pointer scratch, kept separate from data

static void *g_read_buf = 0; // tfs_read()-style whole-file buffer, freed on next call

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

static int vol_read_sectors(uint32_t lba, int count, void *buf) {
    if (lba + (uint32_t)count > g_vol.sector_count) return 0;
    return ata_read_sectors(g_vol.base_lba + lba, count, buf);
}

static int vol_write_sectors(uint32_t lba, int count, const void *buf) {
    if (lba + (uint32_t)count > g_vol.sector_count) return 0;
    return ata_write_sectors(g_vol.base_lba + lba, count, buf);
}

static int read_block(uint32_t blk, void *buf) {
    return vol_read_sectors(blk * T3_SPB, (int)T3_SPB, buf);
}

static int write_block(uint32_t blk, const void *buf) {
    return vol_write_sectors(blk * T3_SPB, (int)T3_SPB, buf);
}

static void set_flat_volume(void) {
    g_vol.base_lba = 0;
    g_vol.sector_count = ata_sector_count();
}

// ---- superblock ----------------------------------------------------------

// Parses+validates one superblock sector into g_sb. Returns 1 valid.
static int parse_superblock(const uint8_t *sec) {
    if (!(sec[0] == 'T' && sec[1] == 'F' && sec[2] == 'S' && sec[3] == '3')) return 0;
    if (sec[4] != T3_VERSION) return 0;
    if (k_fnv1a(sec, 44) != rd32(sec + 44)) return 0;
    uint32_t total_blocks = rd32(sec + 8);
    uint32_t bpg = rd32(sec + 12);
    uint32_t ipg = rd32(sec + 16);
    uint32_t gc = rd32(sec + 20);
    uint32_t group0 = rd32(sec + 24);
    if (bpg != T3_BPG || group0 != T3_GROUP0) return 0;
    if (gc == 0 || ipg == 0 || ipg > T3_BPG) return 0;
    if ((uint64_t)total_blocks * T3_SPB > (uint64_t)g_vol.sector_count) return 0; // claims more volume than exists
    if (T3_GROUP0 + (uint64_t)gc * bpg > total_blocks) return 0;
    g_sb.flags = sec[5];
    g_sb.total_blocks = total_blocks;
    g_sb.bpg = bpg;
    g_sb.ipg = ipg;
    g_sb.gc = gc;
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
// mounted-ready (g_sb filled), 0 no valid superblock anywhere, -1
// primary unreadable (refuse -- never treat a failing disk as blank).
static int load_superblock(int loud) {
    uint8_t sec[ATA_SECTOR_SIZE];
    int got = 0;
    for (int i = 0; i < T3_SB_READ_RETRIES && !got; i++) {
        got = vol_read_sectors(T3_SB_BLOCK * T3_SPB, 1, sec);
    }
    if (!got) return -1;
    if (parse_superblock(sec)) { g_mounted_from_backup = 0; return 1; }

    // Primary readable but invalid -- try the backups, derived from
    // the volume size (see docs/tfs3-design.md "Superblock backups").
    uint32_t vol_blocks = g_vol.sector_count / T3_SPB;
    if (vol_blocks <= T3_GROUP0) return 0;
    uint32_t gc = (vol_blocks - T3_GROUP0) / T3_BPG;
    uint32_t groups[2];
    int n = backup_groups(gc, groups);
    for (int i = 0; i < n; i++) {
        uint32_t blk = T3_GROUP0 + (groups[i] + 1) * T3_BPG - 1;
        if (!vol_read_sectors(blk * T3_SPB, 1, sec)) continue;
        if (parse_superblock(sec)) {
            g_mounted_from_backup = 1;
            if (loud) {
                klog_write("tfs3: primary superblock invalid -- mounted from the backup in group ");
                klog_write_dec(groups[i]);
                klog_write(" (run `fsck repair` to restore the primary)\n");
            }
            return 1;
        }
    }
    return 0;
}

// Serialize g_sb and write it to the primary AND every backup slot --
// fsck repair's restore path, and never called at mount.
static int write_superblock_everywhere(void) {
    k_memset(g_blk, 0, T3_BLOCK);
    g_blk[0] = 'T'; g_blk[1] = 'F'; g_blk[2] = 'S'; g_blk[3] = '3';
    g_blk[4] = T3_VERSION; g_blk[5] = g_sb.flags;
    wr32(g_blk + 8, g_sb.total_blocks);
    wr32(g_blk + 12, g_sb.bpg);
    wr32(g_blk + 16, g_sb.ipg);
    wr32(g_blk + 20, g_sb.gc);
    wr32(g_blk + 24, T3_GROUP0);
    wr32(g_blk + 44, k_fnv1a(g_blk, 44));
    int ok = write_block(T3_SB_BLOCK, g_blk);
    uint32_t groups[2];
    int n = backup_groups(g_sb.gc, groups);
    for (int i = 0; i < n; i++) {
        uint32_t blk = T3_GROUP0 + (groups[i] + 1) * T3_BPG - 1;
        if (!write_block(blk, g_blk)) ok = 0;
    }
    return ok;
}

// ---- group / inode geometry ----------------------------------------------

static uint32_t group_base(uint32_t g) { return T3_GROUP0 + g * T3_BPG; }

static uint32_t cksum_table_blocks(void) {
    // Feature bit 0: per-group data-block checksum table (format-time
    // choice; algorithm deferred to Milestone 16). We don't verify it
    // yet, but the geometry must account for the region either way.
    return (g_sb.flags & 1u) ? (T3_BPG * 4u + T3_BLOCK - 1) / T3_BLOCK : 0;
}

static void derive_geometry(void) {
    g_itb = g_sb.ipg * T3_INODE_SIZE / T3_BLOCK;
    g_meta_off = 2 + cksum_table_blocks() + g_itb;
}

// Sector (volume-relative LBA) holding inode `ino`, plus its offset
// within that sector.
static int inode_pos(uint64_t ino, uint32_t *out_lba, uint32_t *out_off) {
    uint32_t g = (uint32_t)(ino / g_sb.ipg);
    uint32_t idx = (uint32_t)(ino % g_sb.ipg);
    if (g >= g_sb.gc) return 0;
    uint32_t table = group_base(g) + 2 + cksum_table_blocks();
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
};

static int read_inode(uint64_t ino, struct t3_inode *out) {
    uint32_t lba, off;
    if (!inode_pos(ino, &lba, &off)) return 0;
    uint8_t sec[ATA_SECTOR_SIZE];
    if (!vol_read_sectors(lba, 1, sec)) return 0;
    const uint8_t *p = sec + off;
    // Checksum covers bytes 0-87 and 92-127 (everything but itself).
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    if (k_fnv1a(chk, sizeof(chk)) != rd32(p + 88)) return 0;
    out->type = p[0];
    out->links = rd16(p + 2);
    out->size = rd64(p + 4);
    out->created = rd64(p + 12);
    out->modified = rd64(p + 20);
    for (int i = 0; i < 15; i++) out->ptrs[i] = rd32(p + 28 + i * 4);
    return 1;
}

// ---- block map (read side: full direct/single/double/triple walk) -------

// Data-block number for file-block index `idx`, or 0 for a hole /
// out-of-range. Reads at most two pointer blocks into g_ptr_blk.
static uint32_t block_for_index(const struct t3_inode *node, uint32_t idx) {
    if (idx < 12) return node->ptrs[idx];
    idx -= 12;
    if (idx < T3_PTRS_PER_BLOCK) {
        if (!node->ptrs[12]) return 0;
        if (!read_block(node->ptrs[12], g_ptr_blk)) return 0;
        return rd32(g_ptr_blk + idx * 4);
    }
    idx -= T3_PTRS_PER_BLOCK;
    if (idx < T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK) {
        if (!node->ptrs[13]) return 0;
        if (!read_block(node->ptrs[13], g_ptr_blk)) return 0;
        uint32_t mid = rd32(g_ptr_blk + (idx / T3_PTRS_PER_BLOCK) * 4);
        if (!mid || !read_block(mid, g_ptr_blk)) return 0;
        return rd32(g_ptr_blk + (idx % T3_PTRS_PER_BLOCK) * 4);
    }
    idx -= T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK;
    {
        if (!node->ptrs[14]) return 0;
        if (!read_block(node->ptrs[14], g_ptr_blk)) return 0;
        uint32_t hi = rd32(g_ptr_blk + (idx / (T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK)) * 4);
        if (!hi || !read_block(hi, g_ptr_blk)) return 0;
        uint32_t rem = idx % (T3_PTRS_PER_BLOCK * T3_PTRS_PER_BLOCK);
        uint32_t mid = rd32(g_ptr_blk + (rem / T3_PTRS_PER_BLOCK) * 4);
        if (!mid || !read_block(mid, g_ptr_blk)) return 0;
        return rd32(g_ptr_blk + (rem % T3_PTRS_PER_BLOCK) * 4);
    }
}

// ---- path resolution ------------------------------------------------------

// Walk one directory's dirent chain looking for `name` (len bytes, no
// NUL requirement). Returns the child inode or 0. `dir_ino` is only
// for the name cache; pass 0 to bypass caching (e.g. during repair).
static uint64_t dir_lookup(uint64_t dir_ino, const struct t3_inode *dir,
                           const char *name, uint32_t name_len) {
    if (dir_ino && name_len < T3_NCACHE_NAME) {
        for (int i = 0; i < T3_NCACHE; i++) {
            if (g_ncache[i].dir == dir_ino && g_ncache[i].len == name_len &&
                k_memcmp(g_ncache[i].name, name, name_len) == 0) {
                return g_ncache[i].ino;
            }
        }
    }
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = block_for_index(dir, b);
        if (!blk || !read_block(blk, g_blk)) return 0;
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(g_blk + off);
            uint16_t rec_len = rd16(g_blk + off + 4);
            uint8_t nl = g_blk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break; // corrupt chain -- stop, don't loop
            if (e_ino != 0 && nl == name_len &&
                k_memcmp(g_blk + off + 7, name, name_len) == 0) {
                if (dir_ino && name_len < T3_NCACHE_NAME) {
                    int s = g_ncache_next;
                    g_ncache_next = (g_ncache_next + 1) % T3_NCACHE;
                    g_ncache[s].dir = dir_ino;
                    g_ncache[s].ino = e_ino;
                    g_ncache[s].len = (uint8_t)name_len;
                    k_memcpy(g_ncache[s].name, name, name_len);
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
static int resolve(const char *norm, uint64_t *out_ino) {
    uint64_t ino = T3_INO_ROOT;
    const char *p = norm;
    if (*p == '/') p++;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);
        if (len == 0 || len > T3_NAME_MAX) return 0;
        struct t3_inode dir;
        if (!read_inode(ino, &dir) || dir.type != T3_TYPE_DIR) return 0;
        ino = dir_lookup(ino, &dir, start, len);
        if (!ino) return 0;
        if (*p == '/') p++;
    }
    *out_ino = ino;
    return 1;
}

// Backend-internal normalization, per fs_ops.h's contract. kpath's
// lexical normalize is exactly this job.
static int normalize(const char *path, char *out /* T3_PATH_BUF */) {
    if (!path) return 0;
    return k_path_normalize(path, out, T3_PATH_BUF) == 0 ? 0 : 1;
}

// resolve() + normalize() in one, the common op prologue.
static int lookup(const char *path, uint64_t *out_ino, struct t3_inode *out_node) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    if (!resolve(norm, &ino)) return 0;
    if (out_ino) *out_ino = ino;
    if (out_node) return read_inode(ino, out_node);
    return 1;
}

// ---- allocation (RAM bitmaps, write-through, leak-safe ordering) ---------

static void mark_dirty(uint8_t *set, uint32_t g) { set[g >> 3] |= (uint8_t)(1u << (g & 7)); }
static int test_dirty(const uint8_t *set, uint32_t g) { return (set[g >> 3] >> (g & 7)) & 1; }
static void clear_dirty(uint8_t *set, uint32_t g) { set[g >> 3] &= (uint8_t)~(1u << (g & 7)); }

static int bbm_test(uint32_t g, uint32_t i) { return (g_bbm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
static void bbm_set(uint32_t g, uint32_t i, int v) {
    uint8_t *b = &g_bbm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    mark_dirty(g_bbm_dirty, g);
}
static int ibm_test(uint32_t g, uint32_t i) { return (g_ibm[g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1; }
static void ibm_set(uint32_t g, uint32_t i, int v) {
    uint8_t *b = &g_ibm[g * T3_BLOCK + (i >> 3)];
    if (v) *b |= (uint8_t)(1u << (i & 7)); else *b &= (uint8_t)~(1u << (i & 7));
    mark_dirty(g_ibm_dirty, g);
}

static void alog_push(uint32_t blk); // rollback log, defined below with its story

// Highest usable data offset within group g (backup regions excluded).
static uint32_t group_data_end(uint32_t g) {
    uint32_t groups[2];
    int n = backup_groups(g_sb.gc, groups);
    for (int i = 0; i < n; i++) {
        if (groups[i] == g) return T3_BPG - T3_BACKUP_BLOCKS;
    }
    return T3_BPG;
}

// Try to allocate one specific block (the adjacent-first fast path).
static int alloc_block_at(uint32_t blk) {
    if (blk < T3_GROUP0 + g_meta_off) return 0;
    uint32_t g = (blk - T3_GROUP0) / T3_BPG;
    uint32_t i = (blk - T3_GROUP0) % T3_BPG;
    if (g >= g_sb.gc || i < g_meta_off || i >= group_data_end(g)) return 0;
    if (bbm_test(g, i)) return 0;
    bbm_set(g, i, 1);
    g_gd[g].free_blocks--;
    mark_dirty(g_gdt_dirty, g);
    alog_push(blk);
    return 1;
}

// Allocate one data block: `hint` (try hint+1's spirit: exactly that
// block) first, then rotor scan of the preferred group, then every
// other group. Returns the block number or 0.
static uint32_t alloc_block(uint32_t prefer_group, uint32_t adjacent_to) {
    if (adjacent_to && alloc_block_at(adjacent_to + 1)) return adjacent_to + 1;
    if (prefer_group >= g_sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < g_sb.gc; n++) {
        uint32_t g = (prefer_group + n) % g_sb.gc;
        if (g_gd[g].free_blocks == 0) continue;
        uint32_t end = group_data_end(g);
        uint32_t start = g_rotor[g];
        if (start < g_meta_off || start >= end) start = g_meta_off;
        for (uint32_t k = 0; k < end - g_meta_off; k++) {
            uint32_t i = start + k;
            if (i >= end) i = g_meta_off + (i - end);
            if (!bbm_test(g, i)) {
                bbm_set(g, i, 1);
                g_gd[g].free_blocks--;
                mark_dirty(g_gdt_dirty, g);
                g_rotor[g] = i + 1;
                alog_push(group_base(g) + i);
                return group_base(g) + i;
            }
        }
    }
    return 0;
}

static void free_block_bit(uint32_t blk) {
    uint32_t g = (blk - T3_GROUP0) / T3_BPG;
    uint32_t i = (blk - T3_GROUP0) % T3_BPG;
    if (g >= g_sb.gc) return;
    if (!bbm_test(g, i)) return; // double-free guard -- fsck's problem, not a crash
    bbm_set(g, i, 0);
    g_gd[g].free_blocks++;
    mark_dirty(g_gdt_dirty, g);
    if (g_rotor[g] > i) g_rotor[g] = i;
}

static uint64_t alloc_inode(uint32_t prefer_group) {
    if (prefer_group >= g_sb.gc) prefer_group = 0;
    for (uint32_t n = 0; n < g_sb.gc; n++) {
        uint32_t g = (prefer_group + n) % g_sb.gc;
        if (g_gd[g].free_inodes == 0) continue;
        for (uint32_t i = 0; i < g_sb.ipg; i++) {
            if (!ibm_test(g, i)) {
                ibm_set(g, i, 1);
                g_gd[g].free_inodes--;
                mark_dirty(g_gdt_dirty, g);
                return (uint64_t)g * g_sb.ipg + i;
            }
        }
    }
    return 0;
}

static void free_inode_bit(uint64_t ino) {
    uint32_t g = (uint32_t)(ino / g_sb.ipg);
    uint32_t i = (uint32_t)(ino % g_sb.ipg);
    if (g >= g_sb.gc || !ibm_test(g, i)) return;
    ibm_set(g, i, 0);
    g_gd[g].free_inodes++;
    mark_dirty(g_gdt_dirty, g);
}

// Write every dirty bitmap block and GDT block through to disk.
// UNJOURNALED on purpose: called BEFORE the transaction that makes an
// allocation reachable (set-before-use) and AFTER the transaction
// that makes a free unreachable (clear-after-persist), so a crash at
// any point costs a leaked block/inode -- never a double allocation.
// TFS2's exact metadata-ordering rule, see docs/decisions.md.
static int flush_alloc_state(void) {
    int ok = 1;
    for (uint32_t g = 0; g < g_sb.gc; g++) {
        if (test_dirty(g_bbm_dirty, g)) {
            if (!write_block(group_base(g), g_bbm + g * T3_BLOCK)) ok = 0;
            clear_dirty(g_bbm_dirty, g);
        }
        if (test_dirty(g_ibm_dirty, g)) {
            if (!write_block(group_base(g) + 1, g_ibm + g * T3_BLOCK)) ok = 0;
            clear_dirty(g_ibm_dirty, g);
        }
    }
    // GDT blocks: rebuild each dirty block from the RAM free counts.
    // Backup GDT snapshots are deliberately NOT refreshed (stale by
    // design -- fsck recomputes counts anyway; see the design doc).
    for (uint32_t tb = 0; tb < T3_GDT_BLOCKS; tb++) {
        int dirty = 0;
        for (uint32_t i = 0; i < T3_BLOCK / 16 && !dirty; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g < g_sb.gc && test_dirty(g_gdt_dirty, g)) dirty = 1;
        }
        if (!dirty) continue;
        k_memset(g_blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= g_sb.gc) break;
            uint8_t *e = g_blk + i * 16;
            wr32(e, g_gd[g].free_blocks);
            wr32(e + 4, g_gd[g].free_inodes);
            wr32(e + 12, k_fnv1a(e, 12));
            clear_dirty(g_gdt_dirty, g);
        }
        if (!write_block(T3_GDT_BLOCK + tb, g_blk)) ok = 0;
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
static struct {
    uint32_t *v;
    uint32_t n, cap;
    int active;
    int overflow;
} g_alog;

static void alog_begin(void) { g_alog.n = 0; g_alog.active = 1; g_alog.overflow = 0; }

static void alog_push(uint32_t blk) {
    if (!g_alog.active || g_alog.overflow) return;
    if (g_alog.n == g_alog.cap) {
        uint32_t ncap = g_alog.cap ? g_alog.cap * 2 : 64;
        uint32_t *nv = kmalloc(ncap * sizeof(uint32_t));
        if (!nv) { g_alog.overflow = 1; return; }
        if (g_alog.v) { k_memcpy(nv, g_alog.v, g_alog.n * sizeof(uint32_t)); kfree(g_alog.v); }
        g_alog.v = nv;
        g_alog.cap = ncap;
    }
    g_alog.v[g_alog.n++] = blk;
}

static void alog_commit(void) { g_alog.active = 0; }

// Remove one block from the log: it just became referenced by a
// COMMITTED transaction (directory growth commits mid-operation), so
// an outer rollback must not free it out from under that reference.
static void alog_forget(uint32_t blk) {
    for (uint32_t i = 0; i < g_alog.n; i++) {
        if (g_alog.v[i] == blk) {
            g_alog.v[i] = g_alog.v[--g_alog.n];
            return;
        }
    }
}

static void alog_rollback(void) {
    if (g_alog.overflow) {
        // Couldn't track everything -- leak honestly rather than free
        // a partial (possibly wrong) set. fsck reclaims.
        klog_write("tfs3: rollback log overflowed -- leaked blocks left for fsck\n");
    } else {
        for (uint32_t i = 0; i < g_alog.n; i++) free_block_bit(g_alog.v[i]);
        flush_alloc_state();
    }
    g_alog.active = 0;
}

// TRIM freed blocks in runs, best-effort -- parity with tfs.c's
// free_block(): the block is free either way, a refused TRIM must not
// fail the delete. Called with a sorted-ish run start/count.
static void trim_run(uint32_t first_blk, uint32_t count) {
    if (!count || !ata_trim_supported()) return;
    ata_trim(g_vol.base_lba + first_blk * T3_SPB, count * T3_SPB);
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
// Commit discipline = persist_record()'s, generalized (see tfs.c for
// the two-barrier reasoning): stage images + committed header, FLUSH,
// write targets, FLUSH, clear header (no barrier -- a stale committed
// header just replays idempotently).

static void txn_reset(void) { g_txn_count = 0; }

// Stage `blk`'s new image into the transaction. Returns a writable
// pointer to the staged 4 KiB image (pre-loaded from disk so callers
// patch in place), or 0 when full/read-failed. Staging the same block
// twice returns the same image.
static uint8_t *txn_stage(uint32_t blk) {
    for (int i = 0; i < g_txn_count; i++) {
        if (g_txn_target[i] == blk) return g_txn_img[i];
    }
    if (g_txn_count >= (int)T3_JSLOTS) return 0;
    if (!read_block(blk, g_txn_img[g_txn_count])) return 0;
    g_txn_target[g_txn_count] = blk;
    return g_txn_img[g_txn_count++];
}

static int write_journal_header(int commit) {
    uint8_t sec[ATA_SECTOR_SIZE];
    k_memset(sec, 0, sizeof(sec));
    sec[0] = 'J'; sec[1] = 'R'; sec[2] = 'N'; sec[3] = '3';
    sec[4] = (uint8_t)commit;
    sec[5] = (uint8_t)g_txn_count;
    wr32(sec + 8, g_jrn_seq);
    for (int i = 0; i < g_txn_count; i++) {
        wr32(sec + 12 + i * 8, g_txn_target[i]);
        wr32(sec + 16 + i * 8, k_fnv1a(g_txn_img[i], T3_BLOCK));
    }
    wr32(sec + 44, k_fnv1a(sec, 44));
    return vol_write_sectors(T3_JH_BLOCK * T3_SPB, 1, sec);
}

static int txn_commit(void) {
    if (g_txn_count == 0) return 1;
    g_jrn_seq++;
    for (int i = 0; i < g_txn_count; i++) {
        if (!write_block(T3_JDATA_BLOCK + (uint32_t)i, g_txn_img[i])) { txn_reset(); return 0; }
    }
    if (!write_journal_header(1)) { txn_reset(); return 0; }
    ata_flush_now(); // barrier 1: the transaction survives a crash from here
    int ok = 1;
    for (int i = 0; i < g_txn_count; i++) {
        if (!write_block(g_txn_target[i], g_txn_img[i])) ok = 0;
    }
    ata_flush_now(); // barrier 2: targets durable before the commit flag clears
    if (ok) {
        int saved = g_txn_count;
        g_txn_count = 0;
        write_journal_header(0); // no barrier -- see the discipline note above
        g_txn_count = saved;
    } else {
        // Leave the header committed: replay finishes the job next
        // boot, same call replay_journal() in tfs.c makes.
        klog_write("tfs3: transaction target write failed -- left committed for replay\n");
    }
    txn_reset();
    return ok;
}

static void replay_journal(void) {
    uint8_t sec[ATA_SECTOR_SIZE];
    if (!vol_read_sectors(T3_JH_BLOCK * T3_SPB, 1, sec)) return;
    if (!(sec[0] == 'J' && sec[1] == 'R' && sec[2] == 'N' && sec[3] == '3')) return;
    if (k_fnv1a(sec, 44) != rd32(sec + 44)) return; // torn header = no transaction
    g_jrn_seq = rd32(sec + 8);
    if (!sec[4]) return; // not committed
    uint32_t count = sec[5];
    if (count == 0 || count > T3_JSLOTS) count = 0;

    int all_ok = (count > 0);
    for (uint32_t i = 0; i < count && all_ok; i++) {
        if (!read_block(T3_JDATA_BLOCK + i, g_txn_img[i])) all_ok = 0;
        else if (k_fnv1a(g_txn_img[i], T3_BLOCK) != rd32(sec + 16 + i * 8)) all_ok = 0;
    }
    if (all_ok) {
        for (uint32_t i = 0; i < count && all_ok; i++) {
            if (!write_block(rd32(sec + 12 + i * 8), g_txn_img[i])) all_ok = 0;
        }
        if (all_ok) {
            ata_flush_now();
            klog_write("tfs3: replayed a committed journal transaction (");
            klog_write_dec(count); klog_write(" blocks)\n");
        } else {
            klog_write("tfs3: journal replay write failed -- left committed for next boot\n");
            return;
        }
    } else {
        klog_write("tfs3: discarded a torn journal transaction\n");
    }
    g_txn_count = 0;
    write_journal_header(0);
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
    uint8_t chk[124];
    k_memcpy(chk, p, 88);
    k_memcpy(chk + 88, p + 92, 36);
    wr32(p + 88, k_fnv1a(chk, sizeof(chk)));
}

// Patch inode `ino`'s 128 bytes inside its (journal-staged) table
// block. `node == 0` zeroes the slot -- a dead inode fails its
// checksum by design, the bitmap is the allocation authority.
static int txn_stage_inode(uint64_t ino, const struct t3_inode *node) {
    uint32_t lba, off;
    if (!inode_pos(ino, &lba, &off)) return 0;
    uint32_t blk = lba / T3_SPB;
    uint32_t within = (lba % T3_SPB) * ATA_SECTOR_SIZE + off;
    uint8_t *img = txn_stage(blk);
    if (!img) return 0;
    if (node) pack_inode_into(img + within, node);
    else k_memset(img + within, 0, T3_INODE_SIZE);
    return 1;
}

static uint64_t now_epoch(void) {
    struct rtc_time t;
    rtc_read_local(&t);
    return tz_rtc_to_epoch(&t);
}

// ---- block-map allocation (write side) ------------------------------------

// One-deep cache of the last-level pointer block being filled, so a
// long sequential write patches it in RAM and writes it once per
// 1024 data blocks instead of read-modify-writing 4 KiB per block.
static struct {
    uint32_t blk;   // 0 = empty
    int dirty;
    uint8_t buf[T3_BLOCK];
} g_pcache;

static int pcache_flush(void) {
    if (g_pcache.blk && g_pcache.dirty) {
        if (!write_block(g_pcache.blk, g_pcache.buf)) return 0;
        g_pcache.dirty = 0;
    }
    return 1;
}

static int pcache_load(uint32_t blk, int fresh) {
    if (g_pcache.blk == blk) return 1;
    if (!pcache_flush()) return 0;
    g_pcache.blk = blk;
    g_pcache.dirty = 0;
    if (fresh) { k_memset(g_pcache.buf, 0, T3_BLOCK); g_pcache.dirty = 1; return 1; }
    return read_block(blk, g_pcache.buf);
}

static void pcache_drop(void) { g_pcache.blk = 0; g_pcache.dirty = 0; }

// Allocate (if needed) and return the pointer-table slot chain for
// file-block `idx`, allocating intermediate pointer blocks as it
// goes. Returns the existing-or-new data block number via *out_blk
// (0 if a fresh one must be allocated by the caller and recorded with
// map_set_block()). This walks with the pcache for the LEAF table.
static int map_get_or_alloc_tables(struct t3_inode *node, uint32_t idx,
                                    uint32_t prefer_group,
                                    uint32_t *out_leaf_blk, uint32_t *out_leaf_slot,
                                    uint32_t *out_existing) {
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
        table = alloc_block(prefer_group, 0);
        if (!table) return 0;
        node->ptrs[ptr_index] = table;
        fresh = 1;
    }
    if (ptr_index == 12) {
        if (!pcache_load(table, fresh)) return 0;
        *out_leaf_blk = table;
        *out_leaf_slot = rem;
        *out_existing = rd32(g_pcache.buf + rem * 4);
        return 1;
    }

    // Middle level(s): walked with read-modify-write (rare compared
    // to leaf patches -- one RMW per 1024 (or 1024^2) data blocks).
    uint32_t levels = (ptr_index == 13) ? 1 : 2;
    uint32_t divisors[2] = { l1, 1 };
    if (levels == 2) { divisors[0] = l2; divisors[1] = l1; }
    for (uint32_t d = 0; d < levels; d++) {
        uint32_t slot = rem / divisors[d];
        rem = rem % divisors[d];
        if (fresh) k_memset(g_ptr_blk, 0, T3_BLOCK);
        else if (!read_block(table, g_ptr_blk)) return 0;
        uint32_t next = rd32(g_ptr_blk + slot * 4);
        int next_fresh = 0;
        if (!next) {
            next = alloc_block(prefer_group, 0);
            if (!next) return 0;
            wr32(g_ptr_blk + slot * 4, next);
            if (!write_block(table, g_ptr_blk)) return 0;
            next_fresh = 1;
        } else if (fresh) {
            // Can't happen (a fresh table has no entries), but keep
            // the write so a fresh middle table always lands on disk.
            if (!write_block(table, g_ptr_blk)) return 0;
        }
        table = next;
        fresh = next_fresh;
    }
    if (!pcache_load(table, fresh)) return 0;
    *out_leaf_blk = table;
    *out_leaf_slot = rem;
    *out_existing = rd32(g_pcache.buf + rem * 4);
    return 1;
}

static void map_set_block(struct t3_inode *node, uint32_t leaf_blk,
                          uint32_t leaf_slot, uint32_t data_blk) {
    if (!leaf_blk) {
        node->ptrs[leaf_slot] = data_blk; // direct
    } else {
        wr32(g_pcache.buf + leaf_slot * 4, data_blk);
        g_pcache.dirty = 1;
    }
}

// ---- write core ------------------------------------------------------------

// Write [offset, offset+len) into the file behind *node (whose inode
// number is `ino`), allocating as needed, then commit the updated
// inode through the journal. The order is the leak-safe one: bitmap
// state flushes BEFORE the inode transaction makes anything
// reachable. Returns 1/0.
static int do_write_inner(uint64_t ino, struct t3_inode *node, uint64_t offset,
                          const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t prefer_group = (uint32_t)(ino / g_sb.ipg);
    uint32_t last_alloc = 0;
    uint32_t total = 0;

    pcache_drop();
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > len - total) chunk = len - total;

        uint32_t leaf_blk, leaf_slot, existing;
        if (!map_get_or_alloc_tables(node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing))
            return 0;
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = alloc_block(prefer_group, last_alloc);
            if (!blk) return 0;
            map_set_block(node, leaf_blk, leaf_slot, blk);
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
            // same rule tfs.c's batching follows).
            uint32_t cap = (uint32_t)ata_max_sectors_per_xfer() / T3_SPB;
            if (cap < 1) cap = 1;
            if (want > cap) want = cap;
            while (run < want) {
                uint32_t nleaf, nslot, nexist;
                if (!map_get_or_alloc_tables(node, bi + run, prefer_group, &nleaf, &nslot, &nexist))
                    return 0;
                uint32_t nblk = nexist;
                if (!nblk) {
                    nblk = alloc_block(prefer_group, last_alloc);
                    if (!nblk) return 0;
                    map_set_block(node, nleaf, nslot, nblk);
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
            if (!vol_write_sectors(blk * T3_SPB, (int)(run * T3_SPB), src + total)) return 0;
            total += run * T3_BLOCK;
        } else {
            if (fresh || file_off >= node->size) k_memset(g_blk, 0, T3_BLOCK);
            else if (!read_block(blk, g_blk)) return 0;
            k_memcpy(g_blk + within, src + total, chunk);
            if (!write_block(blk, g_blk)) return 0;
            total += chunk;
        }
    }
    if (!pcache_flush()) return 0;
    pcache_drop();

    if (offset + len > node->size) node->size = offset + len;
    node->modified = now_epoch();

    if (!flush_alloc_state()) return 0;       // set-before-use
    if (!txn_stage_inode(ino, node)) { txn_reset(); return 0; }
    return txn_commit();                       // the commit point: file grows atomically
}

// The rollback shell: a runtime failure (refused write, out of space
// midway) frees everything this call allocated -- see the alloc-log
// comment. Only a CRASH is allowed to cost a leak.
static int do_write(uint64_t ino, struct t3_inode *node, uint64_t offset,
                    const void *buf, uint32_t len) {
    alog_begin();
    int ok = do_write_inner(ino, node, offset, buf, len);
    if (ok) {
        alog_commit();
    } else {
        pcache_drop();
        alog_rollback();
    }
    return ok;
}

// Free every data + pointer block behind *node (for truncate/delete),
// TRIMming as it goes, and leave the pointer fields zeroed. The
// caller must have ALREADY committed the inode/dirent transaction
// that makes these blocks unreachable -- clear-after-persist.
static void free_tree_level(uint32_t table_blk, int depth);

static void free_all_blocks(struct t3_inode *node) {
    uint32_t run_start = 0, run_len = 0;
    for (int i = 0; i < 12; i++) {
        uint32_t blk = node->ptrs[i];
        if (blk) {
            free_block_bit(blk);
            if (run_len && blk == run_start + run_len) run_len++;
            else { trim_run(run_start, run_len); run_start = blk; run_len = 1; }
        }
        node->ptrs[i] = 0;
    }
    trim_run(run_start, run_len);
    if (node->ptrs[12]) { free_tree_level(node->ptrs[12], 0); node->ptrs[12] = 0; }
    if (node->ptrs[13]) { free_tree_level(node->ptrs[13], 1); node->ptrs[13] = 0; }
    if (node->ptrs[14]) { free_tree_level(node->ptrs[14], 2); node->ptrs[14] = 0; }
}

// depth 0: entries are data blocks; deeper: entries are tables.
// Recursion depth is bounded at 3 by the format itself. Uses a local
// table copy (kmalloc) instead of the shared scratch because levels
// nest.
static void free_tree_level(uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl) return; // leak rather than corrupt -- fsck reclaims
    if (read_block(table_blk, tbl)) {
        uint32_t run_start = 0, run_len = 0;
        for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
            uint32_t e = rd32(tbl + i * 4);
            if (!e) continue;
            if (depth == 0) {
                free_block_bit(e);
                if (run_len && e == run_start + run_len) run_len++;
                else { trim_run(run_start, run_len); run_start = e; run_len = 1; }
            } else {
                free_tree_level(e, depth - 1);
            }
        }
        trim_run(run_start, run_len);
    }
    free_block_bit(table_blk);
    trim_run(table_blk, 1);
    kfree(tbl);
}

// ---- dirent editing (through the transaction) ------------------------------

// Find room for a new dirent in `dir` and stage the patched block.
// Grows the directory by one block IN ITS OWN transaction first when
// needed (an empty extra dir block is harmless if the follow-up
// transaction never lands -- two consistent states, no 5-slot
// transaction; see the design doc's journal section).
static int dirent_insert(uint64_t dir_ino, struct t3_inode *dir,
                         const char *name, uint32_t name_len, uint64_t child_ino) {
    uint32_t need = (7 + name_len + 3) & ~3u;
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = block_for_index(dir, b);
        if (!blk || !read_block(blk, g_blk)) return 0;
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(g_blk + off);
            uint16_t rec_len = rd16(g_blk + off + 4);
            uint8_t nl = g_blk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            uint32_t used = e_ino ? ((7u + nl + 3u) & ~3u) : 0;
            if (rec_len - used >= need) {
                uint8_t *img = txn_stage(blk);
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
    if (g_txn_count != 0) return 0; // grow needs its own txn; callers stage after insert only
    uint32_t prefer_group = (uint32_t)(dir_ino / g_sb.ipg);
    uint32_t newblk = alloc_block(prefer_group, 0);
    if (!newblk) return 0;
    k_memset(g_blk, 0, T3_BLOCK);
    wr16(g_blk + 4, (uint16_t)T3_BLOCK); // one free entry spanning the block
    if (!write_block(newblk, g_blk)) { free_block_bit(newblk); return 0; }

    // Wire it into the map. Directs cover 12 blocks; past that the
    // single-indirect table gets the pointer (RMW, no pcache needed
    // at dir scale).
    if (nblocks < 12) {
        dir->ptrs[nblocks] = newblk;
    } else {
        uint32_t table = dir->ptrs[12];
        int fresh = 0;
        if (!table) {
            table = alloc_block(prefer_group, 0);
            if (!table) { free_block_bit(newblk); return 0; }
            dir->ptrs[12] = table;
            fresh = 1;
        }
        if (fresh) k_memset(g_ptr_blk, 0, T3_BLOCK);
        else if (!read_block(table, g_ptr_blk)) { free_block_bit(newblk); return 0; }
        wr32(g_ptr_blk + (nblocks - 12) * 4, newblk);
        if (!write_block(table, g_ptr_blk)) { free_block_bit(newblk); return 0; }
    }
    dir->size += T3_BLOCK;
    dir->modified = now_epoch();
    if (!flush_alloc_state()) return 0;
    if (!txn_stage_inode(dir_ino, dir)) { txn_reset(); return 0; }
    if (!txn_commit()) return 0;
    // The grow just COMMITTED: its blocks are referenced by the
    // parent inode now, so they must survive any rollback of the
    // caller's still-pending transaction.
    alog_forget(newblk);
    if (dir->ptrs[12]) alog_forget(dir->ptrs[12]);

    // Now stage the actual insertion into the fresh block, in the
    // caller's transaction.
    uint8_t *img = txn_stage(newblk);
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
static int dirent_remove(struct t3_inode *dir, const char *name,
                         uint32_t name_len, uint64_t *out_child) {
    uint32_t nblocks = (uint32_t)((dir->size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = block_for_index(dir, b);
        if (!blk || !read_block(blk, g_blk)) return 0;
        uint32_t off = 0, prev_off = 0;
        int have_prev = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(g_blk + off);
            uint16_t rec_len = rd16(g_blk + off + 4);
            uint8_t nl = g_blk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
            if (e_ino && nl == name_len && k_memcmp(g_blk + off + 7, name, name_len) == 0) {
                uint8_t *img = txn_stage(blk);
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
static int split_parent(const char *norm, uint64_t *out_parent,
                        const char **out_name, uint32_t *out_len) {
    if (k_strcmp(norm, "/") == 0) return 0;
    const char *last = norm;
    for (const char *p = norm; *p; p++) if (*p == '/') last = p;
    uint32_t len = (uint32_t)k_strlen(last + 1);
    if (len == 0 || len > T3_NAME_MAX) return 0;
    char parent[T3_PATH_BUF];
    if (last == norm) { parent[0] = '/'; parent[1] = '\0'; }
    else {
        uint32_t plen = (uint32_t)(last - norm);
        if (plen >= T3_PATH_BUF) return 0;
        k_memcpy(parent, norm, plen);
        parent[plen] = '\0';
    }
    uint64_t pino = T3_INO_ROOT;
    if (k_strcmp(parent, "/") != 0 && !resolve(parent, &pino)) return 0;
    *out_parent = pino;
    *out_name = last + 1;
    *out_len = len;
    return 1;
}

// ---- probe / format / init -------------------------------------------------

static int tfs3_probe(void) {
    if (!ata_present()) return 0;
    set_flat_volume();
    return load_superblock(0);
}

// Erase every location the probe recognizes: the primary superblock
// sector AND both backups (positions derive from the volume size, the
// same way load_superblock()'s fallback finds them). See fs_ops.h's
// wipe contract for the mounted-a-corpse story that made this an op.
static int tfs3_wipe(void) {
    if (!ata_present()) return 1;
    set_flat_volume();
    uint8_t zero[ATA_SECTOR_SIZE];
    k_memset(zero, 0, sizeof(zero));
    int ok = vol_write_sectors(T3_SB_BLOCK * T3_SPB, 1, zero);
    uint32_t vol_blocks = g_vol.sector_count / T3_SPB;
    if (vol_blocks > T3_GROUP0) {
        uint32_t gc = (vol_blocks - T3_GROUP0) / T3_BPG;
        uint32_t groups[2];
        int n = backup_groups(gc, groups);
        for (int i = 0; i < n; i++) {
            uint32_t blk = T3_GROUP0 + (groups[i] + 1) * T3_BPG - 1;
            if (!vol_write_sectors(blk * T3_SPB, 1, zero)) ok = 0;
        }
    }
    return ok;
}

// Kernel-side format, kept in lockstep with tfs3_writer.py's
// cmd_format() -- one description of the layout, two writers of it.
static int tfs3_format(void) {
    if (!ata_present()) return 0;
    set_flat_volume();

    uint32_t vol_blocks = g_vol.sector_count / T3_SPB;
    if (vol_blocks <= T3_GROUP0 + T3_BPG) {
        klog_write("tfs3: volume too small for one block group -- not formatting\n");
        return 0;
    }
    uint32_t gc = (vol_blocks - T3_GROUP0) / T3_BPG;
    if (gc > T3_GDT_BLOCKS * T3_BLOCK / 16) gc = T3_GDT_BLOCKS * T3_BLOCK / 16;
    uint32_t ipg = T3_BPG * T3_BLOCK / T3_BYTES_PER_INODE;
    if (ipg > T3_BPG) ipg = T3_BPG;
    ipg = (ipg / T3_INODES_PER_SECTOR / T3_SPB) * T3_INODES_PER_SECTOR * T3_SPB; // whole table blocks
    uint32_t itb = ipg * T3_INODE_SIZE / T3_BLOCK;
    uint32_t meta = 2 + itb; // no checksum-table feature at format time (flags = 0)

    uint32_t groups[2];
    int nb = backup_groups(gc, groups);
    uint64_t now = 0;
    {
        struct rtc_time t;
        rtc_read_local(&t);
        now = tz_rtc_to_epoch(&t);
    }

    // Superblock image (one sector's worth, zero-padded to a block by
    // the caller writes below).
    uint8_t sb[ATA_SECTOR_SIZE];
    k_memset(sb, 0, sizeof(sb));
    sb[0] = 'T'; sb[1] = 'F'; sb[2] = 'S'; sb[3] = '3';
    sb[4] = T3_VERSION; sb[5] = 0;
    wr32(sb + 8, vol_blocks);
    wr32(sb + 12, T3_BPG);
    wr32(sb + 16, ipg);
    wr32(sb + 20, gc);
    wr32(sb + 24, T3_GROUP0);
    wr32(sb + 44, k_fnv1a(sb, 44));

    k_memset(g_blk, 0, T3_BLOCK);
    k_memcpy(g_blk, sb, ATA_SECTOR_SIZE);
    if (!write_block(T3_SB_BLOCK, g_blk)) return 0;

    // Empty journal header + zeroed image slots.
    k_memset(g_blk, 0, T3_BLOCK);
    g_blk[0] = 'J'; g_blk[1] = 'R'; g_blk[2] = 'N'; g_blk[3] = '3';
    wr32(g_blk + 44, k_fnv1a(g_blk, 44));
    if (!write_block(T3_JH_BLOCK, g_blk)) return 0;
    k_memset(g_blk, 0, T3_BLOCK);
    for (uint32_t i = 0; i < T3_JSLOTS; i++) {
        if (!write_block(T3_JDATA_BLOCK + i, g_blk)) return 0;
    }

    // Group descriptor table: free-count caches, checksummed each.
    // Built one block at a time (a full table is 64 KiB, bigger than
    // any scratch this kernel keeps around).
    uint32_t root_block = T3_GROUP0 + meta; // first data block of group 0
    for (uint32_t tb = 0; tb < T3_GDT_BLOCKS; tb++) {
        k_memset(g_blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= gc) break;
            uint32_t free_b = T3_BPG - meta;
            uint32_t free_i = ipg;
            for (int j = 0; j < nb; j++) {
                if (groups[j] == g) free_b -= T3_BACKUP_BLOCKS;
            }
            if (g == 0) { free_b -= 1; free_i -= 2; } // root dirent block; ino 0+1
            uint8_t *e = g_blk + i * 16;
            wr32(e, free_b);
            wr32(e + 4, free_i);
            wr32(e + 12, k_fnv1a(e, 12));
        }
        if (!write_block(T3_GDT_BLOCK + tb, g_blk)) return 0;
        // Backup GDT snapshots get the identical block.
        for (int j = 0; j < nb; j++) {
            uint32_t tail = T3_GROUP0 + (groups[j] + 1) * T3_BPG - 1;
            if (!write_block(tail - T3_GDT_BLOCKS + tb, g_blk)) return 0;
        }
    }

    // Per group: block bitmap, inode bitmap, zeroed inode table.
    for (uint32_t g = 0; g < gc; g++) {
        uint32_t base = group_base(g);
        k_memset(g_blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < meta; i++) g_blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        int is_backup = 0;
        for (int j = 0; j < nb; j++) if (groups[j] == g) is_backup = 1;
        if (is_backup) {
            for (uint32_t i = T3_BPG - T3_BACKUP_BLOCKS; i < T3_BPG; i++)
                g_blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
        if (g == 0) {
            uint32_t i = meta; // the root dirent block
            g_blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
        if (!write_block(base, g_blk)) return 0;

        k_memset(g_blk, 0, T3_BLOCK);
        if (g == 0) g_blk[0] |= 0x03; // ino 0 (null) + ino 1 (root)
        if (!write_block(base + 1, g_blk)) return 0;

        // Zero the inode table: a zero inode fails its checksum on
        // purpose, and stale-but-valid inodes from a previous TFS3
        // format must not survive into this one.
        k_memset(g_blk, 0, T3_BLOCK);
        for (uint32_t i = 0; i < itb; i++) {
            if (!write_block(base + 2 + i, g_blk)) return 0;
        }
    }

    // Root inode (ino 1, group 0) + its dirent block (. and ..).
    {
        uint8_t sec[ATA_SECTOR_SIZE];
        uint32_t table_lba = (T3_GROUP0 + 2) * T3_SPB;
        if (!vol_read_sectors(table_lba, 1, sec)) return 0;
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
        if (!vol_write_sectors(table_lba, 1, sec)) return 0;

        k_memset(g_blk, 0, T3_BLOCK);
        wr32(g_blk + 0, T3_INO_ROOT); wr16(g_blk + 4, 12); g_blk[6] = 1; g_blk[7] = '.';
        wr32(g_blk + 12, T3_INO_ROOT); wr16(g_blk + 16, (uint16_t)(T3_BLOCK - 12)); g_blk[18] = 2;
        g_blk[19] = '.'; g_blk[20] = '.';
        if (!write_block(root_block, g_blk)) return 0;
    }

    // Backup superblock copies, byte-identical to the primary.
    k_memset(g_blk, 0, T3_BLOCK);
    k_memcpy(g_blk, sb, ATA_SECTOR_SIZE);
    for (int j = 0; j < nb; j++) {
        uint32_t tail = T3_GROUP0 + (groups[j] + 1) * T3_BPG - 1;
        if (!write_block(tail, g_blk)) return 0;
    }

    ata_flush_now(); // one barrier so the whole format is durable before init() re-reads it
    klog_write("tfs3: formatted a fresh tfs3 filesystem (");
    klog_write_dec(gc); klog_write(" groups, ");
    klog_write_dec(ipg); klog_write(" inodes/group)\n");
    return 1;
}

static void unmount_state(void) {
    if (g_gd) { kfree(g_gd); g_gd = 0; }
    if (g_bbm) { kfree(g_bbm); g_bbm = 0; }
    if (g_ibm) { kfree(g_ibm); g_ibm = 0; }
    if (g_rotor) { kfree(g_rotor); g_rotor = 0; }
    ncache_flush();
    pcache_drop();
    txn_reset();
}

static int tfs3_init(void) {
    g_mounted = 0;
    unmount_state();
    if (!ata_present()) {
        // tfs3 has no RAM-only mode of its own -- that's the default
        // backend's job (vfs.c). Reaching here without a disk means
        // the policy layer chose us anyway; degrade honestly.
        klog_write("tfs3: no disk -- cannot mount\n");
        return 0;
    }
    set_flat_volume();
    if (load_superblock(1) != 1) {
        klog_write("tfs3: no valid superblock (primary or backup) -- not mounted\n");
        return 0;
    }
    if (g_sb.flags != 0) {
        // Feature bits this kernel doesn't implement yet (e.g. the
        // per-block checksum table's write half). Refusing beats
        // mounting read-write and silently rotting the feature's
        // state -- the capabilities-must-not-lie rule, applied to a
        // format.
        klog_write("tfs3: superblock declares feature bits this kernel doesn't support -- not mounted\n");
        return 0;
    }
    derive_geometry();
    replay_journal(); // before anything reads the structures a crash may have half-written

    g_gd = kmalloc(sizeof(struct t3_gd) * g_sb.gc);
    g_bbm = kmalloc((size_t)g_sb.gc * T3_BLOCK);
    g_ibm = kmalloc((size_t)g_sb.gc * T3_BLOCK);
    g_rotor = kmalloc(sizeof(uint32_t) * g_sb.gc);
    if (!g_gd || !g_bbm || !g_ibm || !g_rotor) { unmount_state(); return 0; }
    k_memset(g_bbm_dirty, 0, sizeof(g_bbm_dirty));
    k_memset(g_ibm_dirty, 0, sizeof(g_ibm_dirty));
    k_memset(g_gdt_dirty, 0, sizeof(g_gdt_dirty));
    for (uint32_t g = 0; g < g_sb.gc; g++) {
        g_rotor[g] = g_meta_off;
        if (!read_block(group_base(g), g_bbm + (size_t)g * T3_BLOCK) ||
            !read_block(group_base(g) + 1, g_ibm + (size_t)g * T3_BLOCK)) {
            klog_write("tfs3: bitmap read failed -- not mounted\n");
            unmount_state();
            return 0;
        }
    }
    uint32_t bad_gd = 0;
    for (uint32_t tb = 0; tb <= (g_sb.gc - 1) / (T3_BLOCK / 16); tb++) {
        if (!read_block(T3_GDT_BLOCK + tb, g_blk)) { kfree(g_gd); g_gd = 0; return 0; }
        for (uint32_t i = 0; i < T3_BLOCK / 16; i++) {
            uint32_t g = tb * (T3_BLOCK / 16) + i;
            if (g >= g_sb.gc) break;
            const uint8_t *e = g_blk + i * 16;
            if (k_fnv1a(e, 12) != rd32(e + 12)) {
                // A descriptor is a cache -- a bad one doesn't block
                // the mount, it blocks trusting the cache. Count it
                // as fully-used so nothing over-promises; fsck
                // (Stage D) recomputes and repairs. Log the first few
                // only -- a wiped-and-reused disk fails ALL of them,
                // and 71 identical lines helped nobody.
                static const uint32_t GD_LOG_CAP = 4;
                if (bad_gd < GD_LOG_CAP) {
                    klog_write("tfs3: group descriptor "); klog_write_dec(g);
                    klog_write(" failed its checksum -- treating its free counts as 0 until fsck\n");
                } else if (bad_gd == GD_LOG_CAP) {
                    klog_write("tfs3: ...more group descriptors failed -- run fsck\n");
                }
                bad_gd++;
                g_gd[g].free_blocks = 0;
                g_gd[g].free_inodes = 0;
            } else {
                g_gd[g].free_blocks = rd32(e);
                g_gd[g].free_inodes = rd32(e + 4);
            }
        }
    }

    // Sanity-check the root before declaring victory.
    struct t3_inode root;
    if (!read_inode(T3_INO_ROOT, &root) || root.type != T3_TYPE_DIR) {
        klog_write("tfs3: root inode invalid -- not mounted\n");
        unmount_state();
        return 0;
    }

    g_mounted = 1;
    klog_write("tfs3: mounted (");
    klog_write_dec(g_sb.gc); klog_write(" groups, ");
    klog_write_dec(g_sb.ipg); klog_write(" inodes/group)\n");
    return 1;
}

// ---- read-side ops ---------------------------------------------------------

static uint32_t read_range_impl(const struct t3_inode *node, uint64_t offset,
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
        uint32_t blk = block_for_index(node, bi);
        if (!blk) {
            // A hole reads as zeros (the format allows them even
            // though the Stage C writer never creates one).
            k_memset(dst + total, 0, chunk);
        } else {
            if (!read_block(blk, g_blk)) break;
            k_memcpy(dst + total, g_blk + within, chunk);
        }
        total += chunk;
    }
    return total;
}

static const char *tfs3_read(const char *path, uint32_t *out_size) {
    struct t3_inode node;
    if (!lookup(path, 0, &node) || node.type != T3_TYPE_FILE) return 0;
    if (node.size > 0xFFFFFFFFu - 1) return 0; // whole-buffer call -- use fs_read_range()
    uint32_t size = (uint32_t)node.size;

    if (g_read_buf) { kfree(g_read_buf); g_read_buf = 0; }
    g_read_buf = kmalloc((size_t)size + 1);
    if (!g_read_buf) return 0;
    if (size > 0 && read_range_impl(&node, 0, g_read_buf, size) != size) {
        kfree(g_read_buf); g_read_buf = 0;
        return 0;
    }
    ((uint8_t *)g_read_buf)[size] = 0;
    if (out_size) *out_size = size;
    return (const char *)g_read_buf;
}

static uint64_t tfs3_size(const char *path) {
    struct t3_inode node;
    if (!lookup(path, 0, &node) || node.type != T3_TYPE_FILE) return 0;
    return node.size;
}

static uint32_t tfs3_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct t3_inode node;
    if (!lookup(path, 0, &node) || node.type != T3_TYPE_FILE) return 0;
    return read_range_impl(&node, offset, buf, len);
}

struct t3_read_step {
    struct t3_inode node;
    uint8_t *dst;
    uint64_t offset;
    uint32_t len, total;
};

static void *tfs3_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct t3_inode node;
    if (!lookup(path, 0, &node) || node.type != T3_TYPE_FILE) return 0;
    if (offset >= node.size) len = 0;
    else {
        uint64_t avail = node.size - offset;
        if ((uint64_t)len > avail) len = (uint32_t)avail;
    }
    struct t3_read_step *st = kmalloc(sizeof(*st));
    if (!st) return 0;
    st->node = node;
    st->dst = (uint8_t *)buf;
    st->offset = offset;
    st->len = len;
    st->total = 0;
    return st;
}

static int tfs3_read_range_step(void *handle, uint32_t *out_total) {
    struct t3_read_step *st = (struct t3_read_step *)handle;
    if (st->total < st->len) {
        uint32_t got = read_range_impl(&st->node, st->offset + st->total,
                                       st->dst + st->total,
                                       // one block per step, same
                                       // pacing contract as tfs.c
                                       T3_BLOCK - (uint32_t)((st->offset + st->total) % T3_BLOCK) <= st->len - st->total
                                           ? T3_BLOCK - (uint32_t)((st->offset + st->total) % T3_BLOCK)
                                           : st->len - st->total);
        if (got == 0) {
            if (out_total) *out_total = st->total;
            kfree(st);
            return 2 /* FS_STEP_FAILED */;
        }
        st->total += got;
        if (st->total < st->len) {
            if (out_total) *out_total = st->total;
            return 0 /* FS_STEP_PENDING */;
        }
    }
    if (out_total) *out_total = st->total;
    kfree(st);
    return 1 /* FS_STEP_DONE */;
}

static int tfs3_is_dir(const char *path) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    struct t3_inode node;
    if (!lookup(path, 0, &node)) return 0;
    return node.type == T3_TYPE_DIR;
}

static int tfs3_exists(const char *path) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    uint64_t ino;
    return resolve(norm, &ino) ? 1 : 0;
}

static void tfs3_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    struct t3_inode dir;
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(dir_path, norm)) return;
    uint64_t ino = T3_INO_ROOT;
    if (k_strcmp(norm, "/") != 0 && !resolve(norm, &ino)) return;
    if (!read_inode(ino, &dir) || dir.type != T3_TYPE_DIR) return;

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = block_for_index(&dir, b);
        // Copy the dirent block out of g_blk before per-child inode
        // reads reuse the scratch.
        static uint8_t dirblk[T3_BLOCK];
        if (!blk || !read_block(blk, dirblk)) return;
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
                    if (read_inode(e_ino, &child)) {
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

static int tfs3_stat(const char *path, struct fs_stat_info *out) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0; // root has no entry -- same contract as tfs2
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(norm, &ino) || !read_inode(ino, &node)) return 0;
    if (out) {
        out->ino = ino;             // a real inode number -- FS_CAP_INODES
        out->created = node.created; // stored as epoch natively -- FS_CAP_EPOCH_TIME
        out->modified = node.modified;
    }
    return 1;
}

static int tfs3_disk_usage(uint64_t *out_used, uint64_t *out_total) {
    if (!g_mounted) {
        if (out_used) *out_used = 0;
        if (out_total) *out_total = 0;
        return 1;
    }
    // Same contract as fs.h: usable data space only, metadata excluded.
    uint32_t groups[2];
    int nb = backup_groups(g_sb.gc, groups);
    uint64_t total_data = 0, free_data = 0;
    for (uint32_t g = 0; g < g_sb.gc; g++) {
        uint32_t data = T3_BPG - g_meta_off;
        for (int j = 0; j < nb; j++) if (groups[j] == g) data -= T3_BACKUP_BLOCKS;
        total_data += data;
        free_data += g_gd[g].free_blocks;
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
static int create_entry_inner(const char *path, uint8_t type, uint64_t *out_ino);

static int create_entry(const char *path, uint8_t type, uint64_t *out_ino) {
    alog_begin();
    int ok = create_entry_inner(path, type, out_ino);
    if (ok) alog_commit(); else alog_rollback();
    return ok;
}

static int create_entry_inner(const char *path, uint8_t type, uint64_t *out_ino) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t existing;
    if (resolve(norm, &existing)) return 0; // caller decides what exists means
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(norm, &parent_ino, &name, &name_len)) return 0;
    struct t3_inode parent;
    if (!read_inode(parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    uint32_t prefer_group = (uint32_t)(parent_ino / g_sb.ipg);
    uint64_t ino = alloc_inode(prefer_group);
    if (!ino) return 0;
    // Block allocations below roll back via the alloc log (armed by
    // the create_entry() shell); the inode bit is freed by hand on
    // each failure path since the log only tracks blocks.

    struct t3_inode node;
    k_memset(&node, 0, sizeof(node));
    node.type = type;
    node.links = (type == T3_TYPE_DIR) ? 2 : 1;
    node.created = node.modified = now_epoch();

    if (type == T3_TYPE_DIR) {
        // The child's own dirent block: plain data until the inode
        // transaction lands, so a direct (unjournaled) write is safe.
        uint32_t blk = alloc_block(prefer_group, 0);
        if (!blk) { free_inode_bit(ino); flush_alloc_state(); return 0; }
        k_memset(g_blk, 0, T3_BLOCK);
        wr32(g_blk, (uint32_t)ino); wr16(g_blk + 4, 12); g_blk[6] = 1; g_blk[7] = '.';
        wr32(g_blk + 12, (uint32_t)parent_ino); wr16(g_blk + 16, (uint16_t)(T3_BLOCK - 12));
        g_blk[18] = 2; g_blk[19] = '.'; g_blk[20] = '.';
        if (!write_block(blk, g_blk)) { free_inode_bit(ino); free_block_bit(blk); flush_alloc_state(); return 0; }
        node.ptrs[0] = blk;
        node.size = T3_BLOCK;
    }

    if (!flush_alloc_state()) { free_inode_bit(ino); flush_alloc_state(); return 0; } // set-before-use

    txn_reset();
    int ins = dirent_insert(parent_ino, &parent, name, name_len, ino);
    if (!ins) { txn_reset(); free_inode_bit(ino); flush_alloc_state(); return 0; }
    if (!txn_stage_inode(ino, &node)) { txn_reset(); free_inode_bit(ino); flush_alloc_state(); return 0; }
    if (type == T3_TYPE_DIR) {
        parent.links++;
        parent.modified = node.created;
        if (!txn_stage_inode(parent_ino, &parent)) { txn_reset(); free_inode_bit(ino); flush_alloc_state(); return 0; }
    }
    if (!txn_commit()) { free_inode_bit(ino); flush_alloc_state(); return 0; }
    ncache_flush();
    if (out_ino) *out_ino = ino;
    return 1;
}

static int tfs3_touch(const char *path) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (resolve(norm, &ino)) {
        // Existing file: a no-op that succeeds; existing dir: refuse.
        // Matches tfs_touch()'s behavior exactly (incl. not bumping
        // `modified` -- see fs.h's fs_stat_info comment).
        if (!read_inode(ino, &node)) return 0;
        return node.type == T3_TYPE_FILE;
    }
    return create_entry(path, T3_TYPE_FILE, 0);
}

static int tfs3_mkdir(const char *path) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    if (resolve(norm, &ino)) return 0; // exists (file OR dir) -- refuse, like tfs2
    return create_entry(path, T3_TYPE_DIR, 0);
}

static int tfs3_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(norm, &ino)) {
        if (!create_entry(path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!read_inode(ino, &node) || node.type != T3_TYPE_FILE) return 0;
    if (len == 0) return 1;
    return do_write(ino, &node, offset, buf, len);
}

static int tfs3_write(const char *path, const char *data, int append) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(norm, &ino)) {
        if (!create_entry(path, T3_TYPE_FILE, &ino)) return 0;
    }
    if (!read_inode(ino, &node) || node.type != T3_TYPE_FILE) return 0;

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
        txn_reset();
        if (!txn_stage_inode(ino, &node)) { txn_reset(); return 0; }
        if (!txn_commit()) return 0;
        free_all_blocks(&old);
        flush_alloc_state();
        start = 0;
    } else if (!append) {
        start = 0;
    }
    if (len == 0) return 1;
    return do_write(ino, &node, start, data, len);
}

static int tfs3_delete(const char *path) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;
    uint64_t ino;
    struct t3_inode node;
    if (!resolve(norm, &ino) || !read_inode(ino, &node)) return 0;

    if (node.type == T3_TYPE_DIR) {
        // Empty means "nothing but . and .." -- the no-recursive-
        // delete policy, unchanged (docs/decisions.md).
        uint32_t nblocks = (uint32_t)((node.size + T3_BLOCK - 1) / T3_BLOCK);
        for (uint32_t b = 0; b < nblocks; b++) {
            uint32_t blk = block_for_index(&node, b);
            if (!blk || !read_block(blk, g_blk)) return 0;
            uint32_t off = 0;
            while (off + 8 <= T3_BLOCK) {
                uint32_t e_ino = rd32(g_blk + off);
                uint16_t rec_len = rd16(g_blk + off + 4);
                uint8_t nl = g_blk[off + 6];
                if (rec_len < 8 || off + rec_len > T3_BLOCK) break;
                if (e_ino && !(nl == 1 && g_blk[off + 7] == '.') &&
                    !(nl == 2 && g_blk[off + 7] == '.' && g_blk[off + 8] == '.')) {
                    return 0; // not empty
                }
                off += rec_len;
            }
        }
    }

    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(norm, &parent_ino, &name, &name_len)) return 0;
    struct t3_inode parent;
    if (!read_inode(parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    txn_reset();
    uint64_t removed = 0;
    if (!dirent_remove(&parent, name, name_len, &removed) || removed != ino) {
        txn_reset();
        return 0;
    }

    int gone = 0;
    if (node.type == T3_TYPE_DIR || node.links <= 1) {
        // Last name: zero the inode; blocks are freed after commit.
        if (!txn_stage_inode(ino, 0)) { txn_reset(); return 0; }
        gone = 1;
        if (node.type == T3_TYPE_DIR) {
            parent.links--;
            parent.modified = now_epoch();
            if (!txn_stage_inode(parent_ino, &parent)) { txn_reset(); return 0; }
        }
    } else {
        // A hardlink remains -- just drop the count.
        node.links--;
        if (!txn_stage_inode(ino, &node)) { txn_reset(); return 0; }
    }
    if (!txn_commit()) return 0;
    ncache_flush();

    if (gone) {
        // clear-after-persist: nothing references these anymore.
        free_all_blocks(&node);
        free_inode_bit(ino);
        flush_alloc_state();
    }
    return 1;
}

// The first OPTIONAL fs_ops op, gated by FS_CAP_HARDLINKS (the caps
// honesty check in vfs.c verifies the pair). Files only -- hardlinked
// directories turn the tree into a graph, refused by every real Unix
// filesystem for the same reason (see the design doc).
static int tfs3_link(const char *existing, const char *newpath) {
    char norm[T3_PATH_BUF], newnorm[T3_PATH_BUF];
    if (!g_mounted || !normalize(existing, norm) || !normalize(newpath, newnorm)) return 0;
    uint64_t ino, clash;
    struct t3_inode node;
    if (!resolve(norm, &ino) || !read_inode(ino, &node)) return 0;
    if (node.type != T3_TYPE_FILE) return 0;
    if (resolve(newnorm, &clash)) return 0; // target name taken
    uint64_t parent_ino;
    const char *name; uint32_t name_len;
    if (!split_parent(newnorm, &parent_ino, &name, &name_len)) return 0;
    struct t3_inode parent;
    if (!read_inode(parent_ino, &parent) || parent.type != T3_TYPE_DIR) return 0;

    alog_begin(); // dir growth inside dirent_insert can allocate
    txn_reset();
    if (!dirent_insert(parent_ino, &parent, name, name_len, ino)) { txn_reset(); alog_rollback(); return 0; }
    node.links++;
    if (!txn_stage_inode(ino, &node)) { txn_reset(); alog_rollback(); return 0; }
    if (!txn_commit()) { alog_rollback(); return 0; }
    alog_commit();
    ncache_flush();
    return 1;
}

// Steppable write: one block per step() call, inode committed once on
// the final step -- so a crash mid-stream leaks fresh blocks and
// leaves the file at its old size, same contract the blocking path
// gives (fs.h: on FS_STEP_DONE size/mtime/metadata are updated).
struct t3_write_step {
    uint64_t ino;
    struct t3_inode node;
    const uint8_t *src;
    uint64_t offset;
    uint32_t len, total;
    uint32_t last_alloc;
};

static void *tfs3_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    char norm[T3_PATH_BUF];
    if (!g_mounted || !normalize(path, norm)) return 0;
    uint64_t ino;
    if (!resolve(norm, &ino)) {
        if (!create_entry(path, T3_TYPE_FILE, &ino)) return 0;
    }
    struct t3_write_step *st = kmalloc(sizeof(*st));
    if (!st) return 0;
    if (!read_inode(ino, &st->node) || st->node.type != T3_TYPE_FILE) { kfree(st); return 0; }
    st->ino = ino;
    st->src = (const uint8_t *)buf;
    st->offset = offset;
    st->len = len;
    st->total = 0;
    st->last_alloc = 0;
    pcache_drop();
    return st;
}

static int tfs3_write_range_step(void *handle) {
    struct t3_write_step *st = (struct t3_write_step *)handle;
    // Rollback scope is THIS STEP only -- other fs operations
    // interleave between steps of an async write, so a whole-stream
    // log can't be kept armed. Earlier completed steps of an
    // abandoned stream leak by design (crash-shaped); fsck reclaims.
    alog_begin();
    if (st->total < st->len) {
        uint64_t file_off = st->offset + st->total;
        uint32_t bi = (uint32_t)(file_off / T3_BLOCK);
        uint32_t within = (uint32_t)(file_off % T3_BLOCK);
        uint32_t chunk = T3_BLOCK - within;
        if (chunk > st->len - st->total) chunk = st->len - st->total;

        uint32_t prefer_group = (uint32_t)(st->ino / g_sb.ipg);
        uint32_t leaf_blk, leaf_slot, existing;
        if (!map_get_or_alloc_tables(&st->node, bi, prefer_group, &leaf_blk, &leaf_slot, &existing)) {
            pcache_drop(); alog_rollback(); kfree(st); return 2 /* FS_STEP_FAILED */;
        }
        uint32_t blk = existing;
        int fresh = 0;
        if (!blk) {
            blk = alloc_block(prefer_group, st->last_alloc);
            if (!blk) { pcache_drop(); alog_rollback(); kfree(st); return 2; }
            map_set_block(&st->node, leaf_blk, leaf_slot, blk);
            fresh = 1;
        }
        st->last_alloc = blk;

        int ok;
        if (chunk == T3_BLOCK) {
            ok = write_block(blk, st->src + st->total);
        } else {
            if (fresh || file_off >= st->node.size) {
                k_memset(g_blk, 0, T3_BLOCK);
            } else if (!read_block(blk, g_blk)) {
                pcache_drop(); alog_rollback(); kfree(st); return 2;
            }
            k_memcpy(g_blk + within, st->src + st->total, chunk);
            ok = write_block(blk, g_blk);
        }
        if (!ok) { pcache_drop(); alog_rollback(); kfree(st); return 2; }
        st->total += chunk;
        if (st->total < st->len) return 0 /* FS_STEP_PENDING */;
    }

    // Final step: land the pointer cache, the allocation state, and
    // the inode -- the same commit point do_write() has.
    if (!pcache_flush()) { pcache_drop(); alog_rollback(); kfree(st); return 2; }
    pcache_drop();
    if (st->offset + st->len > st->node.size) st->node.size = st->offset + st->len;
    st->node.modified = now_epoch();
    int ok = flush_alloc_state();
    if (ok) {
        txn_reset();
        ok = txn_stage_inode(st->ino, &st->node) && txn_commit();
    }
    if (ok) alog_commit(); else alog_rollback();
    kfree(st);
    return ok ? 1 /* FS_STEP_DONE */ : 2;
}

// ---- fsck -----------------------------------------------------------------
//
// One pass: walk the namespace from the root, mark every reachable
// block and inode, then reconcile against the allocation bitmaps.
// Result-field mapping keeps fs_check_result's TFS2-era meanings:
// records_used = reachable inodes, leaked/referenced_but_free/
// double_allocated/out_of_range = blocks, exactly as fs.h documents.
// Repair semantics follow the same rules as tfs.c's: reclaim leaks
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

static int fsck_block_ok(uint32_t blk) {
    if (blk < T3_GROUP0 + g_meta_off) return 0;
    uint32_t g = (blk - T3_GROUP0) / T3_BPG;
    uint32_t i = (blk - T3_GROUP0) % T3_BPG;
    if (g >= g_sb.gc) return 0;
    if (i < g_meta_off || i >= group_data_end(g)) return 0;
    return 1;
}

// Mark one referenced block; counts out-of-range and doubles.
// Returns 1 if the block is usable (in range, first reference).
static int fsck_mark_block(struct t3_fsck *fk, uint32_t blk) {
    if (!fsck_block_ok(blk)) { fk->r->out_of_range++; return 0; }
    uint32_t idx = blk - T3_GROUP0;
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
static void fsck_walk_table(struct t3_fsck *fk, uint32_t table_blk, int depth) {
    uint8_t *tbl = kmalloc(T3_BLOCK);
    if (!tbl || !read_block(table_blk, tbl)) { if (tbl) kfree(tbl); return; }
    int dirty = 0;
    for (uint32_t i = 0; i < T3_PTRS_PER_BLOCK; i++) {
        uint32_t e = rd32(tbl + i * 4);
        if (!e) continue;
        if (!fsck_block_ok(e)) {
            fk->r->out_of_range++;
            if (fk->repair) { wr32(tbl + i * 4, 0); dirty = 1; fk->r->pointers_cleared++; }
            continue;
        }
        if (depth == 0) fsck_mark_block(fk, e);
        else if (fsck_mark_block(fk, e)) fsck_walk_table(fk, e, depth - 1);
    }
    if (dirty) write_block(table_blk, tbl);
    kfree(tbl);
}

static void fsck_walk_inode_blocks(struct t3_fsck *fk, uint64_t ino, struct t3_inode *node) {
    int inode_dirty = 0;
    for (int i = 0; i < 12; i++) {
        if (!node->ptrs[i]) continue;
        if (!fsck_block_ok(node->ptrs[i])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[i] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else {
            fsck_mark_block(fk, node->ptrs[i]);
        }
    }
    for (int p = 12; p <= 14; p++) {
        if (!node->ptrs[p]) continue;
        if (!fsck_block_ok(node->ptrs[p])) {
            fk->r->out_of_range++;
            if (fk->repair) { node->ptrs[p] = 0; inode_dirty = 1; fk->r->pointers_cleared++; }
        } else if (fsck_mark_block(fk, node->ptrs[p])) {
            fsck_walk_table(fk, node->ptrs[p], p - 12);
        }
    }
    if (inode_dirty) {
        txn_reset();
        if (txn_stage_inode(ino, node)) txn_commit(); else txn_reset();
    }
}

static void fsck_mark_ino(struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / g_sb.ipg), i = (uint32_t)(ino % g_sb.ipg);
    if (g >= g_sb.gc) return;
    fk->ireach[(size_t)g * T3_BLOCK + (i >> 3)] |= (uint8_t)(1u << (i & 7));
}

static int fsck_ino_reached(struct t3_fsck *fk, uint64_t ino) {
    uint32_t g = (uint32_t)(ino / g_sb.ipg), i = (uint32_t)(ino % g_sb.ipg);
    if (g >= g_sb.gc) return 1;
    return (fk->ireach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
}

// Depth-capped DFS over the directory tree. 32 components is far past
// anything the 64-byte caller paths can even express today; a deeper
// tree gets a klog and an unwalked subtree (reported as leaks --
// wrong, but loudly wrong).
static void fsck_walk_dir(struct t3_fsck *fk, uint64_t dir_ino, uint64_t parent_ino, int depth) {
    if (depth > 32) {
        klog_write("tfs3 fsck: directory nesting past 32 -- subtree not walked\n");
        return;
    }
    struct t3_inode dir;
    if (!read_inode(dir_ino, &dir) || dir.type != T3_TYPE_DIR) return;
    fk->r->records_used++;
    fsck_walk_inode_blocks(fk, dir_ino, &dir);

    uint32_t nblocks = (uint32_t)((dir.size + T3_BLOCK - 1) / T3_BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = block_for_index(&dir, b);
        uint8_t *dirblk = kmalloc(T3_BLOCK);
        if (!dirblk) return;
        if (!blk || !read_block(blk, dirblk)) { kfree(dirblk); continue; }
        uint32_t off = 0;
        while (off + 8 <= T3_BLOCK) {
            uint32_t e_ino = rd32(dirblk + off);
            uint16_t rec_len = rd16(dirblk + off + 4);
            uint8_t nl = dirblk[off + 6];
            if (rec_len < 8 || off + rec_len > T3_BLOCK) {
                klog_write("tfs3 fsck: corrupt dirent chain in inode ");
                klog_write_dec((uint32_t)dir_ino); klog_write("\n");
                break;
            }
            if (e_ino != 0 && nl > 0) {
                int is_dot = (nl == 1 && dirblk[off + 7] == '.');
                int is_dotdot = (nl == 2 && dirblk[off + 7] == '.' && dirblk[off + 8] == '.');
                uint64_t child = e_ino;
                if (child < (uint64_t)g_sb.gc * g_sb.ipg) {
                    uint8_t *nc = &fk->names[child];
                    if (*nc < 255) (*nc)++;
                }
                if (is_dot) {
                    if (child != dir_ino) klog_write("tfs3 fsck: `.` points away from its own directory\n");
                } else if (is_dotdot) {
                    if (child != parent_ino) klog_write("tfs3 fsck: `..` points away from the parent\n");
                } else {
                    struct t3_inode cn;
                    if (!read_inode(child, &cn)) {
                        klog_write("tfs3 fsck: dirent -> inode ");
                        klog_write_dec((uint32_t)child);
                        klog_write(" whose checksum fails (not repaired -- deleting a name is data loss)\n");
                    } else if (fsck_ino_reached(fk, child)) {
                        // Already visited: fine for files (hardlink),
                        // never for dirs.
                        if (cn.type == T3_TYPE_DIR)
                            klog_write("tfs3 fsck: directory reachable by two names\n");
                    } else {
                        fsck_mark_ino(fk, child);
                        if (cn.type == T3_TYPE_DIR) {
                            fsck_walk_dir(fk, child, dir_ino, depth + 1);
                        } else {
                            fk->r->records_used++;
                            fsck_walk_inode_blocks(fk, child, &cn);
                        }
                    }
                }
            }
            off += rec_len;
        }
        kfree(dirblk);
    }
}

static int tfs3_check(int repair, struct fs_check_result *out) {
    struct fs_check_result local;
    struct fs_check_result *r = out ? out : &local;
    k_memset(r, 0, sizeof(*r));
    if (!g_mounted) return 0;

    struct t3_fsck fk;
    k_memset(&fk, 0, sizeof(fk));
    fk.r = r;
    fk.repair = repair;
    size_t bmbytes = (size_t)g_sb.gc * T3_BLOCK;
    uint64_t total_inodes = (uint64_t)g_sb.gc * g_sb.ipg;
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

    fsck_mark_ino(&fk, T3_INO_ROOT);
    fsck_walk_dir(&fk, T3_INO_ROOT, T3_INO_ROOT, 0);

    // Reconcile blocks: reach map vs allocation bitmap, per group.
    // Metadata and backup regions are allocated-by-design and outside
    // the reach map, so only the data area is compared.
    for (uint32_t g = 0; g < g_sb.gc; g++) {
        uint32_t end = group_data_end(g);
        uint32_t free_b = 0;
        uint32_t leak_run_start = 0, leak_run_len = 0;
        for (uint32_t i = 0; i < T3_BPG; i++) {
            int alloc = bbm_test(g, i);
            if (i < g_meta_off || i >= end) continue; // format-owned
            int reach = (fk.breach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
            if (alloc && !reach) {
                r->leaked++;
                if (repair) {
                    bbm_set(g, i, 0);
                    r->reclaimed++;
                    uint32_t blk = group_base(g) + i;
                    if (leak_run_len && blk == leak_run_start + leak_run_len) leak_run_len++;
                    else { trim_run(leak_run_start, leak_run_len); leak_run_start = blk; leak_run_len = 1; }
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { bbm_set(g, i, 1); r->marked_allocated++; }
            }
            if (!bbm_test(g, i)) free_b++;
        }
        if (repair) trim_run(leak_run_start, leak_run_len);

        // Inode bitmap + free counts: reconcile, repair-only writes.
        uint32_t free_i = 0;
        for (uint32_t i = 0; i < g_sb.ipg; i++) {
            uint64_t ino = (uint64_t)g * g_sb.ipg + i;
            int alloc = ibm_test(g, i);
            int reach = (fk.ireach[(size_t)g * T3_BLOCK + (i >> 3)] >> (i & 7)) & 1;
            if (g == 0 && i == 0) { free_i += !alloc; continue; } // ino 0 reserved
            if (alloc && !reach) {
                // An orphaned inode is the inode-space leak. Reported
                // through the same counter (they are the same failure
                // class); reclaimed on repair.
                r->leaked++;
                if (repair) {
                    ibm_set(g, i, 0);
                    r->reclaimed++;
                    txn_reset();
                    if (txn_stage_inode(ino, 0)) txn_commit(); else txn_reset();
                }
            } else if (!alloc && reach) {
                r->referenced_but_free++;
                if (repair) { ibm_set(g, i, 1); r->marked_allocated++; }
            }
            if (!ibm_test(g, i)) free_i++;
        }

        if (g_gd[g].free_blocks != free_b || g_gd[g].free_inodes != free_i) {
            if (repair) {
                g_gd[g].free_blocks = free_b;
                g_gd[g].free_inodes = free_i;
                mark_dirty(g_gdt_dirty, g);
            }
        }
    }

    // Link counts: observed names vs stored counts. A directory's
    // observed count from the walk is its own dirent + `.` + each
    // child's `..`, which is exactly the 2+subdirs rule -- so one
    // comparison covers both types.
    for (uint64_t ino = 1; ino < total_inodes; ino++) {
        if (!fsck_ino_reached(&fk, ino) && ino != T3_INO_ROOT) continue;
        if (!fk.names[ino] && ino != T3_INO_ROOT) continue;
        struct t3_inode node;
        if (!read_inode(ino, &node)) continue;
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
                txn_reset();
                if (txn_stage_inode(ino, &node) && txn_commit()) klog_write(" -- repaired");
                else txn_reset();
            }
            klog_write("\n");
        }
    }

    if (repair) {
        flush_alloc_state();
        // A primary superblock that failed at mount (we're running
        // from a backup) gets rewritten now, deliberately here and
        // never automatically at mount -- see the design doc.
        if (g_mounted_from_backup) {
            if (write_superblock_everywhere()) {
                klog_write("tfs3 fsck: primary superblock restored from the mounted backup\n");
                g_mounted_from_backup = 0;
            }
        }
    }

    kfree(fk.breach);
    kfree(fk.ireach);
    kfree(fk.names);
    return 1;
}

const struct fs_ops tfs3_ops = {
    .name = "tfs3",
    // Format truths, not implementation status: inodes and epoch
    // timestamps are live already; hardlinks/symlinks are carried by
    // the format (link counts, type 2) with their ops still to come --
    // see fs.h's FS_CAP_* comment on exactly this distinction.
    .caps = FS_CAP_INODES | FS_CAP_HARDLINKS | FS_CAP_SYMLINKS | FS_CAP_EPOCH_TIME,
    .probe = tfs3_probe,
    .wipe = tfs3_wipe,
    .format = tfs3_format,
    .init = tfs3_init,
    .touch = tfs3_touch,
    .write = tfs3_write,
    .mkdir = tfs3_mkdir,
    .del = tfs3_delete,
    .read = tfs3_read,
    .size = tfs3_size,
    .read_range = tfs3_read_range,
    .write_range = tfs3_write_range,
    .write_range_begin = tfs3_write_range_begin,
    .write_range_step = tfs3_write_range_step,
    .read_range_begin = tfs3_read_range_begin,
    .read_range_step = tfs3_read_range_step,
    .is_dir = tfs3_is_dir,
    .exists = tfs3_exists,
    .list = tfs3_list,
    .stat = tfs3_stat,
    .disk_usage = tfs3_disk_usage,
    .check = tfs3_check,
    .link = tfs3_link, // optional op, paired with FS_CAP_HARDLINKS above
};
