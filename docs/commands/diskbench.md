# diskbench

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    diskbench [--size MiB] [--path FILE] [--out FILE]

## Description

Times the filesystem's read and write paths and prints the result. Four
profiles, in the shape CrystalDiskMark made familiar — sequential and
random, read and write — over a temp file it creates and removes.

    $ diskbench --size 16
    diskbench: progress SEQ1K-write 1
    ...
    diskbench: result SEQ1K-write 3426 3508 285
    diskbench: result SEQ1K-read 10958 11221 89
    diskbench: result RND4K-write 3508 898 1113
    diskbench: result RND4K-read 9523 2438 410
    diskbench: done

**`--out FILE` writes the report to a file instead of stdout, and
changes its SHAPE: a file gets the whole current state rewritten each
time, at most six lines, rather than an appended log.** That is what a
poller needs — `sys_read` carries at most 1 KiB per call, so an appended
log puts the results, which come last, past where a poller ever reaches.
Disk Mark sat at "Done." with four empty tiles for exactly that reason.
A file rather than a pipe because `SYS_SPAWN`'s `stdout_fd` must be a
pipe write end, and `PIPE_MAX` is 8 KiB kernel-wide — a GUI slow to
drain would block the benchmark it is timing.

**The output is parsed, so its shape is a contract.** A `result` line is
`<profile> <milli-MB/s> <IOPS> <microseconds>` — thousandths and
microseconds, so a reader needs no floating point, of which there is
none in this project's shared code. Disk Mark spawns this and displays
what it prints; that split is the point rather than a convenience, since
these passes take minutes and a GUI client that blocked through one
would stop answering the compositor's pings.

**Why the profiles are not called SEQ1M.** Two things CrystalDiskMark
reports that this OS cannot deliver are stated rather than implied:

- **1 KiB per syscall.** `SYS_WRITE_MAX` is 1024 bytes — an artefact of
  the bounce buffer the syscall copies through — and libsys loops to
  complete a bigger buffer. A "1 MiB transfer" is therefore 1024
  syscalls and the disk never sees one, so the sequential profile is
  **SEQ1K**.
- **Q1T1.** One request in flight, always: there is no asynchronous
  block interface and no threads here, so Q8T1 and Q32T1 have nothing to
  express.

The write passes run first — the write is what lays the file down, so a
separate prepare pass would move the same bytes again for no result.
The random passes cover an eighth of the file: at 4 KiB an operation a
full pass over 256 MiB is 65536 seeks, and the figure does not get truer
for being slower.

## What it deliberately does not do

It does not measure the *device*. Every transfer goes through the
ordinary file syscalls and the filesystem, journal included, because a
ring-3 program has no raw block access — there is no `/dev` here. A
write number is TFS3's, not AHCI's. That is also what CrystalDiskMark
measures on a mounted volume, so it is the honest comparison rather than
a compromise.

It does not verify what it wrote. `stress` does that and this does not:
one proves the bytes are right, the other measures how fast they move.

## The trap

**A number here is not comparable across machines unless the emulator
is.** Everything automated in this repo runs TCG, where throughput is
the emulator's and not the disk's. `make run KVM=1` is a different
measurement again, and a real drive a third. Say which one a number came
from.

**Benchmarking a RAM-only root measures `memcpy`.** Disk Mark refuses
that case by name; this program does not check, so `df` is worth a look
first.

## See also

**Disk Mark** is the GUI over this. `stress` verifies rather than times.
`ahci`, `ata` and `df` say which backend is actually carrying the root.
