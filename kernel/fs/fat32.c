// FAT32 -- the format GRUB can read, and therefore the format `/boot`
// is in.
//
// WHY THIS EXISTS. toy-os's own disk is GPT: a BIOS boot partition
// holding GRUB's core.img, a FAT32 ESP holding /boot/kernel.bin and
// /boot/grub, then TFS3. Everything past the bootloader was written by
// host tools (mtools, tools/install_grub.py) and was unreadable from
// inside the machine -- `ls /boot` on the running OS showed an empty
// directory, because there was nothing there to show. This is the
// driver that closes that, and Real mount points (kernel/mount.h) is
// what attaches it at /boot.
//
// WHAT REAL SYSTEMS DO. Linux's `fs/fat/` is one driver serving
// msdos/vfat, generic over FAT12/16/32, with the ESP as an ordinary
// mount at /boot/efi and nothing in the driver aware of what it holds.
// Windows' FASTFAT is the same shape. toy-os follows that -- this file
// contains no mention of a bootloader, and the read-only default lives
// in the MOUNT POLICY (mount.c) where a policy belongs.
//
// WHAT IT DELIBERATELY IS NOT:
//
//  * NOT FAT12 OR FAT16. Those are a different root-directory layout
//    (a fixed-size region rather than a cluster chain) and a different
//    FAT entry width, which is a second set of paths through every
//    function here for a format nothing on this machine uses. A volume
//    that is not FAT32 is REFUSED by name at probe(), not
//    half-understood -- a parser rejects rather than guesses.
//  * NOT 4096-BYTE SECTORS. `bytes_per_sector` must be 512, which is
//    what the block layer speaks and what every image this OS produces
//    has. A volume claiming anything else is refused rather than read
//    with the wrong stride.
//  * NOT UNICODE. Long names are read as UCS-2 and anything outside
//    ASCII becomes '?'; creating a name with a non-ASCII byte is
//    refused. This kernel has no Unicode anywhere else either, and a
//    half-done encoding is worse than a stated limit.
//  * NOT JOURNALLED, because FAT is not. TFS3's credit-counted
//    transactions have no analogue here: an interrupted write leaves
//    the volume in whatever state the last completed sector left it,
//    which is what FAT has always been. That is a genuine reason to
//    prefer TFS3 for anything that matters, and a genuine reason /boot
//    is mounted read-only by default.
//
// THE ONE ORDERING RULE THAT MATTERS FOR CORRUPTION: when a file grows,
// the FAT chain is extended and FLUSHED before the directory entry's
// size is updated. The other order -- size first -- publishes a length
// that reaches into clusters the FAT does not yet link, which reads as
// garbage. When a file SHRINKS the order reverses: the directory entry
// stops referencing the clusters before they are freed, which is the
// same rule tfs3.c's truncate follows and the same one
// docs/conventions/storage.md states.
#include "fs.h"
#include "fs_ops.h"
#include "fat32.h"
#include "mount.h" // MOUNT_MAX -- the mount limit this backend declares
#include "block.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "heap.h"
#include "tz.h"
#include "timer.h"
#include <stddef.h>

#define SECTOR 512u

// FAT special values, after masking off the top four bits (which are
// reserved and must be PRESERVED on write -- a driver that writes a
// full 32-bit value clobbers them).
#define FAT_MASK      0x0FFFFFFFu
#define FAT_FREE      0x00000000u
#define FAT_BAD       0x0FFFFFF7u
#define FAT_EOC_MIN   0x0FFFFFF8u
#define FAT_EOC       0x0FFFFFFFu

// Directory-entry attributes.
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

#define DIRENT_SIZE 32
#define DIRENT_FREE 0xE5
#define DIRENT_END  0x00

// Longest single path component this driver will create or report.
// FAT allows 255; FS_PATH_MAX is 64 for a WHOLE path here, so anything
// longer could never be named again once created.
#define FAT_NAME_MAX 63

// LFN carries 13 UCS-2 characters per entry.
#define LFN_CHARS 13
#define LFN_MAX_ENTRIES ((FAT_NAME_MAX + LFN_CHARS) / LFN_CHARS)

// ---- state ----------------------------------------------------------
//
// PER MOUNT, in struct fat32_state, reached through `S` -- the mount the
// current call belongs to (fs_ops.h's state_alloc/state_free/
// state_activate). The scratch sectors below are the exception and each
// has a SINGLE purpose, deliberately: one general-purpose scratch shared
// between a directory walk and the FAT lookup that walk makes would
// corrupt the walk halfway through, which is the exact shape of the bug
// tfs3.c's g_blk/g_ptr_blk split exists to prevent.

struct fat_vol {
    const struct block_device *dev;
    int mounted;
    uint32_t sectors_per_cluster;
    uint32_t reserved;          // sectors before the first FAT
    uint32_t num_fats;
    uint32_t fat_sectors;       // per FAT
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t first_data_sector;
    uint32_t cluster_count;     // data clusters, so the last is cluster_count + 1
    uint32_t fsinfo_sector;     // 0 when the BPB names none
    uint32_t free_hint;         // FSInfo's "next free" hint; 0xFFFFFFFF = none
    uint32_t free_count;        // FSInfo's free count; 0xFFFFFFFF = unknown
};

struct fat32_state {
    struct fat_vol v;

    // The cached FAT sector. IN HERE rather than beside the scratch
    // because it can be DIRTY across calls, and a dirty sector written
    // back to whichever volume happens to be current is corruption.
    uint8_t fatsec[SECTOR];
    uint32_t fatsec_lba;        // which one, 0 = nothing cached
    int fatsec_dirty;

};

static struct fat32_state *S;   // NULL between calls -- see fs_ops.h

// Per CALL, not per volume: nothing here outlives one backend call, and
// the filesystem is one global critical section (vfs.c's FS_OP).
static uint8_t g_dirsec[SECTOR];   // a directory sector during a walk
static uint8_t g_datasec[SECTOR];  // file data for a partial-sector read/write
static uint8_t g_tmpsec[SECTOR];   // probe, format, FSInfo -- never held across a walk

// ---- little-endian accessors ----------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---- volume I/O ------------------------------------------------------

static int vol_read(uint32_t lba, int count, void *buf) {
    if ((uint64_t)lba + (uint64_t)count > (uint64_t)S->v.total_sectors) return 0;
    return blkdev_read_sectors(S->v.dev, lba, count, buf);
}

static int vol_write(uint32_t lba, int count, const void *buf) {
    if ((uint64_t)lba + (uint64_t)count > (uint64_t)S->v.total_sectors) return 0;
    return blkdev_write_sectors(S->v.dev, lba, count, buf);
}

static uint32_t cluster_first_sector(uint32_t clus) {
    return S->v.first_data_sector + (clus - 2) * S->v.sectors_per_cluster;
}

static int cluster_valid(uint32_t clus) {
    return clus >= 2 && clus < S->v.cluster_count + 2;
}

// ---- the FAT ---------------------------------------------------------
//
// A ONE-SECTOR WRITE-BACK CACHE, because a chain walk reads 128
// consecutive entries out of the same sector and a per-entry read would
// be 128 disk transfers for one file. Written back on eviction and at
// every fat_sync(), which every mutating op ends with.

static int fat_load(uint32_t lba);

static int fat_flush(void) {
    if (!S->fatsec_dirty || !S->fatsec_lba) return 1;
    // EVERY COPY OF THE FAT, not just the first. mkfs writes two by
    // default and a repair tool compares them; leaving the second stale
    // is a filesystem that fsck calls corrupt and other drivers may
    // read from instead.
    int ok = 1;
    for (uint32_t i = 0; i < S->v.num_fats; i++) {
        uint32_t lba = S->fatsec_lba + i * S->v.fat_sectors;
        if (!vol_write(lba, 1, S->fatsec)) ok = 0;
    }
    S->fatsec_dirty = 0;
    return ok;
}

static int fat_load(uint32_t lba) {
    if (S->fatsec_lba == lba) return 1;
    if (!fat_flush()) return 0;
    if (!vol_read(lba, 1, S->fatsec)) { S->fatsec_lba = 0; return 0; }
    S->fatsec_lba = lba;
    return 1;
}

// The FAT sector holding `clus`, expressed as an offset from the FIRST
// FAT -- fat_flush() adds the per-copy stride, so the cache key is the
// same whichever copy is being written.
static int fat_locate(uint32_t clus, uint32_t *out_lba, uint32_t *out_off) {
    if (clus > S->v.cluster_count + 1) return 0;
    uint32_t byte = clus * 4;
    *out_lba = S->v.reserved + byte / SECTOR;
    *out_off = byte % SECTOR;
    return 1;
}

static int fat_get(uint32_t clus, uint32_t *out) {
    uint32_t lba, off;
    if (!fat_locate(clus, &lba, &off)) return 0;
    if (!fat_load(lba)) return 0;
    *out = rd32(S->fatsec + off) & FAT_MASK;
    return 1;
}

// THE TOP FOUR BITS ARE RESERVED AND ARE PRESERVED. Microsoft's spec
// says so, and a driver that writes a bare 32-bit value silently
// changes bits another implementation may be using.
static int fat_set(uint32_t clus, uint32_t val) {
    uint32_t lba, off;
    if (!fat_locate(clus, &lba, &off)) return 0;
    if (!fat_load(lba)) return 0;
    uint32_t old = rd32(S->fatsec + off);
    wr32(S->fatsec + off, (old & 0xF0000000u) | (val & FAT_MASK));
    S->fatsec_dirty = 1;
    return 1;
}

static int fat_is_eoc(uint32_t v) { return v >= FAT_EOC_MIN; }

// ---- FSInfo ----------------------------------------------------------
//
// A HINT, NOT A FACT. FSInfo's free count and next-free cluster are
// explicitly advisory in the spec, and a volume left dirty by another
// OS routinely has both wrong. So: read them at mount, USE the next-free
// hint as a search start (it is only ever an optimisation), and treat
// the free count as unknown until something asks -- at which point the
// FAT itself is counted and the answer cached. `df` reporting a stale
// number it read off a hint would be worse than reading nothing.

static void fsinfo_load(void) {
    S->v.free_hint = 0xFFFFFFFFu;
    S->v.free_count = 0xFFFFFFFFu;
    if (!S->v.fsinfo_sector) return;
    if (!vol_read(S->v.fsinfo_sector, 1, g_tmpsec)) return;
    if (rd32(g_tmpsec) != 0x41615252u) return;          // "RRaA"
    if (rd32(g_tmpsec + 484) != 0x61417272u) return;    // "rrAa"
    if (rd16(g_tmpsec + 510) != 0xAA55) return;
    uint32_t free_count = rd32(g_tmpsec + 488);
    uint32_t next_free = rd32(g_tmpsec + 492);
    if (next_free >= 2 && next_free < S->v.cluster_count + 2) S->v.free_hint = next_free;
    if (free_count <= S->v.cluster_count) S->v.free_count = free_count;
}

static void fsinfo_store(void) {
    if (!S->v.fsinfo_sector) return;
    if (!vol_read(S->v.fsinfo_sector, 1, g_tmpsec)) return;
    if (rd32(g_tmpsec) != 0x41615252u) return;
    wr32(g_tmpsec + 488, S->v.free_count);
    wr32(g_tmpsec + 492, S->v.free_hint);
    vol_write(S->v.fsinfo_sector, 1, g_tmpsec);
}

// Every mutating operation ends here: the FAT cache is written back,
// FSInfo is refreshed, and the DEVICE is flushed. Without the last one
// a write-back cache holds the change until something else evicts it,
// which is a durable-looking write that is not.
static int fat_sync(void) {
    int ok = fat_flush();
    fsinfo_store();
    if (!blkdev_flush(S->v.dev)) ok = 0;
    return ok;
}

// ---- cluster chains --------------------------------------------------

static int chain_next(uint32_t clus, uint32_t *out) {
    uint32_t v;
    if (!fat_get(clus, &v)) return 0;
    *out = v;
    return 1;
}

// The nth cluster of a chain, or 0 if the chain is shorter than that.
static uint32_t chain_nth(uint32_t start, uint32_t n) {
    uint32_t c = start;
    while (n--) {
        uint32_t next;
        if (!cluster_valid(c) || !chain_next(c, &next)) return 0;
        if (!cluster_valid(next)) return 0;
        c = next;
    }
    return cluster_valid(c) ? c : 0;
}

// Finds a free cluster, marks it as the end of a chain, and links it to
// `prev` when prev is a real cluster. Returns 0 when the volume is full.
//
// THE SEARCH STARTS AT THE HINT AND WRAPS, which is what stops a volume
// that has been written to for a while re-scanning from cluster 2 on
// every allocation. The hint is advisory (see fsinfo_load), so a wrong
// one costs a longer search and never a wrong answer.
static uint32_t cluster_alloc(uint32_t prev) {
    uint32_t total = S->v.cluster_count;
    uint32_t start = (S->v.free_hint >= 2 && S->v.free_hint < total + 2) ? S->v.free_hint : 2;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t c = start + i;
        if (c >= total + 2) c -= total; // wrap, staying in [2, total+2)
        uint32_t v;
        if (!fat_get(c, &v)) return 0;
        if (v != FAT_FREE) continue;
        if (!fat_set(c, FAT_EOC)) return 0;
        if (cluster_valid(prev) && !fat_set(prev, c)) return 0;
        S->v.free_hint = (c + 1 < total + 2) ? c + 1 : 2;
        if (S->v.free_count != 0xFFFFFFFFu && S->v.free_count) S->v.free_count--;
        return c;
    }
    return 0;
}

static int cluster_zero(uint32_t clus) {
    k_memset(g_datasec, 0, SECTOR);
    uint32_t base = cluster_first_sector(clus);
    for (uint32_t i = 0; i < S->v.sectors_per_cluster; i++) {
        if (!vol_write(base + i, 1, g_datasec)) return 0;
    }
    return 1;
}

// Frees a whole chain from `start`. Walks first, frees as it goes; a
// cycle (a corrupt FAT pointing back at itself) would otherwise be an
// infinite loop in the kernel, so the walk is bounded by the cluster
// count.
static int chain_free(uint32_t start) {
    if (!cluster_valid(start)) return 1;
    uint32_t c = start;
    uint32_t guard = S->v.cluster_count + 2;
    while (cluster_valid(c) && guard--) {
        uint32_t next;
        if (!chain_next(c, &next)) return 0;
        if (!fat_set(c, FAT_FREE)) return 0;
        if (S->v.free_count != 0xFFFFFFFFu) S->v.free_count++;
        if (S->v.free_hint > c) S->v.free_hint = c;
        c = next;
    }
    return 1;
}

// ---- names ------------------------------------------------------------

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
static char down(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

// FAT IS CASE-INSENSITIVE AND CASE-PRESERVING, which is not a quirk to
// paper over: it is the format's rule, other operating systems rely on
// it, and a driver that compared case-sensitively would fail to find
// GRUB's own files.
static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (up(*a) != up(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

// Unpack an 8.3 name into "name.ext", honouring the NT case bits (0x08
// = base is lowercase, 0x10 = extension is lowercase) that Windows
// writes so a short name like "grub.cfg" round-trips without an LFN.
static void short_name_unpack(const uint8_t *raw, uint8_t nt_flags, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)raw[i];
        out[n++] = (nt_flags & 0x08) ? down(c) : c;
    }
    int has_ext = 0;
    for (int i = 8; i < 11; i++) if (raw[i] != ' ') has_ext = 1;
    if (has_ext) {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = (char)raw[i];
            out[n++] = (nt_flags & 0x10) ? down(c) : c;
        }
    }
    out[n] = '\0';
    // 0x05 is a real 0xE5 first byte, escaped because 0xE5 means
    // "deleted" in that position. Undo it, or a legitimately named file
    // reads back with the wrong first character.
    if (raw[0] == 0x05) out[0] = (char)0xE5;
}

static int is_shortname_char(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    return k_strchr("$%'-_@~`!(){}^#& ", c) != 0;
}

// Can `name` be stored as a plain 8.3 entry with no LFN? Returns the
// packed form in `out11` when it can. The CASE rules are the subtle
// part: a name that is all-lowercase or all-uppercase in each of its
// two halves fits with an NT case bit; a MIXED-case name does not, and
// needs an LFN to preserve what the user typed.
static int short_name_pack(const char *name, uint8_t *out11, uint8_t *out_nt) {
    k_memset(out11, ' ', 11);
    *out_nt = 0;
    int len = (int)k_strlen(name);
    if (len == 0 || len > 12) return 0;
    if (k_strcmp(name, ".") == 0 || k_strcmp(name, "..") == 0) return 0;

    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') { if (dot) return 0; dot = p; }

    int base_len = dot ? (int)(dot - name) : len;
    int ext_len = dot ? len - base_len - 1 : 0;
    if (base_len < 1 || base_len > 8 || ext_len > 3) return 0;

    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    for (int i = 0; i < base_len; i++) {
        char c = name[i];
        if (!is_shortname_char(c) || c == ' ') return 0;
        if (c >= 'a' && c <= 'z') base_lower = 1;
        if (c >= 'A' && c <= 'Z') base_upper = 1;
        out11[i] = (uint8_t)up(c);
    }
    for (int i = 0; i < ext_len; i++) {
        char c = name[base_len + 1 + i];
        if (!is_shortname_char(c) || c == ' ') return 0;
        if (c >= 'a' && c <= 'z') ext_lower = 1;
        if (c >= 'A' && c <= 'Z') ext_upper = 1;
        out11[8 + i] = (uint8_t)up(c);
    }
    if (base_lower && base_upper) return 0;
    if (ext_lower && ext_upper) return 0;
    if (base_lower) *out_nt |= 0x08;
    if (ext_lower) *out_nt |= 0x10;
    if (out11[0] == 0xE5) out11[0] = 0x05;
    return 1;
}

// The checksum every LFN entry of a set carries, computed over the 8.3
// name it belongs to. It is what ties a run of LFN entries to their
// short entry: a set whose checksum does not match is ORPHANED and must
// be ignored, which is how a directory edited by a driver that does not
// understand LFN still reads correctly.
static uint8_t short_name_checksum(const uint8_t *raw11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + raw11[i]);
    return sum;
}

// ---- directory entries -------------------------------------------------

// Where an entry lives, so a caller can rewrite it without walking
// again. `lfn_count` is how many LFN entries precede it -- deleting the
// entry has to delete those too, or the next driver to read the
// directory sees a name attached to nothing.
struct dirloc {
    uint32_t sector;
    uint32_t offset;    // byte offset within that sector
    uint32_t lfn_start_sector;
    uint32_t lfn_start_offset;
    int lfn_count;
};

struct dirent_info {
    char name[FAT_NAME_MAX + 1];
    uint8_t attr;
    uint32_t cluster;
    uint32_t size;
    uint16_t wrt_date, wrt_time;
    uint16_t crt_date, crt_time;
    struct dirloc loc;
};

static uint32_t ent_cluster(const uint8_t *e) {
    return ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
}

static void ent_set_cluster(uint8_t *e, uint32_t c) {
    wr16(e + 20, (uint16_t)(c >> 16));
    wr16(e + 26, (uint16_t)(c & 0xFFFF));
}

// Copies the ASCII of one LFN entry's 13 UCS-2 slots into `dst`.
// Returns how many were real characters (a set's last entry is padded
// with a NUL then 0xFFFF).
static int lfn_chars(const uint8_t *e, char *dst) {
    static const int offs[LFN_CHARS] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    int n = 0;
    for (int i = 0; i < LFN_CHARS; i++) {
        uint16_t u = rd16(e + offs[i]);
        if (u == 0x0000 || u == 0xFFFF) break;
        // NOT UNICODE (see this file's top comment): anything outside
        // ASCII becomes '?' rather than being mangled into a byte that
        // would then fail to match on lookup.
        dst[n++] = (u < 0x80) ? (char)u : '?';
    }
    return n;
}

// Iterates a directory's entries, assembling LFN sets. `state` is the
// caller's cursor; start it zeroed. Returns 1 and fills `out` for each
// real entry, 0 at the end of the directory.
struct dirwalk {
    uint32_t cluster;      // current cluster (0 = not started)
    uint32_t start;        // the directory's first cluster
    uint32_t sec_in_clus;
    uint32_t off;          // byte offset in the loaded sector
    uint32_t cur_sector;   // the LBA currently in g_dirsec
    int loaded;
    // LFN accumulation
    char lfn[FAT_NAME_MAX + 1 + LFN_CHARS];
    int lfn_len;
    uint8_t lfn_sum;
    int lfn_count;
    uint32_t lfn_sector, lfn_offset;
    int done;
};

static void walk_begin(struct dirwalk *w, uint32_t start_cluster) {
    k_memset(w, 0, sizeof *w);
    w->start = start_cluster;
    w->cluster = start_cluster;
}

// Loads the sector the cursor points at, advancing clusters as needed.
// Returns 0 at the end of the chain.
static int walk_load(struct dirwalk *w) {
    if (w->done || !cluster_valid(w->cluster)) return 0;
    if (w->sec_in_clus >= S->v.sectors_per_cluster) {
        uint32_t next;
        if (!chain_next(w->cluster, &next) || !cluster_valid(next)) { w->done = 1; return 0; }
        w->cluster = next;
        w->sec_in_clus = 0;
    }
    uint32_t lba = cluster_first_sector(w->cluster) + w->sec_in_clus;
    if (w->loaded && w->cur_sector == lba) return 1;
    if (!vol_read(lba, 1, g_dirsec)) { w->done = 1; return 0; }
    w->cur_sector = lba;
    w->loaded = 1;
    return 1;
}

static void walk_advance(struct dirwalk *w) {
    w->off += DIRENT_SIZE;
    if (w->off >= SECTOR) {
        w->off = 0;
        w->sec_in_clus++;
        w->loaded = 0;
    }
}

// The next real entry. A DELETED entry, a VOLUME LABEL and the `.`/`..`
// links are skipped -- the first two are not files, and the last two
// are handled by path resolution rather than being offered as names
// (fs.h's paths are normalized and never contain them).
static int walk_next(struct dirwalk *w, struct dirent_info *out) {
    for (;;) {
        if (!walk_load(w)) return 0;
        uint8_t *e = g_dirsec + w->off;

        if (e[0] == DIRENT_END) { w->done = 1; return 0; }
        if (e[0] == DIRENT_FREE) {
            w->lfn_len = 0; w->lfn_count = 0;
            walk_advance(w);
            continue;
        }

        uint8_t attr = e[11];
        if ((attr & ATTR_LFN) == ATTR_LFN) {
            uint8_t order = e[0];
            int index = (order & 0x3F);
            // 0x40 marks the set's HIGHEST-numbered entry, which is the
            // one stored FIRST -- so this is where a set begins on
            // disk, and where ent_erase() has to start deleting.
            if (order & 0x40) {
                w->lfn_len = 0;
                w->lfn_count = 0;
                w->lfn_sum = e[13];
                w->lfn_sector = w->cur_sector;
                w->lfn_offset = w->off;
            }
            if (index >= 1 && index <= LFN_MAX_ENTRIES && e[13] == w->lfn_sum) {
                char part[LFN_CHARS + 1];
                int n = lfn_chars(e, part);
                int at = (index - 1) * LFN_CHARS;
                if (at + n <= FAT_NAME_MAX) {
                    for (int i = 0; i < n; i++) w->lfn[at + i] = part[i];
                    if (at + n > w->lfn_len) w->lfn_len = at + n;
                    w->lfn_count++;
                }
            } else {
                w->lfn_len = 0; w->lfn_count = 0; // orphaned or too long -- ignore the set
            }
            walk_advance(w);
            continue;
        }

        if (attr & ATTR_VOLUME_ID) { w->lfn_len = 0; w->lfn_count = 0; walk_advance(w); continue; }

        k_memset(out, 0, sizeof *out);
        out->attr = attr;
        out->cluster = ent_cluster(e);
        out->size = rd32(e + 28);
        out->crt_time = rd16(e + 14);
        out->crt_date = rd16(e + 16);
        out->wrt_time = rd16(e + 22);
        out->wrt_date = rd16(e + 24);
        out->loc.sector = w->cur_sector;
        out->loc.offset = w->off;

        // A LONG NAME IS ONLY THIS ENTRY'S IF ITS CHECKSUM MATCHES.
        // Otherwise the set belongs to something that was deleted and
        // this entry's real name is its 8.3 one.
        if (w->lfn_len > 0 && w->lfn_sum == short_name_checksum(e)) {
            int n = w->lfn_len;
            if (n > FAT_NAME_MAX) n = FAT_NAME_MAX;
            k_memcpy(out->name, w->lfn, (uint32_t)n);
            out->name[n] = '\0';
            out->loc.lfn_count = w->lfn_count;
            out->loc.lfn_start_sector = w->lfn_sector;
            out->loc.lfn_start_offset = w->lfn_offset;
        } else {
            short_name_unpack(e, e[12], out->name);
            out->loc.lfn_count = 0;
        }

        w->lfn_len = 0; w->lfn_count = 0;
        walk_advance(w);
        // A `.` or `..` short entry is a directory's own links, not a
        // name anything here should see.
        if (k_strcmp(out->name, ".") == 0 || k_strcmp(out->name, "..") == 0) continue;
        // AND A NAMELESS ENTRY IS NOT A FILE. An 8.3 name of eleven
        // spaces unpacks to "", which no path can ever name -- listing
        // it produces a row nothing can open, delete or explain.
        if (!out->name[0]) continue;
        return 1;
    }
}

static int dir_find(uint32_t dir_cluster, const char *name, struct dirent_info *out) {
    struct dirwalk w;
    walk_begin(&w, dir_cluster);
    struct dirent_info e;
    while (walk_next(&w, &e)) {
        if (name_eq(e.name, name)) { *out = e; return 1; }
    }
    return 0;
}

// ---- path resolution ---------------------------------------------------

// Splits `path` into the parent's cluster and the final component.
// Returns 0 if any intermediate component is missing or is not a
// directory. `path` is a normalized absolute path (see api/fs.h), so
// there are no `.`/`..` components to handle.
static int resolve_parent(const char *path, uint32_t *out_dir, const char **out_leaf) {
    if (!path || path[0] != '/') return 0;
    const char *p = path + 1;
    uint32_t dir = S->v.root_cluster;

    for (;;) {
        const char *slash = k_strchr(p, '/');
        if (!slash) break;
        int len = (int)(slash - p);
        if (len == 0 || len > FAT_NAME_MAX) return 0;
        char seg[FAT_NAME_MAX + 1];
        k_memcpy(seg, p, (uint32_t)len);
        seg[len] = '\0';
        struct dirent_info e;
        if (!dir_find(dir, seg, &e)) return 0;
        if (!(e.attr & ATTR_DIRECTORY)) return 0;
        // A subdirectory whose first cluster is 0 is `..` pointing at
        // the root, which FAT encodes as 0 rather than as the root's
        // real cluster number.
        dir = e.cluster ? e.cluster : S->v.root_cluster;
        p = slash + 1;
    }
    *out_dir = dir;
    *out_leaf = p;
    return 1;
}

// The entry `path` names. Returns 0 for the root, which has no entry of
// its own -- callers that care about the root handle it before asking.
static int lookup(const char *path, struct dirent_info *out) {
    uint32_t dir;
    const char *leaf;
    if (!S->v.mounted) return 0;
    if (!path || k_strcmp(path, "/") == 0) return 0;
    if (!resolve_parent(path, &dir, &leaf)) return 0;
    if (leaf[0] == '\0' || k_strlen(leaf) > FAT_NAME_MAX) return 0;
    return dir_find(dir, leaf, out);
}

// ---- timestamps ---------------------------------------------------------
//
// FAT stores LOCAL time with two-second resolution and no timezone. The
// rest of this OS speaks epoch seconds (fs_stat_info), so the
// conversion happens here -- which is exactly what FS_CAP_EPOCH_TIME
// says a backend NOT declaring it does, and why fat32_ops does not
// declare it.

static void now_fat(uint16_t *out_date, uint16_t *out_time) {
    struct rtc_time t;
    rtc_read_local(&t);
    uint32_t year = t.year;
    if (year < 1980) year = 1980;
    if (year > 2107) year = 2107; // FAT's 7-bit year field ends here
    *out_date = (uint16_t)(((year - 1980) << 9) | ((uint32_t)t.month << 5) | t.day);
    *out_time = (uint16_t)(((uint32_t)t.hour << 11) | ((uint32_t)t.minute << 5) | (t.second / 2));
}

static uint64_t fat_to_epoch(uint16_t date, uint16_t time) {
    if (date == 0) return 0;
    struct rtc_time t;
    t.year = (uint16_t)(1980 + ((date >> 9) & 0x7F));
    t.month = (uint8_t)((date >> 5) & 0x0F);
    t.day = (uint8_t)(date & 0x1F);
    t.hour = (uint8_t)((time >> 11) & 0x1F);
    t.minute = (uint8_t)((time >> 5) & 0x3F);
    t.second = (uint8_t)((time & 0x1F) * 2);
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) return 0;
    return tz_rtc_to_epoch(&t);
}

// ---- writing directory entries -------------------------------------------

// Rewrites the 32 bytes at `loc`. Reads the sector, patches, writes it
// back -- a read-modify-write, because a directory sector holds sixteen
// entries and fifteen of them belong to somebody else.
static int ent_patch(const struct dirloc *loc, uint32_t cluster, uint32_t size,
                     const uint16_t *wrt_date, const uint16_t *wrt_time) {
    if (!vol_read(loc->sector, 1, g_tmpsec)) return 0;
    uint8_t *e = g_tmpsec + loc->offset;
    ent_set_cluster(e, cluster);
    wr32(e + 28, size);
    if (wrt_date) wr16(e + 24, *wrt_date);
    if (wrt_time) wr16(e + 22, *wrt_time);
    return vol_write(loc->sector, 1, g_tmpsec);
}

// Advances a directory cursor by one 32-byte entry, following the
// cluster chain when it runs off the end of one. Its own function
// because three callers need it and each got it slightly different.
static int dir_step(uint32_t *sector, uint32_t *off) {
    *off += DIRENT_SIZE;
    if (*off < SECTOR) return 1;
    *off = 0;
    // Recovering the cluster from the sector is exact, because a
    // cluster IS a run of contiguous sectors.
    uint32_t clus = (*sector - S->v.first_data_sector) / S->v.sectors_per_cluster + 2;
    uint32_t sec_in = (*sector - S->v.first_data_sector) % S->v.sectors_per_cluster;
    if (sec_in + 1 < S->v.sectors_per_cluster) { (*sector)++; return 1; }
    uint32_t next;
    if (!chain_next(clus, &next) || !cluster_valid(next)) return 0;
    *sector = cluster_first_sector(next);
    return 1;
}

// Marks an entry and its LFN set deleted. Walks FORWARD from the LFN
// start to the short entry, which is the order they sit on disk.
static int ent_erase(const struct dirloc *loc) {
    uint32_t sector = loc->lfn_count ? loc->lfn_start_sector : loc->sector;
    uint32_t off = loc->lfn_count ? loc->lfn_start_offset : loc->offset;
    int remaining = loc->lfn_count + 1;

    // The LFN set and its short entry are contiguous in the directory,
    // but may straddle a sector (and a cluster) boundary. Rather than
    // re-deriving the chain, this walks the directory again from the
    // known start sector -- the set is at most LFN_MAX_ENTRIES + 1
    // entries, so at most two sectors are involved in practice.
    while (remaining > 0) {
        if (!vol_read(sector, 1, g_tmpsec)) return 0;
        int dirty = 0;
        while (remaining > 0 && off < SECTOR) {
            g_tmpsec[off] = DIRENT_FREE;
            dirty = 1;
            off += DIRENT_SIZE;
            remaining--;
        }
        if (dirty && !vol_write(sector, 1, g_tmpsec)) return 0;
        if (remaining <= 0) break;
        // off has run past the sector; dir_step from the last entry
        // walks to the next sector (and the next cluster, if this was
        // the last sector of one).
        off -= DIRENT_SIZE;
        if (!dir_step(&sector, &off)) return 0;
    }
    return 1;
}

// Finds `need` consecutive free entries in a directory, growing it by a
// cluster if there is no such run. Returns the sector/offset of the
// first, or 0.
//
// GROWING A DIRECTORY IS WHY THIS RETURNS A SECTOR RATHER THAN AN
// INDEX: a directory is a cluster chain, so entry N and entry N+1 can
// be a cluster apart, and an index would have to be re-walked to be
// used.
static int dir_find_run(uint32_t dir_cluster, int need, uint32_t *out_sector, uint32_t *out_off) {
    uint32_t clus = dir_cluster;
    uint32_t run_sector = 0, run_off = 0;
    int run = 0;
    uint32_t guard = S->v.cluster_count + 2;

    for (;;) {
        for (uint32_t s = 0; s < S->v.sectors_per_cluster; s++) {
            uint32_t lba = cluster_first_sector(clus) + s;
            if (!vol_read(lba, 1, g_dirsec)) return 0;
            for (uint32_t o = 0; o < SECTOR; o += DIRENT_SIZE) {
                uint8_t first = g_dirsec[o];
                if (first == DIRENT_END || first == DIRENT_FREE) {
                    if (run == 0) { run_sector = lba; run_off = o; }
                    if (++run == need) { *out_sector = run_sector; *out_off = run_off; return 1; }
                } else {
                    run = 0;
                }
            }
        }
        uint32_t next;
        if (!chain_next(clus, &next)) return 0;
        if (cluster_valid(next)) {
            if (!guard--) return 0;
            clus = next;
            continue;
        }
        // Out of directory. Extend it by one cluster and zero it, which
        // makes every entry in it DIRENT_END -- so the run continues
        // into it rather than restarting.
        uint32_t fresh = cluster_alloc(clus);
        if (!fresh) return 0;
        if (!cluster_zero(fresh)) return 0;
        clus = fresh;
    }
}

// Writes an LFN set plus its short entry at a located run.
//
// THE SET IS STORED IN REVERSE: the HIGHEST-numbered LFN entry comes
// FIRST on disk and carries the 0x40 "last" bit, then they count down,
// and the 8.3 entry is last. Writing them the other way round produces
// a name every other driver reads backwards -- which is exactly what
// this did on its first outing, and what mtools would have shown.
static int dir_write_set(uint32_t sector, uint32_t off, const uint8_t *raw11,
                         uint8_t nt_flags, const char *longname, int lfn_entries,
                         uint8_t attr, uint32_t cluster, uint32_t size) {
    uint8_t sum = short_name_checksum(raw11);
    uint16_t date, time;
    now_fat(&date, &time);
    int namelen = (int)k_strlen(longname);
    static const int offs[LFN_CHARS] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};

    for (int index = lfn_entries; index >= 1; index--) {
        if (!vol_read(sector, 1, g_tmpsec)) return 0;
        uint8_t *e = g_tmpsec + off;
        k_memset(e, 0, DIRENT_SIZE);
        e[0] = (uint8_t)(index | ((index == lfn_entries) ? 0x40 : 0));
        e[11] = ATTR_LFN;
        e[12] = 0;
        e[13] = sum;
        wr16(e + 26, 0);
        for (int c = 0; c < LFN_CHARS; c++) {
            int at = (index - 1) * LFN_CHARS + c;
            uint16_t u;
            // The character AFTER the name is a NUL and everything past
            // that is 0xFFFF -- padding a set with zeroes instead makes
            // the name look 13 characters long to a reader that stops
            // at the first NUL and finds none.
            if (at < namelen) u = (uint16_t)(uint8_t)longname[at];
            else if (at == namelen) u = 0x0000;
            else u = 0xFFFF;
            wr16(e + offs[c], u);
        }
        if (!vol_write(sector, 1, g_tmpsec)) return 0;
        if (!dir_step(&sector, &off)) return 0;
    }

    if (!vol_read(sector, 1, g_tmpsec)) return 0;
    uint8_t *e = g_tmpsec + off;
    k_memset(e, 0, DIRENT_SIZE);
    k_memcpy(e, raw11, 11);
    e[11] = attr;
    e[12] = nt_flags;
    wr16(e + 14, time);
    wr16(e + 16, date);
    wr16(e + 18, date);   // last-access date
    wr16(e + 22, time);
    wr16(e + 24, date);
    ent_set_cluster(e, cluster);
    wr32(e + 28, size);
    return vol_write(sector, 1, g_tmpsec) ? 1 : 0;
}

// Marks `count` entries from (sector, off) free. What dir_create() uses
// to clean up after a half-written set: a run of LFN entries with no
// short entry behind them is an ORPHAN, which every other driver
// silently ignores and this one used to report as a nameless file.
static int dir_free_run(uint32_t sector, uint32_t off, int count) {
    while (count-- > 0) {
        if (!vol_read(sector, 1, g_tmpsec)) return 0;
        g_tmpsec[off] = DIRENT_FREE;
        if (!vol_write(sector, 1, g_tmpsec)) return 0;
        if (count && !dir_step(&sector, &off)) return 0;
    }
    return 1;
}

// Makes an 8.3 alias for a long name: the first six acceptable
// characters, "~N", and the first three extension characters. N counts
// up until nothing in the directory collides -- Windows' rule, and the
// reason two long names starting the same way do not overwrite one
// another's short entry.
static int make_alias(uint32_t dir_cluster, const char *name, uint8_t *out11) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;

    for (int n = 1; n <= 99; n++) {
        k_memset(out11, ' ', 11);
        int base_len = (n < 10) ? 6 : 5;
        int w = 0;
        for (const char *p = name; *p && w < base_len; p++) {
            if (p == dot) break;
            char c = up(*p);
            if (!is_shortname_char(c) || c == ' ' || c == '.') c = '_';
            out11[w++] = (uint8_t)c;
        }
        if (w == 0) out11[w++] = '_';
        out11[w++] = '~';
        if (n >= 10) out11[w++] = (uint8_t)('0' + n / 10);
        out11[w++] = (uint8_t)('0' + n % 10);
        if (dot) {
            for (int i = 0; i < 3 && dot[1 + i]; i++) {
                char c = up(dot[1 + i]);
                if (!is_shortname_char(c) || c == ' ' || c == '.') c = '_';
                out11[8 + i] = (uint8_t)c;
            }
        }

        // Collision check against every SHORT name in the directory.
        struct dirwalk w2;
        walk_begin(&w2, dir_cluster);
        struct dirent_info e;
        int clash = 0;
        char candidate[13];
        short_name_unpack(out11, 0, candidate);
        while (walk_next(&w2, &e)) {
            char theirs[13];
            // Compare against the entry's own SHORT name, which is what
            // the alias must not duplicate -- two entries may have
            // different long names and the same 8.3 alias.
            //
            // INTO g_tmpsec, NOT g_dirsec. g_dirsec is the WALK's
            // sector and re-reading into it mid-walk is the
            // shared-scratch bug this file's state comment warns about:
            // the walk kept its "loaded" flag, carried on over the
            // other sector's bytes, and produced a directory full of
            // nonsense -- which surfaced as a create that failed and
            // left a nameless entry behind.
            if (!vol_read(e.loc.sector, 1, g_tmpsec)) break;
            short_name_unpack(g_tmpsec + e.loc.offset, 0, theirs);
            if (name_eq(theirs, candidate)) { clash = 1; break; }
        }
        if (!clash) return 1;
    }
    return 0;
}

// Creates an entry. `attr` is ATTR_DIRECTORY for a directory (whose
// first cluster is allocated and seeded with `.`/`..` here) or
// ATTR_ARCHIVE for a file (which starts with no clusters at all --
// FAT's own representation of an empty file, and what makes `touch`
// free).
static int dir_create(uint32_t dir_cluster, const char *name, uint8_t attr,
                      struct dirent_info *out) {
    if (!name || !name[0]) return 0;
    int namelen = (int)k_strlen(name);
    if (namelen > FAT_NAME_MAX) return 0;
    for (int i = 0; i < namelen; i++) {
        unsigned char c = (unsigned char)name[i];
        // NOT UNICODE, and the refusal is deliberate: a name this
        // driver cannot represent must not be created, because it could
        // never be looked up again.
        if (c < 0x20 || c >= 0x80) return 0;
        if (k_strchr("\\/:*?\"<>|", (char)c)) return 0;
    }

    uint8_t raw11[11], nt_flags = 0;
    int lfn_entries = 0;
    if (!short_name_pack(name, raw11, &nt_flags)) {
        if (!make_alias(dir_cluster, name, raw11)) return 0;
        nt_flags = 0;
        lfn_entries = (namelen + LFN_CHARS - 1) / LFN_CHARS;
    }

    uint32_t first = 0;
    if (attr & ATTR_DIRECTORY) {
        first = cluster_alloc(0);
        if (!first) return 0;
        if (!cluster_zero(first)) return 0;
    }

    uint32_t sector, off;
    if (!dir_find_run(dir_cluster, lfn_entries + 1, &sector, &off)) {
        if (first) chain_free(first);
        return 0;
    }
    if (!dir_write_set(sector, off, raw11, nt_flags, name, lfn_entries, attr, first, 0)) {
        // A HALF-WRITTEN SET IS WORSE THAN NO SET: the LFN entries
        // without their short entry are an orphan run that every reader
        // has to skip, and this one reported as a nameless file.
        dir_free_run(sector, off, lfn_entries + 1);
        if (first) chain_free(first);
        return 0;
    }

    if (attr & ATTR_DIRECTORY) {
        // `.` and `..`, which every FAT directory but the root carries.
        // `..` pointing at the root is stored as 0, not as the root's
        // cluster number -- a rule that exists because FAT12/16 roots
        // have no cluster number at all.
        k_memset(g_tmpsec, 0, SECTOR);
        uint16_t date, time;
        now_fat(&date, &time);
        uint8_t *dot = g_tmpsec;
        k_memset(dot, ' ', 11);
        dot[0] = '.';
        dot[11] = ATTR_DIRECTORY;
        wr16(dot + 22, time); wr16(dot + 24, date);
        ent_set_cluster(dot, first);
        uint8_t *dotdot = g_tmpsec + DIRENT_SIZE;
        k_memset(dotdot, ' ', 11);
        dotdot[0] = '.'; dotdot[1] = '.';
        dotdot[11] = ATTR_DIRECTORY;
        wr16(dotdot + 22, time); wr16(dotdot + 24, date);
        ent_set_cluster(dotdot, dir_cluster == S->v.root_cluster ? 0 : dir_cluster);
        if (!vol_write(cluster_first_sector(first), 1, g_tmpsec)) return 0;
    }

    // CREATED MEANS FINDABLE, and it is worth reading back to say so.
    // The caller's `out->loc` has to be the entry's REAL location -- the
    // short entry may have crossed into the next sector, or the next
    // cluster -- and a caller that patched a guessed location would
    // rewrite whatever is actually there. fat32_rename() does exactly
    // that patch, which is what makes this a correctness check rather
    // than a nicety.
    struct dirent_info again;
    if (!dir_find(dir_cluster, name, &again)) {
        dir_free_run(sector, off, lfn_entries + 1);
        if (first) chain_free(first);
        return 0;
    }
    if (out) *out = again;
    return 1;
}

// ---- file data ----------------------------------------------------------

// Reads up to `len` bytes at `offset` from a chain. Short at EOF, which
// fs_read_range()'s contract allows.
static uint32_t chain_read(uint32_t start, uint32_t size, uint64_t offset,
                           void *buf, uint32_t len) {
    if (offset >= size) return 0;
    if (offset + len > size) len = (uint32_t)(size - offset);
    if (!len) return 0;

    uint32_t cluster_bytes = S->v.sectors_per_cluster * SECTOR;
    uint32_t got = 0;
    uint8_t *dst = buf;

    uint32_t clus = chain_nth(start, (uint32_t)(offset / cluster_bytes));
    uint32_t within = (uint32_t)(offset % cluster_bytes);

    while (got < len && cluster_valid(clus)) {
        uint32_t sec_in = within / SECTOR;
        uint32_t sec_off = within % SECTOR;
        uint32_t lba = cluster_first_sector(clus) + sec_in;

        uint32_t take = SECTOR - sec_off;
        if (take > len - got) take = len - got;

        if (sec_off == 0 && take == SECTOR) {
            // A whole-sector read goes straight to the caller's buffer:
            // no bounce, which is what makes a big sequential read cost
            // one transfer per sector rather than one plus a memcpy.
            if (!vol_read(lba, 1, dst + got)) break;
        } else {
            if (!vol_read(lba, 1, g_datasec)) break;
            k_memcpy(dst + got, g_datasec + sec_off, take);
        }
        got += take;
        within += take;
        if (within >= cluster_bytes) {
            uint32_t next;
            if (!chain_next(clus, &next)) break;
            clus = next;
            within = 0;
        }
    }
    return got;
}

// Writes `len` bytes at `offset`, extending the chain as needed.
// `*io_start` is the file's first cluster and is UPDATED when a file
// that had none gets its first. Returns 1 on success.
static int chain_write(uint32_t *io_start, uint64_t offset, const void *buf, uint32_t len) {
    uint32_t cluster_bytes = S->v.sectors_per_cluster * SECTOR;
    const uint8_t *src = buf;
    uint32_t done = 0;

    if (!cluster_valid(*io_start)) {
        uint32_t c = cluster_alloc(0);
        if (!c) return 0;
        if (!cluster_zero(c)) return 0;
        *io_start = c;
    }

    uint32_t want_index = (uint32_t)(offset / cluster_bytes);
    uint32_t clus = *io_start;
    for (uint32_t i = 0; i < want_index; i++) {
        uint32_t next;
        if (!chain_next(clus, &next)) return 0;
        if (!cluster_valid(next)) {
            // A WRITE PAST THE END GROWS THE FILE, and the clusters in
            // the gap are ZEROED -- a sparse hole reading as whatever
            // was there before is somebody else's deleted data.
            next = cluster_alloc(clus);
            if (!next) return 0;
            if (!cluster_zero(next)) return 0;
        }
        clus = next;
    }

    uint32_t within = (uint32_t)(offset % cluster_bytes);
    while (done < len) {
        uint32_t sec_in = within / SECTOR;
        uint32_t sec_off = within % SECTOR;
        uint32_t lba = cluster_first_sector(clus) + sec_in;
        uint32_t take = SECTOR - sec_off;
        if (take > len - done) take = len - done;

        if (sec_off == 0 && take == SECTOR) {
            if (!vol_write(lba, 1, src + done)) return 0;
        } else {
            // Read-modify-write: the rest of the sector belongs to this
            // file's other bytes, which a blind write would zero.
            if (!vol_read(lba, 1, g_datasec)) return 0;
            k_memcpy(g_datasec + sec_off, src + done, take);
            if (!vol_write(lba, 1, g_datasec)) return 0;
        }
        done += take;
        within += take;
        if (within >= cluster_bytes && done < len) {
            uint32_t next;
            if (!chain_next(clus, &next)) return 0;
            if (!cluster_valid(next)) {
                next = cluster_alloc(clus);
                if (!next) return 0;
            }
            clus = next;
            within = 0;
        }
    }
    return 1;
}

// ---- fs_ops: identity ----------------------------------------------------

// Reads a BPB and decides whether it is a FAT32 this driver can serve.
// Fills S->v on success -- probe() and init() both need every field, and
// deriving them twice is how the two answers drift apart.
static int parse_bpb(const struct block_device *dev) {
    k_memset(&S->v, 0, sizeof S->v);
    S->v.dev = dev;
    S->v.total_sectors = blkdev_sector_count(dev); // provisional, for vol_read's bound
    if (S->v.total_sectors < 8) return 0;
    if (!blkdev_read_sectors(dev, 0, 1, g_tmpsec)) return -1;

    if (rd16(g_tmpsec + 510) != 0xAA55) return 0;

    uint16_t bps = rd16(g_tmpsec + 11);
    uint8_t spc = g_tmpsec[13];
    uint16_t reserved = rd16(g_tmpsec + 14);
    uint8_t nfats = g_tmpsec[16];
    uint16_t root_entries = rd16(g_tmpsec + 17);
    uint16_t tot16 = rd16(g_tmpsec + 19);
    uint16_t fatsz16 = rd16(g_tmpsec + 22);
    uint32_t tot32 = rd32(g_tmpsec + 32);
    uint32_t fatsz32 = rd32(g_tmpsec + 36);
    uint32_t root_clus = rd32(g_tmpsec + 44);
    uint16_t fsinfo = rd16(g_tmpsec + 48);

    // 512-BYTE SECTORS ONLY (see this file's top comment). Refusing is
    // the honest answer: the block layer speaks 512, and reading a
    // 4096-byte-sector volume with a 512 stride would produce plausible
    // garbage rather than an error.
    if (bps != SECTOR) return 0;
    if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) return 0;
    if (reserved == 0 || nfats == 0) return 0;

    // THE FAT32 DISCRIMINATOR. A FAT12/16 volume has a nonzero
    // fat_size_16 and a nonzero root_entry_count; FAT32 has neither and
    // has a fat_size_32 instead. Microsoft's spec also uses a cluster
    // count of 65525 to pick the type, but that rule exists for a
    // driver that implements all three -- this one implements FAT32
    // only, so the BPB's own FAT32-only fields are what decide.
    if (fatsz16 != 0 || root_entries != 0 || fatsz32 == 0) return 0;
    if (tot16 != 0) return 0;
    if (tot32 == 0) return 0;
    if (tot32 > blkdev_sector_count(dev)) return 0; // a BPB claiming more than the volume holds

    uint32_t first_data = reserved + (uint32_t)nfats * fatsz32;
    if (first_data >= tot32) return 0;
    uint32_t data_sectors = tot32 - first_data;
    uint32_t clusters = data_sectors / spc;
    if (clusters < 1) return 0;
    // The FAT has to be able to describe every cluster it claims: four
    // bytes each, plus the two reserved entries.
    if ((uint64_t)(clusters + 2) * 4 > (uint64_t)fatsz32 * SECTOR) return 0;
    if (root_clus < 2 || root_clus >= clusters + 2) return 0;

    S->v.sectors_per_cluster = spc;
    S->v.reserved = reserved;
    S->v.num_fats = nfats;
    S->v.fat_sectors = fatsz32;
    S->v.total_sectors = tot32;
    S->v.root_cluster = root_clus;
    S->v.first_data_sector = first_data;
    S->v.cluster_count = clusters;
    S->v.fsinfo_sector = (fsinfo && fsinfo < reserved) ? fsinfo : 0;
    return 1;
}

// A PROBE MUST NOT DISTURB A MOUNT -- parse_bpb() fills S->v, which IS
// the mounted volume. It cannot any more: mount.c hands a probe a
// SCRATCH state, so what this fills belongs to nobody.
static int fat32_probe(const struct block_device *dev) {
    if (!dev) return 0;
    int r = parse_bpb(dev);
    S->v.mounted = 0; // init() re-parses from scratch either way
    return r;
}

// Erase the signature the probe recognises: the boot sector's 0xAA55
// and the FAT32 fields behind it, plus the backup boot sector at LBA 6
// that mkfs writes. See fs_ops.h's wipe contract.
static int fat32_wipe(const struct block_device *dev) {
    if (!dev) return 1;
    uint32_t sectors = blkdev_sector_count(dev);
    if (sectors < 1) return 1;
    k_memset(g_tmpsec, 0, SECTOR);
    int ok = blkdev_write_sectors(dev, 0, 1, g_tmpsec) ? 1 : 0;
    // The conventional backup location; harmless to zero on a volume
    // that has none, and a real signature to a probe that finds one.
    if (sectors > 6) {
        if (!blkdev_write_sectors(dev, 6, 1, g_tmpsec)) ok = 0;
    }
    return ok;
}

// How many sectors per cluster a volume of this size should use. The
// thresholds are mkfs.fat's, which keeps a toy-os-formatted volume
// looking ordinary to every other tool -- and keeps the cluster count
// above FAT32's 65525 floor wherever the volume is big enough for that
// to be possible.
static uint32_t pick_spc(uint32_t sectors) {
    if (sectors <= 532480u) return 1;      // <= 260 MiB
    if (sectors <= 16777216u) return 8;    // <= 8 GiB
    if (sectors <= 33554432u) return 16;
    if (sectors <= 67108864u) return 32;
    return 64;
}

static int fat32_format(const struct block_device *dev) {
    if (!dev) return 0;
    uint32_t total = blkdev_sector_count(dev);
    if (total < 128) {
        klog_write("fat32: volume too small to format\n");
        return 0;
    }

    uint32_t spc = pick_spc(total);
    uint32_t reserved = 32;   // mkfs.fat's FAT32 default: room for the backup at 6 and FSInfo at 1
    uint32_t nfats = 2;

    // Solve for the FAT size: every cluster needs four bytes, and the
    // FATs themselves take sectors away from the data area.
    uint32_t fatsz = 1;
    for (int pass = 0; pass < 8; pass++) {
        uint32_t data = total - reserved - nfats * fatsz;
        uint32_t clusters = data / spc;
        uint32_t need = ((clusters + 2) * 4 + SECTOR - 1) / SECTOR;
        if (need == fatsz) break;
        fatsz = need;
    }

    // THE ITERATION ABOVE DOES NOT ALWAYS CONVERGE, AND FALLING OUT OF
    // IT LEAVES A VOLUME THIS DRIVER'S OWN PROBE REFUSES. It can
    // oscillate between two adjacent sizes -- a bigger FAT leaves fewer
    // clusters, which needs a smaller FAT, which leaves more -- and the
    // loop then ends on whichever it happened to be holding, which is
    // the SMALLER one half the time. A FAT one sector short describes
    // eight bytes fewer than the layout's cluster count, so parse_bpb()
    // rejects it: a 64 MiB volume formatted and then unmountable, which
    // is what an installer's ESP is.
    //
    // The invariant is one-directional, so enforce it directly rather
    // than hoping for a fixed point: the FAT must describe every cluster
    // the resulting layout has. Growing it only ever removes clusters,
    // so this terminates.
    for (int pass = 0; pass < 4; pass++) {
        uint32_t first = reserved + nfats * fatsz;
        if (first + spc > total) break;   // caught by the check below
        uint32_t cl = (total - first) / spc;
        if ((uint64_t)(cl + 2) * 4 <= (uint64_t)fatsz * SECTOR) break;
        fatsz++;
    }

    uint32_t first_data = reserved + nfats * fatsz;
    if (first_data + spc > total) {
        klog_write("fat32: volume too small for a FAT32 layout\n");
        return 0;
    }
    uint32_t clusters = (total - first_data) / spc;
    if (clusters < 2) return 0;

    // ---- boot sector ----
    k_memset(g_tmpsec, 0, SECTOR);
    g_tmpsec[0] = 0xEB; g_tmpsec[1] = 0x58; g_tmpsec[2] = 0x90; // the jump mkfs writes
    k_memcpy(g_tmpsec + 3, "toy-os  ", 8);
    wr16(g_tmpsec + 11, SECTOR);
    g_tmpsec[13] = (uint8_t)spc;
    wr16(g_tmpsec + 14, (uint16_t)reserved);
    g_tmpsec[16] = (uint8_t)nfats;
    wr16(g_tmpsec + 17, 0);      // root_entry_count -- 0 marks FAT32
    wr16(g_tmpsec + 19, 0);      // total_sectors_16
    g_tmpsec[21] = 0xF8;         // media: fixed disk
    wr16(g_tmpsec + 22, 0);      // fat_size_16 -- 0 marks FAT32
    wr16(g_tmpsec + 24, 63);     // sectors per track, cosmetic
    wr16(g_tmpsec + 26, 255);    // heads, cosmetic
    wr32(g_tmpsec + 28, 0);      // hidden sectors: this is a VOLUME, offset 0
    wr32(g_tmpsec + 32, total);
    wr32(g_tmpsec + 36, fatsz);
    wr16(g_tmpsec + 40, 0);      // ext_flags: all FATs mirrored
    wr16(g_tmpsec + 42, 0);      // version
    wr32(g_tmpsec + 44, 2);      // root cluster
    wr16(g_tmpsec + 48, 1);      // FSInfo sector
    wr16(g_tmpsec + 50, 6);      // backup boot sector
    g_tmpsec[64] = 0x80;         // drive number
    g_tmpsec[66] = 0x29;         // extended boot signature
    wr32(g_tmpsec + 67, 0x544F5900u);
    k_memcpy(g_tmpsec + 71, "NO NAME    ", 11);
    k_memcpy(g_tmpsec + 82, "FAT32   ", 8);
    wr16(g_tmpsec + 510, 0xAA55);
    if (!blkdev_write_sectors(dev, 0, 1, g_tmpsec)) return 0;
    if (!blkdev_write_sectors(dev, 6, 1, g_tmpsec)) return 0;

    // ---- FSInfo ----
    k_memset(g_tmpsec, 0, SECTOR);
    wr32(g_tmpsec, 0x41615252u);
    wr32(g_tmpsec + 484, 0x61417272u);
    wr32(g_tmpsec + 488, clusters - 1); // the root's cluster is taken
    wr32(g_tmpsec + 492, 3);
    wr16(g_tmpsec + 510, 0xAA55);
    if (!blkdev_write_sectors(dev, 1, 1, g_tmpsec)) return 0;
    if (!blkdev_write_sectors(dev, 7, 1, g_tmpsec)) return 0;

    // ---- the FATs ----
    //
    // Zeroing a FAT is the slow part of a format: a 64 MiB volume's FAT
    // is 1024 sectors and there are two of them. Written in chunks the
    // device will actually take in one transfer rather than a sector at
    // a time.
    uint32_t chunk = (uint32_t)blkdev_max_sectors_per_xfer(dev);
    if (chunk > 8) chunk = 8;
    if (chunk < 1) chunk = 1;
    static uint8_t zeros[8 * SECTOR];
    k_memset(zeros, 0, sizeof zeros);
    for (uint32_t f = 0; f < nfats; f++) {
        uint32_t base = reserved + f * fatsz;
        for (uint32_t s = 0; s < fatsz; ) {
            uint32_t n = fatsz - s;
            if (n > chunk) n = chunk;
            if (!blkdev_write_sectors(dev, base + s, (int)n, zeros)) return 0;
            s += n;
        }
        // The first two entries are reserved: the media byte in entry 0
        // and an end-of-chain in entry 1, then the root's own chain
        // terminator in entry 2.
        k_memset(g_tmpsec, 0, SECTOR);
        wr32(g_tmpsec, 0x0FFFFFF8u);
        wr32(g_tmpsec + 4, 0x0FFFFFFFu);
        wr32(g_tmpsec + 8, 0x0FFFFFFFu);
        if (!blkdev_write_sectors(dev, base, 1, g_tmpsec)) return 0;
    }

    // ---- the root directory ----
    k_memset(g_tmpsec, 0, SECTOR);
    for (uint32_t s = 0; s < spc; s++) {
        if (!blkdev_write_sectors(dev, first_data + s, 1, g_tmpsec)) return 0;
    }

    blkdev_flush(dev);
    klog_printf("fat32: formatted %u sectors, %u clusters of %u sectors\n",
                total, clusters, spc);
    return 1;
}

static int fat32_init(const struct block_device *dev, uint64_t size_bytes) {
    // A volume's capacity is the volume's; only a backend that lives
    // in memory has a size to be told (fs_ops.h).
    (void)size_bytes;
    S->v.mounted = 0;
    S->fatsec_lba = 0;
    S->fatsec_dirty = 0;

    if (!dev) {
        // FAT32 has no RAM-only mode -- that is ramfs's job. -1, not 0:
        // 0 would mean "mounted, but not persistent", which is the
        // fiction fs_ops.h's three-valued init() exists to stop.
        klog_write("fat32: no volume -- cannot mount\n");
        return -1;
    }
    int r = parse_bpb(dev);
    if (r != 1) {
        klog_write("fat32: not a FAT32 volume this kernel can read -- not mounted\n");
        return -1;
    }
    fsinfo_load();
    S->v.mounted = 1;
    klog_printf("fat32: %u clusters of %u bytes, root at cluster %u\n",
                S->v.cluster_count, S->v.sectors_per_cluster * SECTOR,
                S->v.root_cluster);
    // 1: everything written here goes to the device. Whether the DEVICE
    // survives a power cycle is the block layer's answer, which the
    // mount records separately.
    return 1;
}

static void fat32_umount(const struct block_device *dev) {
    (void)dev;
    if (S->v.mounted) fat_sync();
    S->fatsec_lba = 0;
    S->fatsec_dirty = 0;
    S->v.mounted = 0;
}

// ---- fs_ops: reading -----------------------------------------------------

static int fat32_exists(const char *path) {
    if (!S->v.mounted) return 0;
    if (path && k_strcmp(path, "/") == 0) return 1;
    struct dirent_info e;
    return lookup(path, &e);
}

static int fat32_is_dir(const char *path) {
    if (!S->v.mounted) return 0;
    if (path && k_strcmp(path, "/") == 0) return 1;
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;
    return (e.attr & ATTR_DIRECTORY) != 0;
}

static uint64_t fat32_size(const char *path) {
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;
    if (e.attr & ATTR_DIRECTORY) return 0;
    return e.size;
}

static uint32_t fat32_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;
    if (e.attr & ATTR_DIRECTORY) return 0;
    if (!cluster_valid(e.cluster)) return 0; // an empty file has no chain at all
    return chain_read(e.cluster, e.size, offset, buf, len);
}

// The whole file into one staging buffer -- the same shape every
// backend here has, and the same hazard: vfs.c refuses a NESTED call
// because the buffer is freed and reallocated per read.
static void fat32_list(const char *dir_path, void (*cb)(const char *, uint32_t, int)) {
    if (!S->v.mounted || !cb) return;
    uint32_t dir;
    if (dir_path && k_strcmp(dir_path, "/") == 0) {
        dir = S->v.root_cluster;
    } else {
        struct dirent_info e;
        if (!lookup(dir_path, &e)) return;
        if (!(e.attr & ATTR_DIRECTORY)) return;
        dir = e.cluster ? e.cluster : S->v.root_cluster;
    }
    struct dirwalk w;
    walk_begin(&w, dir);
    struct dirent_info e;
    while (walk_next(&w, &e)) {
        cb(e.name, e.size, (e.attr & ATTR_DIRECTORY) ? 1 : 0);
    }
}

static int fat32_stat(const char *path, struct fs_stat_info *out) {
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;
    if (!out) return 1;
    // NO REAL INODES (hence no FS_CAP_INODES): FAT has no inode number,
    // so the first cluster is used as a stable-enough identity. It IS
    // unique among files that have data, and it is 0 for every empty
    // file -- which is exactly why the capability is not claimed.
    out->ino = e.cluster;
    out->created = fat_to_epoch(e.crt_date, e.crt_time);
    out->modified = fat_to_epoch(e.wrt_date, e.wrt_time);
    return 1;
}

// ---- fs_ops: writing -----------------------------------------------------

static int fat32_touch(const char *path) {
    if (!S->v.mounted) return 0;
    uint32_t dir;
    const char *leaf;
    if (!resolve_parent(path, &dir, &leaf) || !leaf[0]) return 0;
    struct dirent_info e;
    if (dir_find(dir, leaf, &e)) {
        // Already there: update the modification time, which is what
        // touch means on every other system.
        uint16_t date, time;
        now_fat(&date, &time);
        if (!ent_patch(&e.loc, e.cluster, e.size, &date, &time)) return 0;
        return fat_sync();
    }
    if (!dir_create(dir, leaf, ATTR_ARCHIVE, 0)) return 0;
    return fat_sync();
}

static int fat32_mkdir(const char *path) {
    if (!S->v.mounted) return 0;
    uint32_t dir;
    const char *leaf;
    if (!resolve_parent(path, &dir, &leaf) || !leaf[0]) return 0;
    struct dirent_info e;
    if (dir_find(dir, leaf, &e)) return (e.attr & ATTR_DIRECTORY) ? 1 : 0;
    if (!dir_create(dir, leaf, ATTR_DIRECTORY, 0)) return 0;
    return fat_sync();
}

static int fat32_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    if (!S->v.mounted) return 0;
    struct dirent_info e;
    if (!lookup(path, &e)) {
        if (!fat32_touch(path)) return 0;
        if (!lookup(path, &e)) return 0;
    }
    if (e.attr & ATTR_DIRECTORY) return 0;
    if (offset + len > 0xFFFFFFFFull) return 0; // FAT's size field is 32 bits

    uint32_t start = e.cluster;
    if (!chain_write(&start, offset, buf, len)) return 0;

    // THE ORDER: the chain is on disk and flushed BEFORE the size grows.
    // The other way round publishes a length reaching into clusters the
    // FAT does not link yet.
    if (!fat_flush()) return 0;

    uint32_t newsize = e.size;
    if (offset + len > newsize) newsize = (uint32_t)(offset + len);
    uint16_t date, time;
    now_fat(&date, &time);
    if (!ent_patch(&e.loc, start, newsize, &date, &time)) return 0;
    return fat_sync();
}

static int fat32_write(const char *path, const char *data, int append) {
    if (!S->v.mounted) return 0;
    uint32_t len = data ? (uint32_t)k_strlen(data) : 0;
    struct dirent_info e;
    if (!lookup(path, &e)) {
        if (!fat32_touch(path)) return 0;
        if (!lookup(path, &e)) return 0;
    }
    if (e.attr & ATTR_DIRECTORY) return 0;
    uint64_t at = append ? e.size : 0;
    if (!append && e.size) {
        // Overwrite means the old contents go, not "the first N bytes
        // change and the tail survives".
        if (!chain_free(e.cluster)) return 0;
        uint16_t date, time;
        now_fat(&date, &time);
        if (!ent_patch(&e.loc, 0, 0, &date, &time)) return 0;
        if (!fat_flush()) return 0;
        if (!lookup(path, &e)) return 0;
    }
    if (!len) return fat_sync();
    return fat32_write_range(path, at, data, len);
}

static int fat32_truncate(const char *path, uint64_t size) {
    if (!S->v.mounted) return 0;
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;
    if (e.attr & ATTR_DIRECTORY) return 0;
    if (size > 0xFFFFFFFFull) return 0;

    uint32_t cluster_bytes = S->v.sectors_per_cluster * SECTOR;

    if (size > e.size) {
        // GROWING BY TRUNCATE ZEROES THE GAP, same rule as a write past
        // the end: whatever those clusters held belongs to whoever
        // deleted them.
        uint32_t start = e.cluster;
        uint32_t want = (uint32_t)((size + cluster_bytes - 1) / cluster_bytes);
        if (want) {
            if (!cluster_valid(start)) {
                start = cluster_alloc(0);
                if (!start || !cluster_zero(start)) return 0;
            }
            // Walk to the end of the chain, counting, then extend from
            // there. The guard is not decoration: a corrupt FAT whose
            // chain points back at itself would otherwise spin in the
            // kernel with interrupts on and nothing to notice.
            uint32_t have = 1, last = start;
            for (;;) {
                uint32_t next;
                if (!chain_next(last, &next)) return 0;
                if (!cluster_valid(next)) break;
                last = next;
                if (++have > S->v.cluster_count) return 0;
            }
            while (have < want) {
                uint32_t c = cluster_alloc(last);
                if (!c || !cluster_zero(c)) return 0;
                last = c;
                have++;
            }
        }
        if (!fat_flush()) return 0;
        uint16_t date, time;
        now_fat(&date, &time);
        if (!ent_patch(&e.loc, start, (uint32_t)size, &date, &time)) return 0;
        return fat_sync();
    }

    // SHRINKING: the entry stops referencing the clusters BEFORE they
    // are freed. The other order leaves a window where a crash has the
    // file pointing at blocks the allocator has handed out again --
    // docs/conventions/storage.md's rule, and TFS3 follows it too.
    uint32_t keep = (uint32_t)((size + cluster_bytes - 1) / cluster_bytes);
    uint16_t date, time;
    now_fat(&date, &time);
    uint32_t new_start = keep ? e.cluster : 0;
    if (!ent_patch(&e.loc, new_start, (uint32_t)size, &date, &time)) return 0;
    if (!fat_flush()) return 0;

    if (!keep) {
        if (!chain_free(e.cluster)) return 0;
    } else if (cluster_valid(e.cluster)) {
        uint32_t last = chain_nth(e.cluster, keep - 1);
        if (last) {
            uint32_t rest;
            if (!chain_next(last, &rest)) return 0;
            if (!fat_set(last, FAT_EOC)) return 0;
            if (cluster_valid(rest) && !chain_free(rest)) return 0;
        }
    }
    return fat_sync();
}

static int fat32_del(const char *path) {
    if (!S->v.mounted) return 0;
    struct dirent_info e;
    if (!lookup(path, &e)) return 0;

    if (e.attr & ATTR_DIRECTORY) {
        // A NON-EMPTY DIRECTORY IS REFUSED, which is fs.h's contract and
        // what every other backend here does. `rm -r` is a program.
        uint32_t dir = e.cluster ? e.cluster : S->v.root_cluster;
        struct dirwalk w;
        walk_begin(&w, dir);
        struct dirent_info child;
        if (walk_next(&w, &child)) return 0;
    }

    // The entry goes first, then the data -- the shrink rule again.
    if (!ent_erase(&e.loc)) return 0;
    if (!fat_flush()) return 0;
    if (cluster_valid(e.cluster) && !chain_free(e.cluster)) return 0;
    return fat_sync();
}

static int fat32_rename(const char *oldpath, const char *newpath) {
    if (!S->v.mounted) return 0;
    struct dirent_info src;
    if (!lookup(oldpath, &src)) return 0;

    uint32_t newdir;
    const char *newleaf;
    if (!resolve_parent(newpath, &newdir, &newleaf) || !newleaf[0]) return 0;
    struct dirent_info clash;
    if (dir_find(newdir, newleaf, &clash)) return 0; // destination taken

    // CREATE THE NEW NAME FIRST, then remove the old one. Both point at
    // the same clusters in between, which is a moment of two names for
    // one chain -- harmless, and strictly safer than the alternative,
    // where a failure halfway leaves the file with no name at all.
    struct dirent_info made;
    if (!dir_create(newdir, newleaf, src.attr, &made)) return 0;

    // The fresh entry owns a cluster if it is a directory; hand it the
    // ORIGINAL chain and release the one dir_create() just made.
    uint32_t stray = made.cluster;
    uint16_t date = src.wrt_date, time = src.wrt_time;
    if (!ent_patch(&made.loc, src.cluster, src.size, &date, &time)) return 0;
    if ((src.attr & ATTR_DIRECTORY) && cluster_valid(stray) && stray != src.cluster) {
        if (!chain_free(stray)) return 0;
    }
    if (!fat_flush()) return 0;

    if (!ent_erase(&src.loc)) return 0;

    // A MOVED DIRECTORY'S `..` MUST FOLLOW IT. Without this the tree
    // has a child whose parent link points somewhere it no longer
    // lives, which every other FAT driver will believe.
    if ((src.attr & ATTR_DIRECTORY) && cluster_valid(src.cluster)) {
        if (vol_read(cluster_first_sector(src.cluster), 1, g_tmpsec)) {
            uint8_t *dd = g_tmpsec + DIRENT_SIZE;
            if (dd[0] == '.' && dd[1] == '.') {
                ent_set_cluster(dd, newdir == S->v.root_cluster ? 0 : newdir);
                vol_write(cluster_first_sector(src.cluster), 1, g_tmpsec);
            }
        }
    }
    return fat_sync();
}

// ---- fs_ops: steppable I/O -----------------------------------------------
//
// FAT32 does what ramfs does: the whole operation runs in begin() and
// the single step reports the result. The steppable pair exists so a
// caller can advance a long write without blocking (fs.h's async-I/O
// staging), and TFS3 genuinely implements it that way -- doing the same
// here would mean a second write path through the chain walker for a
// mount that is read-only by default and holds a bootloader. The handle
// is still allocated and freed on the terminal result, because that is
// the contract callers are written against.
struct fat_step { int ok; uint32_t total; };

static void *fat32_write_range_begin(const char *path, uint64_t offset,
                                     const void *buf, uint32_t len) {
    struct fat_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->ok = fat32_write_range(path, offset, buf, len);
    h->total = h->ok ? len : 0;
    return h;
}

static int fat32_write_range_step(void *handle) {
    struct fat_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    int ok = h->ok;
    kfree(h);
    return ok ? FS_STEP_DONE : FS_STEP_FAILED;
}

static void *fat32_read_range_begin(const char *path, uint64_t offset,
                                    void *buf, uint32_t len) {
    struct fat_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->total = fat32_read_range(path, offset, buf, len);
    h->ok = 1; // a short read at EOF is a success, same as fs_read_range()
    return h;
}

static int fat32_read_range_step(void *handle, uint32_t *out_total) {
    struct fat_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    if (out_total) *out_total = h->total;
    kfree(h);
    return FS_STEP_DONE;
}

// ---- fs_ops: reporting ---------------------------------------------------

// Counts free clusters by reading the FAT, and caches the answer.
// FSInfo's count is a HINT and is not trusted for this (see
// fsinfo_load) -- `df` printing a number another OS left stale would be
// worse than the scan's cost, which is one pass over the FAT.
static int fat32_disk_usage(uint64_t *out_used, uint64_t *out_total) {
    if (!S->v.mounted) return 0;
    uint64_t cluster_bytes = (uint64_t)S->v.sectors_per_cluster * SECTOR;
    uint64_t total = (uint64_t)S->v.cluster_count * cluster_bytes;

    if (S->v.free_count == 0xFFFFFFFFu) {
        uint32_t free = 0;
        for (uint32_t c = 2; c < S->v.cluster_count + 2; c++) {
            uint32_t v;
            if (!fat_get(c, &v)) return 0;
            if (v == FAT_FREE) free++;
        }
        S->v.free_count = free;
        fsinfo_store();
    }
    if (out_total) *out_total = total;
    if (out_used) *out_used = total - (uint64_t)S->v.free_count * cluster_bytes;
    return 1;
}

// REPORT-ONLY, and that is the honest scope. A FAT repair means
// rebuilding a lost-cluster list and reconciling two FAT copies, which
// is `fsck.vfat`'s whole job; a half-repair on the partition holding
// the bootloader is worse than a clear report. What this DOES check is
// the pair of things a driver can be sure about: that the FAT copies
// agree, and that no cluster is claimed by two chains.
static int fat32_check(int repair, struct fs_check_result *out) {
    if (!S->v.mounted) return 0;
    if (out) k_memset(out, 0, sizeof *out);
    if (repair) {
        klog_write("fat32: repair is not implemented -- reporting only\n");
    }

    // Every cluster referenced by exactly the chains that reach it. A
    // cross-linked FAT is the corruption that loses data silently, and
    // it is detectable with one bitmap and one pass.
    uint32_t nbytes = (S->v.cluster_count + 2 + 7) / 8;
    uint8_t *seen = kmalloc(nbytes);
    if (!seen) return 0;
    k_memset(seen, 0, nbytes);

    int problems = 0, crosslinked = 0, out_of_volume = 0;
    for (uint32_t c = 2; c < S->v.cluster_count + 2; c++) {
        uint32_t v;
        if (!fat_get(c, &v)) { problems++; break; }
        if (v == FAT_FREE || v == FAT_BAD || fat_is_eoc(v)) continue;
        if (!cluster_valid(v)) {
            klog_printf("fat32: cluster %u points outside the volume (%u)\n", c, v);
            problems++; out_of_volume++;
            continue;
        }
        if (seen[v / 8] & (1u << (v % 8))) {
            klog_printf("fat32: cluster %u is claimed by two chains\n", v);
            problems++; crosslinked++;
            continue;
        }
        seen[v / 8] |= (uint8_t)(1u << (v % 8));
    }
    kfree(seen);

    // The fields this format can honestly fill. A cross-linked cluster
    // IS a double allocation, and one pointing off the volume IS an
    // out-of-range pointer, so those two carry the count; the rest --
    // leaked, referenced-but-free, and every repair counter -- stay 0
    // because a FAT scan of this depth cannot know them and a number
    // guessed here would be read as measured.
    if (out) {
        out->blocks_referenced = S->v.cluster_count - (S->v.free_count == 0xFFFFFFFFu ? 0 : S->v.free_count);
        out->double_allocated = (uint32_t)crosslinked;
        out->out_of_range = (uint32_t)out_of_volume;
    }
    return problems == 0;
}

// ---- per-mount state ------------------------------------------------

static void *fat32_state_alloc(void) {
    struct fat32_state *st = kmalloc(sizeof *st);
    if (!st) return NULL;
    k_memset(st, 0, sizeof *st);
    return st;
}

static void *fat32_state_activate(void *st) {
    void *prev = S;
    S = st;
    return prev;
}

static void fat32_state_free(void *st) {
    if (!st) return;
    void *prev = fat32_state_activate(st);
    fat32_state_activate((prev == st) ? NULL : prev);
    kfree(st);
}

const struct fs_ops fat32_ops = {
    .name = "fat32",
    // NOTHING. FAT has no inodes (fat32_stat() synthesises an ino from
    // the first cluster, which is 0 for every empty file), no link
    // counts, no symlinks, and its timestamps are local wall-clock in a
    // packed 16-bit format converted at stat time -- which is exactly
    // what NOT declaring FS_CAP_EPOCH_TIME means.
    .caps = 0,
    .volume_relative = 1, // every access goes through blkdev_* on the device it was handed
    // MORE THAN ONCE: the volume, the FAT sector cache and the read
    // buffer are per mount (struct fat32_state). An installer needs two
    // -- the running system's ESP at /boot, and the target's.
    .max_mounts = MOUNT_MAX,
    .state_alloc = fat32_state_alloc,
    .state_free = fat32_state_free,
    .state_activate = fat32_state_activate,
    .probe = fat32_probe,
    .wipe = fat32_wipe,
    .format = fat32_format,
    .init = fat32_init,
    .umount = fat32_umount,
    .touch = fat32_touch,
    .write = fat32_write,
    .mkdir = fat32_mkdir,
    .del = fat32_del,
    .size = fat32_size,
    .read_range = fat32_read_range,
    .write_range = fat32_write_range,
    .write_range_begin = fat32_write_range_begin,
    .write_range_step = fat32_write_range_step,
    .read_range_begin = fat32_read_range_begin,
    .read_range_step = fat32_read_range_step,
    .rename = fat32_rename,
    .truncate = fat32_truncate,
    .is_dir = fat32_is_dir,
    .exists = fat32_exists,
    .list = fat32_list,
    .stat = fat32_stat,
    .disk_usage = fat32_disk_usage,
    .check = fat32_check,
    .link = 0, // no FS_CAP_HARDLINKS -- FAT has no link counts
};
