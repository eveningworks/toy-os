# run

**a shell builtin.**

**Category:** Processes and programs

## Synopsis

    run <name> [args...]

## Description

Runs a program by name -- the explicit form of just typing the name.
Both go through ONE resolver (`shell_exec_name()`), so they can never
disagree about what a name means.

**The one deliberate difference is reporting.** `run` prints
`Process finished. Exit code: N` afterwards; a bare name prints nothing
on success and one terse line on failure. `run` is the demonstrative
form, and that exit code is the entire assertion
`tools/usertest_run.py` makes -- see `docs/conventions/shell.md`.

**Use `spawn`, not `run`, for anything that blocks or paces itself.**
The legacy loader `run` uses has no scheduler slot, so `SYS_SLEEP`
fails under it and anything reaching the window server is refused.