// mv -- rename, or move between directories.
//
// A FRONT END over lib/ufileop.h. It was one SYS_RENAME; the engine
// adds the fallback that matters -- a rename across parents needs five
// journal credits and a v1 TFS3 image has four slots (fs.h), so that
// one case failed with EIO and now copies and deletes instead.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/ufileop.h"

static struct ufileop g_op;

static void on_error(void *ctx, const char *path, int err) {
    (void)ctx;
    cmd_fail_err("mv", path, err);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        cmd_usage("mv <source> <dest>");
        return 1;
    }
    struct ufileop_policy policy = { .on_error = on_error };
    return ufileop_move(argv[1], argv[2], &g_op, &policy) == UFILEOP_OK ? 0 : 1;
}
