// cp -- copy a file, or a tree with -r.
//
// The gap it fills: this OS could rename (`mv`), delete (`rm`) and
// create (`touch`, `mkdir`) since the fd syscalls landed, and could not
// COPY at all. Nothing in the kernel copies -- there is no SYS_COPY and
// there should not be, since a copy is a read loop and a write loop the
// kernel has no reason to know about.
//
// ONE IMPLEMENTATION, and that is why the file manager spawns this
// rather than carrying its own loop (docs/filemanager-design.md): a
// second copy path inside a GUI app is the "make every fix twice" shape
// this repo has already deleted from the WM and from the `ls` wrapper
// builtin.
//
// The recursion is BREADTH-FIRST OVER AN EXPLICIT QUEUE, not a
// recursive function, and that is forced rather than stylistic: a
// listing is SYS_LISTDIR_MAX x sizeof(struct sys_dirent) = 20 KB, and
// ring-3 frames are capped at 2 KiB (USERLAND_CFLAGS). One static
// listing buffer serves every level because a directory is fully walked
// before the next is popped -- what goes on the queue is a PATH, not a
// listing.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "kpath.h"   // k_path_join/_basename -- the kernel's, KTESTed
#include <string.h>

#define CP_PATH_MAX 64        // FS_PATH_MAX
#define CP_BUF 4096           // one TFS3 block-ish; static, not a frame
#define CP_MAX_DIRS 64        // pending directories in the queue

static char g_buf[CP_BUF];
static struct sys_dirent g_entries[SYS_LISTDIR_MAX];

// The queue holds SOURCE directories still to walk, each with the
// destination it is being copied to. Paths, not handles: a directory
// entry cannot be held open across the walk.
static char g_qsrc[CP_MAX_DIRS][CP_PATH_MAX];
static char g_qdst[CP_MAX_DIRS][CP_PATH_MAX];
static int g_qhead, g_qtail;

static int queue_push(const char *src, const char *dst) {
    if (g_qtail >= CP_MAX_DIRS) return 0;
    strlcpy(g_qsrc[g_qtail], src, CP_PATH_MAX);
    strlcpy(g_qdst[g_qtail], dst, CP_PATH_MAX);
    g_qtail++;
    return 1;
}

static void fail_msg(const char *subject, const char *why) {
    sys_print("cp: ");
    if (subject) { sys_print(subject); sys_print(": "); }
    sys_print(why);
    sys_print("\n");
}

// Copies one regular file. Returns 1 on success.
static int copy_file(const char *src, const char *dst) {
    int in = sys_open(src, 0);
    if (in < 0) { cmd_fail("cp", src); return 0; }

    int out = sys_open(dst, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (out < 0) { cmd_fail("cp", dst); sys_close(in); return 0; }

    int ok = 1;
    for (;;) {
        int64_t n = sys_read(in, g_buf, sizeof g_buf);
        if (n < 0) { cmd_fail("cp", src); ok = 0; break; }
        if (n == 0) break; // EOF
        // A SHORT WRITE IS A FAILURE, not something to retry blindly:
        // this filesystem writes a whole request or reports why, and a
        // loop that kept going would turn "the disk is full" into a
        // silently truncated file.
        int64_t w = sys_write(out, g_buf, (size_t)n);
        if (w != n) { cmd_fail("cp", dst); ok = 0; break; }
    }

    sys_close(in);
    sys_close(out);
    return ok;
}

// Where `src` lands when copied "to" `dst`: into the directory when dst
// is one (cp's oldest behaviour -- `cp f /tmp` means `/tmp/f`),
// otherwise dst itself.
static int resolve_dest(const char *src, const char *dst, char *out, int cap) {
    struct sys_stat st;
    if (sys_stat(dst, &st) == 0 && st.is_dir)
        return k_path_join(dst, k_path_basename(src), out, (size_t)cap);
    strlcpy(out, dst, (size_t)cap);
    return 1;
}

// Is `dst` inside `src`? Copying a directory into itself never
// terminates -- coreutils refuses it too, and finding out by filling
// the disk is a bad way to learn.
static int inside(const char *src, const char *dst) {
    size_t n = strlen(src);
    if (strncmp(dst, src, n) != 0) return 0;
    return dst[n] == '/' || (dst[n] == '\0' && n > 0);
}

static int copy_tree(const char *src, const char *dst) {
    g_qhead = g_qtail = 0;
    if (sys_mkdir(dst) < 0 && sys_errno() != EEXIST) {
        cmd_fail("cp", dst);
        return 0;
    }
    if (!queue_push(src, dst)) { fail_msg(src, "too many directories"); return 0; }

    int ok = 1;
    while (g_qhead < g_qtail) {
        char sdir[CP_PATH_MAX], ddir[CP_PATH_MAX];
        strlcpy(sdir, g_qsrc[g_qhead], sizeof sdir);
        strlcpy(ddir, g_qdst[g_qhead], sizeof ddir);
        g_qhead++;

        int n = sys_listdir(sdir, g_entries, SYS_LISTDIR_MAX);
        if (n < 0) { cmd_fail("cp", sdir); ok = 0; continue; }
        if (n >= SYS_LISTDIR_MAX)
            fail_msg(sdir, "more entries than one listing holds -- copied the first 256");

        for (int i = 0; i < n; i++) {
            char sp[CP_PATH_MAX], dp[CP_PATH_MAX];
            if (!k_path_join(sdir, g_entries[i].name, sp, sizeof sp) ||
                !k_path_join(ddir, g_entries[i].name, dp, sizeof dp)) {
                fail_msg(g_entries[i].name, "path too long");
                ok = 0;
                continue;
            }
            if (g_entries[i].is_dir) {
                if (sys_mkdir(dp) < 0 && sys_errno() != EEXIST) {
                    cmd_fail("cp", dp);
                    ok = 0;
                    continue;
                }
                if (!queue_push(sp, dp)) {
                    fail_msg(sp, "too many directories");
                    ok = 0;
                }
            } else if (!copy_file(sp, dp)) {
                ok = 0;
            }
        }
    }
    return ok;
}

int main(int argc, char **argv) {
    int recursive = 0, arg = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) { recursive = 1; arg = 2; }

    if (argc - arg != 2) {
        cmd_usage("cp [-r] <source> <dest>");
        return 1;
    }
    const char *src = argv[arg], *dst_arg = argv[arg + 1];

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { cmd_fail("cp", src); return 1; }

    char dst[CP_PATH_MAX];
    if (!resolve_dest(src, dst_arg, dst, sizeof dst)) {
        fail_msg(dst_arg, "path too long");
        return 1;
    }

    if (strcmp(src, dst) == 0) {
        fail_msg(src, "source and destination are the same file");
        return 1;
    }

    if (st.is_dir) {
        // coreutils' wording, and the same refusal: -r is an explicit
        // statement that a tree is meant, because the cost of the wrong
        // guess is a directory copied when a file was meant.
        if (!recursive) { fail_msg(src, "is a directory (use -r)"); return 1; }
        if (inside(src, dst)) {
            fail_msg(dst, "cannot copy a directory into itself");
            return 1;
        }
        return copy_tree(src, dst) ? 0 : 1;
    }

    return copy_file(src, dst) ? 0 : 1;
}
