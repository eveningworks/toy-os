# diskbench

**a `/bin` program.**

**Category:** Diagnostics

## Synopsis

    diskbench [--size MiB] [--path FILE] [--out FILE]

## Options

- `--size <MiB>` -- how much data each profile moves, 1 to 4096, default
  64; outside that it refuses with `size-out-of-range`.
- `--path <FILE>` -- the scratch file to write, created and removed by
  the run. The default is in the PERSISTENT scratch directory, never the
  RAM one, which a benchmark pointed at would measure `memcpy`.
- `--out <FILE>` -- write the report to a file instead of stdout, and
  rewrite the whole current state each time rather than appending.

## Description

Times the filesystem's read and write paths and prints the result. Four
profiles, in the shape CrystalDiskMark made familiar — sequential and
random, read and write — over a temp file it creates and removes.

    $ diskbench --size 16
    diskbench: syscall-bytes 65536
    diskbench: clock-granularity-ns 140
    diskbench: progress SEQ-write 5 4194304 61230
    ...
    diskbench: result SEQ-write 7509 120 8323
    diskbench: io SEQ-write read 13223 50484 1319043
    diskbench: io SEQ-write write 2331 45164 242459
    diskbench: io SEQ-write flush 596 0 56392
    diskbench: result SEQ-read 62692 1003 996
    diskbench: io SEQ-read read 1385 34565 145042
    ...
    diskbench: done

**`--out FILE` writes the report to a file instead of stdout, and
changes its SHAPE: a file gets the whole current state rewritten each
time rather than an appended log.** That is what a poller needs: it
re-reads the file in one `sys_read`, so an appended log would put the
results, which come last, past where a poller ever reaches. The snapshot
is bounded so it stays inside the reader's buffer, and a report that
outgrows it ends with `diskbench: error report-truncated` rather than
silently stopping.
A file rather than a pipe because `SYS_SPAWN`'s `stdout_fd` must be a
pipe write end, and `PIPE_MAX` is 8 KiB kernel-wide — a GUI slow to
drain would block the benchmark it is timing.

**A `progress` line is `<profile> <percent> <bytes-moved>
<timed-microseconds>`**, cumulative within the profile: every 5%, and at
least every quarter second of timed work, so a reader can draw the rate
over the run (Disk Mark's chart) by differencing two of them. A reader
that misses one loses resolution, never data.

**The output is parsed, so its shape is a contract.** A `result` line is
`<profile> <milli-MB/s> <IOPS> <microseconds>` — thousandths and
microseconds, so a reader needs no floating point, of which there is
none in this project's shared code.

**An `io` line is `<profile> <op> <calls> <sectors> <microseconds>`,
one per block-layer operation kind, as a delta across that profile
(`QUERY_BLKSTAT`).** It is where the time actually went, and it exists
because a MB/s figure cannot tell apart the three things that make a
disk slow here: commands that are too small, commands that are too many,
and cache flushes. A flush moves no sectors at all and can still be most
of the wall clock — and on emulated hardware it is nearly free while on
a real SSD it forces DRAM to NAND, which is exactly how a number
measured in QEMU gets believed about a laptop.

**A `lookup` line is `<profile> <calls> <reads> <microseconds>`** -- what
the filesystem spent RESOLVING PATHS during that profile
(`QUERY_FSSTAT`). The `io` lines cannot separate it: every read a
resolution issues is attributed to the block layer along with the data
traffic beside it. There is no inode cache, so each `read()`/`write()`
resolves from the root again, and this is the only place that
per-syscall cost is visible. Measured at **29-66% of all block reads**
depending on profile, which is why `docs/pagecache-design.md` puts
caching it first.

**`clock-granularity-ns` says whether to believe the `io` microseconds.**
The kernel times each call with its clocksource, and in every default
QEMU configuration that is the PIT: a guest is not offered an invariant
TSC unless the CPU model says `+invtsc` (it blocks migration), so a
single command rounds to zero and the whole column reads as "this cost
nothing". Real hardware has the TSC and resolves it. A granularity in
the millions means the `io` micros are floor-zero noise; use
`vm.py --kvm --cpu host,+invtsc` to get a real one.

Disk Mark spawns this and displays what it prints; that split is the
point rather than a convenience, since these passes take minutes and a
GUI client that blocked through one would stop answering the
compositor's pings.

**Why the profiles are not called SEQ1M.** Two things CrystalDiskMark
reports that this OS cannot deliver are stated rather than implied:

- **One syscall is not one request.** The sequential block is
  `SYS_WRITE_MAX` (`abi/syscall_abi.h`) — an artefact of the bounce
  buffer the syscall copies through — and libsys loops to complete a
  bigger buffer. A "1 MiB transfer" is therefore several syscalls and
  the disk never sees one, so the profile is plain **SEQ** and the
  program prints the size it actually used as `syscall-bytes`, which is
  the number to read rather than any figure quoted elsewhere.
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
