// cp -- copy a file, or a tree with -r.
//
// A FRONT END over lib/ufileop.h, which owns the copy loop and the tree
// walk. This file is the POLICY `cp` has always had: overwrite without
// asking, say nothing while it runs, and print what failed.
//
// The engine moved out when the File Manager stopped spawning this
// program. Sharing it is what keeps "one implementation of copying,
// testable as text at a prompt" true, which is the half of the old
// arrangement worth keeping -- see lib/ufileop.h on what Windows and
// Linux each share.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ufileop.h"
#include <string.h>

// ~30 KB of working state, at file scope because a ring-3 frame holds
// 2 KiB (USERLAND_CFLAGS).
static struct ufileop g_op;

static void fail_msg(const char *subject, const char *why) {
    sys_print("cp: ");
    if (subject) { sys_print(subject); sys_print(": "); }
    sys_print(why);
    sys_print("\n");
}

static void on_error(void *ctx, const char *path, int err) {
    (void)ctx;
    fail_msg(path, sys_strerror(err));
}

int main(int argc, char **argv) {
    int recursive = 0, arg = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { recursive = 1; arg = 2; }

    if (argc - arg != 2) {
        cmd_usage("cp [-r] <source> <dest>");
        return 1;
    }
    const char *src = argv[arg], *dst = argv[arg + 1];

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { cmd_fail("cp", src); return 1; }
    if (st.is_dir && !recursive) {
        // coreutils' wording, and the same refusal: -r is an explicit
        // statement that a tree is meant, because the cost of the wrong
        // guess is a directory copied when a file was meant.
        fail_msg(src, "is a directory (use -r)");
        return 1;
    }

    // THE TWO REFUSALS CP NAMES ITSELF. The engine refuses both anyway,
    // but it can only say EINVAL -- and "invalid argument" for
    // "you are copying a directory into itself" is a worse message than
    // the one this program had. The check is policy; the backstop is
    // the engine's.
    char dest[UFILEOP_PATH_MAX];
    if (ufileop_resolve_dest(src, dst, dest, sizeof dest)) {
        if (strcmp(src, dest) == 0) {
            fail_msg(src, "source and destination are the same file");
            return 1;
        }
        if (st.is_dir && ufileop_inside(src, dest)) {
            fail_msg(dest, "cannot copy a directory into itself");
            return 1;
        }
    }

    // No on_conflict: NULL means overwrite, which is what cp does.
    struct ufileop_policy policy = { .on_error = on_error };
    return ufileop_copy(src, dst, &g_op, &policy) == UFILEOP_OK ? 0 : 1;
}
