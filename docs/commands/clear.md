# clear

**a shell builtin.**

## Synopsis

    clear

## Description

Clears the console. A builtin because it is the console's state, not a
file or a process -- the same reason `color`, `cursor` and `fontsize`
are builtins.

`Ctrl-L` does the same thing without losing the line you are typing.
