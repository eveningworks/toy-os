# sum

**a `/bin` program.**

**Category:** Developer and diagnostic (`help tests`)

## Synopsis

```
sum
```

Reads two integers from standard input and prints their sum.

```
/$ sum
Give me two numbers: 3 4
Sum is: 3 + 4 = 7
```

The two numbers may be separated by any whitespace, a newline included,
so typing them on separate lines works as well as on one.

Non-numeric input is not rejected: `scanf`'s `%d` assigns nothing and
the operand keeps its initial zero. That is deliberate for a program
this small -- the alternative is a retry loop, and a rejected character
stays in the stream, so the obvious one spins forever (see
`userland/include/stdio.h`).

## Why it exists

As the worked example of an interactive `/bin` program, and as the
thing that found a real gap: the prompt above has no newline, `stdout`
is line-buffered on a terminal, and until `refill()` in
`userland/libc/stdio.c` flushed `stdout` before a blocking read, the
prompt stayed in the buffer -- so the screen was blank while the
program waited, and both lines appeared together at exit. C11 7.21.3p3
lists that flush among the moments a line-buffered stream transmits,
and glibc and MSVC both do it, which is why the same source behaves
correctly on Linux and Windows.
