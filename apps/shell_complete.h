#ifndef SHELL_COMPLETE_H
#define SHELL_COMPLETE_H

// The KERNEL SHELL's completion environment -- its builtin names, its
// argument completers, and the filesystem hooks that reach ring 0's
// fs_list()/fs_is_dir().
//
// The engine itself is api/completion.h, shared with `/bin/tosh` and
// compiled into both rings. This file is only what ring 0 knows that
// ring 3 does not: the console app registry, the shell's PATH, and the
// argument sets for commands that exist only at a `#` prompt (`color`,
// `debug`, `fontface`, `timezone`, ...).

#include <stdint.h>
#include "completion.h"

// Fills *out for the word under `cursor` in `line`, using the kernel
// shell's environment. A thin wrapper over completion_run_env().
int completion_run(const char *line, int cursor, struct completion_result *out);

// Every command name the shell dispatches, NULL-terminated. Lives here
// rather than in shell.c because this is the only thing that needs the
// list as data -- dispatch() is a hand-written if/else chain, and
// converting it to a table would mean ~40 wrapper functions for
// handlers whose signatures genuinely differ.
//
// That does mean the list can drift from the dispatcher. One direction
// is self-reporting: dispatch() checks this table before printing
// "Unknown command", so a name listed here but not dispatched says so
// explicitly. The other direction (dispatched but not listed) shows up
// as "tab doesn't complete my new command", which announces itself the
// first time you use it. Adding a shell command means touching both.
extern const char *const COMPLETION_COMMANDS[];

// 1 if `name` is in COMPLETION_COMMANDS. Used by dispatch()'s unknown-
// command branch for the drift check described above.
int completion_is_known_command(const char *name);

#endif
