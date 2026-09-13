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

**Line editing is ON, and it is `tosh`'s editor.**
Debian builds dash without libedit, so upstream has no editing at all;
this port answers libedit's names with `kernel/lib/klineedit.c` instead
(`userland/backends/dash/histedit_shim.c`), which is the same editor
`tosh`, the GUI Terminal and the physical console use. Arrow keys,
history, the kill ring and the word motions all behave as they do
everywhere else in this system, because they ARE the same code.

Upstream leaves it OFF until `set -o emacs`; `/bin/dash` here passes
`-E` for you (`userland/bin/dash.c`). That divergence is not taste: every
terminal on this system sends specials as the single bytes 0x91-0xA6, so
a shell reading a line canonically puts them straight into it and the
screen fills with blanks -- an arrow key "types spaces". `set +E` turns
editing off if you want upstream's behaviour, and an explicit `-V` still
selects vi mode, which gets the same editor because there is only one.

**Tab, Ctrl-L, Ctrl-R and Alt-. work, and they are the same code
`tosh` runs.** Tab completes through the one completion engine
(`kernel/lib/completion.c`, behind a `completion_env`), Ctrl-R is the
one reverse-search loop over a `histsearch_env` of callbacks, and
Ctrl-L and Alt-. are shared helpers in `userland/lib/uline.c`. Nothing
here is a second implementation, which is the point.

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

**Job control works, and is tested.** `/tests/dashjobs_test` drives a
real shell on a pty: `&` gives the prompt straight back, `jobs` lists the
job as `[1] Running`, `fg` brings it forward, and a Ctrl-C then kills
THE JOB while the shell prompts again. That last step is the one worth
having -- it is only true if `fg` moved the terminal's foreground group,
which is the `tcsetpgrp()` that needed sessions to work at all.

**`bg` and Ctrl-Z are not covered**, nor is `fc`, which compiles now
(dropping `SMALL` turned on `histedit.c`) and has never been run.

`tosh` remains the shell the system actually uses.
