// Implements fs.h's public API (kapi.h's stable filesystem surface --
// unchanged by this file's existence, and by design) by forwarding
// every call to whichever `struct fs_ops` backend is active. This is
// the whole VFS layer: no path parsing, no state of its own beyond
// which backend is active and whether it turned out persistent. See
// fs_ops.h for the interface being dispatched through and why it's
// shaped this way (single active backend, not mount points).
//
// Backend SELECTION lives here now, display_probe()-style (see
// kernel/drivers/display/display.c, the pattern this deliberately
// mirrors): fs_init() brings the disk up once, then walks g_backends
// in priority order asking each one's probe() -- detection only, no
// side effects -- and mounts the first backend that recognizes its
// own superblock. A readable-but-unclaimed disk (blank or foreign) is
// formatted with the DEFAULT backend, deliberately and here, not by
// whichever backend happens to run: a backend never formats on its
// own initiative anymore (that used to be tfs_init()'s else-branch).
// An UNREADABLE superblock is different from a foreign one -- no
// formatting happens at all, and the chosen backend's own init()
// degrades to RAM-only; see tfs.c's init comment for the data-loss
// story behind that distinction.
//
// Adding a filesystem = write its tfs.c-shaped file exposing a
// `const struct fs_ops whatever_ops`, #include its header below, and
// add it to g_backends -- nothing else in the kernel or apps/ needs
// to know that happened.
#include "fs.h"
#include "fs_ops.h"
#include "tfs.h"
#include "tfs3.h"
#include "ata.h"
#include "block.h"
#include "kfmt.h"    // klog_printf
#include "multiboot.h" // a live image arrives as a GRUB module
#include "klog.h"
#include "string.h"
#include "scheduler.h" // scheduler_preempt_disable/enable -- see FS_OP below

// Priority order: first probe() == 1 wins. TFS3 goes FIRST when it
// lands (Stage B of the plan) -- a disk carrying either format is
// unambiguous (different magics at different offsets), so order only
// decides which backend gets asked first, not which one wins a
// contested disk. There is no contested disk.
static const struct fs_ops *const g_backends[] = {
    &tfs3_ops,
    &tfs_ops,
};
#define FS_BACKEND_COUNT ((int)(sizeof(g_backends) / sizeof(g_backends[0])))

// The default backend: what a blank/foreign disk gets formatted with,
// and what serves RAM-only boots (no disk at all). Index into
// g_backends. TFS3 since Stage E of the TFS3 plan -- the write path,
// fsck and crash recovery are all live and verified, and
// tools/seed_disk.py applies the same blank-disk policy at build
// time. An existing TFS2 disk keeps mounting as TFS2 via its probe;
// only genuinely blank disks get the new format. (Stage B briefly
// pointed this at tfs3 by ACCIDENT while the write path didn't exist
// -- combined with a stale-object skew that made the honesty check
// refuse tfs2, one boot quietly reformatted the dev image. The
// lesson kept: this constant and the list order above must be read
// together, which is why they sit one comment apart.)
#define FS_DEFAULT_BACKEND 0

static const struct fs_ops *g_fs = 0;
static int g_persistent = 0;

// EVERY backend call runs inside a preemption-free section.
//
// The backends are not re-entrant and never were: tfs3.c walks
// directories, inodes and file data through module-level scratch
// buffers (g_blk, g_ptr_blk). That is fine for a filesystem only one
// thing at a time uses, and this kernel is not that -- the kernel
// context is a scheduler participant and a ring-3 process is
// preemptible inside a syscall, so the WM reading a file and an app
// reading a file interleave at any instruction. The app's read then
// overwrites the block the WM is parsing.
//
// It did not look like a filesystem bug from outside. The WM reported
// files that plainly exist as missing or unreadable, intermittently and
// with no error logged anywhere -- the desktop losing cursor shapes on
// roughly one boot in three under KVM, hidden behind the built-in
// fallback. vfs.c is the one place every caller passes through, so the
// guard goes here rather than being repeated (and eventually forgotten)
// in each backend.
//
// This is NOT the same thing as fs_read()'s nested-read refusal, which
// protects one buffer during one call; this protects every backend's
// internal state for the whole call. Note it does not make a LIST
// CALLBACK safe to call fs_* from -- that is direct recursion, not
// preemption, and the depth counter cannot see the difference.
#define FS_OP(expr) ({                     \
    scheduler_preempt_disable();           \
    __auto_type _fs_r = (expr);            \
    scheduler_preempt_enable();            \
    _fs_r;                                 \
})

#define FS_OP_VOID(stmt) do {              \
    scheduler_preempt_disable();           \
    stmt;                                  \
    scheduler_preempt_enable();            \
} while (0)

// The display.c caps_are_honest() analogue: a capability and its
// optional function pointer are one fact stated twice, and a backend
// whose two statements disagree is refused. Every op in fs_ops is
// required today, so there is nothing optional to cross-check yet --
// this exists (and runs) so the first optional op (planned: link(),
// gated by FS_CAP_HARDLINKS) extends an enforced rule instead of
// introducing an unenforced one.
static int caps_are_honest(const struct fs_ops *fs) {
    if (!fs->name || !fs->probe || !fs->wipe || !fs->format || !fs->init) return 0;
    // Optional ops: the bit and the pointer must agree, both ways --
    // a NULL op behind a declared cap would crash a caller that
    // trusted fs_has(); a real op behind an undeclared cap is a
    // feature callers can never find. display.c's rule, verbatim.
    if (((fs->caps & FS_CAP_HARDLINKS) != 0) != (fs->link != 0)) return 0;
    return 1;
}

// Mounts `fs`: runs its init() and records the outcome. Returns the
// persistent flag init() reported.
static int mount_backend(const struct fs_ops *fs) {
    g_fs = fs;
    // Persistence is the DEVICE's answer, not the backend's: TFS3
    // mounts a RAM image exactly as it mounts a disk and cannot tell
    // them apart, so asking it would report a live session as
    // persistent -- which `df`, `fsck` and the About window would then
    // repeat to the user.
    g_persistent = fs->init() && blk_persistent();
    klog_write("fs: active backend: ");
    klog_write(fs->name);
    klog_write(g_persistent ? " (persistent)\n" : " (RAM-only)\n");
    return g_persistent;
}

// Probe loop + policy, shared by boot (fs_init) and reformat
// (fs_format_backend): pick a backend for the disk that's present.
// `allow_format` gates the blank-disk policy so a reformat path that
// just formatted doesn't recurse into formatting again.
// A LIVE IMAGE: a filesystem the bootloader handed over as a module,
// mounted from RAM through the same backend a disk uses. See
// docs/live-cd-design.md for why it is a filesystem image rather than
// an archive -- one format, one mount path, no unpack step.
//
// WHEN it is used, and the rule is deliberately conservative: only when
// there is no disk, or when the command line asks for it. A real disk
// present and unasked-for is mounted exactly as before. A live session
// that quietly displaced somebody's installed system would be the worst
// thing this feature could do.
static int try_live_module(void) {
    struct multiboot_module_info mod;
    if (!multiboot_get_module(0, &mod) || !mod.found) return 0;
    if (mod.end <= mod.start) return 0;

    const char *cmdline = multiboot_cmdline();
    int forced = cmdline && k_strstr(cmdline, "live");
    if (ata_present() && !forced) return 0;

    if (!blk_ram_register(mod.start, mod.end - mod.start)) return 0;
    klog_printf("fs: live image at 0x%x, %u KiB%s\n", (unsigned)mod.start,
                 (unsigned)((mod.end - mod.start) / 1024),
                 forced ? " (forced by `live` on the command line)" : "");
    return 1;
}

static void probe_and_mount(int allow_format) {
    const struct fs_ops *fallback = g_backends[FS_DEFAULT_BACKEND];

    // The live image gets first refusal, then the disk. Registering a
    // block device is what makes the probe below read from RAM instead
    // of ATA -- the backends are unchanged and never learn which it is.
    int live = try_live_module();
    if (!live) blk_ata_init();

    if (!blk_present()) {
        // No disk: the default backend's init() sets up its RAM-only
        // mode. Nothing to probe.
        mount_backend(fallback);
        return;
    }

    int unreadable = 0;
    for (int i = 0; i < FS_BACKEND_COUNT; i++) {
        const struct fs_ops *fs = g_backends[i];
        if (!caps_are_honest(fs)) {
            klog_write("fs: backend '");
            klog_write(fs->name ? fs->name : "?");
            klog_write("' declares capabilities it doesn't implement -- refusing it\n");
            continue;
        }
        int p = fs->probe();
        if (p == 1) {
            mount_backend(fs);
            return;
        }
        if (p < 0) unreadable = 1;
    }

    if (unreadable) {
        // At least one backend couldn't read its superblock location
        // at all. That is a failing disk, not a blank one -- never
        // format over it. The default backend's init() re-validates
        // and degrades to RAM-only with its own loud refusal.
        klog_write("fs: superblock unreadable -- NOT formatting, refusing to destroy a possibly-good disk\n");
        mount_backend(fallback);
        return;
    }

    // Readable but nobody claimed it: genuinely blank or foreign.
    if (allow_format) {
        klog_write("fs: disk claimed by no filesystem -- formatting with the default (");
        klog_write(fallback->name);
        klog_write(")\n");
        if (!fallback->format()) {
            klog_write("fs: default format failed -- running RAM-only this boot\n");
        }
    }
    mount_backend(fallback);
}

// The directories the rest of the OS assumes exist on whatever is
// mounted. Both mkdirs are no-ops if the directory is already there,
// so this is safe to call after every mount, which is the point:
//
//   /etc  the config-file convention (see api/etc_config.h). Anything
//         calling etc_config_set() before this has run writes nothing
//         and reports failure -- a persisted timezone or font size
//         silently stops persisting.
//   /tmp  the one directory POSIX actually mandates by name (it says
//         almost nothing else about layout -- see
//         docs/filesystem-layout.md). Scratch space has to exist on
//         any disk, including one this build never seeded, which is
//         why it is created rather than seeded. Deliberately NOT
//         emptied here: fs_delete() refuses non-empty directories on
//         purpose and there is no recursive delete (see
//         docs/decisions.md), so clearing it needs a real directory
//         walk that nothing has needed yet.
//
// This lives beside the mount rather than in kernel_main(), where it
// used to be, because a mount is not only a boot-time event: the
// `fsformat` command reformats and remounts a live disk, and that path
// left both directories missing until the next reboot.
static void ensure_layout(void) {
    fs_mkdir("/etc");
    fs_mkdir("/tmp");
    // Where a config FILE declares itself -- one descriptor per file,
    // read by config_files_scan() (api/config_file.h). It has to exist
    // before anything can register, and `config register` on a machine
    // whose /etc/config.d is missing would fail for a reason the user
    // could do nothing about.
    fs_mkdir("/etc/config.d");
}

void fs_init(void) {
    ata_init(); // the disk comes up once, here -- before any backend is probed
    probe_and_mount(1);
    ensure_layout();
}

int fs_is_persistent(void) {
    return g_persistent;
}

const char *fs_backend_name(void) {
    return (g_fs && g_fs->name) ? g_fs->name : "none";
}

uint32_t fs_capabilities(void) {
    return g_fs ? g_fs->caps : 0;
}

int fs_has(uint32_t cap) {
    return (fs_capabilities() & cap) == cap;
}

int fs_format_backend(const char *name) {
    if (!name) return 0;
    const struct fs_ops *target = 0;
    for (int i = 0; i < FS_BACKEND_COUNT; i++) {
        if (k_strcmp(g_backends[i]->name, name) == 0) { target = g_backends[i]; break; }
    }
    if (!target) return 0;
    if (!blk_present()) return 0;
    // The wipefs rule (fs_ops.h's wipe contract): erase every OTHER
    // backend's signatures first, so nothing stale -- a primary the
    // new format doesn't happen to overwrite, or a far-away backup
    // superblock -- can outclaim the freshly written filesystem at
    // the next probe. Found live: formatting a TFS3 disk as TFS2
    // left TFS3's backups intact, and the probe mounted the corpse.
    for (int i = 0; i < FS_BACKEND_COUNT; i++) {
        if (g_backends[i] != target) g_backends[i]->wipe();
    }
    if (!target->format()) return 0;
    // Remount through the same probe path a boot takes -- the freshly
    // written superblock is what should claim the disk. No formatting
    // on this pass: it just happened.
    probe_and_mount(0);
    if (!(g_fs == target && g_persistent)) return 0;
    // Same layout a boot would leave behind. Without this the disk came
    // back with no /etc and no /tmp until the next reboot, so the very
    // next `timezone` or `font` change silently failed to persist.
    ensure_layout();
    return 1;
}

// Bumped by every mutation below, on SUCCESS only -- a refused write
// changed nothing, and waking a watcher for it would make the counter
// mean "someone tried" rather than "something changed". See
// fs_generation() in api/fs.h for what reads this and why it is global.
static uint64_t g_generation;

uint64_t fs_generation(void) { return g_generation; }

// Bump on a truthy result, and pass that result straight through, so a
// wrapper stays a one-liner and no call site can bump without also
// returning what the backend said.
static inline int bumped(int ok) {
    if (ok) g_generation++;
    return ok;
}

int fs_touch(const char *path) {
    return bumped(FS_OP(g_fs->touch(path)));
}

int fs_write(const char *path, const char *data, int append) {
    return bumped(FS_OP(g_fs->write(path, data, append)));
}

int fs_mkdir(const char *path) {
    return bumped(FS_OP(g_fs->mkdir(path)));
}

int fs_delete(const char *path) {
    return bumped(FS_OP(g_fs->del(path)));
}

// A whole-file read is NOT re-entrant, and this refuses the second one
// rather than letting it corrupt the first.
//
// **The mechanism, because it is not obvious and it panicked a
// desktop.** Every backend implements this the same way: free one
// shared staging buffer, allocate a new one the size of the file, then
// do a BLOCKING read into it (`g_read_buf` in tfs3.c and tfs.c). The
// kernel context is a scheduler participant, so the WM can be preempted
// in the middle of that read; a ring-3 process then makes a syscall
// that also reads a file, which frees the buffer the suspended read is
// still writing into and allocates a smaller one. The first read
// resumes and writes past the end of somebody else's allocation.
//
// That is not hypothetical: it was caught by `heap debug on` as a
// red-zone violation on a 96-byte block whose right red-zone held
// `Name=Calc` -- the tail of a .desktop file the WM was loading while
// Control Panel wrote a setting.
//
// Refusing is the honest answer here rather than queueing: a caller
// already has to handle NULL (a missing file returns it), and the
// alternative -- one buffer per caller -- is a different API. **The
// cost to know about:** a refusal is indistinguishable from "no such
// file" at the call site, so it is logged, and a caller that reports
// "missing" may now be reporting "busy". See docs/decisions.md.
static int g_read_in_flight;

const char *fs_read(const char *path, uint32_t *out_size) {
    if (g_read_in_flight) {
        klog_printf("fs: refusing a nested whole-file read of \"%s\" "
                     "(one is already in flight)\n", path ? path : "(null)");
        if (out_size) *out_size = 0;
        return 0;
    }
    g_read_in_flight = 1;
    const char *r = FS_OP(g_fs->read(path, out_size));
    g_read_in_flight = 0;
    return r;
}

uint32_t fs_read_into(const char *path, void *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    uint8_t *dst = (uint8_t *)buf;
    dst[0] = '\0';

    uint64_t size = FS_OP(g_fs->size(path));
    // Room for the NUL as well, so a text caller can scan the result as
    // a string without a separate length check at every step.
    if (size == 0 || size + 1 > (uint64_t)cap) return 0;

    // A loop rather than one call: fs_read_range() may legitimately
    // return short (see its contract), and treating a short read as the
    // whole file is how a truncated parse gets in.
    uint32_t got = 0;
    while (got < (uint32_t)size) {
        uint32_t n = FS_OP(g_fs->read_range(path, got, dst + got, (uint32_t)size - got));
        if (n == 0) return 0; // EOF-before-size or a real failure; either way, refuse
        got += n;
    }
    dst[got] = '\0';
    return got;
}

uint64_t fs_size(const char *path) {
    return FS_OP(g_fs->size(path));
}

uint32_t fs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    return FS_OP(g_fs->read_range(path, offset, buf, len));
}

int fs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return bumped(FS_OP(g_fs->write_range(path, offset, buf, len)));
}

void *fs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    return FS_OP(g_fs->write_range_begin(path, offset, buf, len));
}

enum fs_step_result fs_write_range_step(void *handle) {
    // A NULL handle means fs_write_range_begin() already failed (or the
    // caller mistakenly stepped a handle twice past its terminal
    // result, which frees it) -- fail cleanly here rather than handing
    // NULL to a backend that assumes a valid handle, same "defend at
    // the dispatch boundary, not in every backend" spirit as the rest
    // of this file.
    if (!handle) return FS_STEP_FAILED;
    enum fs_step_result r = (enum fs_step_result)FS_OP(g_fs->write_range_step(handle));
    // Bump once, on completion -- not per step. A streamed write is one
    // change to the filesystem however many slices it took, and bumping
    // per step would wake a watcher repeatedly through a single save.
    // This path is the one Notepad saves through, so without it editing
    // a file in the editor would not be seen by anything watching.
    if (r == FS_STEP_DONE) g_generation++;
    return r;
}

void *fs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    return FS_OP(g_fs->read_range_begin(path, offset, buf, len));
}

enum fs_step_result fs_read_range_step(void *handle, uint32_t *out_total) {
    // Same "defend at the dispatch boundary" reasoning as
    // fs_write_range_step() above.
    if (!handle) {
        if (out_total) *out_total = 0;
        return FS_STEP_FAILED;
    }
    return (enum fs_step_result)FS_OP(g_fs->read_range_step(handle, out_total));
}

int fs_rename(const char *oldpath, const char *newpath) {
    return bumped(FS_OP(g_fs->rename(oldpath, newpath)));
}

int fs_truncate(const char *path, uint64_t size) {
    return bumped(FS_OP(g_fs->truncate(path, size)));
}

int fs_is_dir(const char *path) {
    return FS_OP(g_fs->is_dir(path));
}

int fs_exists(const char *path) {
    return FS_OP(g_fs->exists(path));
}

void fs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    // The callback runs inside the section too -- it has to, since the
    // walk holds backend state across it. A callback that only records
    // what it is handed (every caller here) is fine; one that called
    // back into fs_* would be re-entering the backend directly, which
    // no amount of preemption control can make safe.
    FS_OP_VOID(g_fs->list(dir_path, cb));
}

int fs_stat(const char *path, struct fs_stat_info *out) {
    return FS_OP(g_fs->stat(path, out));
}

int fs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    return FS_OP(g_fs->disk_usage(out_used_bytes, out_total_bytes));
}

int fs_check(int repair, struct fs_check_result *out) {
    return FS_OP(g_fs->check(repair, out));
}

int fs_link(const char *existing, const char *newpath) {
    // Optional op -- the caps bit and this NULL check are the same
    // fact, and caps_are_honest() made sure they can't disagree.
    if (!g_fs->link) return 0;
    return bumped(FS_OP(g_fs->link(existing, newpath)));
}
