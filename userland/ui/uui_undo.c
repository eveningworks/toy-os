// The edit history -- see ui/uui_undo.h.
//
// A record is laid out as
//
//   [kind:1][flags:1][pad:2][pos:4][len:4][len bytes of text][size:4]
//
// and the trailing size is what lets undo walk BACKWARDS from `at`.
// A record flagged JOIN belongs to the same step as the one before it.
#include "ui/uui_undo.h"

#define K_INS 1
#define K_DEL 2
#define F_JOIN 0x01
#define HDR 12
#define TRL 4
#define RUN_MAX 4096   // a typing run longer than this starts a new step

struct rec { int kind, flags, pos, len; const unsigned char *text; int size; };

// No <string.h>: this file also builds on the host (utext_hostcheck.py),
// where kernel/include/api's string.h is the one found.
static void mv(unsigned char *d, const unsigned char *s, int n) {
    if (d < s) for (int i = 0; i < n; i++) d[i] = s[i];
    else for (int i = n - 1; i >= 0; i--) d[i] = s[i];
}
static int rd32(const unsigned char *p) {
    return (int)((unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24);
}
static void wr32(unsigned char *p, int v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)((unsigned)v >> (8 * i));
}

static void rec_at(const struct uui_undo *u, int off, struct rec *r) {
    const unsigned char *p = u->buf + off;
    r->kind = p[0];
    r->flags = p[1];
    r->pos = rd32(p + 4);
    r->len = rd32(p + 8);
    r->text = p + HDR;
    r->size = HDR + r->len + TRL;
}

// The record that ends at `end`.
static int rec_before(const struct uui_undo *u, int end, struct rec *r) {
    if (end <= 0) return 0;
    int size = rd32(u->buf + end - TRL);
    rec_at(u, end - size, r);
    return end - size;
}

void uui_undo_init(struct uui_undo *u, void *storage, int cap) {
    u->buf = storage;
    u->cap = storage ? cap : 0;
    uui_undo_reset(u);
}

void uui_undo_reset(struct uui_undo *u) {
    u->used = u->at = 0;
    u->clean = 0;
    u->depth = 0;
    u->fresh = 1;
    u->run = 0;
}

void uui_undo_begin(struct uui_undo *u) {
    if (u->depth++ == 0) { u->fresh = 1; u->run = 0; }
}

void uui_undo_end(struct uui_undo *u) {
    if (u->depth > 0 && --u->depth == 0) { u->fresh = 1; u->run = 0; }
}

void uui_undo_break(struct uui_undo *u) { u->run = 0; }

int uui_undo_can_undo(const struct uui_undo *u) { return u->buf && u->at > 0; }
int uui_undo_can_redo(const struct uui_undo *u) { return u->buf && u->at < u->used; }

void uui_undo_mark_clean(struct uui_undo *u) { u->clean = u->at; }
int  uui_undo_is_clean(const struct uui_undo *u) { return u->clean == u->at; }

// A new edit: whatever was undone can no longer be redone.
static void drop_redo(struct uui_undo *u) {
    if (u->used == u->at) return;
    u->used = u->at;
    if (u->clean > u->at) u->clean = -1;
}

// Make `need` more bytes fit by forgetting the OLDEST steps, whole.
static int room(struct uui_undo *u, int need) {
    if (need > u->cap) {
        // One edit bigger than the history: nothing before it can be
        // undone across it, so keep nothing at all.
        u->used = u->at = 0;
        u->clean = -1;
        u->run = 0;
        return 0;
    }
    while (u->used + need > u->cap && u->used > 0) {
        struct rec r;
        int cut = 0;
        do {
            rec_at(u, cut, &r);
            cut += r.size;
            if (cut >= u->used) break;
            rec_at(u, cut, &r);
        } while (r.flags & F_JOIN);
        if (cut > u->used) cut = u->used;
        mv(u->buf, u->buf + cut, u->used - cut);
        u->used -= cut;
        u->at -= cut;
        if (u->at < 0) u->at = 0;
        u->clean = u->clean >= cut ? u->clean - cut : -1;
    }
    return u->used + need <= u->cap;
}

// Reserves a record of `n` text bytes and returns where its text goes.
static unsigned char *push_open(struct uui_undo *u, int kind, int pos, int n) {
    int size = HDR + n + TRL;
    if (!room(u, size)) return 0;
    unsigned char *p = u->buf + u->used;
    p[0] = (unsigned char)kind;
    p[1] = (u->depth > 0 && !u->fresh) ? F_JOIN : 0;
    p[2] = p[3] = 0;
    wr32(p + 4, pos);
    wr32(p + 8, n);
    wr32(p + HDR + n, size);
    u->used += size;
    u->at = u->used;
    u->fresh = 0;
    return p + HDR;
}

static void push(struct uui_undo *u, int kind, int pos, const char *s, int n) {
    unsigned char *t = push_open(u, kind, pos, n);
    if (t) mv(t, (const unsigned char *)s, n);
}

static int is_blank(int c) { return c == ' ' || c == '\t' || c == '\n'; }

// Grows the last record by one byte at its front or its back. room()
// may forget old steps, so the record is found again after it.
static int extend(struct uui_undo *u, int front, char c, int new_pos) {
    struct rec r;
    if (!room(u, 1) || u->at <= 0) return 0;
    int off = rec_before(u, u->at, &r);
    unsigned char *p = u->buf + off;
    int len = r.len;
    if (front) mv(p + HDR + 1, p + HDR, len);
    p[HDR + (front ? 0 : len)] = (unsigned char)c;
    wr32(p + 4, new_pos);
    wr32(p + 8, len + 1);
    wr32(p + HDR + len + 1, r.size + 1);
    u->used += 1;
    u->at = u->used;
    return 1;
}

void uui_undo_note_insert(struct uui_undo *u, int pos, const char *s, int n) {
    if (!u->buf || n <= 0) return;
    drop_redo(u);
    if (u->clean == u->at && u->run) u->run = 0;   // never fold into the clean point
    if (n == 1 && u->run && u->depth == 0 && u->at > 0) {
        struct rec r;
        rec_before(u, u->at, &r);
        if (r.kind == K_INS && r.pos + r.len == pos && r.len < RUN_MAX &&
            !(is_blank(r.text[r.len - 1]) && !is_blank(s[0]))) {
            if (extend(u, 0, s[0], r.pos)) return;
        }
    }
    push(u, K_INS, pos, s, n);
    u->run = (n == 1 && u->depth == 0);
}

void uui_undo_note_erase(struct uui_undo *u, int pos, const char *s, int n) {
    if (!u->buf || n <= 0) return;
    drop_redo(u);
    if (u->clean == u->at && u->run) u->run = 0;
    if (n == 1 && u->run && u->depth == 0 && u->at > 0) {
        struct rec r;
        rec_before(u, u->at, &r);
        if (r.kind == K_DEL && r.len < RUN_MAX) {
            if (pos + 1 == r.pos && extend(u, 1, s[0], pos)) return;      // Backspace
            if (pos == r.pos && extend(u, 0, s[0], pos)) return;          // Delete
        }
    }
    push(u, K_DEL, pos, s, n);
    u->run = (n == 1 && u->depth == 0);
}

void uui_undo_note_erase_from(struct uui_undo *u, int pos, int n,
                              const struct uui_edit_ops *ops, void *text) {
    if (!u->buf || n <= 0) return;
    drop_redo(u);
    u->run = 0;
    unsigned char *t = push_open(u, K_DEL, pos, n);
    if (t) for (int i = 0; i < n; i++) t[i] = (unsigned char)ops->at(text, pos + i);
}

static void put(const struct uui_edit_ops *ops, void *text, int pos,
                const unsigned char *s, int n) {
    if (ops->insert_text) { ops->insert_text(text, pos, (const char *)s, n); return; }
    for (int i = 0; i < n; i++)
        if (!ops->insert(text, pos + i, (char)s[i])) break;
}

int uui_undo_apply(struct uui_undo *u, int redo, const struct uui_edit_ops *ops,
                   void *text, int *cursor) {
    if (!u->buf || u->depth) return 0;
    u->run = 0;
    u->fresh = 1;
    struct rec r;
    if (!redo) {
        if (u->at <= 0) return 0;
        for (;;) {
            int off = rec_before(u, u->at, &r);
            if (r.kind == K_INS) { ops->erase(text, r.pos, r.pos + r.len); *cursor = r.pos; }
            else { put(ops, text, r.pos, r.text, r.len); *cursor = r.pos + r.len; }
            u->at = off;
            if (!(r.flags & F_JOIN) || u->at <= 0) break;
        }
        return 1;
    }
    if (u->at >= u->used) return 0;
    for (;;) {
        rec_at(u, u->at, &r);
        if (r.kind == K_INS) { put(ops, text, r.pos, r.text, r.len); *cursor = r.pos + r.len; }
        else { ops->erase(text, r.pos, r.pos + r.len); *cursor = r.pos; }
        u->at += r.size;
        if (u->at >= u->used) break;
        rec_at(u, u->at, &r);
        if (!(r.flags & F_JOIN)) break;
    }
    return 1;
}
