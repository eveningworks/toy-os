// The system clipboard, packed and unpacked in one place -- see
// lib/uclip.h for why that is worth a file.
#include "lib/uclip.h"
#include "rt/sys.h"
#include <string.h>

int uclip_load(struct uclip *c) {
    if (!c) return 0;
    memset(&c->msg, 0, sizeof c->msg);
    c->msg.type = WIN_REQ_CLIP_GET;
    if (!sys_win_clip(&c->msg)) {
        // Treated as empty rather than as an error: a caller asking
        // "what is on the clipboard" wants an answer it can act on, and
        // "nothing" is one.
        memset(&c->msg, 0, sizeof c->msg);
        return 1;
    }
    return 1;
}

int uclip_op(const struct uclip *c) { return c ? (int)c->msg.op : UCLIP_NONE; }
int uclip_count(const struct uclip *c) { return c ? (int)c->msg.count : 0; }
unsigned uclip_serial(const struct uclip *c) { return c ? c->msg.serial : 0; }

const char *uclip_path(const struct uclip *c, int i) {
    if (!c || i < 0 || i >= (int)c->msg.count) return 0;
    // WALKED, not indexed: the entries are variable-length and packed,
    // which is what keeps a clipboard of short names cheap.
    const char *p = c->msg.data;
    const char *end = c->msg.data + c->msg.len;
    for (int n = 0; n < i; n++) {
        while (p < end && *p) p++;
        if (p >= end) return 0;
        p++;                       // step over the NUL
    }
    return p < end ? p : 0;
}

void uclip_begin(struct uclip *c, int op) {
    if (!c) return;
    memset(&c->msg, 0, sizeof c->msg);
    c->msg.type = WIN_REQ_CLIP_SET;
    c->msg.op = (uint32_t)op;
}

int uclip_add(struct uclip *c, const char *path) {
    if (!c || !path || !*path) return 0;
    uint32_t n = (uint32_t)strlen(path) + 1;      // the NUL is payload
    if (c->msg.count >= WIN_CLIP_MAX) return 0;
    if (c->msg.len + n > WIN_CLIP_BYTES) return 0;
    memcpy(c->msg.data + c->msg.len, path, n);
    c->msg.len += n;
    c->msg.count++;
    return 1;
}

int uclip_commit(struct uclip *c) {
    if (!c || c->msg.count == 0) return 0;
    c->msg.type = WIN_REQ_CLIP_SET;
    return sys_win_clip(&c->msg);
}

int uclip_clear(void) {
    struct win_clip_msg m;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_CLIP_SET;
    m.op = WIN_CLIP_OP_NONE;
    return sys_win_clip(&m);
}
