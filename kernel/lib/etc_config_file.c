// etc_config's FILE half -- the four entry points that touch fs.h.
//
// Split out of etc_config.c so the `name=value` parser beside it can be
// freestanding and compiled a second time into libuapp.a, which is what
// lets the ring-3 WM read a .desktop file without a second parser to
// drift from this one. Same arrangement as kfmt.c/kfmt_print.c, and for
// the same reason: one file is shareable, the other reaches for kernel
// state, and mixing them makes neither.
//
// If you add an entry point here, ask which half it belongs in: does it
// look at a buffer, or does it look at a file?
#include "etc_config.h"
#include "fs.h"
#include "string.h"
#include "heap.h"  // these buffers are KiB-sized -- see below

// THE BUFFERS HERE COME FROM THE HEAP, NOT THE STACK, and that is not
// style: `struct etc_config_buf` is a KiB, this file is on the deepest
// kernel path there is (a setting write -> here -> VFS -> TFS3 journal
// -> ATA), and two of them on one frame is 1.5 KiB of a 16 KiB kernel
// stack gone before the filesystem is even reached. That path has
// already overflowed once, in a kernel stack that was half this size --
// see docs/decisions.md. CFLAGS' -Wframe-larger-than is what surfaced
// it; there is nothing subtle about the fix.
//
// Reading a config file does disk I/O either way, so an allocation is
// far below the noise floor. A failed allocation reads as "no such
// file", which is what a caller already has to handle.
int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size) {
    struct etc_config_buf *buf = kmalloc(sizeof *buf);
    if (!buf) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    int ok = 0;
    if (etc_config_load(path, buf)) {
        ok = etc_config_buf_get(buf, key, out, out_size);
    } else if (out && out_size) {
        out[0] = '\0';
    }
    kfree(buf);
    return ok;
}

int etc_config_load(const char *path, struct etc_config_buf *buf) {
    if (!buf) return 0;
    buf->valid = 0;
    buf->size = 0;
    buf->data[0] = '\0';

    // fs_read_into(), not fs_read() + a copy: the copy would still have
    // to happen after fs_read() returned, and the kernel context can be
    // preempted in that window by a ring-3 process whose own file read
    // swaps the shared staging buffer underneath it. Refusing rather
    // than truncating is fs_read_into()'s contract, which is what this
    // wants anyway -- see the header.
    uint32_t size = fs_read_into(path, buf->data, sizeof buf->data);
    if (size == 0) return 0;

    buf->size = size;
    buf->valid = 1;
    return 1;
}

// Both of these are now read / rewrite / write, with the rewrite shared
// (etc_config_buf_set). They used to carry a copy of that loop each.
static int rewrite(const char *path, const char *section,
                   const char *key, const char *value) {
    // Heap, for the reason etc_config_get() above gives at length: two
    // KiB-sized locals here were a 1552-byte frame on the path that has
    // already overflowed a kernel stack once.
    struct etc_config_buf *in = kmalloc(sizeof *in);
    char *out = kmalloc(ETC_CONFIG_MAX);
    if (!in || !out) {
        kfree(in);
        kfree(out);
        return 0;
    }
    // A missing file is not an error for a SET -- the key is appended to
    // an empty document and the file created. It is for an unset, which
    // etc_config_buf_set() reports by returning 0 for "not there".
    if (!etc_config_load(path, in)) {
        in->valid = 0;
        in->size = 0;
        in->data[0] = '\0';
    }
    // An unset is answered from the document BEFORE the rewrite,
    // because etc_config_buf_set() returns the new length and removing
    // a file's only key produces an EMPTY document -- length 0, the same
    // answer as "not there". That refused every unset of a lone key.
    char probe[128];
    int absent = !value && !(in->valid &&
                             etc_config_buf_get_in(in, section, key, probe, sizeof probe));
    uint32_t n = absent ? 0 : etc_config_buf_set_in(in->data, in->size, section,
                                                    key, value, out, ETC_CONFIG_MAX);
    int rc = 0;
    // A set that produced nothing would not fit; an unset of a present
    // key always writes, even the empty document.
    if (!absent && (n != 0 || !value)) rc = fs_write(path, out, 0);
    kfree(in);
    kfree(out);
    return rc;
}

int etc_config_unset(const char *path, const char *key) {
    return rewrite(path, 0, key, 0);
}

int etc_config_set(const char *path, const char *key, const char *value) {
    return rewrite(path, 0, key, value);
}

int etc_config_unset_in(const char *path, const char *section, const char *key) {
    return rewrite(path, section, key, 0);
}

int etc_config_set_in(const char *path, const char *section,
                      const char *key, const char *value) {
    return rewrite(path, section, key, value);
}

int etc_config_get_in(const char *path, const char *section, const char *key,
                      char *out, uint32_t out_size) {
    struct etc_config_buf *buf = kmalloc(sizeof *buf);
    if (!buf) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    int ok = 0;
    if (etc_config_load(path, buf)) {
        ok = etc_config_buf_get_in(buf, section, key, out, out_size);
    } else if (out && out_size) {
        out[0] = '\0';
    }
    kfree(buf);
    return ok;
}
