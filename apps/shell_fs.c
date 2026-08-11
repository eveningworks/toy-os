// Filesystem-related shell commands: cat/touch/mkdir/write/append/
// rm/pwd/cd/edit. Split out of shell.c once it crossed 900 lines mixing
// every command category together -- see shell_internal.h's top
// comment for the split's own reasoning and CHANGELOG.md for the build
// this happened in. Shares `cwd`/resolve_path() with shell.c and
// shell_sys.c via shell_internal.h.
//
// `ls` used to live here as a kernel-space built-in (fs_list() called
// directly) -- it migrated to a real /bin binary (userland/ls.c),
// invoked via cmd_ls_bin() in shell_sys.c, alongside the rest of the
// disk-hosted-binary commands (cmd_run(), cmd_lspci()'s sibling). See
// CHANGELOG.md/docs/decisions.md for why.
#include "shell_internal.h"
#include "editor.h"

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
    uint32_t size;
    const char *data = fs_read(path, &size);
    if (!data) {
        vga_write("cat: no such file: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }
    vga_write(data);
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
    if (!resolve_path(name, path) || !fs_write(path, rest, append)) {
        vga_write("write: failed\n");
    }
}

// editor_run() blocks the calling context in its own keyboard-read
// loop for the whole editing session -- exactly the class of command
// shell_dispatch() through a vga_sink can't support (see vga.h's
// struct vga_sink comment and terminal.c's BLOCKED_CMDS top comment).
// apps/terminal.c gets `edit`/`nano` working anyway by intercepting the
// command itself and driving editor_handle_key() through its own
// non-blocking per-keystroke callback instead of ever calling
// editor_run() through a sink -- see terminal.c's top comment. This
// guard is a defensive backstop for the case that path is somehow
// bypassed, not the normal way GUI Terminal support works.
void cmd_edit(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: edit <file>\n");
        return;
    }
    if (vga_sink_active()) {
        vga_write("edit: not available here -- run it from the physical shell.\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path)) {
        vga_write("edit: path too long\n");
        return;
    }
    editor_run(path);
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
    struct fs_timestamps ts;
    if (fs_stat(path, &ts)) {
        vga_write("  created:  "); print_stat_timestamp(&ts.created); vga_putc('\n');
        vga_write("  modified: "); print_stat_timestamp(&ts.modified); vga_putc('\n');
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
}
