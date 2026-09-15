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
#include "caltime.h"
#include "string.h"
#include "tz.h"
#include "kpath.h"    // k_path_resolve() -- one resolution rule, kernel-side
#include "kpath_buf.h" // a path is 4096 now and may not be a kernel local
#include "scheduler.h" // struct sched_cwd -- the per-process current directory
#include "ata.h"       // the ATA write-back cache SYS_SYNC writes back
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
static uint32_t g_listdir_start = 0;  // SYS_LISTDIR_AT's offset
static uint32_t g_listdir_seen = 0;   // entries the walk has passed, skipped or not
// Holds the (already-validated, NUL-terminated) directory path for the
// call in progress -- needed alongside `name` to build each entry's own
// full path for the fs_stat() call below (fs_list()'s callback only
// ever hands back the bare last component, per its own doc comment).
static char g_listdir_dir_path[FS_PATH_MAX];

static void listdir_collect(const char *name, uint32_t size, int is_dir) {
    // SKIP the first `g_listdir_start` entries -- this is SYS_LISTDIR_AT's
    // offset, applied here because fs_list() has no cursor of its own and
    // walking to it is what a backend does anyway.
    if (g_listdir_seen++ < g_listdir_start) return;
    if (g_listdir_count >= g_listdir_max) return;
    struct sys_dirent entry;
    struct sys_dirent *e = &entry;
    k_strlcpy(e->name, name, sizeof e->name);
    e->size = size;
    e->is_dir = (uint32_t)is_dir;

    // Build "<dir>/<name>" (or "/<name>" when dir is just "/") to look
    // up this entry's own timestamps -- see struct sys_dirent's `modified`
    // field comment (syscall_abi.h) for why this is here at all.
    // Zeroed rather than left uninitialized on the rare failure path
    // (shouldn't happen for anything fs_list() itself just reported),
    // so a bug here shows up as an obviously-wrong 0000-00-00 rather
    // than reading stale/garbage struct bytes.
    // Static, not a local: this is a per-ENTRY callback, so allocating
    // here would be one kmalloc per directory entry -- and the listdir
    // path above it is already module-level scratch for the same
    // non-reentrancy reason.
    static char full_path[FS_PATH_MAX];
    size_t dl = k_strlen(g_listdir_dir_path);
    k_strlcpy(full_path, g_listdir_dir_path, sizeof full_path);
    if (dl > 1) { // dir isn't just "/" -- needs a separating slash
        if (dl + 1 < FS_PATH_MAX) { full_path[dl] = '/'; full_path[dl + 1] = '\0'; dl++; }
    }
    size_t nl = k_strlen(name);
    if (dl + nl < FS_PATH_MAX) k_strlcpy(full_path + dl, name, FS_PATH_MAX - dl);

    struct fs_stat_info st;
    if (fs_stat(full_path, &st)) {
        // The dirent ABI (syscall_abi.h) deliberately keeps struct
        // rtc_time -- the epoch shape is kernel-internal (fs.h's
        // fs_stat_info), converted back to civil time right here at
        // the boundary so userland (ls -l) is untouched.
        cal_epoch_to_rtc(st.modified, &e->modified);
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

// Resolve the PARENT as its own step before creating anything: Linux's
// path_parentat() shape, where ENOENT/ENOTDIR come from the walk and
// ENOSPC from the create. fs.h reports one 0 for all three, so a caller
// that skips this can only guess -- and open() discarded even the 0.
static int parent_dir_err(const char *path) {
    char *parent = kpath_get();
    if (!parent) return -ENOMEM;
    // Sequential rather than an `else if` chain: check_dispatch.py
    // groups those by brace depth, so three here would join sys_open's
    // flag chain below into one 22-branch run.
    int rc = 0;
    if (!k_path_dirname(path, parent, FS_PATH_MAX)) rc = -ENAMETOOLONG;
    if (!rc && !fs_exists(parent))                  rc = -ENOENT;
    if (!rc && !fs_is_dir(parent))                  rc = -ENOTDIR;
    kpath_put(parent);
    return rc;
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
    char *name = kpath_get();
    if (!name) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int perr = resolve_user_path(pml4, c->a0, name);
    if (perr) {
        klog_write(KLOG_ERR "syscall: open() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)perr;
    } else {

        uint32_t flags = (uint32_t)c->a1;
        int want_write = (flags & SYS_O_WRITE) != 0;
        int want_creat = (flags & SYS_O_CREAT) != 0;
        int want_trunc = (flags & SYS_O_TRUNC) != 0;
        int want_append = (flags & SYS_O_APPEND) != 0;

        // fs_exists(), NOT a whole-file fs_read() to see whether the
        // read succeeds. That probe cost a full read of the file on
        // every open -- and worse, it made open() REPORT -ENOENT for a
        // file too large for the staging buffer, since fs_read()
        // returns NULL for "cannot load this whole" exactly as it does
        // for "not there". A libc that opens real files walks into that
        // immediately.
        //
        // A directory is deliberately NOT "exists" here: opening one
        // used to fail as ENOENT (fs_read() refuses a directory), and
        // an fd naming a directory would be a thing SYS_READ and
        // SYS_FSTAT have no answer for.
        int exists = fs_exists(name) && !fs_is_dir(name);

        // **O_EXCL: EXISTS IS THE FAILURE.** Checked HERE, inside the
        // syscall, because the whole value of the flag is that no other
        // process can create the file between the test and the open --
        // a caller doing fs_exists() then open() has exactly that
        // window, which is why it cannot build a lock file.
        //
        // Refused WITHOUT O_CREAT rather than ignored: POSIX leaves the
        // combination undefined, and a caller who wrote it meant
        // something this cannot provide.
        if (flags & SYS_O_EXCL) {
            if (!want_creat) {
                klog_write(KLOG_ERR "syscall: open() rejected -- O_EXCL without O_CREAT\n");
                c->regs[14] = (uint64_t)(int64_t)-EINVAL;
                kpath_put(name);
                return 0;
            }
            if (exists) {
                c->regs[14] = (uint64_t)(int64_t)-EEXIST;
                kpath_put(name);
                return 0;
            }
        }

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
                //
                // **AND THE TWO TABLES ARE TWO ERRORS.** `fd_desc_alloc`
                // failing means the SYSTEM's open-file descriptions are
                // gone (ENFILE); `fd_install` failing means this process
                // is out of descriptors (EMFILE). Reporting both as
                // EMFILE tells a caller its own table is full when it is
                // not, and a caller that believes it draws a false
                // conclusion from the next call succeeding -- which is
                // what /tests/errno_test did. sys_pipe() below already
                // separates them.
                int e = di < 0 ? -ENFILE : -EMFILE;
                klog_write(di < 0
                    ? "syscall: open() rejected -- no free open-file description\n"
                    : "syscall: open() rejected -- fd table full\n");
                c->regs[14] = (uint64_t)(int64_t)e;
            } else {
                // The descriptor is reserved BEFORE the file work and
                // put back if that work fails -- Linux's
                // get_unused_fd_flags()/put_unused_fd() order, so a
                // refusal here cannot leak the slot it took.
                int ferr = 0;
                const char *why = 0;
                if (want_write) {
                    if (!exists) {
                        ferr = parent_dir_err(name);
                        if (ferr)
                            why = ferr == -ENOTDIR ? "a path component is not a directory"
                                : ferr == -ENOENT  ? "no such directory to create it in"
                                                   : "the path is too long";
                        else if (!fs_touch(name)) {
                            ferr = -ENOSPC; why = "the record table is full";
                        }
                    }
                    // 0 = overwrite, not append. Discarding this left an
                    // fd over a file that still held its old contents.
                    if (!ferr && want_trunc && !fs_write(name, "", 0)) {
                        ferr = -EIO; why = "truncate refused";
                    }
                }
                if (ferr) {
                    fd_close(pml4, fd);
                    klog_printf(KLOG_ERR "syscall: open() rejected -- %s\n", why);
                    c->regs[14] = (uint64_t)(int64_t)ferr;
                    kpath_put(name);
                    return 0;
                }
                k_strlcpy(fd_desc[di].file.name, name, sizeof fd_desc[di].file.name);
                fd_desc[di].file.mode = want_write ? FD_MODE_WRITE : FD_MODE_READ;
                fd_desc[di].file.pos = 0;
                // Read-only fds ignore it, so it is not worth refusing
                // the combination -- but it is worth not SETTING it,
                // since SYS_FSTAT reports nothing about it and a stray
                // flag on a reader would be state nobody could see.
                fd_desc[di].file.append = (uint8_t)(want_write && want_append);
                c->regs[14] = (uint64_t)fd;
            }
        }
    }
    kpath_put(name);
    return 0;
}

int sys_unlink(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    char *name = kpath_get();
    if (!name) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(pml4, c->a0, name);
    if (err) {
        // 0 success / -errno failure since the polarity flip -- the
        // last of the boolean-returning syscalls converted, with every
        // caller in the same commit (docs/errno-design.md).
        klog_write(KLOG_ERR "syscall: unlink() rejected -- invalid path pointer\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_exists(name)) {
        klog_write("syscall: unlink() rejected -- no such file\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (!fs_delete(name)) {
        // Exists and still refused: a non-empty directory is the usual
        // cause, and fs_delete() does not say which it was.
        klog_write(KLOG_ERR "syscall: unlink() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    kpath_put(name);
    return 0;
}

// SYS_LISTDIR and SYS_LISTDIR_AT are one function: the second is the
// first with an offset, and duplicating the validation, the timestamp
// lookup and the empty-vs-missing disambiguation would be two copies of
// the subtle half.
static int listdir_common(struct syscall_ctx *c, uint64_t path_ptr,
                          uint64_t out, uint32_t max, uint32_t start) {
    uint64_t pml4 = c->pml4;
    if (max > SYS_LISTDIR_MAX) max = SYS_LISTDIR_MAX;

    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    if (!vmm_validate_user_range(pml4, out, (uint64_t)max * sizeof(struct sys_dirent)) ||
        resolve_user_path(pml4, path_ptr, path)) {
        klog_write(KLOG_ERR "syscall: listdir() rejected -- invalid pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        // The output array stays a USER address here, and
        // listdir_collect() copies each entry out individually --
        // fs_list() calls back per entry, so there is no single
        // moment when the whole array could be copied at once, and
        // SYS_LISTDIR_MAX of them is far too much to bounce through
        // an 8 KiB kernel stack. The range is validated up front so
        // each per-entry copy is a walk, not a second check.
        g_listdir_out = out;
        g_listdir_pml4 = pml4;
        g_listdir_max = max;
        g_listdir_count = 0;
        g_listdir_seen = 0;
        g_listdir_start = start;
        k_strlcpy(g_listdir_dir_path, path, sizeof g_listdir_dir_path); // see listdir_collect()'s per-entry fs_stat()
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
        // ...but only for the FIRST page. A later page legitimately
        // comes back empty -- that is how a caller learns it has read
        // the whole directory -- and reporting ENOENT for it would turn
        // the end of a listing into a missing directory.
        if (g_listdir_count == 0 && start == 0) {
            if (!fs_exists(path))      c->regs[14] = (uint64_t)(int64_t)-ENOENT;
            else if (!fs_is_dir(path)) c->regs[14] = (uint64_t)(int64_t)-ENOTDIR;
        }
    }
    kpath_put(path);
    return 0;
}

int sys_listdir(struct syscall_ctx *c) {
    return listdir_common(c, c->a0, c->a1, (uint32_t)c->a2, 0);
}

int sys_listdir_at(struct syscall_ctx *c) {
    struct listdir_request req;
    if (!vmm_copy_from_user(c->pml4, &req, c->a0, sizeof req)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    return listdir_common(c, req.path, req.entries, req.max, req.start);
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
    // Three buffers, none of them a local: the raw copy, the scratch
    // k_path_resolve() joins into, and the caller's `out`. At
    // FS_PATH_MAX = 4096 that is 16 KiB, a whole kernel stack.
    char *raw = kpath_get();
    if (!raw) return -ENOMEM;
    struct kpath_scratch sc;
    if (!kpath_scratch_get(&sc)) { kpath_put(raw); return -ENOMEM; }

    int rc = 0;
    if (!vmm_copy_string_from_user(pml4, raw, uaddr, FS_PATH_MAX)) {
        rc = -EFAULT;
    } else if (!k_path_resolve(scheduler_cwd(), raw, out, FS_PATH_MAX, &sc)) {
        // k_path_resolve() refuses rather than truncates -- a shortened
        // path names a different file, which is the failure mode worth
        // avoiding.
        rc = -ENAMETOOLONG;
    }
    kpath_scratch_put(&sc);
    kpath_put(raw);
    return rc;
}

int sys_chdir(struct syscall_ctx *c) {
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write(KLOG_ERR "syscall: chdir() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
        kpath_put(path);
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
    kpath_put(path);
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
// distinguish are checked first (fs_exists(), then parent_dir_err())
// rather than collapsed into one code.
int sys_mkdir(struct syscall_ctx *c) {
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write(KLOG_ERR "syscall: mkdir() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (fs_exists(path)) {
        klog_write("syscall: mkdir() rejected -- already exists\n");
        c->regs[14] = (uint64_t)(int64_t)-EEXIST;
    } else if ((err = parent_dir_err(path)) != 0) {
        klog_write(err == -ENOTDIR
                   ? "syscall: mkdir() rejected -- a path component is not a directory\n"
                   : "syscall: mkdir() rejected -- no such parent directory\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_mkdir(path)) {
        klog_write(KLOG_ERR "syscall: mkdir() failed -- the record table is full\n");
        c->regs[14] = (uint64_t)(int64_t)-ENOSPC;
    } else {
        c->regs[14] = 0;
    }
    kpath_put(path);
    return 0;
}

int sys_rename(struct syscall_ctx *c) {
    char *from = kpath_get();
    if (!from) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    char *to = kpath_get();
    if (!to) { kpath_put(from); c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, from);
    if (!err) err = resolve_user_path(c->pml4, c->a1, to);
    if (err) {
        klog_write(KLOG_ERR "syscall: rename() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_exists(from)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (fs_exists(to)) {
        c->regs[14] = (uint64_t)(int64_t)-EEXIST;
    } else if (!fs_rename(from, to)) {
        // Includes the one case a v1 TFS3 journal genuinely cannot do
        // (a cross-parent directory move needs five credits) -- see
        // fs.h. -EIO rather than a guess at which of several it was.
        klog_write(KLOG_ERR "syscall: rename() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    kpath_put(from); kpath_put(to);
    return 0;
}

int sys_truncate(struct syscall_ctx *c) {
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write(KLOG_ERR "syscall: truncate() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
    } else if (!fs_exists(path)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
    } else if (fs_is_dir(path)) {
        c->regs[14] = (uint64_t)(int64_t)-EISDIR;
    } else if (!fs_truncate(path, c->a1)) {
        klog_write(KLOG_ERR "syscall: truncate() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    kpath_put(path);
    return 0;
}

int sys_stat(struct syscall_ctx *c) {
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write(KLOG_ERR "syscall: stat() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
        kpath_put(path);
        return 0;
    }
    int is_root = k_strcmp(path, "/") == 0;
    if (!is_root && !fs_exists(path)) {
        c->regs[14] = (uint64_t)(int64_t)-ENOENT;
        kpath_put(path);
        return 0;
    }

    struct sys_stat out;
    k_memset(&out, 0, sizeof out);
    out.is_dir = (uint32_t)(is_root || fs_is_dir(path));
    if (!out.is_dir) out.size = fs_size(path);
    if (fs_has(FS_CAP_INODES)) out.flags |= SYS_STAT_INODES;
    if (fs_has(FS_CAP_MODE))   out.flags |= SYS_STAT_MODE;

    struct fs_stat_info st;
    if (fs_stat(path, &st)) {
        out.ino = st.ino;
        // Epoch is kernel-internal (fs.h); civil time crosses the
        // boundary, exactly as struct sys_dirent's `modified` does.
        cal_epoch_to_rtc(st.created, &out.created);
        cal_epoch_to_rtc(st.modified, &out.modified);
        out.mode = st.mode;
        out.nlink = st.nlink;
    } else {
        // THE IMPLICIT ROOT is the only path that reaches this, and it
        // has no entry to carry anything. Its timestamps stay zero --
        // the honest answer -- but a mode of zero would read as "nobody
        // may enter /", so the directory default is reported instead.
        out.mode = 0755;
        out.nlink = 1;
    }
    if (!vmm_copy_to_user(c->pml4, c->a1, &out, sizeof out)) {
        klog_write(KLOG_ERR "syscall: stat() rejected -- invalid output pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        kpath_put(path);
        return 0;
    }
    c->regs[14] = 0;
    kpath_put(path);
    return 0;
}

int sys_chmod(struct syscall_ctx *c) {
    char *path = kpath_get();
    if (!path) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, path);
    if (err) {
        klog_write(KLOG_ERR "syscall: chmod() rejected -- bad path\n");
        c->regs[14] = (uint64_t)(int64_t)err;
        kpath_put(path);
        return 0;
    }
    // The VFS masks the type bits off and refuses a backend that cannot
    // store permissions -- see fs.h's fs_chmod().
    c->regs[14] = (uint64_t)(int64_t)fs_chmod(path, (uint16_t)c->a1);
    kpath_put(path);
    return 0;
}

int sys_link(struct syscall_ctx *c) {
    char *from = kpath_get();
    if (!from) { c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    char *to = kpath_get();
    if (!to) { kpath_put(from); c->regs[14] = (uint64_t)(int64_t)-ENOMEM; return 0; }
    int err = resolve_user_path(c->pml4, c->a0, from);
    if (!err) err = resolve_user_path(c->pml4, c->a1, to);
    if (err) {
        klog_write(KLOG_ERR "syscall: link() rejected -- bad path\n");
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
        klog_write(KLOG_ERR "syscall: link() failed\n");
        c->regs[14] = (uint64_t)(int64_t)-EIO;
    } else {
        c->regs[14] = 0;
    }
    kpath_put(from); kpath_put(to);
    return 0;
}

// The work is fs_sync() (vfs.c), which walks every mount. It used to be
// here and ATA-only:
//
//     if (!ata_cache_active()) { c->regs[14] = 0; return 0; }
//
// so on AHCI or virtio-blk -- every modern machine, and the bare-metal
// laptop -- `sync` returned "nothing was pending" without asking the
// drive for anything. That reads as success and is not: the bytes sat
// in the drive's volatile cache, one power cut from gone. It also left
// `storage.sync = lazy` with nothing able to force durability, which is
// that mode's whole safety story.
// One file's durability, on the volume that holds it -- see
// SYS_FSYNC's ABI comment for why it is scoped to the volume rather
// than to the file, and why fdatasync is the same call.
int sys_fsync(struct syscall_ctx *c) {
    struct open_file *f = fd_get(c->pml4, (int)c->a0);
    if (!f || f->kind != FD_KIND_FILE) {
        c->regs[14] = (uint64_t)(int64_t)-EBADF;
        return 0;
    }
    c->regs[14] = fs_sync_path(f->file.name) ? 0 : (uint64_t)(int64_t)-EIO;
    return 0;
}

int sys_sync(struct syscall_ctx *c) {
    uint32_t wrote = 0;
    if (!fs_sync(&wrote)) {
        // The one disk answer a caller must not read as success.
        c->regs[14] = (uint64_t)(int64_t)-EIO;
        return 0;
    }
    // Sectors written back, as before -- a caller that printed this
    // number keeps meaning the same thing by it.
    c->regs[14] = wrote;
    return 0;
}
