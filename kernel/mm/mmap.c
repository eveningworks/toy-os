// SYS_MMAP / SYS_MUNMAP: file- and zero-backed mappings in their own
// arena (uaddr.h), demand-paged through the same hook as the heap and
// stack. The metadata is struct sched_mm's fixed region array, so it
// lives and dies with the process slot; the frames are ordinary owned
// user pages, so address-space teardown frees them with everything
// else and munmap is the only path that must free them by hand.
//
// docs/decisions.md (kernel) has the two calls this file made that a
// reader will want re-litigated: a file region is remembered by
// absolute path rather than by pinning the fd, and MAP_FIXED refuses
// overlap instead of replacing.

#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "mmap.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "fs.h"
#include "uaddr.h"
#include "string.h"
#include "kfmt.h"     // klog_printf

// The region containing `addr`, or NULL. Linear: MMAP_MAX_REGIONS is
// 16 and this runs per fault-in and per syscall, not per byte.
static struct mmap_region *region_of(struct sched_mm *mm, uint64_t addr) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        struct mmap_region *r = &mm->regions[i];
        if (!r->base) continue;
        if (addr >= r->base && addr < r->base + r->npages * 4096ULL) return r;
    }
    return 0;
}

static struct mmap_region *free_slot(struct sched_mm *mm) {
    for (int i = 0; i < MMAP_MAX_REGIONS; i++)
        if (!mm->regions[i].base) return &mm->regions[i];
    return 0;
}

// Any live region intersecting [base, base + npages*4096)?
static struct mmap_region *overlap_of(struct sched_mm *mm, uint64_t base,
                                      uint64_t npages) {
    uint64_t end = base + npages * 4096ULL;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        struct mmap_region *r = &mm->regions[i];
        if (!r->base) continue;
        uint64_t rend = r->base + r->npages * 4096ULL;
        if (base < rend && r->base < end) return r;
    }
    return 0;
}

// First-fit over the arena. Starts at the arena base and hops over
// whichever region it collides with; the list is unsorted, so each hop
// restarts the scan -- bounded by the region count, not by the arena.
static uint64_t arena_pick(struct sched_mm *mm, uint64_t npages) {
    uint64_t addr = UADDR_MMAP_BASE;
    for (int hops = 0; hops <= MMAP_MAX_REGIONS; hops++) {
        if (addr + npages * 4096ULL > UADDR_MMAP_LIMIT) return 0;
        struct mmap_region *hit = overlap_of(mm, addr, npages);
        if (!hit) return addr;
        addr = hit->base + hit->npages * 4096ULL;
    }
    return 0;
}

// WHOSE mappings: same resolution sys_sbrk uses -- the scheduler's
// notion of who is running, which makes a thread resolve to its
// group's list. No legacy-loader fallback on purpose: the legacy `run`
// path predates fds, and a program old enough to need it has no
// business mmapping.
static struct sched_mm *caller_mm(void) { return scheduler_current_mm(); }

int sys_mmap(struct syscall_ctx *c) {
    struct sched_mm *mm = caller_mm();
    struct mmap_msg m;
    int64_t ret;

    if (!mm) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) {
        ret = -EFAULT; goto out;
    }

    // PROT_READ is required and nothing unknown may be set: a bit this
    // kernel does not implement must refuse, not silently grant.
    if (!(m.prot & SYS_PROT_READ) ||
        (m.prot & ~(SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC))) {
        ret = -EINVAL; goto out;
    }
    if (!(m.flags & SYS_MAP_PRIVATE) ||
        (m.flags & ~(SYS_MAP_PRIVATE | SYS_MAP_FIXED | SYS_MAP_ANONYMOUS))) {
        ret = -EINVAL; goto out;
    }
    if (!m.length || (m.offset & 0xFFF)) { ret = -EINVAL; goto out; }

    uint64_t npages = (m.length + 4095) / 4096;

    struct open_file *f = 0;
    if (!(m.flags & SYS_MAP_ANONYMOUS)) {
        f = fd_get(c->pml4, m.fd);
        // "Open the wrong way" is EBADF here (errno.h's own words): a
        // mapping is a read of the file, so a write-mode fd cannot
        // back one.
        if (!f || f->kind != FD_KIND_FILE || f->file.mode != FD_MODE_READ) {
            ret = -EBADF; goto out;
        }
    }

    uint64_t base;
    if (m.flags & SYS_MAP_FIXED) {
        base = m.addr;
        if ((base & 0xFFF) || !uaddr_is_mmap_range(base) ||
            base + npages * 4096ULL > UADDR_MMAP_LIMIT) {
            ret = -EINVAL; goto out;
        }
        if (overlap_of(mm, base, npages)) { ret = -EEXIST; goto out; }
    } else {
        // The hint is honoured only as FIXED would be, minus the
        // demand: usable means aligned, in-arena and free, and
        // anything else falls back to the allocator's pick.
        base = 0;
        if (m.addr && !(m.addr & 0xFFF) && uaddr_is_mmap_range(m.addr) &&
            m.addr + npages * 4096ULL <= UADDR_MMAP_LIMIT &&
            !overlap_of(mm, m.addr, npages))
            base = m.addr;
        if (!base) base = arena_pick(mm, npages);
        if (!base) { ret = -ENOMEM; goto out; }
    }

    struct mmap_region *r = free_slot(mm);
    if (!r) { ret = -ENOMEM; goto out; }

    r->npages   = npages;
    r->prot     = (uint8_t)m.prot;
    r->file_off = 0;
    r->kind     = MMAP_KIND_ANON;
    r->path[0]  = '\0';
    if (f) {
        r->kind     = MMAP_KIND_FILE;
        r->file_off = m.offset;
        k_strlcpy(r->path, f->file.name, sizeof r->path);
    }
    r->base = base; // last: a non-zero base is what makes the slot live

    ret = (int64_t)base;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_munmap(struct syscall_ctx *c) {
    struct sched_mm *mm = caller_mm();
    uint64_t addr = c->a0, len = c->a1;
    int64_t ret = 0;

    if (!mm) { ret = -EPERM; goto out; }
    if (!len || (addr & 0xFFF) || (len & 0xFFF) ||
        !uaddr_is_mmap_range(addr)) {
        ret = -EINVAL; goto out;
    }

    uint64_t npages = len / 4096;
    uint64_t end = addr + len;
    struct mmap_region *r = region_of(mm, addr);
    uint64_t rend;

    // The whole range must lie inside ONE region -- an edge trim, the
    // middle, or all of it. POSIX allows a range spanning several
    // mappings and holes; nothing here needs that yet, and a refusal
    // can be widened where a silent partial unmap cannot be recalled.
    if (!r) { ret = -EINVAL; goto out; }
    rend = r->base + r->npages * 4096ULL;
    if (end > rend) { ret = -EINVAL; goto out; }

    // A middle cut needs a slot for the tail BEFORE anything is
    // unmapped, so a full table refuses with every page still mapped
    // rather than half-applying.
    struct mmap_region *tail = 0;
    if (addr > r->base && end < rend) {
        tail = free_slot(mm);
        if (!tail) { ret = -ENOMEM; goto out; }
    }

    for (uint64_t p = addr; p < end; p += 4096)
        vmm_release_user_page(c->pml4, p); // untouched pages: nothing there

    if (addr == r->base && end == rend) {
        r->base = 0;
    } else if (addr == r->base) {          // front trim
        r->base = end;
        r->npages -= npages;
        r->file_off += len;
    } else if (end == rend) {              // back trim
        r->npages -= npages;
    } else {                               // middle: r keeps the head
        tail->npages   = (rend - end) / 4096;
        tail->prot     = r->prot;
        tail->kind     = r->kind;
        tail->file_off = r->file_off + (end - r->base);
        k_strlcpy(tail->path, r->path, sizeof tail->path);
        tail->base     = end;
        r->npages      = (addr - r->base) / 4096;
    }

out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- the /lib image cache --------------------------------------------
//
// Every dynamic process faults the same libc.so pages in, and without
// this each fault was a preempt-disabled disk read PER PROCESS -- the
// whole machine stalled a little for every page of every spawn, which
// surfaced as GUI tests flaking under load. One cache, keyed by
// (path, offset), filled on first touch systemwide:
//
//   - a READ-ONLY page (text, rodata) is mapped BORROWED into every
//     process -- one frame total, the design's frame sharing;
//   - a WRITABLE page (data, GOT) is COPIED from the cache -- private
//     by definition, but a memcpy instead of a disk read.
//
// /lib ONLY, by policy: its contents are immutable within a boot (the
// build seeds them), which is what makes "never invalidated, never
// evicted" honest rather than a leak -- the cache is bounded by the
// size of /lib itself. An ordinary file-backed mmap stays uncached,
// because a file a process can rewrite must not serve stale pages.
#define IMGCACHE_MAX 512 // frames -- 2 MiB, several /lib's worth

struct imgcache_ent {
    char     path[64];
    uint64_t off;
    uint64_t frame;
};
static struct imgcache_ent g_imgcache[IMGCACHE_MAX];
static int g_imgcache_n;

static uint64_t imgcache_get(const char *path, uint64_t off) {
    for (int i = 0; i < g_imgcache_n; i++) {
        if (g_imgcache[i].off == off && !k_strcmp(g_imgcache[i].path, path))
            return g_imgcache[i].frame;
    }
    if (g_imgcache_n >= IMGCACHE_MAX) return 0; // full: caller reads the disk

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!frame) return 0;
    for (int i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;
    fs_read_range(path, off, (void *)(uintptr_t)frame, 4096);
    struct imgcache_ent *e = &g_imgcache[g_imgcache_n++];
    k_strlcpy(e->path, path, sizeof e->path);
    e->off = off;
    e->frame = frame;
    return frame;
}

int mmap_fault_in(struct sched_mm *mm, uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t page = vaddr & ~0xFFFULL;
    struct mmap_region *r = region_of(mm, page);
    if (!r) return 0; // a wild pointer that happens to land in the arena

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!frame) {
        klog_printf("mm: no frame for mmap page %#lx -- the process dies here\n",
                    page);
        return 0;
    }
    for (int i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;

    if (r->kind == MMAP_KIND_FILE) {
        // THE RE-ENTRANCY GUARD. This read may run from the copy
        // helpers' fault-in, and if THAT ever fires while an FS_OP
        // holds the preemption guard, the read below would re-enter
        // the backend's module-level scratch state -- recursion the
        // guard itself cannot see (vfs.c). No such path exists today:
        // backends touch only kernel buffers, and syscalls fault user
        // ranges in before FS_OP. This refusal is what keeps that an
        // invariant rather than an accident.
        if (scheduler_preempt_depth() > 0) {
            klog_printf("mm: refused file-backed fault-in of %#lx inside an "
                        "FS_OP -- see mmap_fault_in()\n", page);
            pmm_free_frame(frame);
            return 0;
        }
        // A file that has vanished since mmap() kills the toucher
        // loudly, where a short read past EOF is legitimately zeros.
        if (!fs_exists(r->path)) {
            klog_printf("mm: mmap backing file %s is gone -- fault at %#lx "
                        "stays fatal\n", r->path, page);
            pmm_free_frame(frame);
            return 0;
        }
        uint64_t off = r->file_off + (page - r->base);
        uint64_t cached = k_strncmp(r->path, "/lib/", 5) == 0
                              ? imgcache_get(r->path, off) : 0;
        if (cached && !(r->prot & SYS_PROT_WRITE)) {
            // Shared: every process maps the SAME frame, read-only.
            pmm_free_frame(frame);
            if (!vmm_map_user_borrowed(pml4_phys, page, cached,
                                       0, (r->prot & SYS_PROT_EXEC) != 0,
                                       0 /* VMM_MT_NORMAL */))
                return 0;
            return 1;
        }
        if (cached) {
            k_memcpy((void *)(uintptr_t)frame, (void *)(uintptr_t)cached, 4096);
        } else {
            fs_read_range(r->path, off, (void *)(uintptr_t)frame, 4096);
        }
    }

    if (!vmm_map_user_page_flags(pml4_phys, page, frame,
                                 (r->prot & SYS_PROT_WRITE) != 0,
                                 (r->prot & SYS_PROT_EXEC) != 0)) {
        pmm_free_frame(frame);
        return 0;
    }
    return 1;
}
