// FILES SENT THROUGH THE DEBUGGER -- gdb's `remote put` over vFile
// packets (gdbstub.c), staged here, written to disk by /bin/kdfiled.
// kdfile_abi.h has the contract.
//
// TWO HALVES, AND THE SPLIT IS THE WHOLE DESIGN. While the machine is
// stopped the stub may not touch the filesystem: it may have stopped
// inside kmalloc, holding a mount lock, in the middle of a disk wait. So
// the stopped half only COPIES bytes into a staging area reserved when
// the stub was armed, and the writing happens after resume, in a process,
// with ordinary locking. Windows' `.kdfiles` has the same rule: the
// target pulls a file from the debugger at a point of ITS choosing.
#include "kdebug_internal.h"
#include "kdfile_abi.h"
#include "ksha256.h"
#include "pmm.h"
#include "scheduler.h"
#include "syscalls.h"
#include "syscall_table.h"
#include "vmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"

#define STAGE_BYTES (8u << 20)   // one kernel with room to spare
#define STAGE_FILES 4

enum { F_FREE, F_OPEN, F_READY, F_TAKEN };

struct staged {
    char     path[KDFILE_PATH_MAX];
    uint64_t base, size;
    uint8_t  state;
    uint8_t  bad;       // a write failed: never handed to kdfiled
    int      owner;     // the process that TOOK it (F_TAKEN)
    char     sha256[65];
};

static uint8_t *g_stage;           // NULL: not armed, or no memory for it
static uint64_t g_next;            // bump allocator over g_stage
static struct staged g_files[STAGE_FILES];
static char g_ready_chan;          // kdfiled parks here

void kdb_files_init(void) {
    uint64_t phys = pmm_alloc_contiguous(STAGE_BYTES / 4096, PMM_ZONE_ANY);
    g_stage = phys ? (uint8_t *)(uintptr_t)phys : 0;
    if (!g_stage)
        klog_write(KLOG_WARN "kdebug: no 8 MiB for staging -- `remote put` is off\n");
}

// --- the stopped half: no lock, no allocation, no log ----------------------

int kdb_stage_open(const char *path) {
    if (!g_stage) return -EACCES;
    if (path[0] != '/') return -EINVAL;
    if (k_strlen(path) >= KDFILE_PATH_MAX) return -ENAMETOOLONG;
    // GDB opens one file at a time, so an open one is a put that was
    // abandoned (the link dropped, gdb went away): reclaim it, or every
    // later put this boot would get EMFILE.
    for (int i = 0; i < STAGE_FILES; i++) if (g_files[i].state == F_OPEN) g_files[i].state = F_FREE;
    for (int i = 0; i < STAGE_FILES; i++) {
        struct staged *f = &g_files[i];
        if (f->state != F_FREE) continue;
        k_strlcpy(f->path, path, sizeof f->path);
        f->base = g_next;
        f->size = 0;
        f->bad = 0;
        f->state = F_OPEN;
        return i + 1;
    }
    return -EMFILE;
}

// The open file owns everything from its base to the end of the area,
// since its final size is unknown until close.
int64_t kdb_stage_write(int fd, uint64_t off, const uint8_t *data, uint32_t len) {
    if (fd < 1 || fd > STAGE_FILES || g_files[fd - 1].state != F_OPEN) return -EBADF;
    struct staged *f = &g_files[fd - 1];
    if (off > STAGE_BYTES - f->base || len > STAGE_BYTES - f->base - off) {
        f->bad = 1;
        return -ENOSPC;
    }
    k_memcpy(g_stage + f->base + off, data, len);
    if (off + len > f->size) f->size = off + len;
    return len;
}

void kdb_stage_fail(int fd) {
    if (fd >= 1 && fd <= STAGE_FILES && g_files[fd - 1].state == F_OPEN) g_files[fd - 1].bad = 1;
}

// A FILE THAT EVER FAILED A WRITE IS DISCARDED HERE, never written: gdb
// closes after an error, and a partial file handed on would replace the
// real one (kernel.bin moved to kernel.old, a truncated one installed).
int kdb_stage_close(int fd) {
    if (fd < 1 || fd > STAGE_FILES || g_files[fd - 1].state != F_OPEN) return -EBADF;
    struct staged *f = &g_files[fd - 1];
    if (f->bad) {
        f->state = F_FREE;
        return -EIO;
    }
    static const char hex[] = "0123456789abcdef";
    struct ksha256 c;
    uint8_t d[KSHA256_LEN];
    ksha256_init(&c);
    ksha256_update(&c, g_stage + f->base, (size_t)f->size);
    ksha256_final(&c, d);
    for (int i = 0; i < KSHA256_LEN; i++) {
        f->sha256[i * 2] = hex[d[i] >> 4];
        f->sha256[i * 2 + 1] = hex[d[i] & 15];
    }
    f->sha256[64] = 0;
    g_next = (f->base + f->size + 4095) & ~4095ULL;
    f->state = F_READY;
    return 0;
}

// On resume, not while stopped: wake kdfiled if a file is waiting.
void kdb_stage_kick(void) {
    for (int i = 0; i < STAGE_FILES; i++)
        if (g_files[i].state == F_READY) { scheduler_wake(&g_ready_chan, 0); return; }
}

// --- the running half: SYS_KDFILE, for kdfiled -------------------------------

static int handle_ok(int64_t h) {
    return h >= 1 && h <= STAGE_FILES && g_files[h - 1].state == F_TAKEN &&
           g_files[h - 1].owner == scheduler_current_tgid();
}

int sys_kdfile(struct syscall_ctx *c) {
    int64_t ret = 0;
    if (!kdb.armed || !g_stage) {
        ret = -ENODEV;
    } else if (c->a0 == KDFILE_TAKE) {
        // A taker that died mid-write (a crash, a kill) left its file
        // TAKEN: hand it to whoever asks next, or it and its share of the
        // area are gone until reboot.
        for (int i = 0; i < STAGE_FILES; i++)
            if (g_files[i].state == F_TAKEN && !scheduler_pid_alive(g_files[i].owner))
                g_files[i].state = F_READY;
        for (int i = 0; i < STAGE_FILES; i++) {
            struct staged *f = &g_files[i];
            if (f->state != F_READY) continue;
            struct kdfile_info info;
            k_memset(&info, 0, sizeof info);
            k_strlcpy(info.path, f->path, sizeof info.path);
            info.size = f->size;
            k_memcpy(info.sha256, f->sha256, sizeof info.sha256);
            if (!vmm_copy_to_user(c->pml4, c->a1, &info, sizeof info)) { ret = -EFAULT; break; }
            f->state = F_TAKEN;
            f->owner = scheduler_current_tgid();
            c->regs[14] = (uint64_t)(i + 1);
            return 0;
        }
        if (!ret) {
            c->regs[14] = 0;   // a wake returns 0: ask again
            scheduler_block_current(c->regs, &g_ready_chan, SCHED_WAIT_EVENT);
            return 0;
        }
    } else if (c->a0 == KDFILE_READ) {
        struct kdfile_read r;
        if (!vmm_copy_from_user(c->pml4, &r, c->a1, sizeof r)) {
            ret = -EFAULT;
        } else if (!handle_ok(r.handle)) {
            ret = -EBADF;
        } else {
            struct staged *f = &g_files[r.handle - 1];
            uint64_t n = r.offset >= f->size ? 0 : f->size - r.offset;
            if (n > r.len) n = r.len;
            if (n && !vmm_copy_to_user(c->pml4, r.buf, g_stage + f->base + r.offset, n))
                ret = -EFAULT;
            else
                ret = (int64_t)n;
        }
    } else if (c->a0 == KDFILE_DONE) {
        if (!handle_ok((int64_t)c->a1)) {
            ret = -EBADF;
        } else {
            struct staged *f = &g_files[c->a1 - 1];
            int64_t st = (int64_t)c->a2;
            if (st == 0)
                klog_printf("kdebug: kdfiled wrote %s, %lu bytes, sha256 %s\n",
                            f->path, f->size, f->sha256);
            else
                klog_printf(KLOG_ERR "kdebug: kdfiled could not write %s (%ld)\n",
                            f->path, st);
            f->state = F_FREE;
            int busy = 0;
            for (int i = 0; i < STAGE_FILES; i++) busy |= g_files[i].state != F_FREE;
            if (!busy) g_next = 0;   // the whole area is free again
        }
    } else {
        ret = -EINVAL;
    }
    c->regs[14] = (uint64_t)ret;
    return 0;
}
