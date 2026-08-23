// See lib/uconf.h.
#include "lib/uconf.h"
#include "rt/sys.h"
#include <string.h>

// Reads a whole file into the CALLER's buffer, refusing one that does
// not fit rather than truncating it -- the same contract (and the same
// reason) as the kernel's fs_read_into().
static uint32_t read_into(const char *path, void *buf, uint32_t cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;

    uint32_t total = 0;
    char *out = (char *)buf;
    for (;;) {
        if (total >= cap) { sys_close(fd); return 0; } // too big: refuse
        int64_t n = sys_read(fd, out + total, cap - total);
        if (n < 0) { sys_close(fd); return 0; }
        if (n == 0) break;
        total += (uint32_t)n;
    }
    sys_close(fd);
    return total;
}

int uconf_load(const char *path, struct etc_config_buf *buf) {
    if (!buf) return 0;
    buf->valid = 0;
    buf->size = 0;
    buf->data[0] = '\0';

    uint32_t n = read_into(path, buf->data, (uint32_t)sizeof buf->data);
    if (n == 0) return 0;

    buf->size = n;
    buf->valid = 1;
    return 1;
}

int uconf_get(const char *path, const char *key, char *out, uint32_t out_size) {
    struct etc_config_buf buf;
    if (!uconf_load(path, &buf)) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    return etc_config_buf_get(&buf, key, out, out_size);
}

int uconf_set(const char *path, const char *key, const char *value) {
    struct etc_config_buf in;
    char out[ETC_CONFIG_MAX];

    // A missing file is not an error: the key is appended to an empty
    // document and the file created. Same behaviour as the kernel's
    // etc_config_set(), because it is the same rewriter underneath.
    if (!uconf_load(path, &in)) {
        in.valid = 0;
        in.size = 0;
        in.data[0] = '\0';
    }

    uint32_t n = etc_config_buf_set(in.data, in.size, key, value, out, sizeof out);
    if (n == 0) return 0;

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return 0;
    int64_t w = sys_write(fd, out, n);
    sys_close(fd);

    // A short write is a failed write. A config file half-rewritten is
    // worse than one not rewritten at all -- it parses, with the tail
    // of the document missing.
    return w == (int64_t)n;
}
