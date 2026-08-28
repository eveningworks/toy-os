// rm -- delete a file, an empty directory, or a whole tree with -r.
//
// Named rm rather than unlink because that is the word people type, and
// SYS_UNLINK is what serves both. `fs_delete()` refuses a non-empty
// directory, so -r is not a kernel flag: it is this program emptying a
// tree from the leaves up and then removing each directory, which is
// what coreutils' rm does too.
//
// THE ORDER IS THE WHOLE TRICK. Directories are collected
// breadth-first, so a parent always appears before its children; files
// are deleted as they are met, and the collected directories are then
// removed in REVERSE, which is deepest-first. That is a post-order walk
// without a recursive function -- and it has to be non-recursive for
// the same reason /bin/cp's is: one listing is 20 KB and a ring-3 frame
// is capped at 2 KiB (USERLAND_CFLAGS).
#include "rt/sys.h"
#include "lib/cmd.h"
#include "kpath.h"
#include <string.h>

#define RM_PATH_MAX 64      // FS_PATH_MAX
#define RM_MAX_DIRS 64      // directories held while walking a tree

static struct sys_dirent g_entries[SYS_LISTDIR_MAX];
static char g_dirs[RM_MAX_DIRS][RM_PATH_MAX];
static int g_dir_count;

static void fail(const char *path) {
    // The reason, since the polarity flip gave unlink one to report.
    sys_print("rm: ");
    sys_print(path);
    sys_print(": ");
    sys_print(sys_strerror(sys_errno()));
    sys_print("\n");
}

static int remove_tree(const char *root) {
    g_dir_count = 0;
    strlcpy(g_dirs[g_dir_count++], root, RM_PATH_MAX);

    int failed = 0;
    // `read` walks forward over the directories found so far; anything
    // pushed while walking is walked in turn, so this ends when nothing
    // new is being found.
    for (int read = 0; read < g_dir_count; read++) {
        char dir[RM_PATH_MAX];
        strlcpy(dir, g_dirs[read], sizeof dir);

        int n = sys_listdir(dir, g_entries, SYS_LISTDIR_MAX);
        if (n < 0) { fail(dir); failed = 1; continue; }
        if (n >= SYS_LISTDIR_MAX) {
            sys_print("rm: ");
            sys_print(dir);
            sys_print(": more entries than one listing holds -- some remain\n");
            failed = 1;
        }

        for (int i = 0; i < n; i++) {
            char p[RM_PATH_MAX];
            if (!k_path_join(dir, g_entries[i].name, p, sizeof p)) {
                fail(g_entries[i].name);
                failed = 1;
                continue;
            }
            if (g_entries[i].is_dir) {
                if (g_dir_count >= RM_MAX_DIRS) {
                    sys_print("rm: ");
                    sys_print(p);
                    sys_print(": too many directories\n");
                    failed = 1;
                    continue;
                }
                strlcpy(g_dirs[g_dir_count++], p, RM_PATH_MAX);
            } else if (sys_unlink(p) != 0) {
                fail(p);
                failed = 1;
            }
        }
    }

    // Deepest first -- see the file header.
    for (int i = g_dir_count - 1; i >= 0; i--) {
        if (sys_unlink(g_dirs[i]) != 0) { fail(g_dirs[i]); failed = 1; }
    }
    return !failed;
}

int main(int argc, char **argv) {
    int recursive = 0, arg = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { recursive = 1; arg = 2; }

    if (argc <= arg) {
        cmd_usage("rm [-r] <path> [path...]");
        return 1;
    }

    int failed = 0;
    for (int i = arg; i < argc; i++) {
        if (recursive) {
            struct sys_stat st;
            // A plain file passed to -r is still just a file: rm -r on a
            // mixed list must not refuse the files in it.
            if (sys_stat(argv[i], &st) == 0 && st.is_dir) {
                if (!remove_tree(argv[i])) failed = 1;
                continue;
            }
        }
        if (sys_unlink(argv[i]) != 0) { fail(argv[i]); failed = 1; }
    }
    return failed;
}
