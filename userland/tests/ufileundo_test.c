// lib/ufileundo.h: each kind of operation undone and redone on real
// files under /home (a volume with a Recycle Bin), with the result read
// back through sys_stat -- not through the journal, which is under test.
//
// Then the journal itself: a new record drops what could have been
// redone, and a step that cannot be reversed fails alone.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "lib/ufileundo.h"
#include "lib/utrash.h"

#define DIR "/home/ufu-test"

static struct ufileundo g_u;      // static: hundreds of KB
static struct ufileop g_s;

static int exists(const char *p) {
    struct sys_stat st;
    return sys_stat(p, &st) == 0;
}

static void make(const char *p) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "x", 1); close(fd); }
}

// Record one operation of one step, as an app would after doing it.
static void record(int kind, const char *a, const char *b, const char *bin) {
    struct ufu_op *op = ufileundo_begin(&g_u, kind, "t");
    ufileundo_add(op, a, b, bin);
    ufileundo_commit(&g_u, op);
}

static int run(int redo) {
    struct ufu_op *op = redo ? ufileundo_next_redo(&g_u) : ufileundo_next_undo(&g_u);
    if (!op) return -1;
    int e = 0;
    for (int i = 0; i < op->count; i++) {
        int r = ufileundo_apply(op, i, redo, &g_s, 0);
        if (r) e = r;
    }
    ufileundo_done(&g_u, redo);
    return e;
}

int main(void) {
    utest_begin("ufileundo_test", "the file-operation undo journal", UTEST_VERDICT_FILE);
    struct ufileop_policy pol = { 0 };
    (void)pol;
    char rm[] = DIR;
    ufileop_remove(rm, 1, &g_s, 0);
    sys_mkdir(DIR);
    sys_mkdir(DIR "/sub");

    // MOVE: the file goes back, then forward again.
    make(DIR "/m.txt");
    sys_rename(DIR "/m.txt", DIR "/sub/m.txt");
    record(UFU_MOVE, DIR "/m.txt", DIR "/sub/m.txt", 0);
    int e = run(0);
    utest_checkf(!e && exists(DIR "/m.txt") && !exists(DIR "/sub/m.txt"),
                 "undoing a move puts the file back (e=%d)", e);
    e = run(1);
    utest_checkf(!e && !exists(DIR "/m.txt") && exists(DIR "/sub/m.txt"),
                 "redoing it moves it again (e=%d)", e);

    // RENAME
    make(DIR "/old.txt");
    sys_rename(DIR "/old.txt", DIR "/new.txt");
    record(UFU_RENAME, DIR "/old.txt", DIR "/new.txt", 0);
    e = run(0);
    utest_checkf(!e && exists(DIR "/old.txt") && !exists(DIR "/new.txt"),
                 "undoing a rename restores the old name (e=%d)", e);

    // COPY: undone into the bin, never deleted for good; redone back out.
    make(DIR "/c.txt");
    make(DIR "/sub/c.txt");
    record(UFU_COPY, DIR "/c.txt", DIR "/sub/c.txt", 0);
    e = run(0);
    struct ufu_op *op = ufileundo_next_redo(&g_u);
    utest_checkf(!e && !exists(DIR "/sub/c.txt") && exists(DIR "/c.txt") &&
                 op && op->step[0].bin[0] && exists(op->step[0].bin),
                 "undoing a copy moves the COPY to the bin, the original stays (e=%d)", e);
    e = run(1);
    utest_checkf(!e && exists(DIR "/sub/c.txt"), "redoing it brings the copy back out (e=%d)", e);

    // CREATE
    sys_mkdir(DIR "/New folder");
    record(UFU_CREATE, 0, DIR "/New folder", 0);
    e = run(0);
    utest_checkf(!e && !exists(DIR "/New folder"), "undoing a new folder recycles it (e=%d)", e);

    // TRASH: undone out of the bin, redone back in.
    make(DIR "/t.txt");
    struct utrash_item it;
    char binp[UFU_PATH] = "";
    if (utrash_put(DIR "/t.txt", &it) == 0) utrash_item_path(&it, binp, sizeof binp);
    record(UFU_TRASH, DIR "/t.txt", 0, binp);
    e = run(0);
    utest_checkf(!e && exists(DIR "/t.txt"), "undoing a delete restores the file (e=%d)", e);
    e = run(1);
    utest_checkf(!e && !exists(DIR "/t.txt"), "redoing it sends it to the bin again (e=%d)", e);

    // A new record drops what could have been redone.
    run(0);   // undo the trash again: a redo is now pending
    utest_check(ufileundo_next_redo(&g_u) != 0, "an undo leaves a redo");
    record(UFU_CREATE, 0, DIR "/x", 0);
    utest_check(ufileundo_next_redo(&g_u) == 0, "a new operation drops the redo");

    // A step that cannot be reversed fails alone: the other one runs.
    make(DIR "/p.txt");
    make(DIR "/q.txt");
    sys_rename(DIR "/p.txt", DIR "/sub/p.txt");
    sys_rename(DIR "/q.txt", DIR "/sub/q.txt");
    op = ufileundo_begin(&g_u, UFU_MOVE, "two");
    ufileundo_add(op, DIR "/p.txt", DIR "/sub/p.txt", 0);
    ufileundo_add(op, DIR "/q.txt", DIR "/sub/q.txt", 0);
    ufileundo_commit(&g_u, op);
    make(DIR "/p.txt");   // something now has p's old name
    e = run(0);
    utest_checkf(e == -EEXIST && exists(DIR "/sub/p.txt") && exists(DIR "/q.txt"),
                 "a step onto a taken name is refused; the next step still runs (e=%d)", e);

    ufileop_remove(rm, 1, &g_s, 0);
    return utest_end();
}
