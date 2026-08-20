# beep

**a shell builtin.**

**Category:** Appearance and the console

## Synopsis

    beep

## Description

A short tone through the PC speaker. Fixed at 800 Hz for 200 ms -- not
adjustable, by explicit request: the point was the simplest possible
output, and there is nothing significant about the numbers.

A builtin because `speaker_beep()` has no syscall behind it. That is
the test for every remaining builtin (`docs/conventions/shell.md`):
can ring 3 ask?