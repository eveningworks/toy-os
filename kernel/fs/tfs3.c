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
static uint32_t g_meta_off;   // first data-ish block offset within a group (2 [+cksum] + itb)
static struct t3_gd *g_gd = 0; // free-count caches, g_sb.gc entries

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
    if (parse_superblock(sec)) return 1;

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
// NUL requirement). Returns the child inode or 0.
static uint64_t dir_lookup(const struct t3_inode *dir, const char *name, uint32_t name_len) {
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
        ino = dir_lookup(&dir, start, len);
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

static int tfs3_init(void) {
    g_mounted = 0;
    if (g_gd) { kfree(g_gd); g_gd = 0; }
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
    derive_geometry();

    g_gd = kmalloc(sizeof(struct t3_gd) * g_sb.gc);
    if (!g_gd) return 0;
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
        kfree(g_gd); g_gd = 0;
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

// ---- mutating ops: Stage C ---------------------------------------------

static void write_path_pending(void) {
    klog_write("tfs3: write path not built yet (Stage C) -- operation refused\n");
}

static int tfs3_touch(const char *path) { (void)path; write_path_pending(); return 0; }
static int tfs3_write(const char *path, const char *data, int append) {
    (void)path; (void)data; (void)append; write_path_pending(); return 0;
}
static int tfs3_mkdir(const char *path) { (void)path; write_path_pending(); return 0; }
static int tfs3_delete(const char *path) { (void)path; write_path_pending(); return 0; }
static int tfs3_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    (void)path; (void)offset; (void)buf; (void)len; write_path_pending(); return 0;
}
static void *tfs3_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    (void)path; (void)offset; (void)buf; (void)len; write_path_pending(); return 0;
}
static int tfs3_write_range_step(void *handle) { (void)handle; return 2 /* FS_STEP_FAILED */; }

// fsck: Stage D. Returning 0 means "nothing on disk to check", which
// is not quite honest for a mounted read-only tfs3 -- accepted as a
// temporary lie with a klog, replaced when the real walker lands.
static int tfs3_check(int repair, struct fs_check_result *out) {
    (void)repair;
    if (out) k_memset(out, 0, sizeof(*out));
    if (g_mounted) klog_write("tfs3: fsck not built yet (Stage D)\n");
    return 0;
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
};
