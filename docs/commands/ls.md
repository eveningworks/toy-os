# ls

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    ls [flags] [dir]

## Description

**With no argument it lists the current directory.** It defaulted to `/` until 2026-08-20, and nothing noticed because a builtin wrapper resolved the cwd and passed it in — one command with two halves in two rings, where the ring-3 half was wrong on its own and could not be run on its own. It asks `SYS_GETCWD` up front rather than passing `.` down, so `-R` headers and joined child paths stay absolute. Sorted by name, one entry per line, directories coloured. `-l` type/size/mtime, `-h` human sizes, `-C` columns, `-1` one per line, `-t` newest first, `-S` largest first, `-r` reverse, `-R` recurse, `--color=never\ | always\ | auto`; `-a`/`-F` accepted as no-ops (no dotfile convention, no mode bits). Colour is ANSI escapes the terminal parses, so it survives a pipe and can be turned off. **`--color=auto` is the DEFAULT since 2026-08-22**: coloured to a terminal, plain to a pipe or a file, decided by `sys_isatty(1)` over `SYS_FSTAT`'s `SYS_STAT_TTY` — which became answerable when a terminal became a real object with both the console and a pty saying yes. A real disk-hosted `/bin/ls` binary; `rescue ls` is the only ring-0 listing left. **tosh's builtin `ls` shadowed it until 2026-08-22**, which is why `ls -l` in a Terminal window used to answer `ls: cannot read -l` — the flags and the colour were always here, behind a builtin that had neither.