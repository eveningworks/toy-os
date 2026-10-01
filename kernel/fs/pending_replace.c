// Staged file replacements, applied at boot before init -- see
// abi/update_abi.h for the contract and docs/update-design.md for why
// the kernel, not init, does it: after the reboot the kernel is the one
// component certain to be the new one, and init is itself a file the
// list may be replacing.
#include "fs.h"
#include "klog.h"
#include "kfmt.h"
#include "heap.h"
#include "string.h"
#include "kpath_buf.h" // a path is FS_PATH_MAX and may not be a kernel local
#include "update_abi.h"
#include "pending_replace.h"

// A generous bound on the list: one line per changed file, and a whole
// userland is under a thousand of them.
#define PENDING_MAX (256u * 1024u)

// Idempotent per line: a target whose staged file is gone was applied
// by an earlier boot that lost power before deleting the list, and
// finishing that boot's work is exactly what this is for.
static int apply_line(const char *target, char *staged, int *applied, int *failed) {
    size_t n = (size_t)k_strlen(target);
    size_t sfx = sizeof UPDATE_STAGED_SUFFIX - 1;
    if (target[0] != '/' || n + sfx + 1 > FS_PATH_MAX) return 0;
    if (n >= sfx && k_strcmp(target + n - sfx, UPDATE_STAGED_SUFFIX) == 0) return 0;
    k_memcpy(staged, target, n);
    k_memcpy(staged + n, UPDATE_STAGED_SUFFIX, sfx + 1);
    if (!fs_exists(staged)) return 1;
    if (fs_rename_replace(staged, target)) {
        (*applied)++;
    } else {
        (*failed)++;
        klog_printf(KLOG_ERR "update: could not replace %s -- %s left in place\n",
                    target, staged);
    }
    return 1;
}

// A `-<target>` line: the file an update stopped shipping. Gone already
// is done, as for a rename; a directory is refused, not emptied.
static int remove_line(const char *target, int *removed, int *failed) {
    if (target[0] != '/') return 0;
    if (!fs_exists(target)) return 1;
    if (fs_is_dir(target)) return 0;
    if (fs_delete(target)) {
        (*removed)++;
    } else {
        (*failed)++;
        klog_printf(KLOG_ERR "update: could not remove %s\n", target);
    }
    return 1;
}

void fs_apply_pending_replacements(void) {
    uint64_t size = fs_size(UPDATE_PENDING_PATH);
    if (!size) return;
    if (size >= PENDING_MAX) {
        klog_printf(KLOG_ERR "update: %s is %u bytes -- refusing it\n",
                    UPDATE_PENDING_PATH, (unsigned)size);
        return;
    }
    char *buf = kmalloc((size_t)size + 1);
    char *staged = kpath_get();
    if (!buf || !staged) { kfree(buf); if (staged) kpath_put(staged); return; }
    if (!fs_read_into(UPDATE_PENDING_PATH, buf, (uint32_t)size + 1)) {
        kfree(buf); kpath_put(staged); return;
    }

    int applied = 0, removed = 0, failed = 0, bad = 0;
    char *line = buf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char next = *end;
        *end = '\0';
        if (end > line && end[-1] == '\r') end[-1] = '\0';
        int ok = line[0] == UPDATE_REMOVE_PREFIX ? remove_line(line + 1, &removed, &failed)
               : !line[0] || apply_line(line, staged, &applied, &failed);
        if (!ok) bad++;
        if (!next) break;
        line = end + 1;
    }
    kfree(buf);
    kpath_put(staged);

    // Kept on any failure, so the next boot tries again and a person can
    // read what is still pending; a clean run removes it.
    if (!failed) fs_delete(UPDATE_PENDING_PATH);
    fs_sync(0);
    if (failed)
        klog_printf(KLOG_ERR "update: applied %d staged files at boot, %d FAILED (%s kept)\n",
                    applied, failed, UPDATE_PENDING_PATH);
    else
        klog_printf("update: applied %d staged file%s at boot, removed %d\n", applied,
                    applied == 1 ? "" : "s", removed);
    if (bad) klog_printf(KLOG_WARN "update: ignored %d malformed line%s in %s\n", bad,
                         bad == 1 ? "" : "s", UPDATE_PENDING_PATH);
}
