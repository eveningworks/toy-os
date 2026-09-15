// ramfs -- a filesystem that lives in the kernel heap and nowhere else.
//
// WHAT IT IS FOR: being the root when there is no usable drive. Before
// this existed, a diskless boot mounted NOTHING while announcing
// `fs: active backend: tfs3 (RAM-only)` -- a label on an absence, with
// every fs_* call failing and two comments in the tree pointing at each
// other about a mode neither implemented. See docs/rootfs-design.md.
//
// WHY NOT TFS3 ON A RAM DISK, which the live CD already does through
// block_ram.c: that means running a real on-disk format over memory --
// a superblock, block groups, backups and a JOURNAL, every one of them
// paying for durability that memory cannot have. The journal is the
// point of TFS3 and is pure overhead here. A block device also fixes
// the size at mount, where a filesystem in the heap can grow into
// whatever is actually free.
//
// IT IS `ramfs`, NOT `tmpfs`, and the distinction is real rather than
// cosmetic: tmpfs can page to swap, this kernel has no swap, and
// borrowing the name would promise a mechanism that does not exist.
// **That stays true now that this backs `/tmp`**, which is the obvious
// moment to start calling it tmpfs. What is missing is not the mount,
// it is that these chunks are kmalloc'd KERNEL HEAP, which no page
// reclaim can evict -- Linux's tmpfs is swappable because its pages are
// shmem rather than slab. See docs/swap-design.md, stage 6.
//
// ---- the three decisions worth knowing before editing ----
//
// 1. A NODE KNOWS ITS PARENT, AND NOTHING KNOWS ITS CHILDREN. Listing
//    a directory scans the node table for entries whose parent is that
//    directory. The obvious alternative -- first-child/next-sibling
//    links -- makes listing touch only the directory's own children,
//    and makes create, delete and rename each unlink-and-relink three
//    fields that must agree. This way every one of those is a single
//    field write that cannot leave a dangling link. The cost is a scan
//    of RAMFS_MAX_NODES pointers per lookup, against a listing cap of
//    FS_MAX_FILES (256) and paths bounded to FS_PATH_MAX (64) -- not
//    what a directory listing will be waiting on.
//
// 2. FILE DATA IS CHUNKED, and the reason is the allocator, not taste.
//    kmalloc -> heap_os_alloc() -> pmm_alloc_contiguous(), so every
//    heap region is a physically contiguous run of frames. One buffer
//    per file would ask for a run as large as the file -- 6144
//    contiguous frames for a 24 MiB file -- which fails on a fragmented
//    machine while `meminfo` still shows memory free. That failure is
//    invisible until the box has been up a while, which is the worst
//    shape a bug can have. Fixed RAMFS_CHUNK pages, addressed through a
//    per-file pointer array, never need a run bigger than one page for
//    data. A NULL chunk reads as zeroes, which is also what makes
//    fs_truncate()'s "growing does not consume blocks" contract true
//    here for free.
//
// 3. IT HAS A BUDGET, because this kernel has no OOM killer. Linux's
//    ramfs is famously unbounded and relies on one. Here the ramfs and
//    the kernel heap draw from the same frames, so an unbounded write
//    would take out the allocator every other subsystem depends on.
//    Half of free memory at mount, which is tmpfs's own default, and
//    over it every allocating write fails exactly as a full disk does.
#include "fs.h"
#include "fs_ops.h"
#include "block.h" // struct block_device -- the ops signatures take one
#include "ramfs.h"
#include "ktime.h"
#include "caltime.h"
#include "mount.h" // MOUNT_MAX -- the mount limit this backend declares
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "pmm.h"
#include "tz.h"
#include "storage_config.h" // storage.ramfs_size -- the budget's middle source

// One path COMPONENT, not a path: FS_PATH_MAX (64) bounds the whole
// thing, so a name longer than this could only appear in a path no
// caller can express.
#define RAMFS_NAME_MAX 48

// One page, so a data allocation is always a single-frame request --
// see decision 2 above.
#define RAMFS_CHUNK 4096

// The table is .bss rather than heap: it is the one allocation that
// must succeed for the filesystem to exist at all, and a root
// filesystem that fails to mount because the heap was busy is the
// situation ramfs is here to prevent. 4096 * 8 bytes = 32 KiB.
#define RAMFS_MAX_NODES 4096

// Chunk pointers per file, grown in steps of this. Bounded by the
// budget in practice; the array itself is one contiguous allocation,
// which is why it is pointers and not data (a 24 MiB file's array is
// 48 KiB, against 24 MiB if the data lived there).
#define RAMFS_CHUNKS_GROW 32

struct rnode {
    char name[RAMFS_NAME_MAX];
    int32_t parent;         // index into S->nodes; -1 only for the root
    uint8_t is_dir;
    uint64_t size;          // bytes; a directory's stays 0
    uint64_t created;
    uint64_t modified;
    uint8_t **chunks;       // NULL for a directory, or an empty file
    uint32_t chunk_cap;     // entries in chunks[], not bytes
};

// PER MOUNT, in struct ramfs_state, reached through `S` -- the mount the
// current call belongs to (fs_ops.h). nodes[] IS the filesystem, so a
// second ramfs mount is simply a second tree.
struct ramfs_state {
    struct rnode *nodes[RAMFS_MAX_NODES];
    int mounted;
    uint64_t budget;   // bytes ramfs may hold, decided at mount
    uint64_t used;     // bytes it currently holds

    // The whole-file staging buffer read() hands back a pointer into --
    // the same contract tfs3.c's read() has, and the same hazard: it is
    // freed on the next call. fs.h's own comment tells kernel-side
    // callers to prefer fs_read_into(); nothing changes here.
};

static struct ramfs_state *S;   // NULL between calls -- see fs_ops.h

// ---- time -----------------------------------------------------------

// One line, one number, said where the number is actually decided --
// the test seam overrides the budget after init() and must not leave a
// log claiming the default.
static void ramfs_log_budget(const char *why) {
    if (S->budget >= 1024 * 1024)
        klog_printf("ramfs: mounted, budget %u MiB (%s)\n",
                    (unsigned)(S->budget / (1024 * 1024)), why);
    else
        klog_printf("ramfs: mounted, budget %u KiB (%s)\n",
                    (unsigned)(S->budget / 1024), why);
}

static uint64_t now_epoch(void) {
    struct rtc_time t;
    ktime_read(&t);
    return cal_rtc_to_epoch(&t);
}

// ---- the budget -----------------------------------------------------

static int budget_take(uint64_t bytes) {
    if (S->used + bytes > S->budget) return 0;
    S->used += bytes;
    return 1;
}

static void budget_give(uint64_t bytes) {
    S->used = (S->used >= bytes) ? S->used - bytes : 0;
}

// ---- nodes ----------------------------------------------------------

static void free_chunks(struct rnode *n) {
    if (!n->chunks) return;
    for (uint32_t i = 0; i < n->chunk_cap; i++) {
        if (n->chunks[i]) {
            kfree(n->chunks[i]);
            budget_give(RAMFS_CHUNK);
        }
    }
    budget_give((uint64_t)n->chunk_cap * sizeof(uint8_t *));
    kfree(n->chunks);
    n->chunks = 0;
    n->chunk_cap = 0;
}

static void node_free(int idx) {
    struct rnode *n = S->nodes[idx];
    if (!n) return;
    free_chunks(n);
    kfree(n);
    S->nodes[idx] = 0;
    budget_give(sizeof(struct rnode));
}

static int node_alloc(const char *name, int parent, int is_dir) {
    if (!budget_take(sizeof(struct rnode))) return -1;
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (S->nodes[i]) continue;
        struct rnode *n = kmalloc(sizeof(struct rnode));
        if (!n) { budget_give(sizeof(struct rnode)); return -1; }
        k_memset(n, 0, sizeof *n);
        k_strlcpy(n->name, name, RAMFS_NAME_MAX);
        n->parent = (int32_t)parent;
        n->is_dir = (uint8_t)(is_dir != 0);
        n->created = n->modified = now_epoch();
        S->nodes[i] = n;
        return i;
    }
    budget_give(sizeof(struct rnode));
    return -1;
}

// ---- paths ----------------------------------------------------------
//
// Each backend normalizes for itself (fs_ops.h says so, because a
// future filesystem might have different path rules). These are TFS3's
// rules: absolute, no "." or "..", no trailing slash, and a bare name
// is treated as a child of the root for callers that predate
// directories.

static int name_matches(const struct rnode *n, const char *seg, int len) {
    if (len <= 0 || len >= RAMFS_NAME_MAX) return 0;
    if (k_strlen(n->name) != (size_t)len) return 0;
    return k_memcmp(n->name, seg, (size_t)len) == 0;
}

static int find_child(int parent, const char *seg, int len) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        struct rnode *n = S->nodes[i];
        if (!n || n->parent != parent) continue;
        if (name_matches(n, seg, len)) return i;
    }
    return -1;
}

// Walks `path` and returns its node index, or -1. `stop_before_last`
// resolves only the parent directory and hands back the final
// component through `leaf`/`leaf_len` -- one walker for both jobs, so
// "where does this path live" is answered in exactly one place.
static int walk(const char *path, int stop_before_last,
                const char **leaf, int *leaf_len) {
    if (!path || !*path) return -1;
    if (k_strlen(path) >= FS_PATH_MAX) return -1;

    const char *p = path;
    if (*p == '/') p++;

    int cur = 0; // the root
    if (!*p) {
        // The root itself. It has no last component, so a caller
        // asking for the parent of "/" is asking something meaningless.
        if (stop_before_last) return -1;
        return 0;
    }

    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - seg);
        if (len == 0) return -1;                       // "//" or a trailing slash
        if (len == 1 && seg[0] == '.') return -1;      // not normalized
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return -1;
        if (len >= RAMFS_NAME_MAX) return -1;

        int last = (*p == 0);
        if (last && stop_before_last) {
            *leaf = seg;
            *leaf_len = len;
            return cur;
        }
        int next = find_child(cur, seg, len);
        if (next < 0) return -1;
        if (!last && !S->nodes[next]->is_dir) return -1; // a file used as a directory
        cur = next;
        if (*p == '/') {
            p++;
            // A TRAILING SLASH IS NOT NORMALIZED, and accepting it
            // would make "/d" and "/d/" two spellings of one path --
            // fs.h says callers normalize, so a backend that guesses
            // here disagrees with the shell's `cd` about what a path
            // is. Caught by a KTEST, which is why it says so.
            if (!*p) return -1;
        }
    }
    return cur;
}

static int find(const char *path) {
    const char *leaf; int len;
    return walk(path, 0, &leaf, &len);
}

// Resolves `path` to (parent, leaf) and refuses anything that could not
// be created there: a missing parent, a parent that is a file, or a
// name already taken.
static int resolve_new(const char *path, int *out_parent,
                       const char **out_leaf, int *out_len) {
    const char *leaf; int len;
    int parent = walk(path, 1, &leaf, &len);
    if (parent < 0 || !S->nodes[parent] || !S->nodes[parent]->is_dir) return 0;
    *out_parent = parent;
    *out_leaf = leaf;
    *out_len = len;
    return 1;
}

static int has_children(int idx) {
    for (int i = 0; i < RAMFS_MAX_NODES; i++)
        if (S->nodes[i] && S->nodes[i]->parent == idx) return 1;
    return 0;
}

// ---- file data ------------------------------------------------------

static int chunks_reserve(struct rnode *n, uint32_t want) {
    if (want <= n->chunk_cap) return 1;
    uint32_t cap = n->chunk_cap ? n->chunk_cap : RAMFS_CHUNKS_GROW;
    while (cap < want) cap += RAMFS_CHUNKS_GROW;

    uint64_t bytes = (uint64_t)cap * sizeof(uint8_t *);
    if (!budget_take(bytes - (uint64_t)n->chunk_cap * sizeof(uint8_t *))) return 0;
    uint8_t **grown = kmalloc((size_t)bytes);
    if (!grown) {
        budget_give(bytes - (uint64_t)n->chunk_cap * sizeof(uint8_t *));
        return 0;
    }
    k_memset(grown, 0, (size_t)bytes);
    if (n->chunks) {
        k_memcpy(grown, n->chunks, (size_t)n->chunk_cap * sizeof(uint8_t *));
        kfree(n->chunks);
    }
    n->chunks = grown;
    n->chunk_cap = cap;
    return 1;
}

// The chunk holding byte `off`, allocating it if `create`. A NULL
// return with create==0 is a HOLE, which reads as zeroes -- that is
// what makes a grown-but-unwritten range cost nothing.
static uint8_t *chunk_at(struct rnode *n, uint64_t off, int create) {
    uint32_t idx = (uint32_t)(off / RAMFS_CHUNK);
    if (idx >= n->chunk_cap) {
        if (!create) return 0;
        if (!chunks_reserve(n, idx + 1)) return 0;
    }
    if (!n->chunks[idx] && create) {
        if (!budget_take(RAMFS_CHUNK)) return 0;
        n->chunks[idx] = kmalloc(RAMFS_CHUNK);
        if (!n->chunks[idx]) { budget_give(RAMFS_CHUNK); return 0; }
        k_memset(n->chunks[idx], 0, RAMFS_CHUNK);
    }
    return n->chunks[idx];
}

static uint32_t read_at(struct rnode *n, uint64_t off, void *buf, uint32_t len) {
    if (off >= n->size) return 0;
    if (off + len > n->size) len = (uint32_t)(n->size - off);
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t at = off + done;
        uint32_t in_chunk = (uint32_t)(at % RAMFS_CHUNK);
        uint32_t take = RAMFS_CHUNK - in_chunk;
        if (take > len - done) take = len - done;
        uint8_t *c = chunk_at(n, at, 0);
        if (c) k_memcpy(out + done, c + in_chunk, take);
        else k_memset(out + done, 0, take);   // a hole
        done += take;
    }
    return done;
}

static int write_at(struct rnode *n, uint64_t off, const void *buf, uint32_t len) {
    const uint8_t *in = buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t at = off + done;
        uint32_t in_chunk = (uint32_t)(at % RAMFS_CHUNK);
        uint32_t take = RAMFS_CHUNK - in_chunk;
        if (take > len - done) take = len - done;
        uint8_t *c = chunk_at(n, at, 1);
        if (!c) return 0;                      // out of budget or memory
        k_memcpy(c + in_chunk, in + done, take);
        done += take;
    }
    if (off + len > n->size) n->size = off + len;
    n->modified = now_epoch();
    return 1;
}

// ---- fs_ops ---------------------------------------------------------

static int ramfs_probe(const struct block_device *dev) {
    (void)dev;
    // NEVER claims a device. ramfs is chosen by POLICY in vfs.c, not by
    // detection -- there is no superblock to recognise, and a backend
    // that claimed a disk it cannot read would be the worst possible
    // answer to "what is on this drive".
    return 0;
}

static int ramfs_wipe(const struct block_device *dev) {
    (void)dev;
    // Nothing on any disk bears its signature, so there is nothing to
    // erase. Success, per fs_ops.h ("nothing to wipe counts as
    // success").
    return 1;
}

static void drop_everything(void) {
    for (int i = 1; i < RAMFS_MAX_NODES; i++) node_free(i);
    if (S->nodes[0]) node_free(0);
    S->used = 0;
}

static int ramfs_format(const struct block_device *dev) {
    (void)dev;
    drop_everything();
    return 1;
}

static int ramfs_init(const struct block_device *dev, uint64_t size_bytes) {
    (void)dev; // ramfs has no volume -- it IS the volume
    drop_everything();

    // THE BUDGET, IN THREE STEPS, MOST SPECIFIC FIRST: what this mount
    // asked for (`mount -o size=`), then `storage.ramfs_size`, then half
    // of what the frame allocator says is free -- tmpfs's own default.
    //
    // The last step is not merely a fallback, it is what keeps a
    // DISKLESS BOOT working: the root ramfs is mounted from fs_init(),
    // before /etc is readable and before storage_config_init() has run,
    // so the setting is still 0 there and the root gets the whole rule
    // rather than a /tmp-sized cap. Anything that gave the setting a
    // non-zero compiled default would shrink a diskless root to it, and
    // nothing would say so.
    const char *why = "half of free memory";
    if (size_bytes) {
        S->budget = size_bytes;
        why = "this mount";
    } else if (storage_ramfs_size_bytes()) {
        S->budget = storage_ramfs_size_bytes();
        why = "storage.ramfs_size";
    } else {
        uint64_t free_bytes = pmm_zone_free_frames(PMM_ZONE_DMA32) * 4096ull;
        S->budget = free_bytes / 2;
    }

    // The root. If this fails the machine has no filesystem at all,
    // which is exactly what ramfs exists to prevent -- so it is the one
    // allocation whose failure is worth a loud line.
    S->nodes[0] = kmalloc(sizeof(struct rnode));
    if (!S->nodes[0]) {
        klog_write(KLOG_ERR "ramfs: cannot allocate a root directory -- not mounted\n");
        return -1;
    }
    k_memset(S->nodes[0], 0, sizeof(struct rnode));
    S->nodes[0]->parent = -1;
    S->nodes[0]->is_dir = 1;
    S->nodes[0]->created = S->nodes[0]->modified = now_epoch();
    S->used = sizeof(struct rnode);
    S->mounted = 1;

    ramfs_log_budget(why);
    // 0, NOT 1: mounted, and never persistent. fs_is_persistent()
    // reports this straight through to `df`, `fsck` and About, so the
    // truth is stated once here rather than special-cased there.
    return 0;
}

static int ramfs_touch(const char *path) {
    if (!S->mounted) return 0;
    int existing = find(path);
    if (existing >= 0) {
        if (S->nodes[existing]->is_dir) return 0;
        S->nodes[existing]->modified = now_epoch();
        return 1;
    }
    int parent; const char *leaf; int len;
    if (!resolve_new(path, &parent, &leaf, &len)) return 0;
    char name[RAMFS_NAME_MAX];
    k_memcpy(name, leaf, (size_t)len);
    name[len] = 0;
    return node_alloc(name, parent, 0) >= 0;
}

static int ramfs_mkdir(const char *path) {
    if (!S->mounted) return 0;
    if (find(path) >= 0) return 0;
    int parent; const char *leaf; int len;
    if (!resolve_new(path, &parent, &leaf, &len)) return 0;
    char name[RAMFS_NAME_MAX];
    k_memcpy(name, leaf, (size_t)len);
    name[len] = 0;
    return node_alloc(name, parent, 1) >= 0;
}

static int ramfs_write(const char *path, const char *data, int append) {
    if (!S->mounted || !data) return 0;
    int idx = find(path);
    if (idx < 0) {
        if (!ramfs_touch(path)) return 0;
        idx = find(path);
        if (idx < 0) return 0;
    }
    struct rnode *n = S->nodes[idx];
    if (n->is_dir) return 0;
    uint32_t len = (uint32_t)k_strlen(data);
    if (!append) {
        free_chunks(n);
        n->size = 0;
    }
    if (len == 0) { n->modified = now_epoch(); return 1; }
    return write_at(n, n->size, data, len);
}

static int ramfs_del(const char *path) {
    if (!S->mounted) return 0;
    int idx = find(path);
    if (idx <= 0) return 0;                  // no such thing, or the root
    if (S->nodes[idx]->is_dir && has_children(idx)) return 0;
    node_free(idx);
    return 1;
}

static uint64_t ramfs_size(const char *path) {
    if (!S->mounted) return 0;
    int idx = find(path);
    if (idx < 0 || S->nodes[idx]->is_dir) return 0;
    return S->nodes[idx]->size;
}

static uint32_t ramfs_read_range(const char *path, uint64_t offset, void *buf, uint32_t len) {
    if (!S->mounted || !buf || !len) return 0;
    int idx = find(path);
    if (idx < 0 || S->nodes[idx]->is_dir) return 0;
    return read_at(S->nodes[idx], offset, buf, len);
}

static int ramfs_write_range(const char *path, uint64_t offset, const void *buf, uint32_t len) {
    if (!S->mounted || !buf) return 0;
    int idx = find(path);
    if (idx < 0) {
        if (!ramfs_touch(path)) return 0;
        idx = find(path);
        if (idx < 0) return 0;
    }
    if (S->nodes[idx]->is_dir) return 0;
    if (len == 0) return 1;
    return write_at(S->nodes[idx], offset, buf, len);
}

// The steppable pairs. They exist so a caller can avoid BLOCKING on
// slow I/O (fs.h's async-I/O item); a memcpy has nothing to yield
// between, so each begins by doing the whole thing and the first step
// reports the result. The handle is still allocated and still freed on
// the terminal result, because that is the contract callers are
// written against -- honouring it cheaply beats reinterpreting it.
struct ramfs_step {
    int ok;
    uint32_t total;
};

static void *ramfs_write_range_begin(const char *path, uint64_t offset,
                                     const void *buf, uint32_t len) {
    struct ramfs_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->ok = ramfs_write_range(path, offset, buf, len);
    h->total = h->ok ? len : 0;
    return h;
}

static int ramfs_write_range_step(void *handle) {
    struct ramfs_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    int ok = h->ok;
    kfree(h);
    return ok ? FS_STEP_DONE : FS_STEP_FAILED;
}

static void *ramfs_read_range_begin(const char *path, uint64_t offset,
                                    void *buf, uint32_t len) {
    struct ramfs_step *h = kmalloc(sizeof *h);
    if (!h) return 0;
    h->total = ramfs_read_range(path, offset, buf, len);
    h->ok = 1;   // a short read at EOF is a success, same as fs_read_range()
    return h;
}

static int ramfs_read_range_step(void *handle, uint32_t *out_total) {
    struct ramfs_step *h = handle;
    if (!h) return FS_STEP_FAILED;
    if (out_total) *out_total = h->total;
    kfree(h);
    return FS_STEP_DONE;
}

static int ramfs_rename(const char *oldpath, const char *newpath) {
    if (!S->mounted) return 0;
    int idx = find(oldpath);
    if (idx <= 0) return 0;                   // nothing there, or the root
    if (find(newpath) >= 0) return 0;         // destination taken

    int parent; const char *leaf; int len;
    if (!resolve_new(newpath, &parent, &leaf, &len)) return 0;

    // A directory may not be moved inside itself -- that detaches the
    // subtree from the root and makes the tree a forest, which nothing
    // below would ever notice.
    for (int p = parent; p >= 0; p = S->nodes[p]->parent)
        if (p == idx) return 0;

    struct rnode *n = S->nodes[idx];
    k_memcpy(n->name, leaf, (size_t)len);
    n->name[len] = 0;
    n->parent = (int32_t)parent;
    n->modified = now_epoch();
    return 1;
}

static int ramfs_truncate(const char *path, uint64_t size) {
    if (!S->mounted) return 0;
    int idx = find(path);
    if (idx < 0 || S->nodes[idx]->is_dir) return 0;
    struct rnode *n = S->nodes[idx];
    if (size == n->size) return 1;

    if (size < n->size) {
        // Free every chunk entirely past the new end. The chunk holding
        // the new end keeps its allocation and has its tail zeroed, so
        // a later grow reads zeroes rather than the old contents.
        uint32_t keep = (uint32_t)((size + RAMFS_CHUNK - 1) / RAMFS_CHUNK);
        for (uint32_t i = keep; i < n->chunk_cap; i++) {
            if (n->chunks[i]) {
                kfree(n->chunks[i]);
                n->chunks[i] = 0;
                budget_give(RAMFS_CHUNK);
            }
        }
        uint32_t tail = (uint32_t)(size % RAMFS_CHUNK);
        if (tail && keep > 0 && keep - 1 < n->chunk_cap && n->chunks[keep - 1])
            k_memset(n->chunks[keep - 1] + tail, 0, RAMFS_CHUNK - tail);
    }
    // Growing allocates NOTHING: the new range is holes, which read as
    // zeroes (fs.h: "growing a file to a gigabyte is a metadata-only
    // operation").
    n->size = size;
    n->modified = now_epoch();
    return 1;
}

static int ramfs_is_dir(const char *path) {
    if (!S->mounted) return 0;
    int idx = find(path);
    return idx >= 0 && S->nodes[idx]->is_dir;
}

static int ramfs_exists(const char *path) {
    if (!S->mounted) return 0;
    return find(path) >= 0;
}

static void ramfs_list(const char *dir_path, void (*cb)(const char *, uint32_t, int)) {
    if (!S->mounted || !cb) return;
    int dir = find(dir_path);
    if (dir < 0 || !S->nodes[dir]->is_dir) return;
    int emitted = 0;
    for (int i = 0; i < RAMFS_MAX_NODES && emitted < FS_MAX_FILES; i++) {
        struct rnode *n = S->nodes[i];
        if (!n || n->parent != dir) continue;
        cb(n->name, (uint32_t)n->size, n->is_dir);
        emitted++;
    }
}

static int ramfs_stat(const char *path, struct fs_stat_info *out) {
    if (!S->mounted || !out) return 0;
    int idx = find(path);
    if (idx < 0) return 0;
    // The node index IS the inode number: stable for the life of the
    // node, unique, and never reused while the node exists. It is
    // synthetic in the sense FS_CAP_INODES means (nothing on a disk
    // carries it), which is why that bit stays clear.
    out->ino = (uint64_t)idx;
    out->created = S->nodes[idx]->created;
    out->modified = S->nodes[idx]->modified;
    // NO STORED MODE (hence no FS_CAP_MODE): this format has nowhere to
    // put one. The default for the type is reported rather than zero,
    // because a caller cannot act on "unknown" -- see fs.h.
    out->mode = S->nodes[idx]->is_dir ? 0755 : 0644;
    out->nlink = 1;
    return 1;
}

static int ramfs_disk_usage(uint64_t *out_used, uint64_t *out_total) {
    if (!S->mounted) return 0;
    if (out_used) *out_used = S->used;
    if (out_total) *out_total = S->budget;
    return 1;
}

static int ramfs_check(int repair, struct fs_check_result *out) {
    (void)repair;
    if (!S->mounted || !out) return 0;
    k_memset(out, 0, sizeof *out);
    // A filesystem with no on-disk representation cannot be corrupt in
    // the sense fsck means: there is no bitmap to disagree with a
    // pointer, and a leaked chunk would be a kernel heap bug rather
    // than a filesystem one. What IS worth reporting is the shape of
    // what is here, so `fsck` says something true instead of nothing.
    for (int i = 0; i < RAMFS_MAX_NODES; i++) {
        if (!S->nodes[i]) continue;
        out->records_used++;
        for (uint32_t c = 0; c < S->nodes[i]->chunk_cap; c++)
            if (S->nodes[i]->chunks[c]) out->blocks_referenced++;
    }
    return 1;
}

// ---- per-mount state ------------------------------------------------

static void *ramfs_state_alloc(void) {
    struct ramfs_state *st = kmalloc(sizeof *st);
    if (!st) return NULL;
    k_memset(st, 0, sizeof *st);
    return st;
}

static void *ramfs_state_activate(void *st) {
    void *prev = S;
    S = st;
    return prev;
}

static void ramfs_state_free(void *st) {
    if (!st) return;
    void *prev = ramfs_state_activate(st);
    drop_everything();                 // every node, and the chunks under it
    node_free(0);
    ramfs_state_activate((prev == st) ? NULL : prev);
    kfree(st);
}

const struct fs_ops ramfs_ops = {
    .name = "ramfs",
    // No FS_CAP_INODES: the ino is the node index, not something a
    // format carries. No FS_CAP_HARDLINKS, and .link stays NULL to
    // match -- one fact stated twice, which vfs.c's caps_are_honest()
    // checks both ways.
    .caps = 0,
    // 0, and it is the first honest 0 this field has ever had: ramfs
    // does not go through the block layer at all, so try_partitions()
    // must never offer it a partition. See fs_ops.h.
    .volume_relative = 0,
    // MORE THAN ONCE: nodes[] is per mount now, so a second ramfs is
    // simply a second tree. Each one takes its own budget at mount --
    // half of what was free THEN -- so two of them can between them
    // promise more than the machine has; the allocator refusing is what
    // stops that, not the budget.
    .max_mounts = MOUNT_MAX,
    .state_alloc = ramfs_state_alloc,
    .state_free = ramfs_state_free,
    .state_activate = ramfs_state_activate,
    .probe = ramfs_probe,
    .wipe = ramfs_wipe,
    .format = ramfs_format,
    .init = ramfs_init,
    .touch = ramfs_touch,
    .write = ramfs_write,
    .mkdir = ramfs_mkdir,
    .del = ramfs_del,
    .size = ramfs_size,
    .read_range = ramfs_read_range,
    .write_range = ramfs_write_range,
    .write_range_begin = ramfs_write_range_begin,
    .write_range_step = ramfs_write_range_step,
    .read_range_begin = ramfs_read_range_begin,
    .read_range_step = ramfs_read_range_step,
    .rename = ramfs_rename,
    .truncate = ramfs_truncate,
    .is_dir = ramfs_is_dir,
    .exists = ramfs_exists,
    .list = ramfs_list,
    .stat = ramfs_stat,
    .disk_usage = ramfs_disk_usage,
    .check = ramfs_check,
    .link = 0,
};

// ---- test seam ------------------------------------------------------
//
// The KTESTs run in a LIVE kernel with a real root mounted, so they
// cannot simply mount ramfs and swap the active backend out from under
// the rest of the suite. They drive these directly instead, which is
// the same shape partition_test.c uses for the block device.
// THE TEST SEAM OWNS A STATE, because a KTEST drives this backend
// directly and fs_ops.h's contract is that no state is current between
// calls. One at a time, which is all a KTEST needs; mount_test.c is
// where two ramfs mounts at once are asserted.
static void *g_test_state;

int ramfs_test_mount(uint64_t budget_bytes) {
    ramfs_test_unmount();
    g_test_state = ramfs_state_alloc();
    if (!g_test_state) return 0;
    ramfs_state_activate(g_test_state);
    // Straight through init() now that a mount carries its own size --
    // this used to poke S->budget afterwards, which meant the test seam
    // exercised a path no real mount took.
    if (ramfs_init(NULL, budget_bytes) < 0) { ramfs_test_unmount(); return 0; }
    return 1;
}

void ramfs_test_unmount(void) {
    if (!g_test_state) return;
    ramfs_state_free(g_test_state);
    g_test_state = 0;
}

uint64_t ramfs_test_used(void) { return S->used; }
