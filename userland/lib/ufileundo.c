// The file-operation undo journal -- see ufileundo.h.
#include "lib/ufileundo.h"
#include "lib/utrash.h"
#include <string.h>
#include <errno.h>
#include "rt/sys.h"

struct ufu_op *ufileundo_begin(struct ufileundo *u, int kind, const char *label) {
    u->count = u->at;              // what could have been redone is gone
    if (u->count == UFU_DEPTH) {   // full: the oldest falls off
        memmove(&u->op[0], &u->op[1], sizeof u->op[0] * (UFU_DEPTH - 1));
        u->count--;
        u->at--;
    }
    struct ufu_op *op = &u->op[u->count];
    op->kind = kind;
    op->count = 0;
    strlcpy(op->label, label ? label : "", sizeof op->label);
    return op;
}

int ufileundo_add(struct ufu_op *op, const char *a, const char *b, const char *bin) {
    if (op->count >= UFU_STEPS) return 0;
    struct ufu_step *s = &op->step[op->count++];
    strlcpy(s->a, a ? a : "", sizeof s->a);
    strlcpy(s->b, b ? b : "", sizeof s->b);
    strlcpy(s->bin, bin ? bin : "", sizeof s->bin);
    return 1;
}

void ufileundo_commit(struct ufileundo *u, struct ufu_op *op) {
    if (op != &u->op[u->count] || op->count == 0) return;
    u->count++;
    u->at = u->count;
}

struct ufu_op *ufileundo_next_undo(struct ufileundo *u) {
    return u->at > 0 ? &u->op[u->at - 1] : 0;
}

struct ufu_op *ufileundo_next_redo(struct ufileundo *u) {
    return u->at < u->count ? &u->op[u->at] : 0;
}

void ufileundo_done(struct ufileundo *u, int redo) {
    if (redo && u->at < u->count) u->at++;
    else if (!redo && u->at > 0) u->at--;
}

// A move to a full path: a rename when both ends are on one volume, the
// copy-then-delete engine when they are not. Never over something.
static int move_to(const char *from, const char *to,
                   struct ufileop *s, const struct ufileop_policy *p) {
    struct sys_stat st;
    if (sys_stat(from, &st) != 0) return -ENOENT;
    if (sys_stat(to, &st) == 0) return -EEXIST;
    if (sys_rename(from, to) == 0) return 0;
    return ufileop_move(from, to, s, p) == UFILEOP_OK ? 0 : -EIO;
}

// Into the bin, remembering where it went so the other direction can
// take it back out.
static int to_bin(struct ufu_step *st, const char *path) {
    struct utrash_item it;
    int e = utrash_put(path, &it);
    if (e) return e;
    if (!utrash_item_path(&it, st->bin, sizeof st->bin)) return -ENAMETOOLONG;
    return 0;
}

int ufileundo_apply(struct ufu_op *op, int i, int redo,
                    struct ufileop *s, const struct ufileop_policy *p) {
    if (i < 0 || i >= op->count) return -EINVAL;
    struct ufu_step *st = &op->step[i];
    switch (op->kind) {
    case UFU_MOVE:
        return redo ? move_to(st->a, st->b, s, p) : move_to(st->b, st->a, s, p);
    case UFU_RENAME:
        if (redo) return sys_rename(st->a, st->b) == 0 ? 0 : -sys_errno();
        return sys_rename(st->b, st->a) == 0 ? 0 : -sys_errno();
    case UFU_COPY:
    case UFU_CREATE:
        // What the operation MADE goes to the bin, and comes back out.
        return redo ? utrash_restore_to(st->bin, st->b) : to_bin(st, st->b);
    case UFU_TRASH:
        return redo ? to_bin(st, st->a) : utrash_restore_to(st->bin, st->a);
    default:
        return -EINVAL;
    }
}
