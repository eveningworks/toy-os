// Filesystem-related shell commands: ls/cat/touch/mkdir/write/append/
// rm/pwd/cd/edit. Split out of shell.c once it crossed 900 lines mixing
// every command category together -- see shell_internal.h's top
// comment for the split's own reasoning and CHANGELOG.md for the build
// this happened in. Shares `cwd`/resolve_path() with shell.c and
// shell_sys.c via shell_internal.h.
#include "shell_internal.h"
#include "editor.h"

static void list_cb(const char *name, uint32_t size, int is_dir) {
    vga_write("  ");
    vga_write(name);
    if (is_dir) {
        vga_write("/\n"); // trailing slash marks directories, no size shown
    } else {
        vga_write("  (");
        vga_write_dec(size);
        vga_write(" bytes)\n");
    }
}

void cmd_ls(const char *args) {
    char path[FS_PATH_MAX];
    if (!resolve_path(args, path)) {
        vga_write("ls: path too long\n");
        return;
    }
    if (!fs_is_dir(path)) {
        vga_write("ls: not a directory: ");
        vga_write(path);
        vga_putc('\n');
        return;
    }
    fs_list(path, list_cb);
}

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
