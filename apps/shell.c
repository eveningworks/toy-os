#include "shell.h"
#include "kapi.h"
#include "apps.h"

#define LINE_MAX 128
#define HISTORY_MAX 8

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

static enum vga_color shell_fg = VGA_LIGHT_GREY;

// The shell's current directory -- always a normalized absolute path
// (see fs.h: starts with '/', no trailing slash except root itself, no
// "."/".." components), so it's always safe to hand straight to fs_*.
// fs.c itself has no notion of "current directory" -- every fs_* call
// still takes a full path; this is purely shell-side state, resolved
// against on every command via resolve_path() below.
static char cwd[FS_PATH_MAX] = "/";

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
static int resolve_path(const char *input, char *out) {
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

static void print_two_digit(uint32_t n) {
    if (n < 10) vga_putc('0');
    vga_write_dec(n);
}

// Prints `lines` one at a time (each expected to be one console line,
// i.e. end in '\n'), pausing with a "-- more --" prompt whenever a
// screenful has gone by, rather than printing everything at once and
// letting the console's own scroll-off-the-top behavior silently
// discard whatever didn't fit (which is what used to happen to the
// tail end of `help`'s ~44 lines against a 32-row screen at the
// default font size). Any key continues to the next page; 'q'/'Q'
// stops early. General enough for any future command whose output
// might outgrow one screen -- not just `help`.
static void console_page(const char *const *lines, uint32_t count) {
    // A sink means this might be running inside a non-blocking GUI
    // callback (see apps/terminal.c, phase 4) rather than the
    // interactive console loop's own blocking read -- keyboard_getchar()
    // below would hang whatever's driving that callback forever. Just
    // dump everything unpaginated instead; a GUI caller's own scrollback
    // widget (phase 3) is what handles "doesn't fit on one screen" in
    // that context, the same job this pagination does for the physical
    // console.
    if (vga_sink_active()) {
        for (uint32_t i = 0; i < count; i++) vga_write(lines[i]);
        return;
    }

    uint32_t rows = vga_rows();
    uint32_t page_rows = rows > 1 ? rows - 1 : rows; // reserve the bottom row for the prompt
    uint32_t shown = 0;

    for (uint32_t i = 0; i < count; i++) {
        vga_write(lines[i]);
        shown++;
        if (shown >= page_rows && i + 1 < count) {
            vga_write("-- more (press any key, 'q' to quit) --");
            int key = keyboard_getchar();
            vga_write("\n");
            if (key == 'q' || key == 'Q') return;
            shown = 0;
        }
    }
}

static const char *const HELP_LINES[] = {
    "Available commands:\n",
    "  help          - show this list\n",
    "  clear         - clear the screen\n",
    "  time          - show date/time (local, see `timezone`)\n",
    "  timezone      - show/pick your timezone (interactive list)\n",
    "  timezone <c>  - set timezone directly, e.g. `timezone helsinki`\n",
    "  uptime        - show ticks since boot\n",
    "  echo <text>   - print the given text back\n",
    "  about         - show OS info\n",
    "  meminfo       - show memory map from GRUB\n",
    "  color <name>  - change shell text color\n",
    "  reboot        - reset the machine\n",
    "  ls [dir]      - list a directory (default: cwd)\n",
    "  cd [dir]      - change directory (default: /)\n",
    "  pwd           - print the current directory\n",
    "  mkdir <dir>   - create a directory\n",
    "  cat <f>       - print a file's contents\n",
    "  touch <f>     - create an empty file\n",
    "  write <f> <t> - overwrite file f with text t\n",
    "  append <f> <t>- append text t to file f\n",
    "  rm <f>        - delete a file, or an empty directory\n",
    "  (paths may be relative to cwd or absolute, e.g. /docs/todo.txt)\n",
    "  gui           - graphics mode (Esc returns here)\n",
    "  apps          - list all registered apps\n",
    "  history       - list past commands\n",
    "  run <app>     - launch an app by name\n",
    "  ring3test     - proof-of-concept ring 3 + paging isolation\n",
    "                  (does not return; see README)\n",
    "  elftest       - load+run a real ELF64 binary in ring 3\n",
    "                  (does not return; see README)\n",
    "  syscalltest   - real exit syscall round-trip (returns!)\n",
    "  writetest     - real write syscall (process prints its\n",
    "                  own output, then exits)\n",
    "  ptrtest       - proves write's pointer validation works\n",
    "                  (deliberately bad pointer, expects reject)\n",
    "  guitest       - ring-3 process draws the real screen\n",
    "                  (modal; any key cycles color, 'q' returns)\n",
    "  schedtest     - preemptive round-robin scheduler demo: two\n",
    "                  ring-3 processes run concurrently, neither\n",
    "                  ever yielding (returns once both exit)\n",
    "  fontsize <s>  - set font size: tiny, small, medium, or large\n",
    "  echotest      - interactive ring-3 process: type and see it\n",
    "                  echoed back via SYS_READ_KEY+SYS_SBRK (Esc\n",
    "                  quits, returns to the shell)\n",
    "  wintest       - ring-3 process with its own private pixel\n",
    "                  buffer, composited into a real chrome'd\n",
    "                  window by the kernel (any key cycles color,\n",
    "                  'q'/Esc returns to the shell)\n",
    "  filetest      - ring-3 process writes+reads a real file via\n",
    "                  SYS_OPEN/SYS_READ/SYS_WRITE/SYS_CLOSE (returns)\n",
    "  newsyscalltest - ring-3 process exercises SYS_UNLINK,\n",
    "                  SYS_LISTDIR, SYS_GETTIME, SYS_YIELD (returns)\n",
    "  crashtest     - ring-3 process deliberately faults -- proves the\n",
    "                  kernel recovers (tears it down, returns) instead\n",
    "                  of halting (returns)\n",
    "(arrows browse history; 'history' lists it)\n",
};
#define HELP_LINE_COUNT (sizeof(HELP_LINES) / sizeof(HELP_LINES[0]))

static void cmd_help(void) {
    console_page(HELP_LINES, HELP_LINE_COUNT);
}

static void cmd_time(void) {
    struct rtc_time t;
    rtc_read_local(&t); // local time for the selected `timezone`, not raw UTC

    vga_write(MONTHS[(t.month >= 1 && t.month <= 12) ? t.month - 1 : 0]);
    vga_putc(' ');
    print_two_digit(t.day);
    vga_write(", ");
    vga_write_dec(t.year);
    vga_write("  ");
    print_two_digit(t.hour);
    vga_putc(':');
    print_two_digit(t.minute);
    vga_putc(':');
    print_two_digit(t.second);
    vga_write("  (");
    vga_write(tz_city_name(tz_current_index()));
    vga_write(")\n");
}

// `timezone` alone: numbered list, prompts for a choice.
// `timezone <name>`: sets directly, matching tz_city_name() exactly
// (lowercase, e.g. `timezone losangeles`) -- same case-sensitive,
// exact-match convention `color <name>` already uses.
static void cmd_timezone(const char *args) {
    if (args && k_strlen(args) > 0) {
        int idx = tz_find_by_name(args);
        if (idx < 0) {
            vga_write("Unknown timezone. Run `timezone` with no arguments to see the list.\n");
            return;
        }
        tz_set_index(idx);
        vga_write("Timezone set to ");
        vga_write(tz_city_name(idx));
        vga_write(".\n");
        return;
    }

    // The no-args branch below blocks on keyboard_read_line() waiting
    // for a numbered choice -- fine for the interactive console loop
    // (shell_main() calls its own keyboard_getchar() in a loop already),
    // fatal for a non-blocking GUI callback driving this through a sink
    // (see shell_dispatch()'s comment). Print the list plus a pointer to
    // the direct-set form instead of blocking.
    if (vga_sink_active()) {
        vga_write("Interactive timezone picker isn't available here --\n");
        vga_write("use `timezone <city>` instead. Cities:\n");
        int n = tz_city_count();
        for (int i = 0; i < n; i++) {
            vga_write("  ");
            vga_write(tz_city_name(i));
            vga_putc('\n');
        }
        return;
    }

    int count = tz_city_count();
    int current = tz_current_index();
    for (int i = 0; i < count; i++) {
        vga_write_dec((uint32_t)(i + 1));
        vga_write(i == current ? ") * " : ")   ");
        vga_write(tz_city_name(i));
        vga_putc('\n');
    }
    vga_write("Enter a number (blank to cancel): ");

    char buf[8];
    keyboard_read_line(buf, sizeof(buf));
    if (k_strlen(buf) == 0) {
        vga_write("Cancelled.\n");
        return;
    }

    int choice = 0;
    for (const char *p = buf; *p; p++) {
        if (*p < '0' || *p > '9') { choice = -1; break; }
        choice = choice * 10 + (*p - '0');
    }
    if (choice < 1 || choice > count) {
        vga_write("Not a valid choice.\n");
        return;
    }
    tz_set_index(choice - 1);
    vga_write("Timezone set to ");
    vga_write(tz_city_name(choice - 1));
    vga_write(".\n");
}

static void cmd_uptime(void) {
    uint64_t ticks = pit_ticks(); // 100 Hz
    vga_write_dec((uint32_t)(ticks / 100));
    vga_write(".");
    print_two_digit((uint32_t)(ticks % 100));
    vga_write(" seconds since boot\n");
}

static void cmd_about(void) {
    vga_write("toy-os build "); vga_write(TOYOS_VERSION);
    vga_write(" -- a small x86-64 hobby kernel\n");
    vga_write("Boot: GRUB/Multiboot2 | C + ASM | Tested on QEMU\n");
    vga_write("Storage: ");
    vga_write(fs_is_persistent() ? "disk-backed (files persist across reboots)\n"
                                  : "RAM only (no disk found -- files won't survive a reboot)\n");
}

static void cmd_echo(const char *args) {
    vga_write(args);
    vga_putc('\n');
}

static void cmd_meminfo(void) {
    multiboot_print_meminfo();

    uint64_t total = pmm_total_frames();
    uint64_t free = pmm_free_frames();
    uint64_t used = total - free;

    vga_write("\nPhysical frame allocator (4KB frames):\n");
    vga_write("  total: "); vga_write_dec((uint32_t)total);
    vga_write(" ("); vga_write_dec((uint32_t)(total * 4 / 1024)); vga_write(" MB)\n");
    vga_write("  used:  "); vga_write_dec((uint32_t)used);
    vga_write("  free:  "); vga_write_dec((uint32_t)free); vga_putc('\n');
}

static void cmd_reboot(void) {
    vga_write("Rebooting...\n");
    system_reboot();
}

static void apps_list_cb(const char *name, const char *description) {
    vga_write("  ");
    vga_write(name);
    vga_write("  - ");
    vga_write(description);
    vga_putc('\n');
}

static void cmd_apps(void) {
    vga_write("Registered apps:\n");
    app_list(apps_list_cb);
}

static void cmd_run(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: run <app>  (see 'apps' for the list)\n");
        return;
    }
    if (!app_run(name)) {
        vga_write("run: no such app: ");
        vga_write(name);
        vga_putc('\n');
        return;
    }
    // An app may have drawn over the whole screen (e.g. gui) -- refresh
    // the console on return so the shell prompt is clean either way.
    vga_clear();
    vga_set_color(shell_fg, VGA_BLACK);
}

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

static void cmd_ls(const char *args) {
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

static void cmd_cat(const char *name) {
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

static void cmd_touch(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: touch <file>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path) || !fs_touch(path)) {
        vga_write("touch: failed (bad path, missing parent directory, or disk full)\n");
    }
}

static void cmd_mkdir(const char *name) {
    if (!name || k_strlen(name) == 0) {
        vga_write("usage: mkdir <dir>\n");
        return;
    }
    char path[FS_PATH_MAX];
    if (!resolve_path(name, path) || !fs_mkdir(path)) {
        vga_write("mkdir: failed (bad path, missing parent directory, already exists, or disk full)\n");
    }
}

static void cmd_write_or_append(const char *args, int append) {
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

static void cmd_rm(const char *name) {
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

static void cmd_pwd(void) {
    vga_write(cwd);
    vga_putc('\n');
}

static void cmd_cd(const char *args) {
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

static enum vga_color color_from_name(const char *s) {
    if (k_strcmp(s, "black") == 0) return VGA_BLACK;
    if (k_strcmp(s, "blue") == 0) return VGA_BLUE;
    if (k_strcmp(s, "green") == 0) return VGA_GREEN;
    if (k_strcmp(s, "cyan") == 0) return VGA_CYAN;
    if (k_strcmp(s, "red") == 0) return VGA_RED;
    if (k_strcmp(s, "magenta") == 0) return VGA_MAGENTA;
    if (k_strcmp(s, "brown") == 0) return VGA_BROWN;
    if (k_strcmp(s, "lightgrey") == 0) return VGA_LIGHT_GREY;
    if (k_strcmp(s, "darkgrey") == 0) return VGA_DARK_GREY;
    if (k_strcmp(s, "lightblue") == 0) return VGA_LIGHT_BLUE;
    if (k_strcmp(s, "lightgreen") == 0) return VGA_LIGHT_GREEN;
    if (k_strcmp(s, "lightcyan") == 0) return VGA_LIGHT_CYAN;
    if (k_strcmp(s, "lightred") == 0) return VGA_LIGHT_RED;
    if (k_strcmp(s, "lightmagenta") == 0) return VGA_LIGHT_MAGENTA;
    if (k_strcmp(s, "yellow") == 0) return VGA_LIGHT_BROWN;
    if (k_strcmp(s, "white") == 0) return VGA_WHITE;
    return VGA_LIGHT_GREY;
}

// Changes the console's font size (see gfx_set_font_size() / font_ttf.h
// -- four sizes baked at build time by tools/genttf.py, not runtime
// TrueType rendering). Only affects the framebuffer console; the legacy
// 80x25 text-mode fallback has one fixed cell size and can't resize.
// Persists the choice to /etc/fontsize (see font_config.h) so it
// survives a reboot -- same pattern as `timezone` persisting via tz.c.
static void cmd_fontsize(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: fontsize <tiny|small|medium|large>  (currently: ");
        vga_write(gfx_font_size_name(gfx_font_size()));
        vga_write(")\n");
        return;
    }
    enum font_size want;
    if (k_strcmp(args, "tiny") == 0) want = FONT_SIZE_TINY;
    else if (k_strcmp(args, "small") == 0) want = FONT_SIZE_SMALL;
    else if (k_strcmp(args, "medium") == 0) want = FONT_SIZE_MEDIUM;
    else if (k_strcmp(args, "large") == 0) want = FONT_SIZE_LARGE;
    else {
        vga_write("fontsize: unknown size '");
        vga_write(args);
        vga_write("' -- try tiny, small, medium, or large\n");
        return;
    }
    gfx_set_font_size(want);
    vga_reflow(); // recompute console_cols/rows for the new cell size and clear
    font_config_save(want); // persist to /etc/fontsize so it survives a reboot
    vga_write("Font size set to ");
    vga_write(gfx_font_size_name(want));
    vga_write(".\n");
}

static void cmd_color(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: color <name> (green, lightcyan, white, red)\n");
        return;
    }
    shell_fg = color_from_name(args);
    vga_set_color(shell_fg, VGA_BLACK);
    vga_write("Color set.\n");
}

// Declared here (rather than down near shell_read_line, which is what
// actually fills this in via the up/down arrow keys) so cmd_history()
// below can see it too -- a single file-scope declaration, used by both.
static char history[HISTORY_MAX][LINE_MAX];
static int history_count = 0; // number of entries stored (caps at HISTORY_MAX)

static void cmd_history(void) {
    if (history_count == 0) {
        vga_write("(no commands yet)\n");
        return;
    }
    for (int i = 0; i < history_count; i++) {
        vga_write_dec((uint32_t)(i + 1));
        vga_write("  ");
        vga_write(history[i]);
        vga_putc('\n');
    }
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
        cmd_help();
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
    } else if (k_strcmp(cmd, "reboot") == 0) {
        cmd_reboot();
    } else if (k_strcmp(cmd, "color") == 0) {
        cmd_color(args ? args : "");
    } else if (k_strcmp(cmd, "ls") == 0) {
        cmd_ls(args ? args : "");
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
    } else if (k_strcmp(cmd, "elftest") == 0) {
        vga_write("Loading and running a real ELF64 binary in ring 3. This\n");
        vga_write("does NOT return -- see the diagnostic output for what it\n");
        vga_write("proves.\n\n");
        elf_test_run();
    } else if (k_strcmp(cmd, "syscalltest") == 0) {
        syscall_test_run();
    } else if (k_strcmp(cmd, "writetest") == 0) {
        write_test_run();
    } else if (k_strcmp(cmd, "ptrtest") == 0) {
        ptr_test_run();
    } else if (k_strcmp(cmd, "guitest") == 0) {
        gui_test_run();
    } else if (k_strcmp(cmd, "schedtest") == 0) {
        scheduler_demo_run();
    } else if (k_strcmp(cmd, "fontsize") == 0) {
        cmd_fontsize(args ? args : "");
    } else if (k_strcmp(cmd, "echotest") == 0) {
        echo_test_run();
    } else if (k_strcmp(cmd, "wintest") == 0) {
        win_test_run();
    } else if (k_strcmp(cmd, "filetest") == 0) {
        file_test_run();
    } else if (k_strcmp(cmd, "newsyscalltest") == 0) {
        newsyscalls_test_run();
    } else if (k_strcmp(cmd, "crashtest") == 0) {
        crash_test_run();
    } else if (k_strcmp(cmd, "history") == 0) {
        cmd_history();
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
        } else if (c < 128 && pos < len - 1) {
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
