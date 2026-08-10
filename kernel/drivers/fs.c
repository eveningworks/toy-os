// The in-memory file table this always was, now optionally backed by a
// real disk via ata.c so files survive a reboot, and now with directory
// support: every entry (file or directory) is identified by a full
// normalized absolute path ("/docs/notes.txt") rather than a bare flat
// name. `files[]` below is still the full, fast, in-memory picture
// every existing caller (fs_read/fs_list, and everything built on them
// -- Notepad, the shell's ls/cat/write/mkdir/cd, the SYS_OPEN/READ/
// WRITE/CLOSE syscalls) reads from directly. Disk I/O only happens at
// fs_init() (load the whole table once) and at the end of each
// *mutating* call -- fs_touch/fs_write/fs_mkdir/fs_delete -- which
// write straight through to disk immediately after updating `files[]`.
//
// Directories are deliberately just another entry with no data, not a
// separate on-disk structure: an entry's parent/children relationship
// is entirely derived from its path string at lookup time (see
// path_parent()/fs_list()), the same "no separate index to keep in
// sync" reasoning the original flat design already used for files. The
// implicit root "/" has no entry of its own -- it always exists without
// needing one, so an empty disk still has a (empty) root directory.
//
// On-disk layout (only meaningful if fs_is_persistent() -- see below):
//   LBA 0:            a one-sector superblock: magic "TFS1" + a version
//                      byte. fs_init() only loads a disk whose version
//                      byte matches FS_DISK_VERSION *exactly* -- a disk
//                      written by the pre-directories kernel (version 1,
//                      32-byte flat names, no type byte) would otherwise
//                      be misread as this version's layout (garbage
//                      paths, wrong data offset). No migration path: an
//                      old disk is just treated as foreign and
//                      reformatted, same as a blank one.
//   LBA 1 onward:      FS_MAX_FILES fixed-size records, one per table
//                      slot, each FS_RECORD_SECTORS sectors long
//                      (rounded up from path+type+used+size+FS_DATA_MAX
//                      so the whole record always lands on a sector
//                      boundary -- see serialize_record()). Slot i is
//                      always at the same LBA, so there's no separate
//                      on-disk directory/index to keep in sync with the
//                      table itself. Growing FS_PATH_MAX from the old
//                      32-byte flat name to 64 bytes for full paths
//                      didn't actually grow FS_RECORD_SECTORS (still 5
//                      sectors/2560 bytes -- there was headroom), and
//                      disk.img (1MB/2048 sectors) has plenty of room
//                      left over even after doubling FS_MAX_FILES.
//
// Honest limitations, not solved here:
//   - Write-through, no journaling: fs_touch()/fs_write()/fs_mkdir()/
//     fs_delete() write their one record to disk immediately, so a
//     clean reboot or `reboot` mid-idle never loses anything already
//     returned from a call. A crash/power-loss landing exactly between
//     two of those sector writes could still leave that one record
//     inconsistent -- a real filesystem would solve this with
//     journaling or copy-on-write; this one just accepts the (small,
//     single-record) window.
//   - No recursive delete: fs_delete() on a non-empty directory fails
//     outright rather than deleting its contents. Deliberate -- see
//     fs.h.
//   - fs.c only validates that a path is already *normalized*
//     (path_is_normalized()) -- it doesn't resolve ".."/"." or a
//     cwd-relative path itself. That's the caller's job (see the
//     shell's `cd`/`pwd` and its resolve_path() in shell.c).
#include "fs.h"
#include "string.h"
#include "ata.h"
#include "serial.h"

enum fs_entry_type { FS_TYPE_FILE = 0, FS_TYPE_DIR = 1 };

struct file {
    char path[FS_PATH_MAX];
    uint8_t type; // FS_TYPE_FILE or FS_TYPE_DIR
    char data[FS_DATA_MAX];
    uint32_t size;
    int used;
};

static struct file files[FS_MAX_FILES];
static int g_disk_backed = 0;

#define FS_DISK_VERSION 2 // bumped from 1 -- see top comment
#define FS_SUPERBLOCK_LBA 0
#define FS_TABLE_START_LBA 1

// One record must hold path(FS_PATH_MAX) + type(1) + used(1) + size(4) +
// data(FS_DATA_MAX), rounded up to a whole number of sectors so every
// slot starts at a clean LBA -- see record_lba().
#define FS_RECORD_RAW_BYTES (FS_PATH_MAX + 1 + 1 + 4 + FS_DATA_MAX)
#define FS_RECORD_SECTORS ((FS_RECORD_RAW_BYTES + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE)
#define FS_RECORD_BYTES (FS_RECORD_SECTORS * ATA_SECTOR_SIZE)

static uint32_t record_lba(int index) {
    return FS_TABLE_START_LBA + (uint32_t)index * FS_RECORD_SECTORS;
}

static void serialize_record(const struct file *f, uint8_t *buf) {
    k_memset(buf, 0, FS_RECORD_BYTES);
    k_memcpy(buf, f->path, FS_PATH_MAX);
    buf[FS_PATH_MAX] = f->type;
    buf[FS_PATH_MAX + 1] = (uint8_t)f->used;
    uint32_t size = f->size;
    buf[FS_PATH_MAX + 2] = (uint8_t)(size & 0xFF);
    buf[FS_PATH_MAX + 3] = (uint8_t)((size >> 8) & 0xFF);
    buf[FS_PATH_MAX + 4] = (uint8_t)((size >> 16) & 0xFF);
    buf[FS_PATH_MAX + 5] = (uint8_t)((size >> 24) & 0xFF);
    k_memcpy(buf + FS_PATH_MAX + 6, f->data, FS_DATA_MAX);
}

static void deserialize_record(struct file *f, const uint8_t *buf) {
    k_memcpy(f->path, buf, FS_PATH_MAX);
    f->type = buf[FS_PATH_MAX];
    f->used = buf[FS_PATH_MAX + 1];
    f->size = (uint32_t)buf[FS_PATH_MAX + 2] |
              ((uint32_t)buf[FS_PATH_MAX + 3] << 8) |
              ((uint32_t)buf[FS_PATH_MAX + 4] << 16) |
              ((uint32_t)buf[FS_PATH_MAX + 5] << 24);
    k_memcpy(f->data, buf + FS_PATH_MAX + 6, FS_DATA_MAX);
}

// Writes slot `index`'s current in-memory contents to its fixed disk
// location. Called after every mutation (touch/write/mkdir/delete) once
// g_disk_backed is known -- a no-op (returns success trivially) when
// there's no disk, so callers don't need their own "if persistent"
// branch.
static int persist_record(int index) {
    if (!g_disk_backed) return 1;
    uint8_t buf[FS_RECORD_BYTES];
    serialize_record(&files[index], buf);
    uint32_t lba = record_lba(index);
    for (int s = 0; s < FS_RECORD_SECTORS; s++) {
        if (!ata_write_sector(lba + s, buf + (uint32_t)s * ATA_SECTOR_SIZE)) return 0;
    }
    return 1;
}

static int write_superblock(void) {
    uint8_t buf[ATA_SECTOR_SIZE];
    k_memset(buf, 0, sizeof(buf));
    buf[0] = 'T'; buf[1] = 'F'; buf[2] = 'S'; buf[3] = '1';
    buf[4] = FS_DISK_VERSION;
    return ata_write_sector(FS_SUPERBLOCK_LBA, buf);
}

void fs_init(void) {
    k_memset(files, 0, sizeof(files));
    g_disk_backed = 0;

    ata_init();
    if (!ata_present()) {
        serial_write("fs: no disk found -- files are RAM-only, won't survive reboot\n");
        return;
    }

    uint8_t sb[ATA_SECTOR_SIZE];
    if (ata_read_sector(FS_SUPERBLOCK_LBA, sb) &&
        sb[0] == 'T' && sb[1] == 'F' && sb[2] == 'S' && sb[3] == '1' &&
        sb[4] == FS_DISK_VERSION) {
        // Recognized filesystem, matching version -- load every slot
        // from disk. A slot that fails to read (shouldn't happen on
        // real hardware/QEMU, but this is a toy driver with bounded
        // retries, not infinite ones -- see ata.c) is just left
        // zeroed/unused rather than aborting the whole load.
        g_disk_backed = 1;
        uint8_t rec[FS_RECORD_BYTES];
        for (int i = 0; i < FS_MAX_FILES; i++) {
            uint32_t lba = record_lba(i);
            int ok = 1;
            for (int s = 0; s < FS_RECORD_SECTORS && ok; s++) {
                ok = ata_read_sector(lba + s, rec + (uint32_t)s * ATA_SECTOR_SIZE);
            }
            if (ok) deserialize_record(&files[i], rec);
        }
        serial_write("fs: loaded persistent filesystem from disk\n");
    } else {
        // Blank, foreign, or old-version disk -- format it fresh: write
        // the superblock and every (currently-empty, thanks to the
        // k_memset above) slot.
        g_disk_backed = 1;
        write_superblock();
        for (int i = 0; i < FS_MAX_FILES; i++) persist_record(i);
        serial_write("fs: formatted a fresh persistent filesystem on disk\n");
    }
}

int fs_is_persistent(void) {
    return g_disk_backed;
}

// ---- path helpers ----
//
// Every fs_* entry point below normalizes its path argument with
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

// ---- public API ----

int fs_is_dir(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    struct file *f = find(norm);
    return f != 0 && f->type == FS_TYPE_DIR;
}

int fs_exists(const char *path) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    if (k_strcmp(norm, "/") == 0) return 1;
    return find(norm) != 0;
}

int fs_touch(const char *path) {
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
    persist_record(idx);
    return 1;
}

int fs_mkdir(const char *path) {
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
    persist_record(idx);
    return 1;
}

int fs_write(const char *path, const char *data, int append) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;

    struct file *f = find(norm);
    if (f && f->type == FS_TYPE_DIR) return 0; // can't write to a directory
    if (!f) {
        if (!fs_touch(norm)) return 0;
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
    persist_record((int)(f - files));
    return 1;
}

int fs_delete(const char *path) {
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

const char *fs_read(const char *path, uint32_t *out_size) {
    char norm[FS_PATH_MAX];
    if (!normalize(path, norm)) return 0;
    struct file *f = find(norm);
    if (!f || f->type != FS_TYPE_FILE) return 0;
    if (out_size) *out_size = f->size;
    return f->data;
}

void fs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    char norm[FS_PATH_MAX];
    if (!normalize(dir_path, norm)) return;
    if (!fs_is_dir(dir_path)) return;

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
