// See wm_conf.h.
#include "wm/wm_conf.h"
#include "wm/wm_fs.h"
#include "rt/sys.h"
#include <string.h>

int wm_conf_load(const char *path, struct etc_config_buf *buf) {
    if (!buf) return 0;
    buf->valid = 0;
    buf->size = 0;
    buf->data[0] = '\0';

    uint32_t n = wm_fs_read_into(path, buf->data, (uint32_t)sizeof buf->data);
    if (n == 0) return 0;

    buf->size = n;
    buf->valid = 1;
    return 1;
}

int wm_conf_get(const char *path, const char *key, char *out, uint32_t out_size) {
    struct etc_config_buf buf;
    if (!wm_conf_load(path, &buf)) {
        if (out && out_size) out[0] = '\0';
        return 0;
    }
    // The KERNEL's parser, compiled a second time into libuapp.a -- not
    // a ring-3 reimplementation. See wm_conf.h.
    return etc_config_buf_get(&buf, key, out, out_size);
}

int wm_conf_set(const char *path, const char *key, const char *value) {
    struct etc_config_buf in;
    char out[ETC_CONFIG_MAX];

    // A missing file is not an error: the key is appended to an empty
    // document and the file created. Same behaviour as the kernel's
    // etc_config_set(), because it is the same rewriter underneath.
    if (!wm_conf_load(path, &in)) {
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

uint32_t wm_setting_generation(void) {
    struct setting_msg msg;
    for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
    // Any op fills `generation`; COUNT is the cheapest and has no
    // arguments to get wrong.
    msg.op = SETTING_OP_COUNT;
    if (sys_setting(&msg) != 0) return 0;
    return msg.generation;
}
