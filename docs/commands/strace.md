# strace

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    strace [-c] [-f] [-o FILE] [-e LIST] PROGRAM [ARG]...

## Description

Linux-style syscall tracing: runs `PROGRAM` and prints one line per syscall it makes, then `+++ exited with N +++`, and exits with `PROGRAM`'s status:

    open("/etc/motd", 0) = -2 ENOENT
    write(1, "hello\n", 6) = 6
    waitpid(23, 0x807ff00e10, 0) <unfinished ...>
    <... waitpid resumed> = 23
    exit(0) = ?
    +++ exited with 0 +++

**The trace goes to stderr**, as real strace's does, so `strace foo | grep x` greps `foo`'s output and never the trace, and `strace foo 2> trace.txt` keeps it. Started with `spawn`, a program's stderr is the kernel log, so `spawn /bin/strace foo` leaves the trace in `dmesg`.

**What a line shows.** The arguments as the kernel saw them at the call: a path or a buffer as the bytes it held then (up to 80, `...` after a longer one), a pointer the kernel could not read as the pointer. A failed call's value is the errno and its name (`-2 ENOENT`). A call that waited -- a `read` on an empty pipe, `waitpid` -- prints `<unfinished ...>` when something else is traced in between, and its value later as `<... name resumed>`. A wait that is interrupted and goes again shows `= ? RETRY (re-issued)`, followed by the second call's own line.

**How it works**: the kernel writes each call's entry and exit as a record into a ring this program created (`SPAWN_TRACE` with `SPAWN_TRACE_RING`, `abi/trace_abi.h`), and `strace` decodes them with `lib/utrace.h` -- FreeBSD's `ktrace` and `kdump` in one program. **A full ring slows the traced program rather than losing calls**: the kernel holds the next call until `strace` has caught up.

## Options

- `-c`, `--summary` -- count calls and failed calls per syscall and print only that table at the end, most-called first (Linux's `strace -c`, without its time columns: the records carry no times).
- `-f`, `--follow` -- trace `PROGRAM`'s children too -- those it spawns and those it forks, and theirs -- into the same trace. A child's lines start `[pid N] `; `PROGRAM`'s own are unmarked. `strace` then waits until the last of them has exited, not only `PROGRAM`:

        waitpid(24, 0x807ff00e10, 0) <unfinished ...>
        [pid 24] open("/etc/motd", 0) = 3
        [pid 24] exit(0) = ?
        <... waitpid resumed> = 24

- `-o FILE`, `--output FILE` -- write the trace to `FILE` instead of stderr.
- `-e LIST`, `--trace LIST` -- only the syscalls named in `LIST`, comma-separated: `-e trace=open,read` or `-e open,read`. An unknown name is refused.

Options end at `PROGRAM`: `strace ls -l` gives `-l` to `ls`.

## Limits

**Tracing is a property of the spawn** rather than something you attach to a running program: `strace` cannot attach (a later stage of `docs/trace-design.md`). Without `-f` a program's children run untraced. Several `strace`s may run at once; past the kernel's few trace slots a new one is refused (`device or resource busy`) rather than run untraced.

**Not reachable from the kernel shell with a bare name.** `strace` needs a scheduler slot of its own (it spawns and waits), which the legacy `run` loader does not have -- at a `#` prompt use `spawn /bin/strace PROGRAM`. At a `$` prompt, in `/bin/tosh` or a Terminal window, it is an ordinary command.
