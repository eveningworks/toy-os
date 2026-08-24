# dmesg

**a `/bin` program.**

**Category:** System information

## Synopsis

    dmesg [-n <lines>] [-w|--follow]

## Description

The kernel's in-memory log, from ring 3.

    $ dmesg -n 4
    [0.42] init: starting
    [0.42] init: target graphical
    [0.43] win_surface: granted the framebuffer to pid 2 (900 pages)
    [0.44] mouse: PS/2 wheel mouse detected (4-byte packets)

Every kernel diagnostic goes through `klog_write()`, which buffers a
timestamped copy in a 16 KB ring and forwards the raw bytes to COM1. The
ring is what this reads; the serial port is why a machine that cannot
run this at all still has its log somewhere.

| | |
|---|---|
| `-n <lines>` | the last `<lines>` lines, rather than the whole ring |
| `-w`, `--follow` | keep printing as new lines arrive; Ctrl-C to stop |

## It used to be a builtin, and that was the bug

Until 2026-08-24 this was a ring-0 shell builtin, so at a `$` prompt it
resolved to a `/bin` lookup, found nothing, and failed — which is what
you hit in a Terminal window, the place a person actually reads a log.
It was not a regression; it had simply never been ported, because the
log had no way out of the kernel: `klog_dump()` streams into a callback,
which is the wrong shape for a syscall that must fill a buffer and
return.

`QUERY_KLOG` is the way out. It hands the ring over in byte slices, each
carrying the **absolute offset since boot** that its first byte came
from — which is what makes a walking reader safe, since the kernel keeps
logging while you read and the ring overwrites its oldest bytes. If a
slice starts further along than the previous one ended, bytes were lost,
and `dmesg` says so where it happened:

    [dmesg: 512 bytes lost -- the log wrapped while reading]

Linux prints a `-` for the same event on `/dev/kmsg`. A log that
silently splices two eras together is worse than one with a hole in it,
because only the second kind can be noticed.

## Pagination is gone, on purpose

The builtin drew `-- more (press any key, 'q' to quit) --` and blocked
on a keystroke, which meant it could not run inside a GUI callback and
had to detect that case and behave differently. This writes to stdout
and stops:

    dmesg | less        # page it
    dmesg -n 40         # or just the tail

Both are somebody else's well-tested code, which is what the rule
against a builtin shadowing a `/bin` program that does more is for.

## At the kernel prompt

The ring-0 copy survives as **`rescue dmesg`**. That is a stretch of the
rescue set's stated rule — it is nominally "what you would need to put
`/bin` back", which is why `strace` went to `/bin` and not there — but a
machine whose `/bin` will not load is exactly a machine that cannot run
`/bin/dmesg` to find out why, and `shell_rescue.c` says its own job is
diagnosis. It ignores arguments rather than refusing them: the flags
belong to `/bin/dmesg`, and this is the copy you reach for when `/bin`
is what is broken.

Typing plain `dmesg` at a `#` prompt runs `/bin/dmesg` like any other
program, and says where the kernel's own copy is if `/bin` is gone.

## What it deliberately does not do

It cannot CLEAR the log — there is no `dmesg -C` here. Filtering by
subsystem is a separate item on `docs/roadmap.md`; `dmesg | less` and
reading is the answer today.

`--follow` polls rather than blocking, because `SYS_QUERY` is a snapshot
interface by design: a fact is computed on every read and has no stored
form to wait on. The interval is 200 ms, under what a person notices and
over what shows up in a CPU figure. It never reads the keyboard, so it
can run in a Terminal window without taking a key from anything else.
