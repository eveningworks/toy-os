# strace

**a shell builtin.**

## Synopsis

    strace <binary> [args]

## Description

Linux-style syscall tracing of a `/bin` binary — one decoded line per syscall (`open("notes.txt", O_WRITE\ | O_CREAT) = 3`) plus a count on exit. Also captured in `dmesg`.
