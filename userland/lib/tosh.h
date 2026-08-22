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

// Receives output as it is produced -- streamed, not accumulated, so a
// long-running program's output appears while it runs rather than all
// at once when it exits.
typedef void (*tosh_out_fn)(void *ctx, const char *text, int len);

struct tosh {
    // No `cwd` here: the current directory belongs to the PROCESS and
    // lives in the kernel (SYS_CHDIR/SYS_GETCWD), so a path means the
    // same thing to this shell's builtins and to anything it spawns.
    // A copy here could only ever disagree with it.
    tosh_out_fn out;
    void *ctx;
    int last_status; // exit code of the last external command

    // **MAY A CHILD INHERIT THIS SHELL'S fd 0?** 0 by default, and the
    // front end says otherwise.
    //
    // `/bin/tosh` owns the physical console -- reading fd 0 is its whole
    // existence -- so a child it spawns should read the same keyboard.
    // The GUI TERMINAL does not: it takes keys as window events, and its
    // fd 0 is a console some other process owns (or nobody does). A
    // child left to inherit that blocks forever on a keyboard it will
    // never be given, and since the shell waits for its child, the
    // window stops responding -- a hang, not a wrong answer.
    //
    // So a shell with no terminal input to give hands its children an
    // EMPTY one: a pipe whose write end is closed, which reports EOF at
    // once. That is what a process with no controlling terminal gets on
    // a real system, and it is what makes `cat` with no arguments print
    // nothing in the GUI Terminal instead of freezing it.
    int stdin_ok;
};

void tosh_init(struct tosh *sh, tosh_out_fn out, void *ctx);

// Runs one command line. Builtins are handled in-process; anything else
// is looked up on PATH and SPAWNED, with its stdout piped back through
// the sink. Returns the command's exit status (0 for builtins).
//
// Blocks for the duration of an external command -- which is fine, and
// is the point: this is a separate process, so the desktop keeps
// running while it waits.
int tosh_run_line(struct tosh *sh, const char *line);

#endif
