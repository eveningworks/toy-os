// Implements fs.h's public API (kapi.h's stable filesystem surface --
// unchanged by this file's existence, and by design) by forwarding
// every call to whichever `struct fs_ops` backend is active. This is
// the whole VFS layer: no path parsing, no state of its own beyond
// which backend is active and whether it turned out persistent. See
// fs_ops.h for the interface being dispatched through and why it's
// shaped this way (single active backend, not mount points).
//
// Today there's exactly one backend to choose from -- tfs_ops (tfs.c,
// the original flat/directory filesystem this OS has always had).
// Adding a second filesystem in the future means: write its own
// tfs.c-shaped file exposing a `const struct fs_ops whatever_ops`,
// #include its header below, and either swap which one fs_init()
// assigns to g_fs or pick between them there (e.g. by trying a magic
// number on disk, or a build-time choice) -- nothing else in the
// kernel or apps/ needs to know that happened.
#include "fs.h"
#include "fs_ops.h"
#include "tfs.h"

static const struct fs_ops *g_fs = 0;
static int g_persistent = 0;

void fs_init(void) {
    g_fs = &tfs_ops; // the only backend today -- see this file's top comment
    g_persistent = g_fs->init();
}

int fs_is_persistent(void) {
    return g_persistent;
}

int fs_touch(const char *path) {
    return g_fs->touch(path);
}

int fs_write(const char *path, const char *data, int append) {
    return g_fs->write(path, data, append);
}

int fs_mkdir(const char *path) {
    return g_fs->mkdir(path);
}

int fs_delete(const char *path) {
    return g_fs->del(path);
}

const char *fs_read(const char *path, uint32_t *out_size) {
    return g_fs->read(path, out_size);
}

uint64_t fs_size(const char *path) {
    return g_fs->size(path);
}

uint32_t fs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    return g_fs->read_range(path, offset, buf, len);
}

int fs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return g_fs->write_range(path, offset, buf, len);
}

void *fs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return g_fs->write_range_begin(path, offset, buf, len);
}

enum fs_step_result fs_write_range_step(void *handle) {
    // A NULL handle means fs_write_range_begin() already failed (or the
    // caller mistakenly stepped a handle twice past its terminal
    // result, which frees it) -- fail cleanly here rather than handing
    // NULL to a backend that assumes a valid handle, same "defend at
    // the dispatch boundary, not in every backend" spirit as the rest
    // of this file.
    if (!handle) return FS_STEP_FAILED;
    return (enum fs_step_result)g_fs->write_range_step(handle);
}

void *fs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    return g_fs->read_range_begin(path, offset, buf, len);
}

enum fs_step_result fs_read_range_step(void *handle, uint32_t *out_total) {
    // Same "defend at the dispatch boundary" reasoning as
    // fs_write_range_step() above.
    if (!handle) {
        if (out_total) *out_total = 0;
        return FS_STEP_FAILED;
    }
    return (enum fs_step_result)g_fs->read_range_step(handle, out_total);
}

int fs_is_dir(const char *path) {
    return g_fs->is_dir(path);
}

int fs_exists(const char *path) {
    return g_fs->exists(path);
}

void fs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    g_fs->list(dir_path, cb);
}

int fs_stat(const char *path, struct fs_timestamps *out) {
    return g_fs->stat(path, out);
}

int fs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    return g_fs->disk_usage(out_used_bytes, out_total_bytes);
}

int fs_check(int repair, struct fs_check_result *out) {
    return g_fs->check(repair, out);
}
