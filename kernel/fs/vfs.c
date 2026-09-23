// Implements fs.h's public API (kapi.h's stable filesystem surface --
// unchanged by this file's existence, and by design) by resolving every
// path to a MOUNT and forwarding the call to that mount's backend.
//
// This file used to hold the backend table and the boot policy as
// well. Those moved to kernel/fs/mount.c when `/boot` became a second
// filesystem -- see kernel/mount.h for the mount table's five rules and
// for what Linux and Windows do. The split is by concern: this half is
// "given a path, who answers and what do they get handed", and that
// half is "what is mounted where, and what is allowed to be".
//
// WHAT A BACKEND IS HANDED. Not the path the caller passed: the path
// with its mount point stripped, so a mount at /boot sees /grub/x for
// /boot/grub/x and / for /boot itself. A backend never learns where it
// is mounted, which is what lets one driver serve the root and a
// partition at the same time.
//
// TWO REFUSALS LIVE HERE and both are Unix's. A mutating call on a
// READ-ONLY mount fails rather than being quietly dropped, and an
// operation naming TWO paths (rename, link) that land on different
// mounts is refused rather than half-done -- EXDEV, and the reason
// `cp` exists.
#include "fs.h"
#include "fs_ops.h"
#include "errno.h"   // fs_chmod returns a negative errno
#include "mmap.h"   // imgcache_forget: a rewritten /lib file must not serve stale pages
#include "mount.h"
#include "tmppath.h"
#include "block.h"
#include "ata.h"
#include "kfmt.h"    // klog_printf
#include "klog.h"
#include "string.h"
#include "scheduler.h"
#include "kmutex.h"  // the one filesystem lock -- see FS_OP below
#include "fswatch.h" // a mutation tells whoever watches its path
#include "initcall.h"

// EVERY backend call runs inside ONE LOCK, and a contender SLEEPS.
//
// It was a blanket preemption guard until 2026-09-18, and the reason it
// stopped being one is measured: `tools/latency_under_io.py` put the
// compositor's loaded wake latency at 0.3-0.4 s under the trap gate
// purely because this held preemption off for a whole backend call --
// a process waiting on nothing at all still waited, because the
// machine was not rotating at all. A lock stops only the contexts that
// want the same thing. See docs/blocking-design.md.
//
// The backends are not re-entrant and never were: tfs3.c walks
// directories, inodes and file data through module-level scratch
// buffers (g_blk, g_ptr_blk), and fat32.c walks a FAT chain through a
// per-mount sector buffer. That is fine for a filesystem only one thing
// at a time uses, and this kernel is not that -- the kernel context is
// a scheduler participant and a ring-3 process is preemptible inside a
// syscall, so the WM reading a file and an app reading a file interleave
// at any instruction. The app's read then overwrites the block the WM
// is parsing.
//
// It did not look like a filesystem bug from outside. The WM reported
// files that plainly exist as missing or unreadable, intermittently and
// with no error logged anywhere -- the desktop losing cursor shapes on
// roughly one boot in three under KVM, hidden behind the built-in
// fallback. vfs.c is the one place every caller passes through, so the
// guard goes here rather than being repeated (and eventually forgotten)
// in each backend.
//
// A SECOND MOUNT DOES NOT WEAKEN THIS, and it is why the lock is ONE
// lock rather than one per mount. Two backends have separate state, but
// `mount_enter()` swaps a GLOBAL "which mount is live" pointer and
// tfs3.c's scratch is module-level rather than per mount -- so a
// per-mount lock would protect neither. What would earn a finer lock is
// making that state per mount, which docs/smp-design.md wants anyway.
//
// This is NOT the same thing as the nested-read refusal that guarded
// the old fs_read()'s shared staging buffer -- that call and its buffer
// were deleted on 2026-09-03; this protects every backend's internal
// state for the whole call. Note it does not make a LIST
// CALLBACK safe to call fs_* from -- that is direct recursion, not
// preemption, and the depth counter cannot see the difference.
// THE one lock. A file-scope definition rather than a pointer handed
// around, because there is exactly one filesystem serialisation point
// in this kernel and naming it twice is how a second one appears.
static struct kmutex g_fs_lock;

// Whether a backend call is in flight, and whose. A diagnostic: "the
// filesystem is busy" is otherwise invisible from outside vfs.c, and it
// is what a test uses to prove FS_OP holds the lock for the WHOLE call
// rather than taking and dropping it around the edges.
int fs_lock_held(void)  { return kmutex_held(&g_fs_lock); }
int fs_lock_owner(void) { return kmutex_owner(&g_fs_lock); }
void fs_exclusive_begin(void) { kmutex_lock(&g_fs_lock); }
void fs_exclusive_end(void)   { kmutex_unlock(&g_fs_lock); }

// **THE MOUNT IS RE-CHECKED ONCE THE LOCK IS HELD.** It was resolved
// before the lock, and a caller can SLEEP waiting for the lock -- so an
// unmount can complete in between and zero the entry it is holding. A
// mount gone by then fails the call with 0, which every backend op
// already means as failure; the caller sees a path that no longer
// resolves, which is what it now is.
#define FS_OP(m, expr) ({                  \
    kmutex_lock(&g_fs_lock);               \
    __typeof__(expr) _fs_r = 0;            \
    if ((m)->used) {                       \
        void *_fs_prev = mount_enter(m);   \
        _fs_r = (expr);                    \
        mount_leave(m, _fs_prev);          \
    }                                      \
    kmutex_unlock(&g_fs_lock);             \
    _fs_r;                                 \
})

#define FS_OP_VOID(m, stmt) do {           \
    kmutex_lock(&g_fs_lock);               \
    if ((m)->used) {                       \
        void *_fs_prev = mount_enter(m);   \
        stmt;                              \
        mount_leave(m, _fs_prev);          \
    }                                      \
    kmutex_unlock(&g_fs_lock);             \
} while (0)

// ---- resolution -----------------------------------------------------
//
// One buffer per call, on the stack: FS_PATH_MAX is 64, and a static
// one would be exactly the shared-scratch hazard the preemption guard
// above exists to contain.
// `sub` BORROWS from the `path` the caller passed in (mount.h), so a
// struct resolved is two pointers and must not outlive that argument.
// It held an FS_PATH_MAX array until paths grew to 4096, at which point
// one of these per fs_*() was a quarter of the kernel stack.
struct resolved {
    const struct mount *m;
    const char *sub;
};

static int resolve(const char *path, struct resolved *r) {
    r->m = mount_resolve(path, &r->sub);
    return r->m != NULL;
}

// A mutating call needs a mount that is not read-only. Returns 0 (and
// logs, once per refusal, since a silent no is what a write to /boot
// would otherwise look like) when it is.
static int writable(const struct resolved *r, const char *what, const char *path) {
    if (!(r->m->flags & MNT_RDONLY)) return 1;
    klog_printf(KLOG_ERR "fs: %s refused -- %s is mounted read-only (\"%s\")\n",
                what, r->m->point, path ? path : "");
    return 0;
}

// ---- the API --------------------------------------------------------

static void ensure_layout(void);

void fs_init(void) {
    ata_init(); // the disk comes up once, here -- before any backend is probed
    mount_boot_root();
    ensure_layout();
    // /boot has to EXIST before anything can be mounted over it, which
    // is why this runs after the layout pass and not beside the root
    // mount. Rule 3 in kernel/mount.h.
    mount_boot_auto();
}
INITCALL(fs_init, INIT_FS);

int fs_is_persistent(void) {
    const struct mount *m = mount_root();
    return m ? m->persistent : 0;
}

// THE ROOT'S, not "the filesystem's" -- there are several now. Every
// caller of these predates mounts and means the root: `df`'s summary
// line, the About window, fsck. Per-mount answers come from the
// QUERY_FSINFO provider, which reports one record per mount.
const char *fs_backend_name(void) {
    const struct mount *m = mount_root();
    return (m && m->fs->name) ? m->fs->name : "none";
}

uint32_t fs_capabilities(void) {
    const struct mount *m = mount_root();
    return m ? m->fs->caps : 0;
}

int fs_has(uint32_t cap) {
    return (fs_capabilities() & cap) == cap;
}

// See fs.h. The whole of the difference from fs_format_backend() below
// is that this one disturbs NOTHING: no unmount, no re-probe, no change
// to what is mounted where. A target that would need any of that is
// refused instead, because an installer handed the running root is an
// installer being asked to saw off its own branch.
int fs_format_device(const struct block_device *dev, const char *fstype) {
    if (!dev || !fstype || !fstype[0]) return 0;
    const struct fs_ops *target = mount_backend_named(fstype);
    if (!target || !target->format) return 0;

    // ONLY THE TARGET ITSELF: format() and wipe() run on a SCRATCH
    // state below, so there is no mounted volume for them to repoint.
    // Before per-mount state they wrote into the one set of globals a
    // mounted root was reading -- two crashes that took /bin with them,
    // one of them with a FAT32 target, because mount_wipe_others() runs
    // every OTHER backend's wipe against the same device.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->used || m->dev != dev) continue;
        klog_printf(KLOG_ERR "mkfs: %s is mounted at %s -- refused\n",
                    blk_device_name(dev), m->point);
        return 0;
    }

    // The wipefs rule, same as below and for the same reason: another
    // backend's leftover signature outlives a format and the next probe
    // mounts the corpse.
    mount_wipe_others(target, dev);

    struct fs_scratch sc;
    if (!mount_scratch_begin(target, &sc)) return 0;
    int ok = target->format(dev) ? 1 : 0;
    mount_scratch_end(&sc);
    return ok;
}

int fs_format_backend(const char *name) {
    const struct fs_ops *target = mount_backend_named(name);
    if (!target) return 0;
    if (!blk_present()) return 0;
    const struct block_device *dev = blk_active();

    // Formatting the volume something is mounted from, while it is
    // mounted, is how a live mount ends up describing a filesystem that
    // is no longer there. Everything is dropped and re-probed below,
    // but the ESP at /boot is on a DIFFERENT volume and would survive a
    // root reformat with a stale device -- so it goes first.
    for (int i = mount_count() - 1; i >= 0; i--) {
        const struct mount *m = mount_at(i);
        if (!m || m->point_len == 1) continue;
        const char *why;
        if (!mount_remove(m->point, &why)) {
            klog_printf("fsformat: %s is still mounted (%s)\n", m->point, why);
            return 0;
        }
    }

    // The wipefs rule -- see mount_wipe_others(). Found live: formatting
    // a TFS3 disk as TFS2 left TFS3's backups intact, and the probe
    // mounted the corpse.
    mount_wipe_others(target, dev);

    struct fs_scratch sc;
    if (!mount_scratch_begin(target, &sc)) return 0;
    int formatted = target->format(dev);
    mount_scratch_end(&sc);
    if (!formatted) return 0;

    // Remount through the same probe path a boot takes -- the freshly
    // written superblock is what should claim the disk.
    if (!mount_reprobe_root(target)) return 0;
    // Same layout a boot would leave behind. Without this the disk came
    // back with no /etc and no /tmp until the next reboot, so the very
    // next `timezone` or `font` change silently failed to persist.
    ensure_layout();
    mount_boot_auto();
    return 1;
}

// The directories the rest of the OS assumes exist on whatever is
// mounted. Every mkdir is a no-op if the directory is already there, so
// this is safe to call after every mount, which is the point:
//
//   /etc  the config-file convention (see api/etc_config.h). Anything
//         calling etc_config_set() before this has run writes nothing
//         and reports failure -- a persisted timezone or font size
//         silently stops persisting.
//   /tmp  the one directory POSIX actually mandates by name (it says
//         almost nothing else about layout -- see
//         docs/filesystem-layout.md). Scratch space has to exist on any
//         disk, including one this build never seeded, which is why it
//         is created rather than seeded. Deliberately NOT emptied here:
//         fs_delete() refuses non-empty directories on purpose and
//         there is no recursive delete (see docs/decisions.md), so
//         clearing it needs a real directory walk that nothing has
//         needed yet. It is a MOUNT POINT now -- the `tmpfs` service
//         puts a ramfs over it at boot -- so what is created here is
//         what a machine sees only if that service is removed.
//   /run  RUNTIME state -- init's control file and its status, and a
//         service's stop marker. The FHS's directory for exactly this,
//         and what init's own comment asked for while saying "/tmp is
//         the only such directory here". It stopped being a reasonable
//         stand-in the moment /tmp became a mount point a SERVICE
//         mounts: init's own channel cannot live under a filesystem one
//         of init's services puts there.
//   /var/tmp  scratch that must SURVIVE, and must be real storage.
//         Once /tmp is in RAM the two stop being interchangeable, which
//         is exactly the FHS's distinction and Linux's reason for
//         keeping both. Anything that needs a file to still be there
//         after a reboot, or needs the disk to actually be written,
//         belongs here -- six KTESTs were quietly relying on /tmp for
//         the second of those.
//   /boot  THE MOUNT POINT FOR THE ESP, and it has to exist on the ROOT
//         before anything can be mounted over it -- kernel/mount.h's
//         rule 3, which is Linux's. On a machine with no ESP it stays
//         an empty directory, which is the honest picture: this build's
//         kernel came from somewhere else.
//   /mnt  the conventional place to mount something by hand, empty for
//         the same reason.
//
// This lives beside the mount rather than in kernel_main(), where it
// used to be, because a mount is not only a boot-time event: the
// `fsformat` command reformats and remounts a live disk, and that path
// left the directories missing until the next reboot.
static void ensure_layout(void) {
    fs_mkdir("/etc");
    // THE COMPILED DEFAULTS, not the settings. This pass runs before
    // /etc can be read -- it is what creates /etc -- so it cannot ask
    // where scratch is configured to be. storage_config_init() makes
    // the configured ones once it has read them (INIT_CONFIG, after
    // INIT_FS), which is the second half of the same job.
    fs_mkdir(TMP_DIR_DEFAULT);
    fs_mkdir("/var");
    fs_mkdir(TMP_VARDIR_DEFAULT);
    fs_mkdir("/var/log");
    fs_mkdir(TMP_RUNDIR);
    fs_mkdir("/boot");
    fs_mkdir("/mnt");
    // Where a config FILE declares itself -- one descriptor per file,
    // read by config_files_scan() (api/config_file.h). It has to exist
    // before anything can register, and `config register` on a machine
    // whose /etc/config.d is missing would fail for a reason the user
    // could do nothing about.
    fs_mkdir("/etc/config.d");
}

// Bumped by every mutation below, on SUCCESS only -- a refused write
// changed nothing, and waking a watcher for it would make the counter
// mean "someone tried" rather than "something changed". See
// fs_generation() in api/fs.h for what reads this and why it is global.
static uint64_t g_generation;

uint64_t fs_generation(void) { return g_generation; }

// Everything buffered anywhere on the way to a platter, on EVERY
// mounted volume. Two stages, because they are two different places
// data can be sitting and only one of them used to be emptied:
//
//   1. a driver's software write-back cache (only ATA has one), which
//      holds sectors in RAM;
//   2. the DRIVE's own volatile cache, which a device flush empties.
//
// `sys_sync()` did stage 1 alone and skipped both when the ATA cache
// was absent -- so on AHCI or virtio-blk it asked the disk for nothing
// and reported success. Backend- and driver-agnostic here: the block
// layer knows how to flush whatever is under each mount, and the
// filesystem on top of it never comes into it.
//
// `*wrote_out` is sectors moved out of a SOFTWARE cache, which is 0 on
// a machine that has none and is not a measure of how much work this
// did. Returns 0 if anything failed, and the caller must not treat that
// as cosmetic: it means data is still only in RAM or only in the drive.
// ONE FILE'S DURABILITY, which is what fsync(2) means -- but scoped to
// that file's VOLUME, and the difference is worth stating rather than
// glossing.
//
// There is no page cache here, so nothing is held per FILE: what is
// deferred under `storage.sync = batched` is a journal transaction that
// may carry several files' inode blocks at once, and a device flush is
// a whole-drive operation either way. So this commits the backend
// holding that path and flushes the device under it -- everything
// needed for THIS file to be durable, plus whatever else shares the
// transaction. Narrower than `sync` (other mounts are untouched) and
// wider than POSIX promises.
//
// Per-file granularity would need the write-back page cache
// docs/pagecache-design.md stages, which is precisely why that document
// puts fsync AFTER it.
int fs_sync_path(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    if (r.m->fs->sync && !FS_OP(r.m, r.m->fs->sync())) {
        klog_printf(KLOG_ERR "fs: fsync FAILED -- %s could not commit\n", r.m->point);
        return 0;
    }
    // A mount with no device (ramfs) has nothing to flush and is
    // durable in the only sense it can be.
    if (!r.m->dev) return 1;
    return blkdev_flush(r.m->dev);
}

// The kernel's idle work, for any backend that defers something --
// scheduler_idle() is its one owner (scheduler.c), and this sits beside
// atac_idle() for the same reason: a threshold bounds how MUCH can
// accumulate, only a timer bounds how LONG a machine nobody is touching
// holds it.
//
// Cheap when there is nothing to do, because it runs in every wait loop
// in the kernel: a backend with no `idle` costs one NULL test.
void fs_idle(void) {
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->fs || !m->fs->idle) continue;
        FS_OP_VOID(m, m->fs->idle());
    }
}

int fs_sync(uint32_t *wrote_out) {
    uint32_t wrote = 0, pending = 0;
    int ok = 1;

    // STAGE 0: land whatever a BACKEND is holding back, before either
    // of the stages below. `storage.sync = batched` lets TFS3 keep a
    // journal transaction open across writes, and flushing the device
    // without committing it first would report a durability that had
    // not been reached. Optional per backend -- NULL means nothing is
    // ever deferred, which is true of fat32 and ramfs.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->fs || !m->fs->sync) continue;
        if (!FS_OP(m, m->fs->sync())) {
            klog_printf(KLOG_ERR "fs: sync FAILED -- %s could not commit\n", m->point);
            ok = 0;
        }
    }

    if (ata_cache_active() && !ata_sync(&wrote, &pending)) {
        klog_printf(KLOG_ERR "fs: sync FAILED -- %u sector(s) still in RAM\n", pending);
        if (wrote_out) *wrote_out = wrote;
        return 0;   // a barrier cannot rescue a write that never left
    }

    // Deduped by DEVICE. Two partitions of one disk still cost two
    // flushes -- a partition forwards flush to its parent
    // (block_part.c) and nothing here can see that it did -- which is
    // correct, merely not minimal, and a flush is ~0.7 ms on real
    // hardware rather than free.
    const struct block_device *done[MOUNT_MAX];
    int ndone = 0;
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (!m || !m->dev) continue;    // ramfs has no volume to flush
        int seen = 0;
        for (int j = 0; j < ndone; j++) if (done[j] == m->dev) { seen = 1; break; }
        if (seen) continue;
        if (ndone < MOUNT_MAX) done[ndone++] = m->dev;
        if (!blkdev_flush(m->dev)) {
            klog_printf(KLOG_ERR "fs: sync FAILED -- %s did not flush\n", m->point);
            ok = 0;
        }
    }

    if (wrote_out) *wrote_out = wrote;
    return ok;
}

// Bump on a truthy result, and pass that result straight through, so a
// wrapper stays a one-liner and no call site can bump without also
// returning what the backend said.
static inline int bumped(int ok) {
    if (ok) g_generation++;
    return ok;
}

// ...and tell anyone WATCHING that path or its directory (fswatch.h).
// The generation is for "anything changed"; this is "THIS changed".
static int changed(int ok, const char *path) {
    if (!bumped(ok)) return ok;
    uint64_t self, parent;
    fswatch_hash(path, &self, &parent);
    fswatch_note(self, parent);
    return ok;
}

// Two names, one change: a rename or a link bumps once.
static int changed2(int ok, const char *a, const char *b) {
    if (!changed(ok, a)) return ok;
    uint64_t self, parent;
    fswatch_hash(b, &self, &parent);
    fswatch_note(self, parent);
    return ok;
}

int fs_touch(const char *path) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "touch", path)) return 0;
    return changed(FS_OP(r.m, r.m->fs->touch(r.sub)), path);
}

int fs_write(const char *path, const char *data, int append) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    imgcache_forget(path);
    return changed(FS_OP(r.m, r.m->fs->write(r.sub, data, append)), path);
}

int fs_mkdir(const char *path) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "mkdir", path)) return 0;
    return changed(FS_OP(r.m, r.m->fs->mkdir(r.sub)), path);
}

int fs_delete(const char *path) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "delete", path)) return 0;
    // A MOUNT POINT IS NOT A FILE. Deleting one would remove the
    // directory the mount hangs on and leave a mount pointing at
    // nothing -- Unix's EBUSY.
    for (int i = 0; i < mount_count(); i++) {
        const struct mount *m = mount_at(i);
        if (m && k_strcmp(m->point, path) == 0) {
            klog_printf(KLOG_ERR "fs: refusing to delete \"%s\" -- %s is mounted there\n",
                        path, m->fs->name);
            return 0;
        }
    }
    return changed(FS_OP(r.m, r.m->fs->del(r.sub)), path);
}

uint32_t fs_read_into(const char *path, void *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    uint8_t *dst = (uint8_t *)buf;
    dst[0] = '\0';

    struct resolved r;
    if (!resolve(path, &r)) return 0;

    uint64_t size = FS_OP(r.m, r.m->fs->size(r.sub));
    // Room for the NUL as well, so a text caller can scan the result as
    // a string without a separate length check at every step.
    if (size == 0 || size + 1 > (uint64_t)cap) return 0;

    // A loop rather than one call: fs_read_range() may legitimately
    // return short (see its contract), and treating a short read as the
    // whole file is how a truncated parse gets in.
    uint32_t got = 0;
    while (got < (uint32_t)size) {
        uint32_t n = FS_OP(r.m, r.m->fs->read_range(r.sub, got, dst + got, (uint32_t)size - got));
        if (n == 0) return 0; // EOF-before-size or a real failure; either way, refuse
        got += n;
    }
    dst[got] = '\0';
    return got;
}

uint64_t fs_size(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.m->fs->size(r.sub));
}

uint32_t fs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.m->fs->read_range(r.sub, offset, buf, len));
}

int fs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    imgcache_forget(path);
    return changed(FS_OP(r.m, r.m->fs->write_range(r.sub, offset, buf, len)), path);
}

// A STEP CARRIES NO PATH, so it cannot be resolved. The handle came
// from a backend and has to go back to the same one -- which is what
// this records when begin() succeeded. Two streams from two mounts can
// be in flight at once, so it is per handle rather than a single "the
// last backend that began something".
//
// The wrapper is also what makes a handle SAFE TO STEP TWICE: a
// terminal result clears the slot, so a second step finds no backend
// and fails cleanly rather than handing a freed pointer to a driver.
struct step_handle {
    const struct mount *m;   // NULL = free slot, and = "already finished"
    void *inner;
    uint64_t self, parent;   // the written path's fswatch hashes -- the
                             // path itself is the caller's, and gone
};

static struct step_handle g_steps[8];

static void *step_wrap(const struct mount *m, void *inner) {
    if (!inner) return 0;
    for (unsigned i = 0; i < sizeof g_steps / sizeof g_steps[0]; i++) {
        if (!g_steps[i].m) { g_steps[i].m = m; g_steps[i].inner = inner; return &g_steps[i]; }
    }
    return 0; // more streams in flight than slots -- refused, not misrouted
}

void *fs_write_range_begin(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "write", path)) return 0;
    struct step_handle *h = step_wrap(r.m, FS_OP(r.m, r.m->fs->write_range_begin(r.sub, offset, buf, len)));
    if (h) fswatch_hash(path, &h->self, &h->parent);
    return h;
}

enum fs_step_result fs_write_range_step(void *handle) {
    // A NULL handle means fs_write_range_begin() already failed (or the
    // caller mistakenly stepped a handle twice past its terminal
    // result, which frees it) -- fail cleanly here rather than handing
    // NULL to a backend that assumes a valid handle, same "defend at
    // the dispatch boundary, not in every backend" spirit as the rest
    // of this file.
    if (!handle) return FS_STEP_FAILED;
    struct step_handle *h = handle;
    if (!h->m) return FS_STEP_FAILED;
    enum fs_step_result r = (enum fs_step_result)FS_OP(h->m, h->m->fs->write_range_step(h->inner));
    uint64_t self = h->self, parent = h->parent;
    if (r == FS_STEP_DONE || r == FS_STEP_FAILED) { h->m = 0; h->inner = 0; }
    // Bump once, on completion -- not per step. A streamed write is one
    // change to the filesystem however many slices it took, and bumping
    // per step would wake a watcher repeatedly through a single save.
    // This path is the one Notepad saves through, so without it editing
    // a file in the editor would not be seen by anything watching.
    if (r == FS_STEP_DONE) { g_generation++; fswatch_note(self, parent); }
    return r;
}

void *fs_read_range_begin(const char *path, uint64_t offset, void *buf, uint32_t len) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return step_wrap(r.m, FS_OP(r.m, r.m->fs->read_range_begin(r.sub, offset, buf, len)));
}

enum fs_step_result fs_read_range_step(void *handle, uint32_t *out_total) {
    // Same "defend at the dispatch boundary" reasoning as
    // fs_write_range_step() above.
    if (!handle) {
        if (out_total) *out_total = 0;
        return FS_STEP_FAILED;
    }
    struct step_handle *h = handle;
    if (!h->m) { if (out_total) *out_total = 0; return FS_STEP_FAILED; }
    enum fs_step_result r = (enum fs_step_result)FS_OP(h->m, h->m->fs->read_range_step(h->inner, out_total));
    if (r == FS_STEP_DONE || r == FS_STEP_FAILED) { h->m = 0; h->inner = 0; }
    return r;
}

// TWO PATHS, ONE MOUNT. Unix returns EXDEV for a cross-device rename
// and every tool knows to fall back to copy-then-delete; doing the
// copy here would make one call mean two very different amounts of
// work, and a half-finished one leave a file in both places.
int fs_rename(const char *oldpath, const char *newpath) {
    struct resolved a, b;
    if (!resolve(oldpath, &a) || !resolve(newpath, &b)) return 0;
    if (a.m != b.m) {
        klog_printf(KLOG_ERR "fs: refusing to rename across mounts (\"%s\" -> \"%s\") -- use cp\n",
                    oldpath, newpath);
        return 0;
    }
    if (!writable(&a, "rename", oldpath)) return 0;
    // BOTH names change meaning, and the DESTINATION is the one that
    // matters: an updated /lib file arrives by rename-into-place
    // (tftpd's, dpkg's shape), so hooking only the write paths left the
    // cache serving the file this replaces.
    imgcache_forget(oldpath);
    imgcache_forget(newpath);
    return changed2(FS_OP(a.m, a.m->fs->rename(a.sub, b.sub)), oldpath, newpath);
}

int fs_truncate(const char *path, uint64_t size) {
    struct resolved r;
    if (!resolve(path, &r) || !writable(&r, "truncate", path)) return 0;
    imgcache_forget(path);
    return changed(FS_OP(r.m, r.m->fs->truncate(r.sub, size)), path);
}

int fs_is_dir(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.m->fs->is_dir(r.sub));
}

int fs_exists(const char *path) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.m->fs->exists(r.sub));
}

void fs_list(const char *dir_path, void (*cb)(const char *name, uint32_t size, int is_dir)) {
    struct resolved r;
    if (!resolve(dir_path, &r)) return;
    // The callback runs inside the section too -- it has to, since the
    // walk holds backend state across it. A callback that only records
    // what it is handed (every caller here) is fine; one that called
    // back into fs_* would be re-entering the backend directly, which
    // no amount of preemption control can make safe.
    FS_OP_VOID(r.m, r.m->fs->list(r.sub, cb));
}

int fs_stat(const char *path, struct fs_stat_info *out) {
    struct resolved r;
    if (!resolve(path, &r)) return 0;
    return FS_OP(r.m, r.m->fs->stat(r.sub, out));
}

int fs_chmod(const char *path, uint16_t mode) {
    struct resolved r;
    if (!resolve(path, &r)) return -ENOENT;
    // A backend with nowhere to keep permission bits leaves the slot
    // NULL rather than accepting and discarding them -- see fs_ops.h.
    if (!r.m->fs->chmod) return -ENOTSUP;
    // PERMISSIONS ONLY. The type bits are the filesystem's, and a chmod
    // that could rewrite them would be a corruption primitive.
    return FS_OP(r.m, r.m->fs->chmod(r.sub, (uint16_t)(mode & 07777)));
}

// THE ROOT's usage. `df` reports every mount by walking the mount table
// through QUERY_FSINFO; this stays the root's answer because every
// caller of it means "how full is the disk".
int fs_disk_usage(uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    const struct mount *m = mount_root();
    if (!m) return 0;
    return FS_OP(m, m->fs->disk_usage(out_used_bytes, out_total_bytes));
}

// Per-mount usage, guarded. See api/fs.h -- the point of this function
// existing at all is that the QUERY_FSINFO provider was calling
// `m->fs->disk_usage()` directly, which is one line and skips the
// preemption guard every other backend call in this file takes.
int fs_mount_usage(const void *mount, uint64_t *out_used_bytes, uint64_t *out_total_bytes) {
    const struct mount *m = mount;
    if (!m || !m->fs) return 0;
    return FS_OP(m, m->fs->disk_usage(out_used_bytes, out_total_bytes));
}

int fs_check(int repair, struct fs_check_result *out) {
    const struct mount *m = mount_root();
    if (!m) return 0;
    if (repair && (m->flags & MNT_RDONLY)) return 0;
    return FS_OP(m, m->fs->check(repair, out));
}

int fs_link(const char *existing, const char *newpath) {
    struct resolved a, b;
    if (!resolve(existing, &a) || !resolve(newpath, &b)) return 0;
    if (a.m != b.m) return 0; // a hard link cannot cross a filesystem, anywhere
    if (!writable(&a, "link", newpath)) return 0;
    // Optional op -- the caps bit and this NULL check are the same
    // fact, and caps_are_honest() made sure they can't disagree.
    if (!a.m->fs->link) return 0;
    return changed(FS_OP(a.m, a.m->fs->link(a.sub, b.sub)), newpath);
}
