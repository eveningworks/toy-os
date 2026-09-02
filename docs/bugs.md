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

- [ ] `menubar_test.py`'s `hovering a submenu parent opens the next level` fails under `--vga virtio` when other GUI tools ran first on the same guest -- 2 failures in 8 such runs on 2026-09-02, 0 in 6 when run first or after calendar alone, and `predates.py` found HEAD and the working tree both passing the calendar-then-menubar order; never seen on `-vga std`, where `gui_regress` runs it. Cause NOT established, and the page-flip change of that day is neither implicated nor cleared by these counts
- [ ] `tools/ktest_run.py` reports the debug console never came up -- 5 boots in 9 when measured 2026-08-18, then 0 in 29 on 2026-08-20 with nothing in between that targeted it; cause never established
- [ ] `tools/ktest_run.py` failed twice on 2026-08-20 during a long session and neither failure reproduced (0 in 7, then 0 in 1) -- the output was not captured either time, so it is NOT known to be the debug-console one above; capture the log before re-running. **Seen once more 2026-08-29, same shape: one failure in a long session, output not captured, then 0 in 8 immediately after on the same build.** Three sightings now and no captured log between them, which is the thing to fix -- run it under `tee` when a session is doing many runs
- [ ] ATA writes time out under host I/O pressure -- `dma write failed after 3 attempts (drive stayed busy, command never issued)`; measured 1 run in 3 locally on a clean disk, and it was CI's recurring red build until the filesystem moved to virtio-blk
- [ ] `tools/uterm_test.py`'s checks that run a program inside the Terminal fail together -- `edit`'s caret lands rows away from where it typed, and a spawned `/bin` program's output does not come back (`syscall: open() rejected`). Measured 2026-08-27: **1 run in 4** on 09072c6 and 1 in 4 on the tab-chrome tree, then **0 in 8** across e6cdb29 and the tab-completion tree later the same day, with nothing in between that targeted it. Cause never established. NOT the same thing as the ink-box defect fixed alongside it, which was deterministic. **0 in 6 on 2026-08-29**, measured as the control arm of the `SYS_WAIT_READY` port (`flake_hunt.py uterm -n 6`) on a freshly seeded disk, and the full suite green beside it. That NARROWS it and does not close it: 6 clean runs is what a 1-in-3 rate produces about 9% of the time
- [ ] `tools/live_boot_test.py`'s first phase fails its console-driven checks -- `sh df` and `sh ls` come back EMPTY while a later, slower `sh run libc_test` in the same session succeeds, so it is the harness's first commands after boot, not the mount. Measured 2026-08-27: **3 runs in 3** failing the same 4 checks, and 1 run in 1 failing them at HEAD (8babbd8) with the day's work stashed, against 1 run that passed all 7 earlier the same day. PRE-EXISTING. Cause never established; the second phase (the mount policy, added the same day) reads the serial log instead of driving the console and passed 3 in 3 alongside every one of these failures
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

- [ ] Task Manager's title bar read `(Not Responding)` on the bare-metal laptop while the app was drawing and answering clicks normally, 2026-09-02, on the first instance spawned after the Overview page landed. `ps` showed it `block(event)` -- the healthy state -- and the flag is not latched (`on_window_pong()` clears it), so it was failing to pong while its loop turned. NOT REPRODUCED: a fresh spawn on the same build stayed `false` across ~10 minutes of sampling, and 5 samples over 15 s in QEMU never showed it. The WM's own `client pid N is not responding` line had already rolled out of the klog ring, because Task Manager floods it with a `layout col*`/`order` block per sort report -- capture that line before the ring turns over next time. The instance was one the maintainer had been resizing and clicking; an unbounded ring gauge on a maximised window was a plausible per-frame cost and has been capped since, which is a MITIGATION and not a diagnosis

- [ ] `atac`'s "a flush writes back everything dirty" failed once inside a full `preflight.sh` on 2026-09-02 (`FAIL: (int)written`) and passed 3 runs in 3 as `ktest atac` immediately after; cause not established. **Second sighting the same day, sibling assertion:** "a failed write-back is REPORTED" failed its recovery flush (`(int)pending` expected 0, got 8) inside a full `preflight.sh` on the HDA build, then 8 in 8 as `ktest atac` and 0 in 4 full-suite runs under `flake_hunt.py ktest`; a third sighting the same evening (`pending` 16) -- so 2 of 4 clean-disk `preflight.sh` runs that day against 0 of 5 bare `ktest_run.py` runs, which points at something preflight does first (its `usertest`/boot ordering, or host load) rather than the test. Both shapes are a dirty count that another writer (the desktop is up during the gate) can change under the test -- a test reading the same shared state the code writes
- [ ] `tools/poweroff_test.py` reported 18/20 once, on the first run after the GPE mask/clear/restore change, and did not reproduce in 3 runs immediately after on the same build. WHICH TWO CHECKS FAILED WAS NOT CAPTURED -- the run was grepped down to its summary line, which is the mistake to avoid repeating (`tee` the whole output when a session is doing many runs)

- [ ] `stress 200` failed with "couldn't create test file" on the first command after a boot that had just replayed a journal transaction, then the identical command passed moments later -- virtio-blk, KVM

- [ ] A GUI client read a session-font glyph cell as entirely BLANK once, after many font-face switches in one long-lived VM -- Font Demo reported `session-descender g regular -1` (its "no ink at all" value) for `liberation-sans` while the kernel had logged that atlas building 101/101 glyphs. Did not reproduce: 6 consecutive face switches on a fresh boot, and a clean boot on the same face, were all correct. The session had also been changing font SIZE, so a refused `font_face_build()` (the atlas cache is bounded at 16 entries / 4 MiB and REFUSES rather than evicting) is a plausible mechanism and is NOT established -- the refusal is logged, and that log was not captured before the VM was destroyed

- [ ] The desktop died once at 1.15 s while a `/bin` program ran through the legacy loader -- cause unestablished
- [ ] One `etc_config_set()` write failed on a graphical boot, and did not reproduce

## Reproducible

- [ ] `sum` on the bare-metal laptop disagrees with zlib's crc32 on a 5 MB file while agreeing on a 10-byte one, deterministically, with the bytes on disk proven identical by `remote.py get` plus `cmp` -- measured 2026-09-02, on FAT32 and TFS3 alike; cause NOT established (the streaming crc32 or the large-file read path); `remote.py sync` re-sends every large file every run because of it, and a flashed kernel is verified by pulling it back, not by `sum`. Reproduction under the matching heading in `docs/roadmap-details.md`
- [ ] `damage_sweep.py` reports one violation on `start-menu dismiss` -- 866 px changed outside the damage rect in a 64x14 box at (4,702), the Start button after the menu closes; PRE-EXISTING (identical on 540dd6e5 before the rounded-corner change, measured 2026-09-02); its positive control still goes red
- [ ] The SECOND consecutive `preflight.sh` fails three `fs` KTESTs with `fsck` reporting leaked blocks -- PRE-EXISTING (measured 2026-09-02 on 1f79742: run 1 clean, run 2 three failures; two bare `ktest_run.py` runs in a row stay clean, so the ring-3 tests between them leak the blocks); `make clean-disk` before the gate clears it
- [ ] `tools/virtio_boot_test.py` fails 2 of 11 checks every run -- PRE-EXISTING (HEAD before `pci_bar_mem_size` fails identically, 2026-09-02); one is a stale log string, the other unexplained

- [ ] **A device's PRODUCT string is mojibake while its MANUFACTURER string on the same device reads perfectly** -- measured on the bare-metal laptop 2026-08-31, on all three devices attached: `"??Link0??????????????  ??????" TP-Link`, `"??itech??????????????  ??????" Logitech`, `"??B?????????0?????????????"` for a SanDisk. The format is `"product" manufacturer`, so the SECOND of the two back-to-back `usb_read_string()` calls on a slot is the one that fails. NOT reproducible in QEMU. **CAUSE ESTABLISHED 2026-09-01, and the fix has landed (275cc88) but is NOT yet measured on hardware.** `usb_read_string()` read into `uint8_t buf[256]` and `uint8_t lang[4]` on the STACK, as DMA targets -- which `usb_enum.c`'s own comment forty lines above forbids for `g_desc_buf`. A device returning more than it was asked for overruns the frame, so the second read lands in a frame the first has already damaged, which is exactly the "first fine, second garbage" pattern. The same overrun corrupted a caller's saved RBX and panicked the laptop with a general protection fault in `usb_parse_config_interfaces` (see the disassembly in that commit) at roughly **3 boots in 5**. Both buffers are one aligned static now. **What is still open is the MEASUREMENT**: reboot the laptop several times and check both the panic rate and whether the product strings read correctly -- QEMU cannot settle either, since no emulated device over-returns

- [ ] **A device on the laptop's internal port 5 never enumerates** -- `usb: port 5: connected, high-speed, enabled`, then `control transfer timed out after 2000001 polls`, `configuration 0 unreadable`, and the retry fails too with `address device (slot 6) failed: usb transaction error`. Measured on bare metal 2026-08-31; hardware-only. Every other device on the machine enumerated and bound. **WHAT IS ON THAT PORT IS NOW KNOWN, and it did not reproduce on 2026-09-01**: `lsusb` reports `Port 5 Device 4: ID 064e:9700 Suyin Corp. Asus Integrated Webcam, 480Mb/s, no driver` -- enumerated cleanly, with `no driver` being the expected answer since there is no UVC driver. One clean boot is not a fix and nothing targeted it, so this stays open, narrowed: the question is now why the same port timed out on 2026-08-31

- [ ] **The CDC-ECM receive path in `net_usb_ecm.c` has still never carried a frame** -- it transmits fine and its bulk IN endpoint has never completed a single TRB. **NOT known to be a defect in this driver.** Measured 2026-08-31 on a TP-Link UE300 (RTL8153) cabled to a LIVE segment: in the adapter's CDC-ECM configuration, at SuperSpeed and again at high speed, **Linux's own `cdc_ether` also receives zero** -- interface `UP,LOWER_UP`, carrier 1, `rx 0`. An oracle sharing none of this code behaves identically, so the adapter's ECM configuration looks like a compatibility checkbox its maker never exercised. **What the vendor driver settled, and what it did not.** `net_usb_r8153.c` binds the same adapter's VENDOR configuration and receives perfectly -- DHCP, ICMP and 730 KB of HTTP -- so the hardware, the xHCI bulk IN path and `net_rx()` are all proven, and what remains unproven is `net_usb_ecm.c`'s ECM-specific half alone. Note the vendor driver now CLAIMS this device (`usb_r8153_claims`), so reaching the ECM path on it again means taking `2357:0601` out of that table. An adapter whose ECM actually works would settle the rest

- [ ] **Powering off masks the power button's own GPE, so the machine takes TWO presses to start again** -- on the bare-metal laptop (`GPE0_BLK` 32 bytes, 128 events). `gpe_block_off()` disables every GPE before S5 and leaves them masked, which is the ONLY one of three versions that stops that machine at all -- restoring the enables, or rearming just the ones whose status stays clear, both leave it rebooting instead (`docs/decisions.md` has all three measurements). The fix is a `_PRW`-derived wake set, which needs the namespace walk `docs/aml-design.md` stages; there is now evidence that nothing cheaper substitutes, because the cheaper things were tried
- [ ] **A USB wireless mouse intermittently does not bind on the laptop** -- `lsusb` shows it with `, no driver`, and a reboot fixes it. Rate: 1 boot in ~9 on 2026-09-02, and that boot lost the USB NIC too (`ifconfig`: no network devices), so it is the whole xHCI enumeration missing rather than one device; the next boot bound everything. Cause never established. **Possibly related and separately visible in the same boot log**: a device's product string came back as mojibake -- `usb: port 14: 0781:5584 "???B???0????????????? ??????"`, a SanDisk mass-storage stick whose iProduct should be readable ASCII. Every `?` is `read_string()`'s replacement for a non-ASCII codepoint (`usb_enum.c`), and a one-byte misalignment in a UTF-16LE decode produces exactly that pattern, so the string descriptor read is the thing to look at first -- with the caveat that a garbled STRING and a failed BIND are only assumed to be one fault. **2026-09-01: the mojibake half now has an established cause and a fix** (the DMA-on-stack overrun above, 275cc88), and a frame corrupted mid-enumeration is a plausible way for a device to end up unbound -- so re-measure this one after that fix is confirmed on hardware, before treating it as separate **2026-09-02: the SAME symptom on the UE300 adapter has an established cause** -- its boot log read `r8153 version 0x0000` and half a MAC at 0.35 s, i.e. the driver probed before the chip's autoload; `usb_r8153_bind()` now waits for `AUTOLOAD_DONE` first and retries a zero version. Whether the mouse's failure is the same race is not known

- [ ] **`tools/install_test.py` fails every check of its `disk` medium, and it LEAKS the guests it starts** -- measured 2026-09-01, 4 runs in 4 on the working tree and 2 in 2 at HEAD (214d29e) via `predates.py`, so PRE-EXISTING. The first failing check is `the blank target is a disk the system did not boot from`, which is an `lsblk` assertion made BEFORE the installer runs -- so nothing the install writes is implicated. The same guest reproduced BY HAND (`vm.py --disk <blank target> --virtio-disk <copy of disk.img>`) prints exactly what that check wants: `ata0 512.0M disk` and `virtio0p3 ... ROOT yes`. So the tool is not seeing its own guest. **Separately and certainly**: it leaves its QEMU running after a run, and the next run then dies on `Failed to get "write" lock ... system.img`, which contaminated two `predates.py` measurements before it was noticed -- `ps -eo pid,args | grep toyos-install-test` and kill by PID between runs. Cause of the first failure NOT established; the leak is. The GPT write path itself is covered by `kernel/drivers/partition_test.c`'s KTESTs and by `tools/partition_test.py` (24 checks, MBR written and read back), both green

- [ ] **`tools/net_test.py` fails three checks of how init REPORTS the address one-shot** -- `[none] init reports the address one-shot as failed`, `[dhcp] init reports the one-shot as done rather than running`, `[link-local] init reports the one-shot as done, not failed`. 78 passed, 3 failed. Measured 2026-09-01 by `predates.py`: HEAD (1fda85f) fails the same three, so PRE-EXISTING. Everything the checks are ABOUT works -- the `dhcp` phases pass, DNS resolves, and TCP fetches from a real HTTP server -- so what is wrong is the state `service list` reports for a one-shot, not the addressing. Cause NOT established, and it is not known whether the tool's expectations or init's reporting is the wrong half

- [ ] **Something in the GUEST leaks a couple of blocks, and it is not the seeder** -- `ktest`'s three `fsck` checks failed with `r.leaked expected 0, got 2` (`kernel/fs/fs_test.c:494` and two siblings) on a `disk.img` that had been through `tools/usb_test.py` and several `vm.py` guests since its last `make clean-disk`. Measured 2026-09-01. **Narrowed, not attributed.** A fresh image is clean, and the host seeder is ruled OUT by direct test: after the double-indirect free fix (3ffcbee) a `make clean && make all && make iso` -- which rewrites `/install/kernel.bin`, the one seeded file past the single-indirect ceiling -- leaves `fsck` clean, and `tools/tfs3_writer_test.py` passes. So the blocks are leaked by an operation running INSIDE the guest, and which one is not established: nothing narrowed it to a specific tool, command or filesystem call. Reproduce by running several VM-based tools against one image and then `python3 tools/ktest_run.py`; `make clean-disk && make iso` clears it. Worth knowing before blaming a change: this reddens the gate on a long-lived image and looks exactly like the seeder bug that was fixed

- [ ] A process spawned from the RING-0 debug console leaks an unreapable zombie -- `spawn` is `/bin/spawn` now and runs under the legacy loader, which has no scheduler slot, so `SYS_SPAWN` records its child's ppid as 0. Nothing waits for it and no parent death can reparent it, so the slot is held until reboot. `/bin/spawn`'s own header still claims "the child is reparented to init when this exits". The obvious fix -- parent it to init -- would BREAK `strace` at a `#` prompt, which finds its child precisely because both are pid 0; what it really needs is the legacy loader having an identity
- [ ] `tools/terminal_probe.py`: leaving the alternate screen does not take the pager's status bar with it -- measured PRE-EXISTING against d5400cb, where the same probe scored 20/23 against 22/23 after the tabs rewrite
- [ ] `tools/cursor_ibeam_test.py`: the GUI Terminal's grid does not show an I-beam -- 18/19 checks, and the sprite over the content area reads as neither the arrow nor the I-beam but a third bounding box. Measured PRE-EXISTING against cd0cd6f (identical 18/19, same check) on 2026-08-29, found the day the tool was added to `ondemand_sweep.py`, having been run by nothing until then. Cause NOT established; the Terminal's page is black where every other check's background is near-white, so a sprite detector calibrated on light chrome is as plausible as a missing `WIN_REQ_CURSOR`

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

**2026-08-29: a MECHANISM for this class now exists, and it is NOT
established that it was this one.** `serial_putc()` waited unbounded on
a THRE bit that QEMU's socket chardev stops setting when an attached
peer stops draining, so a guest whose console was attached but idle
stopped dead while its log had nowhere to go -- which is the shape of
every tool here (each holds a `DebugConsole` and then does QMP work).
That is fixed; output is queued now. **Three full-suite runs were clean
afterwards, which at these counts distinguishes nothing** -- 3 clean is
what a 40% rate produces about a fifth of the time. What would settle it
is `flake_hunt.py` over the full suite, which this entry already asked
for.

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
  all three cases. `console_shell_test.py` passes 33 of 33 as well,
  and nobody knows which commit fixed its one check.

- **`virtio_gpu_test.py` was the same shape, and the entry naming its
  failing check was wrong.** What failed was `the desktop is actually
  on screen`, not `the screen KEEPS updating` -- the tool required one
  colour to cover half the screen, which stopped being true of a
  working desktop when wallpapers landed (`aurora`'s dominant colour
  reaches 28%). It measures NON-BLACK coverage now and the driver was
  never at fault; 11 of 11 pass, with the `DISPLAY_CAP_NEEDS_FLUSH`
  positive control still reddening exactly that check (0.1% non-black).
  **The lesson is the mis-transcribed check name**: a triage entry that
  names the wrong assertion sends the next session at the wrong code.

- **`virtio_input_test.py` PASSES now — 16 of 16, measured 3 runs in 3
  on 2026-08-31** (369ce06), including the "a virtio keypress reaches
  the ring-3 desktop (Super opens Start)" check this entry was written
  about. **Nothing targeted it in between and no cause was ever
  established**, so this is a measurement rather than a fix: 3 clean
  runs is what a 1-in-5 flake produces about half the time. It is left
  here, narrowed, rather than deleted, because deleting it would claim a
  fix nobody made.

Reproduce: `make clean-disk && make iso && python3 tools/ondemand_sweep.py --logs DIR`.

- **`ansi_cursor_test.py`** — 5 of 10 checks fail on the physical
  console (blank-column and cursor-up assertions), every run. Measured
  PRE-EXISTING against the pre-dynlink HEAD with `predates.py`
  (2026-08-28, both fail identically), so it is console/ANSI rot or
  harness rot, not the dynamic-userland change; cause not established.

Reproduce: `python3 tools/ansi_cursor_test.py` (boots its own guest).
