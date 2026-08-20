# echo

**a `/bin` program.**

**Category:** Appearance and the console

## Synopsis

    echo [-n] <text...>

## Description

Prints its arguments separated by single spaces.

**It honours `-n` and nothing else.** Backslash escapes are the one
part of `echo` that every shell implements differently -- the split
between `echo -e`, bash's builtin and `/bin/echo` is a well-known mess
-- and a parser that guesses is worse than one that does not exist.
Only a LEADING `-n` counts; a later one is text.

It does not collapse whitespace: `argv` arrives already split, so
`echo  a   b` prints `a b`, which is what every Unix echo does.