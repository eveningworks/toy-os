# dmesg

**a `/bin` program.**

**Category:** System information

## Synopsis

    dmesg [-n <lines>] [-w|--follow] [-T]

## Options

- `-n <lines>` -- print the last `<lines>` lines rather than the whole
  ring; a count that is not positive is refused.
- `-w`, `--follow` -- keep printing as new lines arrive, polling every
  200 ms; Ctrl-C stops it.
- `-T` -- absolute timestamps instead of seconds since boot.

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

## `-T` is derived, and that is worth knowing

    $ dmesg -T -n 3
    [2026-09-01 14:09:01] usb: slot 2: bound as usb-keyboard on endpoint 0x81
    [2026-09-01 14:09:01] usb: slot 2: bound as usb-mouse on endpoint 0x82
    [2026-09-01 14:09:02] dhcp: net0: 192.168.200.107 netmask 255.255.255.0

**Nothing stores a wall clock per line.** `klog_write()` stamps each
logical line with MONOTONIC time, as *text* — `[5068.88] ` is characters
in the ring, not a field — so `-T` computes the boot instant once as
`now − uptime` and adds each line's offset to it. That is exactly what
Linux's `dmesg -T` does, and it inherits the same caveat, which
util-linux also documents: **the answer is wrong if the clock moved
since boot.** A machine whose RTC was wrong until someone set it — a
dead CMOS battery, say — reports every line shifted by however far the
clock was out. Nothing here can detect that; only a wall clock recorded
*at* the line could, and the ring has no room for one.

**The resolution is one second, and the hundredths are dropped
deliberately.** The origin came from a one-second RTC, so a stamp
claiming hundredths of a second would be claiming precision the
derivation does not have. Expect ±1 s against `time`: both `time()` and
the uptime division truncate.

**A line with no timestamp passes through untouched.** The kernel stamps
once per *logical* line, so a continuation legitimately has none —
inventing a time for it would be a fabrication, and this is the command
whose whole point is not splicing a log together silently.

**If the clock is not set at all**, `-T` says so once and keeps the
monotonic stamps rather than printing 1970 for every line:

    $ dmesg -T
    dmesg: the clock is not set -- keeping monotonic times
    [0.42] init: starting

## The log leaves the kernel through `QUERY_KLOG`

`klog_dump()` streams into a callback, which is the wrong shape for a
syscall that must fill a buffer and return, so `QUERY_KLOG` is the way
out. It hands the ring over in byte slices, each carrying the
**absolute offset since boot** that its first byte came from -- which is
what makes a walking reader safe, since the kernel keeps logging while
you read and the ring overwrites its oldest bytes. If a slice starts
further along than the previous one ended, bytes were lost, and `dmesg`
says so where it happened:

    [dmesg: 512 bytes lost -- the log wrapped while reading]

Linux prints a `-` for the same event on `/dev/kmsg`. A log that
silently splices two eras together is worse than one with a hole in it,
because only the second kind can be noticed.

## It does not paginate, on purpose

It writes to stdout and stops, so it can run inside a GUI callback and
never blocks on a keystroke:

    dmesg | less        # page it
    dmesg -n 40         # or just the tail

Both are somebody else's well-tested code, which is what the rule
against a builtin shadowing a `/bin` program that does more is for.

## At the kernel prompt

The ring-0 copy is **`rescue dmesg`**. That is a stretch of the rescue
set's stated rule -- it is nominally "what you would need to put `/bin`
back" -- but a machine whose `/bin` will not load is exactly a machine
that cannot run `/bin/dmesg` to find out why, and `shell_rescue.c` says
its own job is diagnosis. It ignores arguments rather than refusing
them: the flags belong to `/bin/dmesg`, and this is the copy you reach
for when `/bin` is what is broken.

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
