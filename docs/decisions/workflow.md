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

## The GUI tools reach real hardware by being POINTED, not by being ported

A GUI tool here drives two objects: `QMPSession` for pixels and
synthetic input, and `DebugConsole` for the compositor's own geometry.
Both are emulator-shaped -- a QMP socket and the guest's serial port --
and neither exists in front of a laptop. But almost nothing a tool ASKS
is emulator-shaped: `gui menu --json` is the same answer from either
machine, because `guictl` is the same `gui` vocabulary reached from a
program rather than from a serial console.

So `tools/remote_gui.py` serves those two interfaces over telnet and
TFTP, and `TOYOS_REMOTE_HOST` makes the two constructors hand back the
remote objects. A tool reaches the laptop with no edit of its own.

**Why the environment and not a `--host` flag per tool.** The flag is
the explicit version, and it is fifty-odd edits with fifty chances to
forget one -- and a tool that forgot would silently drive the VM while
its output said hardware. The two constructors are already the
chokepoint every tool passes through, the same argument
`launch_qemu_cmd()` makes for being the only way a guest is started.
Each object prints one line naming the machine, because the failure this
trades against is a reader believing the wrong machine was measured.

**Why refusals are by name.** `hmp()`, `mouse_down()`, `key_down()` and
`move_rel()` raise `RemoteUnsupported` with the reason instead of
degrading. The runner reports such a tool as N/A, not FAIL: a suite that
conflates "this check needs an emulator" with "the desktop is broken"
teaches its reader to ignore red, which is the one thing a test tier
cannot afford.

**Why a spot-check tier rather than a gate.** There is one machine, so
runs are serial and a frame costs a second. That buys the questions a VM
cannot answer -- a real backlight, a panel's own modes, real USB, real
timing -- and it cannot buy per-change feedback. Same reasoning as
`qemu_matrix.py`: run it when a change is hardware-shaped, or before a
release.

## A pointer warp belongs in the mouse driver, not in the compositor's event loop

`gui move` injects a position at the compositor's event loop, where it
overrides the pointer for exactly ONE `wm_run()` iteration -- the next
pass reads the driver again and the pointer snaps back. That is right
for a click, whose press and release are edges, and useless for a hover,
which has to still be true while a screen capture is taken. The VM
harness worked around it by moving the REAL pointer through QEMU's input
layer with relative deltas, aiming and correcting in a loop because the
aim is open-loop; hardware has no such layer at all, so hover states,
drag highlights and parked-pointer pixel probes were untestable there.

The capability went where the pointer's position actually lives:
`mouse_set_position()` in `kernel/drivers/input/mouse.c`, clamped
through the same helper every motion path uses. The next relative report
from a device carries on from the new place, which is what makes it a
warp rather than an override -- X11's `XWarpPointer` is server-side for
exactly this reason, and Wayland puts the equivalent in the compositor.

**Reached by `WIN_REQ_WARP_POINTER`, not by a new syscall.** The
compositor already speaks that channel for things only it may do
(`WIN_REQ_FB_CURSOR`, the framebuffer lease), and it is the process that
owns pointer policy; a syscall of its own would have been a second door
into the same room, gated by the same role check.

**The request answers with where the pointer LANDED**, read back from
the driver. It clamps to the pointer's bounds, and the compositor's own
copy does not move until the next raw event reaches it -- so a caller
that echoed its own request would report a position the pointer may
never have had. One round trip replaces the old eight-step loop.

The honest limit: a device that reports ABSOLUTE positions (a tablet,
virtio-input) overwrites a warp with its next report. That is fine for
what a warp is for -- parking a pointer while nothing is touching it --
and it is why the reply is the driver's answer rather than an assumption.

## Audio is judged by RECORDING it, because every counter we had pointed the wrong way

A ring-3 USB audio driver crackled for weeks while every number we could
read said it was healthy. The counters were not lying; they were
answering a different question.

`packets/s` measures whether the DRIVER is busy. Raising `snddrv`'s
priority took it from 7217 to 7959 of the 8000 the endpoint wants -- and
made the audio *worse*, because prioritising the last stage starved the
two that fill the ring, so the driver posted a full-rate stream of an
empty one. That was already recorded as a warning. What was not noticed
is that the same metric had also chosen the buffer depth: judged by
packets/s, a deeper cushion looked catastrophic ("192 outstanding
collapsed to 393 packets/s"), so the driver shipped with three groups.

Judged by the audio, that is exactly backwards. Recording the DAC's own
analogue output back into the machine's line in and counting waveform
discontinuities:

      96 packets (12 ms)   33.1 clicks/s   20 s played in 23.6 s
     160 packets (20 ms)    0.3 clicks/s   20 s played in 20.0 s

The metric had not merely failed to see the fault. It had CAUSED it.

So the rule here: **an output device is judged by what came out of it.**
`tools/audio_loopback_test.py` is the instrument -- a cable from the
DAC's headphone out into the motherboard's line in, a steady tone as the
stimulus, and the sine recurrence `x[n] = 2cos(w)x[n-1] - x[n-2]` as the
detector, so any break in the stream shows up as a residual spike. Its
`--crackle` mode plays one file through the in-kernel driver and the
ring-3 one so the difference is attributable to the driver rather than
to the day.

Two things this bought beyond the fix, both of which a counter would
have hidden. The wall clock is a second, independent witness: an
isochronous endpoint consumes a packet every 125 us whether or not one
arrived, so a 16% shortfall in posting is a 16% longer stream, and the
two numbers agreeing is what ruled out a mere measurement artefact. And
the analyser's own negative control -- the host playing into its own
cable -- caught a bug in the detector on its first run, which is the
only reason a later clean result could be trusted.

The honest limits. The click detector needs a STEADY TONE: real music
has transients that break the recurrence, so a click count on music is
meaningless and only the dropout count survives. The floor is not zero
-- an MP3 stimulus carries the decoder's own ringing, and both drivers
score ~0.3/s on it, so "at the floor" is the goal rather than "zero".
And the cable is not normally connected, which is why the tool SKIPS
rather than fails without it: an absent cable and a silent guest are the
same flat hiss, and calling that red would convict the wrong half.
