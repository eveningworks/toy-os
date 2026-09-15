// See api/kpath_buf.h for why a path may not be a kernel local.
//
// kmalloc rather than a fixed pool of slots: a pool has to guess how
// many paths are live at once, and this kernel preempts inside a
// syscall, so the honest number is "one per process in a path call"
// rather than anything a constant could name. Linux uses a dedicated
// slab for the speed; here the allocation sits next to disk I/O and is
// noise.
#include "kpath_buf.h"
#include "fs.h"
#include "heap.h"

char *kpath_get(void) {
    return (char *)kmalloc(FS_PATH_MAX);
}

void kpath_put(char *buf) {
    kfree(buf);
}

int kpath_scratch_get(struct kpath_scratch *out) {
    if (!out) return 0;
    out->cap = KPATH_SCRATCH_FOR(FS_PATH_MAX);
    out->buf = (char *)kmalloc(out->cap);
    if (!out->buf) { out->cap = 0; return 0; }
    return 1;
}

void kpath_scratch_put(struct kpath_scratch *s) {
    if (!s || !s->buf) return;
    kfree(s->buf);
    s->buf = 0;
    s->cap = 0;
}
