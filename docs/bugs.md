# Bugs

Defects that are **found but not fixed**: something here behaves
wrongly. Split out of `docs/roadmap.md` so that "what is broken" and
"what is not built yet" stop sharing a list -- they are different
questions, they are triaged differently, and a bug carries facts a
roadmap item does not.

**What belongs here:** something misbehaves -- a wrong result, a crash,
a flake, a control that does not respond. **What does not:** a
capability that does not exist yet, a design limitation, or a refactor
someone wants. Those stay in `docs/roadmap.md` under "Known limitations
and papercuts". The test is "is something broken?", not "would I like
this to be better?".

**A fixed bug is DELETED from here, not struck through.** That is the
opposite of the roadmap's rule and deliberate: a roadmap shows
progress, so a struck-through line earns its space, while a bug list is
only useful as a list of what is still wrong. `git log` is the record
of what was fixed -- point at the commit that fixed it if it needs
remembering.

**One line each; the repro lives in `docs/roadmap-details.md` under a
heading of the same name.** Same split the roadmap already uses, for
the same reason -- an index nobody can scan is an index nobody reads.

**Say the RATE for anything intermittent.** "5 boots in 9" is a
measurement; "sometimes" is a shrug, and the difference decides whether
a change made it better. And **say plainly when a cause was never
established** rather than implying one -- a plausible story that fits
the symptoms is not a diagnosis.

**PRE-EXISTING means MEASURED against an earlier commit,** not assumed.
`git stash push -u`, rebuild, and count before blaming your own change;
it has exonerated one this session and convicted another.


## Intermittent -- a rate, not a verdict

- [ ] `tools/ktest_run.py` reports the debug console never came up -- 5 boots in 9 when measured 2026-08-18, then 0 in 29 on 2026-08-20 with nothing in between that targeted it; cause never established
- [ ] `winshare/destroying a window poisons the compositor's mapping` KTEST fails -- 1 run in 18 on 2026-08-20, against 0 in 18 on the previous commit, which at those counts does not distinguish the two; cause never established
- [ ] `tools/ktest_run.py` failed twice on 2026-08-20 during a long session and neither failure reproduced (0 in 7, then 0 in 1) -- the output was not captured either time, so it is NOT known to be the debug-console one above; capture the log before re-running
- [ ] ATA writes time out under host I/O pressure -- `dma write failed after 3 attempts (drive stayed busy, command never issued)`; measured 1 run in 3 locally on a clean disk, and it was CI's recurring red build until the filesystem moved to virtio-blk
- [ ] `heap-debug`'s use-after-free check fails about 1 run in 15 -- PRE-EXISTING
- [ ] `newsyscalls_test` fails intermittently in CI, and not locally
- [ ] `gui_regress.py`'s `uidemo` fails intermittently in the full parallel suite
- [ ] `gui_regress.py`'s `uterm` fails under full parallel load -- 3 runs in 3 on its two `edit` checks (2026-08-24, PRE-EXISTING: the third was HEAD with the day's work stashed); later the same day, 1 full run in 4 failing NINE checks, against 1 in 1 passing alone
- [ ] `damage_hunt.py -j 4` loses VM SLOT 0 every run
- [ ] `ansi_cursor_test.py` and `virtio_input_test.py` fail under an `ondemand_sweep.py` run sharing the machine with other guests, and pass alone -- both 1 run in 1 failing beside four other VMs, 2 runs in 2 passing on their own (2026-08-25); the sweep is single-job, so what competed was another session's tools, not itself
- [ ] Injected clicks are LOST under parallel `gui_regress` load, and the failing checks are finally named
- [ ] A `sched` KTEST fails under KVM, and only under KVM
- [ ] Other GUI tools may share the calculator's mid-paint flake
- [ ] `mm/kmalloc failure is reported, not papered over` fails intermittently -- 2 runs in 10 on 2026-08-22 against 0 in 4 on the commit before, which at those counts does not distinguish the two (0 of 4 is what a 20% rate looks like 41% of the time). The mechanism is plausible and UNPROVEN: the test arms the injector to fail the NEXT kmalloc, and any other kernel allocation arriving in between eats it -- a desktop is running throughout. Both failures were the first run after a `make iso`; six later runs on the same ISO were clean
- [ ] The ring-3 compositor page-faults inside its OWN framebuffer grant -- `RING-3 CRASH: Page fault CS=0x23 error_code=0x6 CR2=0x85001be400`, then `init: toywm (pid 2) exited with code -1`. CR2 is inside WIN_FB_VADDR (0x8500000000) at offset ~1.79 MB, and the grant was 900 pages = 3.6 MB = exactly 1280x720x4, so the faulting address is in the MIDDLE of a region that was granted. error_code 0x6 is write + user + NOT-PRESENT, so a page inside the grant is missing rather than mis-permissioned. First seen on a GitHub runner (QEMU 8.2.2) and not reproduced locally then, including 3 runs through tools/qemu_matrix.py on the same version. **REPRODUCED LOCALLY 2026-08-23** under `ktest_run.py` with the identical CR2 and pid -- 1 run in 7 during signal-handler work, against 0 in 4 on the commit before, which at those counts does not distinguish the two. The RIP differed (0x8000015f2d vs 0x8000012cf5), which is expected across builds and is not evidence of a second cause

## Seen once, cause never established

- [ ] `stress 200` failed with "couldn't create test file" on the first command after a boot that had just replayed a journal transaction, then the identical command passed moments later -- virtio-blk, KVM

- [ ] A GUI client read a session-font glyph cell as entirely BLANK once, after many font-face switches in one long-lived VM -- Font Demo reported `session-descender g regular -1` (its "no ink at all" value) for `liberation-sans` while the kernel had logged that atlas building 101/101 glyphs. Did not reproduce: 6 consecutive face switches on a fresh boot, and a clean boot on the same face, were all correct. The session had also been changing font SIZE, so a refused `font_face_build()` (the atlas cache is bounded at 16 entries / 4 MiB and REFUSES rather than evicting) is a plausible mechanism and is NOT established -- the refusal is logged, and that log was not captured before the VM was destroyed

- [ ] The desktop died once at 1.15 s while a `/bin` program ran through the legacy loader -- cause unestablished
- [ ] One `etc_config_set()` write failed on a graphical boot, and did not reproduce

## Reproducible

- [ ] `tools/init_test.py` fails 12 of its 27 checks, deterministically and PRE-EXISTING -- init stops reaping and stops starting services partway through the run
- [ ] `tools/taskbar_test.py` fails 1 of its 11 checks, deterministically and PRE-EXISTING -- measured 2026-08-25 with `predates.py`, HEAD fails identically
- [ ] `tools/virtio_gpu_test.py` fails "the screen KEEPS updating (the taskbar clock moves)", deterministically and PRE-EXISTING -- measured 2026-08-25 with `predates.py`, HEAD fails identically; cause never established

*(Was empty. Every entry that was here on 2026-08-20 is fixed, was already
fixed, or turned out not to be a defect -- see `git log`. The two above
were found on 2026-08-24 by an unrelated change and measured against
HEAD before being written down. The intermittents are still open, and
the sections above this one are where anything new should go first.)*

## The desktop occasionally never paints its first frame

Seen twice on 2026-08-23 while the icon work was in flight: after boot,
`toywm` is alive and answering `gui state`, its own log shows the
wallpaper decoded and the first frame timed, and the screen still shows
the ring-0 console the machine booted with. Injected `gui move` events
were consumed without the cursor moving.

**Rate: 2 boots out of roughly 8 that afternoon; 0 out of 4 on the same
build immediately afterwards, and not once since across full
`gui_regress` runs.** Cause NOT established. It was originally attributed
to the icon draw path, and that was wrong -- the "isolation" runs that
appeared to clear each suspect were simply boots where it did not
happen, which is what an intermittent does to a bisection.

What would settle it: `tools/flake_hunt.py`-style repeated boots
sampling the framebuffer, since every symptom above is visible from the
host.

## The full parallel `gui_regress` run fails a different tool each time

Measured 2026-08-23 while adding Minesweeper. **`uterm` failed 3 of 5
full-suite runs on that change and 0 of 3 on the commit before it; solo
(`gui_regress.py -k uterm`) it passed 3 of 3 on the same build.** The
same afternoon `font` failed one full run on the change and one on the
commit before, on the same check both times ("the two faces do not have
the same cell"), so the failures are not specific to one tool.

The failing checks are all observation-timing ones -- a shell that did
not come back after Ctrl-D, a font change not yet visible -- which is
the same class as the two entries above about injected clicks being lost
and `uidemo` failing in the parallel suite. **Cause not established, and
the tool it lands on moves run to run.** What is NOT known is whether
the rate genuinely rose with that change or whether three samples of a
load-dependent flake simply landed badly; at these counts the two are
not distinguishable (0 of 3 is what a 40% rate looks like 22% of the
time).

What would settle it: `tools/flake_hunt.py` over the full suite rather
than over a single tool, and a run pinned to fewer jobs (`-j 2`) to see
whether the rate tracks concurrency.

**2026-08-24: two of these had identifiable causes, and both were the
harness.** They do not close this entry -- the tool it lands on still
moves run to run -- but they narrow it, and the shape is worth knowing
before hunting the next one.

- **`font_test` slept 1.2s after each font change** instead of waiting
  for the compositor's own `wm: font changed` line. Three hops
  (a short-lived ring-3 process, the kernel, the client repaint) take a
  load-dependent total, so under load the ESTABLISHING change's line
  landed after the drain that followed it and the next `wait_log()`
  returned the PREVIOUS face's cell -- two different faces comparing
  equal. It failed 3 runs in 5 once an unrelated change made the desktop
  repaint more. Both setters wait on a line that is NEW since their own
  command now, and it passed 6 of 6 after.
- **`uapp_test` polled until the WINDOW existed** and the next check read
  its `resizable` HINT, which arrives over TWP afterwards -- a poll
  weaker than what follows it, which is this repo's own documented flake
  shape. It now polls for the hint.

Neither was a new defect: both were fixed-sleep assumptions that a
busier machine invalidated. **When a tool in this class fails, read its
waits before suspecting the guest** -- and note that a suite run
alongside anything else (including a second `gui_regress`, which is
refused outright now) is competing for the same cores.

## The on-demand tools: what the first sweep found

`tools/ondemand_sweep.py` exists because ~21 test tools are run by
neither `preflight.sh` nor `gui_regress.py`, so nothing notices when
one rots. Its first full run, on a freshly seeded disk, found nine
passing and eleven failing. The failures still open are listed here so
the next session inherits the triage rather than repeating it; the ones
that have since been fixed are gone from the list, with the commit that
fixed them named. **The counts in this section are a snapshot of that
run, not a current score** -- re-run the sweep before believing any of
it.

**MOST OF THESE ARE HARNESS ROT, NOT OS BUGS** — and where that was
checked by hand it is said so. The recurring cause is one thing: a
command moved from a kernel builtin to a `/bin` program, and the tool
either still parses the ring-0 output or runs on an image that has no
`/bin` at all (a freshly formatted one).

- **Fixed since:** `tfs3_v1_test.py` (drove `dmesg`/`mv`/`cat` on an
  image it had just formatted, which therefore has no `/bin` -- it uses
  `rescue` now, 3857d83); `ls_test.py` (`no such directory: /tmp`, which
  the kernel creates at mount and a never-booted image has not got -- it
  stages the directory itself now, 9c3f5c0); `stdin_test.py`
  (`FileNotFoundError` -- it ATTACHES to a running guest, and the sweep
  was not starting one, 299583c). The underlying features were fine in
  all three cases.

- **`console_shell_test.py` — 32 of 33.** One check. Not investigated.
- **`virtio_input_test.py`** — "a virtio keypress reaches the ring-3
  desktop (Super opens Start)" fails. **This one is NOT obviously rot**
  and deserves a look before anything else here: it is an input-path
  assertion, not a command-name one.
- **`virtio_gpu_test.py`** — "the screen KEEPS updating (the taskbar
  clock moves)" fails. Also not obviously rot.

Reproduce: `make clean-disk && make iso && python3 tools/ondemand_sweep.py --logs DIR`.

## `tools/init_test.py` has rotted (detail for the entry above)

**MEASURED, not assumed: 15/31 pass both before and after the
partitioning work** — stashed and rebuilt against the previous commit
to check, and the count is identical. It is not a regression from
anything recent; it is an on-demand tool that nothing has run for a
while, so nothing noticed it going red.

The failures look like one cause, and it is the same rot
`tools/fs_switch_test.py` had (fixed in the TFS2-removal commit): the
tool drives commands that have since become `/bin` programs and parses
output that only the ring-0 builtins still produce. The clearest tell
is `` `kill 1` is refused`` failing with `elf_run: calling
process_run_ring3() for /bin/kill` — the tool is reading the loader's
chatter, not the refusal. Most of the rest are `-- none started` /
`-- ready None`, i.e. a parse that finds nothing rather than a service
that did not run.

**Not yet confirmed to be harness-only.** The likelihood is high given
the shape, but nobody has checked the init/service behaviour by hand
against a boot, so a real defect hiding behind the rot cannot be ruled
out. Doing that is the first step of the fix, not an afterthought.

Reproduce: `make iso && python3 tools/init_test.py`.
