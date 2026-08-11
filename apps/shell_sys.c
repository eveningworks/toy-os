// System-info/settings shell commands: help/time/timezone/uptime/
// about/echo/meminfo/dmesg/reboot/apps/run/fontsize/keyboard/color/
// history/lspci. Split out of shell.c once it crossed 900 lines mixing every
// command category together -- see shell_internal.h's top comment for
// the split's own reasoning and CHANGELOG.md for the build this
// happened in. Shares `shell_fg`/history[]/history_count with shell.c
// (and shell_fs.c) via shell_internal.h.
#include "shell_internal.h"
#include "apps.h"

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

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

// Split in two (see CHANGELOG.md for the request/reasoning): HELP_LINES
// is what a day-to-day user actually needs, grouped under headers
// rather than one flat 50-line list. TEST_HELP_LINES holds the ring-3/
// syscall/scheduler diagnostic commands -- genuinely useful (they're
// how this kernel proves its own isolation/syscall/scheduler claims,
// see README), but not something you need in front of you to use the
// shell day to day, so they're one level down behind `help tests`
// rather than mixed into the main list. Both go through console_page()
// (see its own comment) for pagination, same as before the split.
static const char *const HELP_LINES[] = {
    "toy-os shell -- available commands:\n",
    "\n",
    "General:\n",
    "  help          - show this list ('help tests' for developer/\n",
    "                  diagnostic test commands)\n",
    "  clear         - clear the screen\n",
    "  about         - show OS info\n",
    "  apps          - list all registered apps\n",
    "  run <app>     - launch an app by name\n",
    "  gui           - graphics mode (Esc returns here)\n",
    "  history       - list past commands (arrows browse history)\n",
    "  echo <text>   - print the given text back\n",
    "  reboot        - reset the machine\n",
    "\n",
    "Files & filesystem:\n",
    "  ls [-al] [dir]- list a directory (default: cwd); -l for type/\n",
    "                  size/mtime, -a accepted (no-op, no dotfiles here)\n",
    "  cd [dir]      - change directory (default: /)\n",
    "  pwd           - print the current directory\n",
    "  mkdir <dir>   - create a directory\n",
    "  cat <f>       - print a file's contents\n",
    "  touch <f>     - create an empty file\n",
    "  write <f> <t> - overwrite file f with text t\n",
    "  append <f> <t>- append text t to file f\n",
    "  rm <f>        - delete a file, or an empty directory\n",
    "  stat <f>      - show a file/directory's type, size, and\n",
    "                  created/modified timestamps\n",
    "  edit <f>      - full-screen text editor (nano/pico-style); also\n",
    "                  `nano <f>`. Arrows/Home/End/Delete to navigate and\n",
    "                  edit, F2 to save, F3 to exit. Also works inside\n",
    "                  the GUI Terminal (same command).\n",
    "  (paths may be relative to cwd or absolute, e.g. /docs/todo.txt)\n",
    "\n",
    "System info:\n",
    "  time          - show date/time (local, see `timezone`)\n",
    "  timezone      - show/pick your timezone (interactive list)\n",
    "  timezone <c>  - set timezone directly, e.g. `timezone helsinki`\n",
    "  uptime        - show ticks since boot\n",
    "  meminfo       - show memory map + physical frame allocator stats\n",
    "  dmesg         - show the kernel log (boot messages, driver/\n",
    "                  syscall diagnostics, timestamped)\n",
    "  lspci         - list PCI devices found at boot (bus:dev.func,\n",
    "                  vendor:device ID, class, IRQ, BARs)\n",
    "\n",
    "Appearance:\n",
    "  color <name>  - change shell text color\n",
    "  fontsize <n>  - set font size in points: 8, 10, 12, 14, 16, 18,\n",
    "                  20, or 24 (`fontsize` alone shows the current one)\n",
    "  keyboard <l>  - set keyboard layout: us or se (Swedish/Finnish --\n",
    "                  Å/Ä/Ö at their real physical keys); `keyboard`\n",
    "                  alone shows the current one\n",
};
#define HELP_LINE_COUNT (sizeof(HELP_LINES) / sizeof(HELP_LINES[0]))

static const char *const TEST_HELP_LINES[] = {
    "toy-os shell -- developer/diagnostic test commands:\n",
    "(these exercise specific kernel subsystems -- see README for what\n",
    "each one actually proves; most are meant to be read via their\n",
    "diagnostic output, not used for everyday work)\n",
    "\n",
    "  ring3test     - proof-of-concept ring 3 + paging isolation\n",
    "                  (does not return; see README)\n",
    "  schedtest     - preemptive round-robin scheduler demo: two\n",
    "                  ring-3 processes run concurrently, neither\n",
    "                  ever yielding (returns once both exit)\n",
    "\n",
    "Every other former *test command (elftest, syscalltest, writetest,\n",
    "ptrtest, guitest, echotest, wintest, filetest, newsyscalltest,\n",
    "crashtest, sockettest) is now a real disk-hosted binary under /bin\n",
    "instead of a dedicated command -- see `ls /bin` and `run <name>`\n",
    "(e.g. `run write_test`). See docs/decisions.md for why.\n",
    "\n",
    "Run `help` (no arguments) for everyday commands.\n",
};
#define TEST_HELP_LINE_COUNT (sizeof(TEST_HELP_LINES) / sizeof(TEST_HELP_LINES[0]))

void cmd_help(const char *args) {
    if (args && k_strcmp(args, "tests") == 0) {
        console_page(TEST_HELP_LINES, TEST_HELP_LINE_COUNT);
    } else {
        console_page(HELP_LINES, HELP_LINE_COUNT);
    }
}

void cmd_time(void) {
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
void cmd_timezone(const char *args) {
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

void cmd_uptime(void) {
    uint64_t ticks = pit_ticks(); // 100 Hz
    vga_write_dec((uint32_t)(ticks / 100));
    vga_write(".");
    print_two_digit((uint32_t)(ticks % 100));
    vga_write(" seconds since boot\n");
}

void cmd_about(void) {
    vga_write("toy-os v"); vga_write(TOYOS_VERSION);
    vga_write(" -- a small x86-64 hobby kernel\n");
    vga_write("Boot: GRUB/Multiboot2 | C + ASM | Tested on QEMU\n");
    vga_write("Storage: ");
    vga_write(fs_is_persistent() ? "disk-backed (files persist across reboots)\n"
                                  : "RAM only (no disk found -- files won't survive a reboot)\n");
}

void cmd_echo(const char *args) {
    vga_write(args);
    vga_putc('\n');
}

void cmd_meminfo(void) {
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

// dmesg scratch state -- klog_dump() (klog.h) takes a plain
// void(*)(char) callback with no userdata slot, same pattern fs_list()
// uses (see syscall.c's "SYS_LISTDIR scratch state" comment), so
// pagination state that needs to survive across callback invocations
// lives in file-scope statics here instead of being threaded through
// the callback itself.
static uint32_t dmesg_rows_shown;
static uint32_t dmesg_page_rows;
static int dmesg_quit;

static void dmesg_putc_cb(char c) {
    // klog_dump() is an unconditional walk of the ring buffer with no
    // way to signal "stop" back into it mid-stream (see its own doc
    // comment) -- so quitting early doesn't stop the dump itself, it
    // just makes this callback stop actually drawing anything for the
    // remainder of that walk. Cheap and correct: klog_dump() still
    // touches every remaining byte, this just becomes a no-op for them.
    if (dmesg_quit) return;

    vga_putc(c);
    if (c != '\n') return;
    dmesg_rows_shown++;
    if (dmesg_rows_shown < dmesg_page_rows) return;
    vga_write("-- more (press any key, 'q' to quit) --");
    int key = keyboard_getchar();
    vga_write("\n");
    dmesg_rows_shown = 0;
    if (key == 'q' || key == 'Q') dmesg_quit = 1;
}

void cmd_dmesg(void) {
    dmesg_rows_shown = 0;
    dmesg_quit = 0;
    // A sink means this might be running inside a non-blocking GUI
    // callback (see console_page()'s identical check) -- pagination's
    // keyboard_getchar() would hang whatever's driving that. Dump
    // everything unpaginated in that case; Terminal's own scrollback
    // widget handles "doesn't fit one screen" there, same as it does
    // for every other command's output.
    dmesg_page_rows = vga_sink_active() ? 0xFFFFFFFFu
                                         : (vga_rows() > 1 ? vga_rows() - 1 : vga_rows());
    klog_dump(dmesg_putc_cb);
}

void cmd_reboot(void) {
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

void cmd_apps(void) {
    vga_write("Registered apps:\n");
    app_list(apps_list_cb);
}

void cmd_run(const char *name_and_args) {
    if (!name_and_args || k_strlen(name_and_args) == 0) {
        vga_write("usage: run <app> [args...]  (see 'apps' for the list)\n");
        return;
    }

    // Split off the binary's own name from whatever trailing arguments
    // it should receive -- same first-word/rest split dispatch() itself
    // does in shell.c, done again here because app_run() below still
    // wants just the bare name.
    char name[LINE_MAX];
    k_strcpy(name, name_and_args);
    char *bin_args = name;
    while (*bin_args && *bin_args != ' ') bin_args++;
    if (*bin_args == ' ') {
        *bin_args = '\0';
        bin_args++;
        while (*bin_args == ' ') bin_args++;
    } else {
        bin_args = 0;
    }

    if (app_run(name)) {
        // An app may have drawn over the whole screen (e.g. gui) --
        // refresh the console on return so the shell prompt is clean
        // either way.
        vga_clear();
        vga_set_color(shell_fg, VGA_BLACK);
        return;
    }

    // Not a kernel-space app (apps.c's registry) -- fall through to a
    // real disk-hosted ELF64 binary under /bin (see docs/roadmap.md's
    // real-disk-hosted-ELF-binaries entry, and elf_run.h). "/bin/" + name,
    // bounded the same way SYS_LISTDIR/SYS_OPEN's own path copies are.
    char bin_path[FS_PATH_MAX];
    k_strcpy(bin_path, "/bin/");
    size_t prefix_len = k_strlen(bin_path);
    size_t i = 0;
    while (name[i] && prefix_len + i < FS_PATH_MAX - 1) {
        bin_path[prefix_len + i] = name[i];
        i++;
    }
    bin_path[prefix_len + i] = '\0';

    if (!fs_exists(bin_path) || fs_is_dir(bin_path)) {
        vga_write("run: no such app: ");
        vga_write(name);
        vga_putc('\n');
        return;
    }

    int exit_code = elf_run_from_fs(bin_path, bin_args);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Process finished. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_putc('\n');
    vga_set_color(shell_fg, VGA_BLACK);
}

// The real /bin/ls ELF64 binary's shell-side wrapper (see
// userland/ls.c) -- gets its own dedicated dispatch entry rather than
// going through the generic `run` path, same precedent as cmd_lspci()
// below having its own entry instead of requiring `run lspci`. Splits
// `-a`/`-l`/`-al`/`-la` flags from an optional trailing positional
// directory argument, resolves that argument (or defaults to `cwd`)
// through resolve_path() -- fs.c/fs.h has no cwd concept at all, and
// neither does userland/ls.c, so this is the one place a relative path
// gets turned into an absolute one before crossing into ring 3.
void cmd_ls_bin(const char *args) {
    char flags[8];
    size_t flags_len = 0;
    char positional[FS_PATH_MAX];
    positional[0] = '\0';

    if (args) {
        char scratch[LINE_MAX];
        k_strcpy(scratch, args);
        char *p = scratch;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ') p++;
            int had_space = (*p == ' ');
            *p = '\0';
            if (start[0] == '-') {
                for (size_t j = 1; start[j] && flags_len + 1 < sizeof(flags); j++) {
                    flags[flags_len++] = start[j];
                }
            } else if (positional[0] == '\0') {
                k_strcpy(positional, start);
            }
            if (had_space) p++;
        }
    }
    flags[flags_len] = '\0';

    char path[FS_PATH_MAX];
    if (!resolve_path(positional[0] ? positional : 0, path)) {
        vga_write("ls: path too long\n");
        return;
    }

    char run_args[FS_PATH_MAX + 8];
    run_args[0] = '\0';
    if (flags_len > 0) {
        k_strcpy(run_args, "-");
        k_strcpy(run_args + 1, flags);
        k_strcpy(run_args + 1 + flags_len, " ");
    }
    k_strcpy(run_args + k_strlen(run_args), path);

    int exit_code = elf_run_from_fs("/bin/ls", run_args);
    vga_set_color(shell_fg, VGA_BLACK);
    if (exit_code != 0) {
        vga_write("ls: exited with code ");
        vga_write_exit_code(exit_code);
        vga_putc('\n');
    }
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

// Prints every baked size's name (point size, see font_ttf.h), comma-
// separated -- shared by cmd_fontsize()'s usage line and its "unknown
// size" error, so the list shown to the user can't drift out of sync
// with what font_ttf.c actually has baked in.
static void print_fontsize_choices(void) {
    for (enum font_size i = 0; i < FONT_SIZE_COUNT; i++) {
        if (i > 0) vga_write(", ");
        vga_write(gfx_font_size_name(i));
    }
}

// Changes the console's font size (see gfx_set_font_size() / font_ttf.h
// -- eight point sizes baked at build time by tools/genttf.py, not
// runtime TrueType rendering). Only affects the framebuffer console; the
// legacy 80x25 text-mode fallback has one fixed cell size and can't
// resize. Persists the choice to /etc/fontsize (see font_config.h) so it
// survives a reboot -- same pattern as `timezone` persisting via tz.c.
void cmd_fontsize(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: fontsize <n>  (");
        print_fontsize_choices();
        vga_write(")  (currently: ");
        vga_write(gfx_font_size_name(gfx_font_size()));
        vga_write(")\n");
        return;
    }
    enum font_size want = FONT_SIZE_COUNT;
    for (enum font_size i = 0; i < FONT_SIZE_COUNT; i++) {
        if (k_strcmp(args, gfx_font_size_name(i)) == 0) { want = i; break; }
    }
    if (want == FONT_SIZE_COUNT) {
        vga_write("fontsize: unknown size '");
        vga_write(args);
        vga_write("' -- try ");
        print_fontsize_choices();
        vga_write("\n");
        return;
    }
    gfx_set_font_size(want);
    vga_reflow(); // recompute console_cols/rows for the new cell size and clear
    font_config_save(want); // persist to /etc/fontsize so it survives a reboot
    vga_write("Font size set to ");
    vga_write(gfx_font_size_name(want));
    vga_write(".\n");
}

// Changes the active keyboard scancode layout -- any name with a
// matching /etc/kbs/<name> file (see kernel/core/keyboard_layout.c and
// tools/gen_kbs.py; `us` and `se`/Finnish ship by default). `keyboard`
// alone shows the current layout. Persists via keyboard_config_save()
// so it survives a reboot -- same pattern as `fontsize`/`timezone`.
void cmd_keyboard(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: keyboard <name>  (currently: ");
        vga_write(keyboard_layout_current());
        vga_write(")\n");
        return;
    }
    int found = keyboard_layout_load(args);
    keyboard_config_save(keyboard_layout_current());
    if (!found) {
        vga_write("keyboard: '");
        vga_write(args);
        vga_write("' not found in /etc/kbs -- reverted to ");
        vga_write(keyboard_layout_current());
        vga_write("\n");
        return;
    }
    vga_write("Keyboard layout set to ");
    vga_write(keyboard_layout_current());
    vga_write(".\n");
}

void cmd_color(const char *args) {
    if (!args || k_strlen(args) == 0) {
        vga_write("usage: color <name> (green, lightcyan, white, red)\n");
        return;
    }
    shell_fg = color_from_name(args);
    vga_set_color(shell_fg, VGA_BLACK);
    vga_write("Color set.\n");
}

void cmd_history(void) {
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

// Prints exactly `digits` lowercase hex digits of `v`, no "0x" prefix
// and no digit-trimming -- unlike vga_write_hex() (vga.h), which is
// meant for arbitrary-width values and trims leading zeros. lspci-style
// output wants fixed-width fields (e.g. "8086:1237", not "8086:1237"
// one time and "86:237" the next) so columns actually line up.
static void print_hex_digits(uint32_t v, int digits) {
    char buf[9]; // enough for the widest caller here (4 digits) + '\0'
    for (int i = 0; i < digits; i++) {
        uint8_t nibble = (v >> ((digits - 1 - i) * 4)) & 0xF;
        buf[i] = nibble < 10 ? (char)('0' + nibble) : (char)('a' + nibble - 10);
    }
    buf[digits] = '\0';
    vga_write(buf);
}

// Lists every device pci_init() found at boot (kernel_main() runs it
// once, unconditionally -- see kernel.c) in the traditional
// `bus:device.function  vendor:device  class name` shape, plus IRQ
// line and any nonzero BARs when present. Nothing here re-scans the
// bus -- this only ever shows what was recorded at boot.
void cmd_lspci(void) {
    int count = pci_device_count();
    if (count == 0) {
        vga_write("No PCI devices found.\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;

        print_hex_digits(d->bus, 2);
        vga_putc(':');
        print_hex_digits(d->device, 2);
        vga_putc('.');
        print_hex_digits(d->function, 1);
        vga_write("  ");
        print_hex_digits(d->vendor_id, 4);
        vga_putc(':');
        print_hex_digits(d->device_id, 4);
        vga_write("  ");
        vga_write(pci_class_name(d->class_code, d->subclass));

        if (d->interrupt_line != 0 && d->interrupt_line != 0xFF) {
            vga_write("  irq ");
            vga_write_dec(d->interrupt_line);
        }
        for (int b = 0; b < 6; b++) {
            if (d->bar[b] == 0) continue;
            vga_write("  bar");
            vga_write_dec((uint32_t)b);
            vga_putc('=');
            vga_write_hex(pci_bar_addr(d->bar[b]));
            vga_write(pci_bar_is_io(d->bar[b]) ? "(io)" : "(mem)");
        }
        vga_putc('\n');
    }
}
