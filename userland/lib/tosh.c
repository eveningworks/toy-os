// tosh -- the toy-os shell (t + OS + h), running in RING 3.
//
// The kernel has its own shell (apps/shell*.c) with dozens of builtins
// that reach straight into the filesystem, the drivers and the test
// harness. This is deliberately not that, and not a front-end to it
// either: it is an ordinary ring-3 program that does what a shell does
// using only syscalls -- a handful of builtins over the file API, and
// SYS_SPAWN for everything else.
//
// That is the whole point of the exercise. `run foo` in the kernel
// Terminal calls scheduler_spawn() because Terminal IS kernel code;
// here, `foo` is spawned by a process with no more privilege than the
// program it starts, and its output arrives through a pipe like any
// other data.
//
// Structured as a library rather than a program: userland/terminal.c
// links it and feeds it a line at a time, because a GUI terminal owns
// its own event loop and cannot sit in a read() loop of its own. A
// standalone `tosh` binary would be a thin main() over the same calls.
#include "lib/tosh.h"
#include "rt/sys.h"
#include <ksignal.h> // signal_name() -- one table, shared with the kernel

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static void scopy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// THIS SHELL NO LONGER RESOLVES PATHS, AND THAT IS THE POINT.
//
// It used to hold its own `cwd` and join every relative path against it
// before calling, which meant a bare "docs" became "/docs" for any
// PROGRAM it spawned -- the shell resolved for its own builtins and had
// no way to resolve for anybody else's arguments. The cwd is the
// kernel's now (SYS_CHDIR/SYS_GETCWD, api/scheduler.h's struct
// sched_cwd), inherited across a spawn, so a path means the same thing
// to `cat` here and to /bin/stat over there. The private resolve() and
// up_one() this file carried are deleted rather than moved: kpath.c's
// k_path_resolve() is the one implementation, it handles ".." and "."
// which up_one() did not, and it now runs below the syscall where every
// caller reaches it.

void tosh_init(struct tosh *sh, tosh_out_fn out, void *ctx) {
    sh->out = out;
    sh->ctx = ctx;
    sh->last_status = 0;
}

static void emit(struct tosh *sh, const char *s) {
    if (sh->out) sh->out(sh->ctx, s, slen(s));
}

static void emit_int(struct tosh *sh, int v) {
    char b[16];
    int i = 0;
    if (v < 0) { b[i++] = '-'; v = -v; }
    char d[12];
    int n = 0;
    if (v == 0) d[n++] = '0';
    while (v > 0) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) b[i++] = d[--n];
    b[i] = '\0';
    emit(sh, b);
}

// --- builtins ---------------------------------------------------------

static void bi_ls(struct tosh *sh, const char *arg) {
    // "" means the cwd, which SYS_LISTDIR resolves to for itself.
    const char *path = (arg && arg[0]) ? arg : ".";

    // STATIC, not a local: 32 dirents is ~2.6 KiB against
    // USERLAND_CFLAGS' -Wframe-larger-than=2048 and a 16 KiB ring-3
    // stack with ONE guard page below it -- the Stack Clash shape the
    // WM's own dirent arrays were moved off the stack for. This shell
    // is single-threaded and never lists two directories at once.
    static struct sys_dirent ents[32];
    int n = sys_listdir(path, ents, 32);
    if (n < 0) { emit(sh, "ls: cannot read "); emit(sh, path); emit(sh, "\n"); return; }
    for (int i = 0; i < n; i++) {
        emit(sh, ents[i].name);
        if (ents[i].is_dir) emit(sh, "/");
        emit(sh, "\n");
    }
}

static void bi_cat(struct tosh *sh, const char *arg) {
    if (!arg || !arg[0]) { emit(sh, "cat: needs a filename\n"); return; }
    const char *path = arg;

    int fd = sys_open(path, 0);
    if (fd < 0) { emit(sh, "cat: cannot open "); emit(sh, path); emit(sh, "\n"); return; }
    char buf[256];
    for (;;) {
        int64_t n = sys_read(fd, buf, sizeof buf - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        emit(sh, buf);
    }
    sys_close(fd);
}

static void bi_cd(struct tosh *sh, const char *arg) {
    // One syscall. The checks this used to do by hand -- does it exist,
    // is it a directory -- are the kernel's now and come back as
    // distinct errno values, so `cd notes.txt` says "not a directory"
    // rather than the "no such directory" a listdir probe could only
    // guess at.
    if (sys_chdir((arg && arg[0]) ? arg : "/") < 0) {
        emit(sh, "cd: ");
        emit(sh, (arg && arg[0]) ? arg : "/");
        emit(sh, ": ");
        emit(sh, sys_strerror(sys_errno()));
        emit(sh, "\n");
    }
}

// --- redirection -------------------------------------------------------
//
// `cmd > file`, `cmd >> file`, `cmd < file`, parsed off the END of the
// line and applied by pointing THIS SHELL's own fd 0/1 at the file
// around the spawn, then putting them back:
//
//     saved = dup(1); dup2(file, 1); spawn(...); dup2(saved, 1); close(saved);
//
// That is the dance fork() normally exists to allow, done without one
// because a spawned child INHERITS this table (abi/syscall_abi.h's
// SYS_SPAWN). The child needs no cooperation and runs no setup code.
//
// The parser is deliberately crude and matches this shell's existing
// level: operators must be space-separated, so `ls >x` is not
// recognised. A shell that quietly half-parses redirection is worse
// than one that plainly does not, since the difference only shows up
// as a file that was never written.
struct tosh_redir {
    int out_fd;     // the file opened for >, or -1
    int in_fd;      // the file opened for <, or -1
    int saved_out;  // this shell's own fd 1, parked
    int saved_in;   // ... and fd 0
};

static void redir_init(struct tosh_redir *r) {
    r->out_fd = r->in_fd = r->saved_out = r->saved_in = -1;
}

// Strips the operators off `line` IN PLACE and opens what they name.
// Returns 0 on success, -1 if a file could not be opened -- in which
// case nothing is left open and the command must not run, exactly as a
// real shell refuses to run `catin < missing`.
//
// LEFT TO RIGHT, which is observable. The first version scanned for the
// LAST operator and worked backwards, so `catin < missing > out`
// created `out` before it ever looked at the input -- leaving a file
// behind for a command that never ran. bash processes redirections in
// order and stops at the first failure; so does this.
#define TOSH_REDIR_MAX 4

static int redir_parse(struct tosh *sh, char *line, struct tosh_redir *r) {
    redir_init(r);

    // Offsets, not pointers into a line being edited. Terminating each
    // name in place overwrote the SPACE before the next operator, and
    // the scan's own "an operator must follow a space" rule then
    // skipped that operator -- so `catin < a > b` silently lost its
    // `> b` and wrote to the console instead of the file.
    struct { int op; int at, len; } found[TOSH_REDIR_MAX];
    int n = 0;
    int cut = -1; // where the command text ends

    for (int i = 0; line[i]; i++) {
        if (line[i] != '>' && line[i] != '<') continue;
        if (i > 0 && line[i - 1] != ' ') continue; // must be its own word
        int op = line[i] == '<' ? 3 : (line[i + 1] == '>' ? 2 : 1);
        int at = i + (op == 2 ? 2 : 1);
        while (line[at] == ' ') at++;
        if (!line[at]) {
            emit(sh, "tosh: expected a filename after the redirect\n");
            return -1;
        }
        if (cut < 0) cut = i;

        int end = at;
        while (line[end] && line[end] != ' ') end++;
        if (n < TOSH_REDIR_MAX) {
            found[n].op = op; found[n].at = at; found[n].len = end - at; n++;
        }
        i = end - 1; // the for's i++ lands on the space, or the NUL
    }

    if (cut >= 0) {
        line[cut] = '\0';
        // Trim what the operator left behind: `ls / > f` would
        // otherwise hand `ls` the argument "/ ", and a listdir of "/ "
        // reports zero entries rather than an error -- so the redirect
        // "worked" and produced an empty file.
        while (cut > 0 && line[cut - 1] == ' ') line[--cut] = '\0';
    }

    for (int i = 0; i < n; i++) {
        char name[TOSH_PATH_MAX];
        int len = found[i].len;
        if (len > TOSH_PATH_MAX - 1) len = TOSH_PATH_MAX - 1;
        for (int j = 0; j < len; j++) name[j] = line[found[i].at + j];
        name[len] = '\0';

        const char *path = name;
        int fd;
        if (found[i].op == 3) {
            fd = sys_open(path, 0);
            if (fd < 0) { emit(sh, "tosh: cannot read "); emit(sh, path); emit(sh, "\n"); return -1; }
            if (r->in_fd >= 0) sys_close(r->in_fd);
            r->in_fd = fd;
        } else {
            // `>` truncates and writes from the start; `>>` appends.
            // Both flags are now REQUIRED to say so: a write used to
            // append unconditionally, so `>>` got its behaviour by
            // saying nothing, and SYS_LSEEK gave the fd's position a
            // meaning for writes that made that silence wrong.
            fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT |
                                (found[i].op == 1 ? SYS_O_TRUNC : SYS_O_APPEND));
            if (fd < 0) { emit(sh, "tosh: cannot write "); emit(sh, path); emit(sh, "\n"); return -1; }
            if (r->out_fd >= 0) sys_close(r->out_fd);
            r->out_fd = fd;
        }
    }
    return 0;
}

// Points this shell's own 0/1 at the redirected files, remembering
// where they pointed.
static void redir_apply(struct tosh_redir *r) {
    if (r->out_fd >= 0) { r->saved_out = sys_dup(1); sys_dup2(r->out_fd, 1); }
    if (r->in_fd  >= 0) { r->saved_in  = sys_dup(0); sys_dup2(r->in_fd, 0); }
}

// ...and puts them back. Closing `saved_*` is safe because a
// descriptor is refcounted: it drops this shell's second name for the
// console, not the console.
static void redir_undo(struct tosh_redir *r) {
    if (r->saved_out >= 0) { sys_dup2(r->saved_out, 1); sys_close(r->saved_out); }
    if (r->saved_in  >= 0) { sys_dup2(r->saved_in, 0);  sys_close(r->saved_in); }
    if (r->out_fd >= 0) sys_close(r->out_fd);
    if (r->in_fd  >= 0) sys_close(r->in_fd);
    redir_init(r);
}

// A sink that writes to a descriptor, so a BUILTIN's output can be
// redirected too. Builtins print through sh->out, which for the GUI
// Terminal draws into a window rather than writing to fd 1 -- so
// redirecting fd 1 alone would silently do nothing there. Swapping the
// sink makes `ls > f` mean the same thing in both front ends.
static void fd_sink(void *ctx, const char *text, int len) {
    int fd = (int)(long)ctx;
    sys_write(fd, text, (unsigned long)len);
}

// --- external programs -------------------------------------------------

// Runs `path` with `args`, streaming its output through a pipe into
// this shell's sink. THIS is the part that could not exist before
// SYS_SPAWN/SYS_PIPE: a ring-3 program starting another and reading
// what it prints.
// --- job control ------------------------------------------------------
//
// EVERY EXTERNAL COMMAND RUNS IN A GROUP OF ITS OWN, and that group goes
// in FRONT of the console while it runs. That is what makes Ctrl-C reach
// the job rather than the shell -- the kernel's INTR key signals the
// console's foreground group (kernel/tty.h) -- and it is why a PIPELINE
// dies as a unit: all its stages share the group, so one keystroke ends
// `cat big | grep x | less` instead of leaving two stages running with
// this shell still waiting on them.
//
// THE GUI TERMINAL RUNS THIS SAME CODE AND OWNS NO CONSOLE, so
// sys_tcsetpgrp() answers EPERM there and every call below is a no-op.
// Deliberately not guarded: the alternative is this library knowing
// which front end it is inside, and the kernel already knows the answer.
// Its Ctrl-C is a separate problem (docs/roadmap.md).
//
// The window nobody can close: between the spawn returning and the
// tcsetpgrp below, a Ctrl-C signals the SHELL's group instead. POSIX has
// the same gap for the same reason and closes it no better; the shell
// ignoring SIGINT is what makes it harmless rather than fatal.
static void job_foreground(int pgid) {
    if (pgid > 0) sys_tcsetpgrp(pgid);
}

// Put this shell's own group back in front, which is what tells the
// kernel there is no job running -- and so restores Ctrl-C's other
// meaning, abandoning the line being typed.
static void job_done(void) {
    int mine = sys_getpgid(0);
    if (mine > 0) sys_tcsetpgrp(mine);
}

// Say so when a job was killed by a signal rather than exiting.
//
// SOMEBODY HAS TO PRINT THE `^C`, and it cannot be the line editor any
// more: with a job in the foreground the kernel SWALLOWS the INTR key
// (kernel/tty.h) rather than delivering the byte, so the editor never
// sees it and its KLINE_CANCEL never fires. Without this line a Ctrl-C
// looks like the program having finished normally -- which is exactly
// the ambiguity `^C` exists to remove. bash prints it for the same
// reason and from the same place: the shell, not the terminal.
//
// The exit code IS the report here: this kernel has one int rather than
// POSIX's packed wait status, so 128 + the signal is all there is to
// read (abi/signal_abi.h).
static void report_signal(struct tosh *sh, int code) {
    int sig = code - SIGNAL_EXIT_BASE;
    if (sig <= 0 || sig > SIGNAL_MAX) return;
    emit(sh, sig == SIGINT ? "^C\n" : "\n");
    if (sig != SIGINT) {
        emit(sh, "killed by SIG");
        emit(sh, signal_name(sig));
        emit(sh, "\n");
    }
}

static int run_external(struct tosh *sh, const char *path, const char *args,
                        int stdout_redirected) {
    // WITH `>` IN EFFECT THERE IS NO PIPE AT ALL. Normally this shell
    // captures the child's stdout so it can stream it into its own sink
    // (the GUI Terminal draws it into a window, and has no fd to hand
    // over). But when the line said `> file`, this shell has already
    // pointed its OWN fd 1 at that file, and the child inherits it --
    // so the right thing is to get out of the way and let the child
    // write straight there. Piping and re-writing would copy every byte
    // through this process for no reason, and would lose the child's
    // output entirely if it outlived the read loop.
    if (stdout_redirected) {
        // PGID_NEW: the child leads a group of its own, so Ctrl-C can be
        // pointed at it without also naming this shell.
        int pid = sys_spawn_group(path, args, -1, environ, PGID_NEW);
        if (pid < 0) return -1;
        job_foreground(sys_getpgid(pid));
        int code = -1;
        sys_waitpid(pid, &code);
        job_done();
        report_signal(sh, code);
        return code;
    }

    int fds[2];
    if (sys_pipe(fds) != 1) { emit(sh, "tosh: out of pipes\n"); return -1; }

    int pid = sys_spawn_group(path, args, fds[1], environ, PGID_NEW);
    if (pid < 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return -1;
    }
    job_foreground(sys_getpgid(pid));

    // Close OUR write end. The child holds its own copy, so this does
    // not end the stream -- but leaving it open would mean the read
    // below never sees EOF even after the child exits, because a live
    // writer (us) would still exist. The classic pipe deadlock.
    sys_close(fds[1]);

    char buf[256];
    for (;;) {
        int64_t n = sys_read(fds[0], buf, sizeof buf - 1);
        if (n <= 0) break; // 0 = EOF; the read BLOCKS rather than spinning
        buf[n] = '\0';
        emit(sh, buf);
    }
    sys_close(fds[0]);

    int code = -1;
    sys_waitpid(pid, &code);
    job_done();
    report_signal(sh, code);
    return code;
}

// PATH lookup, in the same order and spirit as the kernel shell's
// (apps/shell_path.c): /bin, then /usr/bin, then /tests. A name
// containing '/' is a path and is used as given.
static const char *const PATH_DIRS[] = { "/bin", "/usr/bin", "/tests" };
#define PATH_DIR_COUNT (int)(sizeof(PATH_DIRS) / sizeof(PATH_DIRS[0]))

// Returns 1 if `out` names a program, 0 if no PATH entry had it, and -1
// if the search could not be COMPLETED -- which is a third answer, not a
// shade of "no". Until open() could say why it refused, running out of
// descriptors was indistinguishable from the file not existing, so a
// machine with a full fd table reported "not found" for a command that
// was sitting right there. See docs/errno-design.md.
static int find_program(const char *name, char *out, int cap) {
    for (const char *p = name; *p; p++) {
        if (*p == '/') { scopy(out, name, cap); return 1; }
    }
    for (int i = 0; i < PATH_DIR_COUNT; i++) {
        int n = 0;
        for (const char *d = PATH_DIRS[i]; *d && n < cap - 2; d++) out[n++] = *d;
        out[n++] = '/';
        for (const char *c = name; *c && n < cap - 1; c++) out[n++] = *c;
        out[n] = '\0';
        // Probing by opening is the only test available: there is no
        // stat syscall yet. A directory would open too, but PATH
        // entries holding a directory named like a command is not a
        // case worth carrying code for.
        int fd = sys_open(out, 0);
        if (fd >= 0) { sys_close(fd); return 1; }
        // ENOENT is the ordinary answer -- keep looking. Anything else
        // is about this SHELL, not about this candidate: EMFILE means
        // the next probe cannot succeed either, so continuing would
        // walk the whole PATH to arrive at a wrong conclusion.
        if (sys_errno() != ENOENT) return -1;
    }
    return 0;
}

// The body, once redirection has been stripped off and applied. Split
// out so every `return` below cannot forget to put fd 0/1 back -- the
// one thing in this file that leaks a descriptor if it is missed.
static int run_stripped(struct tosh *sh, const char *line, int stdout_redirected) {
    // Split into command and the rest. Everything after the first space
    // is handed to the program verbatim -- there is no quoting or
    // globbing here, and pretending otherwise would be worse than not
    // having it.
    char cmd[TOSH_PATH_MAX];
    int i = 0;
    while (line[i] == ' ') i++;
    int c = 0;
    while (line[i] && line[i] != ' ' && c < TOSH_PATH_MAX - 1) cmd[c++] = line[i++];
    cmd[c] = '\0';
    while (line[i] == ' ') i++;
    const char *args = line[i] ? line + i : 0;

    if (!cmd[0]) return 0;

    if (seq(cmd, "ls"))   { bi_ls(sh, args);  return 0; }
    if (seq(cmd, "cat"))  { bi_cat(sh, args); return 0; }
    if (seq(cmd, "cd"))   { bi_cd(sh, args);  return 0; }
    if (seq(cmd, "pwd"))  {
        char here[TOSH_PATH_MAX];
        if (sys_getcwd(here, sizeof here) < 0) scopy(here, "?", sizeof here);
        emit(sh, here);
        emit(sh, "\n");
        return 0;
    }
    if (seq(cmd, "echo")) { if (args) emit(sh, args); emit(sh, "\n"); return 0; }
    if (seq(cmd, "help")) {
        emit(sh, "tosh -- the toy-os shell, running in ring 3.\n"
                 "builtins: ls cat cd pwd echo help\n"
                 "redirection: cmd > file, cmd >> file, cmd < file\n"
                 "anything else is spawned from /bin, /usr/bin or /tests\n");
        return 0;
    }

    char path[TOSH_PATH_MAX];
    int found = find_program(cmd, path, TOSH_PATH_MAX);
    if (found <= 0) {
        emit(sh, cmd);
        if (found < 0) {
            // The search was abandoned, so "not found" would be a claim
            // about the filesystem this shell is in no position to make.
            emit(sh, ": cannot search PATH: ");
            emit(sh, sys_strerror(sys_errno()));
            emit(sh, "\n");
        } else {
            emit(sh, ": not found\n");
        }
        sh->last_status = -1;
        return -1;
    }

    int code = run_external(sh, path, args, stdout_redirected);
    sh->last_status = code;
    if (code != 0) {
        emit(sh, "[exit ");
        emit_int(sh, code);
        emit(sh, "]\n");
    }
    return code;
}

// --- pipelines ---------------------------------------------------------
//
// `a | b | c`. Each stage but the last writes into a pipe the next
// stage reads, and the plumbing is the same dance `>` uses: the shell
// points its OWN fd 0/1 at the right ends, spawns, and puts them back.
// The child needs no cooperation, which is the whole reason no fork()
// is required.
//
// TWO THINGS THAT WOULD DEADLOCK IF DONE THE OBVIOUS WAY.
//
// The parent must CLOSE every pipe end once the stage holding it has
// been spawned. A pipe reports EOF when its last writer goes, and the
// shell counts as a writer -- so a forgotten close leaves the reading
// stage waiting forever for a producer that has already exited.
//
// And a BUILTIN stage runs inside this shell, synchronously. Since a
// full pipe now BLOCKS its writer (api/pipe.h), a builtin producing
// more than 4 KiB before its reader exists would block the shell
// against a stage it has not spawned yet -- a deadlock with itself. So
// builtins are spawned-last: every external stage is running and
// draining before any builtin writes a byte.
#define TOSH_STAGE_MAX 4

struct tosh_stage {
    const char *cmd;   // into the caller's mutable line
    int in_fd, out_fd; // -1 = inherit whatever the shell has
    int pid;           // -1 = a builtin, run in pass 2
};

// Splits on `|` IN PLACE. Returns the stage count, or -1 if there are
// too many or one is empty (`a |` and `| b` are errors, not silence).
static int split_stages(struct tosh *sh, char *line, struct tosh_stage *st) {
    int n = 0;
    char *p = line;
    for (;;) {
        if (n >= TOSH_STAGE_MAX) {
            emit(sh, "tosh: too many pipeline stages\n");
            return -1;
        }
        char *bar = 0;
        for (char *q = p; *q; q++) if (*q == '|') { bar = q; break; }
        if (bar) *bar = '\0';

        while (*p == ' ') p++;
        int len = slen(p);
        while (len > 0 && p[len - 1] == ' ') p[--len] = '\0';
        if (!p[0]) {
            emit(sh, "tosh: empty pipeline stage\n");
            return -1;
        }
        st[n].cmd = p;
        st[n].in_fd = st[n].out_fd = -1;
        st[n].pid = -1;
        n++;
        if (!bar) return n;
        p = bar + 1;
    }
}

// The command word of a stage, for deciding builtin vs external.
static int stage_is_builtin(const char *cmd) {
    char w[TOSH_PATH_MAX];
    int i = 0, c = 0;
    while (cmd[i] == ' ') i++;
    while (cmd[i] && cmd[i] != ' ' && c < TOSH_PATH_MAX - 1) w[c++] = cmd[i++];
    w[c] = '\0';
    return seq(w, "ls") || seq(w, "cat") || seq(w, "cd") || seq(w, "pwd")
        || seq(w, "echo") || seq(w, "help");
}

static int run_pipeline(struct tosh *sh, struct tosh_stage *st, int n,
                        int stdout_redirected) {
    // Stage i's output goes to a fresh pipe, whose read end becomes
    // stage i+1's input. The LAST stage keeps the shell's own fd 1 --
    // which is a `>` file if the line had one, and otherwise whatever
    // run_stage() below arranges for capture.
    int prev_read = -1;
    for (int i = 0; i < n; i++) {
        st[i].in_fd = prev_read;
        prev_read = -1;
        if (i < n - 1) {
            int fds[2];
            if (sys_pipe(fds) != 1) { emit(sh, "tosh: out of pipes\n"); return -1; }
            st[i].out_fd = fds[1];
            prev_read = fds[0];
        }
    }

    // THE LAST STAGE IS CAPTURED, unless the line redirected it. Not an
    // optimisation: this shell's sink is not always fd 1. The GUI
    // Terminal draws into a window and has no descriptor to hand over,
    // so a last stage left on the shell's own fd 1 would print to the
    // PHYSICAL CONSOLE -- output that silently appears on another
    // screen. Skipped for a builtin last stage, which already prints
    // through the sink.
    int capture_r = -1;
    int last = n - 1;
    if (!stdout_redirected && !stage_is_builtin(st[last].cmd)) {
        int fds[2];
        if (sys_pipe(fds) != 1) { emit(sh, "tosh: out of pipes\n"); return -1; }
        capture_r = fds[0];
        st[last].out_fd = fds[1];
    }

    // ONE GROUP FOR THE WHOLE PIPELINE -- the first external stage
    // leads it (PGID_NEW), every later stage joins. That is what makes
    // one Ctrl-C end `cat big | grep x | less` rather than only its last
    // stage, which would leave two processes running and this shell
    // still waiting on them. See job_foreground() above.
    int job_pgid = 0;

    // Pass 1: the external stages, in order.
    for (int i = 0; i < n; i++) {
        if (stage_is_builtin(st[i].cmd)) continue;

        char path[TOSH_PATH_MAX];
        char cmd[TOSH_PATH_MAX];
        int k = 0, c = 0;
        while (st[i].cmd[k] == ' ') k++;
        while (st[i].cmd[k] && st[i].cmd[k] != ' ' && c < TOSH_PATH_MAX - 1)
            cmd[c++] = st[i].cmd[k++];
        cmd[c] = '\0';
        while (st[i].cmd[k] == ' ') k++;
        const char *args = st[i].cmd[k] ? st[i].cmd + k : 0;

        if (!find_program(cmd, path, TOSH_PATH_MAX)) {
            emit(sh, cmd);
            emit(sh, ": not found\n");
            continue; // its stage simply produces nothing
        }

        int saved_in = -1, saved_out = -1;
        if (st[i].in_fd  >= 0) { saved_in  = sys_dup(0); sys_dup2(st[i].in_fd, 0); }
        if (st[i].out_fd >= 0) { saved_out = sys_dup(1); sys_dup2(st[i].out_fd, 1); }

        // -1 = inherit the fds we just set. The GROUP is explicit: the
        // first stage leads, the rest join it.
        st[i].pid = sys_spawn_group(path, args, -1, environ,
                                     job_pgid > 0 ? job_pgid : PGID_NEW);
        if (st[i].pid > 0 && job_pgid <= 0) {
            job_pgid = sys_getpgid(st[i].pid);
            job_foreground(job_pgid);
        }

        if (saved_in  >= 0) { sys_dup2(saved_in, 0);  sys_close(saved_in); }
        if (saved_out >= 0) { sys_dup2(saved_out, 1); sys_close(saved_out); }
    }

    // Every pipe end this shell still holds must go NOW, before anything
    // is waited for: each spawned stage has its own copy, and a stage
    // reading a pipe this shell still writes would never see EOF.
    //
    // The ONE exception is a builtin's own output end, which this shell
    // is about to write through in pass 2. It is closed immediately
    // after that builtin runs, which is the same rule -- close as soon
    // as the writer is finished with it -- applied to a writer that
    // happens to be us.
    for (int i = 0; i < n; i++) {
        if (st[i].in_fd  >= 0) { sys_close(st[i].in_fd);  st[i].in_fd  = -1; }
        int builtin_producer = stage_is_builtin(st[i].cmd) && st[i].pid <= 0;
        if (st[i].out_fd >= 0 && !builtin_producer) {
            sys_close(st[i].out_fd);
            st[i].out_fd = -1;
        }
    }

    // Pass 2: the builtins, LAST, and that ordering is the point. A
    // builtin runs inside this shell, synchronously, and a full pipe now
    // blocks its writer -- so a builtin producing more than 4 KiB before
    // its reader existed would block the shell against a stage it had
    // not spawned yet. By here every external stage is running and
    // draining, so it cannot deadlock against itself.
    for (int i = 0; i < n; i++) {
        if (!stage_is_builtin(st[i].cmd) || st[i].pid > 0) continue;

        // Its output goes to its stage's pipe, not to the shell's sink.
        // A builtin prints through sh->out (the GUI Terminal's draws
        // into a window), so pointing fd 1 somewhere would miss it --
        // the same reason `>` swaps the sink rather than only dup2ing.
        tosh_out_fn prev = sh->out;
        void *prev_ctx = sh->ctx;
        if (st[i].out_fd >= 0) {
            sh->out = fd_sink;
            sh->ctx = (void *)(long)st[i].out_fd;
        }

        run_stripped(sh, st[i].cmd, stdout_redirected);

        sh->out = prev;
        sh->ctx = prev_ctx;
        if (st[i].out_fd >= 0) {
            // NOW, so the next stage sees EOF. This shell was the last
            // writer of that pipe.
            sys_close(st[i].out_fd);
            st[i].out_fd = -1;
        }
    }

    // Drain the last stage into this shell's sink. AFTER the builtins
    // and BEFORE the waits, and both halves of that are deadlocks
    // avoided rather than style. Draining before pass 2 would block
    // this shell on output from a pipeline whose first stage -- a
    // builtin, run by this same shell -- had not started. Waiting
    // before draining would block on a stage that is itself blocked
    // writing into a capture pipe nobody is emptying.
    if (capture_r >= 0) {
        char buf[256];
        for (;;) {
            int64_t got = sys_read(capture_r, buf, sizeof buf - 1);
            if (got <= 0) break; // 0 = EOF
            buf[got] = '\0';
            emit(sh, buf);
        }
        sys_close(capture_r);
    }

    int code = 0;
    for (int i = 0; i < n; i++) {
        if (st[i].pid <= 0) continue;
        int c2 = -1;
        sys_waitpid(st[i].pid, &c2);
        code = c2; // the pipeline's status is its LAST stage's, as in sh
    }
    // AFTER every wait, not after the first: until the last stage has
    // been reaped there is still a job in front of the console, and
    // taking the foreground back early would point a Ctrl-C at this
    // shell while its pipeline was still running.
    job_done();
    report_signal(sh, code);
    return code;
}

int tosh_run_line(struct tosh *sh, const char *line) {
    // A MUTABLE copy: redirection is stripped off the line in place,
    // and the caller's buffer is not ours to edit (the GUI Terminal
    // hands us its editor's live buffer).
    char work[TOSH_PATH_MAX];
    scopy(work, line, TOSH_PATH_MAX);

    struct tosh_redir r;
    if (redir_parse(sh, work, &r) < 0) {
        // Nothing was opened and nothing runs -- `cat < missing` must
        // not execute `cat` against the console.
        redir_undo(&r);
        sh->last_status = -1;
        return -1;
    }
    redir_apply(&r);

    // A builtin prints through sh->out, so redirecting fd 1 alone would
    // miss it in the GUI Terminal, whose sink draws into a window.
    // Swapping the sink is what makes `ls > f` mean one thing in both
    // front ends.
    tosh_out_fn saved_out = sh->out;
    void *saved_ctx = sh->ctx;
    if (r.out_fd >= 0) { sh->out = fd_sink; sh->ctx = (void *)(long)1; }

    // A PIPELINE if the line has a `|`, otherwise the single-command
    // path. Split after redirection was stripped, so `a | b > f`
    // redirects the LAST stage -- which is what a real shell does,
    // because `>` binds to the whole pipeline's output.
    struct tosh_stage st[TOSH_STAGE_MAX];
    int nst = split_stages(sh, work, st);
    int code;
    if (nst < 0) {
        code = -1;
    } else if (nst > 1) {
        code = run_pipeline(sh, st, nst, r.out_fd >= 0);
    } else {
        code = run_stripped(sh, work, r.out_fd >= 0);
    }

    sh->out = saved_out;
    sh->ctx = saved_ctx;
    redir_undo(&r);
    return code;
}
