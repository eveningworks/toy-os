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
int uclip_kind(const struct uclip *c) { return c ? (int)c->msg.kind : UCLIP_KIND_FILES; }
int uclip_count(const struct uclip *c) { return c ? (int)c->msg.count : 0; }
unsigned uclip_serial(const struct uclip *c) { return c ? c->msg.serial : 0; }

const char *uclip_path(const struct uclip *c, int i) {
    if (!c || c->msg.kind != UCLIP_KIND_FILES) return 0;
    if (i < 0 || i >= (int)c->msg.count) return 0;
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
    c->msg.kind = UCLIP_KIND_FILES;
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

// STATIC, not a local: the message embeds the whole payload, which is
// two orders of magnitude past the ring-3 frame budget. The two writers
// below share it because neither ever runs while the other is on the
// stack -- there are no threads in this library's callers, and a worker
// thread may touch nothing in the toolkit anyway.
static struct win_clip_msg g_out;

int uclip_clear(void) {
    memset(&g_out, 0, sizeof g_out);
    g_out.type = WIN_REQ_CLIP_SET;
    g_out.op = WIN_CLIP_OP_NONE;
    g_out.kind = UCLIP_KIND_FILES;
    return sys_win_clip(&g_out);
}

int uclip_set_text(const char *s, int n) {
    if (!s || n < 0 || n > UCLIP_TEXT_MAX) return 0;
    memset(&g_out, 0, sizeof g_out);
    g_out.type = WIN_REQ_CLIP_SET;
    // COPY even for a cut: see uclip.h -- there is nothing to move once
    // the characters are in the server, so an editor's Cut deletes its
    // own selection and copies.
    g_out.op = WIN_CLIP_OP_COPY;
    g_out.kind = UCLIP_KIND_TEXT;
    g_out.count = 1;
    g_out.len = (uint32_t)n + 1;          // the NUL is payload
    memcpy(g_out.data, s, (size_t)n);
    g_out.data[n] = '\0';
    return sys_win_clip(&g_out);
}

const char *uclip_text(const struct uclip *c, int *out_len) {
    if (out_len) *out_len = 0;
    if (!c || c->msg.kind != UCLIP_KIND_TEXT || c->msg.count != 1) return 0;
    if (c->msg.len == 0 || c->msg.len > WIN_CLIP_BYTES) return 0;
    // The server stores what it was given; a payload arriving without
    // its terminator is a malformed one, and reading past it would run
    // off the buffer.
    if (c->msg.data[c->msg.len - 1] != '\0') return 0;
    if (out_len) *out_len = (int)c->msg.len - 1;
    return c->msg.data;
}
