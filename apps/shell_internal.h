// Internal, sharing-only header for the shell's split into four files
// -- shell.c (the REPL loop, dispatch(), and the state everything else
// here shares), shell_fs.c (filesystem commands), shell_sys.c
// (system-info/settings commands), and shell_path.c (PATH lookup and
// the shared "run this name" resolver). Split out once shell.c crossed 900
// lines mixing every command category together (see CHANGELOG.md for
// the build this happened in).
//
// Mirrors apps/wm/wm_internal.h's pattern deliberately: this is still
// fundamentally one component (the shell), split by concern for
// readability the same way wm.c was split into apps/wm/, not a real
// boundary the way kapi.h/wm.h are -- see docs/decisions.md's "window
// manager is one event loop, not decoupled components" entry, which
// applies here unchanged. State is shared through plain `extern`s, not
// hidden behind accessor functions, on purpose. Not included by
// anything outside shell.c/shell_fs.c/shell_sys.c/shell_path.c.
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include "kapi.h"

#define LINE_MAX 128
#define HISTORY_MAX 8

// Shared shell-wide state -- defined in shell.c, the file that owns
// the REPL loop and is the only place any of these actually change.
extern enum vga_color shell_fg;
extern char cwd[FS_PATH_MAX];
extern char history[HISTORY_MAX][LINE_MAX];
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
int shell_exec_name(const char *name, const char *args);
void cmd_path(void);

// Filesystem commands -- defined in shell_fs.c.
void cmd_cat(const char *name);
void cmd_touch(const char *name);
void cmd_mkdir(const char *name);
void cmd_write_or_append(const char *args, int append);
void cmd_edit(const char *name);
void cmd_rm(const char *name);
void cmd_pwd(void);
void cmd_cd(const char *args);
void cmd_stat(const char *name);

// System-info/settings commands -- defined in shell_sys.c.
void cmd_help(const char *args);
void cmd_time(void);
void cmd_timezone(const char *args);
void cmd_uptime(void);
void cmd_about(void);
void cmd_beep(void);
void cmd_echo(const char *args);
void cmd_meminfo(void);
void cmd_df(void);
void cmd_fsck(const char *args);
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
void cmd_reboot(void);
void cmd_apps(void);
void cmd_run(const char *name);
void cmd_strace(const char *name_and_args);
void cmd_cursor(const char *args);
void cmd_fontsize(const char *args);
void cmd_keyboard(const char *args);
void cmd_color(const char *args);
void cmd_history(void);
void cmd_lspci(void);
void cmd_parttable(void);
void cmd_ls_bin(const char *args);
void cmd_debug(const char *args);
void cmd_ata(const char *args);

#endif
