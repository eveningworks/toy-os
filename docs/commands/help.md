# help

**a shell builtin.**

## Synopsis

    help | help tests

## Description

Lists the everyday commands, grouped the way this reference is grouped.
`help tests` lists the developer/diagnostic ones instead -- the `*test`
commands and the disk exercises, which are meant to be read through
their output rather than used for everyday work.

**It is a hand-maintained list, and that is its weakness.** Most of what
it used to describe are `/bin` programs now, which is why the everyday
listing names them in one block and leaves the detail to their own
pages. A command added to `/bin` gains tab-completion automatically
(the completer walks `PATH`) and does NOT gain a `help` line.
