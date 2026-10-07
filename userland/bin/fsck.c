// fsck -- check a mounted filesystem, and repair what can be repaired
// without guessing.
//
// THE CHECK IS THE KERNEL'S (SYS_FS_CHECK); this prints it. It walks the
// backend's live state and repairs through its journal, which is why it
// is not a ring-3 checker over the raw device -- abi/mount_abi.h has the
// reasoning, and fs.h's fs_check() what a repair will and will not fix.
//
// The exit status is e2fsck's, so a script can tell the cases apart:
// 0 clean, 1 problems found and all of them fixed, 4 problems left,
// 8 the check itself failed. 2 is a usage error, as for every lib/uargs
// program.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/uargs.h"

#define FSCK_CLEAN     0
#define FSCK_CORRECTED 1
#define FSCK_LEFT      4
#define FSCK_FAILED    8

static int g_repair;

static const struct uargs_opt OPTS[] = {
    { "repair", 'r', 0, "also fix what can be fixed without guessing", &g_repair, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "fsck",
    .usage = "[-r] [PATH]",
    .summary = "Check the mounted filesystem holding PATH (default /), in the kernel,\n"
               "and with -r also reclaim leaked blocks and fix what is safely fixable.",
    .opts = OPTS,
    .notes = "`fsck repair` is the same as `fsck -r`.\n"
             "Exit status: 0 clean, 1 all problems fixed, 4 problems left, 8 the check failed.",
};

static int problems(const struct fs_check_result *r) {
    return r->leaked || r->referenced_but_free || r->double_allocated || r->out_of_range;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    int at = 0;
    // The kernel shell's builtin took the word; its messages still say it.
    if (a.argc > at && !strcmp(a.argv[at], "repair")) { g_repair = 1; at++; }
    const char *path = a.argc > at ? a.argv[at++] : "/";
    if (a.argc > at) return uargs_error(&PROG, "unexpected argument '%s'", a.argv[at]);

    struct fs_check_result r;
    if (sys_fs_check(path, g_repair ? FSCK_REPAIR : 0, &r) != 0) {
        int e = errno;
        if (e == ENOTSUP)
            fprintf(stderr, "fsck: %s: this filesystem can be checked but not repaired\n", path);
        else if (e == EROFS)
            fprintf(stderr, "fsck: %s: mounted read-only, so it cannot be repaired\n", path);
        else
            fprintf(stderr, "fsck: %s: %s\n", path, strerror(e));
        return FSCK_FAILED;
    }

    printf("fsck: %s %s on %s\n", g_repair ? "checked and repaired" : "checked (read-only)",
           r.fstype, r.point);
    printf("  records in use:        %u\n", r.records_used);
    printf("  blocks referenced:     %u\n", r.blocks_referenced);
    printf("  leaked (unreferenced): %u\n", r.leaked);
    printf("  referenced but free:   %u\n", r.referenced_but_free);
    printf("  double-allocated:      %u\n", r.double_allocated);
    printf("  out-of-range pointers: %u\n", r.out_of_range);

    if (g_repair) {
        printf("  -- repaired --\n");
        printf("  blocks reclaimed:      %u (%u KB)\n", r.reclaimed, r.reclaimed * 4);
        printf("  marked allocated:      %u\n", r.marked_allocated);
        printf("  pointers cleared:      %u\n", r.pointers_cleared);
    } else if (r.leaked || r.referenced_but_free || r.out_of_range) {
        printf("Run `fsck -r %s` to reclaim %u leaked block(s) (%u KB) and fix the rest.\n",
               path, r.leaked, r.leaked * 4);
    }

    if (r.double_allocated) {
        // Never repaired -- choosing which file keeps a shared block
        // destroys the other's data (fs.h).
        printf("WARNING: blocks claimed by more than one file. `fsck -r` will NOT fix\n"
               "this; delete one of the affected files to resolve it.\n");
    }

    if (!problems(&r)) {
        printf("fsck: clean.\n");
        return FSCK_CLEAN;
    }
    // A repair pass reports what it FOUND; it fixed all of it unless a
    // double allocation, which it never touches, was among it.
    return g_repair && !r.double_allocated ? FSCK_CORRECTED : FSCK_LEFT;
}
