# dash

**a `/bin` program.**

**Category:** The shell and the console

## Synopsis

    dash [-c command] [script] [argument ...]

## Options

- `-c` *command* -- run *command* and exit, instead of reading a script
  or the terminal.
- `-s` -- read from standard input even when arguments were given.
- `-i` -- interactive: prompt, and catch the signals a shell must.
- `-l` -- act as a login shell.
- `-e`, `-x`, `-u`, `-v`, `-n`, `-f`, `-m`, `-b`, `-C` -- the standard
  `set` options, settable from the command line as every shell allows.

## Description

The Debian Almquist Shell, vendored from upstream and built for toy-os.
It is a POSIX shell: pipelines, redirection, here-documents, functions,
parameter expansion, arithmetic, job control, `trap`, and the builtins
POSIX requires.

**`tosh` IS STILL THE DEFAULT, and dash is a second shell rather than a
replacement.** Nothing starts it: the `tosh` service unit, the GUI
Terminal, `telnetd` and libc's `system()` all name `/bin/tosh`, and a
`system.shell` setting to change that is a separate roadmap item. Run
it by name to use it.

**It has no line editing.** Debian builds dash without libedit and so
does this port, which means no arrow keys, no history and no Tab
completion at an interactive prompt -- a downgrade from `tosh`, whose
editor is `kernel/lib/klineedit.c` compiled twice. Wiring that editor
into dash's one read-a-line seam is what would fix it, and is the point
of there being exactly one line editor in this tree.

## How it is built

**Six generators run on the HOST**, which is most of what makes this a
port rather than a compile: dash generates its parser tables, node
layout, builtin dispatch table and initialiser from its own sources.
Four are C programs built for the build machine; two are shell scripts.

**`signames.c` is OURS, not dash's.** Upstream generates it with
`src/mksignames.c`, which is GPL-2 from GNU Bash -- the one non-BSD file
in the vendored tree -- and which reads the HOST's `<signal.h>`. Either
alone would be reason enough to replace it: the licence, because its
output is linked; and the header, because a dash built that way would
list Linux's signals rather than this kernel's.
`tools/gen_signames.py` reads `kernel/include/abi/signal_abi.h` instead.

**`userland/backends/dash/config.h` is the port, almost in its
entirety.** dash configures itself through autoconf, and every probe
lands there as one `#define`; porting it was mostly deciding what to
answer. `tools/dash_gap.py` reads that same file, so the measurement and
the build cannot disagree.

## What is not done

**It has not been proven to run a real script.** `dash -c pwd` prints
`/`, `dash -c false` exits 1, and a missing script file is reported in
dash's own voice -- so it parses, executes and reports correctly. What
has not been exercised is pipelines, job control, here-documents or
anything long. `tosh` remains the shell the system actually uses.
