# cat

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    cat [f...]

## Description

Copies files, or STDIN with no argument, to stdout. Streams in fixed chunks rather than reading a file whole, so file size is irrelevant. A missing file is reported and the remaining ones still print, with a non-zero exit.

**`cmd | cat` works because there is no builtin `cat` any more.** tosh
had one, and it required a filename -- so a pipeline into `cat` printed
an error instead of its input, which is the one thing `cat` is most
often asked to do. This program is what runs now, in both shells.

**With no argument it BLOCKS on fd 0, which is correct and is not a
hang.** In `/bin/tosh` at the console that means it waits for you to
type, ending at Ctrl-D. In the GUI Terminal, which owns no console, the
shell hands it an empty stdin instead, so it reads EOF at once and
prints nothing -- rather than freezing the window on a keyboard it can
never be given.