// tosh -- the toy-os shell (t + OS + h), running in RING 3.
//
// The kernel has its own shell (apps/shell*.c) with dozens of builtins
// that reach straight into the filesystem, the drivers and the test
// harness. This is deliberately not that, and not a front-end to it
// either: it is an ordinary ring-3 program that does what a shell does
// using only syscalls -- three builtins that have to be builtins, and
// SYS_SPAWN for everything else.
//
// That is the whole point of the exercise. `run foo` in the kernel
// Terminal calls scheduler_spawn() because Terminal IS kernel code;
// here, `foo` is spawned by a process with no more privilege than the
// program it starts, and its output arrives through a pipe like any
// other data.
//
// Structured as a library rather than a program: `/bin/tosh` is a thin
// main() over it. It had a second caller -- the GUI Terminal linked it
// and fed it a line at a time -- until that window became a real
// terminal emulator running `/bin/tosh` on a pty, which is the shape
// this split was always heading for.
//
// **THREE BUILTINS, AND EACH ONE HAS TO BE.** `cd` changes the SHELL's
// own directory, so a program could not do it; `pwd` and `help` have no
// `/bin` twin. Everything else is a program, and that is a rule rather
// than an inventory:
//
//   **A BUILTIN MUST NOT SHADOW A `/bin` PROGRAM THAT DOES MORE.**
//
// `ls` and `echo` were builtins here and both were strictly worse than
// the programs they hid: this `ls` took no flags at all -- `ls -l`
// answered "cannot read -l" -- and coloured nothing, while `/bin/ls`
// has ten flags and colours directories. `cat`'s builtin went first,
// for the same reason in a different disguise (it could not read fd 0,
// so `foo | cat` printed an error). Three instances of one mistake.
#include "lib/tosh.h"
#include "rt/sys.h"
#include <ksignal.h> // signal_name() -- one table, shared with the kernel
#include "lib/tosh_jobs.h"
#include "lib/upath.h"  // upath_find_program() -- shared with /bin/strace

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
    if (pgid > 0) sys_tcsetpgrp(0, pgid);
}

// Put this shell's own group back in front, which is what tells the
// kernel there is no job running -- and so restores Ctrl-C's other
// meaning, abandoning the line being typed.
static void job_done(void) {
    int mine = sys_getpgid(0);
    if (mine > 0) sys_tcsetpgrp(0, mine);
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

// A job just stopped. Put it in the table and say so, in the shape
// every Unix shell says it: `[1]+  Stopped  sleep 30`.
//
// PRINTED BY THE SHELL, for the same reason `^C` is: the kernel
// SWALLOWS the SUSP key when a job holds the terminal, so nothing else
// is in a position to notice. Without this line a Ctrl-Z looks exactly
// like the program having finished -- and unlike a Ctrl-C, the process
// is still there, holding memory, invisible.
static void job_stopped(struct tosh *sh, int pgid, const int *pids, int npid,
                        const char *label) {
    int id = tosh_jobs_add(pgid, pids, npid, label, 1);
    if (!id) {
        // SAID OUT LOUD rather than dropped. A job with no table entry
        // is one `fg` cannot name and `jobs` will not list, so the only
        // way back to it is `ps` and `kill -CONT` -- which the user
        // needs to be told, at the moment it happens.
        emit(sh, "\ntosh: too many jobs -- group ");
        emit_int(sh, pgid);
        emit(sh, " is stopped and untracked (kill -CONT -");
        emit_int(sh, pgid);
        emit(sh, ")\n");
        return;
    }
    emit(sh, "\n[");
    emit_int(sh, id);
    emit(sh, "]+  Stopped  ");
    emit(sh, label);
    emit(sh, "\n");
}

// Waits for a foreground job, hands the terminal back, and reports.
// Returns the child's status -- which may be a STOP, so a caller must
// not assume the job is over.
//
// ONE FUNCTION FOR THE SIMPLE COMMAND, THE PIPELINE AND `fg`, because
// all three end the same way and the ending is the part with the traps
// in it: put this shell's group back in front, then decide whether what
// came back was a death, a suspension or an ordinary exit.
static int job_wait(struct tosh *sh, int pgid, const int *pids, int npid,
                    const char *label) {
    int code = -1;
    for (int i = 0; i < npid; i++) {
        if (pids[i] <= 0) continue;
        int c = -1;
        sys_waitpid_untraced(pids[i], &c);
        code = c; // the job's status is its LAST stage's, as in sh
        // **THE WHOLE JOB IS SUSPENDED, so stop waiting on the rest.**
        // SUSP reaches the GROUP, so the moment one stage reports a
        // stop the others are stopped too, and waiting on them would
        // park this shell against processes nothing will resume.
        if (SIGNAL_IS_STOP(c)) break;
    }
    // AFTER every wait, not after the first: until the last stage is
    // reaped there is still a job in front of the console, and taking
    // the foreground back early would point a Ctrl-C at this shell
    // while its pipeline was still running.
    job_done();
    if (SIGNAL_IS_STOP(code)) {
        job_stopped(sh, pgid, pids, npid, label);
        return code;
    }
    report_signal(sh, code);
    return code;
}

// Announce a job that has just been put in the background: `[1] 4`,
// the id and the pid, which is what every shell prints and what makes
// `kill` and `fg` both possible from what is on screen.
static void job_backgrounded(struct tosh *sh, int pgid, const int *pids,
                             int npid, const char *label) {
    int id = tosh_jobs_add(pgid, pids, npid, label, 0);
    if (!id) {
        emit(sh, "tosh: too many jobs -- group ");
        emit_int(sh, pgid);
        emit(sh, " is running untracked\n");
        return;
    }
    emit(sh, "[");
    emit_int(sh, id);
    emit(sh, "] ");
    emit_int(sh, pids[npid - 1]);  // the last stage, as every shell prints
    emit(sh, "\n");
}

static int run_external(struct tosh *sh, const char *path, const char *args,
                        const char *label, int background,
                        int stdout_redirected) {
    (void)stdout_redirected; // both cases are the same now -- see below

    // **THE CHILD INHERITS THIS SHELL'S fd 1, ALWAYS.** It used to be
    // captured through a pipe and re-emitted through `sh->out`, because
    // the GUI Terminal hosted this library, drew into a window and had
    // no descriptor to hand over -- a child left on the shell's own fd 1
    // would have printed to the PHYSICAL CONSOLE. That caller is gone:
    // the Terminal runs `/bin/tosh` on a pty now, so this shell's fd 1
    // IS a terminal and the child should have it.
    //
    // **AND CAPTURING WAS NOT MERELY WASTEFUL, IT WAS WRONG.** A pipe is
    // not a terminal, so `isatty(1)` was FALSE in every program this
    // shell ran -- which turned `ls`'s `--color=auto` off and left a
    // listing in a Terminal window uncoloured, with nothing to say why.
    // Reported from a screenshot. Anything else deciding "am I
    // interactive?" would have been wrong the same way.
    //
    // What it also removes: copying every byte of a program's output
    // through this process, and a read loop that would have lost output
    // from a child outliving it.
    //
    // PGID_NEW: the child leads a group of its own, so Ctrl-C can be
    // pointed at it without also naming this shell.
    int pid = sys_spawn_group(path, args, -1, environ, PGID_NEW);
    if (pid < 0) return -1;
    int pgid = sys_getpgid(pid);

    // **A BACKGROUND JOB IS NOT GIVEN THE TERMINAL, AND THAT IS THE
    // WHOLE OF IT.** No job_foreground(), so Ctrl-C keeps pointing at
    // this shell and a read from the job hits SIGTTIN instead of
    // competing with the prompt for keystrokes. No wait either -- the
    // job is reaped at the next prompt by tosh_reap_jobs().
    if (background) {
        job_backgrounded(sh, pgid, &pid, 1, label);
        return 0;
    }

    job_foreground(pgid);
    return job_wait(sh, pgid, &pid, 1, label);
}

// PATH lookup moved to userland/lib/upath.c when `/bin/strace` became
// its second real caller -- it has to find the same program this shell
// would, and a second hand-rolled copy of the search would be a place
// for the two to disagree. The three answers (found / not found / could
// not complete) are upath.h's, and the reason the third exists is the
// one this shell paid for.

// --- the job-control builtins -----------------------------------------
//
// **THESE HAVE TO BE BUILTINS, and that is the test this file's own
// rule sets.** `cd` is a builtin because it changes the SHELL's
// directory; these are builtins because the job table is the shell's
// and there is nothing for a program to read. A `/bin/fg` would be a
// separate process, with no view of this one's table, unable to take
// the terminal on its parent's behalf -- so there is nothing here for a
// program to do more of. POSIX makes them special builtins for the same
// reason.

static void jobs_line(void *ctx, const struct tosh_job *job, char marker) {
    struct tosh *sh = (struct tosh *)ctx;
    emit(sh, "[");
    emit_int(sh, job->id);
    emit(sh, "]");
    char m[2] = { marker, '\0' };
    emit(sh, m);
    emit(sh, "  ");
    emit(sh, job->stopped ? "Stopped  " : "Running  ");
    emit(sh, job->cmd);
    emit(sh, "\n");
}

static void bi_jobs(struct tosh *sh) {
    // SILENT WHEN THERE ARE NONE, as every shell is: `jobs` in a fresh
    // shell printing "no jobs" would make it useless in a pipeline and
    // noisy in a prompt.
    tosh_jobs_each(jobs_line, sh);
}

// `fg [n]` -- put a job back in the foreground and wait for it again.
//
// THE ORDER IS THE WHOLE THING, and getting it wrong is a lost
// keystroke or a shell that waits forever:
//   1. hand the terminal over FIRST, so the job owns it before it runs
//      a single instruction and cannot miss a Ctrl-C typed immediately;
//   2. THEN SIGCONT the group, all of it, because a pipeline suspends
//      as a unit and resuming one stage leaves the others parked;
//   3. wait, exactly as the original command line did -- which is why
//      job_wait() is one function rather than three copies.
static int bi_fg(struct tosh *sh, const char *args) {
    int id = 0;
    if (args) {
        // `%1` as well as `1`: the `%` is what a job spec looks like in
        // every shell, and accepting only the bare number would make
        // copying an id straight out of `jobs` output wrong.
        const char *p = args;
        while (*p == ' ') p++;
        if (*p == '%') p++;
        for (; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
    }

    struct tosh_job *job = tosh_jobs_get(id);
    if (!job) {
        emit(sh, "fg: no such job\n");
        sh->last_status = -1;
        return -1;
    }

    // Copied out before anything can invalidate the entry -- job_wait()
    // may add a job of its own, and the table is an array.
    int pgid = job->pgid, jid = job->id, npid = job->npid;
    int pids[TOSH_JOB_PIDS_MAX];
    for (int i = 0; i < npid; i++) pids[i] = job->pid[i];
    char label[TOSH_JOB_CMD_MAX];
    scopy(label, job->cmd, TOSH_JOB_CMD_MAX);

    // WHAT IS BEING RESUMED, printed first. A resumed job produces no
    // output of its own to say it is back, and `fg` in a shell with
    // several jobs is exactly where a person needs confirming.
    emit(sh, label);
    emit(sh, "\n");

    tosh_jobs_remove(jid);
    job_foreground(pgid);
    sys_kill(-pgid, SIGCONT);

    int code = job_wait(sh, pgid, pids, npid, label);
    sh->last_status = code;
    return code;
}

// `bg [n]` -- resume a suspended job WITHOUT giving it the terminal.
//
// The difference from `fg` is one missing line, and it is the whole
// difference: no job_foreground(), so the shell keeps the terminal and
// gets its prompt straight back. The job stays in the table, now marked
// running, because it is still this shell's to report on when it ends.
//
// **AND IF IT READS THE TERMINAL IT WILL STOP AGAIN**, by SIGTTIN, which
// is exactly right and is why `&` was safe to add at all. That shows up
// at the next prompt as `[1]+ Stopped`, and `fg` is how it gets served.
static int bi_bg(struct tosh *sh, const char *args) {
    int id = 0;
    if (args) {
        const char *p = args;
        while (*p == ' ') p++;
        if (*p == '%') p++;
        for (; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
    }

    struct tosh_job *job = tosh_jobs_get(id);
    if (!job) {
        emit(sh, "bg: no such job\n");
        sh->last_status = -1;
        return -1;
    }
    if (!job->stopped) {
        emit(sh, "bg: job is already running\n");
        return 0;
    }

    job->stopped = 0;
    sys_kill(-job->pgid, SIGCONT);

    emit(sh, "[");
    emit_int(sh, job->id);
    emit(sh, "]+ ");
    emit(sh, job->cmd);
    emit(sh, " &\n");
    return 0;
}

// Everything a background job did while nobody was looking, reported
// once, just before the next prompt.
//
// **CALLED BY THE FRONT END, JUST BEFORE IT DRAWS A PROMPT**, and
// nowhere else. That is what every shell does and it is not merely
// convention: a job that exits mid-command would otherwise interleave
// `[1]+ Done` into the output of whatever is running, and one that
// exits while the user is halfway through typing would land in the
// middle of the line. A prompt is the one moment the screen is the
// shell's -- and the front end is the only thing that knows when one is
// about to happen, which is why this is not called from
// tosh_run_line(): a bare Enter draws a prompt and runs no line at all.
//
// It ASKS AFTER EACH KNOWN PID rather than scanning: this is a job
// table, not a process table, and `ps` is the program that scans.
void tosh_reap_jobs(struct tosh *sh) {
    for (int id = 1; id <= TOSH_JOBS_MAX; id++) {
        struct tosh_job *job = tosh_jobs_get(id);
        if (!job || job->id != id) continue;
        if (job->stopped) continue;   // nothing to hear from a stopped job

        // EVERY STAGE IS ASKED AFTER, so an earlier one that has
        // finished is reaped rather than left a zombie -- but only the
        // LAST one decides whether the job is over, because its status
        // is the job's.
        int code = 0, done = 0, stopped = 0;
        for (int i = 0; i < job->npid; i++) {
            int c = 0;
            int r = sys_waitpid_check(job->pid[i], &c);
            if (r != job->pid[i]) continue;
            if (SIGNAL_IS_STOP(c)) { stopped = 1; break; }
            if (i == job->npid - 1) { code = c; done = 1; }
        }
        if (!stopped && !done) continue;  // still running

        if (stopped) {
            // A BACKGROUND JOB THAT STOPPED ITSELF, which in practice
            // means it tried to read the terminal and SIGTTIN caught it.
            // Reported rather than left silently parked -- a job that
            // stopped for input and said nothing is indistinguishable
            // from one that hung.
            job->stopped = 1;
            emit(sh, "[");
            emit_int(sh, id);
            emit(sh, "]+  Stopped  ");
            emit(sh, job->cmd);
            emit(sh, "\n");
            continue;
        }

        emit(sh, "[");
        emit_int(sh, id);
        emit(sh, "]+  Done  ");
        emit(sh, job->cmd);
        if (code != 0) {
            emit(sh, "  [exit ");
            emit_int(sh, code);
            emit(sh, "]");
        }
        emit(sh, "\n");
        tosh_jobs_remove(id);
    }
}

// Declared here because run_stripped() refuses to background one, and
// the definition lives with the pipeline code that is its other caller.
static int stage_is_builtin(const char *cmd);

// The body, once redirection has been stripped off and applied. Split
// out so every `return` below cannot forget to put fd 0/1 back -- the
// one thing in this file that leaks a descriptor if it is missed.
static int run_stripped(struct tosh *sh, const char *line, int background,
                        int stdout_redirected) {
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

    // **A BUILTIN CANNOT BE BACKGROUNDED, AND IT IS REFUSED RATHER THAN
    // QUIETLY RUN IN FRONT.** bash backgrounds one by forking a
    // subshell; with no fork there is no second copy of this shell to
    // run it in, and running it in the foreground while the user asked
    // for `&` would be a silent difference in meaning. `cd dir &` is
    // almost certainly a typo anyway -- the version that "worked" would
    // change this shell's directory, which is the opposite of what
    // backgrounding it implies.
    if (background && stage_is_builtin(cmd)) {
        emit(sh, cmd);
        emit(sh, ": cannot be backgrounded -- it runs inside this shell\n");
        sh->last_status = -1;
        return -1;
    }

    if (seq(cmd, "cd"))   { bi_cd(sh, args);  return 0; }
    if (seq(cmd, "jobs")) { bi_jobs(sh);       return 0; }
    if (seq(cmd, "fg"))   { return bi_fg(sh, args); }
    if (seq(cmd, "bg"))   { return bi_bg(sh, args); }
    if (seq(cmd, "pwd"))  {
        char here[TOSH_PATH_MAX];
        if (sys_getcwd(here, sizeof here) < 0) scopy(here, "?", sizeof here);
        emit(sh, here);
        emit(sh, "\n");
        return 0;
    }
    if (seq(cmd, "help")) {
        emit(sh, "tosh -- the toy-os shell, running in ring 3.\n"
                 "builtins: cd pwd jobs fg bg help  (everything else is a program)\n"
                 "redirection: cmd > file, cmd >> file, cmd < file\n"
                 "job control: cmd & backgrounds, Ctrl-Z suspends, `jobs` lists,\n"
                 "             `fg [n]` resumes in front, `bg [n]` behind\n"
                 "anything else is spawned from /bin, /usr/bin or /tests\n");
        return 0;
    }

    char path[TOSH_PATH_MAX];
    int found = upath_find_program(cmd, path, TOSH_PATH_MAX);
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

    int code = run_external(sh, path, args, line, background, stdout_redirected);
    sh->last_status = code;
    // A STOP IS NOT AN EXIT, and `[exit 276]` for a job the user just
    // suspended would be a lie about a process that is still there.
    // job_stopped() has already said what happened.
    if (code != 0 && !SIGNAL_IS_STOP(code)) {
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
    return seq(w, "cd") || seq(w, "pwd") || seq(w, "help") ||
           seq(w, "jobs") || seq(w, "fg") || seq(w, "bg");
}

static int run_pipeline(struct tosh *sh, struct tosh_stage *st, int n,
                        const char *label, int background,
                        int stdout_redirected) {
    // Stage i's output goes to a fresh pipe, whose read end becomes
    // stage i+1's input. The LAST stage keeps the shell's own fd 1 --
    // which is a `>` file if the line had one, and otherwise whatever
    // run_stage() below arranges for capture.
    // THE FIRST STAGE HAS NO UPSTREAM, so it INHERITS this shell's fd 0
    // -- which is the terminal this shell is reading, and is exactly
    // right. Every later stage reads the pipe from the one before it.
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

    // **THE LAST STAGE KEEPS THIS SHELL'S fd 1**, which is a terminal.
    // It used to be captured into a pipe and re-emitted, for the reason
    // run_external() above records at length -- and with the same cost:
    // a pipe is not a terminal, so `isatty(1)` was false for the last
    // stage of every pipeline and `ls | cat` behaved differently from
    // `ls` for reasons no user could see.
    (void)stdout_redirected;

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

        if (!upath_find_program(cmd, path, TOSH_PATH_MAX)) {
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
            // A BACKGROUND PIPELINE IS STILL ONE GROUP -- it just is not
            // the foreground one. Skipping this is the entire difference.
            if (!background) job_foreground(job_pgid);
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

        run_stripped(sh, st[i].cmd, 0, stdout_redirected);

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


    // NOT WAITED FOR, and announced under the LAST stage's pid -- which
    // is the pid whose status the pipeline reports when it ends, and so
    // the one tosh_reap_jobs() has to ask after.
    // THE SPAWNED STAGES, in pipeline order. A stage that failed to
    // start has no pid and is skipped -- it produced nothing and there
    // is nothing to wait for.
    int pids[TOSH_JOB_PIDS_MAX];
    int npid = 0;
    for (int i = 0; i < n && npid < TOSH_JOB_PIDS_MAX; i++)
        if (st[i].pid > 0) pids[npid++] = st[i].pid;
    if (!npid) { job_done(); return 0; }

    if (background) {
        job_backgrounded(sh, job_pgid, pids, npid, label);
        return 0;
    }

    // One function for the simple command, the pipeline and `fg`,
    // because all three end the same way -- see job_wait().
    return job_wait(sh, job_pgid, pids, npid, label);
}

// A trailing `&` means BACKGROUND. Removed from the line in place, so
// nothing downstream has to know about it.
//
// TRAILING ONLY, and this shell will not pretend otherwise: `a & b` is
// two commands in a real shell and one malformed one here, because
// there is no `;`-style sequencing yet (docs/roadmap.md). A `&` in the
// middle stays in the line and reaches the program as an argument,
// which is wrong but VISIBLY wrong -- silently splitting on it would
// run half of what was typed.
static int strip_background(char *s) {
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && s[n - 1] == ' ') n--;
    if (n == 0 || s[n - 1] != '&') return 0;
    s[n - 1] = '\0';
    // ...and the spaces before it, so the label a job is listed under
    // is `sleep 5` rather than `sleep 5 `.
    for (n--; n > 0 && s[n - 1] == ' '; n--) s[n - 1] = '\0';
    return 1;
}

int tosh_run_line(struct tosh *sh, const char *line) {
    // A MUTABLE copy: redirection is stripped off the line in place,
    // and the caller's buffer is not ours to edit (the GUI Terminal
    // hands us its editor's live buffer).
    char work[TOSH_PATH_MAX];
    scopy(work, line, TOSH_PATH_MAX);

    // BEFORE the redirection parse, because `&` comes after `> file`
    // and stripping it first is what leaves an ordinary line behind.
    int background = strip_background(work);

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
    // THE LABEL IS TAKEN BEFORE THE SPLIT, because split_stages() cuts
    // the line in place -- every `|` becomes a NUL, so afterwards
    // `work` is only the first stage and a job would be listed under a
    // third of its own name.
    char label[TOSH_JOB_CMD_MAX];
    scopy(label, work, TOSH_JOB_CMD_MAX);

    struct tosh_stage st[TOSH_STAGE_MAX];
    int nst = split_stages(sh, work, st);
    int code;
    if (nst < 0) {
        code = -1;
    } else if (nst > 1) {
        code = run_pipeline(sh, st, nst, label, background, r.out_fd >= 0);
    } else {
        code = run_stripped(sh, work, background, r.out_fd >= 0);
    }

    sh->out = saved_out;
    sh->ctx = saved_ctx;
    redir_undo(&r);
    return code;
}
