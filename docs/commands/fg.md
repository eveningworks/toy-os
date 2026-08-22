# fg

**a shell builtin** — `/bin/tosh` only, not the kernel shell.

**Category:** Processes and programs

## Synopsis

    fg [n]

## Description

Puts a suspended job back in the foreground: hands it the terminal,
resumes it, and waits for it exactly as the original command line did.

`n` is a job id from `jobs`, with or without a `%` (`fg 1` and `fg %1`
are the same request, and the `%` is there because that is what a job
spec looks like in every other shell). **With no argument it resumes the
current job** — the one marked `+`, which is the one most recently
suspended, because that is what a person typing a bare `fg` means.

It prints the command line it is resuming first. A resumed job produces
nothing of its own to say it is back, and in a shell with several jobs
that is exactly where confirming matters.

**A pipeline resumes as a unit.** The whole process group is continued,
not the stage whose status the shell happens to be holding — resuming
one stage of `a | b` would leave the shell waiting on the other, which
nothing would ever continue.

## The order it does things in

Terminal first, then `SIGCONT`, then wait. It matters: a job handed the
terminal only *after* it starts running can miss a `Ctrl-C` typed
immediately, and one resumed without the terminal at all keeps running
while every keystroke goes to the shell — which looks exactly like a
job that has hung.

## Exit status

The job's, once it finishes — and `Ctrl-Z`'ing it again puts it straight
back in the table under a new id rather than ending it.

## What it does NOT do

**There is no `bg`**, so a job can only be resumed in the foreground.
Background jobs need `&` and the terminal-access signals that stop a
background reader stealing the keyboard, which are not built (see
`docs/roadmap.md`).

**It only knows this shell's jobs.** A process suspended with `kill
-STOP` was never in the table; `kill -CONT` resumes that one.

## See also

`jobs` for the list and the ids. `Ctrl-Z` is what puts a job in it.
