#ifndef USH_H
#define USH_H

// ush -- a shell that runs in ring 3. See ush.c for what that means and
// why it is not a front-end to the kernel's own shell.
//
// A LIBRARY, not a program, because its first caller is a GUI terminal
// that owns its own event loop and cannot sit blocked in a read() of
// its own. `ush_run_line()` runs exactly one command line and returns;
// output goes to a caller-supplied sink rather than straight to stdout,
// so a terminal can put it in a window and a standalone shell can write
// it to fd 1, with no difference in the shell itself.

#define USH_PATH_MAX 64 // FS_PATH_MAX

// Receives output as it is produced -- streamed, not accumulated, so a
// long-running program's output appears while it runs rather than all
// at once when it exits.
typedef void (*ush_out_fn)(void *ctx, const char *text, int len);

struct ush {
    char cwd[USH_PATH_MAX];
    ush_out_fn out;
    void *ctx;
    int last_status; // exit code of the last external command
};

void ush_init(struct ush *sh, ush_out_fn out, void *ctx);

// Runs one command line. Builtins are handled in-process; anything else
// is looked up on PATH and SPAWNED, with its stdout piped back through
// the sink. Returns the command's exit status (0 for builtins).
//
// Blocks for the duration of an external command -- which is fine, and
// is the point: this is a separate process, so the desktop keeps
// running while it waits.
int ush_run_line(struct ush *sh, const char *line);

#endif
