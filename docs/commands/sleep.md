# sleep

**a `/bin` program.**

**Category:** Processes and programs

## Synopsis

    sleep <number>[s|m|h|d]...   (several are added: sleep 1m 30s)

## Description

Waits, then exits 0. GNU sleep's operand syntax: a number with an
optional fraction and an optional unit -- `s` (the default), `m`, `h`
or `d` -- and several operands are added together, so `sleep 0.25`,
`sleep 90s` and `sleep 1m 30s` all work.

**It never ends early.** The wait is counted in whole milliseconds, and
a fraction below one rounds UP (`sleep 0.0001` is 1 ms). It ends on the
first timer tick after its deadline.

**It refuses rather than guesses.** A sign, an exponent, `inf`, a space
before the unit, two units in one operand (`1m30s`; write `1m 30s`) or
anything else it cannot read is an error: exit 1 and no wait. So is a
total over a hundred years.

`Ctrl-C` ends it at a `$` prompt like any foreground job. It cannot run
under the kernel shell's legacy `run` loader, which has no scheduler
slot to sleep in; `spawn` it, or use a ring-3 shell.

The parser is `lib/uduration.h`, for any program that takes a typed
length of time. One `SYS_SLEEP` is capped at an hour, so `sleep` loops
against the monotonic clock until the whole deadline has passed.
