// rm -- delete a file, or a tree with -r.
//
// A FRONT END over lib/ufileop.h, which owns the walk. This file is
// rm's policy: delete without asking, print what failed, and keep going
// past a failure so one unreadable file does not abandon the rest.
//
// The deepest-first ordering, the explicit queue and the reason neither
// is recursion all live in the engine now -- see lib/ufileop.h.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ufileop.h"
#include <string.h>

static struct ufileop g_op;

static void on_error(void *ctx, const char *path, int err) {
    (void)ctx;
    sys_print("rm: ");
    sys_print(path);
    sys_print(": ");
    sys_print(sys_strerror(err));
    sys_print("\n");
}

int main(int argc, char **argv) {
    int recursive = 0, arg = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { recursive = 1; arg = 2; }

    if (argc <= arg) {
        cmd_usage("rm [-r] <path> [path...]");
        return 1;
    }

    struct ufileop_policy policy = { .on_error = on_error };
    int failed = 0;
    for (int i = arg; i < argc; i++)
        if (ufileop_remove(argv[i], recursive, &g_op, &policy) != UFILEOP_OK)
            failed = 1;
    return failed;
}
