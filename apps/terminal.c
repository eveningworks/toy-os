// A GUI terminal-emulator app -- phase 4/4 of the plan started at
// CHANGELOG.md build 183 (see that entry, and builds 193/203, for the
// three prerequisite phases this is built on): a vga_sink (vga.h)
// redirecting console output into a text_scrollback widget (widgets.h),
// driving the real shell_dispatch() (shell.h) from keystrokes instead
// of duplicating shell.c's ~40 command handlers.
//
// NOT supported, on purpose (see BLOCKED_CMDS below): commands that
// never return (`ring3test`), that take over the whole physical screen
// by drawing straight to the framebuffer instead of going through the
// console/sink (`gui`), or that block the calling context for their
// whole run without yielding back to the window manager (`schedtest`).
// Typing one of these prints an explanation instead of running it --
// see build 193's CHANGELOG entry for the original reasoning.
//
// `run`/`ls` used to belong in that same list -- both went through
// elf_run_from_fs()/process_run_ring3_args() at the physical shell, a
// synchronous, blocking ring-3 call that would freeze this window's
// event loop for however long the binary ran, same hazard as `gui`/
// `schedtest` above. Milestone 1 phase 4b (docs/roadmap.md) built the
// non-blocking alternative both of these needed: scheduler.h's
// scheduler_spawn()/scheduler_poll() (a public, non-blocking spawn API
// on top of the M16 scheduler, now continuously armed rather than
// demo-only) plus a new wm_run() poll slot (wm.h's
// window_start_process(), mirroring window_start_write()/
// window_start_read()'s existing pattern exactly). `ls` always uses it
// now; `run` uses it only for names on RUN_ALLOWED_BINS below (see its
// own comment for exactly which binaries were verified safe and why) --
// term_run_line() special-cases both BEFORE BLOCKED_CMDS is ever
// consulted. Everything else about `run` (any name not on the
// allowlist, or a kernel-space app like `gui`/`shell` itself) still
// gets an explanatory refusal; the physical shell's own `run` is
// unaffected and can still run anything. No stdin routing to a running
// process yet -- see term_spawn()'s own comment on why that's still
// out of scope this phase. Ordinary commands like `cat`/`cd` still run
// for real through shell_dispatch(), because SYS_WRITE (the only way a
// ring-3 process prints) already goes through vga_putc() -- and
// vga_putc() already respects whatever sink is active (see vga.h's
// struct vga_sink comment) -- with zero terminal-specific code needed
// for any of them.
//
// `edit`/`nano` (build 377) look like they'd belong in that same
// blocked list -- apps/editor.c's editor_run() is exactly the kind of
// blocking, own-keyboard-loop command Terminal excludes everywhere
// else. They're handled here as a special case instead: entering the
// command switches this window into a small non-blocking "editor
// sub-mode" (st->in_editor) that drives editor_handle_key() one
// keystroke at a time from terminal_key(), same event-driven shape as
// every other GUI app's on_key callback, rendering with
// widget_scrollback_draw() instead of editor_run()'s vga_* console
// redraw. See editor.h's top comment for the full split.
#include "terminal.h"
#include "wm/wm.h"
#include "ui/ui.h"
#include "editor.h"
#include "shell.h"
#include "completion.h"
#include "kapi.h"
#include "theme.h"

// Height of the editor sub-mode's status bar (path + key hints +
// last save status) -- same idea as notepad.c's TOOLBAR_H, a macro
// (not a cached constant) so it tracks gfx_char_h() live across a
// font-size change.
#define EDITOR_STATUS_H (gfx_char_h() + 8)

#define TERM_LINE_MAX 128
#define TERM_HISTORY_MAX 8
// How many text rows/cols the initial window should comfortably fit --
// same idea as notepad.c's NOTEPAD_COLS/ROWS: just a starting size for
// the current font, not a hard limit (the scrollback widget reflows to
// whatever size the window actually is on every draw -- see
// widgets.h's comment on why that's cheap enough to just always do).
#define TERM_COLS 70
#define TERM_ROWS 20
// Width of the scrollbar strip reserved along the content area's right
// edge -- scales with font size like everything else here (see
// TOOLBAR_H in notepad.c for the same pattern). Below TERM_MIN_W_FOR_SCROLLBAR
// (an arbitrarily-picked "would leave basically no room for text" content
// width), the scrollbar is skipped entirely and text uses the full width --
// see term_layout().
#define TERM_SCROLLBAR_W (gfx_char_w() + 4)
#define TERM_MIN_W_FOR_SCROLLBAR (TERM_SCROLLBAR_W * 3)

// Single static instance -- like every other GUI app here, only one
// window of it can be open at a time (see wm.c's open_app).
struct terminal_state {
    struct text_scrollback tb;
    char line[TERM_LINE_MAX];
    int line_len;
    char history[TERM_HISTORY_MAX][TERM_LINE_MAX];
    int history_count;
    int hist_index;       // like shell.c's shell_read_line(): one past the
                           // newest = "current blank/in-progress line"
    char saved_current[TERM_LINE_MAX];
    int scrollbar_grab_offset; // set by terminal_drag_start(), read by terminal_drag() -- see widgets.h's widget_scrollbar_thumb_rect()

    // ---- `edit`/`nano` sub-mode (see this file's top comment) ----
    int in_editor;
    struct text_scrollback editor_tb;
    char editor_path[FS_PATH_MAX];
    char editor_status[32];

    // ---- async `ls`/`run` (Milestone 1 phase 4b, docs/roadmap.md) ----
    int running_pid; // 0 if no process running via wm.h's window_start_process(), else its pid
    const struct vga_sink *saved_sink; // whatever was active before term_spawn() installed term_sink, restored in terminal_process_exit()
};
static struct terminal_state g_terminal;

// `elftest`/`guitest`/`wintest`/`echotest` used to be here too -- they
// don't exist as shell commands anymore (their binaries moved to /bin,
// see docs/decisions.md), so blocking them by name would just be dead
// weight.
//
// `run`/`ls` used to be here too -- both go through
// elf_run_from_fs()/process_run_ring3_args() at the physical shell, a
// synchronous, blocking ring-3 call that would freeze this window's
// whole event loop until it returns. Milestone 1 phase 4b
// (docs/roadmap.md) closed that: term_run_line() below now special-cases
// both BEFORE this list is ever consulted, spawning through
// scheduler_spawn() (non-blocking) instead and polling via
// wm.h's window_start_process(), same shape async I/O
// (fs_write_range_begin()/_step() etc) already uses elsewhere in this
// codebase. `ls` always goes through this path now (see term_run_line());
// `run` only does for names on RUN_ALLOWED_BINS below -- everything else
// still gets an explanatory refusal, same spirit as this list, since a
// per-target allowlist needed each binary actually verified safe (no
// stdin read, no framebuffer/window takeover -- see RUN_ALLOWED_BINS's
// own comment), not assumed so by omission.
static const char *const BLOCKED_CMDS[] = {
    // `strace` runs its target through the physical shell's own
    // blocking elf_run_from_fs() -- deliberately, since the trace has
    // to interleave with the traced process's output in real time --
    // so it freezes this window's event loop exactly the way `run`
    // used to before its async path existed. Same refusal, same reason.
    "gui", "ring3test", "schedtest", "strace",
};
#define BLOCKED_CMD_COUNT (sizeof(BLOCKED_CMDS) / sizeof(BLOCKED_CMDS[0]))

static int is_blocked_command(const char *cmd) {
    for (unsigned i = 0; i < BLOCKED_CMD_COUNT; i++) {
        if (k_strcmp(cmd, BLOCKED_CMDS[i]) == 0) return 1;
    }
    return 0;
}

// Explicit allowlist for `run <name>`'s async spawn path (Milestone 1
// phase 4b) -- the opposite of BLOCKED_CMDS's blocklist approach,
// deliberately: a /bin binary not on this list is refused, not assumed
// safe by omission. Verified against each binary's own userland/*.c
// source, not by name alone: none of these read stdin (Terminal
// doesn't route keystrokes to a running process this phase -- one
// blocked reading stdin would hang forever, and close_window() refuses
// to close a window with a process pending, so a hung one would strand
// the whole window) or touch the framebuffer/their own window directly.
// Deliberately excluded: `echo` (loops on SYS_READ_KEY waiting for Esc,
// which never arrives), `gui_test`/`win_test` (framebuffer/own-window
// takeover, same hazard `gui` above has), `counter_a`/`counter_b`
// (infinite-loop-by-design demo processes for `schedtest`, not real
// commands -- see scheduler.c). `crash_test`/`nx_test` both deliberately
// fault -- verified safe anyway: idt.c's fault handler is already
// scheduler-aware (its `recoverable` branch checks
// scheduler_current_pid()), tearing the process down and reporting
// "RING-3 PROCESS CRASHED" through whatever sink is active (same as any
// other console output -- see vga.h's struct vga_sink) with exit code
// -1, same as a legacy `run crash_test`/`run nx_test` from the physical
// shell. `nx_test` specifically proves NX enforcement (Milestone 2,
// docs/roadmap.md) by jumping into a non-executable data page.
static const char *const RUN_ALLOWED_BINS[] = {
    "crash_test", "exit_test", "file_test", "hello", "lspci",
    "newsyscalls_test", "nx_test", "socket_test", "write_bad_test",
    "write_test",
};
#define RUN_ALLOWED_COUNT (sizeof(RUN_ALLOWED_BINS) / sizeof(RUN_ALLOWED_BINS[0]))

static int is_run_allowed(const char *name) {
    for (unsigned i = 0; i < RUN_ALLOWED_COUNT; i++) {
        if (k_strcmp(name, RUN_ALLOWED_BINS[i]) == 0) return 1;
    }
    return 0;
}

static void term_write(struct terminal_state *st, const char *s, enum vga_color fg) {
    widget_scrollback_set_color(&st->tb, fg);
    for (const char *p = s; *p; p++) widget_scrollback_putc(&st->tb, *p);
}

static void term_print_prompt(struct terminal_state *st) {
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREEN);
    for (const char *p = shell_cwd(); *p; p++) widget_scrollback_putc(&st->tb, *p);
    widget_scrollback_putc(&st->tb, '>');
    widget_scrollback_putc(&st->tb, ' ');
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
}

// ---- vga_sink glue: lets shell_dispatch()'s vga_write()/vga_putc()/
// etc calls land in this window's scrollback widget instead of the
// physical console (see vga.h's struct vga_sink comment for the
// mechanism). `ctx` is always &g_terminal.tb.
static void sink_putc(void *ctx, char c) { widget_scrollback_putc((struct text_scrollback *)ctx, c); }
static void sink_backspace(void *ctx) { widget_scrollback_backspace((struct text_scrollback *)ctx); }
static void sink_clear(void *ctx) { widget_scrollback_clear((struct text_scrollback *)ctx); }
static void sink_set_color(void *ctx, enum vga_color fg, enum vga_color bg) {
    (void)bg; // the scrollback widget tracks one color per cell, no per-cell background yet
    widget_scrollback_set_color((struct text_scrollback *)ctx, fg);
}
static uint32_t sink_rows(void *ctx) {
    // Only consulted by console_page() (shell.c's `help` pager) when no
    // sink is active being false -- but console_page() checks
    // vga_sink_active() FIRST and skips straight to unpaginated output
    // whenever a sink (like this one) is installed (see build 193's
    // CHANGELOG entry), so this never actually gets called in practice.
    // Present anyway so the struct vga_sink literal below doesn't need
    // a special-case comment of its own; returns a plausible constant.
    (void)ctx;
    return 24;
}

static void term_history_add(struct terminal_state *st, const char *line) {
    if (k_strlen(line) == 0) return;
    if (st->history_count < TERM_HISTORY_MAX) {
        k_strcpy(st->history[st->history_count], line);
        st->history_count++;
    } else {
        for (int i = 1; i < TERM_HISTORY_MAX; i++) k_strcpy(st->history[i - 1], st->history[i]);
        k_strcpy(st->history[TERM_HISTORY_MAX - 1], line);
    }
}

// Erases the in-progress input line from the scrollback (one
// widget_scrollback_backspace() per character -- same idea as shell.c's
// redraw_line(), just via the widget instead of vga_backspace()) and
// replaces it with `new_line`, used by the up/down history browsing
// below.
// One Tab press, GUI Terminal flavour. Candidate generation is shared
// with the physical shell (apps/completion.c); what differs is purely
// presentation -- everything here goes through the text_scrollback
// widget instead of vga_putc(), and the prompt is reprinted via
// term_print_prompt() rather than by writing cwd out by hand. Keeping
// that split is the whole reason completion.c has no drawing in it.
static void term_complete_line(struct terminal_state *st) {
    static struct completion_result r;
    st->line[st->line_len] = '\0';
    if (completion_run(st->line, st->line_len, &r) == 0) return;

    for (int i = 0; r.insert[i] && st->line_len < TERM_LINE_MAX - 1; i++) {
        st->line[st->line_len++] = r.insert[i];
        widget_scrollback_putc(&st->tb, r.insert[i]);
    }
    if (r.add_space && st->line_len < TERM_LINE_MAX - 1) {
        st->line[st->line_len++] = ' ';
        widget_scrollback_putc(&st->tb, ' ');
    }
    st->line[st->line_len] = '\0';

    if (r.count <= 1) return;

    widget_scrollback_putc(&st->tb, '\n');
    int col = 0;
    for (int i = 0; i < r.count; i++) {
        for (const char *p = r.candidates[i]; *p; p++) widget_scrollback_putc(&st->tb, *p);
        int clen = (int)k_strlen(r.candidates[i]);
        int pad = clen >= 15 ? 1 : 16 - clen;
        for (int p = 0; p < pad; p++) widget_scrollback_putc(&st->tb, ' ');
        if (++col == 4) { widget_scrollback_putc(&st->tb, '\n'); col = 0; }
    }
    if (col != 0) widget_scrollback_putc(&st->tb, '\n');
    if (r.truncated) {
        for (const char *p = "... (more matches not shown)\n"; *p; p++) widget_scrollback_putc(&st->tb, *p);
    }

    term_print_prompt(st);
    for (int i = 0; i < st->line_len; i++) widget_scrollback_putc(&st->tb, st->line[i]);
}

static void term_set_line(struct terminal_state *st, const char *new_line) {
    for (int i = 0; i < st->line_len; i++) widget_scrollback_backspace(&st->tb);
    widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
    int len = 0;
    while (new_line[len] && len < TERM_LINE_MAX - 1) {
        widget_scrollback_putc(&st->tb, new_line[len]);
        st->line[len] = new_line[len];
        len++;
    }
    st->line_len = len;
}

// Resolves a filename argument the same lightweight way editor_run()'s
// caller (shell.c's cmd_edit()) would want, but this file has no access
// to shell.c's own resolve_path() (static to that file, and it does
// real "."/".." collapsing) -- so this is deliberately simpler: only
// absolute paths and plain relative names against shell_cwd() are
// handled, no ".."/"." segments. Good enough for "edit foo.txt" and
// "edit /notes/todo.txt"; a path with ".." in it just won't resolve the
// way the physical shell's `cd`-aware commands would.
static void resolve_editor_path(const char *name, char *out) {
    if (name[0] == '/') {
        k_strcpy(out, name);
        return;
    }
    const char *cwd = shell_cwd();
    k_strcpy(out, cwd);
    size_t cl = k_strlen(cwd);
    if (cl > 1) { out[cl] = '/'; out[cl + 1] = '\0'; cl++; }
    k_strcpy(out + cl, name);
}

// Shared tail of the `ls`/`run` async-spawn paths in term_run_line()
// below (Milestone 1 phase 4b, docs/roadmap.md): installs term_sink so
// the spawned process's SYS_WRITE output lands in this window's
// scrollback exactly the way shell_dispatch()'s own sink already does
// for ordinary kernel-space commands, spawns via scheduler_spawn()
// (non-blocking -- returns immediately with a pid, unlike
// elf_run_from_fs()), and registers it with wm.h's
// window_start_process() so wm_run() polls it once per frame. Prints
// its own error and leaves nothing pending/no sink change on any
// failure. `args` may be "" for none. No stdin routing to the spawned
// process -- terminal_key() below refuses all input while
// st->running_pid is set, same as it does for st->in_editor, so this is
// only really usable for short output-only commands today (exactly
// `ls` and RUN_ALLOWED_BINS's members -- see their own comments).
static void term_spawn(struct window *win, struct terminal_state *st,
                        const char *bin_path, const char *args) {
    if (!fs_exists(bin_path) || fs_is_dir(bin_path)) {
        term_write(st, "run: no such app: ", VGA_LIGHT_RED);
        term_write(st, bin_path, VGA_LIGHT_RED);
        term_write(st, "\n", VGA_LIGHT_RED);
        return;
    }

    int pid = scheduler_spawn(bin_path, (args && args[0]) ? args : NULL);
    if (pid == 0) {
        term_write(st, "run: failed to spawn ", VGA_LIGHT_RED);
        term_write(st, bin_path, VGA_LIGHT_RED);
        term_write(st, "\n", VGA_LIGHT_RED);
        return;
    }
    if (!window_start_process(win, pid)) {
        // Can't happen in practice -- terminal_key() refuses to call
        // term_run_line() at all while st->running_pid is already set,
        // and Terminal is the only caller of window_start_process() --
        // defensive only, same spirit as notepad.c's own
        // belt-and-suspenders window_write_pending() check.
        term_write(st, "run: a process is already running in this window\n", VGA_LIGHT_RED);
        return;
    }

    static const struct vga_sink term_sink = {
        .ctx = &g_terminal.tb, .putc = sink_putc, .backspace = sink_backspace,
        .clear = sink_clear, .set_color = sink_set_color, .rows = sink_rows,
    };
    st->saved_sink = vga_set_sink(&term_sink);
    st->running_pid = pid;
}

static void term_run_line(struct window *win, struct terminal_state *st, char *line) {
    char cmd[TERM_LINE_MAX];
    int i = 0;
    while (line[i] && line[i] != ' ' && i < (int)sizeof(cmd) - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';

    if (cmd[0] && (k_strcmp(cmd, "edit") == 0 || k_strcmp(cmd, "nano") == 0)) {
        const char *name = line[i] ? line + i + 1 : "";
        if (!name[0]) {
            term_write(st, "usage: edit <file>\n", VGA_LIGHT_RED);
            return;
        }
        resolve_editor_path(name, st->editor_path);
        widget_scrollback_init(&st->editor_tb);
        editor_load(&st->editor_tb, st->editor_path);
        st->editor_status[0] = '\0';
        st->in_editor = 1;
        return;
    }

    // `ls` -- always async now (Milestone 1 phase 4b). Mirrors
    // shell_sys.c's cmd_ls_bin() flag/positional-argument parsing
    // (-a/-l/-al/-la plus an optional trailing directory), but resolves
    // the positional argument via resolve_editor_path() above instead
    // of shell.c's own resolve_path() (static to that file, and does
    // real "."/".." collapsing this simpler version skips) -- same
    // "deliberately simpler" tradeoff resolve_editor_path() already
    // documents for `edit`.
    if (cmd[0] && k_strcmp(cmd, "ls") == 0) {
        const char *rest = line[i] ? line + i + 1 : "";
        char flags[8];
        size_t flags_len = 0;
        char positional[FS_PATH_MAX];
        positional[0] = '\0';

        char scratch[TERM_LINE_MAX];
        k_strcpy(scratch, rest);
        char *p = scratch;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ') p++;
            int had_space = (*p == ' ');
            *p = '\0';
            if (start[0] == '-') {
                for (size_t j = 1; start[j] && flags_len + 1 < sizeof(flags); j++) flags[flags_len++] = start[j];
            } else if (positional[0] == '\0') {
                k_strcpy(positional, start);
            }
            if (had_space) p++;
        }
        flags[flags_len] = '\0';

        char path[FS_PATH_MAX];
        if (positional[0]) resolve_editor_path(positional, path);
        else k_strcpy(path, shell_cwd());

        char args[FS_PATH_MAX + 8];
        args[0] = '\0';
        if (flags_len > 0) {
            k_strcpy(args, "-");
            k_strcpy(args + 1, flags);
            k_strcpy(args + 1 + flags_len, " ");
        }
        k_strcpy(args + k_strlen(args), path);

        term_spawn(win, st, "/bin/ls", args);
        return;
    }

    // `run <name> [args...]` -- async only for RUN_ALLOWED_BINS's
    // verified-safe binaries (see its own comment); everything else
    // (an unverified /bin binary, or a kernel-space app name like `gui`/
    // `shell` from apps.c's registry -- deliberately not even checked
    // against that registry, since nothing in it is on the allowlist
    // either way) gets the same explanatory refusal BLOCKED_CMDS's
    // members do.
    if (cmd[0] && k_strcmp(cmd, "run") == 0) {
        const char *rest = line[i] ? line + i + 1 : "";
        if (!rest[0]) {
            term_write(st, "usage: run <app> [args...]\n", VGA_LIGHT_RED);
            return;
        }
        char name[TERM_LINE_MAX];
        k_strcpy(name, rest);
        char *bin_args = name;
        while (*bin_args && *bin_args != ' ') bin_args++;
        if (*bin_args == ' ') {
            *bin_args = '\0';
            bin_args++;
            while (*bin_args == ' ') bin_args++;
        } else {
            bin_args = 0;
        }

        if (!is_run_allowed(name)) {
            term_write(st,
                "Not available in the terminal app yet -- only a small,\n"
                "verified-safe allowlist of /bin binaries can run from\n"
                "here (none that read stdin or touch the framebuffer\n"
                "directly). Esc out of the GUI and use the physical\n"
                "shell's `run` for anything else.\n",
                VGA_LIGHT_RED);
            return;
        }

        char bin_path[FS_PATH_MAX];
        k_strcpy(bin_path, "/bin/");
        size_t prefix_len = k_strlen(bin_path);
        size_t j = 0;
        while (name[j] && prefix_len + j < FS_PATH_MAX - 1) { bin_path[prefix_len + j] = name[j]; j++; }
        bin_path[prefix_len + j] = '\0';

        term_spawn(win, st, bin_path, bin_args ? bin_args : "");
        return;
    }

    if (cmd[0] && is_blocked_command(cmd)) {
        term_write(st,
            "Not available in the terminal app -- it either doesn't\n"
            "return, draws straight to the physical screen (bypassing\n"
            "this window), or would recursively re-enter a blocking\n"
            "loop from here. Esc out of the GUI and run it from the\n"
            "physical shell instead.\n",
            VGA_LIGHT_RED);
        return;
    }

    static const struct vga_sink term_sink = {
        .ctx = &g_terminal.tb, .putc = sink_putc, .backspace = sink_backspace,
        .clear = sink_clear, .set_color = sink_set_color, .rows = sink_rows,
    };
    shell_dispatch(line, &term_sink);
}

// wm.h's window_start_process() callback (via gui_apps.h's
// on_process_exit) -- fires once wm_run()'s per-frame poll of the
// process term_spawn() started reaches a terminal result. The
// process's own output already streamed into st->tb in real time (see
// term_spawn()'s comment) -- this only needs to restore whatever sink
// was active before (vga.h's struct vga_sink doc comment on why a
// stale sink left installed is dangerous), print the exit code the
// same way the physical shell's own `run` does (vga_write_exit_code(),
// shell_sys.c's cmd_run()), and re-show the prompt terminal_key() held
// back while the process was running.
void terminal_process_exit(struct window *win, int exit_code) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("Process finished. Exit code: ");
    vga_write_exit_code(exit_code);
    vga_putc('\n');
    vga_set_sink(st->saved_sink);
    st->running_pid = 0;

    term_print_prompt(st);
    window_invalidate(win);
}

void terminal_default_size(int *w, int *h) {
    *w = TERM_COLS * gfx_char_w();
    *h = TERM_ROWS * gfx_char_h();
}

// Splits the content area into the text region and (if there's room)
// the scrollbar strip -- shared by terminal_draw(), terminal_key()'s
// Page Up/Down handling, and the drag/click handlers below, so all four
// agree on exactly the same geometry widget_scrollback_draw() actually
// used to render (a mismatch there would mean scrolling by the wrong
// page size, or hit-testing against the wrong column count).
static void term_layout(struct window *win, int *out_text_w, int *out_ch, int *out_show_scrollbar) {
    int cw = window_content_w(win);
    *out_ch = window_content_h(win);
    if (cw > TERM_MIN_W_FOR_SCROLLBAR) {
        *out_show_scrollbar = 1;
        *out_text_w = cw - TERM_SCROLLBAR_W;
    } else {
        *out_show_scrollbar = 0;
        *out_text_w = cw;
    }
}

void terminal_open(struct window *win) {
    widget_scrollback_init(&g_terminal.tb);
    g_terminal.line_len = 0;
    g_terminal.history_count = 0;
    g_terminal.hist_index = 0;
    g_terminal.saved_current[0] = '\0';
    g_terminal.in_editor = 0;
    g_terminal.running_pid = 0;
    g_terminal.saved_sink = 0;
    window_set_state(win, &g_terminal);

    term_write(&g_terminal,
        "toy-os terminal -- runs the real shell; type 'help' to get started\n"
        "(a few commands that don't fit inside a window aren't available\n"
        "here -- see README.md's terminal-emulator section)\n",
        VGA_LIGHT_CYAN);
    term_print_prompt(&g_terminal);
}

// Editor sub-mode's status bar: path + key hints + last save status --
// same idea as notepad.c's draw_toolbar(), just anchored at the bottom
// instead of the top (nano's own status line is conventionally at the
// bottom too).
static void draw_editor_status(struct terminal_state *st, int cx, int cy, int cw) {
    uint32_t bg = gfx_rgb(40, 40, 45);
    uint32_t fg = gfx_rgb(220, 220, 220);
    gfx_fill_rect(cx, cy, cw, EDITOR_STATUS_H, bg);
    // FS_PATH_MAX (path) + "  -- F2 Save  F3 Exit" (22) + "  -- " (5) +
    // editor_status's own cap (32, see editor.c's status buffer) + NUL,
    // rounded up generously.
    char line[FS_PATH_MAX + 96];
    k_strcpy(line, st->editor_path);
    k_strcpy(line + k_strlen(line), "  -- F2 Save  F3 Exit");
    if (st->editor_status[0]) {
        k_strcpy(line + k_strlen(line), "  -- ");
        k_strcpy(line + k_strlen(line), st->editor_status);
    }
    gfx_draw_string(cx + 4, cy + 4, line, fg, bg);
}

void terminal_draw(struct window *win) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);

    if (st->in_editor) {
        int text_h = window_content_h(win) - EDITOR_STATUS_H;
        widget_scrollback_draw(&st->editor_tb, cx, cy, cw, text_h, gfx_rgb(0, 0, 0), THEME_SELECTION_BG, 1);
        draw_editor_status(st, cx, cy + text_h, cw);
        return;
    }

    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);

    widget_scrollback_draw(&st->tb, cx, cy, text_w, ch, gfx_rgb(0, 0, 0), THEME_SELECTION_BG, 1);

    if (show_scrollbar) {
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
        widget_scrollbar_draw(cx + text_w, cy, TERM_SCROLLBAR_W, ch, total_lines, visible_rows,
                               st->tb.scroll_offset, gfx_rgb(15, 15, 15), gfx_rgb(90, 90, 90));
    }
}

// Scrollbar track clicks that aren't on the thumb (paging up/down) --
// thumb clicks never reach here, they're claimed by terminal_drag_start()
// instead (see gui_apps.h's on_click/on_drag_start contract).
void terminal_click(struct window *win, int cx, int cy) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    if (st->in_editor) return; // no scrollbar in editor sub-mode this pass -- see this file's top comment
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    if (!show_scrollbar) return;

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, TERM_SCROLLBAR_W, ch,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, cy);
    int page = visible_rows > 1 ? visible_rows - 1 : 1;
    if (zone == SCROLLBAR_ZONE_ABOVE) {
        widget_scrollback_scroll(&st->tb, page);
    } else if (zone == SCROLLBAR_ZONE_BELOW) {
        widget_scrollback_scroll(&st->tb, -page);
    } else {
        return; // click landed in the text area (or the bar isn't shown) -- nothing to do
    }
    window_invalidate(win);
}

int terminal_drag_start(struct window *win, int cx, int cy) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    if (st->in_editor) return 0;
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    if (!show_scrollbar) return 0;

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    enum scrollbar_zone zone = widget_scrollbar_hit(text_w, 0, TERM_SCROLLBAR_W, ch,
                                                      total_lines, visible_rows, st->tb.scroll_offset, cx, cy);
    if (zone != SCROLLBAR_ZONE_THUMB) return 0;

    int thumb_y, thumb_h;
    widget_scrollbar_thumb_rect(0, ch, total_lines, visible_rows, st->tb.scroll_offset, &thumb_y, &thumb_h);
    st->scrollbar_grab_offset = cy - thumb_y;
    return 1;
}

void terminal_drag(struct window *win, int cx, int cy) {
    (void)cx; // this is a purely vertical scrollbar -- only cy matters
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    int text_w, ch, show_scrollbar;
    term_layout(win, &text_w, &ch, &show_scrollbar);
    (void)show_scrollbar; // a drag only ever starts while true; harmless either way if the window shrank mid-drag

    int total_lines, visible_rows;
    widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
    st->tb.scroll_offset = widget_scrollbar_offset_for_drag(0, ch, total_lines, visible_rows,
                                                              cy, st->scrollbar_grab_offset);
    window_invalidate(win);
}

// 3 lines per notch -- an ordinary desktop-scrolling convention (fast
// enough that scrolling any real distance doesn't take forever, without
// blowing past a screenful in one notch on a short window).
#define TERM_WHEEL_LINES 3

void terminal_wheel(struct window *win, int delta) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);
    if (st->in_editor) return; // no scrolling in editor sub-mode this pass
    widget_scrollback_scroll(&st->tb, delta * TERM_WHEEL_LINES);
    window_invalidate(win);
}

void terminal_key(struct window *win, int key) {
    struct terminal_state *st = (struct terminal_state *)window_get_state(win);

    // No stdin routing to a running process yet (Milestone 1 phase 4b,
    // see term_spawn()'s comment) -- ignore all input, the same
    // "windows still redraw/other windows still work, this one just
    // doesn't respond to typing" tradeoff notepad.c's disabled Save
    // As.../Open... buttons make while a write/read is in flight.
    // terminal_process_exit() re-enables this once the process exits.
    if (st->running_pid) return;

    if (st->in_editor) {
        int should_exit = 0;
        st->editor_status[0] = '\0'; // see editor_handle_key()'s header comment on this clear-before-call convention
        editor_handle_key(&st->editor_tb, st->editor_path, key, &should_exit, st->editor_status, sizeof(st->editor_status));
        if (should_exit) {
            st->in_editor = 0;
            term_print_prompt(st);
        }
        window_invalidate(win);
        return;
    }

    if (key == '\r' || key == '\n') {
        widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
        widget_scrollback_putc(&st->tb, '\n');
        st->line[st->line_len] = '\0';
        term_history_add(st, st->line);
        term_run_line(win, st, st->line);
        st->line_len = 0;
        st->hist_index = st->history_count;
        // term_run_line() may have just switched this window into
        // editor sub-mode (`edit`/`nano`) -- if so, don't print another
        // shell prompt into st->tb on top of it; terminal_key()'s own
        // in_editor branch above prints one when the user exits back.
        // Same idea for an async ls/run just started (st->running_pid
        // now set) -- terminal_process_exit() prints the next prompt
        // once it actually finishes, not here.
        if (!st->in_editor && !st->running_pid) term_print_prompt(st);
    } else if (key == '\t') {
        term_complete_line(st);
    } else if (key == '\b') {
        if (st->line_len > 0) {
            st->line_len--;
            widget_scrollback_backspace(&st->tb);
        }
    } else if (key == KEY_ARROW_UP) {
        if (st->hist_index > 0) {
            if (st->hist_index == st->history_count) {
                st->line[st->line_len] = '\0';
                k_strcpy(st->saved_current, st->line);
            }
            st->hist_index--;
            term_set_line(st, st->history[st->hist_index]);
        }
    } else if (key == KEY_ARROW_DOWN) {
        if (st->hist_index < st->history_count) {
            st->hist_index++;
            const char *replacement = (st->hist_index == st->history_count)
                                           ? st->saved_current
                                           : st->history[st->hist_index];
            term_set_line(st, replacement);
        }
    } else if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        // Page size is "one screenful minus a line of overlap" -- a
        // common terminal-scrolling convention, and it also means
        // paging down repeatedly can't skip past the bottom in one
        // jump (widget_scrollback_scroll()'s clamp handles the exact
        // boundary either way, this just picks a sensible step size).
        int text_w, ch, show_scrollbar;
        term_layout(win, &text_w, &ch, &show_scrollbar);
        int total_lines, visible_rows;
        widget_scrollback_metrics(&st->tb, text_w, ch, &total_lines, &visible_rows);
        int page = visible_rows > 1 ? visible_rows - 1 : 1;
        widget_scrollback_scroll(&st->tb, key == KEY_PAGE_UP ? page : -page);
    } else if (IS_PRINTABLE_KEY(key) && st->line_len < TERM_LINE_MAX - 1) {
        widget_scrollback_set_color(&st->tb, VGA_LIGHT_GREY);
        widget_scrollback_putc(&st->tb, (char)key);
        st->line[st->line_len++] = (char)key;
    } else {
        return; // unhandled key -- nothing changed, no need to invalidate
    }

    window_invalidate(win);
}
