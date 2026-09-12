# truncate

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    truncate <file> <size>  |  truncate -s <size> <file>

## Options

- `-s <size>` -- the size, with the file after it. The spelling every
  other `truncate` takes, accepted alongside this system's positional
  `<file> <size>`.

## Description

Sets a file's size exactly. Growing is sparse, so it costs no blocks.

The size is a plain decimal byte count. There are no `K`/`M`/`G`
suffixes, and a size carrying one is REFUSED rather than read as the
leading digits -- a parser that guesses is worse than one that says no.

Shrinking zeroes what is left of the final partial block, so growing
the file back reads zeros rather than the bytes that were discarded.