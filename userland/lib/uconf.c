// See lib/uconf.h.
#include <stdlib.h>
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

// HEAP, NOT THE STACK, and that is what lets ETC_CONFIG_BUF_MAX be
// bigger than a frame. A struct etc_config_buf is the whole document,
// so a local one is a multi-KiB frame against a 2 KiB ring-3 budget --
// and a frame that large does not merely overflow, it steps over the
// single guard page below the stack (CLAUDE.md's Stack Clash note).
// The kernel's half of this rewriter already moved to kmalloc for the
// same reason; this is the ring-3 half catching up.
//
// Not a static, which would be smaller AND wrong: these are library
// functions with no ownership of the caller's context, and a shared
// buffer here is the re-entrancy hazard etc_config.h's "the buffer is
// the caller's" note exists to avoid.
int uconf_get_in(const char *path, const char *section, const char *key,
                 char *out, uint32_t out_size) {
    struct etc_config_buf *buf = malloc(sizeof *buf);
    if (!buf) { if (out && out_size) out[0] = '\0'; return 0; }
    int ok = uconf_load(path, buf);
    int rc = ok ? etc_config_buf_get_in(buf, section, key, out, out_size) : 0;
    if (!ok && out && out_size) out[0] = '\0';
    free(buf);
    return rc;
}

int uconf_get(const char *path, const char *key, char *out, uint32_t out_size) {
    return uconf_get_in(path, 0, key, out, out_size);
}

int uconf_set_in(const char *path, const char *section,
                 const char *key, const char *value) {
    struct etc_config_buf *in = malloc(sizeof *in);
    char *out = malloc(ETC_CONFIG_MAX);
    int rc = 0;
    if (!in || !out) goto done;

    // A missing file is not an error: the key is appended to an empty
    // document and the file created. Same behaviour as the kernel's
    // etc_config_set(), because it is the same rewriter underneath.
    if (!uconf_load(path, in)) {
        in->valid = 0;
        in->size = 0;
        in->data[0] = '\0';
    }

    // AN UNSET IS ANSWERED FROM THE DOCUMENT BEFORE THE REWRITE, because
    // removing a file's only key produces an EMPTY one -- length 0, the
    // same answer the rewriter gives for "it would not fit". Taking that
    // as a failure refuses every unset of a lone key. The kernel's
    // rewrite() carries this fix already (etc_config_file.c); this is
    // the ring-3 half of the same function catching up.
    char probe[128];
    int absent = !value && !(in->valid &&
                             etc_config_buf_get_in(in, section, key,
                                                   probe, sizeof probe));
    if (absent) goto done;

    uint32_t n = etc_config_buf_set_in(in->data, in->size, section, key, value,
                                       out, ETC_CONFIG_MAX);
    if (n == 0 && value) goto done; // a SET that produced nothing did not fit

    // A WRITE THAT CHANGES NOTHING IS NOT PERFORMED, which is what
    // `config` already does for a setting. It stopped being free when
    // /bin/dhcp started running at boot: the nameserver is usually the
    // one already on disk, and every write here is a real filesystem
    // transaction.
    if (in->valid && in->size == n && !memcmp(in->data, out, n)) { rc = 1; goto done; }

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) goto done;
    int64_t w = sys_write(fd, out, n);
    sys_close(fd);

    // A short write is a failed write. A config file half-rewritten is
    // worse than one not rewritten at all -- it parses, with the tail
    // of the document missing.
    rc = (w == (int64_t)n);

done:
    free(in);
    free(out);
    return rc;
}

int uconf_set(const char *path, const char *key, const char *value) {
    return uconf_set_in(path, 0, key, value);
}

int uconf_unset(const char *path, const char *key) {
    return uconf_set_in(path, 0, key, 0);
}
