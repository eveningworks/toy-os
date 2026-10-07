// The shell: REPL loop (shell_main()/shell_read_line()), the single
// command dispatcher (dispatch()/shell_dispatch()), and the state every
// command shares (cwd, shell_fg, history). The commands themselves live
// in apps/shell_fs.c (filesystem), apps/shell_sys.c (system-info/
// settings) and apps/shell_rescue.c (`rescue`) -- split out once this
// file crossed 900 lines mixing every command category together (see
// the git history for the build this happened in). See
// shell_internal.h's top comment for why this is one component split
// across files sharing state via `extern`s, not independent
// components.
//
// MOST EVERYDAY COMMANDS ARE NOT HERE AT ALL. `cat`, `rm`, `touch`,
// `mkdir`, `mv`, `ln`, `stat`, `truncate`, `sync`, `echo`, `uptime`,
// `df` and `meminfo` are /bin programs, reached through shell_path.c's
// resolver like any other executable -- there is no branch for them
// below, and adding one would put a second implementation of `rm` in
// front of the real one. shell_rescue.c holds the kernel's own copies
// of the FILE commands, behind a name that cannot shadow them.
//
// `df` and `meminfo` were the last two holdouts, and the reason they
// held out is the useful part: they were not waiting on a program, they
// were waiting on a QUESTION RING 3 COULD NOT ASK -- which filesystem
// is mounted, what the firmware memory map says, what a page-table
// audit found. The fix for that is a query provider, never a builtin
// and never a syscall of its own. See docs/query-design.md's stage 2
// and docs/conventions/shell.md.
#include "shell.h"
#include "shell_internal.h"
#include "apps.h"
#include "shell_complete.h"
#include "histsearch.h" // the Ctrl-R loop, shared with /bin/tosh
#include "kpath.h"      // k_path_resolve() and its caller-supplied scratch

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
char *history[HISTORY_MAX];
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
    // Static: k_path_resolve() takes its scratch from the caller now
    // (kpath.h), and at FS_PATH_MAX = 4096 the join buffer is 8 KiB --
    // half a kernel stack. The kernel shell is one context and this
    // does not recurse, the same reason tfs3.c's path buffers are
    // static.
    static char scratch[KPATH_SCRATCH_FOR(FS_PATH_MAX)];
    struct kpath_scratch sc = { scratch, sizeof scratch };
    return k_path_resolve(cwd, input, out, FS_PATH_MAX, &sc);
}

// ---- the builtins ----------------------------------------------------
//
// THE NAMES THIS SHELL HANDLES ITSELF, in one table that dispatch(),
// tab completion and the drift KTEST all read -- bash's `struct builtin`
// and busybox's applet table. A command that is a /bin program is NOT
// here: PATH finds it, and a builtin always wins over PATH, so a row
// here hides a program of the same name.

static void b_clear(const char *a)     { (void)a; vga_clear(); }
static void b_beep(const char *a)      { (void)a; cmd_beep(); }
static void b_pwd(const char *a)       { (void)a; cmd_pwd(); }
static void b_path(const char *a)      { (void)a; cmd_path(); }
static void b_apps(const char *a)      { (void)a; cmd_apps(); }
static void b_fputest(const char *a)   { (void)a; cmd_fputest(); }
static void b_history(const char *a)   { (void)a; cmd_history(); }
static void b_schedtest(const char *a) { (void)a; scheduler_demo_run(); }
static void b_write(const char *a)     { cmd_write_or_append(a, 0); }
static void b_append(const char *a)    { cmd_write_or_append(a, 1); }
static void b_gui(const char *a)       { (void)a; app_run("gui"); }

static void b_gui3(const char *a) {
    (void)a;
    app_run("gui3");
    vga_clear();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("Back from GUI mode.\n");
    vga_set_color(shell_fg, VGA_BLACK);
}

static void b_ring3test(const char *a) {
    (void)a;
    vga_write("Running ring-3 isolation test. This does NOT return --\n");
    vga_write("see the diagnostic output for what it proves.\n\n");
    ring3_test_run();
}

const struct shell_builtin SHELL_BUILTINS[] = {
    { "help",      cmd_help },
    { "clear",     b_clear },
    { "beep",      b_beep },
    { "hwcursor",  cmd_hwcursor },
    { "gfxbench",  cmd_gfxbench },
    { "stress",    cmd_stress },
    { "dmatest",   cmd_dmatest },
    { "steptest",  cmd_steptest },
    { "fsformat",  cmd_fsformat },
    { "ktest",     cmd_ktest },
    { "debug",     cmd_debug },
    { "color",     cmd_color },
    { "cd",        cmd_cd },
    { "pwd",       b_pwd },
    { "path",      b_path },
    // Not a name any ordinary command has -- shell_rescue.c says why the
    // kernel's file-command copies live behind one name that can never
    // shadow /bin.
    { "rescue",    cmd_rescue },
    { "write",     b_write },
    { "append",    b_append },
    { "gui",       b_gui },
    { "gui3",      b_gui3 },
    { "apps",      b_apps },
    { "run",       cmd_run },
    { "ring3test", b_ring3test },
    { "schedtest", b_schedtest },
    { "fputest",   b_fputest },
    { "cursor",    cmd_cursor },
    { "fontsize",  cmd_fontsize },
    { "fontface",  cmd_fontface },
    { "keyboard",  cmd_keyboard },
    { "history",   b_history },
};
const int SHELL_BUILTIN_COUNT = (int)(sizeof SHELL_BUILTINS / sizeof SHELL_BUILTINS[0]);

const struct shell_builtin *shell_builtin_find(const char *name) {
    for (int i = 0; i < SHELL_BUILTIN_COUNT; i++)
        if (k_strcmp(SHELL_BUILTINS[i].name, name) == 0) return &SHELL_BUILTINS[i];
    return 0;
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

    if (k_strlen(cmd) == 0) return;
    const struct shell_builtin *b = shell_builtin_find(cmd);
    if (b) {
        b->fn(args ? args : "");
    } else if (shell_exec_name(cmd, args, 0)) {
        // Not a builtin -- a console app from apps.c's registry, or an
        // executable found by searching PATH (shell_path.c). This is
        // what makes `nx_test` work with no `run` prefix; `run` itself
        // is still a command, and goes through the same resolver so the
        // two can't diverge. Builtins are checked first (the table
        // above) -- see docs/decisions.md for why.
    } else {
        vga_write("Unknown command: ");
        vga_write(cmd);
        vga_putc('\n');
        // MOST OF THESE NAMES ARE PROGRAMS NOW, not builtins, so
        // "unknown command" for one of them means the FILE is missing
        // rather than that the name was never valid -- a very
        // different thing to be told when /bin is damaged, and the
        // situation shell_rescue.c exists for. Say so once, here,
        // instead of giving each evicted command a fallback of its own.
        if (shell_rescue_has(cmd)) {
            vga_write("`");
            vga_write(cmd);
            vga_write("` is a program, and /bin does not have it. The\n");
            vga_write("kernel's own copy is `rescue ");
            vga_write(cmd);
            vga_write("` -- see `rescue`.\n");
        } else {
            vga_write("(type 'help' for commands, or `path` for where\n");
            vga_write("executables are searched for)\n");
        }
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

// One staging buffer for the save AND the load: 8 KiB does not belong
// on a 16 KiB kernel stack, and both are reached only from the single
// console shell. The load reads into it (fs_read_into) rather than
// borrowing the backend's buffer, which a ring-3 syscall could free.
static char g_history_buf[HISTORY_MAX * LINE_MAX];

static void history_save(void) {
    // A fixed staging buffer, and it SKIPS an entry that will not fit
    // rather than the whole save failing -- the file is a convenience,
    // and a shorter one beats a truncated line in it.
    char *buf = g_history_buf;
    size_t pos = 0;
    for (int i = 0; i < history_count; i++) {
        if (!history[i]) continue;
        size_t len = k_strlen(history[i]);
        if (pos + len + 1 >= sizeof g_history_buf) continue; // skip one that will not fit; never overrun
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
    const char *data = g_history_buf;
    uint32_t size = fs_read_into(HISTORY_FILE, g_history_buf, sizeof g_history_buf);
    if (size == 0) return; // no history file yet -- fresh boot or RAM-only mode

    uint32_t pos = 0;
    while (pos < size && history_count < HISTORY_MAX) {
        const char *ls = data + pos;
        const char *end = data + size;
        const char *le = ls;
        while (le < end && *le != '\n') le++;
        pos = (uint32_t)((le < end ? le + 1 : le) - data);

        size_t len = (size_t)(le - ls);
        if (len == 0) continue;
        char *copy = kmalloc((uint32_t)len + 1);
        if (!copy) continue;    // one entry lost, not a truncated one
        k_memcpy(copy, ls, len);
        copy[len] = '\0';
        kfree(history[history_count]);
        history[history_count] = copy;
        history_count++;
    }
}

// Everything that belongs to "having a shell session" rather than to
// "running the interactive REPL". Idempotent and called from BOTH
// shell_main() and shell_dispatch(), because the REPL is not the only
// way into the dispatcher: the serial debug console's `sh` reaches it
// without shell_main() ever running.
//
// The trap this closes: a caller that dispatches without shell_main()
// leaves PATH empty, so every command resolved through it reports
// "Unknown command" while builtins beside it work perfectly (which is
// exactly how it presented on the scripted demo boot, since removed).
// Same shape as vfs.c's ensure_layout() -- an init step that
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
    size_t len = k_strlen(line);
    if (len == 0) return;
    // Copied first: a failure must leave the ring as it was rather than
    // having already dropped the oldest entry to make room.
    char *copy = kmalloc((uint32_t)len + 1);
    if (!copy) return;
    k_memcpy(copy, line, len + 1);

    if (history_count < HISTORY_MAX) {
        kfree(history[history_count]);
        history[history_count] = copy;
        history_count++;
    } else {
        kfree(history[0]);
        for (int i = 1; i < HISTORY_MAX; i++) history[i - 1] = history[i];
        history[HISTORY_MAX - 1] = copy;
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
// carries a 128-byte inline line and an undo stack, more than belongs on
// the shell's modest stack. shell_read_line() isn't reentrant anyway --
// there is exactly one physical console.
static struct kline_edit g_ed;

// The editor takes its memory from here: klineedit.c is compiled into
// both rings and can name neither kmalloc() nor malloc() (klineedit.h).
// Without it the line stops at 128 characters and undo does nothing.
static void *ed_alloc(unsigned long n) { return kmalloc((uint32_t)n); }
static void ed_free(void *p) { kfree(p); }
// The ceiling is what shell_main() can actually run: it copies the
// finished line into a LINE_MAX buffer and dispatches THAT, so an
// editor willing to grow past it would let a line be typed whole and
// run short.
static const struct kline_mem ed_mem = { ed_alloc, ed_free, LINE_MAX - 1 };

// What is currently PAINTED after the prompt, which is not the same as
// what's in the editor until repaint_line() runs. Both are needed to
// erase correctly: the console cursor may be sitting mid-line, and
// vga_backspace() erases relative to wherever it actually is.
static int shown_len = 0;
static int shown_cursor = 0;

// **`#` MEANS RING 0, `$` MEANS RING 3**, and the marker is on the
// PROMPT because that is what you are looking at all day -- a banner
// scrolls away, and the state it described goes with it.
//
// It is Unix's own convention (`#` for the privileged shell, `$` for an
// ordinary one) rather than a spelling invented here, so it reads
// without being explained. All three shells on this machine show the
// cwd, so the final character is the whole difference and the eye can
// land on it: `/# ` here, `/$ ` in /bin/tosh and in the GUI Terminal.
//
// They used to be indistinguishable -- this shell and the GUI Terminal
// both drew `<cwd>> `.
static void print_prompt(void) {
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write(cwd);
    vga_write("# ");
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
// shape. THE LOOP IS SHARED (kernel/lib/histsearch.c) -- what is here is
// the three things a console owns: its history, its blocking key read,
// and how the search row is painted.
//
// Leaves the matched line (or the original, on cancel) in g_ed. Returns
// 1 if the user pressed Enter, which in bash runs the match immediately
// rather than just recalling it.
static int hs_count(void *ctx) { (void)ctx; return history_count; }

static const char *hs_entry(void *ctx, int i) {
    (void)ctx;
    return (i >= 0 && i < history_count) ? history[i] : 0;
}

static int hs_getkey(void *ctx) { (void)ctx; return keyboard_getchar(); }

// '\r' + a full row of spaces + '\r' clears the row without needing a
// cursor-addressing primitive, and the whole row is repainted each time
// because both the pattern and the match change length unpredictably.
static void hs_paint(void *ctx, const char *pattern, const char *match) {
    (void)ctx;
    vga_putc('\r');
    for (uint32_t i = 0; i + 1 < vga_cols(); i++) vga_putc(' ');
    vga_putc('\r');
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("(reverse-i-search)`");
    vga_write(pattern);
    vga_write("': ");
    vga_set_color(shell_fg, VGA_BLACK);
    if (match) vga_write(match);
}

static int reverse_search(void) {
    static const struct histsearch_env env = {
        0, hs_count, hs_entry, hs_getkey, hs_paint,
    };
    const char *match = 0;
    enum histsearch_result r = histsearch_run(&env, &match);
    vga_putc('\n');
    if (r == HISTSEARCH_CANCELLED) return 0;   // the line is untouched
    if (match) kline_set(&g_ed, match);
    return r == HISTSEARCH_ACCEPTED;
}

// Alt-.: insert the last word of the previous command, like bash's
// yank-last-arg. Repeating it doesn't walk further back through history
// here -- one level is what the shortcut actually gets used for (re-use
// the path you just typed).
static void insert_last_arg(void) {
    if (history_count == 0) return;
    const char *prev = history[history_count - 1];
    if (!prev) return;              // a slot whose entry could not be stored
    const char *last = k_strrchr(prev, ' ');
    kline_insert_str(&g_ed, last ? last + 1 : prev);
}

static void shell_read_line(char *buf, unsigned int len) {
    kline_free(&g_ed);          // the previous line's buffer and undo stack
    kline_init_mem(&g_ed, &ed_mem);
    shown_len = 0;
    shown_cursor = 0;

    int hist_index = history_count; // one past the newest = "current blank line"
    static char saved_current[LINE_MAX];
    saved_current[0] = '\0';

    for (;;) {
        int c = keyboard_getchar();

        // Fast path for the overwhelmingly common case: a printable
        // character typed at the end of the line. Skips repaint_line()
        // entirely and just echoes it, so ordinary typing costs exactly
        // what it always did before any of this existed.
        if (IS_PRINTABLE_KEY(c) && g_ed.cursor == g_ed.len &&
            shown_cursor == shown_len && g_ed.len < g_ed.cap - 1) {
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
            kline_free(&g_ed);          // the previous line's buffer and undo stack
            kline_init_mem(&g_ed, &ed_mem);
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
                kline_set(&g_ed, history[hist_index] ? history[hist_index] : "");
                repaint_line();
            }
            break;

        case KLINE_HISTORY_NEXT:
            if (hist_index < history_count) {
                hist_index++;
                kline_set(&g_ed, (hist_index == history_count) ? saved_current
                                 : (history[hist_index] ? history[hist_index] : ""));
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
    static char line[LINE_MAX];

    shell_session_init(); // once per boot; the demo and the serial console reach it first, see its comment

    // THIS SHELL EDITS FOR ITSELF, so it asks the console for raw mode
    // -- ICANON and ECHO off, ISIG on. Exactly what /bin/tosh asks for
    // through sys_tty_raw(0), and for the same reason: klineedit.c does
    // the editing and shell_read_line() does the painting, so a kernel
    // line discipline underneath would buffer every line until Enter and
    // echo every character twice.
    //
    // Through api/keyboard.h rather than kernel/tty.h, because apps/ is
    // NOT on that include path and reaching around the boundary would be
    // a compile error rather than a review catch (kernel/include/
    // README.md). A capability an app needs gets a function on the
    // app-facing side; this is that function.
    keyboard_console_set_raw(1);

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    // NOT "tosh". That is a real program -- /bin/tosh, a RING-3 shell
    // with its own page -- and this is the kernel's own, which had been
    // borrowing the name. Two shells introducing themselves identically
    // is how somebody ends up wondering why Ctrl-C does nothing here.
    vga_write("toy-os kernel shell (ring 0) -- type `help`.\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (;;) {
        print_prompt();

        shell_read_line(line, LINE_MAX);
        history_add(line);
        dispatch(line);
    }
}
