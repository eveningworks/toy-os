# wc

**a `/bin` program.**

**Category:** Text processing

## Synopsis

    wc [-l] [-w] [-m] [-c] [<file>...]   (no flag: -l -w -c)

## Description

Counts lines (`-l`), words (`-w`), characters (`-m`) and bytes (`-c`)
in each file, or in stdin with no file. With no flag it prints lines,
words and bytes. The columns always come in that order -- lines, words,
characters, bytes -- whatever order the flags were given in, and each is
seven wide (BSD's layout), so a column of files lines up. Several files
get a `total` line.

A **word** is a run of non-space bytes, as POSIX defines it. A
**character** is a UTF-8 code point, so `-m` and `-c` differ only for
text outside ASCII: `äö` is 2 characters and 4 bytes.

A line is counted by its newline, so a last line with none is not one
-- input ending in `a` with no newline is 0 lines, as on every Unix.
