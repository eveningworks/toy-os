# less

**a `/bin` program.**

## Synopsis

    less [file]

## Description

Pager: one screenful at a time. Space/PageDown forward, `b`/PageUp back, arrows by line, `g`/Home and `G`/End to the ends, `q` to quit; a status line shows the position. With no argument it reads STDIN, so `ls -l \ | less` works. It reads keys through `SYS_READ_KEY` rather than fd 0 — which is what makes the pipe case possible at all, since in a pipeline fd 0 is the pipe (real `less` opens `/dev/tty` for the same reason; this OS has none). Page height comes from `SYS_CONSOLE_SIZE`, not a baked 80x25, because the console is font-derived. **Run it with `spawn`, not `run`** — the legacy loader has no scheduler slot, so `SYS_SLEEP` fails there and the key poll spins. Holds the input in memory (256 KB cap, then says TRUNCATED) because a pipe cannot be rewound.
