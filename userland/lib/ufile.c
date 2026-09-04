// See lib/ufile.h.
#include "lib/ufile.h"
#include "rt/sys.h"
#include <stdlib.h>

enum ufile_result ufile_slurp(const char *path, size_t cap,
                              uint8_t **out, size_t *out_len) {
    *out = NULL;
    if (out_len) *out_len = 0;

    struct sys_stat st;
    if (sys_stat(path, &st) < 0) return UFILE_NOENT;
    if (st.size == 0) return UFILE_EMPTY;
    if (cap && st.size > (uint64_t)cap) return UFILE_TOO_BIG;

    size_t len = (size_t)st.size;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return UFILE_NOMEM;

    int fd = sys_open(path, 0);
    if (fd < 0) { free(buf); return UFILE_OPEN; }

    // A LOOP, because sys_read() is allowed to return short. This is the
    // line the three hand-written copies existed to get right.
    size_t got = 0;
    while (got < len) {
        int64_t n = sys_read(fd, buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    sys_close(fd);

    if (got != len) { free(buf); return UFILE_SHORT; }
    *out = buf;
    if (out_len) *out_len = len;
    return UFILE_OK;
}

size_t ufile_read_head(const char *path, uint8_t *buf, size_t cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    int64_t got = sys_read(fd, buf, cap);
    sys_close(fd);
    return got > 0 ? (size_t)got : 0;
}
