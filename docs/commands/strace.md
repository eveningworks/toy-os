# strace

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    strace [-c] [-o FILE] [-e LIST] PROGRAM [ARG]...

## Description

Linux-style syscall tracing: runs `PROGRAM` and prints one line per syscall it makes, then `+++ exited with N +++`, and exits with `PROGRAM`'s status:

    open("/etc/motd", 0) = -2 ENOENT
    write(1, "hello\n", 6) = 6
    waitpid(23, 0x807ff00e10, 0) <unfinished ...>
    <... waitpid resumed> = 23
    exit(0) = ?
    +++ exited with 0 +++

**The trace goes to stderr**, as real strace's does, so `strace foo | grep x` greps `foo`'s output and never the trace, and `strace foo 2> trace.txt` keeps it. Started with `spawn`, a program's stderr is the kernel log, so `spawn /bin/strace foo` leaves the trace in `dmesg`.

**What a line shows.** The arguments as the kernel saw them at the call: a path or a buffer as the bytes it held then (up to 80, `...` after a longer one), a pointer the kernel could not read as the pointer. A failed call's value is the errno and its name (`-2 ENOENT`). A call that waited -- a `read` on an empty pipe, `waitpid` -- prints `<unfinished ...>` when something else is traced in between, and its value later as `<... name resumed>`.

**How it works**: the kernel writes each call's entry and exit as a record into a ring this program created (`SPAWN_TRACE` with `SPAWN_TRACE_RING`, `abi/trace_abi.h`), and `strace` decodes them with `lib/utrace.h` -- FreeBSD's `ktrace` and `kdump` in one program. **A full ring slows the traced program rather than losing calls**: the kernel holds the next call until `strace` has caught up.

## Options

- `-c`, `--summary` -- count calls and failed calls per syscall and print only that table at the end, most-called first (Linux's `strace -c`, without its time columns: the records carry no times).
- `-o FILE`, `--output FILE` -- write the trace to `FILE` instead of stderr.
- `-e LIST`, `--trace LIST` -- only the syscalls named in `LIST`, comma-separated: `-e trace=open,read` or `-e open,read`. An unknown name is refused.

Options end at `PROGRAM`: `strace ls -l` gives `-l` to `ls`.

## Limits

**One traced program at a time** -- a second `strace` while one runs is refused (`device or resource busy`) -- and tracing is a property of the spawn rather than something you attach to a running program. It cannot attach, and does not follow a program's own children; both are later stages of `docs/trace-design.md`.

**Not reachable from the kernel shell with a bare name.** `strace` needs a scheduler slot of its own (it spawns and waits), which the legacy `run` loader does not have -- at a `#` prompt use `spawn /bin/strace PROGRAM`. At a `$` prompt, in `/bin/tosh` or a Terminal window, it is an ordinary command.
