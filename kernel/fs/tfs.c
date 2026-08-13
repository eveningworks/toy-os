// TFS2 -- this OS's persistent filesystem, journaled and timestamped
// as of build 480, and (as of the large-file rework -- see
// CHANGELOG.md) block-addressed with indirect pointers instead of one
// fixed-size inline data blob per file. This used to be fs.c itself,
// back when it was the only filesystem toy-os could have; it's now
// just one backend behind the VFS dispatch layer (vfs.c), reachable
// only through the `tfs_ops` vtable at the bottom of this file (see
// fs_ops.h for what that interface is and why it exists, and
// kernel/include/kernel/tfs.h for this file's own public surface). Nothing
// outside vfs.c should #include tfs.h or call anything in this file
// directly -- go through fs.h's fs_* API instead, same as before this
// split.
//
// *** IMPORTANT: each TFS2 version byte is incompatible with the ones
// *** before it, and an old disk is detected as foreign and reformatted
// *** from scratch -- the standing "no migration, just reformat" policy
// *** for format changes here (see the superblock comment below).
// *** Existing files WILL be lost the first time a newer kernel boots
// *** against an older disk.img.
// ***   v1 -> v2: inline 2048-byte-per-file data replaced by the block
// ***             allocator + indirect pointers below. Also needed a
// ***             much bigger disk.img (the old 1MB image can't hold
// ***             the bitmap region) -- see the Makefile.
// ***   v2 -> v3: FS_MAX_FILES 32 -> 256 (fs.h), which moves
// ***             FS_BITMAP_START_LBA and every LBA after it.
// *** tools/tfs2_writer.py mirrors this layout host-side and must be
// *** updated in lockstep with any of these constants.
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
#include "debugflags.h"

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

// FS_DISK_TOTAL_BLOCKS above is the compile-time MAXIMUM (it sizes
// g_bitmap, which has to be a fixed array). This is the number actually
// in play, clamped at mount to what the drive really reports via
// ata_sector_count() -- so a disk.img smaller than 9 GiB (someone
// running from a release download, a hand-made image, a real machine's
// small disk) is used correctly instead of the allocator cheerfully
// handing out blocks past the end of it. Only ever <= the compile-time
// value: a BIGGER disk still uses 9 GiB, because the bitmap can't
// address more without a rebuild.
static uint32_t g_total_blocks = FS_DISK_TOTAL_BLOCKS;

// Sets g_total_blocks from the drive's real capacity. Called once, from
// tfs_init(), after ata_init() has run. A drive that doesn't report a
// capacity (ata_sector_count() == 0) leaves the compile-time value
// alone -- same "no bound available" convention ata.c's own range check
// uses.
static void clamp_total_blocks_to_disk(void) {
    uint32_t sectors = ata_sector_count();
    if (sectors == 0) {
        // Ambiguous on purpose: a drive that genuinely doesn't report
        // an LBA28 capacity is indistinguishable here from one with no
        // capacity, so this keeps the compile-time maximum rather than
        // refusing to mount something that might be fine. The one case
        // that slips through is a 0-length disk image (QEMU answers
        // reads with zeros instead of erroring, so it looks like a
        // blank disk and gets formatted -- writes then go nowhere, and
        // the boot selftest is what catches it, loudly). Say why the
        // clamp didn't happen so `dmesg` explains the difference.
        klog_write("fs: drive reports no LBA28 capacity -- using the built-in maximum unclamped\n");
        return;
    }
    uint32_t blocks = sectors / FS_BLOCK_SECTORS;
    if (blocks < g_total_blocks) {
        klog_write("fs: disk is smaller than the built-in maximum -- using ");
        klog_write_dec(blocks); klog_write(" of "); klog_write_dec(FS_DISK_TOTAL_BLOCKS);
        klog_write(" blocks\n");
        g_total_blocks = blocks;
    }
}

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
// Version 3: FS_MAX_FILES went 32 -> 256 (see fs.h), which moves
// FS_BITMAP_START_LBA and every LBA after it. A version-2 disk is
// therefore detected as foreign and reformatted, same "no migration,
// just reformat" policy every previous format change used -- see this
// file's top-of-file warning.
#define FS_DISK_VERSION 3
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

// How many times tfs_init() re-attempts the superblock read before
// concluding the disk is genuinely unreadable -- on top of
// ata_read_sector()'s own ATA_DMA_MAX_RETRIES. See tfs_init() for why
// "couldn't read it" must never be treated as "it isn't ours".
#define FS_SUPERBLOCK_READ_MAX_RETRIES 3

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

// persist_bitmap_sector() already rides on ata_write_sector() ->
// dma_transfer_with_retry() (ata.c, ATA_DMA_MAX_RETRIES = 3), so this
// exists for the rare case that ALSO exhausts every one of those --
// found live: a `stress` run on real hardware hit exactly this on a
// bitmap sector (a transient DMA/IRQ miss, not a real drive fault --
// see ata.c's own retry-wrapper comment for why this class of failure
// happens at all). One more bounded attempt round costs nothing on
// the (already rare) failure path and catches a slightly-longer-than-
// usual miss without masking a genuinely dead drive, which still
// surfaces as a hard failure below. Always logged (independent of
// `debug fs`) -- a silently-stale on-disk bitmap sector risks a
// future double-allocation across the ~4096 blocks' worth of
// free-space bookkeeping one sector covers, which is a correctness
// bug, not noise.
#define FS_BITMAP_PERSIST_MAX_RETRIES 3

static int persist_bitmap_sector_with_retry(uint32_t sector_index) {
    for (int attempt = 1; attempt <= FS_BITMAP_PERSIST_MAX_RETRIES; attempt++) {
        if (persist_bitmap_sector(sector_index)) return 1;
    }
    klog_write("fs: WARNING -- bitmap sector "); klog_write_dec(sector_index);
    klog_write(" (lba "); klog_write_dec(FS_BITMAP_START_LBA + sector_index);
    klog_write(") failed to persist after "); klog_write_dec((uint32_t)FS_BITMAP_PERSIST_MAX_RETRIES);
    klog_write(" attempts -- will retry on next flush\n");
    return 0;
}

// Backs write_batch_begin()/write_batch_end() below -- one bit per
// bitmap SECTOR (not per data block, note: FS_BITMAP_SECTORS is tiny,
// a few hundred even on a multi-GB disk, since each sector's 512 bytes
// covers 4096 blocks' worth of bits). During a batch, persist_bitmap_
// bit() only marks the sector dirty here instead of writing it
// immediately -- a large sequential write allocates thousands of
// contiguous blocks that virtually always land in the same handful of
// bitmap sectors, so without this a bulk write would otherwise issue
// one redundant sector write per newly-allocated block even after the
// ata_flush_begin()/end() batching (ata.h) removed the FLUSH after
// each one. write_batch_end() persists each dirty sector exactly once
// (still inside the ata.h batch, so those writes are flush-deferred
// too) before closing the ata-level batch out with one real flush
// covering everything.
static uint8_t g_bitmap_dirty[(FS_BITMAP_SECTORS + 7) / 8];
static int g_write_batch_depth = 0; // nestable, mirrors ata_flush_begin/end's own depth counter

static void persist_bitmap_bit(uint32_t block) {
    uint32_t sector = (block / 8) / ATA_SECTOR_SIZE;
    if (g_write_batch_depth > 0) {
        g_bitmap_dirty[sector / 8] |= (uint8_t)(1u << (sector % 8));
        return;
    }
    // Outside a batch, still leave it marked dirty on failure (instead
    // of just discarding the error) -- persist_bitmap_sector_with_retry()
    // already logs the failure; marking dirty here means the very next
    // write_batch_begin()/end() (any subsequent write) gets one more
    // chance to persist it rather than losing it for good.
    if (!persist_bitmap_sector_with_retry(sector)) {
        g_bitmap_dirty[sector / 8] |= (uint8_t)(1u << (sector % 8));
    }
}

// Brackets a run of block_for_index()/write_block() calls that only
// need to be durable as a whole (e.g. one fs_write_range() call's
// worth of file data) -- see ata_flush_begin()/end()'s doc comment
// (ata.h) for the underlying mechanism this rides on, and
// persist_bitmap_bit() above for why bitmap sector writes get the
// same treatment. EVERY write_batch_begin() must be matched by
// write_batch_end() on every exit path, including error returns --
// see write_range_impl() for the pattern (a single cleanup point, not
// duplicated at each early return).
static void write_batch_begin(void) {
    if (!g_disk_backed) return; // RAM-only has no flush/bitmap-sector concept to defer
    g_write_batch_depth++;
    ata_flush_begin();
}

static void write_batch_end(void) {
    if (!g_disk_backed) return;
    if (g_write_batch_depth > 0) g_write_batch_depth--;
    if (g_write_batch_depth == 0) {
        int flushed_any = 0;
        for (uint32_t i = 0; i < FS_BITMAP_SECTORS; i++) {
            if (g_bitmap_dirty[i / 8] & (1u << (i % 8))) {
                // Only clear the dirty bit on success -- persist_bitmap_
                // sector_with_retry() already retried and logged, but
                // leaving the bit set on failure means the NEXT flush
                // (the very next disk-backed write) gets another chance
                // at this sector instead of the bitmap silently going
                // stale on disk for good.
                if (persist_bitmap_sector_with_retry(i)) {
                    g_bitmap_dirty[i / 8] &= (uint8_t)~(1u << (i % 8));
                }
                flushed_any++;
            }
        }
        if (flushed_any && dbgflag_enabled(DBGFLAG_FS)) {
            klog_write("fs: write_batch_end flushed "); klog_write_dec((uint32_t)flushed_any);
            klog_write(" dirty bitmap sector(s)\n");
        }
    }
    ata_flush_end(); // must run AFTER the dirty-sector writes above -- see ata.h: the real
                      // flush only fires once ITS OWN depth reaches 0, and those writes
                      // need to land while still inside the deferred window to be covered.
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
            uint32_t end = pass == 0 ? g_total_blocks : g_bitmap_scan_hint;
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

// Returns 1 if the block is genuinely zeroed on disk. Checking this
// matters more than it looks: a freshly allocated INDIRECT block that
// silently failed to zero keeps whatever a previously deleted file left
// there, and walk_indirect() would then read those stale bytes as real
// block pointers -- handing a file blocks that belong to something else.
// Every caller treats a failure here as "the allocation failed".
static int zero_block(uint32_t block) {
    static uint8_t zero[FS_BLOCK_SIZE]; // .bss, already zero -- never written to, just a source buffer
    return write_block(block, zero);
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
static uint32_t g_walk_scratch[FS_PTRS_PER_BLOCK];

// block_for_index()/walk_indirect()'s `allocate` argument. It used to
// be a plain 0/1 flag, with 1 always meaning "allocate AND zero-fill".
// Zero-filling a freshly allocated block is what makes a partial-block
// read-modify-write safe and gives sparse gaps free zero-fill -- but it
// costs a full block write, and for the common case (a sequential write
// that immediately overwrites the whole block) that write is pure
// waste: it doubled the commands issued per block of a large file, and
// defeated the multi-block coalescing below entirely, since the zeroing
// happened one block at a time between the coalesced runs.
//
// BLK_ALLOC_NOZERO says "the caller is about to overwrite this entire
// block right now, don't bother". Only ever valid for a LEAF data block
// being written in full -- indirect index blocks are always zeroed
// regardless of this flag (walk_indirect() ignores it for them), since
// their unwritten entries are read back as block pointers and must be
// the 0 sentinel, not whatever a deleted file left behind.
//
// If the caller's write then fails, the block keeps that stale content
// -- but it's past the file's size (which only advances for bytes
// actually written), so no read can reach it, and the next write to
// that offset overwrites it in full anyway.
#define BLK_NO_ALLOC     0
#define BLK_ALLOC        1
#define BLK_ALLOC_NOZERO 2 // one block's worth of pointers -- 4KB, static (not stack) since this isn't reentrant, see heap.c's own precedent

static uint32_t walk_indirect(uint32_t *top_slot, int depth, uint32_t index, int allocate) {
    if (*top_slot == 0) {
        if (!allocate) return 0;
        uint32_t nb = alloc_block();
        if (!nb) return 0;
        if (!zero_block(nb)) { free_block(nb); return 0; }
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
                // Leaf data block -- the only place BLK_ALLOC_NOZERO applies.
                if (allocate != BLK_ALLOC_NOZERO && !zero_block(child)) { free_block(child); return 0; }
                g_walk_scratch[slot_i] = child;
                if (!write_block(cur_block, g_walk_scratch)) { free_block(child); return 0; }
            }
            return child;
        }

        if (child == 0) {
            if (!allocate) return 0;
            child = alloc_block();
            if (!child) return 0;
            if (!zero_block(child)) { free_block(child); return 0; }
            g_walk_scratch[slot_i] = child;
            if (!write_block(cur_block, g_walk_scratch)) { free_block(child); return 0; }
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
            if (allocate != BLK_ALLOC_NOZERO && !zero_block(nb)) { free_block(nb); return 0; }
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

// Moves `f`'s block pointers into `snapshot` and clears them in `f`,
// without touching the bitmap at all. This is what lets the truncate
// (tfs_write with append=0) and delete paths persist a record that
// references NOTHING *before* the blocks are actually returned to the
// free bitmap.
//
// Order matters here and used to be backwards: free_all_blocks() ran
// first, so a crash (or a failed persist_record()) between the bitmap
// update and the record write left an on-disk record still pointing at
// blocks the bitmap had already marked free -- the next allocation
// hands one of them to a different file, and now two files share a
// block. Freeing last means the worst case is the opposite and
// harmless: blocks marked allocated that nothing references, which is
// a space leak recoverable by a future fsck-style pass, not corruption.
static void detach_blocks(struct file *f, struct file *snapshot) {
    for (int i = 0; i < FS_N_DIRECT; i++) {
        snapshot->direct[i] = f->direct[i];
        f->direct[i] = 0;
    }
    snapshot->single_indirect = f->single_indirect; f->single_indirect = 0;
    snapshot->double_indirect = f->double_indirect; f->double_indirect = 0;
    snapshot->triple_indirect = f->triple_indirect; f->triple_indirect = 0;
}

// Puts a detach_blocks() snapshot back, for the error path where the
// record couldn't be persisted and nothing was freed after all -- so
// the in-memory file still describes exactly what's on disk.
static void reattach_blocks(struct file *f, const struct file *snapshot) {
    for (int i = 0; i < FS_N_DIRECT; i++) f->direct[i] = snapshot->direct[i];
    f->single_indirect = snapshot->single_indirect;
    f->double_indirect = snapshot->double_indirect;
    f->triple_indirect = snapshot->triple_indirect;
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

// ---- multi-block coalescing (the fast path for a large sequential
// range) ----
//
// One ATA command per FS_BLOCK_SIZE block is correct but slow: it's one
// command dispatch and one completion IRQ per 4KB regardless of how
// contiguous the blocks are, which was this filesystem's real
// throughput ceiling (~18 MB/s write on the PIO/DMA ATA path). The
// allocator hands out blocks by scanning forward from a hint, so a
// sequential write's blocks are almost always physically contiguous --
// which means they can go out as ONE command covering up to
// ata_max_sectors_per_xfer() sectors (64KB, 16 blocks, when the big DMA
// bounce buffer came up -- see ata.h).
//
// The run detection below is deliberately conservative: it only merges
// blocks that are (a) already allocated or allocatable in order, (b)
// physically consecutive, and (c) fully covered by the caller's range
// (no partial first/last block -- those still go through the
// read-modify-write single-block path, which is where correctness is
// subtle). Anything that doesn't fit falls back to exactly the
// pre-existing per-block code, so the slow path is still the one that's
// been exercised by every `stress` run to date.
#define FS_MAX_BLOCKS_PER_RUN (ATA_MAX_SECTORS_PER_XFER / FS_BLOCK_SECTORS) // 16

// How many whole blocks starting at `first_index` are contiguous on
// disk AND wholly inside [*total, len)? Fills `out_first_block` with
// the run's starting block number. Returns 0 if the run isn't worth
// coalescing (fewer than 2 blocks), in which case the caller uses the
// single-block path unchanged. `allocate` is passed through to
// block_for_index(), so a write can build a fresh contiguous run and a
// read only ever reports blocks that already exist (a hole ends the
// run, since a hole has no block number to be contiguous with).
static uint32_t contiguous_run(struct file *f, uint32_t first_index, uint32_t max_blocks,
                                int allocate, uint32_t *out_first_block) {
    if (!g_disk_backed) return 0; // RAM-only blocks are separate kmalloc'd chunks, never contiguous
    if (max_blocks < 2) return 0;
    uint32_t limit = (uint32_t)ata_max_sectors_per_xfer() / FS_BLOCK_SECTORS;
    if (limit < 2) return 0; // no room to merge anything this boot
    if (max_blocks > limit) max_blocks = limit;
    if (max_blocks > FS_MAX_BLOCKS_PER_RUN) max_blocks = FS_MAX_BLOCKS_PER_RUN;

    uint32_t first = block_for_index(f, first_index, allocate);
    if (!first) return 0;
    *out_first_block = first;

    uint32_t n = 1;
    while (n < max_blocks) {
        uint32_t b = block_for_index(f, first_index + n, allocate);
        if (b != first + n) break; // a hole, an allocation failure, or a non-contiguous block
        n++;
    }
    return n >= 2 ? n : 0;
}

// One block's worth of scratch per block in a run -- 64KB of .bss, the
// same "static, not stack, this file is single-threaded" reasoning
// g_io_scratch/g_walk_scratch already use (and see heap.c's precedent).
static uint8_t g_run_scratch[FS_MAX_BLOCKS_PER_RUN * FS_BLOCK_SIZE];

static uint32_t g_io_scratch[FS_BLOCK_SIZE / 4]; // one block, reused for every partial-block copy below

// One block's worth of read_range_impl()'s (old) loop body -- pulled
// out the same way write_range_one_block() was above, so the exact
// same per-block logic backs both the blocking path (read_range_impl()
// below, which now just calls this in a tight loop same as before) and
// the steppable path (tfs_read_range_step() further down, which calls
// it once per external step() call). Neither caller's behavior changed
// by this split. Returns 1 and advances `*total` by the chunk read on
// success, 0 on failure (a bad block) with `*total` left unchanged.
// Unlike write_range_one_block(), there's no "out of space" case here
// (`block_for_index(..., 0)` never allocates), and the EOF clamp
// (`len` never exceeding what's actually in the file) is the caller's
// job -- see read_range_impl()/tfs_read_range_begin() below, both of
// which do it once, up front, before any block is read.
static int read_range_one_block(struct file *f, uint64_t offset, uint8_t *dst,
                                 uint32_t *total, uint32_t len) {
    uint64_t file_off = offset + *total;
    uint32_t block_index = (uint32_t)(file_off / FS_BLOCK_SIZE);
    uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
    uint32_t chunk = FS_BLOCK_SIZE - within;
    if (chunk > len - *total) chunk = len - *total;

    uint32_t blk = block_for_index(f, block_index, BLK_NO_ALLOC);
    if (!read_block(blk, g_io_scratch)) return 0;
    k_memcpy(dst + *total, (uint8_t *)g_io_scratch + within, chunk);
    *total += chunk;
    return 1;
}

static uint32_t read_range_impl(struct file *f, uint64_t offset, void *buf, uint32_t len) {
    if (offset >= f->size) return 0;
    uint64_t avail = f->size - offset;
    if ((uint64_t)len > avail) len = (uint32_t)avail;

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        // Mirror of write_range_impl()'s coalescing, same conditions
        // (block-aligned, >= 2 whole contiguous blocks, disk-backed) --
        // see contiguous_run(). A hole ends a run, so a sparse file
        // still reads correctly through the per-block path below.
        uint64_t file_off = offset + total;
        uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
        uint32_t whole_blocks = (len - total) / FS_BLOCK_SIZE;
        if (within == 0 && whole_blocks >= 2) {
            uint32_t first_block = 0;
            uint32_t run = contiguous_run(f, (uint32_t)(file_off / FS_BLOCK_SIZE),
                                          whole_blocks, BLK_NO_ALLOC, &first_block);
            if (run >= 2) {
                uint32_t bytes = run * FS_BLOCK_SIZE;
                if (!ata_read_sectors(first_block * FS_BLOCK_SECTORS,
                                      (int)(run * FS_BLOCK_SECTORS), g_run_scratch)) break;
                k_memcpy(dst + total, g_run_scratch, bytes);
                total += bytes;
                continue;
            }
        }
        if (!read_range_one_block(f, offset, dst, &total, len)) break;
    }
    return total;
}

// One block's worth of write_range_impl()'s (old) loop body -- pulled
// out so the exact same per-block logic backs both the blocking path
// (write_range_impl() below, which now just calls this in a tight loop
// same as before) and the steppable path (tfs_write_range_step()
// further down, which calls it once per external step() call). Neither
// caller's behavior changed by this split -- see write_range_impl()'s
// own comment. Returns 1 and advances `*total` by the chunk written on
// success, 0 on failure (out of space or a real disk error) with
// `*total` left unchanged.
static int write_range_one_block(struct file *f, uint64_t offset, const uint8_t *src,
                                  uint32_t *total, uint32_t len) {
    uint64_t file_off = offset + *total;
    uint32_t block_index = (uint32_t)(file_off / FS_BLOCK_SIZE);
    uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
    uint32_t chunk = FS_BLOCK_SIZE - within;
    if (chunk > len - *total) chunk = len - *total;

    int full_block = (within == 0 && chunk == FS_BLOCK_SIZE);
    uint32_t blk = block_for_index(f, block_index, full_block ? BLK_ALLOC_NOZERO : BLK_ALLOC);
    if (!blk) return 0; // out of space -- whatever was written before this point stays, see fs.h

    if (full_block) {
        k_memcpy(g_io_scratch, src + *total, FS_BLOCK_SIZE);
    } else {
        if (!read_block(blk, g_io_scratch)) return 0;
        k_memcpy((uint8_t *)g_io_scratch + within, src + *total, chunk);
    }
    if (!write_block(blk, g_io_scratch)) return 0;
    *total += chunk;
    return 1;
}

static int write_range_impl(struct file *f, uint64_t offset, const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t total = 0;
    int ok = 1;

    // See write_batch_begin()/end() above -- everything this loop
    // writes (data blocks AND any newly-allocated blocks' bitmap
    // sectors) only needs to be durable as a whole once this call
    // returns, not after each individual 4KB block. `ok` tracks
    // whether the loop finished cleanly so there's exactly one
    // write_batch_end() call regardless of which exit path was taken
    // -- an unmatched begin() would silently leave every future write
    // unflushed (see ata.h).
    write_batch_begin();
    while (total < len) {
        // Try the coalesced path first: it applies only when the
        // remaining range starts exactly on a block boundary and covers
        // at least two whole contiguous blocks. Everything else (the
        // unaligned head, the partial tail, a fragmented region) drops
        // straight through to the original one-block-at-a-time code.
        uint64_t file_off = offset + total;
        uint32_t within = (uint32_t)(file_off % FS_BLOCK_SIZE);
        uint32_t whole_blocks = (len - total) / FS_BLOCK_SIZE;
        if (g_disk_backed && within == 0 && whole_blocks >= 2) {
            uint32_t first_block = 0;
            uint32_t run = contiguous_run(f, (uint32_t)(file_off / FS_BLOCK_SIZE),
                                          whole_blocks, BLK_ALLOC_NOZERO, &first_block);
            if (run >= 2) {
                uint32_t bytes = run * FS_BLOCK_SIZE;
                k_memcpy(g_run_scratch, src + total, bytes);
                if (!ata_write_sectors(first_block * FS_BLOCK_SECTORS,
                                       (int)(run * FS_BLOCK_SECTORS), g_run_scratch)) { ok = 0; break; }
                total += bytes;
                continue;
            }
        }
        if (!write_range_one_block(f, offset, src, &total, len)) { ok = 0; break; }
    }
    write_batch_end();

    if (!ok) return 0;
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

// Returns 1 if the record is genuinely on disk, 0 if any step of the
// journal-protected write failed. That return used to be discarded at
// every call site, which meant a failed metadata write was reported to
// the caller as a successful touch/write/delete -- the file "existed"
// until the next reboot and then didn't, with nothing logged. Now every
// caller checks it (see tfs_touch()/tfs_mkdir()/tfs_write()/
// tfs_delete()/tfs_write_range()), and the failure is always logged
// here regardless of `debug fs`, matching persist_bitmap_sector_with_
// retry()'s convention for the same class of problem: silently diverging
// from what's on disk is a correctness bug, not noise.
//
// A failure partway through is still safe against corruption -- that's
// what the journal is for. Either the commit header never landed (the
// table slot keeps its old contents) or it did and replay_journal()
// finishes the job on the next boot.
static int persist_record(int index) {
    if (!g_disk_backed) return 1;
    uint8_t buf[FS_RECORD_BYTES];
    serialize_record(&files[index], buf);
    uint32_t checksum = fnv1a(buf, FS_RECORD_BYTES);

    // Journal-batched flush: this sequence used to take a synchronous
    // CMD_CACHE_FLUSH after every one of its four writes, on the
    // reasoning that a write-ahead journal needs each write durable
    // before the next is issued. Two of those four barriers turn out to
    // carry no weight, and dropping them halves the cost of every
    // metadata operation without changing what replay_journal()
    // guarantees:
    //
    //   1. journal data   -- NO barrier needed. A torn write here is
    //      caught by the FNV-1a checksum in the header below, and
    //      replay discards the entry. "The operation didn't happen" is
    //      a valid crash outcome; a silently-wrong one wouldn't be.
    //   2. commit header  -- BARRIER REQUIRED. Once the table slot is
    //      being overwritten, the journal entry is the only copy of the
    //      old-or-new record that can survive a tear. It has to be on
    //      the platter before that write is issued.
    //   3. table slot     -- BARRIER REQUIRED. The journal entry can't
    //      be retired until the real slot is durable, or a crash
    //      between the two leaves a torn slot with nothing to replay.
    //   4. clear header   -- NO barrier needed. Losing this write costs
    //      exactly one redundant replay on the next boot, which rewrites
    //      the same bytes to the same slot. Idempotent.
    //
    // ata_flush_now() rather than ata_flush_end() for the two real
    // barriers, because persist_record() can be called from inside an
    // outer write batch (tfs_check()'s repair pass does exactly this) --
    // where end() wouldn't flush at all, since its depth never reaches
    // 0, and the journal would silently lose every barrier it has. See
    // ata.h.
    ata_flush_begin();

    int ok = 1;
    for (int s = 0; s < FS_RECORD_SECTORS && ok; s++) {
        ok = ata_write_sector(FS_JOURNAL_DATA_LBA + s, buf + (uint32_t)s * ATA_SECTOR_SIZE);
    }
    if (ok) ok = write_journal_header(1, (uint32_t)index, checksum);
    if (ok) ata_flush_now(); // barrier: commit header (and the data it describes) durable

    if (ok) ok = write_table_slot(index, buf);
    if (ok) ata_flush_now(); // barrier: real table slot durable before the entry is retired

    if (ok) {
        write_journal_header(0, 0, 0);
        ata_flush_end_no_flush(); // trailing clear is safe to lose -- see above
        return 1;
    }
    ata_flush_end_no_flush();

    klog_write("fs: WARNING -- record slot "); klog_write_dec((uint32_t)index);
    klog_write(" (lba "); klog_write_dec(record_lba(index));
    klog_write(") failed to persist -- in-memory state may not match disk\n");
    return 0;
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
        if (write_table_slot((int)slot, buf)) {
            klog_write("fs: replayed a pending journal entry from an unclean shutdown\n");
        } else {
            // Leave the journal header COMMITTED so the next boot tries
            // again -- clearing it here would drop the entry for good.
            klog_write("fs: WARNING -- couldn't write the replayed journal entry to its table slot;\n");
            klog_write("fs: leaving it pending for the next boot\n");
            return;
        }
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

    // Reading the superblock and JUDGING the superblock are two
    // different questions, and conflating them used to be a data-loss
    // bug: this was one `if (ata_read_sector(...) && magic ok) {load}
    // else {format}`, so a *failed read* took the same branch as a
    // genuinely foreign disk and reformatted a perfectly good
    // filesystem. Not hypothetical -- ata_read_sector() gives up after
    // ATA_DMA_MAX_RETRIES (3) exhausted attempts, and transient
    // 3-in-a-row DMA misses are exactly what this project has already
    // seen on real hardware (see ata.c's dma_transfer_with_retry()
    // comment and the bitmap-persist retry above). So: retry the read
    // itself a few more times, and if it STILL can't be read, refuse to
    // touch the disk at all -- degrade to RAM-only for this boot rather
    // than destroying what might be a fine filesystem. A blank/foreign
    // disk still formats normally, because that path is only reached
    // when the read genuinely SUCCEEDED and the bytes just aren't ours.
    g_total_blocks = FS_DISK_TOTAL_BLOCKS;
    clamp_total_blocks_to_disk();

    // A drive that can't even hold the reserved metadata region (
    // superblock + journal + record table + bitmap) can't host this
    // filesystem at all. Formatting it would "succeed" -- every write
    // lands somewhere the drive silently discards -- and the first real
    // symptom would be data that reads back as zeros, which is exactly
    // what a 0-length disk image produced before this check existed.
    // Degrade to RAM-only instead, same as an unreadable superblock.
    if (g_total_blocks <= FS_DATA_START_BLOCK) {
        klog_write("fs: disk is too small to hold the filesystem metadata region (");
        klog_write_dec(g_total_blocks); klog_write(" blocks, need more than ");
        klog_write_dec(FS_DATA_START_BLOCK);
        klog_write(") -- running RAM-only this boot\n");
        return 0;
    }

    uint8_t sb[ATA_SECTOR_SIZE];
    int sb_read = 0;
    for (int attempt = 1; attempt <= FS_SUPERBLOCK_READ_MAX_RETRIES; attempt++) {
        if (ata_read_sector(FS_SUPERBLOCK_LBA, sb)) { sb_read = 1; break; }
    }
    if (!sb_read) {
        klog_write("fs: disk present but the superblock could not be read after ");
        klog_write_dec((uint32_t)FS_SUPERBLOCK_READ_MAX_RETRIES);
        klog_write(" attempts\n");
        klog_write("fs: NOT formatting -- refusing to destroy a possibly-good disk\n");
        klog_write("fs: running RAM-only this boot; files will not persist\n");
        return 0; // g_disk_backed stays 0 -- same degrade path as "no disk found"
    }

    if (sb[0] == FS_DISK_MAGIC0 && sb[1] == FS_DISK_MAGIC1 &&
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
        int slots_failed = 0;
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!persist_record(i)) slots_failed++;
        }
        if (slots_failed) {
            // Each failure already logged its own slot/LBA in
            // persist_record(). A blank record that didn't land isn't
            // corruption (the slot's `used` byte is whatever was there
            // before, and tfs_init() will read it back as-is next boot),
            // but it does mean this format didn't fully take -- say so
            // rather than claiming a clean format.
            klog_write("fs: WARNING -- format left "); klog_write_dec((uint32_t)slots_failed);
            klog_write(" of "); klog_write_dec((uint32_t)FS_MAX_FILES);
            klog_write(" record slots unwritten\n");
        }
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
    // Roll the slot back on a failed persist rather than reporting a
    // success the disk doesn't agree with -- an in-memory-only file
    // that vanishes at reboot is worse than a clean "couldn't create
    // it", and the caller (fs_touch()) already has a 0 return for it.
    if (!persist_record(idx)) { k_memset(f, 0, sizeof(*f)); return 0; }
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
    if (!persist_record(idx)) { k_memset(f, 0, sizeof(*f)); return 0; } // same rollback as tfs_touch()
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
    int idx = (int)(f - files);
    uint64_t start;
    if (append) {
        start = f->size;
    } else {
        // Truncate. Detach the old blocks and persist the now-empty
        // record BEFORE returning them to the bitmap -- see
        // detach_blocks()'s comment for the double-allocation window
        // that ordering closes. The write_batch_begin()/end() pair
        // around the actual freeing is what keeps free_all_blocks()
        // from issuing one synchronous bitmap-sector write per freed
        // block (see write_batch_end()); it nests safely with the one
        // write_range_impl() opens below.
        struct file old_blocks;
        detach_blocks(f, &old_blocks);
        uint64_t old_size = f->size;
        f->size = 0;
        if (!persist_record(idx)) {
            reattach_blocks(f, &old_blocks);
            f->size = old_size;
            return 0;
        }
        write_batch_begin();
        free_all_blocks(&old_blocks);
        write_batch_end();
        start = 0;
    }

    if (data_len > 0 && !write_range_impl(f, start, data, data_len)) {
        // Partial data may have landed and blocks may have been
        // allocated -- persist so the record actually references them
        // (fs.h documents the file's state on partial failure as "what
        // was written before the failure", and an unpersisted record
        // would instead leak those blocks: allocated in the bitmap,
        // referenced by nothing after a reboot).
        persist_record(idx);
        return 0;
    }

    rtc_read_local(&f->modified);
    return persist_record(idx);
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
    }

    int idx = (int)(f - files);
    struct file old_blocks;
    k_memset(&old_blocks, 0, sizeof(old_blocks));
    detach_blocks(f, &old_blocks); // no-op for a directory -- it has no blocks

    struct file saved = *f;
    f->used = 0;
    f->size = 0;
    // Record first, blocks second -- see detach_blocks(). If the record
    // can't be persisted, nothing has been freed yet, so restoring the
    // in-memory entry leaves memory and disk agreeing again.
    if (!persist_record(idx)) { *f = saved; return 0; }

    if (saved.type == FS_TYPE_FILE) {
        // Batched: free_all_blocks() calls persist_bitmap_bit() once per
        // freed block, and unbatched that's one synchronous ATA write
        // per block, rewriting the same handful of bitmap sectors over
        // and over (see write_batch_end()). Found live: `stress <mb>`'s
        // own cleanup delete of its test file (tens of thousands of
        // blocks for a multi-hundred-MB run) made "reading back and
        // verifying" reach 100% and then visibly sit there before the
        // final PASSED line -- this batch is what that pause was
        // actually spent on.
        write_batch_begin();
        free_all_blocks(&old_blocks);
        write_batch_end();
    }
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
    int idx = (int)(f - files);
    if (!write_range_impl(f, offset, buf, len)) {
        persist_record(idx); // keep any partially-written blocks referenced, see tfs_write()
        return 0;
    }
    rtc_read_local(&f->modified);
    return persist_record(idx);
}

// ---------------------------------------------------------------------
// Steppable write (Phase 2 of the async-I/O roadmap item, see
// docs/roadmap.md) -- fs_write_range_begin()/fs_write_range_step()'s
// TFS2 backend. Same per-block work as write_range_impl() above (built
// from the same write_range_one_block() helper), but split so a caller
// can advance it one block at a time from OUTSIDE this file, instead of
// this file looping to completion internally. No real caller uses this
// yet -- fs_write_range() above and everything built on it still go
// through the unchanged blocking write_range_impl(). Proven standalone
// via the new `steptest [mb]` shell command (see apps/shell_sys.c), the
// same "prove the primitive works in isolation first" approach Phase 1
// took with `dmatest`.
//
// Heap-allocated (not a single static like ata.c's g_pending) since
// there's no hardware register forcing "only one at a time" the way
// ata.c's shared PRD/bounce buffer does -- a future caller COULD have
// two steppable writes in flight (e.g. two GUI windows each mid-save).
// Nothing does yet, but there's no reason to bake in ata.c's tighter
// constraint here when it isn't actually required.
struct tfs_write_step {
    struct file *f;
    int file_index;        // for the finishing persist_record() call below
    const uint8_t *src;
    uint64_t offset;
    uint32_t len;
    uint32_t total;
};

// Mirrors tfs_write_range()'s own path handling (normalize, create the
// file if it doesn't exist yet, reject a directory) just above. Returns
// NULL on any of the same failures fs_write_range() already reports
// via a 0 return: bad/too-long path, the path names a directory, or
// the file table is full and creation fails. On success, the write
// batch is already open (write_batch_begin()) -- it closes in
// fs_write_range_step() once stepping reaches a terminal result,
// matching write_range_impl()'s single begin/end pair around the whole
// operation.
static void *tfs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return NULL;
    struct file *f = find(norm);
    if (f && f->type == FS_TYPE_DIR) return NULL;
    if (!f) {
        if (!tfs_touch(norm)) return NULL;
        f = find(norm);
    }

    struct tfs_write_step *st = kmalloc(sizeof(struct tfs_write_step));
    if (!st) return NULL;
    st->f = f;
    st->file_index = (int)(f - files);
    st->src = (const uint8_t *)buf;
    st->offset = offset;
    st->len = len;
    st->total = 0;
    write_batch_begin();
    return st;
}

// Advances one block's worth of work (or, if `len` is 0 or already
// fully written, resolves immediately) and returns FS_STEP_PENDING
// (call again), FS_STEP_DONE, or FS_STEP_FAILED. On either terminal
// result the handle is already freed and the write batch already
// closed -- same "caller doesn't need to clean up separately" contract
// dma_transfer_poll() (ata.c) established in Phase 1. On FS_STEP_DONE,
// f->size/modified/the on-disk directory record are all updated first,
// exactly matching what tfs_write_range() does above after its own
// write_range_impl() call returns success -- a caller can't tell from
// the file's own state afterward whether it was written steppably or
// in one blocking call.
static int tfs_write_range_step(void *handle) {
    struct tfs_write_step *st = (struct tfs_write_step *)handle;

    if (st->total < st->len) {
        if (!write_range_one_block(st->f, st->offset, st->src, &st->total, st->len)) {
            write_batch_end();
            kfree(st);
            return 2 /* FS_STEP_FAILED, see fs.h */;
        }
        if (st->total < st->len) return 0 /* FS_STEP_PENDING */;
    }

    // Finished (either the loop above just wrote the last block, or
    // len was 0 to begin with) -- same tail tfs_write_range() above
    // runs after a successful blocking write: close the batch, grow
    // f->size if needed, stamp modified time, persist the directory
    // record.
    write_batch_end();
    if (st->offset + st->total > st->f->size) st->f->size = st->offset + st->total;
    rtc_read_local(&st->f->modified);
    int persisted = persist_record(st->file_index);
    kfree(st);
    // Same contract as the blocking path: a write whose directory
    // record didn't reach disk is a failed write, not a successful one.
    return persisted ? 1 /* FS_STEP_DONE */ : 2 /* FS_STEP_FAILED */;
}

// ---------------------------------------------------------------------
// Steppable read (Phase 4 of the async-I/O roadmap item, see
// docs/roadmap.md) -- fs_read_range_begin()/fs_read_range_step()'s
// TFS2 backend. Same per-block work as read_range_impl() above (built
// from the same read_range_one_block() helper), split the same way the
// write side was for Phase 2. Simpler than the write struct/step pair:
// no write batch to open/close, and nothing on disk to update on
// success (a read never changes size/modified/the directory record).
// First real caller: apps/notepad.c's Open... (Milestone 1 phase 4).
struct tfs_read_step {
    struct file *f;
    uint8_t *dst;
    uint64_t offset;
    uint32_t len; // already EOF-clamped by tfs_read_range_begin(), same as read_range_impl() clamps up front
    uint32_t total;
};

// Mirrors tfs_read_range()'s own path handling (normalize, must
// already exist, reject a directory) just above -- unlike the write
// side, never creates the file. Returns NULL on any of the same
// failures fs_read_range() already reports via a 0 return: bad/
// too-long path, the path names a directory, or the file doesn't
// exist. `len` is clamped against the file's actual size here, once,
// the same EOF handling read_range_impl() does up front -- so a caller
// that steps to FS_STEP_DONE always gets exactly the same byte count
// fs_read_range() would have returned for the same call.
static void *tfs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return NULL;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return NULL;

    if (offset >= f->size) len = 0;
    else {
        uint64_t avail = f->size - offset;
        if ((uint64_t)len > avail) len = (uint32_t)avail;
    }

    struct tfs_read_step *st = kmalloc(sizeof(struct tfs_read_step));
    if (!st) return NULL;
    st->f = f;
    st->dst = (uint8_t *)buf;
    st->offset = offset;
    st->len = len;
    st->total = 0;
    return st;
}

// Advances one block's worth of work (or, if `len` was already clamped
// to 0 by begin(), resolves immediately) and returns FS_STEP_PENDING
// (call again), FS_STEP_DONE, or FS_STEP_FAILED. `out_total` is
// written every call with the bytes copied into `buf` so far -- see
// fs.h's fs_read_range_step() for why the read side needs this and the
// write side doesn't. On either terminal result the handle is already
// freed -- don't call step() again or free anything.
static int tfs_read_range_step(void *handle, uint32_t *out_total) {
    struct tfs_read_step *st = (struct tfs_read_step *)handle;

    if (st->total < st->len) {
        if (!read_range_one_block(st->f, st->offset, st->dst, &st->total, st->len)) {
            if (out_total) *out_total = st->total;
            kfree(st);
            return 2 /* FS_STEP_FAILED, see fs.h */;
        }
        if (st->total < st->len) {
            if (out_total) *out_total = st->total;
            return 0 /* FS_STEP_PENDING */;
        }
    }

    if (out_total) *out_total = st->total;
    kfree(st);
    return 1 /* FS_STEP_DONE */;
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

    // A disk smaller than the selftest's own offset can't run this at
    // all -- and since the allocator is now clamped to the drive's real
    // capacity (see clamp_total_blocks_to_disk()), attempting it would
    // fail legitimately and print a FAILED line that reads like a
    // filesystem bug rather than "this disk is 512MB". Skip explicitly
    // instead, and say which it was.
    uint64_t capacity = (uint64_t)g_total_blocks * FS_BLOCK_SIZE;
    if (TFS_SELFTEST_OFFSET + sizeof(pattern) > capacity) {
        klog_write("fs: selftest skipped -- disk is too small for the triple-indirect offset\n");
        return;
    }

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
        total = g_total_blocks - FS_DATA_START_BLOCK;
        for (uint32_t b = FS_DATA_START_BLOCK; b < g_total_blocks; b++) {
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

// ---------------------------------------------------------------------
// Consistency check / repair -- fs_check()'s TFS2 backend (`fsck`).
//
// The reason this exists: the truncate/delete paths deliberately
// persist a record referencing nothing BEFORE returning its blocks to
// the bitmap (see detach_blocks()), which means an interrupted
// operation leaks blocks rather than double-allocating them. That's the
// right trade -- a leak costs space, a double-allocation costs data --
// but it's only the right trade if something can eventually reclaim the
// leak. This is that something. See docs/decisions.md.
//
// The check is the classic mark-and-compare: walk every in-use record's
// block tree marking a "referenced" bitmap, then compare that bitmap
// against the real free-block bitmap. Blocks set in one and not the
// other are the two interesting disagreements, in both directions.
//
// g_fsck_seen is a static array, not a kmalloc()'d one, for the same
// reason g_bitmap right above it is (its size is a compile-time
// constant either way) plus one specific to a repair tool: a 288KB
// allocation needs 72 contiguous frames from pmm, and failing to get
// them would mean "can't check the disk" exactly when something is
// already wrong. A repair tool that can fail for lack of memory is a
// repair tool you can't rely on.
static uint8_t g_fsck_seen[FS_BITMAP_BYTES];

// A block number is usable only if it's inside the data region -- below
// FS_DATA_START_BLOCK is reserved metadata (superblock/journal/table/
// bitmap), at or above g_total_blocks is past the end of the drive.
// Block 0 is the "no block" sentinel and is never a pointer to check.
static int block_in_range(uint32_t b) {
    return b >= FS_DATA_START_BLOCK && b < g_total_blocks;
}

// Marks one block as referenced, counting a double-allocation if it was
// already marked. Returns 1 if this was the first reference (so the
// caller knows whether to descend into it).
static int fsck_mark(uint32_t b, struct fs_check_result *r) {
    if (bit_test(g_fsck_seen, b)) { r->double_allocated++; return 0; }
    bit_set(g_fsck_seen, b, 1);
    r->blocks_referenced++;
    if (!bit_test(g_bitmap, b)) {
        r->referenced_but_free++;
        return 2; // caller repairs; still a first reference, so descend
    }
    return 1;
}

// Walks one indirect subtree, marking every block it references.
// `depth` follows walk_indirect()'s convention (1 = this block's
// entries are data pointers, 2/3 = one/two more levels below). Reuses
// g_free_scratch the same way free_tree() does -- each active frame in
// one call chain has a distinct depth, so frames never alias, and this
// file is single-threaded (no free_tree() can be running concurrently).
static void fsck_walk_tree(uint32_t block, int depth, int repair, struct fs_check_result *r) {
    if (!read_block(block, g_free_scratch[depth])) return;
    int dirty = 0;
    for (int i = 0; i < FS_PTRS_PER_BLOCK; i++) {
        uint32_t child = g_free_scratch[depth][i];
        if (child == 0) continue;
        if (!block_in_range(child)) {
            r->out_of_range++;
            if (repair) {
                g_free_scratch[depth][i] = 0; // becomes a hole -- reads as zero
                r->pointers_cleared++;
                dirty = 1;
            }
            continue;
        }
        int first = fsck_mark(child, r);
        if (first == 2 && repair) {
            bit_set(g_bitmap, child, 1);
            persist_bitmap_bit(child);
            r->marked_allocated++;
        }
        if (first && depth > 1) fsck_walk_tree(child, depth - 1, repair, r);
    }
    if (dirty) write_block(block, g_free_scratch[depth]);
}

// One record's whole block tree: the direct pointers, then each
// indirect root. Returns 1 if the record itself was modified (an
// out-of-range direct pointer cleared), so the caller can persist it.
static int fsck_walk_record(struct file *f, int repair, struct fs_check_result *r) {
    int record_dirty = 0;
    uint32_t *roots[3] = { &f->single_indirect, &f->double_indirect, &f->triple_indirect };

    for (int i = 0; i < FS_N_DIRECT + 3; i++) {
        int is_direct = i < FS_N_DIRECT;
        uint32_t *slot = is_direct ? &f->direct[i] : roots[i - FS_N_DIRECT];
        uint32_t b = *slot;
        if (b == 0) continue;
        if (!block_in_range(b)) {
            r->out_of_range++;
            if (repair) { *slot = 0; r->pointers_cleared++; record_dirty = 1; }
            continue;
        }
        int first = fsck_mark(b, r);
        if (first == 2 && repair) {
            bit_set(g_bitmap, b, 1);
            persist_bitmap_bit(b);
            r->marked_allocated++;
        }
        // An indirect root's depth is its position: single = 1, double
        // = 2, triple = 3. Direct pointers have no subtree.
        if (first && !is_direct) fsck_walk_tree(b, i - FS_N_DIRECT + 1, repair, r);
    }
    return record_dirty;
}

static int tfs_check(int repair, struct fs_check_result *out) {
    struct fs_check_result r;
    k_memset(&r, 0, sizeof(r));
    if (!g_disk_backed) {
        if (out) *out = r;
        return 0; // RAM-only: no persistent bitmap, nothing to reconcile
    }

    k_memset(g_fsck_seen, 0, sizeof(g_fsck_seen));

    // One batch around the whole pass so a repair that touches many
    // bitmap sectors writes each one once at the end, not once per
    // block -- same reasoning as free_all_blocks()'s batch.
    write_batch_begin();

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        r.records_used++;
        if (fsck_walk_record(&files[i], repair, &r) && repair) persist_record(i);
    }

    // Everything allocated but unreferenced is a leak. Only the data
    // region is in scope: blocks below FS_DATA_START_BLOCK are the
    // permanently-allocated metadata region and must stay that way.
    for (uint32_t b = FS_DATA_START_BLOCK; b < g_total_blocks; b++) {
        if (!bit_test(g_bitmap, b)) continue;
        if (bit_test(g_fsck_seen, b)) continue;
        r.leaked++;
        if (repair) {
            free_block(b); // clears the bit and marks its sector dirty
            r.reclaimed++;
        }
    }

    write_batch_end();

    // Freed blocks are almost certainly below the current scan hint;
    // reset it so the next allocation actually finds them.
    if (r.reclaimed) g_bitmap_scan_hint = FS_DATA_START_BLOCK;

    if (out) *out = r;
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
    .write_range_begin = tfs_write_range_begin,
    .write_range_step = tfs_write_range_step,
    .read_range_begin = tfs_read_range_begin,
    .read_range_step = tfs_read_range_step,
    .is_dir = tfs_is_dir,
    .exists = tfs_exists,
    .list = tfs_list,
    .stat = tfs_stat,
    .disk_usage = tfs_disk_usage,
    .check = tfs_check,
};
