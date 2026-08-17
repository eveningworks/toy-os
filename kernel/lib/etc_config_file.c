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

int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size) {
    struct etc_config_buf buf;
    if (!etc_config_load(path, &buf)) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    return etc_config_buf_get(&buf, key, out, out_size);
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
static int rewrite(const char *path, const char *key, const char *value) {
    struct etc_config_buf in;
    char out[ETC_CONFIG_MAX];
    // A missing file is not an error for a SET -- the key is appended to
    // an empty document and the file created. It is for an unset, which
    // etc_config_buf_set() reports by returning 0 for "not there".
    if (!etc_config_load(path, &in)) {
        in.valid = 0;
        in.size = 0;
        in.data[0] = '\0';
    }
    uint32_t n = etc_config_buf_set(in.data, in.size, key, value, out, sizeof out);
    if (n == 0 && !value) return 0; // unset: key absent, leave the file alone
    if (n == 0 && value) return 0;  // would not fit
    return fs_write(path, out, 0);
}

int etc_config_unset(const char *path, const char *key) { return rewrite(path, key, 0); }

int etc_config_set(const char *path, const char *key, const char *value) {
    return rewrite(path, key, value);
}
