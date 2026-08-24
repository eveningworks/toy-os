# edit

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    edit [-n] <file>

## Description

A full-screen nano-style editor. Arrows, Home/End, Backspace and Delete
navigate and edit; **F2 saves** and **F3 exits** (Esc also exits). A file
that does not exist yet opens empty and is created on the first save.

**Where you are is always on the status bar** -- `Ln 42, Col 7`, 1-based
because every editor and every error message that will ever name a line
to you counts from 1. It costs no horizontal space, which is the whole
reason it is unconditional.

**`-n` adds a gutter of line numbers** down the left. That one costs
columns an 80-column terminal has few of, so it is opt-in. The width is
derived from the file's line count rather than fixed, and the numbers
are right-aligned, so the text does not shift sideways as you cross line
100. A WRAPPED row is blank in the gutter rather than numbered -- a
continuation is not a new line, and numbering it would make the file
look longer than it is, which is what `vim` and `nano -l` also do.

The two answer different questions, which is why both exist: the status
bar says where the caret is, and the gutter says which line any row on
screen is -- the question you have when something else named a line
number at you.

**It was a kernel builtin until 2026-08-22.** Nothing about editing a
file needs ring 0, and everything the old one took from there now exists
as a terminal: it draws with ANSI escapes on fd 1 instead of
`vga_putc()`, asks `SYS_TCGETWINSZ` how big the screen is instead of
reading `vga_rows()`, and reads a raw fd 0 instead of calling
`keyboard_getchar()`. Moving it emptied `apps/ui/` -- the kernel image
contains no widget code at all now.

**The text model is `utext`, the same one Notepad uses**, so Ctrl+A,
Shift+arrows, typing-replaces-selection and Backspace-over-a-selection
mean the same thing in both. That is one implementation, not two that
agree: `utext.h` records that Notepad hand-wrote sixty lines of this
before the shared core existed, and that a second editor would have
written them again, differently.

## What it does not do

**It needs a terminal that can ADDRESS its screen**, and both of this
OS's terminals now can: the physical console and a Terminal window, which
gained a cursor-addressable grid for exactly this. Both resolve the
escapes through `kernel/lib/ansi.c` -- one parser, compiled twice.

What a window still lacks is an ALTERNATE SCREEN. A real terminal
switches to a separate buffer for a full-screen program and switches back
afterwards, so the shell's transcript is exactly as it was; here the
editor's output goes into the scrollback and pushes the transcript up.
Roadmap item.

**`nano` is gone as a name.** It was an alias on the kernel builtin, and
an alias for a `/bin` program would be a second copy of the binary or a
hard link to maintain. Type `edit`.

No syntax highlighting, no search, no undo beyond what the shared edit
core carries, and one buffer at a time.
