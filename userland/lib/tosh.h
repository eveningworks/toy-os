#ifndef TOSH_H
#define TOSH_H

// tosh -- the toy-os shell (t + OS + h), running in ring 3. See tosh.c
// for what that means and why it is not a front-end to the kernel's
// own shell.
//
// The NAME belongs to the shell language and behaviour, not to this
// file: the kernel-side command line (apps/shell.c) is the same shell
// by a different front end, the way `sh` names a language rather than
// one binary. It is not the TERMINAL -- `uterm` is the terminal
// emulator, and conflating the two is a distinction real systems keep.
// See docs/decisions.md for the name and the collisions it dodges.
//
// A LIBRARY, not a program, because its first caller is a GUI terminal
// that owns its own event loop and cannot sit blocked in a read() of
// its own. `tosh_run_line()` runs exactly one command line and returns;
// output goes to a caller-supplied sink rather than straight to stdout,
// so a terminal can put it in a window and a standalone shell can write
// it to fd 1, with no difference in the shell itself.

#define TOSH_PATH_MAX 64 // FS_PATH_MAX

// THE WHOLE COMMAND LINE, which is not a path and was sized as one.
// tosh_run_line() staged the line in a TOSH_PATH_MAX buffer, so a
// command over 63 characters was silently cut -- measured: `echo x >
// /probe_a.txt` ran and a 70-character line did not. A command line is
// as long as a person types plus what completion inserts, and paths in
// it are bounded separately by TOSH_PATH_MAX.
#define TOSH_CMD_MAX 1024

// The most words (arguments plus operators) one line may lex into. A
// line of TOSH_CMD_MAX bytes cannot hold more than half that in
// one-character words; this is the bound the parser's arrays carry.
#define TOSH_WORD_MAX 128

// One lexed token: a word (`text` into `struct tosh.unq`, unquoted) or
// an operator (`text` NULL). See tosh.c's parser section.
struct tosh_word {
    char *text;
    unsigned char kind;
};

// Receives output as it is produced -- streamed, not accumulated, so a
// long-running program's output appears while it runs rather than all
// at once when it exits.
typedef void (*tosh_out_fn)(void *ctx, const char *text, int len);

struct tosh {
    // No `cwd` here: the current directory belongs to the PROCESS and
    // lives in the kernel (SYS_CHDIR/SYS_GETCWD), so a path means the
    // same thing to this shell's builtins and to anything it spawns.
    // A copy here could only ever disagree with it.
    // **A CHILD INHERITS THIS SHELL'S fd 0, unconditionally.** There
    // used to be a `stdin_ok` flag and an empty-pipe fallback here,
    // because the GUI Terminal took keys as WINDOW EVENTS and its fd 0
    // was a console some other process owned -- a child left to inherit
    // that blocked forever on a keyboard it would never be given, and
    // the window hung. That is gone: the Terminal opens a pty and runs
    // /bin/tosh on the slave, so every shell using this library is on a
    // real terminal and its children should have it. See
    // docs/tty-design.md.
    tosh_out_fn out;
    void *ctx;
    int last_status; // exit code of the last external command
    // The first line of `help`, or NULL: the PROGRAM's, since it knows
    // the build -- this library is in libuapp.so, and a version compiled
    // in here would change the library on every commit.
    const char *banner;

    // The parser's scratch, IN THE OBJECT rather than on the stack: a
    // ring-3 frame is budgeted at 2 KiB and these are 2.5 KiB. One line
    // is parsed at a time, so there is nothing to be re-entrant about.
    char unq[TOSH_CMD_MAX];                // the words, unquoted
    struct tosh_word words[TOSH_WORD_MAX];
};

void tosh_init(struct tosh *sh, tosh_out_fn out, void *ctx);

// **A FAILED LINE EXITS LIKE A SHELL, IN 0..255.** 127 is "no such
// command" and 126 "found but not runnable" in every Bourne descendant;
// 2 is a syntax error and 1 an ordinary shell error. They were a bare
// -1, which `tosh -c` handed to the kernel as its exit code -- and a
// negative one there is PROCESS_CRASHED, so a mistyped command reported
// the shell as having faulted.
#define TOSH_ST_ERROR      1
#define TOSH_ST_SYNTAX     2
#define TOSH_ST_NOEXEC   126
#define TOSH_ST_NOTFOUND 127

// Runs one command line. Builtins are handled in-process; anything else
// is looked up on PATH and SPAWNED, with its stdout piped back through
// the sink. Returns the command's exit status (0 for builtins).
//
// Blocks for the duration of an external command -- which is fine, and
// is the point: this is a separate process, so the desktop keeps
// running while it waits.
int tosh_run_line(struct tosh *sh, const char *line);

// Reports every background job that has finished or stopped since the
// last time it was asked, then forgets the finished ones.
//
// **THE FRONT END CALLS THIS IMMEDIATELY BEFORE EVERY PROMPT.** Not
// from tosh_run_line(), because a bare Enter draws a prompt without
// running a line, and because output landing in the middle of a
// running command is exactly what this timing exists to avoid. Cheap
// when there are no jobs, which is the normal case.
void tosh_reap_jobs(struct tosh *sh);

#endif
