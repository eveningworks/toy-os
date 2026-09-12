# stalls

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    stalls [dist | track on|off | reset]

## Options

- `dist` -- the same measurements as a log2 histogram, summed over every
  syscall: the SHAPE of the machine's stalls, where the default table is
  the attribution. Empty buckets are skipped.
- `track on|off` -- start or stop recording. Arming from off zeroes the
  counters. Refused on a machine whose TSC frequency was never
  calibrated, because ticks cannot be converted to time there.
- `reset` -- re-arm from zero. It writes `off` then `on`, because a
  setting written the value it already holds does nothing, so a bare
  `track on` while already on would leave the old totals in place and
  look like a reset that did nothing.

## Description

How long each syscall held the CPU, worst first — over
`QUERY_SYSCALL_STALL`, and legitimately EMPTY while tracking is off,
which is a different answer from "every syscall took no time".

**What it measures is not what the caller waited.** A syscall handler
runs with interrupts off, so its duration is time in which nothing else
on the machine ran at all: no other process was scheduled, no timer tick
landed, and a compositor parked on a frame deadline simply woke late.
`gui latency` counts those late frames from the other end; this says
which syscall spent them. Linux's equivalents are ftrace's `irqsoff`
tracer and bcc's `funclatency`.

**It is timed with the TSC, and the rate is printed for that reason.**
The system clocksource cannot answer this question at all: on every
default boot it is the PIT tick counter, incremented by the timer
interrupt, and that interrupt is off for exactly the window being
measured — so it reads the same value at both ends and every stall comes
out as zero. Without an *invariant* TSC the rate drifts as the CPU
throttles, which is accepted here; this measures millisecond-scale
stalls.

**Off by default**, because armed it costs two TSC reads per syscall. The
tunable is `kernel.syscall_stall`.

The output shape is a contract — `tools/latency_under_io.py` parses it.

    $ stalls track on
    syscall stall timing on (counters zeroed)
    $ diskbench --size 16
    ...
    $ stalls
    timed with the TSC at 3599 MHz -- not the system clock, which cannot
    see a window with interrupts off (see stalls.c)
    syscall              calls      max us      avg us    total us
    write(2)               937      194089        4698     4402426
    open(10)               166      210445       70277    11665982
    unlink(12)               1       94777       94777       94777
