# gui

**a shell builtin.**

## Synopsis

    gui

## Description

Starts the desktop, which is a RING-3 PROCESS: it spawns
`/bin/wm/system/toywm` and waits for it.

On a `graphical` boot target init has already started one, and `gui`
refuses rather than starting a second -- only one process can hold the
compositor role. `gui3` is an alias kept so older notes resolve.

Esc does not close anything; `Exit to shell` in the desktop's own menu
returns here. See `docs/conventions/gui.md`.
