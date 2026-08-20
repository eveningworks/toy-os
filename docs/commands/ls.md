# ls

**a `/bin` program.**

## Synopsis

    ls [flags] [dir]

## Description

**With no argument it lists the current directory.** It defaulted to `/` until 2026-08-20, and nothing noticed because a builtin wrapper resolved the cwd and passed it in — one command with two halves in two rings, where the ring-3 half was wrong on its own and could not be run on its own. It asks `SYS_GETCWD` up front rather than passing `.` down, so `-R` headers and joined child paths stay absolute. Sorted by name, one entry per line, directories coloured. `-l` type/size/mtime, `-h` human sizes, `-C` columns, `-1` one per line, `-t` newest first, `-S` largest first, `-r` reverse, `-R` recurse, `--color=never\ | always`; `-a`/`-F` accepted as no-ops (no dotfile convention, no mode bits). Colour is ANSI escapes the console parses, so it survives a pipe and can be turned off — there is no `--color=auto` because nothing can yet ask whether an fd is a terminal. A real disk-hosted `/bin/ls` binary with no builtin in front of it; `rescue ls` is the only ring-0 listing left.
