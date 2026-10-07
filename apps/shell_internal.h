// Internal, sharing-only header for the shell's split into five files
// -- shell.c (the REPL loop, dispatch(), and the state everything else
// here shares), shell_fs.c (filesystem commands), shell_sys.c
// (system-info/settings commands), shell_path.c (PATH lookup and
// the shared "run this name" resolver), and shell_rescue.c (the
// kernel-side file-command copies behind `rescue`). Split out once shell.c crossed 900
// lines mixing every command category together (see the git history for
// the build this happened in).
//
// Mirrors userland/wm/wm_internal.h's pattern deliberately: this is still
// fundamentally one component (the shell), split by concern for
// readability the same way wm.c was split into userland/wm/, not a real
// boundary the way kapi.h/wm.h are -- see docs/decisions.md's "window
// manager is one event loop, not decoupled components" entry, which
// applies here unchanged. State is shared through plain `extern`s, not
// hidden behind accessor functions, on purpose. Not included by
// anything outside shell.c/shell_fs.c/shell_sys.c/shell_path.c.
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include "kapi.h"

// The command line the shell reads, dispatches and remembers. It was
// 128 and matched the editor's fixed buffer; the editor grows now, so
// this is what decides how long a command can actually BE -- a line
// longer than this would be typed, shown, and then run truncated.
#define LINE_MAX 1024
#define HISTORY_MAX 8

// Shared shell-wide state -- defined in shell.c, the file that owns
// the REPL loop and is the only place any of these actually change.
extern enum vga_color shell_fg;
extern char cwd[FS_PATH_MAX];
// ALLOCATED PER ENTRY, so a long command comes back from Up as the
// command that was typed. It was a fixed HISTORY_MAX x LINE_MAX array,
// and once the line editor stopped being bounded at 128 that became the
// worst kind of limit -- a command that ran correctly and then recalled
// SHORTER, silently, as a different command. An entry that cannot be
// allocated is not stored; a slot may therefore be NULL.
extern char *history[HISTORY_MAX];
extern int history_count;

// Resolves `input` (absolute if it starts with '/', otherwise relative
// to `cwd`) into a normalized absolute path in `out` -- see shell.c's
// own doc comment above its definition for the full contract. Defined
// in shell.c since it's the file that owns `cwd`; every filesystem
// command in shell_fs.c calls this before touching fs_*.
int resolve_path(const char *input, char *out);

// Runs `name` as a console app (apps.c's registry) or a PATH binary,
// with `args` passed through verbatim -- the single resolver behind
// both a bare typed name (dispatch()) and an explicit `run <name>`.
// Returns 1 if something was found and run, 0 if the name resolved to
// nothing. Defined in shell_path.c.
//
// `report` non-zero prints "Process finished. Exit code: N" afterwards.
// `run` passes 1 (it is the explicit form, and the exit code is what
// tools/usertest_run.py asserts on); a bare typed name passes 0, since
// a shell that announced itself after every `rm` would bury the output
// you asked for.
int shell_exec_name(const char *name, const char *args, int report);
void cmd_path(void);

// `rescue <cmd> [args...]` -- the kernel's own copies of the file
// commands, for a disk whose /bin is damaged. Defined in
// shell_rescue.c, which is also where the set of them is listed.
void cmd_rescue(const char *args);

// One builtin: the shell's own command, never a /bin program. `args` is
// the rest of the line, "" when there is none -- never NULL.
struct shell_builtin {
    const char *name;
    void (*fn)(const char *args);
};
extern const struct shell_builtin SHELL_BUILTINS[];  // shell.c
extern const int SHELL_BUILTIN_COUNT;
const struct shell_builtin *shell_builtin_find(const char *name);

// Is `name` one of the commands `rescue` carries? dispatch() asks so
// that a name whose /bin program is MISSING gets told where the kernel
// copy is, rather than a flat "unknown command".
int shell_rescue_has(const char *name);

// Filesystem commands -- defined in shell_fs.c.
void cmd_cat(const char *name);
void cmd_touch(const char *name);
void cmd_mkdir(const char *name);
void cmd_write_or_append(const char *args, int append);
void cmd_rm(const char *name);
void cmd_pwd(void);
void cmd_cd(const char *args);
void cmd_stat(const char *name);

// System-info/settings commands -- defined in shell_sys.c.
void cmd_help(const char *args);
void cmd_beep(void);
void cmd_df(void);   // shell_sys.c -- only reachable through `rescue df` now
void cmd_fsck(const char *args);
void cmd_sync(const char *args);
void cmd_fsformat(const char *args); // shell_fs.c -- destructive, physical shell only
void cmd_ln(const char *args);       // shell_fs.c -- hardlink, refuses on FS_CAP_HARDLINKS-less backends
void cmd_mv(const char *args);       // shell_fs.c -- rename/move; never overwrites
void cmd_truncate(const char *args); // shell_fs.c -- set a file's size exactly; grows sparsely
void cmd_ktest(const char *args);
void cmd_fputest(void);
void cmd_stress(const char *args);
void cmd_dmatest(const char *args);
void cmd_steptest(const char *args);
void cmd_dmesg(void);
void cmd_apps(void);
void cmd_run(const char *name);
void cmd_cursor(const char *args);
void cmd_fontsize(const char *args);
void cmd_fontface(const char *args);
void cmd_keyboard(const char *args);
void cmd_color(const char *args);
void cmd_history(void);
void cmd_debug(const char *args);
void cmd_gfxbench(const char *args);
void cmd_hwcursor(const char *args);

#endif
