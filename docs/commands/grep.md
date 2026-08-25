# grep

**a `/bin` program.**

**Category:** Text processing

## Synopsis

    grep [-i] [-n] [-v] [-c] <pattern> [file ...]
      -i  ignore case
      -n  print the line number before each line
      -v  print the lines that do NOT match
      -c  print only a count of matching lines
      with no file, reads standard input

## Description

Prints the lines of each file that match `pattern`. With no file, reads
standard input — which is what makes `cat log | grep error` work.

With several files, each line is prefixed with the file it came from. Exit
status is **0** if anything matched, **1** if nothing did, and **2** on an
error (a bad pattern, an unreadable file) — the distinction matters, because
"nothing matched" is a normal result and "the pattern was nonsense" is not.

The matching is tolibc's `<regex.h>` (`userland/libc/regex.c`), a Thompson
NFA. It has no input that makes it slow: `(a*)*b` against a long run of `a`
is linear here and is the case that hangs a backtracking engine.

## The syntax is ERE, not BRE

**This is a deliberate divergence from POSIX grep.** POSIX grep speaks Basic
Regular Expressions, where `+ ? |` are literal characters and grouping is
spelled `\( \)`. Those rules exist because grep predates the extended syntax
and could not break the scripts already written against it — compatibility
baggage this OS has no reason to inherit.

So here, `+ ? | ( )` are operators and a backslash makes them literal:

| Pattern | Matches |
|---|---|
| `^fs:` | lines beginning `fs:` |
| `[0-9]+` | one or more digits |
| `(ata\|virtio)` | either word |
| `\.qoi$` | lines ending in `.qoi` |
| `a{2,3}` | two or three `a`s |
| `[[:digit:]]` | a digit, by class name |

There is no `-E`, because there is nothing to switch to. `regcomp()` does
implement BRE, so adding the flag is a line if a POSIX script ever needs it.

## Traps

**The shell eats `|` before grep sees it.** `/bin/tosh` splits on `|` first
and has no quoting, so `grep -c "(a|b)" f` runs `grep -c "(a` piped into
`b)" f`. Until tosh grows quoting (`docs/roadmap.md`), use a bracket
expression — `[ab]` — or a pattern without alternation. This is a shell
limitation, not a grep one.

**A very long line is matched in full but printed truncated**, with
` [truncated]` appended, at 1024 bytes. Silently cutting it would make grep
lie about what it found.

**`grep -v` inverts the match, not the exit status.** A `-v` that prints
lines still exits 0.

## See also

`cat` (its usual producer), `dmesg`, `less`.
