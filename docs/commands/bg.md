# bg

**a shell builtin** — `/bin/tosh` only, not the kernel shell.

**Category:** Processes and programs

## Synopsis

    bg [n]

## Description

Resumes a suspended job **without giving it the terminal**, so the shell
keeps its prompt and the job carries on behind it. `fg` is the same
resume with one line more — the handover — and that line is the entire
difference between the two.

`n` is a job id from `jobs`, with or without a `%`. With no argument it
resumes the current job, the one marked `+`.

It prints `[1]+ <command> &`, which is what the line would have looked
like had it been backgrounded in the first place.

## A job that wants input will stop again

Immediately, and that is correct rather than a limitation. A background
job that reads the terminal is sent `SIGTTIN` and suspended, because two
processes reading one keyboard is a race over every keystroke — the
shell would silently lose characters out of the line being typed. So
`bg` on something like `cat` resumes it, it reads, and it is back in the
table as `Stopped` before the next prompt.

`fg` is the answer for a job that wants input. bash behaves identically.

## What it does NOT do

**It does not stop a background job's OUTPUT.** A backgrounded job's
writes interleave with the shell's, as they do on Linux with `TOSTOP`
unset — which is the default there too. toy-os has no `TOSTOP` and no
`SIGTTOU`; see `kernel/include/abi/signal_abi.h` for why that is a
decision rather than an omission.

**It only knows this shell's jobs.** A process suspended with `kill
-STOP` was never in the table; `kill -CONT` resumes that one.

## See also

`jobs` for the list and the ids, `fg` to resume in front instead, and
`cmd &` to start a job in the background without suspending it first.
