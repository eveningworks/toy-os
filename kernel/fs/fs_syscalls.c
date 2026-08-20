// The path-keyed filesystem syscalls: open, unlink, listdir, mkdir,
// rename, truncate, stat, link, the filesystem's change counter, the
// disk flush -- and the CURRENT DIRECTORY every one of them resolves
// against (see resolve_user_path() near the bottom).
//
// The fd TABLE is not here -- it belongs to the process, not to the
// filesystem, and sockets and pipes share it (kernel/proc/syscall_fd.c).
// What is here is everything that takes a path.
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "vmm.h"
#include "fs.h"
#include "string.h"
#include "tz.h"
#include "kpath.h"    // k_path_resolve() -- one resolution rule, kernel-side
#include "scheduler.h" // struct sched_cwd -- the per-process current directory
#include "ata.h"       // the write-back cache SYS_SYNC flushes
#include "kfmt.h"      // klog_printf
#include <stddef.h>

// SYS_LISTDIR scratch state -- fs_list() (fs.c) takes a plain callback
// with no context/userdata parameter, so there's nowhere to thread "which
// output array, how much room is left" through it directly. Bounce
// through these file-scope globals for the duration of a single
// SYS_LISTDIR call instead: safe because syscalls in this kernel are
// never reentrant or concurrent (same assumption SYS_WIN_* above already
// relies on).
// g_listdir_out is a USER virtual address, not a pointer -- deliberately
// typed as one so it cannot be dereferenced by accident. Each entry is
// copied out with vmm_copy_to_user() as fs_list() reports it; see the
// SYS_LISTDIR arm for why the array isn't bounced through the kernel
// stack in one go.
// Defined at the bottom, beside the cwd it resolves against.
static int resolve_user_path(uint64_t pml4, uint64_t uaddr, char *out);

static uint64_t g_listdir_out = 0;
static uint64_t g_listdir_pml4 = 0;
static uint32_t g_listdir_max = 0;
static uint32_t g_listdir_count = 0;
// Holds the (already-validated, NUL-terminated) directory path for the
// call in progress -- needed alongside `name` to build each entry's own
// full path for the fs_stat() call below (fs_list()'s callback only
// ever hands back the bare last component, per its own doc comment).
static char g_listdir_dir_path[FS_PATH_MAX];

static void listdir_collect(const char *name, uint32_t size, int is_dir) {
    if (g_listdir_count >= g_listdir_max) return;
    struct dirent entry;
    struct dirent *e = &entry;
    k_strcpy(e->name, name);
    e->size = size;
    e->is_dir = (uint32_t)is_dir;

    // Build "<dir>/<name>" (or "/<name>" when dir is just "/") to look
    // up this entry's own timestamps -- see struct dirent's `modified`
    // field comment (syscall_abi.h) for why this is here at all.
    // Zeroed rather than left uninitialized on the rare failure path
    // (shouldn't happen for anything fs_list() itself just reported),
    // so a bug here shows up as an obviously-wrong 0000-00-00 rather
    // than reading stale/garbage struct bytes.
    char full_path[FS_PATH_MAX];
    size_t dl = k_strlen(g_listdir_dir_path);
    k_strcpy(full_path, g_listdir_dir_path);
    if (dl > 1) { // dir isn't just "/" -- needs a separating slash
        if (dl + 1 < FS_PATH_MAX) { full_path[dl] = '/'; full_path[dl + 1] = '\0'; dl++; }
    }
    size_t nl = k_strlen(name);
    if (dl + nl < FS_PATH_MAX) k_strcpy(full_path + dl, name);

    struct fs_stat_info st;
    if (fs_stat(full_path, &st)) {
        // The dirent ABI (syscall_abi.h) deliberately keeps struct
        // rtc_time -- the epoch shape is kernel-internal (fs.h's
        // fs_stat_info), converted back to civil time right here at
        // the boundary so userland (ls -l) is untouched.
        tz_epoch_to_rtc(st.modified, &e->modified);
    } else {
        k_memset(&e->modified, 0, sizeof(e->modified));
    }

    // The range was validated once, before fs_list() started, so this
    // cannot fail -- and if it somehow did, dropping the entry is the
    // right answer, not writing a partial one.
    if (!vmm_copy_to_user(g_listdir_pml4, g_listdir_out + (uint64_t)g_listdir_count * sizeof entry,
                           &entry, sizeof entry)) {
        return;
    }
    g_listdir_count++;
}

int sys_open(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;

    // Path length isn't known up front -- validate the max any
    // fs.c name can be (FS_PATH_MAX) rather than dereference an
    // unvalidated pointer to find out. A caller whose actual buffer
    // is shorter than that (but still followed by unmapped memory)
    // would get rejected here even if the real string is safely
    // NUL-terminated well before the end -- an acceptable tradeoff
    // for a path buffer this small.
    char name[FS_PATH_MAX];
    int perr = resolve_user_path(pml4, c->a0, name);
    if (perr) {
        klog_write("syscall: open() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)perr;
    } else {

        uint32_t flags = (uint32_t)c->a1;
        int want_write = (flags & SYS_O_WRITE) != 0;
        int want_creat = (flags & SYS_O_CREAT) != 0;
        int want_trunc = (flags & SYS_O_TRUNC) != 0;

        uint32_t existing_size = 0;
        int exists = fs_read(name, &existing_size) != 0;

        if (!exists && !(want_write && want_creat)) {
            klog_write("syscall: open() rejected -- file not found\n");
            c->regs[14] = (uint64_t)(int64_t)-ENOENT;
        } else {
            // A DESCRIPTION plus a descriptor naming it. Two steps
            // rather than one because dup2 can later point a second
            // descriptor at this same open file.
            int di = fd_desc_alloc(FD_KIND_FILE, -1);
            int fd = di >= 0 ? fd_install(pml4, di) : -1;
            if (fd < 0) {
                if (di >= 0) fd_desc_unref(di);
                // EMFILE, NOT ENOENT -- these two being the same answer
                // is the concrete failure that motivated error codes at
                // all: /bin/tosh probes each PATH candidate with open()
                // and reads any failure as "not there", so a machine out
                // of descriptors reported "command not found".
                klog_write("syscall: open() rejected -- fd table full\n");
                c->regs[14] = (uint64_t)(int64_t)-EMFILE;
            } else {
                if (want_write) {
                    if (!exists) fs_touch(name);
                    if (want_trunc) fs_write(name, "", 0); // 0 = overwrite, not append
                }
                k_strcpy(fd_desc[di].file.name, name);
                fd_desc[di].file.mode = want_write ? FD_MODE_WRITE : FD_MODE_READ;
                fd_desc[di].file.offset = 0;
                c->regs[14] = (uint64_t)fd;
            }
        }
    }
    return 0;
}

int sys_unlink(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    char name[FS_PATH_MAX];
    if (resolve_user_path(pml4, c->a0, name)) {
        // 0, NOT -EFAULT. This call reports success as 1 and failure as
        // 0 -- the opposite polarity to everything error codes were
        // added for -- so a negative code here would be TRUTHY and every
        // `if (!sys_unlink(p))` caller would read a failure as success.
        // Flipping it is a caller-visible change and belongs in its own
        // commit; see docs/roadmap.md's item on the boolean-returning
        // syscalls, which still cannot say why they refused.
        klog_write("syscall: unlink() rejected -- invalid path pointer\n");
        c->regs[14] = 0;
    } else {
        c->regs[14] = (uint64_t)fs_delete(name);
    }
    return 0;
}

int sys_listdir(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    uint32_t max = (uint32_t)c->a2;
    if (max > SYS_LISTDIR_MAX) max = SYS_LISTDIR_MAX;

    char path[FS_PATH_MAX];
    if (!vmm_validate_user_range(pml4, c->a1, (uint64_t)max * sizeof(struct dirent)) ||
        resolve_user_path(pml4, c->a0, path)) {
        klog_write("syscall: listdir() rejected -- invalid pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        // The output array stays a USER address here, and
        // listdir_collect() copies each entry out individually --
        // fs_list() calls back per entry, so there is no single
        // moment when the whole array could be copied at once, and
        // SYS_LISTDIR_MAX of them is far too much to bounce through
        // an 8 KiB kernel stack. The range is validated up front so
        // each per-entry copy is a walk, not a second check.
        g_listdir_out = c->a1;
        g_listdir_pml4 = pml4;
        g_listdir_max = max;
        g_listdir_count = 0;
        k_strcpy(g_listdir_dir_path, path); // see listdir_collect()'s per-entry fs_stat()
        fs_list(path, listdir_collect);
        c->regs[14] = g_listdir_count;
        g_listdir_out = 0; // don't leave a stale user pointer armed
                            // between calls -- next call re-arms it

        // AN EMPTY DIRECTORY AND A MISSING ONE BOTH LEAVE THE COUNT AT
        // ZERO, because fs_list() returns void and "does nothing" is its
        // documented behaviour for a path that is not a listable
        // directory (fs.h). So `ls /nope` printed an empty listing and
        // exited 0, which is the one thing a caller must not conclude.
        //
        // Disambiguated HERE rather than by giving fs_list() a return
        // value: every backend would have to grow one, and the VFS is
        // not where this matters -- a syscall is, because ring 3 is the
        // only caller that cannot look for itself. Same shape as
        // opendir(), which is where ENOENT/ENOTDIR come from.
        //
        // Only on the zero path, so an ordinary listing pays nothing:
        // the two probes are directory walks, and a non-empty result has
        // already proved the directory exists by producing its children.
        if (g_listdir_count == 0) {
            if (!fs_exists(path))      c->regs[14] = (uint64_t)(int64_t)-ENOENT;
            else if (!fs_is_dir(path)) c->regs[14] = (uint64_t)(int64_t)-ENOTDIR;
        }
    }
    return 0;
}

int sys_fs_generation(struct syscall_ctx *c) {
    // One integer, no user pointer to validate -- see
    // syscall_abi.h for why this is not a SYS_SYSINFO field.
    c->regs[14] = fs_generation();
    return 0;
}

// ---- the current directory -------------------------------------------
//
// The cwd itself lives in the scheduler (api/scheduler.h) beside the
// heap, for the same reason: a scheduler slot holds one and the kernel
// context holds a single one of the same type. What is here is the
// syscall half -- and resolve_user_path(), which EVERY path argument in
// this file goes through.

// Copies a path argument out of user memory and resolves it against the
// caller's cwd into `out` (FS_PATH_MAX). Returns 0, or the negative
// errno to hand back.
//
// EVERY path-taking syscall goes through this, including the three that
// predate the cwd. That is the point: a relative path used to be
// silently root-relative, so `cat notes.txt` meant a different file
// depending on whether the shell that typed it resolved first. An
// absolute path is unchanged by resolution, so nothing that already
// passed one behaves differently.
static int resolve_user_path(uint64_t pml4, uint64_t uaddr, char *out) {
    char raw[FS_PATH_MAX];
    if (!vmm_copy_string_from_user(pml4, raw, uaddr, FS_PATH_MAX)) return -EFAULT;
    // k_path_resolve() refuses rather than truncates -- a shortened path
    // names a different file, which is the failure mode worth avoiding.
    if (!k_path_resolve(scheduler_cwd(), raw, out, FS_PATH_MAX)) return -ENAMETOOLONG;
    return 0;
}

int sys_chdir(struct syscall_ctx *c) {
    char path[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write("syscall: chdir() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
        return 0;
    }
    if (!fs_exists(path) && k_strcmp(path, "/") != 0) {
        // "/" has no entry of its own (fs.h), so it would fail an
        // existence test while being the one directory always present.
        klog_write("syscall: chdir() rejected -- no such directory\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (k_strcmp(path, "/") != 0 && !fs_is_dir(path)) {
        klog_write("syscall: chdir() rejected -- not a directory\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOTDIR;
    } else {
        struct sched_cwd *cw = scheduler_current_cwd();
        if (cw) k_strlcpy(cw->path, path, sizeof cw->path);
        else scheduler_set_kernel_cwd(path);
        c->regs[14] = 0;
    }
    return 0;
}

int sys_getcwd(struct syscall_ctx *c) {
    const char *cwd = scheduler_cwd();
    size_t len = k_strlen(cwd);
    if (c->a1 <= len) {
        // -ERANGE, never a truncated path: half a path is a different
        // directory, not a shorter answer to the same question.
        c->regs[14] = (uint64_t)(int64_t)-ERANGE;
    } else if (!vmm_copy_to_user(c->pml4, c->a0, cwd, len + 1)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        c->regs[14] = (uint64_t)len;
    }
    return 0;
}

// The five that are one fs.h call each. They share a shape: resolve,
// call, map "it returned 0" onto a reason. The reasons are guesses only
// where fs.h genuinely cannot say more -- fs_mkdir() reports one 0 for
// "exists", "no parent" and "the disk refused", so the ones this CAN
// distinguish are checked first rather than collapsed into -EIO.
int sys_mkdir(struct syscall_ctx *c) {
    char path[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write("syscall: mkdir() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (fs_exists(path)) {
        klog_write("syscall: mkdir() rejected -- already exists\n");
        c->regs[14] = (uint64_t)(int64_t)-EEXIST;
    } else if (!fs_mkdir(path)) {
        klog_write("syscall: mkdir() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOENT; // missing parent is the usual cause
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_rename(struct syscall_ctx *c) {
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, from);
    if (!err) err = resolve_user_path(c->pml4, c->a1, to);
    if (err) {
        klog_write("syscall: rename() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_exists(from)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (fs_exists(to)) {
        c->regs[14] = (uint64_t)(int64_t)-EEXIST;
    } else if (!fs_rename(from, to)) {
        // Includes the one case a v1 TFS3 journal genuinely cannot do
        // (a cross-parent directory move needs five credits) -- see
        // fs.h. -EIO rather than a guess at which of several it was.
        klog_write("syscall: rename() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_truncate(struct syscall_ctx *c) {
    char path[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write("syscall: truncate() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_exists(path)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (fs_is_dir(path)) {
        c->regs[14] = (uint64_t)(int64_t)-EISDIR;
    } else if (!fs_truncate(path, c->a1)) {
        klog_write("syscall: truncate() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_stat(struct syscall_ctx *c) {
    char path[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write("syscall: stat() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
        return 0;
    }
    if (!vmm_validate_user_range(c->pml4, c->a1, sizeof(struct sys_stat))) {
        klog_write("syscall: stat() rejected -- invalid output pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    int is_root = k_strcmp(path, "/") == 0;
    if (!is_root && !fs_exists(path)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
        return 0;
    }

    struct sys_stat out;
    k_memset(&out, 0, sizeof out);
    out.is_dir = (uint32_t)(is_root || fs_is_dir(path));
    if (!out.is_dir) out.size = fs_size(path);
    if (fs_has(FS_CAP_INODES)) out.flags |= SYS_STAT_INODES;

    struct fs_stat_info st;
    if (fs_stat(path, &st)) {
        out.ino = st.ino;
        // Epoch is kernel-internal (fs.h); civil time crosses the
        // boundary, exactly as struct dirent's `modified` does.
        tz_epoch_to_rtc(st.created, &out.created);
        tz_epoch_to_rtc(st.modified, &out.modified);
    }
    // Only the implicit root reaches fs_stat() failing, and its zeroed
    // timestamps are the honest answer: it has no entry to carry any.
    vmm_copy_to_user(c->pml4, c->a1, &out, sizeof out); // validated above
    c->regs[14] = 0;
    return 0;
}

int sys_link(struct syscall_ctx *c) {
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    int err = resolve_user_path(c->pml4, c->a0, from);
    if (!err) err = resolve_user_path(c->pml4, c->a1, to);
    if (err) {
        klog_write("syscall: link() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_has(FS_CAP_HARDLINKS)) {
        // A distinct code, because "this filesystem's FORMAT has no link
        // counts" is a permanent property of the volume rather than
        // something about these two paths -- the `ln` shell command
        // already says so, and a program deserves the same distinction.
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else if (!fs_exists(from)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (fs_is_dir(from)) {
        c->regs[14] = (uint64_t)(int64_t)-EISDIR;
    } else if (fs_exists(to)) {
        c->regs[14] = (uint64_t)(int64_t)-EEXIST;
    } else if (!fs_link(from, to)) {
        klog_write("syscall: link() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    return 0;
}

int sys_sync(struct syscall_ctx *c) {
    if (!ata_cache_active()) {
        // Not a failure: with no write-back cache in front of the disk,
        // every write has already reached it. 0 sectors is the truth.
        c->regs[14] = 0;
        return 0;
    }
    uint32_t wrote = 0, pending = 0;
    int ok = ata_sync(&wrote, &pending);
    if (!ok) {
        // The one disk answer a caller must not read as success: those
        // sectors exist in RAM only, and powering off loses them.
        klog_printf("syscall: sync() FAILED -- %u sector(s) still pending\n", pending);
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = wrote;
    }
    return 0;
}
