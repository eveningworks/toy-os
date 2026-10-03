# head

**a `/bin` program.**

**Category:** Text processing

## Synopsis

    head [-n <lines> | -<lines> | -c <bytes>] [<file>...]

## Description

Prints the first ten lines of each file, or of stdin with no file (`-`
also means stdin). `-n` sets the count, `-<lines>` is the old spelling
of the same thing (`head -40`), and `-c` counts bytes instead.

**It stops reading at the count.** That is what makes `dmesg | head`
cheap: once `head` exits, the writer's next write fails and it dies of
SIGPIPE, which the shell does not report.

Given several files, each is introduced by `==> name <==`, as in GNU
head. A count that is not a plain decimal number (`10k`, `-n x`) is
refused rather than read as its leading digits.
