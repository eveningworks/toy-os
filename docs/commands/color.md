# color

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    color <name>

## Description

Sets the shell's text colour. One of: black, blue, brown, cyan,
darkgrey, green, lightblue, lightcyan, lightgreen, lightgrey,
lightmagenta, lightred, magenta, red, white, yellow.

A builtin because it is the SHELL's own state -- a program could not
change the colour of the prompt that outlives it. Case-sensitive, and
the names are lowercase; nothing has asked for otherwise.

Not the same mechanism as colour in output: `ls` colours its listing
with ANSI escapes the console parses, which survive a pipe. See
`docs/conventions/shell.md`.