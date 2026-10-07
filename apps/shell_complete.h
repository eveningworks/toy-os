#ifndef SHELL_COMPLETE_H
#define SHELL_COMPLETE_H

// The KERNEL SHELL's completion environment -- its builtin names (read
// from shell.c's SHELL_BUILTINS, the table dispatch() runs), its
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

#endif
