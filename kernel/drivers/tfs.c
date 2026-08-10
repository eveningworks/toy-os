// TFS2 -- this OS's persistent filesystem, journaled and timestamped
// as of build 480 (previously "TFS1": write-through, no timestamps).
// This used to be fs.c itself, back when it was the only filesystem
// toy-os could have; it's now just one backend behind the VFS dispatch
// layer (vfs.c), reachable only through the `tfs_ops` vtable at the
// bottom of this file (see fs_ops.h for what that interface is and why
// it exists, and kernel/include/tfs.h for this file's own public
// surface). Nothing outside vfs.c should #include tfs.h or call
// anything in this file directly -- go through fs.h's fs_* API
// instead, same as before this split.
//
// In-memory file table, now optionally backed by a real disk via ata.c
// so files survive a reboot, with directory support: every entry (file
// or directory) is identified by a full normalized absolute path
// ("/docs/notes.txt") rather than a bare flat name. `files[]` below is
// still the full, fast, in-memory picture every existing caller
// (fs_read/fs_list, and everything built on them -- Notepad, the
// shell's ls/cat/write/mkdir/cd, the SYS_OPEN/READ/WRITE/CLOSE
// syscalls) reads from directly, via vfs.c. Disk I/O only happens at
// tfs_init() (load the whole table once, replaying any pending journal
// entry first -- see below) and at the end of each *mutating* call --
// tfs_touch/tfs_write/tfs_mkdir/tfs_delete -- which write through to
// disk immediately after updating `files[]`, via the journal.
//
// Directories are deliberately just another entry with no data, not a
// separate on-disk structure: an entry's parent/children relationship
// is entirely derived from its path string at lookup time (see
// path_parent()/tfs_list()), the same "no separate index to keep in
// sync" reasoning the original flat design already used for files. The
// implicit root "/" has no entry of its own -- it always exists without
// needing one, so an empty disk still has a (empty) root directory.
//
// On-disk layout (only meaningful if this backend reports persistent --
// see tfs_init() below):
//   LBA 0:             a one-sector superblock: magic "TFS2" + a
//                       version byte. tfs_init() only loads a disk
//                       whose magic+version matches *exactly* -- a disk
//                       written by an older kernel (the pre-journal
//                       "TFS1" layout, or the pre-directories flat
//                       layout before that) would otherwise be misread
//                       (garbage paths, wrong field offsets). No
//                       migration path: an old disk is just treated as
//                       foreign and reformatted, same as a blank one --
//                       acceptable since nothing about this project
//                       needs old disk images to keep working across a
//                       format change.
//   LBA 1:              the journal header (one sector) -- see the
//                       "Journaling" section below.
//   LBA 2 .. 2+FS_RECORD_SECTORS-1:
//                       the journal's data area -- one record's worth
//                       of space, holding whatever mutation is
//                       currently (or was most recently) in flight.
//   FS_TABLE_START_LBA onward:
//                       FS_MAX_FILES fixed-size records, one per table
//                       slot, each FS_RECORD_SECTORS sectors long
//                       (rounded up from path+type+used+size+
//                       created+modified+FS_DATA_MAX so the whole
//                       record always lands on a sector boundary --
//                       see serialize_record()). Slot i is always at
//                       the same LBA, so there's no separate on-disk
//                       directory/index to keep in sync with the table
//                       itself. Adding the two rtc_time timestamps
//                       (14 bytes) didn't grow FS_RECORD_SECTORS either
//                       (still 5 sectors/2560 bytes -- there was
//                       headroom left from the FS_PATH_MAX growth), and
//                       disk.img has ample room left over for the new
//                       journal region too.
//
// Journaling: a write-ahead log, sized for exactly one record in
// flight -- every mutating call here (touch/write/mkdir/delete) only
// ever changes ONE table slot, so one journal slot is always enough;
// this isn't a general-purpose multi-record transaction log. persist_
// record() -- see its own comment -- writes the new record contents to
// the journal's data area, then flips the journal header's commit flag
// (the single-sector write that's this scheme's atomicity boundary),
// then applies the same bytes to the real table slot, then clears the
// header. replay_journal(), called from tfs_init() before the table
// load, checks on every boot whether a commit was left set: if so, and
// the journal payload's checksum still matches, it replays those exact
// bytes into the target slot (safe/idempotent whether or not the real
// table write had already finished before the crash); if the checksum
// doesn't match, the journal write itself was torn, so the mutation
// never reached a committed state and is simply discarded -- the real
// table slot was never touched for it, so it's still exactly as it was
// before. This closes the "crash mid-write corrupts one record" window
// TFS1 explicitly accepted (see CHANGELOG.md's build 480 and docs/
// decisions.md). The checksum is a plain FNV-1a hash -- enough to catch
// an accidentally-torn write with overwhelming probability, not a
// cryptographic integrity check against deliberate corruption.
//
// Timestamps: every record now carries `created` and `modified`, both
// struct rtc_time (timer.h) read via tz.c's rtc_read_local() -- the
// same local-time source SYS_GETTIME and the shell's `time` use, not a
// Unix epoch integer (see fs.h's fs_stat() doc comment for why, and
// docs/tfs2-spec.md for how a host-side tool should interpret them).
// Set once at creation (tfs_touch()/tfs_mkdir(), new-entry path only --
// touching an already-existing file stays a no-op, matching pre-TFS2
// behavior) and `modified` is bumped again on every tfs_write() that
// actually changes a file's data. One honest quirk: entries created
// very early in boot (kernel_main()'s own `fs_mkdir("/etc")`, which
// runs before tz_init() has loaded a timezone selection) get a raw-UTC
// `created` stamp rather than the user's configured local time, since
// there's no selected city yet at that point.
//
// Honest limitations, not solved here:
//   - The journal protects each individual record write, but there's no
//     locking against two mutations racing each other if the scheduler
//     ever preempted mid-write -- not a real risk today (every fs_*
//     caller runs its mutation to completion before yielding, and nothing
//     in this kernel calls fs_* concurrently from two contexts), but
//     worth naming rather than silently assuming away.
//   - No recursive delete: tfs_delete() on a non-empty directory fails
//     outright rather than deleting its contents. Deliberate -- see
//     fs.h.
//   - This file only validates that a path is already *normalized*
//     (path_is_normalized()) -- it doesn't resolve ".."/"." or a
//     cwd-relative path itself. That's the caller's job (see the
//     shell's `cd`/`pwd` and its resolve_path() in shell.c).
#include "fs.h"
#include "tfs.h"
#include "string.h"
#include "ata.h"
#include "klog.h"
#include "tz.h"

enum fs_entry_type { FS_TYPE_FILE = 0, FS_TYPE_DIR = 1 };

struct file {
    char path[FS_PATH_MAX];
    uint8_t type; // FS_TYPE_FILE or FS_TYPE_DIR
    char data[FS_DATA_MAX];
    uint32_t size;
    int used;
    struct rtc_time created;
    struct rtc_time modified;
};

static struct file files[FS_MAX_FILES];
static int g_disk_backed = 0;

// "TFS2" -- see top comment. Version byte tracks revisions of the TFS2
// layout itself (starts at 1); a magic-only match with a different
// version would mean a future TFS2 revision, not a different
// filesystem, but this kernel has only ever had version 1 of it so
// far, same "no migration, just reformat" policy as the magic bytes.
#define FS_DISK_MAGIC0 'T'
#define FS_DISK_MAGIC1 'F'
#define FS_DISK_MAGIC2 'S'
#define FS_DISK_MAGIC3 '2'
#define FS_DISK_VERSION 1
#define FS_SUPERBLOCK_LBA 0

// One record must hold path(FS_PATH_MAX) + type(1) + used(1) + size(4) +
// created(FS_RTC_BYTES) + modified(FS_RTC_BYTES) + data(FS_DATA_MAX),
// rounded up to a whole number of sectors so every slot starts at a
// clean LBA -- see record_lba().
#define FS_RTC_BYTES 7 // hour, minute, second, day, month (1 byte each) + year (uint16 LE)
#define FS_RECORD_RAW_BYTES (FS_PATH_MAX + 1 + 1 + 4 + FS_RTC_BYTES * 2 + FS_DATA_MAX)
#define FS_RECORD_SECTORS ((FS_RECORD_RAW_BYTES + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
#define FS_RECORD_BYTES (FS_RECORD_SECTORS * ATA_SECTOR_SIZE)

// Byte offsets within one serialized record -- named rather than
// inlined at every use site now that there are enough fields for the
// old "just add a literal" style to get error-prone.
#define REC_OFF_TYPE     FS_PATH_MAX
#define REC_OFF_USED     (FS_PATH_MAX + 1)
#define REC_OFF_SIZE     (FS_PATH_MAX + 2)
#define REC_OFF_CREATED  (FS_PATH_MAX + 6)
#define REC_OFF_MODIFIED (REC_OFF_CREATED + FS_RTC_BYTES)
#define REC_OFF_DATA     (REC_OFF_MODIFIED + FS_RTC_BYTES)

// The journal: one header sector plus one record's worth of data
// sectors, sitting between the superblock and the table proper -- see
// this file's top comment ("Journaling") for the write-ahead scheme
// this backs.
#define FS_JOURNAL_HEADER_LBA (FS_SUPERBLOCK_LBA + 1)
#define FS_JOURNAL_DATA_LBA   (FS_JOURNAL_HEADER_LBA + 1)
#define FS_TABLE_START_LBA    (FS_JOURNAL_DATA_LBA + FS_RECORD_SECTORS)

static uint32_t record_lba(int index) {
    return FS_TABLE_START_LBA + (uint32_t)index * FS_RECORD_SECTORS;
}

static void serialize_rtc(const struct rtc_time *t, uint8_t *buf) {
    buf[0] = t->hour;
    buf[1] = t->minute;
    buf[2] = t->second;
    buf[3] = t->day;
    buf[4] = t->month;
    buf[5] = (uint8_t)(t->year & 0xFF);
    buf[6] = (uint8_t)((t->year >> 8) & 0xFF);
}

static void deserialize_rtc(struct rtc_time *t, const uint8_t *buf) {
    t->hour = buf[0];
    t->minute = buf[1];
    t->second = buf[2];
    t->day = buf[3];
    t->month = buf[4];
    t->year = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
}

static void serialize_record(const struct file *f, uint8_t *buf) {
    k_memset(buf, 0, FS_RECORD_BYTES);
    k_memcpy(buf, f->path, FS_PATH_MAX);
    buf[REC_OFF_TYPE] = f->type;
    buf[REC_OFF_USED] = (uint8_t)f->used;
    uint32_t size = f->size;
    buf[REC_OFF_SIZE + 0] = (uint8_t)(size & 0xFF);
    buf[REC_OFF_SIZE + 1] = (uint8_t)((size >> 8) & 0xFF);
    buf[REC_OFF_SIZE + 2] = (uint8_t)((size >> 16) & 0xFF);
    buf[REC_OFF_SIZE + 3] = (uint8_t)((size >> 24) & 0xFF);
    serialize_rtc(&f->created, buf + REC_OFF_CREATED);
    serialize_rtc(&f->modified, buf + REC_OFF_MODIFIED);
    k_memcpy(buf + REC_OFF_DATA, f->data, FS_DATA_MAX);
}

static void deserialize_record(struct file *f, const uint8_t *buf) {
    k_memcpy(f->path, buf, FS_PATH_MAX);
    f->type = buf[REC_OFF_TYPE];
    f->used = buf[REC_OFF_USED];
    f->size = (uint32_t)buf[REC_OFF_SIZE + 0] |
              ((uint32_t)buf[REC_OFF_SIZE + 1] << 8) |
              ((uint32_t)buf[REC_OFF_SIZE + 2] << 16) |
              ((uint32_t)buf[REC_OFF_SIZE + 3] << 24);
    deserialize_rtc(&f->created, buf + REC_OFF_CREATED);
    deserialize_rtc(&f->modified, buf + REC_OFF_MODIFIED);
    k_memcpy(f->data, buf + REC_OFF_DATA, FS_DATA_MAX);
}

// Plain FNV-1a, 32-bit -- see this file's top comment ("Journaling") for
// why this doesn't need to be cryptographic, just good enough to catch
// an accidentally-torn write.
static uint32_t fnv1a(const uint8_t *buf, int len) {
    uint32_t hash = 0x811C9DC5u;
    for (int i = 0; i < len; i++) {
        hash ^= buf[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static int write_table_slot(int index, const uint8_t *buf) {
    uint32_t lba = record_lba(index);
    for (int s = 0; s < FS_RECORD_SECTORS; s++) {
        if (!ata_write_sector(lba + s, buf + (uint32_t)s * ATA_SECTOR_SIZE)) return 0;
    }
    return 1;
}

// Writes (or clears) the journal header sector -- see this file's top
// comment. `commit` 0 means "empty, nothing pending"; 1 means "slot/
// checksum below describe a mutation that must be replayed if this
// header is still set at the next boot."
static int write_journal_header(int commit, uint32_t slot, uint32_t checksum) {
    uint8_t buf[ATA_SECTOR_SIZE];
    k_memset(buf, 0, sizeof(buf));
    buf[0] = 'J'; buf[1] = 'R'; buf[2] = 'N'; buf[3] = '1';
    buf[4] = (uint8_t)commit;
    buf[5] = (uint8_t)(slot & 0xFF);
    buf[6] = (uint8_t)((slot >> 8) & 0xFF);
    buf[7] = (uint8_t)((slot >> 16) & 0xFF);
    buf[8] = (uint8_t)((slot >> 24) & 0xFF);
    buf[9]  = (uint8_t)(checksum & 0xFF);
    buf[10] = (uint8_t)((checksum >> 8) & 0xFF);
    buf[11] = (uint8_t)((checksum >> 16) & 0xFF);
    buf[12] = (uint8_t)((checksum >> 24) & 0xFF);
    return ata_write_sector(FS_JOURNAL_HEADER_LBA, buf);
}

// Returns 0 if the header sector doesn't even have the journal magic
// (a disk this format just freshly reformatted, before the first
// write_journal_header(0,...) call ever ran on it -- see tfs_init())
// or couldn't be read at all; 1 otherwise, with *out_commit/*out_slot/
// *out_checksum filled in.
static int read_journal_header(int *out_commit, uint32_t *out_slot, uint32_t *out_checksum) {
    uint8_t buf[ATA_SECTOR_SIZE];
    if (!ata_read_sector(FS_JOURNAL_HEADER_LBA, buf)) return 0;
    if (buf[0] != 'J' || buf[1] != 'R' || buf[2] != 'N' || buf[3] != '1') return 0;
    *out_commit = buf[4];
    *out_slot = (uint32_t)buf[5] | ((uint32_t)buf[6] << 8) |
                ((uint32_t)buf[7] << 16) | ((uint32_t)buf[8] << 24);
    *out_checksum = (uint32_t)buf[9] | ((uint32_t)buf[10] << 8) |
                    ((uint32_t)buf[11] << 16) | ((uint32_t)buf[12] << 24);
    return 1;
}

// Writes slot `index`'s current in-memory contents to its fixed disk
// location, via the journal. Called after every mutation (touch/write/
// mkdir/delete) once g_disk_backed is known -- a no-op (returns success
// trivially) when there's no disk, so callers don't need their own "if
// persistent" branch. See this file's top comment ("Journaling") for
// why this is a 4-step write-ahead sequence rather than one direct
// write to the table slot.
static int persist_record(int index) {
    if (!g_disk_backed) return 1;
    uint8_t buf[FS_RECORD_BYTES];
    serialize_record(&files[index], buf);
    uint32_t checksum = fnv1a(buf, FS_RECORD_BYTES);

    // 1. Stage the new contents in the journal's data area.
    for (int s = 0; s < FS_RECORD_SECTORS; s++) {
        if (!ata_write_sector(FS_JOURNAL_DATA_LBA + s, buf + (uint32_t)s * ATA_SECTOR_SIZE)) return 0;
    }
    // 2. Commit -- the single-sector write that's this scheme's
    //    atomicity boundary. Once this returns, replay_journal() will
    //    correctly finish this mutation on the next boot even if
    //    everything below never runs.
    if (!write_journal_header(1, (uint32_t)index, checksum)) return 0;
    // 3. Apply the same bytes to the real table slot.
    if (!write_table_slot(index, buf)) return 0;
    // 4. Clear the journal -- fully durable in its final home now, so
    //    there's nothing left to replay.
    write_journal_header(0, 0, 0);
    return 1;
}

// Called once from tfs_init(), before loading the table -- checks
// whether an unclean shutdown left a committed journal entry pending,
// and either finishes it (checksum still matches: replay is always
// safe/idempotent, whether or not the real table write had already
// completed before the crash) or discards it (checksum doesn't match:
// the journal write itself was torn, so the real table slot was never
// touched for this mutation and needs no recovery). See this file's
// top comment ("Journaling").
static void replay_journal(void) {
    int commit;
    uint32_t slot, checksum;
    if (!read_journal_header(&commit, &slot, &checksum)) return; // no journal magic yet
    if (!commit) return; // nothing pending
    if (slot >= FS_MAX_FILES) { write_journal_header(0, 0, 0); return; } // corrupt header

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

// Backs tfs_ops.init -- see fs_ops.h for the contract (return 1 if
// persisted to real storage, 0 if RAM-only).
static int tfs_init(void) {
    k_memset(files, 0, sizeof(files));
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
        // Recognized filesystem, matching version -- replay any pending
        // journal entry first (see replay_journal()), then load every
        // slot from disk. A slot that fails to read (shouldn't happen
        // on real hardware/QEMU, but this is a toy driver with bounded
        // retries, not infinite ones -- see ata.c) is just left
        // zeroed/unused rather than aborting the whole load.
        g_disk_backed = 1;
        replay_journal();
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
        // Blank, foreign, or old-version disk -- format it fresh: write
        // the superblock, clear the journal, and write every
        // (currently-empty, thanks to the k_memset above) slot.
        g_disk_backed = 1;
        write_superblock();
        write_journal_header(0, 0, 0);
        for (int i = 0; i < FS_MAX_FILES; i++) persist_record(i);
        klog_write("fs: formatted a fresh persistent filesystem on disk\n");
    }
    return g_disk_backed;
}

// ---- path helpers ----
//
// Every entry point below normalizes its path argument with
// normalize() before doing anything else, then works only with that
// normalized form. See fs.h's top comment for exactly what
// "normalized" means (absolute, no trailing slash except root itself,
// no "."/".." components) and why a bare name like "notes.txt" is
// silently treated as "/notes.txt" rather than rejected.

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
    if (len == 1) return 1; // exactly "/"
    if (p[len - 1] == '/') return 0; // no trailing slash otherwise

    size_t i = 1;
    while (i < len) {
        size_t start = i;
        while (i < len && p[i] != '/') i++;
        if (!is_valid_component(p + start, (int)(i - start))) return 0;
        if (i < len) i++; // skip the '/'
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

// Parent of a normalized non-root path. Never called on "/" itself
// (root has no parent) -- callers guard that case separately.
static void path_parent(const char *norm_path, char out[FS_PATH_MAX]) {
    size_t len = k_strlen(norm_path);
    size_t last_slash = 0;
    for (size_t i = 0; i < len; i++) {
        if (norm_path[i] == '/') last_slash = i;
    }
    if (last_slash == 0) {
        out[0] = '/';
        out[1] = '\0';
    } else {
        k_memcpy(out, norm_path, last_slash);
        out[last_slash] = '\0';
    }
}

static struct file *find(const char *norm_path) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && k_strcmp(files[i].path, norm_path) == 0) return &files[i];
    }
    return 0;
}

static int find_free_index(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) return i;
    }
    return -1;
}

// True if `norm_path`'s parent directory exists -- either the implicit
// root, or a real entry of type FS_TYPE_DIR. `norm_path` must not be
// "/" itself.
static int parent_is_dir(const char *norm_path) {
    char parent[FS_PATH_MAX];
    path_parent(norm_path, parent);
    if (k_strcmp(parent, "/") == 0) return 1;
    struct file *f = find(parent);
    return f && f->type == FS_TYPE_DIR;
}

// ---- backend implementation (see fs_ops.h for the interface these
// satisfy, and tfs_ops at the bottom of this file for the wiring) ----

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
    if (k_strcmp(norm, "/") == 0) return 0; // can't touch root

    struct file *existing = find(norm);
    if (existing) return existing->type == FS_TYPE_FILE; // already a file: fine; a dir: conflict

    if (!parent_is_dir(norm)) return 0;

    int idx = find_free_index();
    if (idx < 0) return 0;

    struct file *f = &files[idx];
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
    if (k_strcmp(norm, "/") == 0) return 0; // root always exists
    if (find(norm)) return 0; // already exists (file or dir) -- unlike touch, that's a conflict here

    if (!parent_is_dir(norm)) return 0;

    int idx = find_free_index();
    if (idx < 0) return 0;

    struct file *f = &files[idx];
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
    if (f && f->type == FS_TYPE_DIR) return 0; // can't write to a directory
    if (!f) {
        if (!tfs_touch(norm)) return 0;
        f = find(norm);
    }

    uint32_t data_len = (uint32_t)k_strlen(data);
    uint32_t start = append ? f->size : 0;

    if (start + data_len >= FS_DATA_MAX) {
        data_len = FS_DATA_MAX - 1 - start; // truncate to fit
    }

    k_memcpy(f->data + start, data, data_len);
    f->size = start + data_len;
    f->data[f->size] = '\0';
    rtc_read_local(&f->modified);
    persist_record((int)(f - files));
    return 1;
}

static int tfs_delete(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0; // can't delete root

    struct file *f = find(norm);
    if (!f) return 0;

    if (f->type == FS_TYPE_DIR) {
        // Refuse if it has any direct (or deeper) child -- no
        // recursive delete, see fs.h.
        size_t plen = k_strlen(norm);
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (!files[i].used) continue;
            if (k_strncmp(files[i].path, norm, plen) == 0 && files[i].path[plen] == '/') {
                return 0;
            }
        }
    }

    f->used = 0;
    f->size = 0;
    persist_record((int)(f - files));
    return 1;
}

static const char *tfs_read(const char *path, uint32_t *out_size) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return 0;
    if (out_size) *out_size = f->size;
    return f->data;
}

static int tfs_stat(const char *path, struct fs_timestamps *out) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 0; // root has no entry -- see fs.h
    struct file *f = find(norm);
    if (!f) return 0;
    if (out) {
        out->created = f->created;
        out->modified = f->modified;
    }
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
            rest = p + 1; // every path starts with '/'
        } else {
            if (k_strncmp(p, norm, plen) != 0 || p[plen] != '/') continue;
            rest = p + plen + 1;
        }
        if (*rest == '\0') continue; // shouldn't happen

        int is_direct_child = 1;
        for (const char *q = rest; *q; q++) {
            if (*q == '/') { is_direct_child = 0; break; }
        }
        if (!is_direct_child) continue;

        cb(rest, files[i].size, files[i].type == FS_TYPE_DIR);
    }
}

// The vtable vfs.c dispatches through -- see fs_ops.h and tfs.h.
const struct fs_ops tfs_ops = {
    .name = "tfs2",
    .init = tfs_init,
    .touch = tfs_touch,
    .write = tfs_write,
    .mkdir = tfs_mkdir,
    .del = tfs_delete,
    .read = tfs_read,
    .is_dir = tfs_is_dir,
    .exists = tfs_exists,
    .list = tfs_list,
    .stat = tfs_stat,
};
