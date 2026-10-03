# ls

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    ls [OPTION]... [DIR]

## Options

- `-l` -- long form: type, size and modification time before each name.
- `-C` -- multi-column; ignored when `-l` is given.
- `-1` -- one entry per line, which is the default.
- `-h` -- human-readable sizes, in the long form.
- `-t` -- newest first.
- `-S` -- largest first.
- `-r` -- reverse the order.
- `-R` -- recurse, each directory under its own header.
- `-a`, `-F` -- accepted and do nothing: there is no dotfile convention
  here, and the trailing `/` on a directory is unconditional.
- `--color=never`, `--color=always`, `--color=auto` -- whether to colour
  directories. `auto` is the default.
- `--help` -- every option and what it does. Only the long form: `-h`
  is human-readable sizes, as in GNU `ls`.

## Description

**With no argument it lists the current directory.** It asks
`SYS_GETCWD` up front rather than passing `.` down, so `-R` headers and
joined child paths stay absolute. Sorted by name, one entry per line,
directories coloured.

Colour is ANSI escapes the terminal parses, so it survives a pipe and
can be turned off. **`--color=auto` is the default**: coloured to a
terminal, plain to a pipe or a file, decided by `sys_isatty(1)` over
`SYS_FSTAT`'s `SYS_STAT_TTY`, which the console and a pty both answer.

A real disk-hosted `/bin/ls` binary; `rescue ls` is the only ring-0
listing left.
