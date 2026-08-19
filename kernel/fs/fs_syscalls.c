// The path-keyed filesystem syscalls: open, unlink, listdir, and the
// filesystem's change counter.
//
// The fd TABLE is not here -- it belongs to the process, not to the
// filesystem, and sockets and pipes share it (kernel/proc/syscall_fd.c).
// What is here is everything that takes a path.
#include "syscalls.h"
#include "syscall_abi.h"
#include "klog.h"
#include "vmm.h"
#include "fs.h"
#include "string.h"
#include "tz.h"
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
    if (!vmm_copy_string_from_user(pml4, name, c->a0, FS_PATH_MAX)) {
        klog_write("syscall: open() rejected -- invalid path pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {

        uint32_t flags = (uint32_t)c->a1;
        int want_write = (flags & SYS_O_WRITE) != 0;
        int want_creat = (flags & SYS_O_CREAT) != 0;
        int want_trunc = (flags & SYS_O_TRUNC) != 0;

        uint32_t existing_size = 0;
        int exists = fs_read(name, &existing_size) != 0;

        if (!exists && !(want_write && want_creat)) {
            klog_write("syscall: open() rejected -- file not found\n");
            c->regs[14] = (uint64_t)-1;
        } else {
            // A DESCRIPTION plus a descriptor naming it. Two steps
            // rather than one because dup2 can later point a second
            // descriptor at this same open file.
            int di = fd_desc_alloc(FD_KIND_FILE, -1);
            int fd = di >= 0 ? fd_install(pml4, di) : -1;
            if (fd < 0) {
                if (di >= 0) fd_desc_unref(di);
                klog_write("syscall: open() rejected -- fd table full\n");
                c->regs[14] = (uint64_t)-1;
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
    if (!vmm_copy_string_from_user(pml4, name, c->a0, FS_PATH_MAX)) {
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
        !vmm_copy_string_from_user(pml4, path, c->a0, FS_PATH_MAX)) {
        klog_write("syscall: listdir() rejected -- invalid pointer\n");
        c->regs[14] = (uint64_t)-1;
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
    }
    return 0;
}

int sys_fs_generation(struct syscall_ctx *c) {
    // One integer, no user pointer to validate -- see
    // syscall_abi.h for why this is not a SYS_SYSINFO field.
    c->regs[14] = fs_generation();
    return 0;
}
