# help

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    help | help tests

## Description

**This page is the kernel shell's `help`.** `/bin/tosh` -- the shell in
the Terminal and over `telnetd` -- has its own: its built-ins, the
command-line syntax (pipes, redirection, `&`, quoting), its keys, and
`doc NAME` for a command's full manual, under the line its startup
banner prints (`tosh -- toy-os 0.5.0-dev (2513429a)`, the build it is
on).

Lists the everyday commands, grouped the way this reference is grouped.
`help tests` lists the developer/diagnostic ones instead -- the `*test`
commands and the disk exercises, which are meant to be read through
their output rather than used for everyday work.

**It is a hand-maintained list, and that is its weakness.** Most of the
everyday commands are `/bin` programs, which is why the everyday listing
names them in one block and leaves the detail to their own pages. A
command added to `/bin` gains tab-completion automatically (the
completer walks `PATH`) and does NOT gain a `help` line.