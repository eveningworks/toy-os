# tail

**a `/bin` program.**

**Category:** Text processing

## Synopsis

    tail [-n <lines> | -n +<from> | -<lines> | -c <bytes>] [<file>...]

## Description

Prints the last ten lines of each file, or of stdin with no file (`-`
also means stdin). `-n` sets the count, `-<lines>` is the old spelling,
`-c` counts bytes, and `-n +<from>` prints from line `<from>` to the end.

**It reads its input once, front to back**, keeping the last lines in a
ring as they go by, so a pipe works exactly as a file does. The cost is
reading the whole of a large file to print its end. A count over
100000 lines is refused rather than allocated for.

**There is no `-f`.** The logs worth following have their own:
`log -f` and `dmesg -w`.

Given several files, each is introduced by `==> name <==`, as in GNU
tail.
