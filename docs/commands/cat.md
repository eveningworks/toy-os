# cat

**a `/bin` program.**

## Synopsis

    cat [f...]

## Description

Copies files, or STDIN with no argument, to stdout. Streams in fixed chunks rather than reading a file whole, so file size is irrelevant. A missing file is reported and the remaining ones still print, with a non-zero exit.
