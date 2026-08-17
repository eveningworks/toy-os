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
#include "completion.h"

// Ctrl-<letter> arrives as that letter's control code -- see keyboard.h
// on the encoding. Only reverse_search() below has to name one
// directly; every other binding goes through klineedit.c's keymap.
#define CTRL_KEY(c) ((c) - 'a' + 1)

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
// to `cwd`) into a normalized absolute path in `out` (size
// FS_PATH_MAX), collapsing "." and ".." along the way. A blank/NULL
// `input` resolves to `cwd` itself. Returns 1 on success, 0 if the
// result would be too deep or too long.
//
// The segment-stack implementation this used to carry is kpath.c's
// k_path_resolve() now. That move wasn't about size -- it's that this
// function was `static` to this file, so apps/terminal.c couldn't
// reach it and grew a "deliberately simpler" copy that didn't handle
// ".." at all. Same command, two answers, depending on which window
// you typed it in. One implementation now, with tests.
//
// It stays shell-side in the sense that matters: `cwd` is the shell's
// state, and fs.c still only ever sees already-normalized absolute
// paths (see its own top comment). kpath just does the string work.
int resolve_path(const char *input, char *out) {
    return k_path_resolve(cwd, input, out, FS_PATH_MAX);
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
        // Trim TRAILING spaces too. Most commands here treat `args` as
        // a single value (a path, a colour name, a number) rather than
        // splitting it further, so "cat /etc/timezones " would look up a
        // filename with a space on the end and fail with "no such file".
        // That was always true for a hand-typed trailing space; tab
        // completion made it easy to hit, since completing a unique
        // match appends one.
        char *end = args;
        while (*end) end++;
        while (end > args && end[-1] == ' ') { end--; *end = '\0'; }
        if (*args == '\0') args = 0;
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
    } else if (k_strcmp(cmd, "random") == 0) {
        cmd_random(args ? args : "");
    } else if (k_strcmp(cmd, "about") == 0) {
        cmd_about();
    } else if (k_strcmp(cmd, "beep") == 0) {
        cmd_beep();
    } else if (k_strcmp(cmd, "echo") == 0) {
        cmd_echo(args ? args : "");
    } else if (k_strcmp(cmd, "meminfo") == 0) {
        cmd_meminfo();
    } else if (k_strcmp(cmd, "heap") == 0) {
        cmd_heap(args ? args : "");
    } else if (k_strcmp(cmd, "gfxbench") == 0) {
        cmd_gfxbench(args ? args : "");
    } else if (k_strcmp(cmd, "df") == 0) {
        cmd_df();
    } else if (k_strcmp(cmd, "stress") == 0) {
        cmd_stress(args ? args : "");
    } else if (k_strcmp(cmd, "dmatest") == 0) {
        cmd_dmatest(args ? args : "");
    } else if (k_strcmp(cmd, "steptest") == 0) {
        cmd_steptest(args ? args : "");
    } else if (k_strcmp(cmd, "sync") == 0) {
        cmd_sync(args ? args : "");
    } else if (k_strcmp(cmd, "fsck") == 0) {
        cmd_fsck(args ? args : "");
    } else if (k_strcmp(cmd, "fsformat") == 0) {
        cmd_fsformat(args ? args : "");
    } else if (k_strcmp(cmd, "ln") == 0) {
        cmd_ln(args ? args : "");
    } else if (k_strcmp(cmd, "mv") == 0) {
        cmd_mv(args ? args : "");
    } else if (k_strcmp(cmd, "truncate") == 0) {
        cmd_truncate(args ? args : "");
    } else if (k_strcmp(cmd, "ktest") == 0) {
        cmd_ktest(args ? args : "");
    } else if (k_strcmp(cmd, "dmesg") == 0) {
        cmd_dmesg();
    } else if (k_strcmp(cmd, "debug") == 0) {
        cmd_debug(args ? args : "");
    } else if (k_strcmp(cmd, "ata") == 0) {
        cmd_ata(args ? args : "");
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
    } else if (k_strcmp(cmd, "path") == 0) {
        cmd_path();
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
    } else if (k_strcmp(cmd, "gui3") == 0) {
        app_run("gui3"); // the ring-3 desktop -- see apps/gui3.c
        vga_clear();
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_write("Back from GUI mode.\n");
        vga_set_color(shell_fg, VGA_BLACK);
    } else if (k_strcmp(cmd, "apps") == 0) {
        cmd_apps();
    } else if (k_strcmp(cmd, "run") == 0) {
        cmd_run(args ? args : "");
    } else if (k_strcmp(cmd, "strace") == 0) {
        cmd_strace(args ? args : "");
    } else if (k_strcmp(cmd, "ring3test") == 0) {
        vga_write("Running ring-3 isolation test. This does NOT return --\n");
        vga_write("see the diagnostic output for what it proves.\n\n");
        ring3_test_run();
    } else if (k_strcmp(cmd, "schedtest") == 0) {
        scheduler_demo_run();
    } else if (k_strcmp(cmd, "fputest") == 0) {
        cmd_fputest();
    } else if (k_strcmp(cmd, "cursor") == 0) {
        cmd_cursor(args ? args : "");
    } else if (k_strcmp(cmd, "fontsize") == 0) {
        cmd_fontsize(args ? args : "");
    } else if (k_strcmp(cmd, "keyboard") == 0) {
        cmd_keyboard(args ? args : "");
    } else if (k_strcmp(cmd, "history") == 0) {
        cmd_history();
    } else if (k_strcmp(cmd, "lspci") == 0) {
        cmd_lspci();
    } else if (k_strcmp(cmd, "parttable") == 0) {
        cmd_parttable();
    } else if (shell_exec_name(cmd, args)) {
        // Not a builtin -- a console app from apps.c's registry, or an
        // executable found by searching PATH (shell_path.c). This is
        // what makes `nx_test` work with no `run` prefix; `run` itself
        // is still a command, and goes through the same resolver so the
        // two can't diverge. Builtins are checked first (every branch
        // above this one) -- see docs/decisions.md for why.
    } else if (completion_is_known_command(cmd)) {
        // Listed in apps/completion.c's table but not handled above --
        // the two lists have drifted. Say so specifically rather than
        // claiming the command doesn't exist, since tab-completion just
        // offered it. See completion.h on why the table is separate.
        vga_write("Internal error: '");
        vga_write(cmd);
        vga_write("' is tab-completable but has no dispatch case.\n");
    } else {
        vga_write("Unknown command: ");
        vga_write(cmd);
        vga_write("\n(type 'help' for commands, or `path` for where\n");
        vga_write("executables are searched for)\n");
    }
}

static void shell_session_init(void);

// See shell.h's doc comment -- the public entry point that lets a GUI
// terminal-emulator app run a command line through the real dispatcher.
void shell_dispatch(char *line, const struct vga_sink *sink) {
    shell_session_init();
    const struct vga_sink *prev = vga_set_sink(sink);
    dispatch(line);
    vga_set_sink(prev);
}

const char *shell_cwd(void) {
    return cwd;
}

// Public wrapper around resolve_path() (shell_internal.h) for callers
// outside the shell's own three files -- apps/completion.c needs to turn
// a partially-typed path into something fs_list() will accept, and that
// resolution is genuinely shell state (it's relative to `cwd`), not
// something fs.h should grow a notion of.
int shell_resolve_path(const char *input, char *out) {
    return resolve_path(input, out);
}

// Persists across reboot the same way timezone/fontsize/keyboard do --
// a small file under /etc. Not folded into etc_config.h's shared
// toyos.conf though: that's a key=value store with one line per
// setting, and history entries are arbitrary shell input that can
// itself contain '=' (e.g. `write f.txt a=b`) -- a bare one-command-
// per-line file (like tz.c's original /etc/timezone, before it moved
// to key=value) is the only shape that doesn't need to escape the
// content it's storing. Whole-file rewrite on every history_add() call
// rather than an incremental append: HISTORY_MAX is only 8 entries, so
// the full array is always small (well under 1KB), and this avoids
// needing separate "append one line" vs. "drop the oldest line from an
// existing file" logic -- fs_write() already provides the same
// overwrite-the-whole-file primitive every other /etc setting uses.
#define HISTORY_FILE "/etc/history"

static void history_save(void) {
    char buf[HISTORY_MAX * LINE_MAX];
    size_t pos = 0;
    for (int i = 0; i < history_count; i++) {
        size_t len = k_strlen(history[i]);
        if (pos + len + 1 >= sizeof(buf)) break; // shouldn't happen at HISTORY_MAX=8, but don't overrun if it ever grows
        k_memcpy(buf + pos, history[i], len);
        pos += len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    fs_write(HISTORY_FILE, buf, 0); // overwrite, not append
}

// Called once from shell_main() before the REPL loop starts. Same
// line-splitting shape tz.c's tz_load_cities() uses for /etc/timezones
// -- walk the buffer, one line per iteration, no shared helper (every
// /etc reader in this kernel rolls its own small split, see that
// file's precedent).
static void history_load(void) {
    uint32_t size = 0;
    const char *data = fs_read(HISTORY_FILE, &size);
    if (!data) return; // no history file yet -- fresh boot or RAM-only mode

    uint32_t pos = 0;
    while (pos < size && history_count < HISTORY_MAX) {
        const char *ls = data + pos;
        const char *end = data + size;
        const char *le = ls;
        while (le < end && *le != '\n') le++;
        pos = (uint32_t)((le < end ? le + 1 : le) - data);

        size_t len = (size_t)(le - ls);
        if (len == 0 || len >= LINE_MAX) continue; // blank line, or too long to have been written by us
        k_memcpy(history[history_count], ls, len);
        history[history_count][len] = '\0';
        history_count++;
    }
}

// Everything that belongs to "having a shell session" rather than to
// "running the interactive REPL". Idempotent and called from BOTH
// shell_main() and shell_dispatch(), because the REPL is not the only
// way into the dispatcher: apps/demo.c's `sh` verb and the serial debug
// console's `sh` both reach it without shell_main() ever running.
//
// The trap this closes: with the demo ISO, shell_main() is never
// reached at all, so PATH was left empty and every command resolved
// through it reported "Unknown command" while builtins beside it worked
// perfectly. Same shape as vfs.c's ensure_layout() -- an init step that
// belongs to a THING must not live only in one of the paths that
// creates it. See docs/decisions.md.
static void shell_session_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    history_load();
    shell_path_init(); // read PATH from /etc/toyos.conf once, see shell_path.c
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
    history_save();
}

// ---- the input line ----
//
// WHAT the line looks like after a keystroke lives in
// kernel/lib/klineedit.c, shared with the GUI Terminal so the two can't
// drift (see klineedit.h -- the three path resolvers that disagreed are
// the cautionary tale). Everything here is this front end's own job:
// painting the line on the physical console, plus the three things the
// core deliberately doesn't own -- history, completion, and the screen.
//
// `g_ed` is file-scope rather than a local because struct kline_edit
// carries an undo stack and runs ~1.2KB, more than belongs on the
// shell's modest stack. shell_read_line() isn't reentrant anyway --
// there is exactly one physical console.
static struct kline_edit g_ed;

// What is currently PAINTED after the prompt, which is not the same as
// what's in the editor until repaint_line() runs. Both are needed to
// erase correctly: the console cursor may be sitting mid-line, and
// vga_backspace() erases relative to wherever it actually is.
static int shown_len = 0;
static int shown_cursor = 0;

static void print_prompt(void) {
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write(cwd);
    vga_write("> ");
    vga_set_color(shell_fg, VGA_BLACK);
}

// Repaints the input line, leaving the console cursor on the editor's
// cursor cell.
//
// Deliberately a whole-line repaint rather than a minimal diff: the
// line is at most 128 characters, the framebuffer console draws that
// well inside a frame, and every clever partial-update scheme has to
// know about wrapping across rows -- which vga_backspace() and
// vga_cursor_move() already handle, correctly and identically, so
// leaning on them is both shorter and less likely to be wrong. Typing
// at the end of the line skips this entirely (see the fast path in
// shell_read_line()).
static void repaint_line(void) {
    // Erase from wherever the cursor actually is: forward to the end of
    // the painted text first, then backspace over all of it.
    if (shown_len > shown_cursor) vga_cursor_move(shown_len - shown_cursor);
    for (int i = 0; i < shown_len; i++) vga_backspace();

    vga_write(g_ed.buf);
    if (g_ed.len > g_ed.cursor) vga_cursor_move(-(g_ed.len - g_ed.cursor));

    shown_len = g_ed.len;
    shown_cursor = g_ed.cursor;
}

// Reprints prompt + line from scratch, for the cases that have just
// written something else to the console (Ctrl-L, an ambiguous
// completion's candidate list, leaving reverse search).
static void reprint_prompt_and_line(void) {
    print_prompt();
    shown_len = 0;
    shown_cursor = 0;
    repaint_line();
}

// Moves the cursor to the end of the painted line, for the cases about
// to print something on a new line.
//
// Repaints rather than just moving the cursor. That was originally
// load-bearing: the cursor erased its cell to black when it moved
// away, so parking from mid-line left a hole where the character under
// it had been (`cat /etc/toyos.conf` ran correctly but echoed back as
// `cat /etc/toyos conf` -- the '.' had been under the cursor). The
// cursor saves and restores its pixels now, so a plain move would be
// correct too; the repaint stays because it also normalises
// shown_cursor/shown_len against the editor in one place, and a
// once-per-command repaint of a 128-character line costs nothing.
static void park_at_end(void) {
    if (shown_cursor == shown_len) return;
    g_ed.cursor = g_ed.len;
    repaint_line();
}

// Applies one Tab press. Candidate generation lives in
// apps/completion.c (shared with the GUI Terminal); everything here is
// this shell's own idea of how to show the result -- insert the agreed
// text inline, and on a genuine ambiguity print the candidates in
// columns and redraw the prompt underneath, the way zsh does.
//
// completion_run() already took a cursor position, so completing the
// word UNDER the cursor rather than at the end of the line needed no
// change -- it simply never got a cursor that wasn't at the end before.
static void shell_complete_line(void) {
    static struct completion_result r; // ~3KB -- static, not stack: this runs on the shell's own modest stack
    if (completion_run(g_ed.buf, g_ed.cursor, &r) == 0) return;

    if (r.insert[0]) kline_insert_str(&g_ed, r.insert);
    if (r.add_space) kline_insert_str(&g_ed, " ");

    if (r.count <= 1) { // a single candidate needs no list
        repaint_line();
        return;
    }

    // Ambiguous: list what's available, then reprint the prompt and the
    // line so the user is back where they were with more information.
    park_at_end();
    vga_putc('\n');
    int col = 0;
    for (int i = 0; i < r.count; i++) {
        vga_write(r.candidates[i]);
        unsigned int clen = (unsigned int)k_strlen(r.candidates[i]);
        // Pad to a 16-column grid, wrapping at 4 columns -- wide enough
        // for most command and file names without assuming a console
        // width this code can't actually query.
        unsigned int pad = clen >= 15 ? 1 : 16 - clen;
        for (unsigned int p = 0; p < pad; p++) vga_putc(' ');
        if (++col == 4) { vga_putc('\n'); col = 0; }
    }
    if (col != 0) vga_putc('\n');
    if (r.truncated) vga_write("... (more matches not shown)\n");

    reprint_prompt_and_line();
}

// Ctrl-R: incremental reverse history search, with bash's own prompt
// shape. Runs its own key loop rather than becoming another mode inside
// the editor core, because it genuinely IS a different editor -- the
// keys build a search pattern, not the command line.
//
// Leaves the matched line (or the original, on cancel) in g_ed.
// Returns 1 if the user pressed Enter, which in bash runs the match
// immediately rather than just recalling it.
static int reverse_search(void) {
    char pattern[LINE_MAX];
    int plen = 0;
    pattern[0] = '\0';

    char original[LINE_MAX];
    k_strlcpy(original, g_ed.buf, sizeof(original));

    int match = -1;               // index into history[], or -1 for none
    int from = history_count - 1; // where the next search starts

    for (;;) {
        // Redraw the whole search line each time: both the pattern and
        // the match change length unpredictably, so there's nothing
        // worth updating incrementally. '\r' + spaces + '\r' clears the
        // row without needing a cursor-addressing primitive.
        vga_putc('\r');
        for (uint32_t i = 0; i + 1 < vga_cols(); i++) vga_putc(' ');
        vga_putc('\r');
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_write("(reverse-i-search)`");
        vga_write(pattern);
        vga_write("': ");
        vga_set_color(shell_fg, VGA_BLACK);
        if (match >= 0) vga_write(history[match]);

        int key = keyboard_getchar();

        if (key == '\r' || key == '\n') {
            if (match >= 0) kline_set(&g_ed, history[match]);
            vga_putc('\n');
            return 1; // bash runs it straight away
        }
        if (key == 0x1B) { // Esc -- keep the match, but edit it instead of running
            if (match >= 0) kline_set(&g_ed, history[match]);
            vga_putc('\n');
            return 0;
        }
        if (key == CTRL_KEY('c') || key == CTRL_KEY('g')) { // abandon the search
            kline_set(&g_ed, original);
            vga_putc('\n');
            return 0;
        }

        if (key == CTRL_KEY('r')) {
            from = (match >= 0) ? match - 1 : history_count - 1; // next older match
        } else if (key == '\b' || key == 0x7F) {
            if (plen > 0) pattern[--plen] = '\0';
            from = history_count - 1; // a shorter pattern can match later entries again
        } else if (IS_PRINTABLE_KEY(key) && plen < LINE_MAX - 1) {
            pattern[plen++] = (char)key;
            pattern[plen] = '\0';
            from = history_count - 1;
        } else {
            continue; // anything else doesn't affect the search
        }

        match = -1;
        for (int i = from; i >= 0; i--) {
            if (k_strstr(history[i], pattern)) { match = i; break; }
        }
    }
}

// Alt-.: insert the last word of the previous command, like bash's
// yank-last-arg. Repeating it doesn't walk further back through history
// here -- one level is what the shortcut actually gets used for (re-use
// the path you just typed).
static void insert_last_arg(void) {
    if (history_count == 0) return;
    const char *prev = history[history_count - 1];
    const char *last = k_strrchr(prev, ' ');
    kline_insert_str(&g_ed, last ? last + 1 : prev);
}

static void shell_read_line(char *buf, unsigned int len) {
    kline_init(&g_ed);
    shown_len = 0;
    shown_cursor = 0;

    int hist_index = history_count; // one past the newest = "current blank line"
    char saved_current[LINE_MAX];
    saved_current[0] = '\0';

    for (;;) {
        int c = keyboard_getchar();

        // Fast path for the overwhelmingly common case: a printable
        // character typed at the end of the line. Skips repaint_line()
        // entirely and just echoes it, so ordinary typing costs exactly
        // what it always did before any of this existed.
        if (IS_PRINTABLE_KEY(c) && g_ed.cursor == g_ed.len &&
            shown_cursor == shown_len && g_ed.len < KLINE_MAX - 1) {
            kline_key(&g_ed, c);
            vga_putc((char)c);
            shown_len = g_ed.len;
            shown_cursor = g_ed.cursor;
            continue;
        }

        switch (kline_key(&g_ed, c)) {
        case KLINE_REDRAW:
            repaint_line();
            break;

        case KLINE_ACCEPT:
            park_at_end();
            vga_putc('\n');
            k_strlcpy(buf, g_ed.buf, len);
            return;

        case KLINE_CANCEL: // Ctrl-C -- abandon this line, fresh prompt
            park_at_end();
            vga_write("^C\n");
            kline_init(&g_ed);
            hist_index = history_count;
            print_prompt();
            shown_len = 0;
            shown_cursor = 0;
            break;

        case KLINE_EOF:
            // Ctrl-D on an empty line ends input in bash. There is
            // nothing to exit TO here -- this shell is the top of the
            // stack, not a process with a parent -- so it's ignored
            // rather than pretending to be an exit.
            break;

        case KLINE_COMPLETE:
            shell_complete_line();
            break;

        case KLINE_CLEAR_SCREEN:
            vga_clear();
            reprint_prompt_and_line();
            break;

        case KLINE_HISTORY_PREV:
            if (hist_index > 0) {
                if (hist_index == history_count) {
                    k_strlcpy(saved_current, g_ed.buf, sizeof(saved_current));
                }
                hist_index--;
                kline_set(&g_ed, history[hist_index]);
                repaint_line();
            }
            break;

        case KLINE_HISTORY_NEXT:
            if (hist_index < history_count) {
                hist_index++;
                kline_set(&g_ed, (hist_index == history_count) ? saved_current
                                                                : history[hist_index]);
                repaint_line();
            }
            break;

        case KLINE_SEARCH: {
            park_at_end();
            vga_putc('\n');
            int run_it = reverse_search();
            if (run_it) {
                k_strlcpy(buf, g_ed.buf, len);
                return;
            }
            hist_index = history_count;
            reprint_prompt_and_line();
            break;
        }

        case KLINE_LAST_ARG:
            insert_last_arg();
            repaint_line();
            break;

        case KLINE_IGNORED:
            break;
        }
    }
}

void shell_main(void) {
    char line[LINE_MAX];

    shell_session_init(); // once per boot; the demo and the serial console reach it first, see its comment

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("tosh -- the toy-os shell. Type 'help' to get started.\n");
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
