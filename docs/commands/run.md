# run

**a shell builtin.**

**Category:** Processes and programs

## Synopsis

    run <name> [args...]

## Description

Runs a program by name through the LEGACY blocking loader -- the
explicit, demonstrative form. A bare name RESOLVES identically (one
resolver, `shell_exec_name()`) but is SPAWNED as a real scheduled
process and waited on, which is what lets it be a dynamic executable:
every `/bin` program links `/lib/libc.so` now, and the legacy loader
refuses `PT_INTERP` by name. So `run cat` says "use spawn" where a
bare `cat` works -- `run` still loads the static `/tests` binaries,
which is what the test harness drives it for.

**The other deliberate difference is reporting.** `run` prints
`Process finished. Exit code: N` afterwards; a bare name prints nothing
on success and one terse line on failure. That exit code is the entire
assertion
`tools/usertest_run.py` makes -- see `docs/conventions/shell.md`.

**Use `spawn`, not `run`, for anything that blocks or paces itself.**
The legacy loader `run` uses has no scheduler slot, so `SYS_SLEEP`
fails under it and anything reaching the window server is refused.