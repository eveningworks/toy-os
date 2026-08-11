// The shell: REPL loop (shell_main()/shell_read_line()), the single
// command dispatcher (dispatch()/shell_dispatch()), and the state every
// command shares (cwd, shell_fg, history). The commands themselves live
// in apps/shell_fs.c (filesystem) and apps/shell_sys.c (system-info/
// settings) -- split out once this file crossed 900 lines mixing every
// command category together (see CHANGELOG.md for the build this
// happened in). See shell_internal.h's top comment for why this is a
// three-file split sharing state via `extern`s, not three independent
// components.
#include "shell.h"
#include "shell_internal.h"
#include "apps.h"

// Shell-wide state -- declared `extern` in shell_internal.h for
// shell_fs.c/shell_sys.c, defined here since this is the file that owns
// the REPL loop and is the only place any of these actually change
// (aside from cmd_cd()/cmd_color()/etc. in the split files, which is
// exactly what the extern sharing is for).
enum vga_color shell_fg = VGA_LIGHT_GREY;

// The shell's current directory -- always a normalized absolute path
// (see fs.h: starts with '/', no trailing slash except root itself, no
// "."/".." components), so it's always safe to hand straight to fs_*.
// fs.c itself has no notion of "current directory" -- every fs_* call
// still takes a full path; this is purely shell-side state, resolved
// against on every command via resolve_path() below.
char cwd[FS_PATH_MAX] = "/";

// Declared here (rather than down near shell_read_line, which is what
// actually fills this in via the up/down arrow keys) so cmd_history()
// (shell_sys.c) can see it too, via shell_internal.h's extern.
char history[HISTORY_MAX][LINE_MAX];
int history_count = 0; // number of entries stored (caps at HISTORY_MAX)

// Resolves `input` (absolute if it starts with '/', otherwise relative
// to `cwd`) into a normalized absolute path in `out`, collapsing "."
// and ".." components along the way (so `cd ..`, `cat ../notes.txt`
// etc. work). A blank/NULL `input` resolves to `cwd` itself. Returns 1
// on success, 0 if the result would be empty, too deep, or too long
// for `out` (size FS_PATH_MAX).
//
// This is deliberately shell-side, hand-rolled logic, not something
// fs.c does -- fs.c only ever sees already-normalized absolute paths
// (see its own top comment), the same boundary the rest of the kernel
// draws between "the shell's job" and "the filesystem's job".
int resolve_path(const char *input, char *out) {
    char combined[3 * FS_PATH_MAX];

    if (!input || k_strlen(input) == 0) {
        k_strcpy(out, cwd);
        return 1;
    }

    if (input[0] == '/') {
        if (k_strlen(input) >= sizeof(combined)) return 0;
        k_strcpy(combined, input);
    } else {
        size_t cl = k_strlen(cwd);
        if (cl >= sizeof(combined)) return 0;
        k_strcpy(combined, cwd);
        if (cl > 1) { // cwd isn't just "/" -- needs a separating slash
            if (cl + 1 >= sizeof(combined)) return 0;
            combined[cl] = '/';
            combined[cl + 1] = '\0';
            cl++;
        }
        if (cl + k_strlen(input) >= sizeof(combined)) return 0;
        k_strcpy(combined + cl, input);
    }

    // Split on '/', processing "." (skip) and ".." (pop) as we go, into
    // a stack of pointers back into `combined` (mutated in place with
    // '\0's at each separator so each stack entry is its own C string).
    char *stack[16];
    int depth = 0;
    char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        int seglen = (int)(p - start);
        int had_slash = (*p == '/');
        if (had_slash) *p = '\0';

        if (seglen == 1 && start[0] == '.') {
            // no-op
        } else if (seglen == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) depth--;
        } else if (seglen > 0) {
            if (depth >= 16) return 0; // too deep
            stack[depth++] = start;
        }

        if (had_slash) p++;
    }

    size_t pos = 1;
    out[0] = '/';
    out[1] = '\0';
    for (int i = 0; i < depth; i++) {
        size_t seglen = k_strlen(stack[i]);
        if (i > 0) {
            if (pos + 1 >= FS_PATH_MAX) return 0;
            out[pos++] = '/';
        }
        if (pos + seglen >= FS_PATH_MAX) return 0;
        k_memcpy(out + pos, stack[i], seglen);
        pos += seglen;
        out[pos] = '\0';
    }
    return 1;
}

static void dispatch(char *line) {
    // split first word from the rest
    char *cmd = line;
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args == ' ') {
        *args = '\0';
        args++;
        while (*args == ' ') args++;
    } else {
        args = 0;
    }

    if (k_strlen(cmd) == 0) {
        return;
    } else if (k_strcmp(cmd, "help") == 0) {
        cmd_help(args);
    } else if (k_strcmp(cmd, "clear") == 0) {
        vga_clear();
    } else if (k_strcmp(cmd, "time") == 0) {
        cmd_time();
    } else if (k_strcmp(cmd, "timezone") == 0) {
        cmd_timezone(args);
    } else if (k_strcmp(cmd, "uptime") == 0) {
        cmd_uptime();
    } else if (k_strcmp(cmd, "about") == 0) {
        cmd_about();
    } else if (k_strcmp(cmd, "echo") == 0) {
        cmd_echo(args ? args : "");
    } else if (k_strcmp(cmd, "meminfo") == 0) {
        cmd_meminfo();
    } else if (k_strcmp(cmd, "dmesg") == 0) {
        cmd_dmesg();
    } else if (k_strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (k_strcmp(cmd, "color") == 0) {
        cmd_color(args ? args : "");
    } else if (k_strcmp(cmd, "ls") == 0) {
        cmd_ls_bin(args ? args : "");
    } else if (k_strcmp(cmd, "cat") == 0) {
        cmd_cat(args ? args : "");
    } else if (k_strcmp(cmd, "touch") == 0) {
        cmd_touch(args ? args : "");
    } else if (k_strcmp(cmd, "mkdir") == 0) {
        cmd_mkdir(args ? args : "");
    } else if (k_strcmp(cmd, "cd") == 0) {
        cmd_cd(args ? args : "");
    } else if (k_strcmp(cmd, "pwd") == 0) {
        cmd_pwd();
    } else if (k_strcmp(cmd, "write") == 0) {
        cmd_write_or_append(args ? args : "", 0);
    } else if (k_strcmp(cmd, "append") == 0) {
        cmd_write_or_append(args ? args : "", 1);
    } else if (k_strcmp(cmd, "rm") == 0) {
        cmd_rm(args ? args : "");
    } else if (k_strcmp(cmd, "stat") == 0) {
        cmd_stat(args ? args : "");
    } else if (k_strcmp(cmd, "edit") == 0 || k_strcmp(cmd, "nano") == 0) {
        cmd_edit(args ? args : "");
    } else if (k_strcmp(cmd, "gui") == 0) {
        app_run("gui"); // shortcut for `run gui`
        vga_clear();
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_write("Back from GUI mode.\n");
        vga_set_color(shell_fg, VGA_BLACK);
    } else if (k_strcmp(cmd, "apps") == 0) {
        cmd_apps();
    } else if (k_strcmp(cmd, "run") == 0) {
        cmd_run(args ? args : "");
    } else if (k_strcmp(cmd, "ring3test") == 0) {
        vga_write("Running ring-3 isolation test. This does NOT return --\n");
        vga_write("see the diagnostic output for what it proves.\n\n");
        ring3_test_run();
    } else if (k_strcmp(cmd, "schedtest") == 0) {
        scheduler_demo_run();
    } else if (k_strcmp(cmd, "fontsize") == 0) {
        cmd_fontsize(args ? args : "");
    } else if (k_strcmp(cmd, "keyboard") == 0) {
        cmd_keyboard(args ? args : "");
    } else if (k_strcmp(cmd, "history") == 0) {
        cmd_history();
    } else if (k_strcmp(cmd, "lspci") == 0) {
        cmd_lspci();
    } else {
        vga_write("Unknown command: ");
        vga_write(cmd);
        vga_write("\n(type 'help' for a list of commands)\n");
    }
}

// See shell.h's doc comment -- the public entry point that lets a GUI
// terminal-emulator app run a command line through the real dispatcher.
void shell_dispatch(char *line, const struct vga_sink *sink) {
    const struct vga_sink *prev = vga_set_sink(sink);
    dispatch(line);
    vga_set_sink(prev);
}

const char *shell_cwd(void) {
    return cwd;
}

static void history_add(const char *line) {
    if (k_strlen(line) == 0) return;
    if (history_count < HISTORY_MAX) {
        k_strcpy(history[history_count], line);
        history_count++;
    } else {
        // drop oldest, shift up
        for (int i = 1; i < HISTORY_MAX; i++) k_strcpy(history[i - 1], history[i]);
        k_strcpy(history[HISTORY_MAX - 1], line);
    }
}

// Redraw the current input line in place (erase old, print new).
static void redraw_line(const char *old, const char *new_line) {
    unsigned int old_len = (unsigned int)k_strlen(old);
    for (unsigned int i = 0; i < old_len; i++) vga_backspace();
    vga_write(new_line);
}

static void shell_read_line(char *buf, unsigned int len) {
    unsigned int pos = 0;
    buf[0] = '\0';
    int hist_index = history_count; // one past the newest = "current blank line"
    char saved_current[LINE_MAX];
    saved_current[0] = '\0';

    for (;;) {
        int c = keyboard_getchar();

        if (c == '\n') {
            vga_putc('\n');
            buf[pos] = '\0';
            break;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                vga_backspace();
            }
        } else if (c == KEY_ARROW_UP) {
            if (hist_index > 0) {
                if (hist_index == history_count) {
                    buf[pos] = '\0';
                    k_strcpy(saved_current, buf);
                }
                hist_index--;
                redraw_line(buf, history[hist_index]);
                k_strcpy(buf, history[hist_index]);
                pos = (unsigned int)k_strlen(buf);
            }
        } else if (c == KEY_ARROW_DOWN) {
            if (hist_index < history_count) {
                hist_index++;
                const char *replacement = (hist_index == history_count) ? saved_current : history[hist_index];
                buf[pos] = '\0';
                redraw_line(buf, replacement);
                k_strcpy(buf, replacement);
                pos = (unsigned int)k_strlen(buf);
            }
        } else if (IS_PRINTABLE_KEY(c) && pos < len - 1) {
            char ch = (char)c;
            buf[pos++] = ch;
            buf[pos] = '\0';
            vga_putc(ch);
        }
    }
}

void shell_main(void) {
    char line[LINE_MAX];

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("toy-os shell -- type 'help' to get started\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (;;) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write(cwd);
        vga_write("> ");
        vga_set_color(shell_fg, VGA_BLACK);

        shell_read_line(line, LINE_MAX);
        history_add(line);
        dispatch(line);
    }
}
