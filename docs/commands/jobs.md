# jobs

**a shell builtin** — `/bin/tosh` only, not the kernel shell.

**Category:** Processes and programs

## Synopsis

    jobs

## Description

Lists the jobs this shell is keeping track of: the ones suspended with
`Ctrl-Z`, one line each.

    [1]+  Stopped  spin_test 900000
    [2]-  Running  spin_test 900000

The number in brackets is the **job id**, which is what `fg` takes. The
character after it is the marker every Unix shell prints: `+` is the
**current** job — the one a bare `fg` means, and the one most recently
suspended — `-` is the one before it, and a space is everything else.

A **pipeline is one job**, listed under the whole command line as typed.
That is not cosmetic: `Ctrl-Z` suspends the process *group*, so every
stage of `a | b | c` stops together and `fg` resumes them together.
Listing three jobs would invite resuming one of them, which would leave
the shell waiting on two stages nothing was going to continue.

**It prints nothing when there are no jobs**, as every shell does.

## Why this is a builtin

The job table is the shell's own memory of what it started. A
`/bin/jobs` would be a separate process with no view of it — there is
nothing here for a program to do more of, which is the test this project
applies before letting a builtin exist at all. POSIX makes `jobs` a
special builtin for the same reason.

## What it does NOT do

**It does not enumerate processes** — `ps` is that program, and this one
holds no opinion about anything this shell did not start. A job started
by another shell, or a process suspended by hand with `kill -STOP`, does
not appear here and cannot be `fg`'d; `kill -CONT` is how those come
back.

**There is a limit** — eight jobs. A ninth suspension is reported as
untracked at the moment it happens, with its pid, rather than being
silently dropped.

**It does not show a job another shell started**, and it does not
survive this shell exiting — the table is memory, not a file.

## See also

`fg` and `bg` to resume one, in front or behind. `cmd &` to start one in
the background. `kill -STOP`/`-CONT` for the same mechanism without a
shell's bookkeeping, and `ps` to see the `stopped` state directly.
