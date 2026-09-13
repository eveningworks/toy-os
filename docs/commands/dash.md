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

**Line editing is `set -o emacs`, and then it is `tosh`'s editor.**
Debian builds dash without libedit, so upstream has no editing at all;
this port answers libedit's names with `kernel/lib/klineedit.c` instead
(`userland/backends/dash/histedit_shim.c`), which is the same editor
`tosh`, the GUI Terminal and the physical console use. Arrow keys,
history, the kill ring and the word motions all behave as they do
everywhere else in this system, because they ARE the same code.

It is opt-in because upstream makes it so -- `Eflag` starts clear and
dash builds its editor only once `set -o emacs` (or `-o vi`, which gets
the same editor here, there being only one) sets it. Put it in the
shell's startup file to have it always.

**Tab completion is not wired up.** The editor reports the keystroke and
nothing answers it yet; `tosh` fills that in from
`kernel/lib/completion.c`, and dash could use the same engine.

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

## Running a script by name

A file starting with `#!/bin/dash` runs as a command in its own right --
`/tmp/build.sh arg` rather than `dash /tmp/build.sh arg`. **That is the
LOADER's doing, not this shell's** (`build_image()` in
kernel/proc/scheduler.c, the position Linux's `binfmt_script` takes), so
it works the same from `tosh`, from `execve()` and from a bare name at
either prompt. The interpreter receives the script's path as its first
argument and `$0` is the script, as on every Unix.

One optional argument may follow the interpreter -- `#!/bin/dash -u` --
and it arrives as a SINGLE argument, so `-e -u` would be one word rather
than two. Again standard, and again not this shell's choice.

## What is not done

**Job control is unproven.** `tools/dash_test.py` drives 22 cases --
pipelines, redirection, here-documents, functions, parameter expansion,
arithmetic, `case`, loops, command substitution, `test`, `trap`, exit
status, and three `#!` scripts run by name -- and they pass. What that
harness cannot give dash is a terminal, so `fg`/`bg`/`jobs` and the
signal handling around them are not exercised at all.

`tosh` remains the shell the system actually uses.
