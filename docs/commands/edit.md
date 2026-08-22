# edit

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    edit <file>

## Description

A full-screen nano-style editor. Arrows, Home/End, Backspace and Delete
navigate and edit; **F2 saves** and **F3 exits** (Esc also exits). A file
that does not exist yet opens empty and is created on the first save.

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

**It needs a terminal that can ADDRESS its screen**, which today means
the physical console. A full-screen program says "put the caret at row 4,
column 12", and that needs a grid; the GUI Terminal's screen is a
character stream in a scrollback, which is right for a shell transcript
and cannot express moving back up. Running `edit` in a Terminal window
prints the escape sequences instead of obeying them. Giving that window a
cursor-addressable grid is a roadmap item, and it is the same item as the
alternate screen buffer.

**`nano` is gone as a name.** It was an alias on the kernel builtin, and
an alias for a `/bin` program would be a second copy of the binary or a
hard link to maintain. Type `edit`.

No syntax highlighting, no search, no undo beyond what the shared edit
core carries, and one buffer at a time.
