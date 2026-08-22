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
`KILL` (9), `SEGV` (11), `TERM` (15), `CHLD` (17), `CONT` (18), `STOP`
(19) and `TSTP` (20) — POSIX's numbers, so nothing here means something
different from everywhere else.

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
— so `kill -INT` aimed at `/bin/tosh` does nothing, by design. `KILL`,
`QUIT` and `STOP` cannot be ignored, so there is always something that
works.

## Stopping and continuing

`-STOP` and `-TSTP` **suspend** a process: it keeps its memory, its
descriptors and its place in the process tree, and the scheduler simply
stops choosing it. `ps` reports it as `stopped`, and its CPU column
stops advancing — which is the way to see that it really is suspended
rather than merely idle. `-CONT` resumes it.

`STOP` cannot be ignored and `TSTP` can, the same split Unix makes
between the command and the `Ctrl-Z` key.

**A stop takes effect immediately**, unlike every other signal here,
because suspending a process only changes whether the scheduler picks
it — so it works even on one wedged inside a kernel path. The flip side
is stated below.

**And a stopped process cannot be `TERM`ed.** The `TERM` is set pending
and waits, because the kernel acts on a pending signal only when the
target returns to ring 3 and a suspended process never gets there. It
dies the moment somebody `-CONT`s it. `-KILL` works regardless — that
is what it is for, here as on Linux.

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
group — see `docs/signals-design.md`. `Ctrl-Z` is `-TSTP` the same way,
and `jobs`/`fg`/`bg` in `/bin/tosh` are `-CONT` with bookkeeping.
