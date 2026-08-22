# kill

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    kill [-SIGNAL] <pid|-pgid> [pid...]    (`ps` for pids)

## Description

`/bin/kill` sends a **signal** to a process, or to a whole process
group. With no signal named it sends `TERM`, as every Unix does.

The signal may be given by name or by number, with or without a `SIG`
prefix and in any case: `-TERM`, `-SIGTERM`, `-term` and `-15` are the
same request. The signals this kernel has are `INT` (2), `QUIT` (3),
`KILL` (9), `SEGV` (11), `TERM` (15) and `CHLD` (17) — POSIX's numbers,
so nothing here means something different from everywhere else.

**A negative target is a process GROUP**, POSIX's spelling: `kill -TERM
-4` signals every live member of group 4. Pids are 1-based, so a
negative number cannot be mistaken for one. `ps` prints a PGID column.

It **names the process before signalling it** — "sent SIGTERM to pid 4"
is only useful if it says which process that was, and the caller's
mental model of pid 4 is what needs confirming while there is still time
to check.

## What it does NOT promise

**A signal other than `KILL` is PENDING when this program exits.** The
kernel acts on it when the target next returns to ring 3 — its next
syscall, or the next timer tick that finds it running — so a process
that is doing anything at all dies within a few milliseconds, and one
wedged inside a kernel path may not die at all.

`-KILL` is the exception, and that is the whole reason it exists: it
terminates the target immediately and cannot be ignored. It is the last
resort rather than the first, because a process killed that way gets no
chance to finish what it was doing.

**A process may ignore a signal**, and a shell ignores `INT` on purpose
— so `kill -INT` aimed at `/bin/tosh` does nothing, by design. `KILL`
and `QUIT` cannot be ignored, so there is always something that works.

## Exit status

0 if every target was signalled, 1 if any was not — an unknown pid, an
empty group, a target that is not a number, or a signal name this kernel
does not have. Each failure says which.

A **signalled process reports `128 + the signal`** as its own exit code,
which is what a shell prints: 130 for a Ctrl-C, 143 for a `TERM`, 137
for a `KILL`. That is what distinguishes a signalled death from an
ordinary non-zero exit.

## See also

`ps` for pids and groups. `Ctrl-C` at a shell prompt is the same
mechanism with no typing: it sends `INT` to the console's foreground
group — see `docs/signals-design.md`.
