// The mount table, and the boot-time policy that fills it.
//
// This was the top half of vfs.c until `/boot` needed to be a second
// filesystem. The split is by concern, the same call `userland/wm/`
// made: vfs.c implements api/fs.h by dispatching a path to a backend,
// and this file decides which backends exist, where they are attached,
// and what is allowed to attach where. Neither half wants to be read
// while working on the other.
//
// See kernel/mount.h for the five rules this enforces and for what
// Linux and Windows do. Everything here is about not destroying data:
// a drive's root comes from a PARTITION, never a whole-disk volume;
// nothing is auto-formatted; and the ESP -- which holds the bootloader
// that started this kernel -- is mounted READ-ONLY unless somebody
// deliberately says otherwise.
#include "mount.h"
#include "block.h"
#include "tfs3.h"
#include "fat32.h"
#include "ramfs.h"
#include "ata.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "multiboot.h"
#include "partition.h"
#include "syscalls.h" // fd_desc[] -- the open-file check in mount_remove()
#include "scheduler.h" // the drain's park and wake
#include "barrier.h"   // cpu_relax()

// driver-none: re-registers a disk a driver already found

// Priority order: first probe() == 1 wins.
//
// Order decides which backend is ASKED first, not which one wins: two
// formats' magics live at different offsets, so a volume is claimed by
// at most one probe. There is no contested volume.
static const struct fs_ops *const g_backends[] = {
    &tfs3_ops,
    &fat32_ops,
};
#define FS_BACKEND_COUNT ((int)(sizeof(g_backends) / sizeof(g_backends[0])))

// RAMFS IS DELIBERATELY NOT IN THAT TABLE, and it is not an oversight.
// g_backends is the list of ON-DISK backends: things a probe can find
// on a volume, things `fsformat` can write to one. ramfs is neither --
// it is chosen by the policy below when there is no usable volume at
// all, and it is reached through this pointer instead.
//
// Being in the table would be actively dangerous, which is the part
// worth keeping: fs_format_backend() wipes every OTHER backend's
// signatures before formatting with the named one, so `fsformat ramfs`
// would erase TFS3's superblock from a perfectly good disk and THEN
// fail its own persistence check. Out of the table, that command finds
// no such backend and refuses before touching anything.
static const struct fs_ops *const g_ram_backend = &ramfs_ops;

static struct mount g_mounts[MOUNT_MAX];

// ---- the table -----------------------------------------------------

int mount_count(void) {
    int n = 0;
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].used) n++;
    return n;
}

const struct mount *mount_at(int index) {
    int n = 0;
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!g_mounts[i].used) continue;
        if (n == index) return &g_mounts[i];
        n++;
    }
    return NULL;
}

const struct mount *mount_root(void) {
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && g_mounts[i].point_len == 1) return &g_mounts[i];
    }
    return NULL;
}

// Does `path` fall under mount point `point`? RULE 1: the prefix must
// end at a COMPONENT BOUNDARY. A bare k_strncmp() would hand
// `/bootloader.cfg` to the mount at `/boot`, which is one filesystem
// serving another's files with nothing to notice it.
static int under(const char *point, int plen, const char *path) {
    if (plen == 1) return 1; // the root claims everything nothing else does
    if (k_strncmp(path, point, (uint32_t)plen) != 0) return 0;
    char next = path[plen];
    return next == '\0' || next == '/';
}

const struct mount *mount_resolve(const char *path, const char **out_sub) {
    if (!path) path = "/";
    const struct mount *best = NULL;
    for (int i = 0; i < MOUNT_MAX; i++) {
        const struct mount *m = &g_mounts[i];
        if (!m->used) continue;
        if (!under(m->point, m->point_len, path)) continue;
        // LONGEST prefix, so a mount at /boot/grub would outrank one at
        // /boot -- nesting works without being a special case.
        if (!best || m->point_len > best->point_len) best = m;
    }
    if (!best) return NULL;

    if (out_sub) {
        // RULE 2: the backend is handed a root-relative path and never
        // learns where it is mounted.
        //
        // **THE RESULT POINTS INTO `path`** -- stripping a mount point
        // leaves a SUFFIX, so there is nothing to copy and no buffer to
        // size. It used to copy into the caller's array, which put an
        // FS_PATH_MAX local in every fs_*() in vfs.c; at 4096 that was
        // a quarter of a kernel stack per call. The borrow is only
        // valid while `path` is.
        const char *rest = (best->point_len == 1) ? path : path + best->point_len;
        *out_sub = (rest[0] == '\0') ? "/" : rest; // "" means the mount point itself
    }
    return best;
}

// ---- mounting ------------------------------------------------------

// ---- the locks -------------------------------------------------------

// The lock is mutable state in an otherwise read-mostly table; callers
// hold `const struct mount *` from mount_resolve().
void mount_lock(const struct mount *m)   { kmutex_lock(&((struct mount *)m)->lock); }
void mount_unlock(const struct mount *m) { kmutex_unlock(&((struct mount *)m)->lock); }
int mount_trylock(const struct mount *m) { return kmutex_trylock(&((struct mount *)m)->lock); }

static int depth_of(const struct mount *m) {
    if (!m->used) return FS_PATH_MAX;           // empty slots last
    if (m->point_len == 1) return 0;            // the root
    int d = 0;
    for (int i = 0; i < m->point_len; i++) if (m->point[i] == '/') d++;
    return d;
}

// EVERY SLOT, used or not, so the set released is the set taken even if
// the table changes in between -- which is what a mount under exclusion
// does. Ordered parent before child (see mount.h); a nested call takes
// each again, which the recursion makes free.
//
// g_excl FIRST, so the order is computed from a table no other exclusive
// holder is changing -- the table only changes under exclusion.
static struct kmutex g_excl;
static void drain(struct mount *m);

static void exclusive_take(void);
static void exclusive_release(void);

// AND NO PER-OBJECT LOCK HELD: an op that took an inode lock and dropped
// the volume lock for its I/O is still mid-call, waiting to retake it --
// so holding every mount lock would leave it stuck and us facing its
// inode. Take, check, and if anything is held let go and wait for it.
// Rare (partition writes, the legacy `run`, mount changes), so the retry
// costs nothing that matters.
//
// Counts OTHER callers' locks (the backend's locks_held() says so): a
// nested exclusion inside an op holding its own would otherwise wait on
// itself forever. And a nested call skips the wait altogether -- the
// outer one already did it.
static char g_excl_wait;   // an address to park on; mount_locks_released() wakes it

void mount_locks_released(void) { scheduler_wake(&g_excl_wait, 0); }

void fs_exclusive_begin(void) {
    int me = scheduler_current_pid();
    int nested = kmutex_held(&g_excl) && kmutex_owner(&g_excl) == me;
    kmutex_lock(&g_excl);
    for (;;) {
        scheduler_wait_arm(&g_excl_wait);
        exclusive_take();
        int held = 0;
        for (int i = 0; !nested && i < MOUNT_MAX; i++) {
            struct mount *m = &g_mounts[i];
            if (m->used && m->fs->locks_held && m->fs->locks_held(m->state)) held = 1;
        }
        if (!held) { scheduler_wait_disarm(); return; }
        exclusive_release();
        // A release happens under a mount lock we no longer hold, i.e.
        // after the arm above, so it cannot be missed.
        if (!scheduler_block_kernel(&g_excl_wait, SCHED_WAIT_LOCK)) {
            scheduler_wait_disarm();
            cpu_relax();
        }
    }
}

static void exclusive_take(void) {
    int order[MOUNT_MAX];
    for (int i = 0; i < MOUNT_MAX; i++) order[i] = i;
    for (int i = 1; i < MOUNT_MAX; i++) {
        int v = order[i], j = i;
        while (j > 0 && depth_of(&g_mounts[order[j - 1]]) > depth_of(&g_mounts[v])) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    for (int i = 0; i < MOUNT_MAX; i++) kmutex_lock(&g_mounts[order[i]].lock);
    // AND NOTHING AT THE DISK: a call that dropped its lock for device
    // I/O is still in the driver (mount_io_begin()).
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].used) drain(&g_mounts[i]);
}

static void exclusive_release(void) {
    for (int i = MOUNT_MAX - 1; i >= 0; i--) kmutex_unlock(&g_mounts[i].lock);
}

// ---- device I/O without the lock ----------------------------------------

static int can_drop(struct mount *m);

static struct mount *mount_of_state(void *st) {
    if (!st) return NULL;
    for (int i = 0; i < MOUNT_MAX; i++)
        if (g_mounts[i].used && g_mounts[i].state == st) return &g_mounts[i];
    return NULL;   // a scratch state (probe, format, a test): no mount to drop
}

// Wait until `m` has no open gap. The caller holds m's lock, so no new
// gap can open; the ones in flight need only the device to finish.
static void drain(struct mount *m) {
    for (;;) {
        scheduler_wait_arm(&m->io_gaps);
        if (!__atomic_load_n(&m->io_gaps, __ATOMIC_ACQUIRE)) {
            scheduler_wait_disarm();
            return;
        }
        if (!scheduler_block_kernel(&m->io_gaps, SCHED_WAIT_LOCK)) {
            scheduler_wait_disarm();
            cpu_relax();   // no slot to park in: spin, preemptible
        }
    }
}

void mount_io_drain(void *st) {
    struct mount *m = mount_of_state(st);
    if (m) drain(m);
}

int mount_io_begin(void *st, struct mount_io *g) {
    struct mount *m = mount_of_state(st);
    g->m = NULL;
    if (!m) return 0;
    // EXACTLY ONCE, and ours: a nested hold would not really release it,
    // and an exclusive holder has promised the disk is quiet.
    if (!can_drop(m)) return 0;
    g->m = m;
    g->gen = m->gen;
    __atomic_add_fetch(&m->io_gaps, 1, __ATOMIC_ACQ_REL);
    kmutex_unlock(&m->lock);
    return 1;
}

// Can this caller drop m's lock at all? Held exactly once, by it, and
// not under exclusion -- mount_io_begin()'s test.
static int can_drop(struct mount *m) {
    int me = scheduler_current_pid();
    if (m->lock.depth != 1 || !m->lock.owned || m->lock.owner != me || m->lock.handed)
        return 0;
    return !(kmutex_held(&g_excl) && kmutex_owner(&g_excl) == me);
}

int mount_wait(void *st, const void *chan) {
    struct mount *m = mount_of_state(st);
    if (!m || !can_drop(m)) return 0;
    uint32_t gen = m->gen;
    // ARMED BEFORE THE UNLOCK: the release that ends this wait happens
    // under the lock, i.e. after this point, so it cannot be missed.
    scheduler_wait_arm(chan);
    kmutex_unlock(&m->lock);
    if (!scheduler_block_kernel(chan, SCHED_WAIT_LOCK)) {
        scheduler_wait_disarm();
        cpu_relax();          // no slot to park in: retry, preemptibly
    }
    kmutex_lock(&m->lock);
    return (m->used && m->gen == gen) ? 1 : -1;
}

int mount_can_wait(void *st) {
    struct mount *m = mount_of_state(st);
    return m && can_drop(m);
}

void mount_op_restart(void *st) {
    struct mount *m = mount_of_state(st);
    if (m) m->restart = 1;
}

int mount_op_depth(void *st) {
    struct mount *m = mount_of_state(st);
    return m ? m->lock.depth : 0;
}

int mount_io_end(struct mount_io *g) {
    struct mount *m = g->m;
    if (__atomic_sub_fetch(&m->io_gaps, 1, __ATOMIC_ACQ_REL) == 0)
        scheduler_wake(&m->io_gaps, 0);
    kmutex_lock(&m->lock);
    return m->used && m->gen == g->gen;
}

void fs_exclusive_end(void) {
    exclusive_release();
    kmutex_unlock(&g_excl);
}

// Empty a slot. NOT a memset: the lock may have callers queued on it,
// and `gen` is how they find out the mount they resolved has gone.
static void slot_clear(struct mount *m) {
    struct kmutex lock = m->lock;
    uint32_t gen = m->gen + 1;
    k_memset(m, 0, sizeof *m);
    m->lock = lock;
    m->gen = gen;
}

int fs_lock_held_at(const char *path) {
    const struct mount *m = mount_resolve(path, 0);
    return m ? kmutex_held(&m->lock) : 0;
}

int fs_lock_owner_at(const char *path) {
    const struct mount *m = mount_resolve(path, 0);
    return m ? kmutex_owner(&m->lock) : 0;
}

// ---- per-mount backend state ---------------------------------------

// **A SCRATCH OPERATION HOLDS THE FILESYSTEM LOCK**, like any backend
// call: its state is private, but a backend's per-call scratch buffers
// (and tfs3's journal staging) are module-level until stage 2 of
// docs/fslock-design.md. Recursive, so a probe run from inside an
// FS_OP nests.
int mount_scratch_begin(const struct fs_ops *fs, struct fs_scratch *sc) {
    sc->fs = fs;
    sc->st = NULL;
    if (!fs->state_alloc) return 1;   // a backend with no state needs none
    void *st = fs->state_alloc();
    if (!st) return 0;
    fs_exclusive_begin();             // released by mount_scratch_end()
    sc->st = st;
    return 1;
}

void mount_scratch_end(struct fs_scratch *sc) {
    if (!sc->st) return;   // a backend with no state: no lock was taken
    sc->fs->state_free(sc->st);
    sc->st = NULL;
    fs_exclusive_end();
}

// Is a backend already mounted somewhere, and how many times? A
// backend whose state is module-level statics may be mounted once
// (fs_ops.max_mounts) -- and the failure of ignoring that is silent
// rather than loud, which is why it is checked here and not discovered:
// a second mount would re-point the one set of statics and leave the
// FIRST mount reading the second one's volume.
static int mounts_of(const struct fs_ops *fs) {
    int n = 0;
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && g_mounts[i].fs == fs) n++;
    }
    return n;
}

static struct mount *free_slot(void) {
    for (int i = 0; i < MOUNT_MAX; i++) if (!g_mounts[i].used) return &g_mounts[i];
    return NULL;
}

static struct mount *find_point(const char *point) {
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (g_mounts[i].used && k_strcmp(g_mounts[i].point, point) == 0) return &g_mounts[i];
    }
    return NULL;
}

// The display.c caps_are_honest() analogue: a capability and its
// optional function pointer are one fact stated twice, and a backend
// whose two statements disagree is refused.
static int caps_are_honest(const struct fs_ops *fs) {
    if (!fs->name || !fs->probe || !fs->wipe || !fs->format || !fs->init) return 0;
    if (fs->max_mounts < 1) return 0;
    // Optional ops: the bit and the pointer must agree, both ways -- a
    // NULL op behind a declared cap would crash a caller that trusted
    // fs_has(); a real op behind an undeclared cap is a feature callers
    // can never find. display.c's rule, verbatim.
    if (((fs->caps & FS_CAP_HARDLINKS) != 0) != (fs->link != 0)) return 0;
    if (((fs->caps & FS_CAP_REPLACE) != 0) != (fs->rename_replace != 0)) return 0;
    // alloc and free are one fact stated twice.
    int st_ops = (fs->state_alloc != 0) + (fs->state_free != 0);
    if (st_ops == 1) return 0;
    // And a backend cannot promise a second mount without them: its
    // volume would be one set of statics read by two mounts, which is
    // the silent corruption max_mounts exists to refuse.
    if (fs->max_mounts > 1 && st_ops != 2) return 0;
    return 1;
}

// Fills a slot and announces it. `r` is what init() returned. `used`
// goes LAST: mount_resolve() reads the table without a lock and skips a
// slot that is not used, so it never matches a half-copied point.
static void record(struct mount *slot, const struct fs_ops *fs,
                   const struct block_device *dev, const char *point,
                   unsigned flags, int r) {
    k_strlcpy(slot->point, point, sizeof slot->point);
    slot->point_len = (int)k_strlen(slot->point);
    slot->fs = fs;
    slot->dev = dev;
    slot->flags = flags;
    // Persistence is the DEVICE's answer as well as the backend's:
    // TFS3 mounts a RAM image exactly as it mounts a disk and cannot
    // tell them apart, so asking it alone would report a live session
    // as persistent -- which `df`, `fsck` and the About window would
    // then repeat to the user. A backend that says 0 (ramfs) is not
    // persistent whatever the device says.
    slot->persistent = (r == 1) && dev && dev->persistent;
    __atomic_store_n(&slot->used, 1, __ATOMIC_RELEASE);
    klog_printf("fs: %s mounted at %s on %s%s%s\n", fs->name, slot->point,
                dev ? dev->name : "(no device)",
                (flags & MNT_RDONLY) ? ", read-only" : "",
                slot->persistent ? "" : " (not persistent -- files vanish on reboot)");
}

int mount_add(const struct block_device *dev, const char *fstype,
              const char *point, unsigned flags, uint64_t size_bytes,
              const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = "";

    if (!point || point[0] != '/') { *why = "mount point must be an absolute path"; return 0; }
    int plen = (int)k_strlen(point);
    if (plen >= FS_PATH_MAX) { *why = "mount point too long"; return 0; }
    if (plen > 1 && point[plen - 1] == '/') { *why = "mount point must not end in '/'"; return 0; }

    if (find_point(point)) { *why = "something is already mounted there"; return 0; }

    // RULE 3: the point must exist and be a directory on whatever
    // currently answers for it. Linux's rule -- mounting onto nothing
    // would create a path that exists only while mounted, which no
    // `ls` of the parent could show.
    if (plen > 1) {
        if (!mount_root()) { *why = "nothing is mounted to mount onto"; return 0; }
        if (!fs_is_dir(point)) { *why = "mount point does not exist, or is not a directory"; return 0; }
    }

    if (dev) {
        for (int i = 0; i < MOUNT_MAX; i++) {
            if (g_mounts[i].used && g_mounts[i].dev == dev) {
                *why = "that volume is already mounted";
                return 0;
            }
        }
    }

    struct mount *slot = free_slot();
    if (!slot) { *why = "the mount table is full"; return 0; }

    // A named type is asked directly; an unnamed one is probed, which
    // is what `mount 2 /boot` does. ramfs is reachable BY NAME only --
    // it claims nothing, so a probe would never find it, and that is
    // what makes `mount ramfs /mnt` a scratch filesystem rather than a
    // thing that happens to a disk.
    const struct fs_ops *chosen = NULL;
    const struct fs_ops *at_limit = NULL;   // skipped for its mount limit
    if (fstype && fstype[0]) {
        if (k_strcmp(fstype, g_ram_backend->name) == 0) {
            chosen = g_ram_backend;
            dev = NULL; // it has no volume, whatever was named
        } else {
            for (int i = 0; i < FS_BACKEND_COUNT; i++) {
                if (k_strcmp(g_backends[i]->name, fstype) == 0) { chosen = g_backends[i]; break; }
            }
            if (!chosen) { *why = "no such filesystem type"; return 0; }
            if (!dev) { *why = "that filesystem needs a volume"; return 0; }
            struct fs_scratch sc;
            if (!mount_scratch_begin(chosen, &sc)) { *why = "out of memory"; return 0; }
            int claimed = chosen->probe(sc.st, dev);
            mount_scratch_end(&sc);
            if (claimed != 1) { *why = "no such filesystem on that volume"; return 0; }
        }
    } else {
        if (!dev) { *why = "no volume, and no filesystem type named"; return 0; }
        for (int i = 0; i < FS_BACKEND_COUNT; i++) {
            const struct fs_ops *fs = g_backends[i];
            if (!fs->volume_relative || !caps_are_honest(fs)) continue;
            // A BACKEND AT ITS MOUNT LIMIT IS NOT ASKED. It could not be
            // mounted anyway, and probing it is what turned a
            // side-effect in one probe into a silently empty root.
            if (mounts_of(fs) >= fs->max_mounts) { at_limit = fs; continue; }
            struct fs_scratch sc;
            if (!mount_scratch_begin(fs, &sc)) continue;
            int claimed = fs->probe(sc.st, dev);
            mount_scratch_end(&sc);
            if (claimed == 1) { chosen = fs; break; }
        }
        // A BACKEND SKIPPED FOR ITS LIMIT IS A DIFFERENT ANSWER FROM
        // "nothing recognises this". Saying the first when the second
        // was true sent a session hunting a bad format for a volume that
        // was perfectly good -- the backend was simply already mounted.
        // We cannot know it WOULD have claimed the volume without
        // probing it, which is exactly what the skip exists to avoid, so
        // the message says both halves rather than guessing.
        if (!chosen && at_limit) {
            *why = "nothing else recognises it, and the filesystem that might "
                   "is already mounted (one at a time)";
            return 0;
        }
        if (!chosen) { *why = "nothing recognises the filesystem on that volume"; return 0; }
    }

    if (!caps_are_honest(chosen)) { *why = "that backend's capabilities are inconsistent"; return 0; }
    if (mounts_of(chosen) >= chosen->max_mounts) {
        *why = "that filesystem can only be mounted once at a time";
        return 0;
    }

    void *state = NULL;
    if (chosen->state_alloc) {
        state = chosen->state_alloc();
        if (!state) { *why = "out of memory"; return 0; }
    }

    fs_exclusive_begin();   // a backend call -- see mount_scratch_begin()
    int r = chosen->init(state, dev, size_bytes);
    fs_exclusive_end();
    if (r < 0) {
        if (state) chosen->state_free(state);
        *why = "the filesystem would not mount";
        return 0;
    }

    slot->state = state;
    record(slot, chosen, dev, point, flags, r);
    return 1;
}

// Is any process holding an open file under this mount? `struct
// open_file` already records a FD_FILE's absolute path, so this needs
// no new bookkeeping -- and bookkeeping that exists only to answer one
// question is the kind that drifts out of step with what it counts.
static int has_open_files(const struct mount *m) {
    for (int i = 0; i < fd_desc_count(); i++) {
        const struct open_file *f = fd_desc_at(i);
        if (!f) continue;
        if (f->refs <= 0 || f->ops != &file_fd_ops) continue;
        if (under(m->point, m->point_len, f->file.name)) return 1;
    }
    return 0;
}

static int remove_locked(const char *point, const char **why);

int mount_remove(const char *point, const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = "";

    if (!point) { *why = "no mount point given"; return 0; }
    // HELD FOR THE WHOLE REMOVAL, so no backend call is mid-flight on
    // this mount while it is torn down; one that resolved it earlier
    // finds it gone once it gets the lock (vfs.c's FS_OP).
    fs_exclusive_begin();
    int ok = remove_locked(point, why);
    fs_exclusive_end();
    return ok;
}

static int remove_locked(const char *point, const char **why) {
    struct mount *m = find_point(point);
    if (!m) { *why = "nothing is mounted there"; return 0; }
    if (m->point_len == 1) { *why = "the root filesystem cannot be unmounted"; return 0; }

    // Something mounted UNDER this one holds it in place, exactly as a
    // process's open file does. Unmount the deeper one first.
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!g_mounts[i].used || &g_mounts[i] == m) continue;
        if (under(m->point, m->point_len, g_mounts[i].point)) {
            *why = "another filesystem is mounted underneath it";
            return 0;
        }
    }

    if (has_open_files(m)) { *why = "a file on it is still open"; return 0; }

    // Flush before forgetting the mount: a write-back cache's failure
    // surfaces at the flush, and after this the volume has no owner to
    // report it to.
    if (m->dev) blkdev_flush(m->dev);
    if (m->fs->umount) m->fs->umount(m->state, m->dev);
    // AFTER umount(), which is the last thing that may write: state_free
    // drops the caches this mount is holding, and a sync into a freed
    // state is the one ordering that does not survive being got wrong.
    if (m->state) m->fs->state_free(m->state);

    klog_printf("fs: %s unmounted from %s\n", m->fs->name, m->point);
    slot_clear(m);
    return 1;
}

// ---- boot policy ---------------------------------------------------

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
//
// WHO ASKS THE "IS THERE A DISK" QUESTION IS THE CALLER'S JOB, and that
// is the fix: this used to ask `ata_present()` itself, which is the
// LEGACY IDE probe. On a machine whose only disk is AHCI or virtio it
// answered "no disk" and a live boot displaced the real one anyway;
// on a machine presenting both, the opposite. The predicate that was
// meant is blk_root_present(), and it cannot be read here -- it is only true
// after the disk drivers have run, which a FORCED live session must
// happen before. So the caller asks, in that order.
// THE IMAGE ON THE MEDIA IS GZIPPED AND THE KERNEL NEVER SEES THAT.
// GRUB's gzio decompresses any file it reads whose CONTENT starts with
// the gzip magic -- measured, and by content rather than by name: an
// image called `live.img` with no extension at all still arrives here
// inflated. So the ISO holds ~25 MiB where this holds ~86 MiB, and there
// is nothing for this function to unpack.
//
// That is also the better arrangement for MEMORY. GRUB decompresses into
// the buffer it was going to allocate anyway, so the peak is one copy; a
// kernel-side unpack would hold the compressed module and the inflated
// image at once. An in-kernel decompressor was written for this and
// deleted unrun (see docs/decisions/build.md).
static int try_live_module(int forced) {
    struct multiboot_module_info mod;
    if (!multiboot_get_module(0, &mod) || !mod.found) return 0;
    if (mod.end <= mod.start) return 0;

    if (!blk_ram_register(mod.start, mod.end - mod.start)) return 0;
    klog_printf("fs: live image at 0x%x, %u KiB%s\n", (unsigned)mod.start,
                 (unsigned)((mod.end - mod.start) / 1024),
                 forced ? " (forced by `live` on the command line)" : "");
    return 1;
}

// The boot disk's partition table, read once and kept. try_partitions()
// needs it, and so does mount_partition_device() when `/bin/mount`
// names a partition by number long after boot.
//
// STATIC, not a stack local: struct partition_table is ~1.5 KB against
// a 1 KB kernel frame budget -- the same call partition_query.c makes,
// for the same reason.
static struct partition_table g_tbl;
static int g_tbl_valid;

static int read_table(void) {
    if (g_tbl_valid) return g_tbl.kind != PART_TABLE_NONE;
    if (!partition_read_table(&g_tbl)) return 0;
    g_tbl_valid = 1;
    return g_tbl.kind != PART_TABLE_NONE;
}

static int entry_window_of(const struct partition_table *tbl,
                           const struct partition_entry *pe,
                           uint64_t *base, uint64_t *count);

// One entry's window on the disk, in sectors. 0 if the entry cannot be
// used (an unusable GPT range, or a protective MBR slot).
static int entry_window(const struct partition_entry *pe, uint64_t *base, uint64_t *count) {
    return entry_window_of(&g_tbl, pe, base, count);
}

// The same, against a table the caller supplies -- name_all_partitions()
// walks a disk that is not the active one and must not read g_tbl, which
// caches the ROOT disk's table.
static int entry_window_of(const struct partition_table *tbl,
                           const struct partition_entry *pe,
                           uint64_t *base, uint64_t *count) {
    if (tbl->kind == PART_TABLE_GPT) {
        // GPT's range is INCLUSIVE at both ends, so the count is
        // end - start + 1. Getting that off by one costs the last
        // sector of every volume, which a filesystem notices only when
        // it is nearly full.
        if (pe->gpt_lba_end < pe->gpt_lba_start) return 0;
        *base = pe->gpt_lba_start;
        *count = pe->gpt_lba_end - pe->gpt_lba_start + 1;
        return 1;
    }
    if (pe->mbr_type == 0xEE) return 0; // protective entry, never a filesystem
    *base = pe->mbr_lba_start;
    *count = pe->mbr_num_sectors;
    return *count != 0;
}

const struct block_device *mount_partition_device(int number) {
    const struct block_device *disk = blk_root_disk();
    if (!disk || !read_table()) return NULL;
    if (number < 1 || number > g_tbl.entry_count) return NULL;
    uint64_t base, count;
    if (!entry_window(&g_tbl.entries[number - 1], &base, &count)) return NULL;
    return blk_part_create(disk, base, count, number);
}

// Tries to mount a root out of one of the disk's PARTITIONS.
// Returns:
//    1  mounted -- that partition is now the active block device
//    0  there IS a table, but no partition held a filesystem
//   -1  no partition table at all
//
// The caller needs 0 and -1 apart, and they now mean two different
// things to a user: 0 is "your partitions are empty, format one", -1
// is "this drive has no table and toy-os will not mount it". Both end
// in ramfs; only one of them is somebody's mistake.
//
// Order is the table's own order, not "biggest" or "first bootable" --
// there is nothing here that would make a cleverer policy more correct,
// and a table's order is the one thing a person writing it controls.
// Gives every partition of every DISK a device (and therefore a name),
// so `mount ahci0p1 /mnt` can reach a drive that carries no root.
//
// Reads each disk's own table through partition_read_table_of() rather
// than the active-device reader, and creates a window per usable entry.
// Nothing is mounted and nothing becomes active: this is naming, which
// is a separate question from what the root is -- the same split the
// device table exists for.
//
// A disk with no table, or one whose entries are unusable, simply
// contributes nothing. That is not an error: a blank drive is a
// perfectly ordinary thing to have plugged in.
// Is anything mounted from this device? A window a mount still points at
// must not be released -- the mount would be left holding a slot
// somebody else can reuse.
static int device_is_mounted(const struct block_device *dev) {
    for (int i = 0; i < MOUNT_MAX; i++)
        if (g_mounts[i].used && g_mounts[i].dev == dev) return 1;
    return 0;
}

// Drop this disk's existing windows, so the scan below NAMES the new
// ones instead of appending them beside the old.
//
// WITHOUT THIS A RE-READ IS ADDITIVE AND THE OLD NAME WINS.
// blk_part_create() reuses a slot only when the base AND the size match,
// so a partition that CHANGED SIZE gets a second slot with the same name
// -- and blk_device_by_name() answers with the first, which is the stale
// one. `install` onto a disk that already had a table then formatted the
// OLD window: a 119 GB partition with a 441 MB filesystem in it, on a
// machine that booted perfectly and used 0.4% of its disk. Linux's
// BLKRRPART deletes and re-adds partition devices for the same reason.
static void forget_windows_of(const struct block_device *disk) {
    // Downward, because releasing compacts the table under us.
    for (int i = blk_device_count() - 1; i >= 0; i--) {
        const struct blk_entry *e = blk_device_at(i);
        if (!e || e->parent != disk || e->dev == disk) continue;
        if (device_is_mounted(e->dev)) continue;
        blk_part_release(e->dev);
    }
}

void mount_force_readonly(const struct block_device *dev, const char *why) {
    if (!dev) return;
    for (int i = 0; i < MOUNT_MAX; i++) {
        struct mount *m = &g_mounts[i];
        if (!m->used || m->dev != dev || (m->flags & MNT_RDONLY)) continue;
        m->flags |= MNT_RDONLY;
        klog_printf("mount: %s is now read-only -- %s\n", m->point,
                    why ? why : "the filesystem reported an error");
    }
}

int mount_rescan_disk(const struct block_device *disk) {
    if (!disk) return 0;
    // STATIC, not a stack local: struct partition_table is ~1.5 KB
    // against a 1 KB kernel frame budget, the same call g_tbl and
    // partition_query.c both make. Separate from g_tbl, which caches
    // the ROOT's table.
    static struct partition_table tbl;
    if (!partition_read_table_of(disk, &tbl)) return 0;
    if (tbl.kind == PART_TABLE_NONE || tbl.entry_count == 0) return 0;

    forget_windows_of(disk);

    int named = 0;
    for (int n = 0; n < tbl.entry_count; n++) {
        uint64_t base, count;
        if (!entry_window_of(&tbl, &tbl.entries[n], &base, &count)) continue;
        if (blk_part_create(disk, base, count, n + 1)) named++;
    }
    return named;
}

static void name_all_partitions(void) {
    // Snapshot the count: creating windows APPENDS to the table, and a
    // loop over a growing table would walk into the partitions it just
    // made and try to sub-partition them.
    int disks = blk_device_count();
    for (int i = 0; i < disks; i++) {
        const struct blk_entry *e = blk_device_at(i);
        if (!e || e->dev != e->parent) continue;   // a partition, not a disk
        mount_rescan_disk(e->dev);
    }
}

// `root=` -- which device carries the root, by the name block.h's table
// gave it (`ahci0`, `virtio0`, `ata0p2`). Empty when unset.
//
// A PARTITION CANNOT BE NAMED UNTIL IT EXISTS, and partitions are only
// registered once try_partitions() walks a table. So this is split: the
// DISK half is applied at boot, before the scan, and decides which disk
// gets scanned; the PARTITION half is remembered here and consulted
// inside the scan. Naming a partition therefore also names its disk,
// which is what a reader would expect from `root=ahci0p2`.
static char g_root_disk[BLK_NAME_MAX];
static int  g_root_part;   // 1-based partition number, 0 = any

// Same substring-plus-boundary matching every boot word here uses, and
// for the reason target.c gives: without the boundary check a longer
// word ending in `root=` would silently match.
static int cmdline_root(const char *cmdline, char *out, uint32_t out_size) {
    if (!cmdline) return 0;
    for (const char *p = cmdline; (p = k_strstr(p, "root=")) != 0; p += 5) {
        if (p != cmdline && p[-1] != ' ') continue;
        const char *v = p + 5;
        uint32_t n = 0;
        while (v[n] && v[n] != ' ' && n + 1 < out_size) n++;
        if (n == 0) return 0;
        k_memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

// Splits `ahci0p2` into the disk `ahci0` and the partition 2. A name
// with no `p<digits>` tail is a whole disk and leaves g_root_part 0.
//
// The scan is from the END: a driver stem can contain a `p` of its own
// one day, and the partition suffix is always the last one followed by
// nothing but digits.
static void split_root(const char *name) {
    k_strlcpy(g_root_disk, name, sizeof g_root_disk);
    g_root_part = 0;

    int len = (int)k_strlen(g_root_disk);
    for (int i = len - 1; i > 0; i--) {
        if (g_root_disk[i] != 'p') continue;
        int all_digits = (i + 1 < len);
        for (int j = i + 1; j < len && all_digits; j++)
            if (g_root_disk[j] < '0' || g_root_disk[j] > '9') all_digits = 0;
        if (!all_digits) break;
        int v = 0;
        for (int j = i + 1; j < len; j++) v = v * 10 + (g_root_disk[j] - '0');
        if (v > 0) { g_root_part = v; g_root_disk[i] = '\0'; }
        break;
    }
}

// A `root=` that names nothing is REPORTED AND IGNORED, never fatal.
// Every /etc reader here treats a typo that way, and the argument is
// stronger on the boot line: a machine that refuses to boot because of
// one mistyped word gives its owner nothing to fix it with. What it
// must not do is fail SILENTLY, so the table is printed.
static void root_override(const char *cmdline) {
    char want[BLK_NAME_MAX];
    if (!cmdline_root(cmdline, want, sizeof want)) return;

    split_root(want);
    const struct blk_entry *e = blk_device_by_name(g_root_disk);
    if (e && blk_set_root(e->dev)) {
        if (g_root_part)
            klog_printf("fs: root=%s -- disk %s, partition %d\n",
                        want, g_root_disk, g_root_part);
        else
            klog_printf("fs: root=%s -- using %s\n", want, g_root_disk);
        return;
    }

    klog_printf("fs: root=%s names no device this boot found; using %s instead\n",
                want, blk_root_present() ? blk_device_name(blk_root()) : "nothing");
    for (int i = 0; i < blk_device_count(); i++) {
        const struct blk_entry *d = blk_device_at(i);
        klog_printf("fs:   have %s (%llu sectors)\n", d->name,
                    (unsigned long long)d->dev->sector_count(d->dev));
    }
    g_root_disk[0] = '\0';
    g_root_part = 0;
}

static int try_partitions(void) {
    const struct block_device *disk = blk_root_disk();
    if (!disk) return -1;
    if (!read_table()) return -1;
    if (g_tbl.entry_count == 0) return 0; // an empty table is still a table

    klog_printf("fs: %s partition table, %d entries\n",
                g_tbl.kind == PART_TABLE_GPT ? "GPT" : "MBR", g_tbl.entry_count);

    // Remembered so the "nothing mounted" path below can re-register
    // the first partition that is ours -- see there.
    uint32_t first_base = 0, first_count = 0;
    int first_ok = 0, first_index = 1;

    for (int i = 0; i < g_tbl.entry_count; i++) {
        const struct partition_entry *pe = &g_tbl.entries[i];
        uint64_t base, count;

        // THE FIRMWARE'S PARTITIONS ARE NOT THE ROOT. A BIOS boot
        // partition holds GRUB's core.img with no filesystem in it, and
        // the ESP holds /boot/kernel.bin -- this OS's own disk has both
        // in front of the filesystem. Skipping them here does two
        // things: neither becomes the ROOT, and neither can become the
        // "leave partition 1 active" fallback below, which would point
        // `fsformat` at the bootloader.
        //
        // The ESP is not ignored any more, though -- mount_boot_auto()
        // comes back for it and mounts it at /boot, read-only. That is
        // a different question from "what is the root", which is why it
        // is a different pass.
        if (partition_is_firmware(pe, g_tbl.kind)) {
            klog_printf("fs: partition %d is the firmware's (bootloader/ESP) -- not the root\n", i + 1);
            continue;
        }

        // `root=<disk>p<n>` names ONE partition. Others are still
        // registered by the pass above -- they stay mountable by hand --
        // but only the named one is offered to a backend as the root.
        if (g_root_part && g_root_part != i + 1) continue;

        if (!entry_window(pe, &base, &count)) continue;
        if (!blk_part_register(disk, base, count, i + 1)) continue;
        if (!first_ok) {
            first_base = base; first_count = count; first_index = i + 1; first_ok = 1;
        }

        const struct block_device *dev = blk_root();
        for (int b = 0; b < FS_BACKEND_COUNT; b++) {
            const struct fs_ops *fs = g_backends[b];
            // A backend that addresses the disk absolutely would bypass
            // the partition window entirely and probe the DISK's LBA 0
            // -- claiming a partition it never looked at. See fs_ops.h.
            if (!fs->volume_relative) continue;
            if (!caps_are_honest(fs)) continue;
            if (mounts_of(fs) >= fs->max_mounts) continue;
            struct fs_scratch sc;
            if (!mount_scratch_begin(fs, &sc)) continue;
            int claimed = fs->probe(sc.st, dev);
            mount_scratch_end(&sc);
            if (claimed == 1) {
                klog_printf("fs: mounting %s from partition %d (LBA %llu, %llu sectors)\n",
                            fs->name, i + 1, (unsigned long long)base, (unsigned long long)count);
                const char *why;
                if (mount_add(dev, fs->name, "/", 0, 0, &why)) return 1;
                klog_printf("fs: partition %d would not mount: %s\n", i + 1, why);
            }
        }
    }

    // Nothing claimed anything -- every partition is empty, which is
    // what a freshly `mkpart`ed disk looks like.
    //
    // LEAVE THE FIRST PARTITION ACTIVE rather than restoring the whole
    // disk -- the first one that is OURS, since a firmware partition
    // never reaches this loop. `fsformat` formats whatever the active
    // block device is, so this is what makes "mkpart, reboot, fsformat"
    // put a filesystem INSIDE a partition instead of flat across the
    // table and every partition it describes. Restoring the disk here
    // was the obvious thing and it is the wrong thing: it would make
    // the one command you reach for next quietly undo the one you just
    // ran.
    //
    // If partition 1 will not register (a table describing a window off
    // the end of the disk), fall back to the whole disk -- that is a
    // broken table, and refusing to have any active device at all would
    // be a worse answer than the one this OS has always given.
    if (first_ok) {
        klog_printf("fs: no partition holds a filesystem -- leaving partition %d "
                    "active (LBA %u, %u sectors) for `fsformat`\n",
                    first_index, first_base, first_count);
        blk_part_register(disk, first_base, first_count, first_index);
    } else {
        blk_register(disk);
    }
    return 0;
}

// Mounts the in-memory root, and says why it came to that. The one
// place ramfs is chosen, so "when do we end up in RAM" is answerable by
// reading one function rather than four call sites.
static void mount_ramfs_root(const char *why_log) {
    klog_write(why_log);
    const char *why;
    // The honesty check runs inside mount_add() -- ramfs is never
    // probed, so skipping it would leave the one backend that can
    // always be reached as the one nothing validates.
    if (!mount_add(NULL, g_ram_backend->name, "/", 0, 0, &why)) {
        klog_printf("fs: ramfs did not mount either (%s) -- NO FILESYSTEM this boot\n", why);
    }
}

// Picks what to mount as the ROOT, and it is a TABLE of situations
// rather than a fallthrough -- see docs/rootfs-design.md:
//
//   a live module, and (`live` asked for, or no disk)  -> TFS3 in RAM
//   a drive with a table, a partition somebody claims  -> that backend
//   a drive with a table, nothing claimable            -> ramfs
//   a drive with NO table                              -> REFUSED, ramfs
//   no drive at all                                    -> ramfs
//
// A DRIVE'S ROOT IS A PARTITION OR IT IS NOTHING. A whole-disk volume
// ("superfloppy") is a legal shape that no installed system has had in
// twenty years: Windows will not boot one at all, and no Linux
// installer produces one. Supporting it meant a second probe path
// through the code that decides what to mount -- untested, and on the
// one decision where being wrong in the permissive direction destroys
// data (see docs/decisions/storage.md's entry on removing a backend).
// One rule, one path, and an image that predates it is TOLD so.
//
// AND NOTHING IS AUTO-FORMATTED. The old blank-disk policy wrote a
// fresh filesystem over any readable disk nobody claimed. With a
// whole-disk volume refused there is nowhere left for it to write: a
// partition's contents are the partition's business, and `mkpart` then
// `fsformat` is how a drive gets a filesystem. The lesson that flag
// carried -- an unrecognised disk is not an invitation -- is now true
// by construction rather than by a branch remembering it.
static void probe_and_mount_root(void) {
    // The live image gets first refusal ONLY IF ASKED FOR. Registering a
    // block device is what makes the probe below read from RAM instead
    // of a disk -- the backends are unchanged and never learn which it
    // is. See try_live_module() for why the two halves of the rule are
    // asked at different points.
    const char *cmdline = multiboot_cmdline();
    int forced = cmdline && k_strstr(cmdline, "live") ? 1 : 0;

    // EVERY DRIVER RUNS, WHATEVER THE OTHERS FOUND. This was
    // `if (!blk_virtio_init() && !blk_ahci_init()) blk_ata_init();`, and
    // the short circuit meant a machine with a virtio disk never ran the
    // AHCI driver at all -- its SATA disk did not exist, for `parttable`
    // or `mount` or anything else. Enumeration is READ-ONLY (identify,
    // read the table), so running all three costs nothing but the probe
    // and is what makes every disk reachable. See block.h's device
    // table for the split this is half of.
    //
    // ORDER STILL SETS PRECEDENCE, because blk_register() is
    // last-writer-wins: ATA, then AHCI, then NVMe, then VIRTIO, so virtio
    // ends up the default root exactly as before -- it is the faster and
    // better-tested path (block_virtio.c has the numbers), NVMe is what
    // a laptop of this decade boots from, AHCI is what a SATA machine
    // presents, legacy IDE is the fallback and still the only disk on
    // some hardware. `novirtio`, `nonvme` and `noahci` step down a rung
    // each, which keeps the lower paths reachable and tested.
    blk_ata_init();
    blk_ahci_init();
    blk_nvme_init();
    blk_virtio_init();

    // The live image: asked for, or nothing else to mount. It registers
    // LAST, so it takes the root from any disk above -- which is the
    // rule, and now the disks stay in the table and stay reachable, so a
    // live session can see and mount the machine's own drives. That is
    // most of what a live CD is for, and it could not before.
    if (forced || !blk_root_present()) try_live_module(forced);

    // `root=` overrides the precedence, naming a disk from the table.
    // Linux's `root=` and NT's BCD `osdevice`: the boot line decides,
    // not whichever driver happened to probe last.
    root_override(cmdline);

    // Every disk's partitions get a name, not just the root's. A device
    // with no name cannot be mounted, so without this pass the second
    // drive is enumerated and still unusable -- visible in the table and
    // impossible to reach, which is the worse half of not enumerating it
    // at all. Creating a window does NOT mount or activate it.
    name_all_partitions();

    if (!blk_root_present()) {
        mount_ramfs_root("fs: no disk -- mounting ramfs (nothing here survives a reboot)\n");
        return;
    }

    // A LIVE IMAGE GOES THROUGH THE PARTITION SCAN TOO, and it is not an
    // exception worth carving out: `make live-iso` seeds it with
    // `seed_disk.py --layout rest`, so the module the bootloader hands
    // over carries a table with one partition exactly like a drive
    // does. Special-casing it here -- probing the whole RAM volume
    // directly -- made every live boot fall through to ramfs with no
    // /bin, because the backend's superblock is inside the partition
    // and not at the volume's LBA 0.
    int part_result = try_partitions();
    if (part_result == 1) return;   // mounted from a partition -- the normal case

    if (part_result == 0) {
        // A table, but no partition held a filesystem: empty partitions
        // waiting for `fsformat`, not free space to claim.
        mount_ramfs_root("fs: no partition holds a filesystem -- mounting ramfs "
                         "(`fsformat` claims the active partition)\n");
        return;
    }

    // No table at all: a blank drive, a whole-disk volume from before
    // this rule, or something foreign. Name the fix, because the
    // migration is destructive and nobody should have to guess it.
    klog_write("fs: this disk has NO PARTITION TABLE -- toy-os mounts a root only from a\n");
    klog_write("fs: partition. Nothing has been written to it. On the host: "
               "`make clean-disk && make iso`;\n");
    klog_write("fs: on the machine: `mkpart --gpt <sizes> confirm`, reboot, "
               "`fsformat tfs3 confirm`.\n");
    mount_ramfs_root("fs: mounting ramfs meanwhile (nothing here survives a reboot)\n");
}

void mount_boot_root(void) {
    probe_and_mount_root();
}

// THE ESP MOUNTS AT /boot, READ-ONLY.
//
// Read-only is the default and not a placeholder: this partition holds
// the bootloader and the kernel image that started the machine, and a
// FAT driver's first outing is not where that should be writable by
// accident. `mount -w 2 /boot` after `umount /boot` is the deliberate
// way in, which is exactly Linux's shape -- an ESP in /etc/fstab is
// conventionally `ro` or not mounted at all until something needs it.
void mount_boot_auto(void) {
    const struct block_device *disk = blk_root_disk();
    if (!disk || !read_table()) return;
    if (!mount_root()) return;

    for (int i = 0; i < g_tbl.entry_count; i++) {
        const struct partition_entry *pe = &g_tbl.entries[i];
        if (!partition_is_esp(pe, g_tbl.kind)) continue;

        uint64_t base, count;
        if (!entry_window(pe, &base, &count)) continue;
        const struct block_device *dev = blk_part_create(disk, base, count, i + 1);
        if (!dev) continue;

        const char *why;
        if (mount_add(dev, NULL, "/boot", MNT_RDONLY, 0, &why)) return;
        // Not an error worth alarming about: a machine whose ESP holds
        // a FAT this kernel cannot read still boots perfectly, and the
        // only thing lost is being able to look at /boot.
        klog_printf("fs: /boot not mounted (%s)\n", why);
        return;
    }
}

int mount_reprobe_root(const struct fs_ops *expect) {
    // Drop everything: a reformat invalidates the root, and anything
    // mounted under it was reached through a path the root owns.
    fs_exclusive_begin();
    for (int i = 0; i < MOUNT_MAX; i++) slot_clear(&g_mounts[i]);
    fs_exclusive_end();
    g_tbl_valid = 0;
    probe_and_mount_root();
    const struct mount *root = mount_root();
    return root && root->fs == expect && root->persistent;
}

const struct fs_ops *mount_backend_named(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < FS_BACKEND_COUNT; i++) {
        if (k_strcmp(g_backends[i]->name, name) == 0) return g_backends[i];
    }
    return NULL;
}

int mount_wipe_others(const struct fs_ops *target, const struct block_device *dev) {
    for (int i = 0; i < FS_BACKEND_COUNT; i++) {
        if (g_backends[i] == target) continue;
        // On a SCRATCH state: wipe() repoints a backend at `dev`, and
        // this runs it against every backend but the target -- which is
        // how a mounted TFS3 root twice ended up reading a FAT32 disk
        // being formatted, taking /bin with it.
        struct fs_scratch sc;
        if (!mount_scratch_begin(g_backends[i], &sc)) continue;
        g_backends[i]->wipe(sc.st, dev);
        mount_scratch_end(&sc);
    }
    return 1;
}
