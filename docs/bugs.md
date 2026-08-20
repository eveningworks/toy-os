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
- [ ] ATA writes time out under host I/O pressure -- `dma write failed after 3 attempts (drive stayed busy, command never issued)`; measured 1 run in 3 locally on a clean disk, and it was CI's recurring red build until the filesystem moved to virtio-blk
- [ ] `heap-debug`'s use-after-free check fails about 1 run in 15 -- PRE-EXISTING
- [ ] `newsyscalls_test` fails intermittently in CI, and not locally
- [ ] `gui_regress.py`'s `uidemo` fails intermittently in the full parallel suite
- [ ] `damage_hunt.py -j 4` loses VM SLOT 0 every run
- [ ] Injected clicks are LOST under parallel `gui_regress` load, and the failing checks are finally named
- [ ] A `sched` KTEST fails under KVM, and only under KVM
- [ ] Other GUI tools may share the calculator's mid-paint flake

## Seen once, cause never established

- [ ] The ring-3 compositor page-faults inside its OWN framebuffer grant on CI -- `RING-3 CRASH: Page fault RIP=0x8000012cf5 CS=0x23 error_code=0x6 CR2=0x85001be400`, then `init: toywm (pid 2) exited with code -1 after 5450 ms`. CR2 is inside WIN_FB_VADDR (0x8500000000) at offset ~1.79 MB, and the grant was 900 pages = 3.6 MB = exactly 1280x720x4, so the faulting address is in the MIDDLE of a region that was granted. error_code 0x6 is write + user + NOT-PRESENT, so a page inside the grant is missing rather than mis-permissioned. Seen on a GitHub runner (QEMU 8.2.2); not yet reproduced locally, including 3 runs through tools/qemu_matrix.py on the same QEMU version

- [ ] `stress 200` failed with "couldn't create test file" on the first command after a boot that had just replayed a journal transaction, then the identical command passed moments later -- virtio-blk, KVM

- [ ] The desktop died once at 1.15 s while a `/bin` program ran through the legacy loader -- cause unestablished
- [ ] One `etc_config_set()` write failed on a graphical boot, and did not reproduce

## Reproducible

- [ ] `SYS_LISTDIR` returns 0 for a missing directory and for an empty one alike, so `ls` cannot tell them apart
- [ ] The taskbar overflows off the right edge once enough windows are open
- [ ] `damage_sweep.py`'s `resize-shrink Terminal` step reports a real missed damage
- [ ] `rammeter` doesn't appear at the physical console
- [ ] Control Panel applets can't show hover
- [ ] `ui_checkbox` and `ui_radio_list` aren't in the focus ring
- [ ] Resizing a window by its grip sometimes doesn't take on the first drag
- [ ] `tools/damage_sweep.py`'s random walk sometimes drives Notepad's file picker open by clicking where its
