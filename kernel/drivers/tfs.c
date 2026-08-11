// TFS2 -- this OS's persistent filesystem, journaled and timestamped
// as of build 480, and (as of the large-file rework -- see
// CHANGELOG.md) block-addressed with indirect pointers instead of one
// fixed-size inline data blob per file. This used to be fs.c itself,
// back when it was the only filesystem toy-os could have; it's now
// just one backend behind the VFS dispatch layer (vfs.c), reachable
// only through the `tfs_ops` vtable at the bottom of this file (see
// fs_ops.h for what that interface is and why it exists, and
// kernel/include/tfs.h for this file's own public surface). Nothing
// outside vfs.c should #include tfs.h or call anything in this file
// directly -- go through fs.h's fs_* API instead, same as before this
// split.
//
// *** IMPORTANT: this format is NOT compatible with the previous TFS2
// *** layout (inline 2048-byte-per-file data, no block allocator).
// *** The on-disk version byte was bumped specifically so an old disk
// *** is detected as foreign and reformatted from scratch, same "no
// *** migration, just reformat" policy this project has always used
// *** for format changes (see the superblock comment below) --
// *** existing files WILL be lost the first time this boots against
// *** an old disk.img. This also needs a much bigger disk.img than
// *** before (the old 1MB image can't hold the new bitmap region) --
// *** see the Makefile and docs/decisions.md.
//
// In-memory file table (`files[]`) still holds every file/directory's
// METADATA (path, type, size, timestamps) for every existing caller
// that only needs that (fs_list, fs_stat, fs_exists, ...) -- but a
// file's actual DATA no longer lives inline in this table. Instead
// each entry holds a small set of block-number pointers (direct +
// single/double/triple indirect, classic Unix-inode shape) into a
// block-addressed region of the disk, and data is only ever read into
// RAM a block (or a caller-requested range) at a time. This is what
// makes a multi-gigabyte file possible at all: the whole point is that
// nothing needs to hold the whole file in memory at once (see fs.h's
// fs_read() vs. fs_read_range()/fs_write_range() doc comments).
//
// Directories are still just another entry with no data, not a
// separate on-disk structure -- unchanged from before this rework.
//
// On-disk layout (only meaningful if this backend reports persistent):
//   LBA 0:                     superblock (1 sector): magic "TFS2" +
//                               version byte 2 (bumped from 1 -- see
//                               the warning above).
//   LBA 1:                     journal header (1 sector).
//   LBA 2 .. +FS_RECORD_SECTORS-1:
//                               journal data area (one record's worth
//                               -- much smaller now that a record is
//                               just metadata + pointers, not 2048
//                               bytes of inline data).
//   FS_TABLE_START_LBA onward: FS_MAX_FILES fixed-size records.
//   FS_BITMAP_START_LBA onward: the free-block bitmap, one bit per
//                               FS_BLOCK_SIZE-byte block of the WHOLE
//                               disk (including the reserved region
//                               below it -- see FS_DATA_START_BLOCK).
//   FS_DATA_START_LBA onward:  block-addressed file data. Block number
//                               0 is reserved (used as the "no block"
//                               null pointer value in records), and
//                               every block before FS_DATA_START_BLOCK
//                               is permanently marked allocated at
//                               format time so the allocator can never
//                               hand out a block that actually holds
//                               superblock/journal/table/bitmap data.
//
// Block addressing: FS_BLOCK_SIZE (4096 bytes = FS_BLOCK_SECTORS
// sectors) chosen to exactly match ATA_MAX_SECTORS_PER_XFER (see
// ata.h) -- every block read/write is exactly one multi-sector ATA
// command, not up to 8 separate ones. Each record has FS_N_DIRECT
// direct block pointers plus single/double/triple indirect pointers
// (an "indirect block" is just FS_PTRS_PER_BLOCK block-number entries
// packed into one block) -- the same scheme real Unix filesystems have
// used for decades, chosen because it needs no separate free-space
// search structure beyond the bitmap and scales from a tiny file (a
// few direct blocks) up to hundreds of gigabytes (triple indirect)
// without a different code path for "big" vs. "small" files. See
// block_for_index()/walk_indirect() below for the actual pointer walk,
// and docs/decisions.md for why indirect blocks specifically (vs. a
// flat extent list) and why direct+single+double wasn't enough on its
// own (tops out around 1GB -- short of the 8GB target this was built
// for).
//
// Journaling: unchanged in spirit from before -- write_journal_header/
// persist_record() protect one table-slot RECORD (metadata + block
// pointers) from a torn write, exactly as before. What's NEW: this no
// longer also protects a file's actual DATA, because data isn't part
// of the record anymore -- block and indirect-block writes (see
// write_block()/walk_indirect()) go straight to disk, un-journaled.
// This is a real, honest gap: a crash mid-write to a large file could
// leave a data or indirect block partially written, or a pointer set
// before its target block's content was durable. The top-level
// record itself (and therefore the file's *size* and top block
// pointers) still can't end up torn/corrupted -- just possibly
// pointing at a block whose content wasn't the last thing written to
// it. Fixing this fully would mean journaling arbitrary-sized writes,
// which is a meaningfully bigger scheme (see docs/decisions.md); not
// attempted here.
//
// Honest limitations, not solved here:
//   - The data-write journaling gap above.
//   - Still no locking against two mutations racing each other --
//     unchanged from before, still not a real risk today (see the
//     original build-480 reasoning).
//   - No recursive delete: tfs_delete() on a non-empty directory fails
//     outright rather than deleting its contents. Deliberate -- see
//     fs.h.
//   - This file only validates that a path is already *normalized*
//     (path_is_normalized()) -- resolving ".."/"." or a cwd-relative
//     path is the caller's job (see the shell's `cd`/`pwd`).
//   - The "no real disk found" RAM-only fallback (see tfs_init()) now
//     backs block storage with individually kzalloc()'d chunks instead
//     of a disk -- capped at RAM_ONLY_MAX_BLOCKS (16MB total) rather
//     than the disk-backed bitmap's much larger range, since this mode
//     only ever existed as a graceful degrade for "no disk found," not
//     a real large-file backend. A multi-GB file needs a real disk.
#include "fs.h"
#include "tfs.h"
#include "string.h"
#include "ata.h"
#include "klog.h"
#include "tz.h"
#include "heap.h"

enum fs_entry_type { FS_TYPE_FILE = 0, FS_TYPE_DIR = 1 };

// Classic Unix-inode shape: FS_N_DIRECT direct block pointers, then
// single/double/triple indirect for anything bigger. See this file's
// top comment for the capacity math.
#define FS_N_DIRECT 12

struct file {
    char path[FS_PATH_MAX];
    uint8_t type; // FS_TYPE_FILE or FS_TYPE_DIR
    int used;
    uint64_t size;
    struct rtc_time created;
    struct rtc_time modified;
    uint32_t direct[FS_N_DIRECT];
    uint32_t single_indirect;
    uint32_t double_indirect;
    uint32_t triple_indirect;
};

static struct file files[FS_MAX_FILES];
static int g_disk_backed = 0;

// ---- block geometry ----

#define FS_BLOCK_SIZE 4096
#define FS_BLOCK_SECTORS (FS_BLOCK_SIZE / ATA_SECTOR_SIZE) // 8 -- matches ATA_MAX_SECTORS_PER_XFER exactly
#define FS_PTRS_PER_BLOCK (FS_BLOCK_SIZE / 4) // 1024 block numbers (uint32_t each) per indirect block

// Total disk size this backend assumes -- MUST match (or be smaller
// than) disk.img's real size, or writes past the real image will fail
// once QEMU's emulated drive reports a smaller capacity than this. See
// the Makefile's DISK_IMG rule and docs/decisions.md. Kept as one
// constant rather than queried at runtime because ata.c has no
// IDENTIFY-based capacity query today (see its own top comment) --
// consistent with the rest of this kernel's "fixed compile-time
// limits, not runtime-detected" style (FS_MAX_FILES, MAX_WINDOWS, ...).
#define FS_DISK_TOTAL_BYTES (9ULL * 1024 * 1024 * 1024) // 9 GiB
#define FS_DISK_TOTAL_SECTORS (FS_DISK_TOTAL_BYTES / ATA_SECTOR_SIZE)
#define FS_DISK_TOTAL_BLOCKS ((uint32_t)(FS_DISK_TOTAL_SECTORS / FS_BLOCK_SECTORS))

// ---- RAM-only fallback geometry (see tfs_init()'s "no disk" path) ----
#define RAM_ONLY_MAX_BLOCKS 4096 // 16MB total -- degrade mode only, see top comment

static uint8_t *g_ram_blocks[RAM_ONLY_MAX_BLOCKS]; // lazily kzalloc()'d, NULL = never written
static uint8_t g_ram_bitmap[(RAM_ONLY_MAX_BLOCKS + 7) / 8];
static uint32_t g_ram_scan_hint = 1; // block 0 reserved as the null sentinel, same convention as disk mode

// "TFS2" -- see top comment. Version byte tracks revisions of the TFS2
// layout itself; bumped from 1 to 2 for the block-addressed rework
// (see the warning at the top of this file).
#define FS_DISK_MAGIC0 'T'
#define FS_DISK_MAGIC1 'F'
#define FS_DISK_MAGIC2 'S'
#define FS_DISK_MAGIC3 '2'
#define FS_DISK_VERSION 2
#define FS_SUPERBLOCK_LBA 0

#define FS_RTC_BYTES 7 // hour, minute, second, day, month (1 byte each) + year (uint16 LE)
// path + type + used + size(8) + created + modified + direct(FS_N_DIRECT*4) + 3 indirect ptrs(4 each)
#define FS_RECORD_RAW_BYTES (FS_PATH_MAX + 1 + 1 + 8 + FS_RTC_BYTES * 2 + FS_N_DIRECT * 4 + 3 * 4)
#define FS_RECORD_SECTORS ((FS_RECORD_RAW_BYTES + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
#define FS_RECORD_BYTES (FS_RECORD_SECTORS * ATA_SECTOR_SIZE)

#define REC_OFF_TYPE      FS_PATH_MAX
#define REC_OFF_USED      (FS_PATH_MAX + 1)
#define REC_OFF_SIZE      (FS_PATH_MAX + 2)
#define REC_OFF_CREATED   (REC_OFF_SIZE + 8)
#define REC_OFF_MODIFIED  (REC_OFF_CREATED + FS_RTC_BYTES)
#define REC_OFF_DIRECT    (REC_OFF_MODIFIED + FS_RTC_BYTES)
#define REC_OFF_SINGLE    (REC_OFF_DIRECT + FS_N_DIRECT * 4)
#define REC_OFF_DOUBLE    (REC_OFF_SINGLE + 4)
#define REC_OFF_TRIPLE    (REC_OFF_DOUBLE + 4)

#define FS_JOURNAL_HEADER_LBA (FS_SUPERBLOCK_LBA + 1)
#define FS_JOURNAL_DATA_LBA   (FS_JOURNAL_HEADER_LBA + 1)
#define FS_TABLE_START_LBA    (FS_JOURNAL_DATA_LBA + FS_RECORD_SECTORS)
#define FS_BITMAP_START_LBA   (FS_TABLE_START_LBA + (uint32_t)FS_MAX_FILES * FS_RECORD_SECTORS)
#define FS_BITMAP_BYTES       ((FS_DISK_TOTAL_BLOCKS + 7) / 8)
#define FS_BITMAP_SECTORS     ((FS_BITMAP_BYTES + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
#define FS_DATA_START_LBA_RAW (FS_BITMAP_START_LBA + FS_BITMAP_SECTORS)
// Round up to a whole block boundary so block number <-> LBA is a
// clean multiply, and mark every block below this as reserved (see
// tfs_init()'s format path).
#define FS_DATA_START_BLOCK   ((FS_DATA_START_LBA_RAW + FS_BLOCK_SECTORS - 1) / FS_BLOCK_SECTORS)
#define FS_DATA_START_LBA     (FS_DATA_START_BLOCK * FS_BLOCK_SECTORS)

static uint32_t record_lba(int index) {
    return FS_TABLE_START_LBA + (uint32_t)index * FS_RECORD_SECTORS;
}

// The free-block bitmap -- one bit per block of the WHOLE disk (see
// FS_DATA_START_BLOCK's comment for why blocks below it are pre-marked
// allocated rather than excluded from the bitmap entirely). Kept as a
// static array (not kmalloc'd) since it's needed before/independent of
// heap_init() ordering concerns, and its size is a compile-time
// constant either way.
static uint8_t g_bitmap[FS_BITMAP_BYTES];
static uint32_t g_bitmap_scan_hint = FS_DATA_START_BLOCK;

static int bit_test(const uint8_t *bitmap, uint32_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}
static void bit_set(uint8_t *bitmap, uint32_t bit, int val) {
    if (val) bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
    else bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static int persist_bitmap_sector(uint32_t sector_index) {
    if (!g_disk_backed) return 1;
    return ata_write_sector(FS_BITMAP_START_LBA + sector_index, g_bitmap + (uint32_t)sector_index * ATA_SECTOR_SIZE);
}
static void persist_bitmap_bit(uint32_t block) {
    persist_bitmap_sector((block / 8) / ATA_SECTOR_SIZE);
}

// Allocates one free block, disk-backed or RAM-only depending on
// g_disk_backed -- returns 0 (the reserved null value) if out of
// space. Scans forward from a hint, wrapping once, same simple
// first-fit approach the kernel heap (heap.c) uses for its own
// allocation -- fine at this scale (a handful of files, a
// gigabyte-class disk), not trying to be a sophisticated allocator.
static uint32_t alloc_block(void) {
    if (g_disk_backed) {
        for (uint32_t pass = 0; pass < 2; pass++) {
            uint32_t start = pass == 0 ? g_bitmap_scan_hint : FS_DATA_START_BLOCK;
            uint32_t end = pass == 0 ? FS_DISK_TOTAL_BLOCKS : g_bitmap_scan_hint;
            for (uint32_t b = start; b < end; b++) {
                if (!bit_test(g_bitmap, b)) {
                    bit_set(g_bitmap, b, 1);
                    persist_bitmap_bit(b);
                    g_bitmap_scan_hint = b + 1;
                    return b;
                }
            }
        }
        return 0;
    } else {
        for (uint32_t pass = 0; pass < 2; pass++) {
            uint32_t start = pass == 0 ? g_ram_scan_hint : 1;
            uint32_t end = pass == 0 ? RAM_ONLY_MAX_BLOCKS : g_ram_scan_hint;
            for (uint32_t b = start; b < end; b++) {
                if (!bit_test(g_ram_bitmap, b)) {
                    bit_set(g_ram_bitmap, b, 1);
                    g_ram_scan_hint = b + 1;
                    return b;
                }
            }
        }
        return 0;
    }
}

static void free_block(uint32_t b) {
    if (b == 0) return; // null sentinel, never a real allocation
    if (g_disk_backed) {
        if (b < FS_DATA_START_BLOCK) return; // never free reserved metadata blocks
        bit_set(g_bitmap, b, 0);
        persist_bitmap_bit(b);
        if (b < g_bitmap_scan_hint) g_bitmap_scan_hint = b;
    } else {
        if (b >= RAM_ONLY_MAX_BLOCKS) return;
        bit_set(g_ram_bitmap, b, 0);
        if (g_ram_blocks[b]) { kfree(g_ram_blocks[b]); g_ram_blocks[b] = 0; }
        if (b < g_ram_scan_hint) g_ram_scan_hint = b;
    }
}

// Reads/writes exactly one FS_BLOCK_SIZE-byte block. See this file's
// top comment for the RAM-only fallback's own limitations.
static int read_block(uint32_t block, void *buf) {
    if (block == 0) { k_memset(buf, 0, FS_BLOCK_SIZE); return 1; }
    if (g_disk_backed) {
        return ata_read_sectors(block * FS_BLOCK_SECTORS, FS_BLOCK_SECTORS, buf);
    }
    if (block >= RAM_ONLY_MAX_BLOCKS || !g_ram_blocks[block]) { k_memset(buf, 0, FS_BLOCK_SIZE); return 1; }
    k_memcpy(buf, g_ram_blocks[block], FS_BLOCK_SIZE);
    return 1;
}

static int write_block(uint32_t block, const void *buf) {
    if (block == 0) return 0;
    if (g_disk_backed) {
        return ata_write_sectors(block * FS_BLOCK_SECTORS, FS_BLOCK_SECTORS, buf);
    }
    if (block >= RAM_ONLY_MAX_BLOCKS) return 0;
    if (!g_ram_blocks[block]) {
        g_ram_blocks[block] = kmalloc(FS_BLOCK_SIZE);
        if (!g_ram_blocks[block]) return 0;
    }
    k_memcpy(g_ram_blocks[block], buf, FS_BLOCK_SIZE);
    return 1;
}

static void zero_block(uint32_t block) {
    static uint8_t zero[FS_BLOCK_SIZE]; // .bss, already zero -- never written to, just a source buffer
    write_block(block, zero);
}

// ---- indirect-pointer walk ----
//
// Finds (and, if `allocate`, creates) the block for the `index`-th
// FS_BLOCK_SIZE-byte block of a file (0-based). Direct blocks are
// handled inline in block_for_index(); anything past FS_N_DIRECT
// blocks descends through walk_indirect()'s single/double/triple
// indirect chain. Freshly allocated blocks -- both index blocks and
// leaf data blocks -- are always zero-filled before their pointer is
// returned/stored, which is what makes a partial-block
// read-modify-write in write_range_impl() always safe (no stale disk
// content from a previously deleted file can leak into a new one) and
// gives "write past the old end of the file" free zero-fill of the gap.
static uint32_t g_walk_scratch[FS_PTRS_PER_BLOCK]; // one block's worth of pointers -- 4KB, static (not stack) since this isn't reentrant, see heap.c's own precedent

static uint32_t walk_indirect(uint32_t *top_slot, int depth, uint32_t index, int allocate) {
    if (*top_slot == 0) {
        if (!allocate) return 0;
        uint32_t nb = alloc_block();
        if (!nb) return 0;
        zero_block(nb);
        *top_slot = nb;
    }
    uint32_t cur_block = *top_slot;
    uint32_t remaining = index;

    for (int level = depth; level >= 1; level--) {
        uint32_t child_capacity = 1;
        for (int i = 0; i < level - 1; i++) child_capacity *= FS_PTRS_PER_BLOCK;
        uint32_t slot_i = remaining / child_capacity;
        remaining = remaining % child_capacity;

        if (!read_block(cur_block, g_walk_scratch)) return 0;
        uint32_t child = g_walk_scratch[slot_i];

        if (level == 1) {
            if (child == 0) {
                if (!allocate) return 0;
                child = alloc_block();
                if (!child) return 0;
                zero_block(child);
                g_walk_scratch[slot_i] = child;
                if (!write_block(cur_block, g_walk_scratch)) return 0;
            }
            return child;
        }

        if (child == 0) {
            if (!allocate) return 0;
            child = alloc_block();
            if (!child) return 0;
            zero_block(child);
            g_walk_scratch[slot_i] = child;
            if (!write_block(cur_block, g_walk_scratch)) return 0;
        }
        cur_block = child;
    }
    return 0; // unreachable (depth >= 1 always returns from inside the loop)
}

static uint32_t block_for_index(struct file *f, uint32_t index, int allocate) {
    if (index < FS_N_DIRECT) {
        uint32_t *slot = &f->direct[index];
        if (*slot == 0) {
            if (!allocate) return 0;
            uint32_t nb = alloc_block();
            if (!nb) return 0;
            zero_block(nb);
            *slot = nb;
        }
        return *slot;
    }
    index -= FS_N_DIRECT;

    uint32_t single_cap = FS_PTRS_PER_BLOCK;
    uint32_t double_cap = FS_PTRS_PER_BLOCK * FS_PTRS_PER_BLOCK;

    if (index < single_cap) return walk_indirect(&f->single_indirect, 1, index, allocate);
    index -= single_cap;
    if (index < double_cap) return walk_indirect(&f->double_indirect, 2, index, allocate);
    index -= double_cap;
    return walk_indirect(&f->triple_indirect, 3, index, allocate); // capacity ~1G blocks (~4TB) -- far past this disk's real size
}

// Frees `block` and (if depth > 0) every block it indirectly points
// to first -- depth follows the same convention as walk_indirect()'s
// (1 = this block's entries are direct data pointers, 2/3 = one/two
// more levels of indirection below it). Recursive, but each frame's
// only large local is g_free_scratch[depth] -- a STATIC array indexed
// by the frame's own depth, not a stack allocation, so recursion depth
// (max 3) costs no meaningful stack space. Safe because each active
// frame in one call chain has a distinct depth value (3, then 2, then
// 1), so frames never alias the slice they're using even though the
// backing array is shared -- and this whole file is already
// documented as non-reentrant/single-threaded (see heap.c's identical
// reasoning), so there's no concurrent caller to worry about either.
static uint32_t g_free_scratch[4][FS_PTRS_PER_BLOCK];

static void free_tree(uint32_t block, int depth) {
    if (block == 0) return;
    if (depth > 0) {
        if (read_block(block, g_free_scratch[depth])) {
            for (int i = 0; i < FS_PTRS_PER_BLOCK; i++) {
                if (g_free_scratch[depth][i]) free_tree(g_free_scratch[depth][i], depth - 1);
            }
        }
    }
    free_block(block);
}

static void free_all_blocks(struct file *f) {
    for (int i = 0; i < FS_N_DIRECT; i++) {
        if (f->direct[i]) free_block(f->direct[i]);
        f->direct[i] = 0;
    }
    free_tree(f->single_indirect, 1); f->single_indirect = 0;
    free_tree(f->double_indirect, 2); f->double_indirect = 0;
    free_tree(f->triple_indirect, 3); f->triple_indirect = 0;
}

// ---- range read/write (fs_read_range/fs_write_range's backend, and
// the shared core tfs_read()/tfs_write() build on too) ----

static uint32_t g_io_scratch[FS_BLOCK_SIZE / 4]; // one block, reused for every partial-block copy below

static uint32_t read_range_impl(struct file *f, uint64_t offset, void *buf, uint32_t len) {
    if (offset >= f->size) return 0;
    uint64_t avail = f->size - offset;
    if ((uint64_t)len > avail) len = (uint32_t)avail;

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t block_index = (uint32_t)(file_off / FS_BLOCK_SIZE);
        uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
        uint32_t chunk = FS_BLOCK_SIZE - within;
        if (chunk > len - total) chunk = len - total;

        uint32_t blk = block_for_index(f, block_index, 0);
        if (!read_block(blk, g_io_scratch)) break;
        k_memcpy(dst + total, (uint8_t *)g_io_scratch + within, chunk);
        total += chunk;
    }
    return total;
}

static int write_range_impl(struct file *f, uint64_t offset, const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t total = 0;
    while (total < len) {
        uint64_t file_off = offset + total;
        uint32_t block_index = (uint32_t)(file_off / FS_BLOCK_SIZE);
        uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
        uint32_t chunk = FS_BLOCK_SIZE - within;
        if (chunk > len - total) chunk = len - total;

        uint32_t blk = block_for_index(f, block_index, 1);
        if (!blk) return 0; // out of space -- whatever was written before this point stays, see fs.h

        if (within == 0 && chunk == FS_BLOCK_SIZE) {
            k_memcpy(g_io_scratch, src + total, FS_BLOCK_SIZE);
        } else {
            if (!read_block(blk, g_io_scratch)) return 0;
            k_memcpy((uint8_t *)g_io_scratch + within, src + total, chunk);
        }
        if (!write_block(blk, g_io_scratch)) return 0;
        total += chunk;
    }
    if (offset + total > f->size) f->size = offset + total;
    return 1;
}

static void serialize_rtc(const struct rtc_time *t, uint8_t *buf) {
    buf[0] = t->hour; buf[1] = t->minute; buf[2] = t->second;
    buf[3] = t->day; buf[4] = t->month;
    buf[5] = (uint8_t)(t->year & 0xFF); buf[6] = (uint8_t)((t->year >> 8) & 0xFF);
}
static void deserialize_rtc(struct rtc_time *t, const uint8_t *buf) {
    t->hour = buf[0]; t->minute = buf[1]; t->second = buf[2];
    t->day = buf[3]; t->month = buf[4];
    t->year = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
}

static void put_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFF); buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF); buf[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint32_t get_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}
static void put_u64(uint8_t *buf, uint64_t v) {
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}
static uint64_t get_u64(const uint8_t *buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[i] << (8 * i);
    return v;
}

static void serialize_record(const struct file *f, uint8_t *buf) {
    k_memset(buf, 0, FS_RECORD_BYTES);
    k_memcpy(buf, f->path, FS_PATH_MAX);
    buf[REC_OFF_TYPE] = f->type;
    buf[REC_OFF_USED] = (uint8_t)f->used;
    put_u64(buf + REC_OFF_SIZE, f->size);
    serialize_rtc(&f->created, buf + REC_OFF_CREATED);
    serialize_rtc(&f->modified, buf + REC_OFF_MODIFIED);
    for (int i = 0; i < FS_N_DIRECT; i++) put_u32(buf + REC_OFF_DIRECT + i * 4, f->direct[i]);
    put_u32(buf + REC_OFF_SINGLE, f->single_indirect);
    put_u32(buf + REC_OFF_DOUBLE, f->double_indirect);
    put_u32(buf + REC_OFF_TRIPLE, f->triple_indirect);
}

static void deserialize_record(struct file *f, const uint8_t *buf) {
    k_memcpy(f->path, buf, FS_PATH_MAX);
    f->type = buf[REC_OFF_TYPE];
    f->used = buf[REC_OFF_USED];
    f->size = get_u64(buf + REC_OFF_SIZE);
    deserialize_rtc(&f->created, buf + REC_OFF_CREATED);
    deserialize_rtc(&f->modified, buf + REC_OFF_MODIFIED);
    for (int i = 0; i < FS_N_DIRECT; i++) f->direct[i] = get_u32(buf + REC_OFF_DIRECT + i * 4);
    f->single_indirect = get_u32(buf + REC_OFF_SINGLE);
    f->double_indirect = get_u32(buf + REC_OFF_DOUBLE);
    f->triple_indirect = get_u32(buf + REC_OFF_TRIPLE);
}

static uint32_t fnv1a(const uint8_t *buf, int len) {
    uint32_t hash = 0x811C9DC5u;
    for (int i = 0; i < len; i++) { hash ^= buf[i]; hash *= 0x01000193u; }
    return hash;
}

static int write_table_slot(int index, const uint8_t *buf) {
    uint32_t lba = record_lba(index);
    for (int s = 0; s < FS_RECORD_SECTORS; s++) {
        if (!ata_write_sector(lba + s, buf + (uint32_t)s * ATA_SECTOR_SIZE)) return 0;
    }
    return 1;
}

static int write_journal_header(int commit, uint32_t slot, uint32_t checksum) {
    uint8_t buf[ATA_SECTOR_SIZE];
    k_memset(buf, 0, sizeof(buf));
    buf[0] = 'J'; buf[1] = 'R'; buf[2] = 'N'; buf[3] = '1';
    buf[4] = (uint8_t)commit;
    put_u32(buf + 5, slot);
    put_u32(buf + 9, checksum);
    return ata_write_sector(FS_JOURNAL_HEADER_LBA, buf);
}

static int read_journal_header(int *out_commit, uint32_t *out_slot, uint32_t *out_checksum) {
    uint8_t buf[ATA_SECTOR_SIZE];
    if (!ata_read_sector(FS_JOURNAL_HEADER_LBA, buf)) return 0;
    if (buf[0] != 'J' || buf[1] != 'R' || buf[2] != 'N' || buf[3] != '1') return 0;
    *out_commit = buf[4];
    *out_slot = get_u32(buf + 5);
    *out_checksum = get_u32(buf + 9);
    return 1;
}

static int persist_record(int index) {
    if (!g_disk_backed) return 1;
    uint8_t buf[FS_RECORD_BYTES];
    serialize_record(&files[index], buf);
    uint32_t checksum = fnv1a(buf, FS_RECORD_BYTES);

    for (int s = 0; s < FS_RECORD_SECTORS; s++) {
        if (!ata_write_sector(FS_JOURNAL_DATA_LBA + s, buf + (uint32_t)s * ATA_SECTOR_SIZE)) return 0;
    }
    if (!write_journal_header(1, (uint32_t)index, checksum)) return 0;
    if (!write_table_slot(index, buf)) return 0;
    write_journal_header(0, 0, 0);
    return 1;
}

static void replay_journal(void) {
    int commit;
    uint32_t slot, checksum;
    if (!read_journal_header(&commit, &slot, &checksum)) return;
    if (!commit) return;
    if (slot >= FS_MAX_FILES) { write_journal_header(0, 0, 0); return; }

    uint8_t buf[FS_RECORD_BYTES];
    int ok = 1;
    for (int s = 0; s < FS_RECORD_SECTORS && ok; s++) {
        ok = ata_read_sector(FS_JOURNAL_DATA_LBA + s, buf + (uint32_t)s * ATA_SECTOR_SIZE);
    }
    if (ok && fnv1a(buf, FS_RECORD_BYTES) == checksum) {
        write_table_slot((int)slot, buf);
        klog_write("fs: replayed a pending journal entry from an unclean shutdown\n");
    } else {
        klog_write("fs: discarded a torn journal entry from an unclean shutdown\n");
    }
    write_journal_header(0, 0, 0);
}

static int write_superblock(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    k_memset(buf, 0, sizeof(buf));
    buf[0] = FS_DISK_MAGIC0; buf[1] = FS_DISK_MAGIC1;
    buf[2] = FS_DISK_MAGIC2; buf[3] = FS_DISK_MAGIC3;
    buf[4] = FS_DISK_VERSION;
    return ata_write_sector(FS_SUPERBLOCK_LBA, buf);
}

// Writes the whole in-memory bitmap out to disk in FS_BLOCK_SECTORS-
// sized multi-sector chunks (the new ata_read_sectors/write_sectors --
// see ata.h) instead of one ATA command per 512-byte sector, same
// motivation as every other block I/O in this file.
static void write_full_bitmap(void) {
    uint32_t sectors_left = FS_BITMAP_SECTORS;
    uint32_t sector = 0;
    while (sectors_left > 0) {
        int chunk = sectors_left > ATA_MAX_SECTORS_PER_XFER ? ATA_MAX_SECTORS_PER_XFER : (int)sectors_left;
        ata_write_sectors(FS_BITMAP_START_LBA + sector, chunk, g_bitmap + (uint32_t)sector * ATA_SECTOR_SIZE);
        sector += (uint32_t)chunk;
        sectors_left -= (uint32_t)chunk;
    }
}
static void read_full_bitmap(void) {
    uint32_t sectors_left = FS_BITMAP_SECTORS;
    uint32_t sector = 0;
    while (sectors_left > 0) {
        int chunk = sectors_left > ATA_MAX_SECTORS_PER_XFER ? ATA_MAX_SECTORS_PER_XFER : (int)sectors_left;
        ata_read_sectors(FS_BITMAP_START_LBA + sector, chunk, g_bitmap + (uint32_t)sector * ATA_SECTOR_SIZE);
        sector += (uint32_t)chunk;
        sectors_left -= (uint32_t)chunk;
    }
}

// Defined near the bottom of this file (after the touch/write_range/
// read_range/delete helpers it uses) -- forward-declared here so
// tfs_init() can call it once, right after a disk is confirmed usable.
static void tfs_selftest(void);

static int tfs_init(void) {
    k_memset(files, 0, sizeof(files));
    k_memset(g_ram_blocks, 0, sizeof(g_ram_blocks));
    k_memset(g_ram_bitmap, 0, sizeof(g_ram_bitmap));
    g_ram_scan_hint = 1;
    g_disk_backed = 0;

    ata_init();
    if (!ata_present()) {
        klog_write("fs: no disk found -- files are RAM-only, won't survive reboot\n");
        return 0;
    }

    uint8_t sb[ATA_SECTOR_SIZE];
    if (ata_read_sector(FS_SUPERBLOCK_LBA, sb) &&
        sb[0] == FS_DISK_MAGIC0 && sb[1] == FS_DISK_MAGIC1 &&
        sb[2] == FS_DISK_MAGIC2 && sb[3] == FS_DISK_MAGIC3 &&
        sb[4] == FS_DISK_VERSION) {
        g_disk_backed = 1;
        replay_journal();
        read_full_bitmap();
        g_bitmap_scan_hint = FS_DATA_START_BLOCK;
        uint8_t rec[FS_RECORD_BYTES];
        for (int i = 0; i < FS_MAX_FILES; i++) {
            uint32_t lba = record_lba(i);
            int ok = 1;
            for (int s = 0; s < FS_RECORD_SECTORS && ok; s++) {
                ok = ata_read_sector(lba + s, rec + (uint32_t)s * ATA_SECTOR_SIZE);
            }
            if (ok) deserialize_record(&files[i], rec);
        }
        klog_write("fs: loaded persistent filesystem from disk\n");
    } else {
        // Blank, foreign, or old-version disk (including a pre-rework
        // TFS2 image, see this file's top-of-file warning) -- format
        // fresh: superblock, empty journal, a bitmap with every
        // reserved block pre-marked allocated, and an empty table.
        g_disk_backed = 1;
        write_superblock();
        write_journal_header(0, 0, 0);
        k_memset(g_bitmap, 0, sizeof(g_bitmap));
        for (uint32_t b = 0; b < FS_DATA_START_BLOCK; b++) bit_set(g_bitmap, b, 1);
        write_full_bitmap();
        g_bitmap_scan_hint = FS_DATA_START_BLOCK;
        for (int i = 0; i < FS_MAX_FILES; i++) persist_record(i);
        klog_write("fs: formatted a fresh persistent filesystem on disk\n");
    }
    if (g_disk_backed) tfs_selftest();
    return g_disk_backed;
}

// ---- path helpers (unchanged from before this rework) ----

static int is_valid_component(const char *s, int len) {
    if (len == 0) return 0;
    if (len == 1 && s[0] == '.') return 0;
    if (len == 2 && s[0] == '.' && s[1] == '.') return 0;
    return 1;
}

static int path_is_normalized(const char *p) {
    size_t len = k_strlen(p);
    if (len == 0 || len >= FS_PATH_MAX) return 0;
    if (p[0] != '/') return 0;
    if (len == 1) return 1;
    if (p[len - 1] == '/') return 0;

    size_t i = 1;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != '/') i++;
        if (!is_valid_component(p + start, (int)(i - start))) return 0;
        if (i < len) i++;
    }
    return 1;
}

static int normalize(const char *in, char out[FS_PATH_MAX]) {
    if (!in) return 0;
    if (in[0] == '/') {
        if (k_strlen(in) >= FS_PATH_MAX) return 0;
        k_strcpy(out, in);
    } else {
        if (k_strlen(in) + 1 >= FS_PATH_MAX) return 0;
        out[0] = '/';
        k_strcpy(out + 1, in);
    }
    return path_is_normalized(out);
}

static void path_parent(const char *norm_path, char out[FS_PATH_MAX]) {
    size_t len = k_strlen(norm_path);
    size_t last_slash = 0;
    for (size_t i = 0; i < len; i++) if (norm_path[i] == '/') last_slash = i;
    if (last_slash == 0) { out[0] = '/'; out[1] = '\0'; }
    else { k_memcpy(out, norm_path, last_slash); out[last_slash] = '\0'; }
}

static struct file *find(const char *norm_path) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && k_strcmp(files[i].path, norm_path) == 0) return &files[i];
    }
    return 0;
}

static int find_free_index(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) if (!files[i].used) return i;
    return -1;
}

static int parent_is_dir(const char *norm_path) {
    char parent[FS_PATH_MAX];
    path_parent(norm_path, parent);
    if (k_strcmp(parent, "/") == 0) return 1;
    struct file *f = find(parent);
    return f && f->type == FS_TYPE_DIR;
}

// ---- backend implementation ----

static int tfs_is_dir(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    struct file *f = find(norm);
    return f != 0 && f->type == FS_TYPE_DIR;
}

static int tfs_exists(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    return find(norm) != 0;
}

static int tfs_touch(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;

    struct file *existing = find(norm);
    if (existing) return existing->type == FS_TYPE_FILE;

    if (!parent_is_dir(norm)) return 0;

    int idx = find_free_index();
    if (idx < 0) return 0;

    struct file *f = &files[idx];
    k_memset(f, 0, sizeof(*f));
    k_strcpy(f->path, norm);
    f->type = FS_TYPE_FILE;
    f->size = 0;
    f->used = 1;
    rtc_read_local(&f->created);
    f->modified = f->created;
    persist_record(idx);
    return 1;
}

static int tfs_mkdir(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;
    if (find(norm)) return 0;

    if (!parent_is_dir(norm)) return 0;

    int idx = find_free_index();
    if (idx < 0) return 0;

    struct file *f = &files[idx];
    k_memset(f, 0, sizeof(*f));
    k_strcpy(f->path, norm);
    f->type = FS_TYPE_DIR;
    f->size = 0;
    f->used = 1;
    rtc_read_local(&f->created);
    f->modified = f->created;
    persist_record(idx);
    return 1;
}

static int tfs_write(const char *path, const char *data, int append) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;

    struct file *f = find(norm);
    if (f && f->type == FS_TYPE_DIR) return 0;
    if (!f) {
        if (!tfs_touch(norm)) return 0;
        f = find(norm);
    }

    uint32_t data_len = (uint32_t)k_strlen(data);
    uint64_t start;
    if (append) {
        start = f->size;
    } else {
        free_all_blocks(f); // reclaim whatever the old content used before writing fresh
        f->size = 0;
        start = 0;
    }

    if (data_len > 0 && !write_range_impl(f, start, data, data_len)) return 0;

    rtc_read_local(&f->modified);
    persist_record((int)(f - files));
    return 1;
}

static int tfs_delete(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;

    struct file *f = find(norm);
    if (!f) return 0;

    if (f->type == FS_TYPE_DIR) {
        size_t plen = k_strlen(norm);
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!files[i].used) continue;
            if (k_strncmp(files[i].path, norm, plen) == 0 && files[i].path[plen] == '/') return 0;
        }
    } else {
        free_all_blocks(f); // reclaim the file's data blocks -- previously a no-op since data was inline
    }

    f->used = 0;
    f->size = 0;
    persist_record((int)(f - files));
    return 1;
}

// Reused across tfs_read() calls -- see fs.h's fs_read() doc comment
// for why (gathering block-scattered data into one contiguous buffer
// needs somewhere to put it, and a fresh kmalloc() every call with no
// caller-visible free() would just leak).
static void *g_read_buf = 0;

static const char *tfs_read(const char *path, uint32_t *out_size) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return 0;

    if (f->size > 0xFFFFFFFFu - 1) return 0; // too big for this whole-buffer call -- use fs_read_range()
    uint32_t size = (uint32_t)f->size;

    if (g_read_buf) { kfree(g_read_buf); g_read_buf = 0; }
    g_read_buf = kmalloc((size_t)size + 1);
    if (!g_read_buf) return 0; // out of memory -- e.g. file too big for available RAM, see fs.h

    if (size > 0) {
        uint32_t got = read_range_impl(f, 0, g_read_buf, size);
        if (got != size) { kfree(g_read_buf); g_read_buf = 0; return 0; }
    }
    ((uint8_t *)g_read_buf)[size] = 0;

    if (out_size) *out_size = size;
    return g_read_buf;
}

static uint64_t tfs_size(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return 0;
    return f->size;
}

static uint32_t tfs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return 0;
    return read_range_impl(f, offset, buf, len);
}

static int tfs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (f && f->type == FS_TYPE_DIR) return 0;
    if (!f) {
        if (!tfs_touch(norm)) return 0;
        f = find(norm);
    }
    if (!write_range_impl(f, offset, buf, len)) return 0;
    rtc_read_local(&f->modified);
    persist_record((int)(f - files));
    return 1;
}

static int tfs_stat(const char *path, struct fs_timestamps *out) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0;
    struct file *f = find(norm);
    if (!f) return 0;
    if (out) { out->created = f->created; out->modified = f->modified; }
    return 1;
}

static void tfs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    char norm[FS_PATH_MAX];
    if (!normalize(dir_path, norm)) return;
    if (!tfs_is_dir(dir_path)) return;

    int root = (norm[0] == '/' && norm[1] == '\0');
    size_t plen = k_strlen(norm);

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        const char *p = files[i].path;
        const char *rest;
        if (root) {
            rest = p + 1;
        } else {
            if (k_strncmp(p, norm, plen) != 0 || p[plen] != '/') continue;
            rest = p + plen + 1;
        }
        if (*rest == '\0') continue;

        int is_direct_child = 1;
        for (const char *q = rest; *q; q++) if (*q == '/') { is_direct_child = 0; break; }
        if (!is_direct_child) continue;

        // fs_list()'s callback still takes a uint32_t size -- fine for
        // every real caller (ls-style listings truncate/display a
        // human-readable size anyway); a directory listing showing the
        // low 32 bits of a multi-GB file's size is a cosmetic-only
        // limitation, not a correctness one (fs_size() returns the
        // real uint64_t for anything that actually needs it).
        cb(rest, (uint32_t)files[i].size, files[i].type == FS_TYPE_DIR);
    }
}

// Proves the indirect-pointer addressing actually works, specifically
// the TRIPLE indirect path -- the part of this rework that only
// exists to reach an 8GB file at all (direct+single+double alone tops
// out around 4GB, see this file's top comment). Deliberately does NOT
// write gigabytes of real data to prove this (that would slow down
// every single boot for no extra confidence -- see below for why it
// doesn't need to): block_for_index()/walk_indirect() only ever
// allocate the blocks actually asked for, so writing a small chunk at
// a LARGE offset (chosen to be past double indirect's ~4GB capacity,
// forcing the triple-indirect chain to be built) exercises exactly the
// same pointer-walking code a real multi-gigabyte file would use, at
// the cost of one small write instead of gigabytes of them. This is
// the same "prove the mechanism, not the scale" reasoning heap.c's
// heap_selftest() already uses for its own coalescing checks. See
// docs/decisions.md for the full writeup, including why this can't
// also prove real multi-GB write *throughput* (that needs an actual
// timed multi-gigabyte test, deliberately left as a manual/CI-adjacent
// check rather than a boot-time one).
#define TFS_SELFTEST_PATH "/.tfs_selftest_tmp"
// ~4.6GB: past direct+single+double indirect's combined ~4.004GB
// capacity (FS_N_DIRECT*4KB + 1024*4KB + 1024*1024*4KB), so reaching
// it can only succeed if the triple-indirect chain was built and
// walked correctly.
#define TFS_SELFTEST_OFFSET (4600ULL * 1024 * 1024)

static void tfs_selftest(void) {
    uint8_t pattern[64];
    for (int i = 0; i < 64; i++) pattern[i] = (uint8_t)(i * 7 + 3);

    if (!tfs_touch(TFS_SELFTEST_PATH)) {
        klog_write("fs: selftest FAILED (couldn't create test file)\n");
        return;
    }
    if (!tfs_write_range(TFS_SELFTEST_PATH, TFS_SELFTEST_OFFSET, pattern, sizeof(pattern))) {
        klog_write("fs: selftest FAILED (triple-indirect write failed)\n");
        tfs_delete(TFS_SELFTEST_PATH);
        return;
    }
    if (tfs_size(TFS_SELFTEST_PATH) != TFS_SELFTEST_OFFSET + sizeof(pattern)) {
        klog_write("fs: selftest FAILED (size wrong after triple-indirect write)\n");
        tfs_delete(TFS_SELFTEST_PATH);
        return;
    }

    uint8_t readback[64];
    k_memset(readback, 0, sizeof(readback));
    uint32_t got = tfs_read_range(TFS_SELFTEST_PATH, TFS_SELFTEST_OFFSET, readback, sizeof(readback));
    if (got != sizeof(readback)) {
        klog_write("fs: selftest FAILED (short read back from triple-indirect region)\n");
        tfs_delete(TFS_SELFTEST_PATH);
        return;
    }
    for (int i = 0; i < 64; i++) {
        if (readback[i] != pattern[i]) {
            klog_write("fs: selftest FAILED (data mismatch reading back triple-indirect region)\n");
            tfs_delete(TFS_SELFTEST_PATH);
            return;
        }
    }

    // Also prove a lower, ordinary offset still reads back as zero
    // (the gap between byte 0 and TFS_SELFTEST_OFFSET was never
    // written -- should read as zero-fill, not garbage or an error).
    uint8_t gap[16];
    got = tfs_read_range(TFS_SELFTEST_PATH, 4096, gap, sizeof(gap));
    if (got != sizeof(gap)) {
        klog_write("fs: selftest FAILED (couldn't read the zero-filled gap)\n");
        tfs_delete(TFS_SELFTEST_PATH);
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (gap[i] != 0) {
            klog_write("fs: selftest FAILED (gap wasn't zero-filled)\n");
            tfs_delete(TFS_SELFTEST_PATH);
            return;
        }
    }

    if (!tfs_delete(TFS_SELFTEST_PATH)) {
        klog_write("fs: selftest FAILED (couldn't delete test file / free its blocks)\n");
        return;
    }
    if (tfs_exists(TFS_SELFTEST_PATH)) {
        klog_write("fs: selftest FAILED (test file still exists after delete)\n");
        return;
    }

    klog_write("fs: selftest passed (triple-indirect addressing verified)\n");
}

// Backs fs_disk_usage() -- counts set bits in whichever bitmap is
// active (disk-backed vs. RAM-only, same split alloc_block()/
// free_block() already make) and scales to bytes. `total` deliberately
// excludes FS_DATA_START_BLOCK's reserved metadata blocks (superblock/
// journal/record-table/bitmap) in disk-backed mode -- those blocks are
// pre-marked allocated in g_bitmap (see tfs_init()) and would otherwise
// count as "used" data, which isn't what a `df`-style command means by
// used/total. No such reservation exists in RAM-only mode (block 0 is
// just the null sentinel, not reserved metadata), so that branch scans
// the bitmap's full range starting at block 1.
static int tfs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    uint64_t used = 0, total;
    if (g_disk_backed) {
        total = FS_DISK_TOTAL_BLOCKS - FS_DATA_START_BLOCK;
        for (uint32_t b = FS_DATA_START_BLOCK; b < FS_DISK_TOTAL_BLOCKS; b++) {
            if (bit_test(g_bitmap, b)) used++;
        }
    } else {
        total = RAM_ONLY_MAX_BLOCKS - 1; // block 0 is the null sentinel, not usable
        for (uint32_t b = 1; b < RAM_ONLY_MAX_BLOCKS; b++) {
            if (bit_test(g_ram_bitmap, b)) used++;
        }
    }
    if (out_used_bytes) *out_used_bytes = used * FS_BLOCK_SIZE;
    if (out_total_bytes) *out_total_bytes = total * FS_BLOCK_SIZE;
    return 1;
}

const struct fs_ops tfs_ops = {
    .name = "tfs2",
    .init = tfs_init,
    .touch = tfs_touch,
    .write = tfs_write,
    .mkdir = tfs_mkdir,
    .del = tfs_delete,
    .read = tfs_read,
    .size = tfs_size,
    .read_range = tfs_read_range,
    .write_range = tfs_write_range,
    .is_dir = tfs_is_dir,
    .exists = tfs_exists,
    .list = tfs_list,
    .stat = tfs_stat,
    .disk_usage = tfs_disk_usage,
};
