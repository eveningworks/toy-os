// SYS_SHM_OPEN / SYS_SHM_UNLINK: named shared memory, and the frames
// behind SYS_MMAP's MAP_SHARED.
//
// WHY IT EXISTS. Two ring-3 processes here could not share a byte. Every
// cross-process mapping was bespoke and kernel-owned -- the sound ring
// to its one owner, a window buffer to the registered compositor -- and
// there was no way for a process to hand memory to another process the
// kernel had no opinion about. A sound daemon is the caller that needed
// one (docs/decisions.md).
//
// THE NAME IS THE RENDEZVOUS, and that is the half people forget: with
// no unix sockets and no fd passing, shared frames alone are useless
// because neither side can say WHICH frames. The namespace is what two
// unrelated processes agree on.
//
// TWO REFERENCE HOLDERS, and both must be counted or the frames leak or
// vanish under a live mapping: an open descriptor, and a mapping. A
// mapping outlives its descriptor (POSIX says close(2) leaves the map
// alone), so the mapping table below is not redundant with the fd
// table -- it is the half that survives the close.
//
// driver-none: a memory object, not a device

#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "shm.h"
#include "query.h"
#include "query_abi.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "initcall.h"
#include "ktest.h"
#include <stddef.h>

// 16 objects of at most 64 pages (256 KiB). The frame array is the
// whole cost -- 16 * 64 * 8 = 8 KiB of BSS -- and 64 pages is four
// audio rings, so raising either is a number, not a redesign.
#define SHM_MAX       16
#define SHM_PAGES_MAX 64
#define SHM_MAPS_MAX  32 // live mappings, kernel-wide

struct shm_object {
    char     name[SHM_NAME_MAX];
    uint32_t npages;
    int      refs;      // descriptors + mappings; 0 = the slot is free
    int      unlinked;  // no new openers; the frames still live
    int      creator;   // pid, for QUERY_SHM
    uint64_t creator_mm; // its address space, so death can be noticed
    uint64_t frames[SHM_PAGES_MAX];
};

struct shm_map {
    uint64_t pml4;   // 0 = free
    uint64_t base;
    uint64_t npages;
    int      idx;
};

static struct shm_object g_obj[SHM_MAX];
static struct shm_map    g_map[SHM_MAPS_MAX];

// --- objects ---------------------------------------------------------

static void obj_free(struct shm_object *o) {
    for (uint32_t i = 0; i < o->npages; i++)
        if (o->frames[i]) pmm_free_frame(o->frames[i]);
    k_memset(o, 0, sizeof *o);
}

int shm_lookup(const char *name) {
    for (int i = 0; i < SHM_MAX; i++)
        if (g_obj[i].refs && !g_obj[i].unlinked && !k_strcmp(g_obj[i].name, name))
            return i;
    return -1;
}

void shm_get(int idx) {
    if (idx >= 0 && idx < SHM_MAX && g_obj[idx].refs) g_obj[idx].refs++;
}

void shm_put(int idx) {
    if (idx < 0 || idx >= SHM_MAX || !g_obj[idx].refs) return;
    if (--g_obj[idx].refs == 0) obj_free(&g_obj[idx]);
}

uint64_t shm_npages(int idx) {
    if (idx < 0 || idx >= SHM_MAX) return 0;
    return g_obj[idx].refs ? g_obj[idx].npages : 0;
}

uint64_t shm_frame(int idx, uint64_t page) {
    if (idx < 0 || idx >= SHM_MAX || !g_obj[idx].refs) return 0;
    if (page >= g_obj[idx].npages) return 0;
    return g_obj[idx].frames[page];
}

// --- the mapping table -----------------------------------------------

int shm_map_add(uint64_t pml4, int idx, uint64_t base, uint64_t npages) {
    for (int i = 0; i < SHM_MAPS_MAX; i++) {
        if (g_map[i].pml4) continue;
        g_map[i].idx = idx;
        g_map[i].base = base;
        g_map[i].npages = npages;
        g_map[i].pml4 = pml4; // last: a non-zero pml4 makes the slot live
        shm_get(idx);
        return 0;
    }
    return -ENOMEM;
}

// Every mapping of `pml4` that STARTS inside the range. munmap refuses
// a range spanning more than one region, so a mapping is either wholly
// in or wholly out; a partial trim of a shared mapping would need the
// frames split between two entries and is refused at the syscall.
void shm_unmap_range(uint64_t pml4, uint64_t base, uint64_t npages) {
    uint64_t end = base + npages * 4096ULL;
    for (int i = 0; i < SHM_MAPS_MAX; i++) {
        if (g_map[i].pml4 != pml4) continue;
        if (g_map[i].base < base || g_map[i].base >= end) continue;
        int idx = g_map[i].idx;
        k_memset(&g_map[i], 0, sizeof g_map[i]);
        shm_put(idx);
    }
}

void shm_process_gone(uint64_t pml4) {
    for (int i = 0; i < SHM_MAPS_MAX; i++) {
        if (g_map[i].pml4 != pml4) continue;
        int idx = g_map[i].idx;
        k_memset(&g_map[i], 0, sizeof g_map[i]);
        shm_put(idx);
    }
    // A DEAD CREATOR'S NAME GOES WITH IT, and the reason is a deadlock
    // rather than tidiness: a server holds a reference to each client's
    // object, so the object outlives the client -- and while it lives
    // the NAME is still in the namespace, which is the only way the
    // server has to notice the client is gone. It never does. The next
    // process to reuse that pid then unlinks a name it cannot free and
    // creates a SECOND object behind it, the server keeps mixing the
    // first, and the new client's ring is read by nobody (docs/bugs.md).
    //
    // The frames still go at the last reference; only the name goes now.
    for (int i = 0; i < SHM_MAX; i++)
        if (g_obj[i].refs && g_obj[i].creator_mm == pml4)
            g_obj[i].unlinked = 1;
}

// --- the syscalls ----------------------------------------------------

// A name is one path segment: no '/', no '.' run that could climb, and
// not empty. It never reaches the filesystem, so this is about keeping
// the namespace flat and printable rather than about traversal.
static int name_ok(const char *n) {
    if (!n[0]) return 0;
    for (const char *p = n; *p; p++)
        if (*p == '/' || *p < 0x20 || (unsigned char)*p > 0x7E) return 0;
    return !(n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])));
}

static int obj_create(const char *name, uint64_t npages, int pid,
                      uint64_t pml4) {
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_obj[i].refs) continue;
        struct shm_object *o = &g_obj[i];
        k_memset(o, 0, sizeof *o);
        for (uint64_t p = 0; p < npages; p++) {
            uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
            if (!f) {
                for (uint64_t q = 0; q < p; q++) pmm_free_frame(o->frames[q]);
                k_memset(o, 0, sizeof *o);
                return -ENOMEM;
            }
            // Zeroed at creation, not on fault: a shared frame has no
            // per-mapper first touch to hang the zeroing on, and one
            // process must never read another's freed memory.
            k_memset((void *)(uintptr_t)f, 0, 4096);
            o->frames[p] = f;
        }
        k_strlcpy(o->name, name, sizeof o->name);
        o->npages = (uint32_t)npages;
        o->creator = pid;
        o->creator_mm = pml4;
        o->refs = 1; // the descriptor about to be installed
        return i;
    }
    return -ENOSPC;
}

int sys_shm_open(struct syscall_ctx *c) {
    struct shm_open_msg m;
    char name[SHM_NAME_MAX];
    int64_t ret;

    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if (!vmm_copy_string_from_user(c->pml4, name, (uint64_t)(uintptr_t)m.name,
                                   sizeof name)) { ret = -EFAULT; goto out; }
    if (!name_ok(name)) { ret = -EINVAL; goto out; }
    if (m.flags & ~(SHM_CREATE | SHM_EXCL)) { ret = -EINVAL; goto out; }

    int idx = shm_lookup(name);
    if (idx >= 0) {
        if ((m.flags & SHM_CREATE) && (m.flags & SHM_EXCL)) { ret = -EEXIST; goto out; }
        shm_get(idx);
    } else {
        if (!(m.flags & SHM_CREATE)) { ret = -ENOENT; goto out; }
        uint64_t npages = (m.length + 4095) / 4096;
        if (!npages || npages > SHM_PAGES_MAX) { ret = -EINVAL; goto out; }
        idx = obj_create(name, npages, scheduler_current_pid(), c->pml4);
        if (idx < 0) { ret = idx; goto out; }
    }

    // A DESCRIPTION plus a descriptor naming it, fs_syscalls.c's pairing:
    // fd_desc_alloc() hands back the ONE reference and fd_install() only
    // points at it, so the unref belongs on the failure path alone.
    int di = fd_desc_alloc(FD_KIND_SHM, idx);
    if (di < 0) { shm_put(idx); ret = -ENFILE; goto out; }
    int fd = fd_install(c->pml4, di);
    if (fd < 0) { fd_desc_unref(di); ret = -EMFILE; goto out; }
    ret = fd;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_shm_unlink(struct syscall_ctx *c) {
    char name[SHM_NAME_MAX];
    int64_t ret;

    if (!vmm_copy_string_from_user(c->pml4, name, c->a0, sizeof name)) {
        ret = -EFAULT; goto out;
    }
    int idx = shm_lookup(name);
    if (idx < 0) { ret = -ENOENT; goto out; }
    // The name goes now; the frames go with the last holder. An object
    // somebody is still using survives its own unlink, which is what
    // makes "no new openers" expressible.
    g_obj[idx].unlinked = 1;
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- QUERY_SHM: the namespace, so a server can find its clients ------

static int shm_q_count(void) {
    int n = 0;
    for (int i = 0; i < SHM_MAX; i++) if (g_obj[i].refs) n++;
    return n;
}

static int shm_q_fill(int index, void *out) {
    int n = 0;
    for (int i = 0; i < SHM_MAX; i++) {
        if (!g_obj[i].refs) continue;
        if (n++ != index) continue;
        struct query_shm *r = out;
        k_memset(r, 0, sizeof *r);
        k_strlcpy(r->name, g_obj[i].name, sizeof r->name);
        r->bytes = (uint64_t)g_obj[i].npages * 4096ULL;
        r->refs = (uint32_t)g_obj[i].refs;
        r->creator_pid = (int32_t)g_obj[i].creator;
        r->flags = g_obj[i].unlinked ? QUERY_SHM_UNLINKED : 0;
        return 1;
    }
    return 0;
}

// No addressable fields: every record here is per OBJECT, and a flat
// `shm.bytes` would have to pick one of them. The record is read whole,
// as QUERY_DRIVER's is.
static const struct query_provider shm_q_provider = {
    .cls = QUERY_SHM,
    .name = "shm",
    .record_size = sizeof(struct query_shm),
    .flags = QUERY_F_LIST,
    .count = shm_q_count,
    .fill = shm_q_fill,
    .fields = NULL,
    .field_count = 0,
};

static void shm_query_init(void) { query_register(&shm_q_provider); }
INITCALL(shm_query_init, INIT_QUERY);

// --- tests -----------------------------------------------------------

KTEST("shm", "an object outlives its unlink while somebody holds it") {
    int idx = obj_create("ktest-shm", 2, 0, 0);
    KTEST_ASSERT(idx >= 0);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);
    KTEST_ASSERT(shm_frame(idx, 0) != 0 && shm_frame(idx, 1) != 0);
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm"), idx);

    shm_get(idx);              // a second holder
    g_obj[idx].unlinked = 1;
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm"), -1); // no new openers
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);     // still alive

    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 2);
    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);     // last holder freed it
}

KTEST("shm", "two frames of one object are distinct and zeroed") {
    int idx = obj_create("ktest-shm2", 2, 0, 0);
    KTEST_ASSERT(idx >= 0);
    uint64_t a = shm_frame(idx, 0), b = shm_frame(idx, 1);
    KTEST_ASSERT(a && b && a != b);
    for (int i = 0; i < 4096; i++) KTEST_ASSERT_EQ(((uint8_t *)(uintptr_t)a)[i], 0);
    // A write through one frame is not visible in the other -- the
    // check that would fail if a whole object were one aliased page.
    ((uint8_t *)(uintptr_t)a)[7] = 0xAB;
    KTEST_ASSERT_EQ(((uint8_t *)(uintptr_t)b)[7], 0);
    shm_put(idx);
}

KTEST("shm", "a name is refused before a slot is spent on it") {
    KTEST_ASSERT_EQ(name_ok(""), 0);
    KTEST_ASSERT_EQ(name_ok("a/b"), 0);
    KTEST_ASSERT_EQ(name_ok(".."), 0);
    KTEST_ASSERT_EQ(name_ok("snd.3"), 1);
}

KTEST("shm", "a dead creator's NAME goes, so a server notices") {
    // The deadlock this exists to break: a server holds a reference to
    // each client's object, so the object outlives the client -- and
    // while it lives the name is in the namespace, which is the only
    // thing the server can watch. Without this the name never goes.
    int idx = obj_create("ktest-shm4", 1, 42, 0xBEEF000);
    KTEST_ASSERT(idx >= 0);
    shm_get(idx);                       // stand in for the server's hold
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), idx);

    shm_process_gone(0xBEEF000);        // the creator dies
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), -1); // the name is gone...
    KTEST_ASSERT_EQ((int)shm_npages(idx), 1);      // ...the frames are not

    // And the name is free at once, rather than after the last holder:
    // a process reusing that pid must not collide with the corpse.
    int second = obj_create("ktest-shm4", 1, 42, 0xCAFE000);
    KTEST_ASSERT(second >= 0 && second != idx);
    KTEST_ASSERT_EQ(shm_lookup("ktest-shm4"), second);

    shm_put(idx);                       // the creator's own reference
    KTEST_ASSERT_EQ((int)shm_npages(idx), 1); // the server still holds it
    shm_put(idx);                       // the server lets go
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);
    shm_put(second);
}

KTEST("shm", "a dead address space drops its mappings") {
    int idx = obj_create("ktest-shm3", 1, 0, 0);
    KTEST_ASSERT(idx >= 0);
    KTEST_ASSERT_EQ(shm_map_add(0xDEAD000, idx, 0x9000000000ULL, 1), 0);
    KTEST_ASSERT_EQ(g_obj[idx].refs, 2);
    shm_process_gone(0xDEAD000);
    KTEST_ASSERT_EQ(g_obj[idx].refs, 1);
    shm_put(idx);
    KTEST_ASSERT_EQ((int)shm_npages(idx), 0);
}
