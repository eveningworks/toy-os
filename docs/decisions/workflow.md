# Decisions: Workflow and tooling

How this project is built, tested and delivered -- the dev tooling, the
test harnesses, and the conventions around them.

Part of **[docs/decisions.md](../decisions.md)**, which indexes every
decision in this project and is GENERATED from these files -- run
`tools/gen_decisions_index.py` after adding an entry here, or
`tools/check_docs.py` will fail.

## The GUI suite's wall clock is its slowest tool, so the fix was a timeout knob rather than parallelism

`gui_regress.py` ran 24 tools totalling ~425 tool-seconds at the time
(see the follow-up at the end for the current figures). The obvious
levers were tried and measured, and two of the three did almost nothing:

| change | wall |
|---|---|
| `-j4` (as it was) | 76s |
| `-j8` on a 16-core host | 72s |
| `-j8` + KVM (`--kvm`) | 64s |
| shortening the slowest tool's timeout | **61s** |

Raising the job count bought four seconds and KVM another eight, because
the suite was never CPU-bound. Its slow tools sit WAITING on real
timeouts, and neither more cores nor a faster guest shortens a
wall-clock wait. `forcequit_test.py` alone waits out the WM's 3-second
not-responding timeout about ten times -- once per dialog it raises, six
of them in the repeat loop -- which pinned the whole suite at ~72s under
every configuration.

So the fix was `gui pingtimeout` (`userland/wm/wm_debug.c`), turning that
timeout down to 0.4s for that tool's run: 72s to 34s, and the suite to
61s. The new floor is `notepad` at 42s.

**Why a WM constant may be moved by a test at all.** The timeout's VALUE
is not what `forcequit_test.py` asserts -- the detection, the dialog and
force quit are, and the tool's own docstring already treated raising the
timeout as a positive control. Shortening it therefore loses no
coverage. It is a variable rather than a registered setting for two
reasons: it has no user-facing meaning worth a Control Panel row, and a
persisted value would silently change how the desktop treats a slow app
on every later boot. `gui watchdog <ms>` is the same shape and the
precedent.

Two rules ride with it, both of which would otherwise turn a speedup
into a silently weaker test. The tool must ASSERT the knob took --
without that, a missing or refused command leaves every wait shorter
than the timeout it is waiting for, and the whole tool fails in a way
that reads exactly like the feature being broken. And every NEGATIVE
check ("a hung app nobody is closing does not raise a dialog") must wait
a MULTIPLE of the timeout rather than a fixed number of seconds, so
shrinking it keeps the relationship those proofs depend on.

**The transferable half is the measurement, not the knob:** on a fan-out,
look at the maximum and never the sum. Parallelism and faster guests
both act on the sum, which is why both barely moved. The same session
also opened by asserting from memory that the suite took "about seven
minutes" when it took 76 seconds -- the difference was the session's own
polling, not the suite. Measure the baseline before optimising it.

**Follow-up (2026-08-21): once the max was cut, the SUM started to
bind.** A later pass made every tool wait on an observable rather than a
fixed sleep -- `enter_gui()` polls the desktop ready instead of sleeping
~3s per tool, and `menubar`/`taskmgr` (the last fixed-sleep tools) poll
their app's own layout/log reports. That took the sum to ~408
tool-seconds over ~25 tools with `notepad` (~41s) the floor, and made a
higher job count worth having, so `DEFAULT_JOBS` rose to `min(12,
cores//2)` -- a ceiling against oversubscription (still ~one guest per
physical core), which only bites a host with more than 24 threads. So the
"look at the max, never the sum" rule is really "cut the max FIRST, then
the sum begins to matter": the two passes complemented each other rather
than one superseding the other.

## No per-file licence headers, and the MIT notice lives in one place

Source files here carry no licence header -- not the MIT boilerplate,
not an SPDX tag. `LICENSE` at the root is the whole statement, and
`tools/check_licenses.py` makes the third-party inventory in it a build
property. Asked and decided 2026-08-30; written down because it is a
question that gets asked again.

**The legal part is not the interesting part.** MIT requires the notice
to accompany "copies or substantial portions of the Software", and a
`LICENSE` file shipped with the source does that. Per-file headers are
not required by the licence and never have been.

**What real projects do.** Linux carried full boilerplate for 25 years,
accumulated around 700 inconsistent variants of it, and in 2017-18
REPLACED all of it with SPDX one-liners across ~60,000 files -- the
argument being that machine-readable beats human-readable and that
boilerplate had stopped being maintainable. FreeBSD ran the same
migration; LLVM cut its header to two lines and a tag; wlroots and KWin
are SPDX-only. musl went the other way and has no per-file notice at
all, one `COPYRIGHT` file and nothing else. Nobody who has revisited
this recently chose full boilerplate.

**Why this repo lands on musl's answer rather than Linux's.** The case
for SPDX tags is real and was weighed: this is a mixed-licence tree
(MIT here, GPL-2.0-or-later in `userland/ports/doom/`, OFL and Bitstream
Vera fonts), and a tag is the only thing that makes that boundary
visible at the FILE rather than only from `LICENSE`. What decided it
against is scale and audience. Linux's problem was tens of thousands of
files, hundreds of contributors and downstream vendors running
compliance scanners; this is one maintainer and a repository nobody
ships a derivative product from. 800 tag lines would be bought with no
present buyer, and the one scenario they solve -- somebody lifting a
single file out of the tree -- is better served by the file's own
top-of-file comment naming the project, which every non-trivial file
here already has.

**Two things that were ruled out separately, and would stay ruled out
if this is revisited.** A per-file COPYRIGHT LINE is a year and a holder
written into 800 places that somebody must keep true, which is the exact
shape this project deletes everywhere else -- and it is where the
maintainer's real name would eventually leak, against the standing
privacy convention (see this file on the history rewrite). And
**`userland/ports/` must not be tagged at all**: `doomgeneric.c` carries
no header upstream, and adding a licence identifier to third-party code
without verifying it per file is worse than leaving the directory's own
`LICENSE` to do the job it already does correctly.

**What would change the answer**: a second contributor whose
contributions are under different terms, a downstream that runs a
compliance scanner, or files being copied out of here often enough for
it to be a real event rather than a hypothetical. Any of those makes
SPDX tags worth the eight hundred lines -- and it is SPDX tags, not
boilerplate, that they would be worth.

## The test harness shuts the guest down, because a power cut leaks blocks BY DESIGN

`vm.py stop` used to be `os.kill(pid, 15)` and `boot_smoke_test.py`
called `proc.terminate()`. Both are a power cut from the guest's side:
QEMU goes away without the guest being told, so anything mid-write is
simply lost.

That is not a filesystem bug, and the distinction is the whole point of
this entry. `tfs3.c`'s `flush_alloc_state()` writes the allocation
bitmap **unjournaled and set-before-use**, so a crash between marking a
block allocated and committing the transaction that references it leaks
that block -- deliberately, because the alternative ordering risks
DOUBLE-ALLOCATING it, and a leak is the safe direction. `fsck` exists to
reclaim exactly this.

The consequence nobody had connected: `preflight.sh` runs
`boot_smoke_test.py` and then `ktest_run.py`, and three `fs` KTESTs
assert `r.leaked == 0`. So the gate booted the image, cut its power
mid-write, and then failed a suite on the debris it had just created --
reliably, on the FIRST run, on a freshly seeded disk. It was recorded in
`docs/bugs.md` for three days as a filesystem defect, twice with the
wrong reproduction ("the SECOND consecutive preflight", then "one
ordinary boot"), because every attempt to characterise it changed the
timing and so changed the answer.

Isolated by a three-way comparison on one build: fresh disk then
`ktest_run.py` passes 654/0 and passes again on a second bare run; the
same fresh disk with one `boot_smoke_test.py` in between fails three
checks; the same fresh disk with an **8 second wait** before the kill
passes. Time, not booting, was the variable. `fsck` instrumented to name
what it found reported a DATA block (`group 0 idx 13152`), not an
orphaned inode -- both are counted through `r->leaked`, which is why
"2 blocks" had been an assumption rather than a reading.

So both tools ask the guest to shut down first and fall back to the
signal: `vm.py stop` sends QMP `system_powerdown` and waits, with
`--hard` to skip it for a wedged guest, and `boot_smoke_test.py` does
the same over a **unix** QMP socket -- a unix socket rather than a TCP
port because that needs no slot from `port_guard` and cannot clash with
another guest. Stopping a VM must never itself be able to hang, so the
fallback is unconditional after a timeout.

What this does NOT do is remove the underlying gap, and it is worth
naming: an unclean shutdown still leaves debris that only `fsck`
reclaims. ext3/4 do better -- an orphan inode list replayed at mount, so
a crash mid-unlink or mid-truncate recovers itself without a full check.
That is the honest upgrade path if this ever costs anything real; it
touches the journal, which is the one part of TFS3 with a
credit-counting design, so it was not worth doing to fix a harness bug.
