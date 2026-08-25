// Filesystem-related shell commands: cat/touch/mkdir/write/append/
// rm/pwd/cd/edit. Split out of shell.c once it crossed 900 lines mixing
// every command category together -- see shell_internal.h's top
// comment for the split's own reasoning and the git history for the build
// this happened in. Shares `cwd`/resolve_path() with shell.c and
// shell_sys.c via shell_internal.h.
//
// `ls` used to live here as a kernel-space built-in (fs_list() called
// directly). It is a /bin program now (userland/bin/ls.c) with no
// builtin in front of it at all -- resolved through PATH like any other
// executable. The only ring-0 listing left is `rescue ls`
// (apps/shell_rescue.c), for a disk whose /bin is damaged. See
// `docs/decisions.md`.
#include "shell_internal.h"

// Reused across cmd_cat() calls, same reasoning as TFS2's own
// g_read_buf (fs_read()'s staging buffer): a fresh kmalloc() every call
// with no caller-visible free() would leak.
static void *g_cat_buf = 0;

// Reads via the stepped fs_read_range_begin()/fs_read_range_step() API
// instead of a single blocking fs_read() -- Milestone 1 phase 4 (see
// docs/roadmap.md), the shell-prompt half deferred when Phase 4's read
// side landed for the GUI (Notepad's Open...). keyboard_getchar()'s own
// comment (kernel/drivers/keyboard.c) documents the gap this closes: a
// blocking command doesn't get debug_console_poll()/vga_cursor_tick()
// serviced at all until it returns, unlike the shell's idle wait at the
// prompt (which already rides keyboard_getchar()'s hlt loop) or the
// GUI's wm_run() (which rides its own per-frame poll). `cat` on a large
// file was the obvious real caller: unlike Notepad's Open..., which is
// capped at SCROLLBACK_CAP (8KB) and finishes in 1-2 steps regardless,
// fs_read()'s whole-file load has no such cap -- a multi-MB `cat` is a
// genuinely long blocking read today. No hlt/throttling between steps
// (unlike wm_run()'s per-frame poll, which is gated on its own idle
// wait) -- this loop has real work to do and wants to finish as fast as
// the disk allows, it just also services the debug console/cursor
// between blocks instead of not at all.
void cmd_cat(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: cat <file>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path)) {
        vga_write("cat: path too long\n");
        return;
    }
    if (!fs_exists(path) || fs_is_dir(path)) {
        vga_write("cat: no such file: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }

    uint64_t size64 = fs_size(path);
    if (size64 > 0xFFFFFFFFu - 1) { // too big for this whole-buffer call, same ceiling fs_read() itself has
        vga_write("cat: file too big\n");
        return;
    }
    uint32_t size = (uint32_t)size64;

    if (g_cat_buf) { kfree(g_cat_buf); g_cat_buf = 0; }
    g_cat_buf = kmalloc((size_t)size + 1);
    if (!g_cat_buf) {
        vga_write("cat: out of memory\n");
        return;
    }

    if (size > 0) {
        void *h = fs_read_range_begin(path, 0, g_cat_buf, size);
        enum fs_step_result r = FS_STEP_FAILED;
        if (h) {
            do {
                scheduler_idle();
                vga_cursor_tick();
                r = fs_read_range_step(h, 0);
            } while (r == FS_STEP_PENDING);
        }
        if (r != FS_STEP_DONE) {
            vga_write("cat: read failed\n");
            kfree(g_cat_buf);
            g_cat_buf = 0;
            return;
        }
    }
    ((uint8_t *)g_cat_buf)[size] = 0;

    vga_write((const char *)g_cat_buf);
    vga_putc('\n');
}

void cmd_touch(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: touch <file>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path) || !fs_touch(path)) {
        vga_write("touch: failed (bad path, missing parent directory, or disk full)\n");
    }
}

void cmd_mkdir(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: mkdir <dir>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path) || !fs_mkdir(path)) {
        vga_write("mkdir: failed (bad path, missing parent directory, already exists, or disk full)\n");
    }
}

// Longest line `write`/`append` will put in a file. Generous next to
// any real config line and small enough to sit on the shell's stack.
#define SHELL_WRITE_LINE_MAX 256

void cmd_write_or_append(const char *args, int append) {
    if (!args || k_strlen(args) == 0) {
        vga_write(append ? "usage: append <file> <text>\n" : "usage: write <file> <text>\n");
        return;
    }
    char name[FS_PATH_MAX];
    const char *rest = args;
    unsigned int i = 0;
    while (*rest && *rest != ' ' && i < FS_PATH_MAX - 1) name[i++] = *rest++;
    name[i] = '\0';
    while (*rest == ' ') rest++;

    char path[FS_PATH_MAX];
    if (!resolve_path(name, path)) {
        vga_write("write: failed\n");
        return;
    }

    // Each command writes one LINE, terminated. Neither used to, which
    // made a multi-line file impossible to author from the shell at all
    // -- `write f a` then `append f b` produced "ab", so every
    // line-based format this system has (/etc/toyos.conf, .desktop
    // entries) could be read by the shell and never written by it.
    //
    // Refuses rather than truncating when the line will not fit, the
    // same rule kfmt's formatters follow: a silently shortened config
    // line is a wrong value, not a cosmetic problem.
    char line[SHELL_WRITE_LINE_MAX];
    unsigned int n = k_strlen(rest);
    if (n + 2 > sizeof line) {
        vga_write("write: line too long\n");
        return;
    }
    k_memcpy(line, rest, n);
    line[n] = '\n';
    line[n + 1] = '\0';

    if (!fs_write(path, line, append)) vga_write("write: failed\n");
}

void cmd_rm(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: rm <file>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path)) {
        vga_write("rm: path too long\n");
        return;
    }
    if (!fs_delete(path)) {
        vga_write("rm: no such file, or a non-empty directory: ");
        vga_write(path);
        vga_putc('\n');
    }
}

// Same zero-padded "MM/DD/YYYY HH:MM:SS" shape for both timestamps
// below -- deliberately not shell_sys.c's `time` command's friendlier
// "Aug 10, 2026" layout, since that one owns a MONTHS[] table this file
// has no reason to duplicate just to print two extra fields. A real
// browsing tool (see docs/tfs2-spec.md) reads the raw on-disk fields
// directly rather than this formatting anyway.
static void print_padded(uint32_t n, int width) {
    // Only ever called with width 2 or 4 here, both comfortably within
    // uint32_t range for anything rtc_time can hold (year is 16-bit).
    uint32_t div = 1;
    for (int i = 1; i < width; i++) div *= 10;
    while (div > 1 && n < div) { vga_putc('0'); div /= 10; }
    vga_write_dec(n);
}

static void print_stat_timestamp(const struct rtc_time *t) {
    print_padded(t->month, 2);
    vga_putc('/');
    print_padded(t->day, 2);
    vga_putc('/');
    print_padded(t->year, 4);
    vga_write(" ");
    print_padded(t->hour, 2);
    vga_putc(':');
    print_padded(t->minute, 2);
    vga_putc(':');
    print_padded(t->second, 2);
}

void cmd_stat(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: stat <path>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path)) {
        vga_write("stat: path too long\n");
        return;
    }
    if (!fs_exists(path)) {
        vga_write("stat: no such file or directory: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }
    vga_write("  path:     "); vga_write(path); vga_putc('\n');
    vga_write("  type:     "); vga_write(fs_is_dir(path) ? "directory" : "file"); vga_putc('\n');
    if (!fs_is_dir(path)) {
        uint32_t size;
        fs_read(path, &size);
        vga_write("  size:     "); vga_write_dec(size); vga_write(" bytes\n");
    }
    struct fs_stat_info st;
    if (fs_stat(path, &st)) {
        // Timestamps come back as (local-derived) epoch seconds now --
        // see fs.h's fs_stat_info -- so display converts back to civil
        // time. The inode number is real on a backend with inodes,
        // synthetic-but-stable (table slot) otherwise; say which.
        struct rtc_time t;
        vga_write("  inode:    "); vga_write_dec((uint32_t)st.ino);
        vga_write(fs_has(FS_CAP_INODES) ? "\n" : " (synthetic)\n");
        tz_epoch_to_rtc(st.created, &t);
        vga_write("  created:  "); print_stat_timestamp(&t); vga_putc('\n');
        tz_epoch_to_rtc(st.modified, &t);
        vga_write("  modified: "); print_stat_timestamp(&t); vga_putc('\n');
    } else {
        // Only the implicit root "/" (no entry of its own -- see fs.h)
        // reaches here, since fs_exists() already confirmed everything
        // else really has an entry.
        vga_write("  created:  (root has no timestamp of its own)\n");
    }
}

void cmd_pwd(void) {
    vga_write(cwd);
    vga_putc('\n');
}

void cmd_cd(const char *args) {
    char path[FS_PATH_MAX];
    // Bare `cd` (no args) goes to root -- there's no $HOME concept here.
    if (!resolve_path((args && k_strlen(args) > 0) ? args : "/", path)) {
        vga_write("cd: path too long\n");
        return;
    }
    if (!fs_is_dir(path)) {
        vga_write("cd: not a directory: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }
    k_strcpy(cwd, path);
    // Tell the kernel too, so a ring-3 program started from here with
    // `run` or `spawn` inherits where this shell is standing. Without
    // it the two notions of "here" silently disagree: `cd /docs` then
    // `spawn /bin/mkdir notes` would create /notes.
    scheduler_set_kernel_cwd(path);
}

// Hardlink: `ln <existing> <newname>`. The caps mechanism's showcase
// command -- on a filesystem whose format has no link counts (tfs2)
// it says so instead of failing mysteriously.
void cmd_ln(const char *args) {
    char first[FS_PATH_MAX];
    int n = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < FS_PATH_MAX - 1) first[n++] = *p++;
    first[n] = '\0';
    while (*p == ' ') p++;

    if (n == 0 || !*p) {
        vga_write("usage: ln <existing-file> <new-name>\n");
        return;
    }
    if (!fs_has(FS_CAP_HARDLINKS)) {
        vga_write("ln: the active filesystem (");
        vga_write(fs_backend_name());
        vga_write(") has no hardlinks\n");
        return;
    }
    char src[FS_PATH_MAX], dst[FS_PATH_MAX];
    if (!resolve_path(first, src) || !resolve_path(p, dst)) {
        vga_write("ln: path too long\n");
        return;
    }
    if (!fs_exists(src)) {
        vga_write("ln: no such file: "); vga_write(src); vga_putc('\n');
        return;
    }
    if (fs_is_dir(src)) {
        vga_write("ln: hardlinks to directories are refused (they make the tree a graph)\n");
        return;
    }
    if (fs_exists(dst)) {
        vga_write("ln: already exists: "); vga_write(dst); vga_putc('\n');
        return;
    }
    if (fs_link(src, dst)) {
        vga_write("ln: "); vga_write(dst); vga_write(" -> same inode as "); vga_write(src); vga_putc('\n');
    } else {
        vga_write("ln: failed\n");
    }
}

// Splits "<a> <b>" into two resolved absolute paths. Returns 0 (and
// prints nothing) if either word is missing, so each caller can print
// its own usage line.
static int two_paths(const char *args, const char *what, char *a, char *b) {
    char first[FS_PATH_MAX];
    int n = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < FS_PATH_MAX - 1) first[n++] = *p++;
    first[n] = '\0';
    while (*p == ' ') p++;
    if (n == 0 || !*p) return 0;
    if (!resolve_path(first, a) || !resolve_path(p, b)) {
        vga_write(what); vga_write(": path too long\n");
        return -1;
    }
    return 1;
}

// `mv <src> <dst>` -- rename or move. Deliberately refuses to
// overwrite: fs_rename() has no atomic replace, and a move that
// silently destroys the destination is the one mistake this command
// can make that the user cannot undo.
void cmd_mv(const char *args) {
    char src[FS_PATH_MAX], dst[FS_PATH_MAX];
    int r = two_paths(args, "mv", src, dst);
    if (r < 0) return;
    if (r == 0) {
        vga_write("usage: mv <source> <destination>\n");
        return;
    }
    if (!fs_exists(src)) {
        vga_write("mv: no such file or directory: "); vga_write(src); vga_putc('\n');
        return;
    }
    if (fs_exists(dst)) {
        vga_write("mv: already exists: "); vga_write(dst);
        vga_write(" (remove it first -- mv never overwrites)\n");
        return;
    }
    // The one refusal this command can diagnose better than the
    // backend can: a directory moved under itself. Checked BEFORE the
    // call so the message names the actual reason instead of listing
    // candidates.
    if (fs_is_dir(src)) {
        size_t slen = k_strlen(src);
        if (k_strncmp(dst, src, slen) == 0 && dst[slen] == '/') {
            vga_write("mv: cannot move "); vga_write(src);
            vga_write(" inside itself\n");
            return;
        }
    }
    if (fs_rename(src, dst)) {
        vga_write("mv: "); vga_write(src); vga_write(" -> "); vga_write(dst); vga_putc('\n');
        return;
    }
    vga_write("mv: failed -- see dmesg\n");
}

// `truncate <file> <size>` -- set a file's size exactly. Growing is
// sparse, so `truncate big 1000000000` is instant and costs no blocks.
void cmd_truncate(const char *args) {
    char first[FS_PATH_MAX], path[FS_PATH_MAX];
    int n = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < FS_PATH_MAX - 1) first[n++] = *p++;
    first[n] = '\0';
    while (*p == ' ') p++;

    if (n == 0 || !*p) {
        vga_write("usage: truncate <file> <size-in-bytes>\n");
        return;
    }
    uint64_t size = 0;
    if (!k_parse_u64(p, &size)) {
        vga_write("truncate: not a number: "); vga_write(p); vga_putc('\n');
        return;
    }
    if (!resolve_path(first, path)) {
        vga_write("truncate: path too long\n");
        return;
    }
    if (!fs_exists(path)) {
        vga_write("truncate: no such file: "); vga_write(path); vga_putc('\n');
        return;
    }
    if (fs_is_dir(path)) {
        vga_write("truncate: "); vga_write(path); vga_write(" is a directory\n");
        return;
    }
    uint64_t was = fs_size(path);
    if (!fs_truncate(path, size)) {
        vga_write("truncate: failed\n");
        return;
    }
    vga_printf("truncate: %s %llu -> %llu bytes\n", path, (unsigned long long)was,
               (unsigned long long)size);
}

// Reformat the disk with a named backend and remount -- the live
// filesystem-switching path (see fs.h's fs_format_backend()). The
// `confirm` word is mandatory: this destroys everything on disk, and
// a destructive command that can be typed by accident is a bug in the
// command, not the user. Physical shell only -- the GUI Terminal's
// blocked-command list includes it (apps/terminal.c), since yanking
// the filesystem out from under open windows helps nobody.
void cmd_fsformat(const char *args) {
    char name[16];
    int n = 0;
    const char *p = args;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && n < (int)sizeof(name) - 1) name[n++] = *p++;
    name[n] = '\0';
    while (*p == ' ') p++;

    if (n == 0) {
        vga_write("usage: fsformat <tfs3> confirm\n");
        vga_write("       DESTROYS the current filesystem and reformats with the named one\n");
        return;
    }
    if (k_strcmp(p, "confirm") != 0) {
        vga_write("fsformat: this DESTROYS every file on disk.\n");
        vga_write("fsformat: run `fsformat "); vga_write(name);
        vga_write(" confirm` if that is really what you want.\n");
        return;
    }
    if (!fs_is_persistent()) {
        vga_write("fsformat: no disk (RAM-only boot) -- nothing to format\n");
        return;
    }
    vga_write("fsformat: formatting with "); vga_write(name); vga_write("...\n");
    if (fs_format_backend(name)) {
        k_strcpy(cwd, "/"); // the old working directory no longer exists
        vga_write("fsformat: done -- active filesystem is now ");
        vga_write(fs_backend_name());
        vga_putc('\n');
    } else {
        vga_write("fsformat: failed (unknown filesystem name, or the format itself failed -- see dmesg)\n");
        vga_write("fsformat: known names: tfs3\n");
    }
}
