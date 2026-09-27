# tools/

The dev/build helper scripts — not compiled, not shipped as part of the
OS. CLAUDE.md keeps a question-to-tool index; this file is the full
reference for each one: what it does, why it exists, and the traps it
encodes.

**The bar for adding one:** does it fix a rederive-from-scratch cost?
That is the reasoning that produced every tool below. Add freely when it
does — and add its entry here, which `tools/check_docs.py` verifies.

Dev/build helper scripts, not compiled or shipped as part of the OS:
`genttf.py` (font generation, pre-existing), `gen_kbs.py`
(generates the `seed/sync/etc/kbs/<layout>` keyboard-layout data files
from Linux's own XKB data -- see `docs/decisions.md` on layouts being
data files, not a compiled-in enum), `qmp_test.py`
(QEMU/QMP GUI testing helpers — see `docs/testing.md`), `boot_smoke_test.py` (fast
non-GUI boot check — see `docs/testing.md`), `gen_version.sh`/`set_version.sh`
(versioning — see CLAUDE.md's `version.h`/`VERSION` bullets),
`ktest_run.py` (drives the in-kernel test suite over serial and turns
it into an exit code -- what `make test` and CI run; `--virtio-disk
PATH` attaches a second disk on virtio-blk, which then carries the
filesystem while the IDE drive stays for the `[ata]`/`[atac]` suites,
`--mem MIB` sizes the guest (256 by default, and the `mm` and `paging`
above-4-GiB checks SKIP below 4096 -- `highmem_test.py` is the runner
that refuses the skip), and `-v` prints the WHOLE transcript, boot
messages included. **It
stops the desktop before the suite** -- `service stop toywm`, after
waiting for init to report it ready, because the winshare KTESTs need
the compositor role and refuse to take it from a live desktop; nothing
about that stop survives to the next boot, and the run FAILS if the
role was still held when the suite ran, so a silently-skipped suite
cannot read as a pass. **It stops `logd` too, and waits for the DHCP
lease**, because the suite needs a QUIET DISK: the `fs` and `atac`
tests compare disk usage and flush counts against themselves, and
`logd` persisting the suite's own kernel-log lines landed between
those reads on 4 fresh boots in 6, while the dhcp one-shot writes
`/etc/resolv.conf` a few seconds into the boot. **And it runs `fsck
repair` first, printing what it reclaimed**: a guest killed with a
`batched` inode update still deferred leaks the blocks past it by
design, every harness here kills its guest, and the suite's first fsck
asserts clean -- so the precondition is established rather than
inherited from whichever tool booted the image last), `vm.py`
(start a headless VM and run shell commands against it, getting text
back, `--virtio-disk` likewise; `--usb xhci|xhci+mouse` attaches an
xHCI controller and USB HID devices, off by default because attaching a
`usb-kbd` takes the keyboard AWAY from PS/2 — see `docs/testing.md`;
`--usb-host VID:PID` passes a REAL device off the host's bus through to
the guest, which is the only way to reach a driver path no emulated
device has — QEMU's `usb-audio` is UAC1 at one format, so every UAC2
path in `sound_usb.c` is unreachable without it; it needs write access
to the device's `/dev/bus/usb/BBB/DDD` node (`root:root 0664` normally,
so `sudo chmod o+rw` on it, which a replug resets) and it TAKES THE
DEVICE AWAY from the host until the guest exits;
`--net e1000|virtio|both|none` chooses the NIC, where `e1000` is what
QEMU already attached implicitly to every guest ever launched here and
`virtio` is the only way to reach `virtio_net.c`; `--hostfwd tcp::8080-:80`
maps a host port onto a guest one, which is the ONLY way anything can
start a conversation WITH the guest -- SLIRP is a NAT, so outbound needs
no configuration and inbound needs this).

The rest, added once the build/test/delivery loop had enough repeated
manual steps to be worth automating:
- **`acpi_dump.py`** — pulls an ACPI table out of a running guest (via
  `acpi --dump`) in RANGES and reassembles it, then checks the length the
  table declares and the ACPI checksum, which every table's bytes sum to
  zero for. The ranges are not an optimisation: asking for a whole
  8605-byte DSDT in one command returned 8557 bytes with the missing
  lines SCATTERED rather than truncated, because that much hex outruns
  the debug console — the failure mode that looks like success. The
  checksum is what turns "I got some hex" into "I got the table"; a
  single flipped byte breaks it. `--print` emits a C array ready to paste
  into a KTEST, which is the point: `docs/aml-design.md` needs a real
  DSDT, and a hand-written one would only ever agree with the parser
  written beside it.
- **`aml_walk.py`** -- a DSDT dump on the HOST, classified: how its
  `_PRT` is written (a static package, a method returning one, a method
  choosing between two by a `PIC` flag, a method returning a reference),
  every entry decoded, and whether each link device's `_CRS` is a
  constant or a method that reads hardware. It is the measurement the
  kernel's `_PRT` reader was designed from, and the oracle for
  `kernel/acpi/aml_data.c` -- the two share no code. No AML disassembler
  is installed on this host; this is the one that exists. Refuses a dump
  whose ACPI checksum fails (a console dump loses bytes in transit; for
  a laptop write it to a file and `remote.py get` it). `--tree` prints
  the namespace.
- **`qemu_matrix.py`** -- runs the kernel test suite against SEVERAL
  QEMU versions in Docker (6.2, 7.2, 8.2 -- the last is what GitHub's
  runner has). Nothing is BUILT in the container: the ISO comes from
  the host's `make iso` and is mounted read-only, so what runs is
  byte-for-byte a normal build and there is no second toolchain to
  drift. **15 s per version** on a cached image (69 MB each), so ~45 s
  for all three -- cheap enough to run whenever "could this behave
  differently on another QEMU?" has a plausible yes: a driver, a poll
  loop, a timeout, a clocksource, DMA. **Run at a RELEASE and when
  asked** -- deliberately not part of `preflight.sh` (which must not
  start requiring Docker) and not an automatic per-change habit. A
  disagreement between versions is a REPORT: an older QEMU can be
  quirky rather than right, so it is the maintainer's call whether a
  finding is worth fixing, filing, or noting as a known limitation.
  **It exists because a whole class of bug is invisible on one QEMU.**
  A virtio-blk defect -- `virtqueue_poll()` spending a ~12 ms budget it
  believed was 5 s, then letting late completions desync the used ring
  -- never appeared on QEMU 11.1 and reproduced every time on 8.2.2,
  because which clocksource the kernel picks depends on the host. It
  was found through CI instead, at six pushes and several wrong
  diagnoses, on a loop costing ~90 s at best and sometimes failing for
  reasons of GitHub's own (one run died with `apt-get install` timing
  out after 8 minutes). This makes the second QEMU local and fast.
  **`--suite usertest` runs the ring-3 `/tests` suite instead of ktest,
  and `--runs N` turns a row into a RATE.** Both exist because the two
  suites drive the disk differently: ktest never hung on 8.2.2 while the
  ring-3 suite deadlocked the emulator 4 runs in 17 (see "An ATA command
  is done when the bus master says so" in `docs/decisions.md`). A hung
  run is reported as `HUNG`, its container killed BY NAME -- a timeout
  on the `docker run` client alone leaves the container and its QEMU
  running -- and each run's output and serial log are kept in `.qemu_matrix/`.
- **`backup_repo.sh`** -- a complete, verifiable backup of the GitHub
  repo: mirror clone, a bundle of LOCAL refs (catching branches never
  pushed), **every release asset**, and the repo/PR/issue metadata.
  Run it before anything that changes the repo's identity or history --
  a transfer, a visibility change, an account rename, a history
  rewrite. The reason it exists: `git clone --mirror` is NOT a backup
  here, because release assets (~130 MB of ISOs and disk images) live
  only on GitHub and cannot be recovered from a clone. It verifies the
  bundle and checksums any release shipping a `SHA256SUMS`, but does
  NOT run the restore test (clone the mirror, `make all`) -- do that by
  hand before relying on it, since matching hashes prove the bytes
  survived and only a build proves it restores to a working project.
  See `docs/decisions.md`.
- **`genrelocs.py`** -- builds the kernel's own relocation table for
  kernel ASLR: extracts every ABSOLUTE reference from a
  `ld --emit-relocs` link and emits it as a C array the second link
  pass embeds in `.krelocs` (~7,400 fixups, 29 KB), which
  `kernel/arch/x86_64/reloc.c` applies at boot. `--verify` re-derives
  the table from the FINAL image and fails the build if the two
  disagree -- run automatically by the kernel's link rule, because a
  table that disagrees with its image is otherwise a kernel that does
  not boot with nothing to read. Three traps live in the Makefile rule
  and are commented there: the shipped kernel must have its `.rela`
  sections stripped (GRUB will not boot the `--emit-relocs` image, and
  the symptom is an EMPTY serial log), `build/krelocs.c` must be a
  named prerequisite and `.PRECIOUS` or make deletes it as an
  intermediate, and `linker.ld`'s `.krelocs` must stay after `.data`.
  See `docs/decisions.md`.
- **`check_docs.py`** -- the documentation rules a script can check,
  because the ones that rotted before were the ones nobody checked. A
  pointer to the DELETED changelog, a milestone heading that reintroduces
  a number or a target version, a DUPLICATED roadmap entry, a roadmap item
  that WRAPS onto a second line or runs past 140 characters, a
  roadmap-details HEADING that names no roadmap item or bug any more, a
  stale decisions index, a link to a doc that does not exist, and a tool
  in `tools/` that CLAUDE.md never mentions. The one-line rule is checked
  rather than stated for the usual reason: the roadmap reached 2,939
  lines by accumulating a paragraph per item, and nothing noticed. The duplicate check earns its
  place on its own -- two of this repo's own roadmap edits duplicated an
  entry and a third silently deleted three. The heading check earns its
  place the same way: the pairing by TITLE is what sends a reader from a
  one-line item to its long form, and the two files are edited apart, so
  an item reworded in place leaves its heading stranded. Eight had
  drifted when the check was added -- two section renames and six items
  reworded or ticked. The tool check enforces a
  rule this file already stated and nothing verified; it is deliberately
  a NAME check, so it says a tool is mentioned, not that what is written
  about it is still true. It does NOT flag `Milestone N` in prose
  (historical, and `docs/roadmap-details.md` ends with a legend for
  those); the noise would be what stopped anyone running it. In
  `preflight.sh` and CI.

  **It also checks that every command has a page** in `docs/commands/`
  -- every `/bin` program and every `dispatch()` builtin -- and that
  every page documents something that exists. `docs/roadmap.md` had
  wanted this since the man-pages milestone was written, and it is the
  half of a per-command docs folder that makes it stay true: a command
  shipping undocumented is not found by anyone reading the docs, it is
  found by someone typing `help` and meeting a name nothing explains.
  Where a program declares a `cmd_usage()` string the page must carry it
  verbatim, so a flag added to the program and not to the page fails the
  build; the PROSE is deliberately unchecked, since that is the part
  only a person can write.

  **BOTH FORMS OF THAT STRING COUNT, and for a while only one did.** The
  match was a string LITERAL, so the four programs passing a named
  `USAGE` constant -- `mount`, `umount`, `grep`, `mkpart` -- were skipped
  in silence, indistinguishable in the output from a program with no
  usage at all. `mount`'s source argument then grew a whole new form (a
  device name) with nothing comparing the page against the program,
  which is exactly what this check exists to catch. A named constant is
  resolved now, including one built from several adjacent literals, and
  the page's four-space code-block indent is accepted on either side.

  **And every tool must be named in `docs/tools.md`.** When only
  CLAUDE.md's list was checked, `multidisk_test.py` passed the gate while
  missing from the full reference; that list is gone from CLAUDE.md now
  (it cost always-loaded context, and each runner's `--list` is the live
  answer), so this file is the one the check reads. The `/bin` list comes from the SEED TREE
  rather than from `userland/bin/*.c`, because the Makefile renames some
  programs on the way in and the name on disk is the name people type --
  which also means this check is skipped in a checkout that has not run
  `make iso`. Exemptions are named with reasons in
  `COMMAND_PAGE_EXEMPT`.
- **`gen_next_up.py`** -- regenerates the "Next up" section at the top
  of `docs/roadmap.md` from the `**NEXT**` markers on the items
  themselves.

  **WHY THERE IS A SECOND AXIS.** The roadmap is ordered by DEPENDENCY,
  which is what makes reading it top to bottom answer "what next" for
  the phases -- and deliberately not by priority, since a total priority
  order would imply the tracks depend on each other when they do not. So
  urgency cannot share that ordering, and an item states its own where
  it lives.

  **WHY IT IS GENERATED**, rather than a short hand-kept list of titles
  at the top: that list is a pointer whose correctness depends on
  somebody remembering to update a second place, which is the shape of
  every maintenance burden this repo has deleted (CLAUDE.md's "prefer
  facts that cannot go stale"). `check_docs.py` runs it with `--check`
  and fails the build when the committed section is stale, exactly as it
  already does for the decisions index.

  It collects only UNFINISHED items: a marker that survived a tick would
  put completed work in the queue, and nobody remembers to remove one.

  **The generated block is EXEMPT from `check_docs.py`'s one-line cap.**
  This copies each marked item and appends its section name, so an item
  that fits when typed can fail as its own generated copy -- a rule
  punishing the wrong line, in a file nobody edits by hand. The cap is
  about what somebody writes; both halves are still checked at the
  source item.
- **`gen_commands_index.py`** -- regenerates the categorised index of
  command pages into `docs/commands/README.md`, the same shape (and for
  the same reason) as `gen_decisions_index.py`. **The category comes
  from each PAGE**, a `**Category:**` line near its top -- not from a
  table in the script, which would be the central list the settings
  registry deliberately does not have, where a new command means editing
  two files and the one nobody edits is the table. A page with no
  category is reported rather than filed under a default, since a
  catch-all is the pile nobody reads. `check_docs.py` runs it with
  `--check` and fails the build when the committed index is stale: the
  coverage check refuses a command with no page, and this refuses a page
  nobody can find.
- **`check_deps.py`** -- proves the build's header dependency tracking
  is actually live: touches one header per build directory (discovered
  from `build/`, not listed, so a new source directory is covered as
  soon as it's been built once), asks `make all -n` what it would
  rebuild, and fails on any directory that answers "nothing". Restores
  mtimes, so a run changes nothing. Exists because that tracking broke
  silently for the whole of `kernel/` when objects moved directories
  and nothing noticed -- a clean build can't observe a stale `.o`, so
  neither could CI. In `preflight.sh` and CI.
- **`preflight.sh`** -- one command running `make clean && make all &&
  make iso` + `check_deps.py` + `check_layout.py` + `boot_smoke_test.py` + `ktest_run.py`
  + `usertest_run.py`
  + a `git status --short` summary (`fs_switch_test.py` is NOT in it --
  that one needs a disk copy and a longer boot cycle, run it yourself
  after `kernel/fs/` changes), so
  "am I safe to deliver?" is one call instead of three run by hand.
  `--skip-clean` skips the initial `make clean`.
  **It refuses to run until this repository has a commit identity of its
  own.** That identity is per-repository, so a CLONE DOES NOT CARRY IT
  and a fresh checkout falls back to the global one -- the real name a
  history rewrite once removed from every commit here, with nothing in
  git warning before the first commit puts it back. What is required is
  that a LOCAL identity is set at all, not that it matches this
  project's own (`toy-os` / `noreply@toy-os.local`, which it notes if
  you differ): demanding the exact value would refuse anyone working on
  a fork, who has every right to commit as themselves. The hazard is
  "you did not decide", not "you chose wrongly". In the gate rather than
  in a hook because `.git/hooks` is not cloned either.
  **It REFUSES to start while a `vm.py` guest is running**, and that
  guard is worth its four lines: `make iso` re-seeds `disk.img` while
  the guest holds a write lock on it, and the first thing to complain is
  `boot_smoke_test` with "qemu exited early" -- which reads as the
  kernel failing to boot, several minutes and a whole clean rebuild
  after the actual mistake. Run `python3 tools/vm.py stop` first.
- **`gui_debug.py`** -- `DebugConsole`, the connection to the guest's
  serial debug console, and the thing to reach for BEFORE pixels: it
  returns facts to assert on rather than an image to interpret.
  **It reads TWO sockets on a `vm.py` guest**: commands and replies on
  the debug console's (`.vm.N.serial`, COM2), and the kernel log on
  `.vm.N.log` (COM1) through a background thread, into the same buffer
  `logs()`/`events()`/`damage_bugs()` have always read -- so a tool
  waiting for an app's log line (`settings: layout ...`) does not
  change. `events()` returns the log lines that arrived since the
  command before its sweep began. On a one-port guest there is no log
  socket and it reads the one wire as before. See docs/decisions.md,
  "The kernel log and the debug console are two serial ports".
  `send(cmd)` runs a debug command, `json(cmd)` parses one with
  `--json`, `sh <cmd>` runs a KERNEL-shell command, and `settle()` waits
  for injected input to drain (never replace it with a fixed sleep --
  the console is asynchronous). `windows()`/`window(title)` ask the WM
  what it is drawing, and `enter_gui()` polls the desktop ready instead
  of sleeping.

  **A REPLY THAT TIMES OUT USED TO SHIFT EVERY LATER REPLY BY ONE.**
  `send()` reads to the next prompt; a command slower than `timeout`
  (a long `dmesg` while a TCG guest is still booting) left its output
  and prompt in flight, the next command stopped at THAT prompt, and
  from then on each check was handed the previous command's answer --
  measured by forcing one timeout: `echo C` answered `BBB`, for the rest
  of the run. It surfaced as checks quoting unrelated output
  (`resolv.conf`, `rectory`) and runs of empty replies, in more than one
  audio tool. A timeout now marks the console out of step, and the next
  `send()` first resyncs on a marker nothing else prints (`_resync()`:
  an unknown command whose REPLY names a unique tag); what it skips goes
  to `log_lines`. Counting owed prompts was tried first and is wrong: a
  guest still booting can drop a typed line, and a prompt that never
  comes then wedges every later read.

  **`widgets()` / `widget_center()` / `widget_at()` answer "where is
  that control"** -- the question this module could not answer at all
  until the client began exporting its widget map (`docs/decisions.md`,
  and `abi/win_proto.h`'s `WIN_REQ_WIDGET`; the compositor cannot see
  inside a window, so it is the CLIENT that reports). `widgets()`
  returns `{name: {x, y, w, h, screen: {x, y}}}` for the frontmost
  window or one named by title, keyed by the app's own `uui_item.name`;
  `widget_center(name)` gives the SCREEN point to click, which is the
  half that otherwise gets re-derived (and got wrong) per tool; and
  `widget_at(x, y)` names what is under a point. `widget_center` RAISES
  on an unknown name and lists what the window does report, because
  returning a plausible `(0, 0)` lands the click on the window corner
  and fails the test somewhere else entirely. An empty map is the
  ordinary answer for a client with no named widgets, not an error.
  **26 tools still hand-parse the `: layout ` lines** for the same
  geometry; converting them is separate work, and `uidemo_test.py`
  cross-checks the two sources against each other.

  **`cursor_shape()` reports what the compositor would DRAW under the
  pointer** -- `gui state --json`'s `cursor.shape`, a `WM_CURSOR_*`
  (`userland/wm/wm_internal.h`), with the frame's edge rules and the
  client's own `WIN_REQ_CURSOR` already resolved against each other.
  That is the check for a client-side resize cursor: the alternative is
  recognising a 15x21 sprite in a screenshot, which cannot tell "the
  client asked for the wrong shape" from "the theme failed to load".
  `key(k, mods="ctrl")` sets the `KEY_MOD_*` bits delivered ALONGSIDE
  the key without re-encoding it, which is what a real keyboard does.

  **`processes()` gives every process as a dict** (pid, ppid, pgid,
  state, cpu, name), parsed from `/bin/ps` run through the kernel's
  shell -- a different reader from whatever ring-3 shell is under test,
  so a broken one cannot make the two agree.
  `processes_named(prefix)` filters it and drops zombies.

  **`cpu` is how you tell a SUSPENDED process from an idle one.** A
  state column says what the kernel thinks; only a number that stops
  advancing says the scheduler agrees, and that distinction is what
  caught a "stopped" process still being scheduled. A zombie is included
  by `processes()` on purpose: filter it out for "is it still running"
  and ask about it separately for "was everything reaped", because
  mixing the two makes every count ambiguous between them. Two tools
  hand-rolled this parser in one session before it moved here, which is
  this file's usual bar.

  **`warp_cursor(qmp, x, y)` puts the REAL cursor somewhere and CONFIRMS
  it arrived**, and `hover_frames(qmp, tmp, rest_at, hover_at)` returns
  the two settled frames a hover check compares. Both exist because
  `gui move` cannot hold a hover: an injected pointer position overrides
  the mouse for the single `wm_run()` iteration that consumes it, and
  the next one reads the driver again and snaps back -- exactly right
  for a click (press and release are edges), useless for a state that
  has to survive a capture. A tool that hovers with `gui move`
  photographs the frame after the pointer left and reports a working
  hover as dead. `warp_cursor` closes the loop (`goto()` is open-loop
  and the WM ACCELERATES the delta, so asking for (300,250) from the
  corner arrives at (450,374)); `hover_frames` adds the two things
  around it -- settled captures, and a REST frame with the pointer
  parked somewhere real, since the cursor sprite is part of the screen.

  **`drag_real(qmp, x0, y0, x1, y1)` is the only drag a ring-3 CLIENT
  actually sees**, and neither of the two obvious ones works on one.
  `gui drag` queues injected positions the WM consumes one per
  iteration, reading the real mouse in between -- so the client gets a
  leave event and no held motion at all. `QMPSession.drag()` sends its
  moves faster than the guest draws frames under TCG, so the WM sees ONE
  position change: measured, a 152px drag reported an identical `mx` on
  every frame from press to release, and the client was never told
  anything moved. This warps the real cursor once per step, CONFIRMED,
  so each step costs the guest a frame and the client sees motion.

  A kernel-side app notices neither problem, because the WM calls its
  `on_press` every tick with the current position -- which is why
  `scrollbar_test.py` passes on `gui drag` and a client test cannot.
  Only a client needs this; both failures look identical from the
  outside, and identical to the app simply ignoring the drag.

  **`gui resize W H` is the only way to resize a window**, and it is in
  the WM rather than here for a reason worth stating: the frame's grip
  needs a real pointer the compositor tracks across frames, and neither
  injected input nor `drag_real` can be one for a frame drag. A check
  that dragged the grip measured a window **988x498 before and after**
  -- unchanged -- and both its halves passed against a terminal that had
  never resized. Every client-side resize behaviour was untestable until
  this existed, which is how a broken one shipped.

  **`warp_confirmed(qmp, x, y, check)` confirms a DIFFERENT claim**, and
  the difference matters: `warp_cursor` proves the pointer is where you
  aimed, not that where you aimed is what you meant. A list row is one
  line tall, so a y computed from a stale origin, a scrolled view or a
  column header lands on a NEIGHBOURING row -- perfectly plausible, so
  the failure reads as "the click did nothing" instead of "the click hit
  the wrong thing". `check` is a callable returning truthy when the app
  itself reports the intended target under the pointer (its hovered row,
  its hot button); it re-warps until that holds. Assert on the return
  value, or a miss passes silently. Poll it with `layout_now()` rather
  than a wait-for-change helper: a layout block is emitted only when it
  CHANGES, so a warp that lands where the pointer already was logs
  nothing at all.

  **`changed_rows(rest, hover, box)` is the band form**, for a control
  whose rows have no reported geometry -- a dropdown popup, a listbox.
  It compares mean brightness per pixel row, so text contributes to both
  frames equally and only a wash moves the number. **Assert the BAND,
  not the change**: one band, containing the pointer, no taller than a
  row. "Something got darker" also passes on a repaint, a scroll, or a
  whole list highlighting at once. Two rules the CALLER still owns: park
  the pointer at one end of the control and measure the other (or the
  sprite's own pixels answer the question), and make sure the hovered
  row is not the SELECTED one, since selection correctly outranks hover
  and hovering the current value measures nothing.
- **`remote_test.py`** -- `telnetd`, `tftpd` and `remote.py` end to end
  against a QEMU guest, seven checks. On demand: it boots its own guest
  and ENABLES services that ship disabled, so it leaves `disk.img` with
  a network shell turned on -- `make clean-disk && make iso` afterwards
  if that matters.

  Three of its checks exist because of a specific way this can pass
  while broken. It asserts a command's OUTPUT and that the command LINE
  is absent, because a broken framer returns the echo and looks fine.
  Its round-trip content is address-derived rather than uniform, since
  a run of identical bytes cannot tell a working block walk from one
  that repeats a block. And one file is an exact multiple of 512 bytes,
  the case needing a final zero-length DATA packet that a hand-written
  TFTP nearly always gets wrong.

  It also encodes init's rule that **an admin `stop` outranks
  `Restart=`**: the test disables and stops both services to establish
  its own precondition, and `enable` alone does not bring them back.
  Getting that wrong reported both services `running` while nothing
  answered the network.

- **`remote.py`** -- drive a toy-os machine over the NETWORK: run
  commands, push and pull files, or open an interactive session.

      python3 tools/remote.py --host 192.168.200.104 exec "lsusb" "dmesg"
      python3 tools/remote.py --host 192.168.200.104 put build/userland/bin/ls /bin/ls
      python3 tools/remote.py --host 192.168.200.104 get /tmp/crash.log ./crash.log
      python3 tools/remote.py --host 192.168.200.104 screenshot shot.png
      python3 tools/remote.py --host 192.168.200.104 shell      # Ctrl-] quits
      python3 tools/remote.py --host 192.168.200.104 sync seed/sync/bin /bin
      python3 tools/remote.py --host 192.168.200.104 --timeout 60 flash build/kernel.bin

  **This is the tool for the BARE-METAL laptop**, which `vm.py` cannot
  reach: `vm.py` drives a QEMU guest through its serial debug console,
  and the laptop has no serial console attached. It is where half of
  `docs/bugs.md` lives -- a USB mouse that will not bind, a power button
  that needs two presses, a garbled product string -- and until this
  existed, investigating any of them meant sitting at the machine. The
  guest side is `/bin/telnetd` and `/bin/tftpd`, both shipped DISABLED
  (`service enable telnetd`).

  **`screenshot` IS THE ONLY WAY TO SEE THAT MACHINE'S SCREEN FROM
  HERE.** `vm.py` can ask QMP for a screendump of a QEMU guest; the
  laptop has no such channel and nobody is sitting in front of it. So
  the capture is taken BY THE MACHINE -- `/bin/screenshot`, an ordinary
  program with no window that asks the compositor for pixels -- and
  fetched over the same TFTP link. `--window`, `--region X,Y,W,H`,
  `--pointer` and `--delay` pass straight through.

  **PNG by default**, because the point is to open it on THIS machine
  and nothing here decodes QOI; `.qoi` in the filename (or `--format`)
  gets the other one. The guest writes the file, so there is no
  host-side conversion step to go wrong. It lands in `/var/tmp` on the
  machine, not `/tmp`, which is a ramfs mount -- a screenshot is worth
  keeping if the fetch fails.

  It is also the one path that exercises the HARDWARE cursor plane:
  `--pointer` there draws a sprite the back buffer does not contain,
  which no emulated setup here reproduces (`guictl state` reports
  `hwcursor: true` on the laptop and `false` under QEMU).

  **`--timeout` MUST OUTLAST THE COMMAND, and the default is 15s.** A
  command that simply runs longer than that raises with the output it
  has so far -- which reads exactly like the command's RESULT rather
  than a truncated capture. A `kbd --timeout 25` recording was read as
  "no keypresses arrived" that way, which is the wrong answer to a
  question about a dead keyboard. Anything with its own duration
  (`kbd`, a sleep, a long `sum`) needs `--timeout` set above it; the
  error message says so now.

  **`flash` REPLACES THE KERNEL ON THE MACHINE'S OWN BOOT PARTITION**,
  which is four commands with one irreversible step in the middle when
  done by hand. It remounts the read-only `/boot` writable, rotates the
  RUNNING kernel to `/boot/boot/kernel.old` so the "previous kernel"
  entry in `grub.cfg` is known-good, writes the new one, and reads a
  sha256 back off the partition, and then **REBOOTS -- which is the
  DEFAULT**, and only ever after that verify succeeds. `--no-reboot`
  leaves the machine up.

  **The default is that way round because NOT rebooting is the
  dangerous state.** A verified kernel that has not been booted leaves
  the machine running the OLD one against the NEW `/lib` -- the exact
  mismatch the rescue session below exists to survive, and the one that
  makes `telnetd` accept a connection and close it. That state used to
  be what you got by FORGETTING a flag, and every session forgot it.
  `--reboot` is still accepted and now says nothing.

  **IT HOLDS ONE SESSION FOR THE WHOLE FLASH, opened before the first
  write.** The comparison each tree needs is a shell -- it lists and
  hashes the remote tree -- and a flash replaces the programs that shell
  is made of. The session used to be opened just before `/lib`, on the
  theory that `/lib` was the only dangerous tree; `/bin` breaks a shell
  SOONER, because its new binaries are linked against libraries that
  have not been sent yet. So the sync opened a fresh session per tree,
  got `connection closed` on the one after `/bin`, and gave up having
  written no kernel -- leaving `/bin` new, `/lib` old and the machine
  reachable only over TFTP. Both laptops ended up there on 2026-09-07.
  A session opened before any write survives all of them and reboots at
  the end.

  **It SYNCS /bin, /lib, /tests and /usr FIRST**, because a kernel is
  half a build: an ABI struct that changes size moves fields under every
  binary compiled against the old one, and the machine then boots
  perfectly and cannot be given an address. `--kernel-only` skips it.

  **AND IT HOLDS A SESSION OPEN ACROSS THE `/lib` SYNC, WHICH IS WHAT
  LETS IT REBOOT ITSELF.** Replacing a shared library under a machine
  still running the OLD kernel kills every LATER spawn the moment the
  two disagree about a struct: `telnetd` accepts a connection and its
  shell dies, so no fresh session can be made and nothing can reboot the
  machine. That stranded both test laptops on 2026-09-07 and needed a
  power button on each. A session opened BEFORE that write is already
  running and survives it, and **`/bin/reboot` is statically linked**
  (the Makefile's third exception, beside `init` and `toywm`) so the one
  thing that session still has to spawn does not go through `/lib`.
  Neither half works alone -- a static `reboot` with no live shell to
  run it from is just as unreachable. Failing to hold the session is
  not fatal: the flash still completes and ends at the old power-button
  instruction.

  **MEASURED, not reasoned.** The pair shipped (`de533a5d`) with the
  rescue path untested, because a build whose `.so` files are unchanged
  never reaches it. A later flash carrying a real `libc.so` change
  (`d5489356`) did: the fresh session died after the `/lib` sync, the
  kernel was verified over TFTP instead, and the held session rebooted
  the machine -- which came back in ~12s with no power button pressed.
  That run is what the two paragraphs above describe; before it they
  were an argument.

  **NEVER PIPE A FLASH.** The exit status of `remote.py flash | tail` is
  TAIL'S, so a flash that FAILED reports success. That is not a
  hypothetical: on 2026-09-07 a flash piped through `tail` ended after
  the `/bin` sync with `sent 97 file(s)` as its last line and exit 0,
  and was reported as done. It had not written the kernel. The same
  flash run un-piped a few hours later printed `FAILED syncing /tests`
  and returned 1 -- the same failure, finally visible.

  Redirect (`> log 2>&1`) rather than pipe: that preserves the status.
  And note that **stderr is unbuffered while stdout is not**, so in such
  a log the error lines appear at the TOP, before the output that
  preceded them; read it by content, not by position.

  **What that failure actually was** is the `/bin`-before-`/lib` hazard
  now handled by the single held session (above) -- the first diagnosis
  here blamed a `timeout` wrapper, which was wrong, and the wrong story
  survived one commit.

  **THE RECOVERY IS TFTP, and it needs no shell.** (Still worth knowing
  even with the session fix: a flash interrupted any other way leaves
  the same state.) TFTP is a separate
  service and keeps answering when `telnetd` cannot spawn:

      python3 tools/remote.py --host H get /boot/boot/kernel.bin /tmp/k
      sha256sum /tmp/k build/kernel.bin     # do they match?
      python3 tools/remote.py --host H put build/kernel.bin /boot/boot/kernel.bin
      python3 tools/remote.py --host H put seed/sync/lib/libc.so /lib/libc.so   # ...and each of the rest

  Read every file back with `get` and compare hashes -- that is the only
  confirmation available with no shell to ask. The machine still needs a
  POWER CYCLE afterwards, because rebooting it is the one thing TFTP
  cannot do and the held session died with the interrupted run.

  **`/etc` IS SYNCED, but NEW FILES ONLY** (`USERLAND_TREES`), so a new
  service descriptor does arrive while the machine's own configuration
  -- which services are enabled, its address -- is never overwritten.
  **A FLASH ADDS AND REPLACES; IT NEVER DELETES.** The sync sends what
  the staging tree has and leaves everything else alone, so a file
  REMOVED from `data/` stays on a machine that was flashed while it
  existed -- with no error and nothing saying so. It bit on 2026-09-20:
  two settings replaced by an effect descriptor were deleted from the
  tree, and the laptop kept showing their rows in System Settings for
  the rest of the session. `rm` the file on the machine (`remote.py
  exec "rm <path>"`); there is no sweep, because "delete what the
  staging tree does not have" would empty `/etc` of everything a
  machine configured for itself.

  **`/etc/settings.d` IS THE EXCEPTION and is overwritten**, because
  those files are not that machine's configuration: they are shipped
  metadata declaring what each setting IS, and a stale copy with no
  `Type=` line declares nothing -- every desktop setting would vanish
  from System Settings on a flashed machine while the newly added ones
  appeared. It is its own tree, ahead of `etc`, which is also how the
  disk image has always treated it (`seed/sync`, content-hash synced).
  What NEW FILES ONLY means for an INTERRUPTED flash is worth knowing,
  because it is how this was misdiagnosed once: the trees go `bin`,
  `tests`, `usr`, `etc/settings.d`, `etc`, `lib`, so a run cut short
  after `/bin` leaves NEITHER the new `/etc` file NOR the new `/lib`. The missing service then looks like a
  policy ("flash must not touch /etc") rather than like the half-copy it
  is. If you are repairing one by hand, push the descriptor too:
  `put data/etc/services.d/<name> /etc/services.d/<name>`.

  **It REFUSES while `grub.cfg` says `set timeout=0`.** That is the
  whole safety argument: the rescue entry exists on every installed
  machine, and with no timeout GRUB draws no menu, so it cannot be
  picked and a bad kernel needs a USB stick. Fixing that is a one-line
  edit to `/boot/boot/grub/grub.cfg` and deliberately not something
  this does for you -- see `docs/conventions/build.md`.

  **`sync` sends only what differs, and ASKS the machine rather than
  remembering.** It uploads a manifest of `<crc32> <size> <path>` and
  runs `sum -c` over it, so the answer comes from the files that are
  actually there -- a local record of what was last pushed is a second
  source of truth, and the case that matters most (somebody rebuilt and
  did not deploy) is exactly when it would be wrong. Measured against
  the bare-metal laptop: 79 of 86 files, 22.9 MB, 60 s; a second run
  moves nothing.

  Two traps it encodes, both found by its own positive control. **The
  manifest goes over as a FILE because a shell line is 128 bytes** --
  `KLINE_MAX`; the first version batched `sum` by path COUNT, ran past
  the limit, and the line was TRUNCATED mid-path, so every file after
  the cut reported "no such file" and was re-sent: 51 of 86 on a second
  run against a machine that was already correct. And **it creates the
  destination ROOT, not just subdirectories** -- a first sync to a path
  that did not exist wrote every file into nowhere while TFTP reported
  each one as sent, which `/bin` and `/lib` hid by already existing.
  That second one was a real OS bug as well, and it is FIXED: `open()`
  with `O_CREAT` answers -ENOENT for a missing parent now, so tftpd
  sends a TFTP error instead of acknowledging a discarded write. `sync`
  still creates the root, because a correct refusal is not a directory.

  **It is a PUSH, and the pull is the better shape** -- see
  `docs/update-design.md`, which argues for a `/bin/update` that fetches
  a manifest over HTTP so the machine needs nothing listening at all.
  `sync` still earns its place for a machine with no network
  configuration yet, and for a debug loop over one rebuilt binary.

  **One tool rather than "use telnet, then use curl"**, because the two
  halves are always used together and each has a trap that reads as the
  guest being broken: the telnet negotiation has to be answered before
  a command can be sent, and TFTP's reply-from-a-new-port is dropped by
  a stateful firewall (see `docs/commands/tftpd.md`).

  **`exec` frames on a RENDERED LINE, not a substring**, and that is the
  part worth knowing before changing it. It sends a marker `echo` after
  each command and reads until a rendered line equals the marker. Two
  reasons: the shell echoes what it is sent, so matching raw bytes would
  stop at the marker's own echoed command line, before the command it is
  framing has run; and the shared line editor repaints the whole line
  from column 0 on every keystroke, so `\r` must be replayed as a SEEK
  rather than stripped -- stripping it concatenates forty partial
  repaints into one line of garbage. A timeout RAISES rather than
  returning what it has, because a partial answer that looks like a
  whole one is the failure this exists to avoid.

  Not a terminal emulator: a full-screen program (`edit`, `less`) is not
  usable through `exec`. Use `shell` for those.

- **`gui_flow.py`** -- named, composable QMP click-flows on top of
  `qmp_test.py`'s `QMPSession` (`GuiFlow` class: `enter_gui()`,
  `open_app(name)`, `run_system_action(label)`, `screenshot_named()`),
  so a testing session doesn't hand-derive Start-menu row pixel math
  from scratch every time.

  **Rows are found BY LABEL, from the kernel's own geometry** -- there
  is no list of apps in the file to keep in sync, and there was one
  until 2026-08-20. `APP_ORDER` mirrored the WM's desktop entries and
  the menu's top edge was DERIVED from its length, so when `Crash Test`
  was added to `/usr/wm/applications/` and not to the list, two things broke
  at once: every later app's index, and the computed origin. The visible
  symptom was `open_app("System Settings")` opening Task Manager.

  **Pass your own `console=` if you already hold a `DebugConsole`.** Two
  of them are two connections to one serial socket and the guest's reply
  goes to whichever is reading -- which presented as "the Start menu did
  not open" on a menu that was demonstrably open. `open_start_menu()` is
  idempotent (the Start button TOGGLES) and recalibrates the pointer
  first, so it is safe as the first thing a script does.

  For most purposes prefer `DebugConsole.open_app(name)`, which sends
  `gui open <name>` and involves no menu, no pixels and nothing to
  drift. `GuiFlow.open_app()` is for a test that wants the real menu
  exercised rather than bypassed -- which is why it still exists.

  **THE NAME IS THE DESKTOP ENTRY'S, INCLUDING ITS CASE**: "Terminal",
  not "terminal". `gui open` answers a wrong name with `no app named
  "x". Known apps: ...` and returns normally, so a caller that ignored
  the reply got a desktop with no window and a failure several checks
  later, on something unrelated. `DebugConsole.open_app()` RAISES on
  that now, quoting the guest's own list -- one line instead of a
  debugging round.
- **`vm.py stop` ASKS THE GUEST TO SHUT DOWN, and only then signals
  QEMU** -- QMP `system_powerdown`, waited for, with SIGTERM as an
  unconditional fallback so stopping a VM can never itself hang.
  `--hard` skips straight to the signal, for a guest wedged badly enough
  not to act on ACPI.

  **It is not politeness, it is the gate's correctness.** Terminating
  QEMU is a power cut from the guest's side, and `tfs3` writes its
  allocation bitmap unjournaled and set-before-use, so a crash between
  marking a block allocated and committing the transaction that
  references it LEAKS that block -- deliberately, because the other
  ordering risks double-allocating it. `fsck` then reports the debris
  and the three `fs` KTESTs that assert `leaked == 0` go red. That is
  how `preflight.sh` came to fail, on the first run and on a freshly
  seeded disk, a suite it had dirtied itself: it boots the image in
  `boot_smoke_test.py` and then runs `ktest_run.py`.
  `boot_smoke_test.py` shuts down the same way, over a **unix** QMP
  socket -- no `port_guard` slot, so it cannot clash with another guest.
  See `docs/decisions.md`.
- **A `vm.py` guest has TWO serial ports**: COM1, the kernel log alone,
  on `.vm.N.log`; COM2, the debug console, on `.vm.N.serial` -- the name
  every tool already connects to. `--instance N` derives both. A tool
  that builds its own one-port QEMU line still works: the kernel keeps
  both on COM1 when it finds no second UART.
- **`vm.py --serial-log PATH`** -- everything the kernel LOGS (COM1),
  from the first byte, whether or not anything reads the log socket. **A `start` that fails says WHICH failure now**: QEMU
  EXITED (with `-no-reboot`, the guest reset -- a crash) or still
  running (a hang). Both used to print "never reached the debug
  console", and the serial output of either went nowhere; this is what
  found the boot livelock in `clocksource_register()` that only a dozen
  guests booting at once could provoke.
- **`vm.py --machine <type>`** -- the QEMU CHIPSET, default i440fx
  because that is what every existing test was written against.
  `--machine q35` is the only way to reach an ACPI 2.0-era machine here:
  i440fx presents a revision-0 RSDP, an RSDT, a 116-byte FADT and NO
  reset register, while q35 presents a 244-byte revision-3 FADT with a
  real reset register at port 0xcf9 and an MCFG beside it -- so the two
  exercise different halves of `kernel/acpi/`. **q35 has no legacy IDE
  at all**, so it implies `--disk-kind ahci` rather than silently
  booting a machine with no disk, which would read as a kernel bug.
- **`remote.py flash` REFUSES A STALE STAGING TREE**, the way
  `iso_guard.py` refuses a stale ISO, and for the same reason. It sends
  from `seed/sync`, which `make all` does NOT populate -- the `seed`
  target that `make iso` runs does. A flash after a bare `make all`
  therefore ships the PREVIOUS build's userland, and the sync cannot
  save you: it compares the machine against that stale staging and
  truthfully reports no differences. It is a clean-looking flash that
  changed nothing. `TOYOS_ALLOW_STALE_ISO=1` bypasses it.
  **`--force` is NOT the fix for a stale userland** and was believed to
  be for most of a session: the comparison is sound -- measured, a
  single flipped byte at unchanged size in a 512 KB file is detected and
  re-sent, and a corrupted `/bin` binary is repaired by a plain sync.
  What made `--force` look like the cure is that it was the only path
  that got PAST the early return described below. Reach for it when you
  suspect the MACHINE, not when you suspect the build.
- **A MATCHING KERNEL SKIPS THE KERNEL WRITE AND NOTHING ELSE.** A flash
  whose kernel already matched used to return before syncing `/bin`,
  `/lib`, `/tests` and `/usr`, printing `that kernel is already
  installed` -- a flash that silently changed nothing and read like
  success (`docs/bugs.md`). It now leaves `/boot` alone and syncs the
  userland as usual, which is what makes an ordinary incremental flash
  send exactly the files that changed.
- **`vm.py put <host-file> [guest-path]`** -- copies a file INTO the
  running guest, over TFTP, defaulting to `/tmp/<name>`. It closes the
  asymmetry that `remote.py` had put/get/sync for the bare-metal machine
  while the VM side had no file transfer at all: planting a file in a
  guest meant seeding a disk image from the host and rebooting, which is
  fine for a fixture decided before boot and useless for anything a test
  wants to put there mid-run.
  **The transfer is `remote.py`'s `do_put()`, not a second copy of it**
  -- the same TFTP client, already carrying the blksize/windowsize
  negotiation and the retries. What differs is only how the guest is
  reached: a QEMU hostfwd onto 127.0.0.1 instead of the laptop's
  address, on a port derived from `--instance` like every other
  per-slot resource, and added to EVERY launch so this works against
  any guest `vm.py` started rather than one somebody remembered to
  forward a port for.
  **It starts `tftpd` in the guest if it is not running**, because it is
  not a service in the default image -- `/etc/services.d` has none, the
  bare-metal laptop enables it and a QEMU guest does not. Spawned rather
  than enabled: a service would persist into the next boot and change
  what every other tool is testing.
  It needs networking, so `--net none` has no path for it.
- **`vm.py spawn <path> [args]`** -- spawns a guest program and prints
  the FILE it writes its report to, waiting until that file stops
  changing. It replaces a three-command dance that was hand-rolled four
  times in one session: `exec "spawn ..."`, sleep a guessed number of
  seconds, `exec "cat /tmp/....out"`.
  **Why a file rather than the console**: a SPAWNED program's output
  arrives while this harness is between commands, where it is dropped --
  so every spawned test here writes its verdict to `/tmp` and the
  harness reads that. Waiting on the artifact rather than on a sleep is
  this repo's own rule, and it is what makes this reliable rather than
  merely shorter.
  **Why spawn rather than `run`**: the legacy `run` loader has no
  scheduler slot, so a program that blocks (on a pipe, on a child) or
  asks for its own pid (`clock()`) cannot work under it.
  It STRIPS the kernel's own log lines before deciding whether the file
  has appeared -- without that, `elf_run:`/`syscall:` noise around every
  `cat` looks like stable output and the first version returned three
  kernel lines for a benchmark that had not finished running.
- **`vm.started_ok(output)`** -- the one line every caller uses to decide
  whether a `vm.py ... start` worked, and it exists because the obvious
  test is wrong in a way that looks right. `vm.py` answers a slot that is
  already taken with `vm: already running`, and **"al-ready" contains
  "ready"** -- so `"ready" in output` passes on the one output that means
  the opposite, after which the tool talks to a serial socket that is not
  there and fails several steps later, pointing at whatever it was doing
  by then. Eight tools here carried that test; there is one place to be
  right now. Import it as `import vm as vm_mod` (the module runs nothing
  at import; its CLI is behind `__main__`).

  The sibling hazard, which no helper can fix: **a stopped guest keeps
  its QMP port for about a minute**, because a `QMPSession` leaves the
  connection in TIME_WAIT on the server side and `port_guard` -- rightly
  -- refuses a start against a held port. A tool that boots more than
  once should take a **fresh slot per boot**
  (`port_guard.find_free_instance()`) rather than pinning one and waiting
  it out.
- **`ansi_cursor_test.py`** -- ANSI cursor movement and erasing, checked
  as PIXELS. `kernel/lib/ansi.c` is a pure state machine whose KTESTs
  assert what a sequence RESOLVES to with no display at all; this is the
  other half, whether `vga.c` then puts ink in the right cell. **The
  desktop owns the screen, so it removes the toywm service descriptor
  and kills it first** -- the same unsupervise-then-kill pattern
  `compositor_death_test.py` uses, and for the same reason.
  **It SELF-CALIBRATES its cell size from the pattern it drew**, because
  this project's layout is font-derived and a hardcoded 8x16 rots the
  day the default font size moves. It did: the first version assumed 16
  against a real 14, which presented as every row after the third being
  "one row high" while the kernel was correct throughout. Every check
  has a BLANK NEIGHBOUR -- "something was drawn" is satisfied by a
  console that ignores cursor movement entirely. Run on demand.
- **`shell_flow.py`** -- the same idea as `gui_flow.py`, for the
  PHYSICAL (pre-`gui`) shell instead of the GUI (though
  `type_command()` alone drives anything keystrokes reach, the GUI
  Terminal included): `ShellFlow.
  run_command(cmd, subdir=...)` types a full command -- including
  spaces/hyphens/underscores/a few other punctuation chars
  `qmp_test.py`'s `send_text()` can't handle on its own, and
  **UPPERCASE, which `send_text()` silently DROPS** (typing `PATH`
  through it produces nothing at all; `ShellFlow` sends shift+key) --
  presses
  Enter, waits, and screenshots, instead of hand-interleaving
  `send_text()`/`send_key('spc')`/`combo(['shift','minus'])` calls
  character by character every session (a real mistake -- a dropped
  space, a hyphen typed where an underscore was needed -- happened
  twice in the session this was built in). Returns a screenshot path,
  not parsed text: this kernel's console auto-selects a framebuffer
  backend (glyphs drawn as pixels) whenever GRUB provides one, which
  is the normal case for this project's QEMU launch flags, so there's
  no legacy-VGA-text-buffer memory-read shortcut to plain text the way
  there might be on a kernel that only ever used 0xB8000 -- see the
  module's own docstring.
- **`check_layout.py`** -- verifies `disk.img`'s directory structure
  matches `docs/filesystem-layout.md`'s table, which is the source of
  truth for where things live on the OS's own filesystem. Runs in
  `preflight.sh` and CI. Fails in both directions (an undocumented
  directory on the image, or a documented-as-present one missing), and
  understands the table's "Created by" column -- a `build`-created
  directory must exist on a freshly built image, a `boot`-created one
  needn't until the OS has run. **Read that doc before adding a
  directory, a config file, or any new seeded data**: it also records
  the budgets (64-byte caller-side path buffers everywhere; the
  256-record table on TFS2 images only -- TFS3, the only
  since Milestone 15, has ~590k inodes) and the `sync`-never-deletes
  trap that makes moving a seeded file need an explicit cleanup. It
  also WARNS (never fails) about orphans -- a file in a seeded directory
  that `seed/sync/` no longer places there, i.e. exactly that trap
  having already happened -- and prints the `delete` commands to fix it.
  That check found all four ring-3 GUI apps still sitting in `/tests`
  months after they moved to `/bin`, each frozen at the build that put
  them there. **It fired again in 2026-08-16, and that time the orphans
  were LOAD-BEARING**: six stale binaries at `/bin/<name>` were what
  eight GUI test tools had been spawning, long after seeding moved them
  to `/bin/wm/{system,apps,demos}/`. Deleting the orphans (the remedy
  the tool prints) turned the suite red instantly. The right repair is
  to point the tools at the CURRENT path, not to keep the stale copy --
  but the lesson generalises: **an orphan the tool reports may be
  something you are still using, so re-run `gui_regress.py` after acting
  on that warning**, and treat a test that still works after a file
  moved as evidence it is testing the old copy.
- **`usertest_run.py`** -- runs the self-checking ring-3 diagnostics in
  `/tests` (`libc_test`, `fpu_test`, `klineedit_test`, `newsyscalls_test`,
  `file_test`, `write_test`, `exit_test`, `random_test`, `memtest`,
  `guard_test`, `malloc_test`, `wrap_test`, `focusring_test`) as one
  pass/fail table, asserting BOTH an
  exit code and required output. In `preflight.sh`. It fills a real gap:
  `make test` runs inside the kernel and `gui_regress.py` covers the
  windowed clients, so nothing ever ran a plain `/tests` binary except a
  person typing `run <name>`. **Read its `EXCLUDED` list before adding
  to it** -- a test that faults on purpose, blocks on the serial port,
  needs a desktop, or needs a parent to spawn it will fail in a way that
  says nothing about the code under test. `pipe_test` is the worked
  example: it exits 3 under `run` because its `waitpid` finds no parent,
  and passes fine under the KTEST that spawns it properly.
  **`--serial-log PATH` keeps the guest's serial output** (vm.py's own
  flag, passed through) -- the one record left when the EMULATOR hangs,
  since the table is only printed at the end.
  **Verdicts are read with the debug console's `readfile`, not `sh
  cat`** (`vm.parse_framed()`): one frame, taken by its declared length,
  with preemption off and the kernel log held off the wire while it goes
  out. `cat` crossed the wire in chunks and a log line between two of
  them tore verdict lines -- 4 full runs in 25 reported a passing test
  as failed. Any tool reading a file off the guest should do the same.
  **An expected exit code of `None` means SPAWN IT INSTEAD**, and judge
  it by what it printed. `run` uses the legacy loader, which has no
  scheduler slot, so anything reaching the window server is refused
  there -- `wrap_test` needs a real font (`ugfx_font_init()` goes
  through `SYS_WIN_REQUEST`) and measures nothing at all under `run`,
  where every width comes back 0 and every check passes for the wrong
  reason. A spawned test has no exit code to read, so its printed
  verdict carries the whole assertion, and it writes that verdict to
  `/tmp/<name>.out`: a spawned program's console output arrives while
  the harness is between commands, where it is dropped, so the harness
  waits on the ARTIFACT rather than on the timing. **That file is
  `UTEST_VERDICT_FILE`'s now, not each test's own** -- every self-checking
  `/tests` program reports through `userland/lib/utest.h`, which streams
  each line to the file as it is produced rather than buffering the
  transcript and writing it at the end, so a test that dies part-way
  leaves the lines it reached. **The table therefore states its expected
  strings only where a test DEVIATES from the harness's one epilogue**;
  `None` in both string columns means the default, which is what took it
  from a string per test to a handful of exceptions. `focusring_test` is
  the same shape and covers the eleven widgets that accept keyboard
  focus, drawing each into a plain surface and counting accent pixels
  -- **both ways**, absent unfocused and present focused, since a
  one-sided check passes on a control that rings itself
  unconditionally. Its load-bearing checks are the ROW ones: a ring
  round the whole box and a ring on the selected row both put accent
  pixels down, and only the height tells them apart.
- **`/bin/mkfiles`** (a guest program, not a host tool, but this is
  where anyone looks for it) -- fills a directory with N files to test
  the filesystem at scale: `mkfiles /big 5000`, `mkfiles /docs 100 512`,
  `mkfiles /docs 100 0-64000`, and `mkfiles --verify` to read them back.
  Content is derived from (file index, offset), so `--verify` proves
  every file still holds ITS OWN bytes -- a constant fill cannot detect
  two files sharing a block, since both read back the constant and look
  perfect. Sizes in a range are derived from the index too, so a verify
  reproduces them without being told a seed. **Pass `vm.py --timeout`**:
  the default is 30s and 5,000 files takes ~50s. It found the
  binary-write truncation the day it was written.
- **`serial_console.py`** -- boot a guest with COM1 as a SOCKET and drive
  it as text in / text out. Not a test: the shared channel under
  `ktest_run.py` and `faulttest_run.py`, which had written half of it
  each. A serial console is the standard answer to "the graphical
  session owns the keyboard", the same reason Linux developers drive a
  guest with `console=ttyS0` -- it does not care who holds the screen.
  Two things it adds over a raw socket. It launches with **`wait=on`**,
  so QEMU blocks until the harness connects and the transcript starts at
  the kernel's first byte (with `nowait` the banner a caller waits for
  has already gone, which hung every run until the timeout) -- which is
  also why it cannot use `qmp_test.py`'s `launch_qemu_cmd()`, whose
  `-daemonize` never returns for a QEMU blocked on `accept()`. And it
  turns a dead wire into a REPORT rather than a traceback:
  `diagnostics()` gives elapsed time, bytes received, whether QEMU is
  alive and with what code, the tail of QEMU's own log, and the last
  thing the guest said. "0 bytes, QEMU exited 1, could not bind" and
  "40 KB, QEMU healthy, guest never reached the banner" are different
  bugs that used to print the same sentence. **Sample `diagnostics()`
  while the guest is still up** -- after teardown every failure reports
  "QEMU exited with code 0", which is the harness's own kill.
  **`send()` waits for the previous command's prompt** (bounded): the
  console is a terminal, so a line typed while a command runs is that
  command's INPUT -- `ktest` sent under a running `sh fsck repair`
  arrived as `nt`. A string printed during a command is not the
  boundary; the prompt is. `prompt_wait=0` types at a running command
  on purpose.
- **`faulttest_run.py`** -- the ring-3 diagnostics that FAULT ON
  PURPOSE, which `usertest_run.py` correctly excludes and which
  therefore nothing ran at all. A faulting binary has no exit code and
  no output of its own, so the assertion is the KERNEL's report, read
  out of the serial log: each entry names required AND forbidden
  substrings, which is where the value is -- a stack overflow and a
  null dereference are both page faults, and every entry doubles as the
  positive control for its neighbours. Each test gets its own QEMU,
  because a ring-3 crash takes the debug console's command loop down
  with it. **The command goes over COM1, not the keyboard**
  (`serial_console.py`), and that is what makes it work at all: it used
  to type at the physical shell over QMP, which stopped working the day
  the desktop began starting at boot -- a compositor holding the role
  parks every ring-0 blocking reader, so the keystrokes went to the
  desktop and all three entries failed identically having never run.
  **Assertions match only what arrives AFTER the command**, anchored on
  the guest going quiet rather than on the console's banner: the banner
  lands early and the kernel keeps printing for seconds afterwards, so a
  banner-anchored window still contains boot output and an entry
  requiring a boot string passed vacuously (measured, on a control).
  Not in `gui_regress.py`; run it after touching the fault path, the ELF
  loader, or the user address-space layout. `stack_smash_test` is
  deliberately absent -- its message goes to the process's stdout, i.e.
  the screen, so there is nothing in the log to assert on.
- **`mem_stress.py`** -- several memory hogs at once: does the machine
  survive running out, and is the memory each one got actually its own?
  Drives `/tests/memtest`, which takes a DEFAULT 64 MiB (the bound is
  ~2046 MiB of address space now, and taking all of it would mean trying
  to allocate the machine), writes a pattern derived from the ADDRESS
  plus a random per-process salt, and reads it back. The heap LIMIT is
  still asserted, by asking for a terabyte and requiring the refusal --
  a cap silently stops covering a bound unless something else does. **The address-derived pattern is the whole design**: a constant
  fill cannot detect two virtual pages sharing one physical frame,
  because both read back the constant and look perfect -- with this,
  the loser reads a value that is a valid pattern for a different
  address or a different salt, and says so. Proven by deliberately
  aliasing every 64th heap page, which it catches and names.
  **`--mem` shrinks the guest so exhaustion is REACHABLE** -- one
  process cannot fill a 2 GiB machine, `-n 8 --mem 128` does it -- and
  the summary says whether memory actually ran out, because a run where
  everything fitted is a different result, not a better one. Ends by
  running `meminfo audit`. `memtest` alone is in `usertest_run.py`, so
  the single-process integrity check runs on every preflight.
- **`frame_balance.py`** -- does a process's teardown balance? Spawns a
  process, lets it exit, and compares the physical allocator's
  free-frame count against the baseline. Two directions, needing
  opposite fixes: DOWN and staying down is a leak; UP is an OVER-FREE,
  which is worse and quieter -- teardown handed back frames the process
  never owned. It found exactly that (an exiting GUI client returning
  pages of kernel `.rodata`, see `docs/decisions.md`). **It boots its
  own VM because an over-free fires only ONCE**: the second exit finds
  those frames already free, so any run that reuses a booted VM
  measures nothing. A `fork` cycle (`/tests/fork_test --forks 20`)
  sits between the control and the GUI cycles: twenty copy-on-write
  children and their parent must return every frame, which is the
  refcount's own balance check. The non-GUI control must stay flat -- if it drifts,
  the fault is in ordinary teardown or in the harness, not in the
  borrowed-mapping path. Run it after touching `vmm.c`'s mapping or
  teardown paths, or after adding any mapping of memory a process does
  not own.
- **`stdin_test.py`** -- blocking stdin (fd 0) and the standalone ring-3
  shell `/bin/tosh`. Three properties, each with a failure the others
  miss: a line typed at tosh RUNS (asserted through the filesystem --
  `file_test` is on tosh's PATH and writes `/filetest.txt`, so the check
  is a round trip from keystroke to key ring to a parked process's
  trapframe to a spawn, and nothing short of the whole path satisfies
  it); an idle tosh is BLOCKED, not spinning (`kstack slots` reports
  state 4, and a spin-poll implementation passes the first check
  perfectly); and the console has exactly ONE reader, checked in both
  directions -- `touch` is a kernel-shell builtin and NOT on tosh's
  PATH, so typing `touch /claimprobe.txt` at tosh must create nothing,
  and after Ctrl-D the same line must work. Without both directions
  "the claim works" and "the claim is stuck on" look identical.
  **Two preconditions it establishes itself**, and both are the point:
  it puts the guest on the US keyboard layout (`sh keyboard us`, or
  `kbd=us` on the GRUB line), because a QMP qcode names a PHYSICAL key
  by its US label and under this OS's `se` default every `/` arrived as
  `-` -- `spawn /bin/tosh` became `spawn -bin-tosh` and the substring
  assertions passed against a file genuinely called `-claimprobe.txt`;
  and it takes the desktop out of init's hands (`rm
  /etc/services.d/toywm`, then kill) because a desktop owns the
  keyboard and a supervised one comes straight back. It therefore edits
  `/etc` -- run it against a throwaway copy of `disk.img`.
  Positive controls, measured: removing `keyboard.c`'s
  `scheduler_wake(SCHED_WAIT_KEY)` reddens three checks, removing
  `keyboard_claim_console()` reddens two, and neither reddens the
  other's.
- **`ls_test.py`** -- `/bin/ls`: ordering, the format flags, colour and
  the listing cap. Fourteen checks against a THROWAWAY copy of
  `disk.img`, into which it stages a 300-entry directory from the host
  with `tfs3_writer.py` -- the cap is 256, so nothing a person could
  reasonably type at a shell reaches that branch, and a test that made a
  handful of files would be green with the whole limit removed. Two of
  the checks exist because their failures are SILENT: `ls` listed a
  40-file directory as 32 entries and simply stopped, and the console's
  ANSI parser being unwired shows up as literal `[1;36m` beside every
  directory name rather than as missing colour (the captured console
  output cannot show colour at all). Positive controls, measured:
  disabling `ansi_feed()`'s dispatch reddens the escape check -- with
  `\x1b[1;36mbin\x1b[0m/` printed in the failure detail -- and dropping
  ls's truncation message reddens the other, each naming its own
  failure.
- **`hires_test.py`** -- a desktop ABOVE the mode this OS boots into by
  default, and whether a ring-3 client window can actually fill it. Two
  constants have to move together and did not: the mode the display
  layer selects (`DISPLAY_MAX_W/H` plus the ladder in
  `kernel/drivers/display/display.c`, and whether a modesetting driver
  picked it up) and `WIN_CLIENT_MAX_W/H` (`abi/win_proto.h`), the
  largest buffer the window server will hand a client. When the second
  is smaller, MAXIMIZE FAILS SILENTLY -- the WM proposes the new content
  size, `resize_window()` refuses, and the window wears full-screen
  chrome around a stale buffer with nothing logged.

  The load-bearing check is a PIXEL, not the WM's own numbers:
  `gui windows --json` reporting w=1920 is second-hand (the WM adopts a
  client's size only when the client acks), so the test samples a point
  far outside any 1280x720 buffer and requires it to change from the
  desktop's background colour to the client's. A taskbar pixel is
  sampled alongside it and must NOT change, so "everything repainted"
  cannot pass it. Positive control, measured: with
  `WIN_CLIENT_MAX_W/H` put back to 1280x720 it reddens exactly three
  checks -- the frame size, the content size and the far-corner pixel --
  and leaves the taskbar neighbour green.

  **It needs an ISO built differently from the one every other tool
  wants**, which is why it is not in `gui_regress.py` or
  `preflight.sh`: the mode is chosen at boot, so it comes from the
  kernel command line.

  ```
  make iso KCMDLINE="video=1920x1080"     # re-seeds disk.img
  python3 tools/vm.py start
  python3 tools/hires_test.py
  python3 tools/vm.py stop
  make iso                                # put the default ISO back
  ```

  `--require-min` (default 1920x1080) FAILS rather than skips when the
  guest is not actually running big enough -- at 1280x720 every
  assertion in the file passes vacuously, which is this repo's "the data
  never reached the code under test" trap exactly.
- **`taskbar_test.py`** -- opens Notepad until the taskbar overflows,
  and asserts the strip never reaches the tray. Eleven checks against
  `gui taskbar --json`, which comes from the SAME `taskbar_layout()`
  that draws the buttons and hit-tests them (`userland/wm/wm_taskbar.c`)
  -- before that fix the debug console was a fourth, independent walk of
  the window list and was already eight pixels wrong, so every test
  click aimed at a reported button centre missed it. Drives every
  threshold the layout has: natural width, shrink-to-fit, the floor, and
  collapsing windows of one application into a counted button with a
  jump-list popup. Slow (each window is a real process, so budget a
  couple of minutes), deliberately NOT in `gui_regress.py`.
  Its positive control is worth reading in the file: the first attempt
  reddened three checks and left the OVERFLOW check green, because the
  layout's placement guard still refused to put a button past the strip
  -- the control never reached the code the check was about. Removing
  the width policy AND the guard reproduces the reported bug at 14
  windows, and the first run is why there is now an unconditional
  assertion that no window is left off the strip.

- **`init_test.py`** -- init as pid 1 AND as a supervisor, end to end:
  the kernel spawns it, it holds pid 1, an idle init is BLOCKED rather
  than spinning, `kill 1` is refused, abandoned children are adopted AND
  reaped, the process table returns to its baseline, `SYS_SLEEP` passes
  its own checks, the target it read matches the settings REGISTRY, the
  desktop is init's child, a service that cannot start is given up on
  without taking the desktop with it, killing the desktop brings it back
  with no shell involved, and `After=`/`Before=` decide the order the
  services are spawned in, and `Ready=notify` makes `After=` wait for a
  service to be USABLE rather than merely spawned. **The readiness
  checks assert on dmesg TIMESTAMPS, not on log order** -- the desktop
  announces itself about 300 ms after it is spawned, and a service
  ordered after it must start at the announcement rather than the
  spawn, which is a difference a reading of the transcript cannot see.
  Its other half is the timeout: `/tests/notready` is a fixture that
  stays alive and never announces anything, so the barrier can be
  watched expiring and its dependent starting anyway -- the property
  that keeps a hung service from being able to leave the machine with
  nothing started. **The ordering checks are written against
  the order they create the files in** -- both groups of three demand
  the REVERSE of it, from opposite ends of the relation, so an init
  ignoring the keys would have to be handed a perfectly reversed
  directory listing twice to pass; a group whose expected order happened
  to match creation order would be no test at all. **The slot count is the assertion** for
  the reaping half -- a zombie nobody reaps is invisible until the table
  fills up, and every individual process behaves perfectly either way.
  Two fixtures are load-bearing: `/tests/orphan_test` must be started
  with `spawn`, not `run` (the legacy loader's children have ppid 0
  already, so there is nothing to orphan), and the crash-loop fixture is
  a descriptor naming a nonexistent binary, written onto the disk COPY
  from the host so the machine boots with it already in place -- as are
  the three readiness descriptors, for the same reason: both barriers
  resolve in the first few seconds, and a descriptor that arrives after
  the boot has nothing left to observe. Two
  positive controls, in its docstring: `heir = 0` in
  `reparent_children()` reddens the two adoption checks, and dropping
  init's give-up reddens the crash-loop one while the desktop check stays
  green. **What it does NOT cover, and says so: the `target=text`
  command-line override**, which needs its own ISO -- `make iso
  KCMDLINE="target=text"`. Not in `gui_regress.py`; run it after
  touching the scheduler's parentage, reaping, `SYS_SLEEP`, the target
  setting, the service descriptors or readiness. **It currently fails 12
  of its checks, deterministically and pre-existing** -- see
  `docs/bugs.md`; a run that reports 19/31 is the known-good state, not
  a regression.
**`enter_gui()` TURNS THE LAYOUT LOG ON**, once, for every tool that
calls it. Toykit apps report their widget geometry so a test can drive
them by asking rather than by guessing pixels, and that report is OFF by
default (`desktop.layout_log`) because it is written every frame and
made `dmesg` unreadable on any machine with a window open. An app reads
the setting when it STARTS, so it has to be on before anything is
launched -- which is what that function is for. A tool that opens a
window without going through it will find its layout polls timing out.

- **`terminal_probe.py`** -- **the GUI Terminal, asked what it actually
  does**: the editing keys, paging, scrolling and clearing. Written
  because answering three ordinary questions about it took FIVE invalid
  runs before one valid one, and every failure was in the harness rather
  than in the OS. It encodes all five: **a fixture smaller than one
  screen cannot test a pager** (the first attempt used a one-screen
  config file, so `less` exited at once and the space landed at a shell
  prompt -- `/tests/sample.txt` exists for this, with every line naming
  its own number); **the taskbar clock ticks once a second**, so any two
  full-screen captures differ regardless -- crop to the CONTENT rect the
  WM reports rather than computing one from a guessed title height;
  **`open_app` wants the entry's exact name** ("Terminal", not
  "terminal"), and a wrong one opens nothing and returns normally;
  **`send_text()` silently drops uppercase and most punctuation**, so
  `echo AB > /f` arrives as `echo  f` -- type through `gui key` instead;
  and **"the frame changed" is not a measurement**, because the caret
  BLINKS -- it reports the PERCENTAGE of the content area that moved,
  where typing ten characters is ~0.2%, a page turn is >20%, and a caret
  alone is ~0.02%. Without that number, `less` not paging at all reads
  as "changed".

  **Two kinds of probe, and the first is the one to reach for.** A
  keystroke that reached the shell and did the right thing leaves
  different BYTES ON DISK, so the keymap probes drive the Terminal with
  keys and assert through the FILESYSTEM -- which no redraw timing can
  fake, and which is how nine editing keys were confirmed working in a
  single run. Pixels are kept for the questions only pixels answer:
  whether the screen cleared (the discriminator is that what is LEFT is
  >90% background -- a clear and a one-line scroll both move a lot of
  pixels), whether it scrolled, whether a pager paged. Both halves lead
  with a CONTROL, and the control doubles as the scale for everything
  after it. `--keys` and `--pixels` run one half. Boots its own VM; not
  in `gui_regress.py`.
- **`kbd_test.py`** -- **`kbd`'s four columns, on both input drivers.**
  Boots twice (PS/2, then `--virtio-input`), types four keys chosen so
  each exercises a different part of the path, and asserts every column
  of the table against what each stage must say: `a` (the plain case),
  `Shift+a` (the same keycode producing a different character, plus a
  modifier key that produces none), `F5` (a `KEY_*` code rather than a
  character), and Up (an extended `0xE0` scancode, whose keycode is NOT
  its wire byte -- the one shape a naive tap gets wrong).

  **THE LOAD-BEARING HALF IS THE SECOND BOOT.** Reading back `1e 30 'a'`
  on PS/2 proves only that the tool can read its own kernel's ring; a tap
  that echoed the wire byte would pass it. The same keys on virtio-input
  must give the SAME keycode and the SAME character with the scancode
  column BLANK -- the input core's whole reason for existing, and not
  something an implementation that is not really reading each stage can
  fake. Stated as its own check at the end rather than left implicit in
  two passing lists.

  **It also checks the OFF state first, before anything arms the tap.**
  `kernel.kbdtap` is off out of the box, so that is the state the system
  ships in and the one that matters most: keys are typed with the tap
  off and the log must be empty, and `kbd` must SAY the tap is off
  rather than print an empty table -- an empty table is what a broken
  tap looks like too. Live mode is then checked from an off tap, so
  arming and disarming are asserted rather than assumed.

  Three positive controls were run and each reddened exactly its own
  claim. Dropping the wire byte (`key_event(..., 0, extended)`) failed
  the five scancode checks and the parity check, leaving every
  keycode/character check green. Making `kbdtap_produced()` a no-op
  failed the character checks in BOTH boots -- and the live-mode Esc
  check with them, which is the sign that check is genuinely coupled to
  the data it tests.

  **The third control fired NOTHING, and fixing that changed the
  program.** Deleting the privacy gate from `kbdtap_key()` -- so the
  kernel records while the switch says off -- left all 40 checks green,
  because `kbd --last` consulted the switch and returned before reading
  the ring. The in-kernel KTEST caught it on the right assertion; this
  tool structurally could not. `--last` reads the ring FIRST now and
  reports a non-empty ring under an "off" switch as a loud anomaly, and
  the same control then reddens four checks across both drivers with
  "10 rows" naming the leak. **A tool that consults a flag before
  looking at the thing the flag describes cannot check the flag.**

  Three harness traps are written into it, all paid for here. **`"ready"
  not in out` matches "al-ready running"**, so the readiness test passes
  on the one output that means the opposite -- matched as `"vm: ready"`.
  **A fresh slot per boot**, because this tool's own `QMPSession` leaves
  the QMP port in TIME_WAIT on the server side for about a minute after
  the guest dies, and `port_guard` correctly refuses the next boot on it;
  pinning both boots to one slot made a socket-lifetime problem look like
  a virtio failure. And **`ps` is read for STATE, not for the name** --
  the kernel shell's `spawn` does not reap, so earlier `kbd` zombies make
  "is kbd running" answer yes forever.

  Runs `kbd --last` through the legacy `run` loader rather than `spawn`,
  because a spawned process's stdout goes to the console framebuffer and
  never reaches the serial socket. Live mode is checked through the
  process table instead, for exactly that reason. Not in
  `gui_regress.py`: it boots twice and prints a table, so there are no
  pixels in it.
- **`keyboard_paths_test.py`** -- **the same keys do the same thing
  whichever driver reported them.** Boots the `text` target twice per
  input path (PS/2, then `--virtio-input`), on the `se` layout, and
  types two lines.

  It exists because that property was NOT true and nothing noticed. The
  layout tables were keyed on AT set-1 scancodes, so the input core had
  to translate evdev DOWN into that encoding for every non-PS/2 device
  -- a hand-kept table pointing the wrong way, with a hole at
  `KEY_102ND`, the ISO key that carries `|` on every Nordic layout. A
  pipeline could be typed on PS/2 and not on virtio-input.

  **THE ORACLE IS THE FILESYSTEM, NOT THE SCREEN**, and that is the
  reusable part. `touch /kb_probe.txt` proves `_` arrived -- which a
  screenshot cannot, because `_` draws NOTHING on the ring-0 console
  (its ink is below `line_h`), so a lost keystroke and an invisible
  glyph look identical. `echo x | touch /kb_pipe.txt` proves `|` arrived
  AND piped: without the pipe the same keystrokes are `echo x touch
  /kb_pipe.txt`, which creates nothing, so the file's existence is the
  whole discrimination. Deliberately no `>` in it -- mixing a redirect
  into a keyboard check makes a failure ambiguous between the two.

  Two harness lessons are written into it. A QMP qcode names a PHYSICAL
  key by its US label, so on `se` every punctuation key must be spelled
  out (`_` is Shift over the key US calls `/`). And **`altgr` is not a
  qcode** -- `alt_r` is; an invalid one is refused by QMP and sends
  nothing, which reads exactly like the guest dropping the key.
- **`debug_tty_test.py`** -- **the serial debug console is a terminal.**
  Attaches to a running guest and types raw bytes at the console's own
  port: `sh cat` reads a typed line, Ctrl-D ends it, Ctrl-C stops a
  `spin_test` that is confirmed still running, Ctrl-Z does NOT strand a
  command, a program that went raw does not leave the line raw, a job a
  command leaves behind (`tosh -c "counter_a &"`) prints NOTHING on the
  port once the prompt is back (it is confirmed alive across the
  window), `sh spawn`'s job is detached from the start, and a spawned
  reader does not take the next command's line. Its docstring lists a
  positive control per check, each verified. Not DebugConsole, because
  it has to speak while a command runs.
- **`ctrlc_test.py`** -- **`Ctrl-C` interrupts the foreground JOB and
  nothing else**, end to end through the real keyboard (stages 0-2 of
  `docs/signals-design.md`). Boots the `text` target twice, the same way
  `console_shell_test.py` does and for the same reasons, because a real
  ring-3 shell on the physical keyboard is the only place this feature
  exists -- the GUI Terminal reads keys as window events, owns no
  console and has no foreground group.

  Twelve checks, and four of them are the ones that matter. A **spinning
  job** dies: `spin_test` writes nothing and makes no syscalls while it
  spins, so the only path that can reach it is the timer-tick delivery
  in `isr_dispatch`. A **two-stage pipeline** dies as a UNIT, which is
  the check process groups exist for -- signalling the foreground *pid*
  instead of the group leaves one stage running and the shell waiting on
  it forever. The **shell survives**, which every other check here would
  pass without. And at an **empty prompt** the key is still a keystroke:
  the kernel must not signal, the byte reaches the line editor, and the
  line is abandoned -- asserted by typing a command, pressing Ctrl-C,
  pressing Enter, and requiring that the command did NOT run, with a
  control that runs the same line uncancelled so "the file is absent"
  cannot pass against a wedged shell.

  **Both halves were confirmed with positive controls.** Making
  `signal_char()` return 0 reddens the three job checks and leaves the
  empty-prompt ones green; replacing `signal_send_group()` with a
  single-pid send reddens exactly the pipeline check.
- **`jobs_test.py`** -- **job control end to end through the real
  keyboard**: `Ctrl-Z` suspends, `jobs` lists, `fg`/`bg` resume, `&`
  backgrounds, and a background reader is stopped rather than served.
  `ctrlc_test.py`'s sibling, deliberately the same shape: the two keys
  go down the same path in `kernel/tty/ldisc.c` and differ only in what
  they do at the end of it, so the tools differ only in what they
  assert. Boots the `text` target twice, for the same reason.

  Five checks carry it. **The job survives AND stops** -- surviving is
  the whole difference from Ctrl-C, but a "suspended" process that keeps
  running is what a missing check in the scheduler's picker looks like,
  and no process list can see it; the CPU column can, because nothing
  that is not scheduled can accrue any. **A pipeline suspends as a unit
  and is ONE job**, which is the check process groups exist for on this
  side too. **`fg` hands the terminal over**, asserted by Ctrl-C'ing the
  resumed job: a resume that forgot the terminal leaves the job running
  perfectly while the key goes to the line editor, which looks like a
  hung job rather than a shell bug. And **a background reader is
  stopped**, asserted twice over -- `cat &` ends up `stopped`, and the
  shell still runs the command typed after it, which is the half a
  process list cannot show. And **a finished background job is reaped
  with NO KEY PRESSED** -- the shell reports and reaps at a prompt, and
  until `SIGCHLD` reached a shell the only thing that produced a prompt
  was a keystroke, so a `&` job that finished while nobody was typing
  sat as a zombie until the next Enter. Asserted by PID rather than
  against an empty table, because the sections before it deliberately
  leave jobs behind -- including a STOPPED `cat`, which cannot be reaped
  at all until something continues it. Its own control is built in: the
  job has to be seen RUNNING first, or "no zombie" is also what a job
  that never started looks like.

  It also counts **zombies**, which nothing else here does: a `fg` that
  waits only for the stage whose status it reports leaves a resumed
  pipeline's other stages unreaped forever, and every other check passes
  while it happens.

  It reads `jobs` output by REDIRECTING it to a file and `cat`ing that
  back over the serial socket -- a builtin prints through the shell's
  own sink, which goes to the physical screen, and reading it back
  through the kernel's `cat` is an independent path to the same bytes.

  **Four positive controls, each reddening a different set.** Deleting
  the SUSP branch in `tty_ldisc_input()` reddens the stopped and
  pipeline checks while "the job is alive" stays GREEN -- correct, and
  why that check is not the interesting one. Removing `job_foreground()`
  from `fg` reddens exactly one: the Ctrl-C after the resume. And making
  `tty_check_background_read()` serve everybody reddens the SIGTTIN
  pair, with `cat` sitting in `block(key)` and the next typed command
  never reaching the shell at all -- which is the keystroke theft, shown
  rather than argued. And taking the `SIGCHLD` handler out of
  `/bin/tosh` reddens the no-keystroke reap and, downstream of the same
  cause, the zombie count -- while all twenty-eight other checks stay
  green, which is what says the new one is measuring something none of
  them could.
- **`console_shell_test.py`** -- a `text` boot reaches a RING-3 shell
  prompt and the kernel shell is not involved (`docs/init-design.md`'s
  stage 4). Eleven checks: init is what started `/bin/tosh`, an idle
  tosh is BLOCKED rather than spinning, a typed line spawns a program
  (asserted through the FILESYSTEM -- `file_test` writes
  `/filetest.txt`), a kernel-shell builtin typed at that prompt creates
  nothing, and Ctrl-D is followed by a fresh prompt rather than a dead
  console. Five more assert REDIRECTION -- `>` to a file, `>>`
  appending rather than truncating (running the same command twice
  must leave TWO copies, since a `>>` that silently truncated looks
  identical to a working `>`), `<` feeding `/tests/catin`, a
  BUILTIN redirecting (tosh's `ls` prints through the shell's own
  sink rather than fd 1, so this is the check that the sink swap
  works), and a failed redirect NOT running the command. Three assert
  PIPELINES -- two stages, three stages (so the loop is exercised
  rather than a special case for two), and a BUILTIN feeding one.
  Five more assert CONSOLE OWNERSHIP, which only exists on this boot --
  under a desktop nobody owns the console and every one of them would
  pass vacuously: `tty` names the shell as the owner, it has a
  foreground group (the invariant that makes Ctrl-C mean anything), the
  keyboard stood ring 0 down because a ring-3 process claimed fd 0 and
  NOT because a compositor took it, `ps` reports the shell as
  `block(child)` while it waits for the very job writing the file, and
  no blocked row reports a bare `block` -- which is what a state column
  that had lost the reason would still satisfy. Read back through a
  FILE, since a /bin program's output goes to the screen tosh owns
  rather than to the serial socket.

  **Two checks cover `dmesg` reaching the kernel log from ring 3**,
  which is the half the `#` prompt cannot prove: SYS_QUERY answers the
  legacy loader too, so a check run there would pass against a build
  where a real scheduled process could not read the log at all. They are
  STRUCTURAL -- every klog line is `[<seconds>] text`, a stamp klog.c
  adds per logical line, and nothing else the shell could emit here has
  it. The first draft looked for a boot line instead and failed against
  a working `dmesg`, because `-n 5` tails the NEWEST lines and those are
  whatever the machine did a moment ago.
  Three more assert the SHARED LINE EDITOR at that prompt --
  Home+Delete editing mid-line, Ctrl-U killing a line before it runs,
  and Up recalling the previous command -- each through a filesystem
  round trip, so a redraw that merely looks plausible cannot satisfy
  them and an append-only editor fails all three. It BOOTS TWICE against
  a disk copy: the first boot sets `system.default_target text` and the
  US keyboard layout, the second is the one under test. Two traps it encodes. **The restart check is a
  second `init: started tosh` in the log, never a changed pid** -- a pid
  is a slot index plus one and slots are reused, so the replacement
  lands in the slot the dead one just left and reports the same number.
  And **`dmesg` is `sh dmesg`**: the debug console has no such command
  of its own, and the first version of this tool "read the log"
  successfully because the serial stream carries live klog lines, so
  recent lines were there and older ones were not. Its positive control
  is recorded honestly in its docstring -- disabling the stand-down
  reddens only the log check, because `keyboard_claim_console()` covers
  the steady state and what the gate removes is the ten-millisecond
  window before that claim exists. Not in `gui_regress.py` (it reboots
  and rewrites `/etc`); run it after touching init's services, the
  console claim, or `apps/apps.c`.
- **`port_guard.py`** -- refuses to start a guest on a QMP or VNC port
  another guest already holds, picks a free slot for callers that ask,
  and is where a tool that DRIVES a guest gets its `--instance N` flag
  (`add_instance_args()` + `resolve_instance()`): one number derives
  both the QMP port and the serial socket, so the two cannot name
  different guests -- which `--qmp-port 4447` with the default socket
  silently did across 47 tools. The legacy pair still works; a lone one
  derives its partner from its slot and prints which; a mix is refused. It exists because **a QMP port clash does not fail as a port
  clash**: everything here defaults to 4445, a second launch silently
  fights the first, and the error lands minutes later as a
  `BrokenPipeError`/`ConnectionResetError` against whichever tool was
  mid-command -- accusing whichever one was unlucky, never the one that
  caused it. Three "failures" in one session (`scrollbar`, then `uapp`
  and `forcequit`) were all this. Wired into the same two places
  `iso_guard.py` is, which are the only two places anything here starts
  a guest: `launch_qemu_cmd()` and `vm.py`'s start path. Two things to
  know. **The check is a BIND, not a connect** -- QEMU's monitor accepts
  ONE client, so a second connect can hang rather than refuse, while a
  bind asks exactly the question QEMU is about to ask. And
  **`find_free_instance()` is NOT a lock**: two callers picking "the
  lowest free slot" in the same instant get the same answer, so it
  narrows the window and `assert_ports_free()` at the launch catches the
  residue. `TOYOS_ALLOW_PORT_CLASH=1` bypasses it deliberately.
- **`window_resize_probe.py`** -- drags a client window's resize grip
  repeatedly and prints what the compositor believes about it after each
  drag: the content size, which buffer and generation it is showing, the
  WM's own measured resize lag, and how many proposals the drag caused.
  **IT USED TO PRINT TWO VIEWS.** The kernel kept its own record of
  every window and `lswin` reported it beside this one, because a
  window-protocol bug was usually the two disagreeing -- reading only
  one of them is how three wrong hypotheses about a resize bug got
  written in an afternoon. Stage 6b deleted the kernel's record, so
  there is one view and nothing to disagree with it.

  `--mode live|outline|auto` sets `desktop.resize_mode` first, because a
  drag is not one path: the WM either repaints the window live or draws
  an outline and proposes once on release, and a bug in one is invisible
  to the other. `--app`/`--title` point it at a different client.

  It needs a guest already running (`vm.py start`), REFUSES an ambiguous
  title rather than picking -- two clients sharing one made it compare
  two different windows and report a disagreement that was its own,
  twice -- and has no verdict, so no runner names it. A diagnostic, like
  `pixel_probe.py` beside it.
- **`pixel_probe.py`** -- reads exact pixel values out of screenshots,
  and tabulates the same points across several (`--compare a.png b.png
  --at 85,100 --at 215,100`), flagging which moved and which didn't.
  This is how `docs/gui-guidelines.md` says to verify a GUI change, and
  the rule exists because a hover state that shifted the background by
  TWO units out of 255 looked entirely plausible in a PNG. Always
  include a point that should NOT change -- half the assertion is the
  neighbour staying put. `--box N` averages a square, for anti-aliased
  edges where a single pixel is a coin toss.
- **`ping_rtt.py`** -- the compositor<->client ROUND TRIP in
  microseconds, read from the ping every client is already sent
  (`gui compositor --json`'s `ping_us_last/max/avg`). Attaches to a
  running guest, opens three apps and waits for a dozen answers. It
  exists because stage 8 of `docs/winserver-ring3-design.md` moved a
  client's events off a kernel queue onto its own ring, and the one
  thing a kernel wake is plausibly better at is latency -- so the move
  carries a number on each side. Quote DIFFERENCES on one host; the
  absolute figure is a TCG guest's. On demand (`ondemand_sweep.py`).
- **`latency_under_io.py`** -- WHAT HEAVY DISK I/O DOES TO DESKTOP
  LATENCY, measured from both ends: the compositor's own `work`/`wake`/
  `ping` distributions (`gui latency --json`, the EFFECT) against the
  kernel's per-syscall stall table (`sh stalls`, the CAUSE). Attaches to
  a running guest, opens three apps, samples a quiet baseline, then
  spawns `/bin/diskbench` and samples again while it runs -- waiting on
  the benchmark's own `done` line rather than on a sleep, since a guest
  under load takes as long as it takes. It is the yardstick
  `docs/roadmap.md`'s interruptible-syscall work is measured against, so
  the point is a BEFORE and an AFTER on one host, never an absolute.
  Exit 1 only when it collected nothing; no threshold on the latency
  itself, because there is no baseline to pick one from yet.
  **`wake` is the number to read**: when another process holds the CPU
  the WM does no work at all, so every frame it eventually runs looks
  fast and only the gap between the wait it asked for and the one it got
  moves -- which is cyclictest's measurement, and it was confirmed here
  (`work` got FASTER under load while `wake` went up tenfold).
  **Give it a TSC clocksource or half the report is floor noise**:
  `python3 tools/vm.py --instance N --kvm --cpu host,+invtsc start` is
  the only way to reach one in this environment, and without it the
  compositor's figures quantise to the PIT's 10 ms while the stall table
  -- which is TSC-timed in the kernel -- resolves either way. A reported
  granularity of **0 means the clock never advanced**, the worst reading
  rather than the best, and the tool says so in words. On demand
  (`ondemand_sweep.py`).
- **`fs_isolation.py`** -- DOES I/O ON ONE PATH SLOW THE FILESYSTEM DOWN
  ON ANOTHER? The yardstick for `docs/fslock-design.md`, whose stages
  are each a claim about who stops waiting for whom -- which
  `latency_under_io.py` cannot see, since the compositor's reads all
  land on `/`. Times a PROBE (`/tests/fslat_bench`: one `stat()` per
  tick, each call timed) on one path alone, then again while a
  `diskbench` LOAD runs on another, and prints count/avg/p99/max both
  ways. Defaults: probe `/tmp` (ramfs), load `/var/tmp` (the disk) --
  stage 2's claim; `--probe-path /etc` asks the same-volume question of
  stages 3-4. **THE PROBE MUST SLEEP BETWEEN CALLS** (`--gap-ms`, default
  1, which the timer rounds to a tick): a tight loop spends nearly all
  its time holding the lock, so under ONE lock it starves the load
  instead of waiting behind it, and the first version of this tool
  reported stage 1 as the isolated one. **It exits 1 if the load
  finished before the probe**, since that arm measured a quiet machine.
  Attaches to a running guest; use `--kvm --cpu host,+invtsc`. Compare
  two builds, never an absolute. On demand (`ondemand_sweep.py`).
  **`--during PROFILE` picks the load's phase**: diskbench starts with
  SEQ-write, which creates its file and so allocates, and a probe
  started at its first progress line measures mostly that -- fslock 3b
  (overwrites only) read as nothing there and ~200x faster
  `--during RND4K-write`. **It follows the LATEST progress line and
  requires the probe to END inside the phase** (exit 1 otherwise): the
  console returns only a long report's tail, and a phase shorter than
  the probe -- SEQ-write of 256 MiB is ~1.5 s under KVM -- measures the
  next one. Size the load so the phase outlasts `--secs`.
- **`kvm_soak.py`** -- the desktop under KVM, across FRESH BOOTS, failing
  on the symptoms that appear only there: a WM frame over a threshold, a
  file that exists but will not read, an incomplete cursor-theme load, a
  varying desktop entry count. Exists because **every other test here
  runs TCG**, and on 2026-08-17 that hid three real bugs at once -- a
  lost-wakeup race in the ATA driver (5s frozen desktop per disk read),
  a 54-read desktop reload (40ms TCG / 2.5s KVM), and a non-re-entrant
  filesystem that silently lost cursor shapes on ~1 boot in 3. Fresh
  boots per round because that last one is intermittent and one clean
  run says nothing. Not in `gui_regress.py` (needs `/dev/kvm`, boots its
  own VMs); run it after touching the disk driver, the filesystem, the
  VFS, the scheduler's preemption handling, or anything the WM reads
  from disk -- and whenever a user reports something this environment
  cannot reproduce. SKIPS loudly without KVM rather than passing
  quietly. **Its workload CHURNS `/usr/wm/applications` on purpose**: the
  desktop only re-reads when that directory changed and a cached read
  never reaches the drive, so without it the tool does almost no disk
  I/O -- verified by disarming the VFS preemption guard entirely and
  still getting four clean rounds. **It also switches the cursor theme
  once per page** between two real themes: the WM reloads a theme only
  when it changes (pushed config), so without that the incomplete-load
  check saw no loads and passed on nothing.
  **ITS SENSITIVITY IS MEASURED, AND IT IS LOW: with every mount lock
  made a no-op, 2 rounds of 6 went red** (System Settings page-faulting
  on what it read); the rest passed. On one vCPU two contexts meet
  inside a backend only when a holder sleeps in a disk wait, which is
  timing. So **use `-n 10` for locking work** (~98% to see a red round
  at that rate), and never read a 3-round pass as proof. It was broken
  for weeks by three harness faults at once -- a sidebar filter left
  over from the tree, a `settings: set` count that missed lines glued
  to a JSON reply, and a QMP client left open so the next round's port
  sat in TIME_WAIT -- and said nothing more useful than "did not come
  up" (fixed 2026-09-24; it now says why).
- **`serial_capture.py`** -- read a running VM's serial console RAW,
  optionally sending one command first. The case `gui_debug.py` cannot
  cover: a command that KILLS the guest. `DebugConsole` is
  request/response, so a panicking kernel never returns a prompt and the
  panic block is discarded as a timeout -- and `capture_panic()` only
  helps while the console still answers. Only ONE reader may hold the
  socket, so drive input through `--gui` (QMP) or this tool's `--send`,
  never a second console. Exits 2 if the capture contains a `PANIC:`,
  because a run that caught one is not a successful test. Pipe it into
  `panic_resolve.py`.
- **`panic_resolve.py`** -- paste a panic (from the log, or typed off a
  photograph) and it names every address in it, RIP and stack scan
  alike, annotating the original lines. It finds the relocation delta
  from the text itself -- from the `kernel relocated +0x...` line if the
  text has one, and **otherwise by DERIVING it from the faulting symbol**:
  the kernel resolves that symbol itself, so `in foo+0xef` beside
  `RIP=0x...` is an nm lookup and a subtraction away from the delta. That
  second path is the one a bare-metal panic usually needs, because a
  panic photographed off a screen has long since scrolled the relocation
  line away -- and doing it by hand is an nm lookup at exactly the moment
  nobody wants one. It PRINTS the arithmetic, so a reader can check it
  rather than trust it. **It checks the BUILD ID first and refuses to
  be quietly wrong**: resolving against a different build gives
  confident, plausible, wrong names -- verified, the address that was
  `try_merge_next` in one report is `rtc_read_local` a few commits
  later. `--elf` points it at a userland ELF for a ring-3 crash, and
  `--delta 0` goes with it -- userland is not relocated, so a live
  ring-3 RIP (from `kstack slots`, say) has no delta in the text for it
  to find. **Reach for this instead of `nm`**: a hand-rolled
  nearest-symbol-below over `nm` output named `sys_thread_detach` for an
  address that DWARF resolves to `sys_yield` at `rt/sys.c:217`, and an
  hour went into reconciling a function the program never calls.
  **`--crash <file>` reads a ring-3 crash report** from `/var/crash`
  (`kernel/proc/crash_report.c`): prints its text header, finds the ELF
  from the report's `program:` line (a Makefile seed rename such as
  `uterm` is looked up, never guessed), and names RIP and every word on
  the saved stack that lands in an executable segment. Get the file off
  a machine with `remote.py get`, or out of a disk image with
  `tfs3_writer.py read`.
- **`qmp_test.py`'s `QMPSession.hmp(cmd)`** -- run a QEMU MONITOR
  command and get its text. **The one oracle the guest cannot fake**:
  every other probe here asks the guest about itself, in code that is
  usually the code under suspicion. `info registers` gives CPL, RIP and
  `HLT` (halted with interrupts enabled means an interrupt that is not
  coming, and CPL says which ring is really executing); `info pic` gives
  the 8259s' `imr`/`isr`/`irr`, where an unacked IRQ is visible and
  nowhere else; `info pci` says what is on the bus before a driver has
  an opinion. Together the first two eliminated the entire interrupt
  layer of the compositor-stall investigation in one command.
  **Sample a distribution, never one reading** -- it stops the vCPU, so
  a mostly-idle guest reads as halted every single time.
- **`build_conf.py`** -- resolves `build.conf` (`<name> = builtin |
  module`) into the source files the Makefile builds as `.ko` modules.
  A name is a file's basename under `kernel/drivers/`, found by walking
  that tree so there is no name-to-path table; an unknown, ambiguous or
  misspelt name is an ERROR rather than a driver silently becoming
  builtin.
- **`gen_modalias.py`** -- writes `/lib/modules/modules.alias` from the
  built `.ko` files (one `pci <vendor> <device> <class> <subclass>
  <progif> <module>` line per PCI match in each module's
  `.pci_drivers`), which is what the kernel loads modules BY at boot --
  Linux's `depmod` output, derived from the objects themselves so it
  cannot disagree with them. It also FAILS THE BUILD on a module whose
  undefined symbols are not all `EXPORT_SYMBOL`s in
  `kernel/core/kexports.c` (the load would be refused at runtime;
  this names the symbol at build time) and on a relocation type the
  loader does not handle (a module compiled without `-mcmodel=large`).
  `modules/unexported.c` is the deliberate exception. Hand-rolled ELF64
  parse, so the build needs no pyelftools.
- **`gen_syms.py`** -- bakes the kernel's function symbol table into the
  image so a panic can name the function instead of printing an address
  nobody can resolve (the kernel relocates itself, so a raw RIP is
  meaningless without the boot log). Same two-pass + `--verify` shape as
  `genrelocs.py`; the blob is deliberately pointer-free so it costs no
  relocations.
- **`crashtest_test.py`** -- the fault paths (9 checks): the app
  enumerates the kernel's fault kinds, kernel faults are refused while
  disarmed, and a ring-3 crash kills the app WITHOUT taking the desktop
  with it. In `gui_regress.py`, which is only safe because the kernel
  half is disarmed unless `faultinject` is on the command line.
- **`player_test.py`** -- the Audio Player, on a machine with **no
  sound device**, which is the default boot and therefore the
  configuration nearly everything sees. That is the point: an audio app
  is most likely to be broken exactly where `usnd_init()` fails, and
  `audio_test.py` -- which boots its own AC97 -- can never see it. It
  asks whether the window opens as a ring-3 client and reports the
  missing device rather than dying, whether the sounds directory is
  listed by PROBING (the count checked against what the host seeded, not
  against the guest's own opinion), whether the transport and both
  scales have ink where the app says they are, and whether `uui_scale`
  takes a real pointer -- clicking the left and right ends of the volume
  track must move the reported volume, which is the new widget's only
  test with a pointer in it. It can see nothing about sound. In
  `gui_regress.py`.
- **`gen_audio.py`** -- generates the WAV files that ship:
  `data/usr/share/sounds/*.wav` (the sound effects the Audio Player
  lists and Minesweeper plays) and `data/tests/sine1k.wav` (the
  FIXTURE). Written here rather than fetched, for the reason the
  wallpapers and icons are: a build-time dependency on somebody's media
  files is the failure that shipped images with no keyboard layouts for
  months, silently. **Every file is deliberately in a DIFFERENT
  format** -- 44.1 kHz stereo 16-bit, 22.05 kHz mono, 48 kHz mono,
  44.1 kHz 8-bit unsigned -- so the shipped data alone exercises each
  branch of `userland/lib/usnd.c`'s rate/channel/width conversion on
  every boot. A set that were all 48 kHz stereo would exercise the
  copy-only fast path and nothing else, forever. `sine1k.wav` is
  separate and is the only one with a measurable property: a STEADY
  1 kHz tone, because `tools/audio_test.py` judges the recording by
  counting zero crossings on the host and the musical files have no
  single frequency to count. No Pillow, no third-party module -- a WAV
  is a 44-byte header and some integers. Its noise burst uses a fixed
  LCG rather than `random`, so a regenerated file is byte-identical and
  a tracked binary does not change for no reason.
- **`gen_imgdata.py`** -- generates the wallpapers and the decoders'
  test vectors: `data/wallpapers/*.jpg` (the desktop backgrounds, drawn
  here rather than committed as somebody's photograph, so the repo owns
  every pixel it carries) and `userland/tests/uimg_vectors.h` (the JPEG
  decoder's test vectors). One tool because both are "a JPEG produced by
  libjpeg for toy-os to read back", and splitting them would leave two
  scripts with the same encoder settings to keep in step. **The vectors'
  reference pixels are LIBJPEG's, not this decoder's**, which is the
  whole point of them: a decoder compared against its own output is
  self-consistent, and so is one with a wrong IDCT constant. Both
  outputs are COMMITTED, like the cursor themes and the baked font, so a
  checkout without Pillow still builds. Nine vectors, chosen for what
  each can fail at on its own: 4:4:4/4:2:2/4:2:0, a 17x9 image whose
  partial MCU catches a decoder that forgets to crop its padding,
  grayscale, restart markers (a decoder ignoring them drifts in BANDS
  after the first interval), and three refusals -- progressive,
  truncated, not-an-image -- which must come back as the RIGHT errno,
  since a decoder answering "broken" to everything would pass a test
  that only asked whether it failed.
- **`gen_icons.py`** -- draws the application icons into `data/icons/`
  (one 64x64 QOI per icon NAME) and `--check`s that the files on disk
  match the script, the same contract `gen_cursors.py` has. The art is
  deliberately simple -- a rounded tile in a per-app hue and a white
  pictogram -- because that is what still reads at 20 pixels in a Start
  menu row, and it is drawn here rather than committed as somebody's
  icon set so the repo owns every pixel it ships. **PILLOW encodes
  them**, which is what keeps `uimg_qoi.c`'s DECODER honest: these files
  come from a foreign implementation, so a chunk type the decoder
  misreads cannot round-trip through a matching bug of our own. (The
  repo does write QOI now -- `/bin/screenshot` does -- and its encoder is
  checked the other way round, by Pillow decoding it; see
  `uimg_codec_hostcheck.py`.) **Crash Test gets no
  icon on purpose** -- it is the entry that exercises the letter-tile
  fallback on every boot, the same trick `data/fonts/` plays by shipping
  `vera-mono` with no bold companion.
- **`uimg_hostcheck.py`** -- the same `userland/lib/uimg_jpeg.c` and
  `uimg_jpeg_enc.c`, compiled with the host gcc and run against ~370
  Pillow-generated images (four patterns x five sizes x three
  subsamplings x three qualities, each BOTH sequential and PROGRESSIVE,
  plus grayscale, restart markers and all eight Exif orientations),
  every pixel compared against libjpeg's. **It exists because a guest
  test can only carry the vectors somebody committed**, and a JPEG
  decoder's bugs live in the combinations. It is how the chroma
  upsampler was found to be visibly wrong: replication needed a
  tolerance of 70 to pass, and the triangle filter libjpeg uses brought
  the worst case to 3.

  **`--positive-control`** removes the successive-approximation
  correction bit -- the one place a progressive decode ADJUSTS a
  coefficient rather than assigning it, and the bug that leaves a
  plausible, slightly wrong picture. Every judged progressive check must
  go red and every baseline one stay green. The `gradient` pattern is
  excluded from that verdict, and the reason is about the FIXTURE: a
  smooth ramp quantises to almost no nonzero AC coefficients, so the
  sabotaged line is never reached and five of them decode identically
  without it. Judging them would make the control's bar a measurement of
  how flat the test images are.

  **The ENCODER is checked in two ways, because they fail differently.**
  Our decode of the file we wrote against libjpeg's decode of the same
  bytes, at the decoder's own tolerance of 3 -- two independent decoders
  reading one bitstream must agree to a rounding step. And how much the
  file LOST against how much libjpeg loses encoding the same image at
  the same quality, which is a COMPARISON rather than a threshold on
  purpose: 4:2:0 at q85 moves a 3-pixel checkerboard by 180 levels and
  that is the format working as designed, so an absolute bar loose
  enough to pass it would pass real damage too.

  `--file` checks a real photograph; `--tolerance` tightens the bar. Not
  in any gate: it needs Pillow, and `/tests/uimg_test` is the version
  that runs in the guest.
- **`uimg_codec_hostcheck.py`** -- the QOI and PNG codecs, BOTH WAYS,
  against Pillow and Python's `zlib`.

  **Encoding:** `uimg_qoi.c` and `uimg_png.c` compiled with the host gcc,
  run over eight images chosen for what they do to each format (a flat
  fill for QOI's runs, a gradient for its luma chunks, noise that no
  chunk helps, a UI-shaped image, one with alpha, a 1x1 and a 1x300),
  and every file opened again by **Pillow** -- with each PNG also
  inflated by Python's `zlib` and unfiltered by hand, so three
  implementations that share no code with ours read what we wrote.

  **A foreign decoder is the whole point.** An encoder tested against
  this repo's own decoder passes whenever the two share a mistake, and
  the two mistakes an image encoder actually makes are exactly that
  shape: a QOI index table updated on the wrong chunk, or a Huffman code
  packed least-significant-bit-first. Both produce a file that
  round-trips perfectly here and that nothing else can open.

  **Decoding:** the same argument in reverse -- files PILLOW wrote,
  read by us, compared exactly (PNG is lossless, so there is no
  tolerance to hide behind). Every colour type including palette, each
  row filter forced in turn, and sizes whose stride is not round. Plus
  what it must REFUSE and with which errno: a 16-bit or interlaced file
  is `-ENOTSUP` (the file is fine, this build is not) and a corrupted
  chunk is `-EINVAL`, because an app prints a different sentence for
  each.

  **The interlaced fixture is BUILT BY HAND**, and that is the
  interesting part: Pillow silently ignores `interlace=1` and writes a
  progressive-free file, so asking it for one produced a fixture that
  never reached the code under test and a check that passed for the
  wrong reason. Flipping the IHDR byte and repairing its CRC is what
  actually tests the refusal.

  `--positive-control` carries TWO sabotages in two files -- an
  unreversed Huffman code in the compressor, and the Paeth predictor
  dropped from unfiltering -- and requires encode AND decode to go red
  while QOI stays green. One sabotage reddened only half the harness,
  which meant the other half was untested. `--keep DIR` leaves the
  files to look at. Not in any gate: it needs Pillow, and
  `/tests/uimg_test` is the round-trip that runs in the guest.
- **`fetch_extras.py`** -- the registry of optional, differently-licensed
  material, and the licence acceptance in front of it. `make iso
  EXTRAS=1` runs it; nothing else does, so an ordinary build reaches no
  network and an ordinary image carries nothing but ours.

  **The distinction it is built around is that fetching is not
  distributing.** Downloading a file onto your own machine makes you the
  recipient, and nothing third-party enters this repository -- which is
  why the Doom IWAD is fetched rather than committed. What collapses that
  is publishing a BUILT ARTIFACT: an ISO carrying a fetched WAD, uploaded
  as a release asset, is you distributing the WAD. So an EXTRAS build
  writes `/usr/share/licenses/extras.txt` INTO the image, saying what is
  in there and that publishing it distributes those things -- the one
  note still attached when somebody decides whether to upload it.

  Acceptance is remembered in `.extras-accepted` at the repo root
  (gitignored), keyed by item name AND a hash of the licence summary, so
  a new extra or a changed licence asks again: agreeing to id's shareware
  terms must not silently agree to whatever is added next. It is at the
  ROOT rather than under `build/` because `make clean` wipes that and
  `preflight.sh` starts with one. **A non-tty is REFUSED, never
  prompted** -- a captured build has nothing to answer with and a prompt
  there hangs forever, which is the mtools trap from a different
  direction; `LICENSE=agree` (or `TOYOS_LICENSE=agree`) is how CI says
  yes. `--list` shows what exists and what is present.

  It is a registry rather than a script per item, the same shape
  `display_driver` and `block_device` use: the tenth extra is a row.
- **`usnd_hostcheck.py`** -- the same `userland/lib/usnd_mp3.c`, compiled
  with the host gcc and run against eight lame-encoded files (CBR and
  VBR, mono/stereo/joint stereo, 32 to 320 kbps), every sample compared
  against ffmpeg's decode. Same split as `uimg_hostcheck.py` and for the
  same reason: a guest test carries the vectors somebody committed, and
  a decoder's bugs live in the combinations. **The source it encodes
  carries deliberate clicks**, because a transient is what makes an
  encoder emit SHORT BLOCKS -- three IMDCTs, subblock gains and the
  reorder, which a tone-only fixture leaves entirely untested.

  Two things it taught, both about the harness rather than the decoder.
  **The tolerance was 10 000x too loose**: it was set to 0.02 by eye,
  and `--positive-control` then showed a deliberately broken decoder
  passing two of eight checks under it. Measured agreement is ~1.6e-6
  (one LSB in 32768, which is all ISO/IEC 11172-4 requires -- a
  compliant decoder is defined by error bound, not bit-equality), so the
  default is 1e-3. And **the lag is not always a whole frame**: a file
  with a LAME/Xing tag makes ffmpeg drop the encoder delay, 1105 samples
  rather than a multiple of 1152, so a frame-stepped alignment reported
  the shipped song as badly broken. It searches every sample now.

  `--positive-control` builds the decoder with two Huffman tables
  swapped and REQUIRES every check to go red -- shaped to defeat the
  structural check on purpose, since swapping two codewords of equal
  length leaves the code complete and prefix-free. Not in any gate: it
  needs `lame` and `ffmpeg`.
- **`gen_mp3_tables.py`** -- regenerates
  `userland/lib/usnd_mp3_tables.h`, the three Layer III tables that have
  no generating formula (the Huffman codes, the 512-tap synthesis window,
  the scalefactor band edges). It exists so that data which was TAKEN can
  be CHECKED: without it the header is 473 lines nobody can audit without
  redoing the work, and `loc.py` counts it as code somebody wrote.

  **It verifies far more than it copies.** Two public-domain
  implementations store the Huffman tables in two completely different
  packed formats; this walks BOTH back to the standard's plain (length,
  codeword) per (x, y) form and refuses to write unless they agree entry
  for entry -- 1378 entries across 15 tables -- with every table also
  required to be a complete prefix code. Both checks earned their place:
  an early hand-written table 7 failed the Kraft check, which is how
  transcribing from memory was abandoned, and a first version of the
  pdmp3 walk dropped that format's `>= 250` long-jump escape, which
  surfaced as table 24 disagreeing in 63 of 256 entries -- the walker was
  wrong, not the data, and only having two sources made that visible.

  `--check` verifies without writing (useful for asking whether the
  committed header still matches its sources); `--from DIR` uses local
  copies instead of downloading. Not run by the build and not in any
  gate: it needs the network, and the header it writes is committed.
- **`gen_music.py`** -- generates the MP3s that ship, into
  `data/usr/share/music/` and `data/tests/`. Same call `gen_audio.py`
  made for the WAVs: written rather than fetched, because a build-time
  dependency on somebody else's media is the failure that shipped images
  with no keyboard layouts for months. The output is tracked, so a clean
  checkout has music, and it is ours, so `LICENSE` needs no entry for it.
  **The encode is as much the point as the tune**: `first-boot.mp3` is
  joint stereo VBR with an ID3v2 tag (mid/side frames, a Xing header,
  and a few KiB to skip before the first sync word), while
  `data/tests/sine1k.mp3` is mono CBR with no tag at all -- and it is a
  steady 1 kHz tone because `audio_test.py` judges playback by counting
  zero crossings in QEMU's own recording, which music cannot be judged
  by. Needs `lame`; nothing in the build runs it.
- **`gen_cursors.py`** -- generates the shipped cursor themes into
  `data/cursors/`, which the Makefile's `seed` target stages onto the
  image. **Into `data/`, NOT `seed/sync/`** -- that tree is gitignored
  and `make clean` deletes it, so the first version's themes were never
  committed and every checkout but the authoring one silently got the
  built-in fallback. It is the authoring path for a new theme too
  (a theme is a function returning shape name -> masks). It
  EXTRACTS the default arrow from `userland/wm/wm_render.c`'s own baked
  arrays and ports the procedural resize shapes, so the files on disk
  cannot drift from the built-in fallback they mirror. `--check` fails
  if they are stale.
- **`cursor_theme_test.py`** -- cursor themes end to end (9 checks): the
  theme loads completely, switching it changes the drawn pointer, the
  size setting scales it by the right MAGNITUDE, returning to normal is
  pixel-exact, and a theme that does not exist still leaves a working
  pointer. Read its docstring before editing: the built-in fallback
  means "a cursor is on screen" proves nothing, so every check asserts a
  load count or a pixel difference. It also sets both settings
  explicitly at the start -- they persist to the disk image, so
  inheriting them makes every measurement relative to a silently wrong
  baseline. In `gui_regress.py`.
- **`cursor_ibeam_test.py`** -- named pointer shapes (~20 checks): a
  client naming its own (`WIN_REQ_CURSOR`), the compositor's clamp, and
  the one shape the compositor raises by itself. It covers all four ways
  a shape is named -- a widget's ops table
  (`uui_textbox` in UI Demo, with no app code at all), an app's own
  `uapp_set_cursor()` (Notepad's document), a window-wide constant set
  at open (the Terminal's grid), and the WM's own modal chrome
  (Notepad's Save-as field) -- with a control point beside each: the
  dropdown next to the textbox, the menu bar and scrollbar and status
  bar around the document, the dialog's panel below the field.
  Two things worth knowing before editing it. **The two shapes are told
  apart by where they sit RELATIVE TO THE HOTSPOT**, not by size or by
  ink: the arrow is drawn from the pointer down and right (bbox starts
  at 0,0), the I-beam is centred on it (bbox starts near -3,-8). That is
  what makes a wrong shape unable to pass as the right one, and a
  MISSING one fail both tests rather than one. And **the sprite is
  isolated by a THREE-frame diff** -- away, here, away again, keeping
  only pixels that differ from both away-frames -- because the
  two-frame version reported Notepad's menu bar as an I-beam when the
  menu title under the pointer redrew its hover. Its load-bearing check
  is the clamp: the same window's TITLE BAR, and the taskbar with the
  window deliberately dragged UNDER it, must both read as arrow, which
  a compositor that simply believed the client would fail while every
  other check still passed.
  Its last two phases are the BUSY pointer, and they are built to keep
  its two sources apart. `/tests/hangclient`'s `b` key names
  `WIN_CURSOR_WAIT` and keeps pumping, so the tool asserts the busy
  shape appears WHILE the WM still considers the window healthy --
  without that pairing, "busy appeared" would equally mean the app hung.
  Then `h` wedges it and NOBODY ASKS IT TO CLOSE, which makes that phase
  the ping cadence's test as much as the cursor's: before
  `WM_PING_INTERVAL_DEFAULT` existed, `not_responding` could not become
  true without a close attempt and the check fails outright.
  It shortens both `gui pingtimeout` and `gui pinginterval` (detection
  is interval + timeout, not either alone) and sets them back.
  On demand, not in `gui_regress.py`.
- **`console_bleed_test.py`** -- the kernel console vs the desktop (5
  checks). A ring-3 process writing to fd 1 reaches the framebuffer
  console, which draws into its own back buffer and then BLITS THAT OVER
  THE WHOLE SCREEN -- so `dmesg` covered 100% of the desktop and DOOM's
  startup banner was how it got noticed. TWO properties, and a fix that
  only stopped the console DRAWING would pass one and fail the other:
  nothing bleeds through while the desktop is up, and the accumulated
  text is still there once it goes away. The second is checked against
  the console as it looked BEFORE the desktop started -- "is there any
  text" would pass vacuously, since the boot log is already on that
  screen. The bleed check carries its own control (a real window opening
  MUST move the same pixels), for the reason `idle_desktop_test.py`
  exists. It kills the desktop rather than driving the Start menu, since
  both reach `compositor_gone()` and one of them cannot fail for reasons
  this tool is not about -- and it DELETES `/etc/services.d/toywm` first
  so init cannot restart it mid-check, without restoring it (there is
  nothing in the guest to copy it back from; `make iso` re-seeds it).
  On demand, not in `gui_regress.py`.
- **`font_test.py`** -- runtime fonts end to end (~20 checks): a `.ttf`
  under `/usr/share/fonts` rasterizes, switching faces reaches the
  screen with NO restart (the compositor is told through `WIN_EV_FONT`),
  a size nobody baked works, and the baked font still draws when no face
  is selected. Its second half opens **Font Demo** (now a font
  previewer) and asserts on the numbers that app measures for itself:
  that bold is distinct from regular and that kerning TIGHTENS a sample
  rather than loosening it (both on the SESSION face) -- and the
  interactive half a static demo cannot have: the previewer's size ladder
  loads on open, and DRIVING ITS DROPDOWN to another family renders a
  genuinely different face, proven by the pangram's width changing. The
  bold/kerning checks run on `liberation-sans` deliberately -- on the
  default monospace face bold has the regular advances and there is no
  `kern` table at all, so every one of them would pass vacuously.

  It also PROBES THE DEMO'S PIXELS, and the reason is the lesson: the
  measurement checks above all passed while the demo rendered every
  letter as a hollow outline, because they read numbers out of the
  mapped atlas and nothing looked at the screen. The probe asserts the
  window's DOMINANT colour is the panel background -- not an ink count,
  which was tried first and passed the broken build by a wide margin
  because a black background counts as ink. Read its docstring before editing, for the same reason
  `cursor_theme_test.py`'s says so: the baked font is a complete working
  fallback, so "text is on screen" proves nothing at all -- every check
  is a DIFFERENCE between two states. The load-bearing one is that
  `liberation-sans` (proportional) draws the same right-aligned text
  starting ~50px further right than `dejavu-sans-mono` does; a build
  that ignored per-glyph advances passes every other check in the file
  and fails exactly that one, which was verified by making it do so.
  AND EVERY FONT CHANGE WAITS ON THE COMPOSITOR'S OWN LINE, never on a
  sleep: `set_face()`/`set_size()` go through one helper that waits for
  a `wm: font changed` that is NEW since the spawn. The three hops
  between a setting and a repaint (a short-lived ring-3 process, the
  kernel, the client) take a load-dependent total, and a fixed sleep let
  a change land AFTER the drain that followed it -- so the next wait
  returned the PREVIOUS face's cell and two different faces compared
  equal. The sharper half is that a change still in flight is observed
  by the NEXT call's wait: `set_size(14)` then `set_face("builtin")`
  returned on the size's line and measured the old face, which reads
  exactly like "switching to builtin did nothing".

  Two measurement traps it encodes: measure the RIGHT-ALIGNED version
  text rather than the desktop icon captions (those are clipped to the
  icon cell, so a narrower font mostly just un-truncates them and moves
  three pixels), and crop the frame yourself -- `stable_pixels()` writes
  the whole screen and only COMPARES the box, so scanning its output
  counts the taskbar as ink.

  **Its last section drives `/bin/font`**, the glyph probe -- both
  views, the hash agreement between them, the descender flag on `g`, and
  the discriminating pair that gives the whole thing teeth: `g` has ink
  and a SPACE does not. A probe that always answered "ink yes" passes
  every other check in that section; one that always answered "blank"
  passes the space check alone. It reaches the space by CODEPOINT
  (`0x20`), because a space cannot be passed as an argument through any
  shell here. Two mechanics worth not rediscovering: the output is
  captured through a FILE (`tosh -c ... > /path`), because a spawned
  process's stdout goes to the console it inherited and `gui spawn`
  hands the debug console back only its own "spawned as pid N" line --
  a probe asserting on that would pass on any output at all; and the
  path is unique per call, since a shared one read back after a failed
  spawn returns the PREVIOUS call's output, which is the quietest way a
  check like this can lie.

  **It is in `ondemand_sweep.py`, NOT in `gui_regress.py`, and that is
  the point of the move.** Several of its checks compare what the KERNEL
  reports for a glyph against what a CLIENT draws and require a match --
  which stopped being true when the kernel's font path was deleted, so
  they are red BY DESIGN and no build can turn them green.
  `docs/bugs.md` carries the owed rewrite (ask fontd what the session
  font is; wait on the beacon rather than on `WIN_EV_FONT`). Until then
  it is off the standard gate rather than dropped, because the ~20
  checks around them still test real things -- a gate that always cries
  wolf gets ignored, which is why per-push CI was retired here too.
- **`check_layout.py`** -- see CLAUDE.md's `docs/` section: verifies the built
  image's directories against `docs/filesystem-layout.md`, and warns
  about orphaned seeded files. Runs in `preflight.sh` and CI.
- **`dialog_test.py`** -- verifies the confirm dialog's buttons by
  PIXEL VALUE: hover moves the hovered button and leaves its neighbour
  alone, a press dragged off doesn't commit, No closes it. Three traps
  it encodes: hover needs the REAL cursor parked (use
  `DebugConsole.warp_cursor()` -- `gui move` holds for one WM iteration
  only, and `QMPSession.goto()` is open-loop and undershoots a large
  jump); don't sample the pixel under the cursor sprite; and take the
  button rects from `gui dialog --json`, not by scanning a row for
  THEME_BUTTON_BG, which only ever worked for a Yes/No dialog and
  cannot measure the wider "Force Quit"/"Wait" one.
- **`uidemo_test.py`** -- drives UI Demo's widgets and asserts on its
  log (27 checks: click selection, cancel paths, keyboard navigation,
  Tab/Shift-Tab focus cycling, Space activating a focused button,
  wheel-scrolls-without-selecting, the dropdown popup's open/commit/
  dismiss/Esc, and keyboard focus). Exits non-zero on a failed check.
  Run it after touching anything in `apps/ui/`. Geometry comes from the
  app's own `uidemo: layout ...` lines rather than from re-deriving row
  offsets in Python -- the Python copy drifts silently the moment a row
  is added to the app, which is exactly what happened when the dropdown
  and listbox rows landed mid-file.
- **`calculator_client_test.py`** -- drives the RING-3 Calculator
  (`userland/gui/calculator.c`) and asserts on it (8 checks). Worth reading
  for two techniques: it uses **no OCR** -- every check is a round trip
  (a state change must alter the display's pixels, and returning to the
  same logical state must restore them EXACTLY), which proves rendering
  and arithmetic together and also catches a right number drawn in the
  wrong place; and its last check presses a button, drags OFF it and
  releases, which must NOT commit. That one matters because a client
  acting on button-down passes every other check and fails only that.
  Geometry is derived from the window's reported content size, not
  hardcoded, so it survives a font-size change.
- **`uterm_test.py`** -- drives the RING-3 Terminal, which is a real
  TERMINAL EMULATOR now: a pty with `/bin/tosh` on the far end.

  **Its TAB checks open their own window**, because `run()` ends by
  closing the first one with Alt+F4 -- three of them passed against
  nothing on the first run, reading STALE log lines from the window that
  had been there. The load-bearing one is the SWITCH, not the count: a
  Terminal that drew a strip and pointed both tabs at ONE session passes
  every count and every pixel check, so a marker is typed into the
  second tab and the first is required not to have it. Confirmed by
  control. Its key
  check is worth copying elsewhere: it distinguishes a BUILTIN (`echo
  hi`, handled inside the shell with no spawn) from an EXTERNAL program
  (`lscpu`, dozens of lines that can only appear if it was spawned and
  its stdout piped back) by INK VOLUME. A terminal that echoed commands
  but never captured output passes every other check and fails that one.

  **The check the whole TTY layer exists for is here**: a spinning job is
  started in the window, `Ctrl-C` is sent as the byte `0x03`, and the job
  must die while the SHELL SURVIVES -- the second half is load-bearing,
  since a Ctrl-C that killed the shell too would satisfy the first. Its
  positive control is the best evidence in the repo that this is ONE
  implementation and not two that agree: disabling `signal_char()` in
  `kernel/tty/ldisc.c` reddens this check AND `ctrlc_test.py`'s
  physical-keyboard ones, from the same three lines.

  **`Ctrl-Z` and `fg` are checked here too, and for the sibling reason.**
  0x1A on the same pty, recognised by the same discipline function, so
  what this proves is not the suspension -- `jobs_test.py` does that
  against the physical console -- but that a WINDOW is a terminal in the
  same sense: same foreground group, same job table, same shell binary.
  A difference between the two would mean the tty layer had failed at its
  one job.

  **And the check that says a FULL-SCREEN program works here**: it runs
  `/bin/edit` in the window, types, saves with F2 and exits with F3, then
  reads the file back through a completely different path. Two assertions
  beside it are what make that mean something, because a file gets
  written whatever the screen did with the escape sequences: the CARET
  must be near cell 5 (an editor whose `ESC[1;6H` was printed rather than
  obeyed would have a caret hundreds of cells along), and the status bar
  must be a long unbroken RUN of the reverse-video colour. **Counting
  bar-coloured PIXELS does not work and was tried** -- glyphs are drawn
  in the same grey, so a frame with no bar at all scored 4304. A run
  discriminates: a glyph is a few pixels wide, a bar is hundreds.

  Two premises here went stale in the GOOD direction when the Terminal
  became an emulator, and both are recorded in the file. A bare `cat` now
  WAITS for input instead of returning at once -- it has a real terminal
  to inherit, where before it got a closed pipe -- so the check types
  Ctrl-D to end it, as a person would. And there is no prompt WIDGET to
  report a position for any more, so the layout log reports the
  emulator's CURSOR, which is what would be wrong if `\r` or overwrite
  were mishandled.
- **`notepad_client_test.py`** -- drives the RING-3 Notepad and asserts
  a full round trip: type, save, verify the bytes on disk via `cat` (a
  completely independent path -- the editor claiming success proves
  nothing), clear, reopen, and require the rendered text to match pixel
  for pixel. Two traps it encodes: `ls`'s output on this console is
  interleaved with kernel log lines, so parsing it needs a strict
  entry-shaped regex rather than `split()[-1]`; and a reference
  screenshot must park the caret first, since `load_file()` resets the
  cursor to 0 and a caret bar is a real pixel difference.
- **`clipboard_test.py`** -- drives the system TEXT clipboard across two
  apps, which is the claim that makes it a system clipboard rather than
  a feature of one: text copied in Notepad pastes into the GUI Terminal,
  which shares no code with it.

  **Four checks, in an order where each leaves what the next needs.** A
  paste with NOTHING on the clipboard runs first, while that is still
  true, and must change the document not at all -- the check that a
  `kind` field earns, since without one a file path would paste as a
  line of text. Then a round trip through the DISK rather than through
  pixels: copy, paste, save, and read the file back with `sh cat`, an
  independent path that shares nothing with the editor that wrote it,
  requiring the phrase TWICE. Then the cross-app paste, asserted as a
  BAND -- the change must be contiguous and near the top, where a shell
  prompt is, so a paste that scattered ink down the grid fails while
  "something changed" would pass. Then an oversized copy: Select All in
  `pci.ids` (1.6 MB) asks for far more than `WIN_CLIP_BYTES`, and the
  clipboard must still hold what the round trip put there -- a
  truncating clipboard would replace it with the first 64 KB of
  pci.ids, which is the failure the refusal exists to prevent.

  **A trap it encodes: counting dark pixels is backwards over the
  Terminal**, whose grid is white on black -- there every pixel is
  "ink" and text REDUCES the count, which reported a paste that had
  plainly landed as nothing having happened. Anything over a dark app
  compares two shots instead, which does not care which way round the
  colours are.

  Positive control: making `uclip_set_text()` return 0 without sending
  reddens four of the five checks and leaves the empty-clipboard one
  green, which is the right split.
- **`uiclient_test.py`** -- drives `userland/tests/uiclient.c`, the ring-3
  client that renders real text with `userland/ui/ugfx.c`, and asserts on
  it (8 checks: text actually rendered, the button drew, a click and a
  key each repaint, the unchanged label comes back identical, the close
  handshake works). Two things it encodes: "text was rendered" is
  asserted as INK COVERAGE in a band rather than a single-pixel sample
  (a glyph run puts a countable number of non-background pixels in its
  rows; a blank window and a solid fill are both distinguishable that
  way), and **a client's `stdout` goes to the owning Terminal's
  scrollback, not the serial console** -- so `DebugConsole.logs()`
  can't see a client's own log lines even though a shell-spawned
  process's are visible. Run it after touching `userland/ui/ugfx.c` or
  the font-sharing path.
- **`winclient_test.py`** -- drives `userland/tests/winclient.c`, the ring-3
  client that owns a real window on the desktop, and asserts the
  windowing protocol end to end (8 checks: the window appears in the
  WM's own list at the requested size, the client's pixels reach the
  screen, a key and a click each route to it and make it redraw, the
  window behind it does NOT change, the close handshake completes, the
  desktop survives). Geometry comes from `gui windows` and content from
  PIXEL VALUES with a control point, per `docs/gui-guidelines.md`. Run
  it after touching `userland/wm/wm_client.c`, `kernel/proc/win_role.c`,
  or anything in `abi/win_proto.h`.
- **`sched_gui_test.py`** -- proves the desktop stays ALIVE while a
  ring-3 process runs, the end-to-end counterpart to
  `kernel/proc/sched_test.c`'s KTESTs. The trick it encodes: the `gui`
  debug commands are dispatched from inside `wm_run()`, so a frozen WM
  cannot answer one -- which makes "did the WM answer?" a direct
  liveness test with no screenshot to interpret. Every sample is paired
  with the WM's own `proc_pid` (`gui state --json`) so only samples
  overlapping a genuinely live process count; OVERLAP is the claim, not
  speed. Run it after touching the scheduler, `wm_run()`'s loop, or
  anything about process spawning. Both it and the KTESTs were checked
  as positive controls with the change disabled (0 overlapping samples
  there, versus a continuously responsive desktop) -- do that again
  before trusting a clean run, same reasoning as `damage_sweep.py`'s
  `--positive-control`.
- **`gfxdemo_test.py`** -- drives the Shapes demo (`userland/gui/gfxdemo.c`)
  and asserts on its log + its pixels, 13 checks. Run it after touching
  `kernel/lib/geom.c`/`fixed.c`, `uui_canvas`, or anything a ring-3
  client draws with. Three of its checks encode reasoning worth
  reusing: the window is proved to be a ring-3 client from `gui windows
  --json`'s `client_pid` rather than from how it looks; "it rotates" is
  paired with "it stops dead at speed 0", because either half alone
  proves almost nothing; and the AA toggle is checked by COUNTING
  DISTINCT COLOURS in the canvas (468 with, 5 without) rather than by
  sampling a point, since a curve moves and a fixed sample point
  doesn't follow it.
- **`animation_test.py`** -- window animations (`userland/wm/wm_anim.c`)
  and `desktop.animations`. Opens, minimizes, restores and closes
  Calculator with the setting on and then off. For each change: `gui
  state --json`'s `anims` is >= 1 right after and 0 once settled, and a
  frame captured mid-ghost differs inside the window's rect from BOTH
  the frame before the change and the settled frame -- so the ghost is
  something in between, not one of them. Off: `anims` stays 0 and the
  first frame after the change already equals the settled one. The
  close is the client being killed, which is the case the ghost exists
  to outlive. Puts the setting back to `on` in a `finally`.

- **`damage_sweep.py`'s positive control injects its miss.** `gui damage
  shrink <n>` (a test lever in `wm_render.c`) insets every
  `wm_damage_window_rect()` by 32 px for the next `n` rendered frames;
  `--positive-control` shrinks four frames before a drag and expects the
  verifier to report the border the window vacated. Shrink rather than
  drop, because a frame whose damage is all dropped has none, and a frame
  with no damage is a full repaint -- correct by construction. Until
  2026-09-18 the control only EXPECTED a violation, which passed for as
  long as the WM had a real one -- two, as it turned out (a focus change's
  shadow and the Start button's lit state) -- and the day both were fixed
  it reported the harness as "not checking anything". **The injected miss
  does not yet make the verifier fire** -- `docs/bugs.md` has the
  measurement -- so the control exits 1, loudly, and a clean sweep is
  not yet proof until it does.

- **`shadow_test.py`** -- drop shadows (`userland/wm/wm_shadow.c`) and
  `desktop.shadows`. Places Notepad and Calculator over the flat lower
  wallpaper band, focuses one, and reads LUMINANCE just below and
  beside their edges: the focused window darkens the desktop and the
  darkening fades with distance; the inactive window darkens it less;
  Notepad's File menu (a popup window) darkens what is below it; with
  the setting off the pixel beside the edge matches a control pixel
  60 px out, and that control itself has not moved. Puts the setting
  back to `on` in a `finally`.

- **`smooth_scroll_test.py`** -- smooth scrolling (`ui/uui_scrollanim.h`)
  in the File Manager's icon grid, and the `desktop.smooth_scroll`
  setting. One wheel notch with the setting ON must draw SEVERAL frames
  at distinct positions, spread over time, with shrinking steps
  (ease-out); with it OFF, exactly one. The positions are the app's own
  `files: layout cellgrid` lines read back through `dmesg`, because the
  app emits one per drawn frame that differs from the last and the
  console's live buffer misses a 20 ms burst. A thumb drag must land in
  one frame -- a drag never glides. It writes the setting to
  /etc/desktop.conf and puts it back to `on` in a `finally`, since a
  setting left behind changes the machine for every later tool.

- **`scrollbar_test.py`** -- scrollbar BEHAVIOUR, against the ring-3
  Notepad: the thumb doesn't jump when grabbed anywhere on it, a drag is
  reversible, the trough pages while an arrow steps, and the strip is
  wide enough to hit. It measures the THUMB'S PIXELS (track and thumb
  are known flat colours, so a column scan gives its exact top and
  height) rather than reading text, and takes the strip's rect from
  Notepad's own `notepad: layout scrollbar` line. Written after the
  ring-3 Notepad shipped a bar that scrolled -- so every other check
  passed -- while jumping to put the thumb's top under the cursor,
  making it grabbable only by its top edge. The spec it enforces is
  `docs/gui-guidelines.md`'s "Scrollbars: what a real one does"; run it
  after touching either `apps/ui/ui_scrollbar.c` or
  `userland/ui/uui_scrollbar.c`.
- **A tool that PARKS the real cursor must un-park it.**
  `DebugConsole.warp_cursor()` is the right way to hold a hover -- `gui
  move` lasts one WM iteration -- but the cursor then STAYS there, and a
  menu opened later finds the pointer already inside it. That turned one
  check red 5/5 while its partner ("a click outside dismisses the menu")
  stayed green for the wrong reason: the menu had never opened. Park,
  measure, un-park; `menubar_test.py`'s `hover()`/`unpark()` pair is the
  worked example. This is what the long-standing menubar flake turned
  out to be -- see `docs/decisions.md`.
- **`menubar_test.py`** -- the menu bar, its nested submenus and the
  status bar (`userland/ui/uui_menubar.*`, `uui_statusbar.*`), driven
  through the ring-3 Notepad. 22 checks: the popup is DRAWN (not merely
  responsive), a title opens on press while an item commits on release,
  a press dragged off commits nothing, a click outside dismisses AND is
  swallowed rather than reaching the text, submenus open on hover and
  are placed to the right, Esc closes one level, disabled and checkable
  items behave, and the status bar's indicator tracks the cursor while
  its message pane does not. Two positive controls are recorded in its
  docstring, and they are the reusable part: commit-on-press reddens
  exactly the drag-off check, and a dismissing click that falls through
  leaves "the menu closed" GREEN and reddens only the caret measurement.
  Run it after touching either widget.
- **`popup_test.py`** -- a menu LEAVES its window: popup surfaces
  (`WIN_REQ_POPUP`, `userland/wm/wm_client.c`, `ui/uui_popup.h`),
  driven through Notepad shrunk to its minimum. 18 checks: the
  compositor lists the open menu as a popup entry of Notepad's, its rect
  leaves the parent's content, the popup background is on screen at a
  point the parent does not own (sampled as NOT that colour before the
  menu opened), a row beyond the parent's edge still commits, a press on
  another window closes the menu and does NOT raise that window, a press
  on the desktop dismisses through the compositor and the client agrees
  (`WIN_EV_POPUP_DONE`), and near the taskbar the menu flips above its
  title and stays clear of the panel. Two positive controls in its
  docstring: a provider that refuses every surface reddens the surface
  checks and nothing else (the menu still works in-window, so a check
  green under it was measuring the menu); a grab that delivers every
  press reddens the three "another window" checks. Run it after
  touching `wm_client.c`'s popup path, `uapp.c`'s surface table or
  `uui_menubar.c`'s open/close paths.
- **`remote_gui.py`** -- THE GUI TOOLS, POINTED AT THE BARE-METAL
  MACHINE. Not a runner: two objects serving the interfaces a tool
  already drives -- `RemoteScreen` for `QMPSession` (screenshots through
  the machine's own `screenshot` program, fetched over TFTP; input
  through `guictl`) and `RemoteConsole` for `DebugConsole` (the same
  `gui` vocabulary over telnet, subclassed so every helper from
  `menu_row()` to `hover_frames()` comes across and cannot drift).
  `TOYOS_REMOTE_HOST=<ip>` is what redirects a tool, and it is read in
  the two constructors rather than in a flag each tool has to grow --
  fifty-odd tools, fifty chances to forget. Three things it does NOT
  pretend: a call needing QEMU (`hmp()`, a held button, relative motion)
  raises `RemoteUnsupported` naming the reason; a settle compares the
  caller's box or everything above the taskbar, because a whole frame is
  never identical twice with a clock in it; and the kernel log comes
  from `dmesg` as a delta, since telnet is not the wire the klog is on.
  ONE session per process, given back at exit -- `inetd` serves four.
  Driven by `gui_regress.py --host`; `docs/testing.md` has the worked
  commands.
- **`start_menu_test.py`** -- the Start menu's FOLDERS, search field and
  keyboard (`userland/wm/start_menu.c`, and `wm_overlay.h`'s `key` op
  that makes typing into an overlay possible at all). 23 checks: the
  menu reports two columns with the app rows starting right of the
  sidebar, clicking a folder shows exactly that folder's apps (compared
  against `gui menu --json`'s own app-to-folder list, with the selection
  asserted SEPARATELY from the contents -- the pair is what tells "the
  highlight moved but the list did not" from the reverse), typing with
  no click into the field filters and the reported query is what was
  typed, Enter launches the highlighted result into a real window, Esc
  clears the query before it closes anything, and the arrows move the
  result and the folder. The pixel half: typing CHANGES the search
  field's own rect while a sidebar action row stays byte-identical --
  "it responds" is not "it is drawn", and the neighbour is half the
  assertion. It also covers what PERSISTS: a pin made
  through the row's own context menu is read back from
  `/etc/start-menu.conf` through an independent path (`sh cat`), and
  then the guest is REBOOTED and asked again -- the only version of that
  check worth having. **It is the one tool that reboots its guest**, so
  `gui_regress.py` launches it with `--reboot`: the default
  `-no-reboot` answers a guest reboot by ending QEMU, and the tool's
  next command would then fail as a dead socket somewhere unrelated.
  It also covers the DESCRIPTION: a line too long for the strip is
  marked `..` and the tooltip carries the whole of it, with the three
  halves asserted apart because they fail apart -- not up immediately,
  up after the delay with the full text, and a click that still reaches
  the row underneath. Four positive controls in its docstring: refusing
  printable characters reddens the seven search/Enter checks and nothing
  else; a pane that ignores the selected folder reddens the contents
  check while the selection check stays green; a pin that never reaches
  the disk reddens the four persistence checks and leaves the same-boot
  one green; and a tooltip with no delay reddens exactly the check that
  separates a hint from a box that strobes across a list. One check is
  SKIPPED rather than failed against the bare-metal machine -- "no
  tooltip yet" cannot be asked in under half a second over telnet, which
  is the whole delay, so measuring it there would measure the transport;
  it prints as "not measurable here". Run it after
  touching `start_menu.c`, `start_store.c`, `wm_tooltip.c`, the overlay
  table or a `.desktop` `Category=`.
- **`filedialog_test.py`** -- the shared file chooser as an OWNED window
  (`WIN_REQ_DIALOG`, `ui/uui_filedialog.h`, `uapp_window_open()`),
  driven through Notepad, Image Viewer and Audio Player. 17 checks: the
  compositor lists a second toplevel with `dialog`/`owner`/`modal` set
  and real chrome, the strip gives it no taskbar button of its own, a
  press on the owner leaves the dialog on top AND cannot drag the owner,
  a Places row changes the directory the view reports, a typed path
  commits and the app loads that file, Escape cancels and the owner gets
  the focus back, killing the owner takes the dialog with it, and both
  other apps open the same chooser.

  **Read its docstring before trusting the modality checks.** Two
  earlier versions of them stayed GREEN with the block removed
  entirely -- the obvious press point (the centre of the owner's title
  bar) is under the chooser, which is wider than the window that opened
  it, and the next one along is the app ICON, where a press opens the
  window menu rather than starting a drag. `clear_grab()` picks a point
  that is neither, and `place_owner()` pins the window first so such a
  point exists at all; without the pin the cascade can put the whole
  title bar under the dialog and the check fails as a fixture problem
  that reads exactly like a regression. Three positive controls are
  recorded there, including the one that found this.
- **`forcequit_test.py`** -- not-responding detection and force quit
  (TWP's ping/pong, `scheduler_kill()`, the dialog, and the slot
  reaping). 15 checks. The design point it encodes: a client that
  REFUSES to close and one that is WEDGED are identical to a plain
  timeout, so `winclient` (declines, keeps answering) and
  `userland/tests/hangclient.c` (stops pumping on `h`) are tested
  against each other -- neither half means much alone. Its positive
  control reddens exactly one check and leaves the winclient ones green,
  which is worth reading before trusting them.
- **`idle_desktop_test.py`** -- with nobody touching it, does the screen
  SIT STILL? Two regions (the icon column, an empty patch) must be
  pixel-identical across eight captures. Written after a blinking console
  cursor appeared on top of a desktop icon and every one of the 23 GUI
  tools passed, for a structural reason worth knowing: they all DRIVE the
  desktop and assert on what changed, so nothing was asking the opposite
  question. It is the cheapest check for a whole class of bug -- a second
  owner writing to the framebuffer. **The taskbar is excluded and its
  clock is the control**: it must CHANGE, which is what proves the
  capture pipeline can see motion at all (without it, a harness handing
  back one cached frame would report a beautifully steady desktop). In
  `gui_regress.py`.

  **It was the only tool that did not wait for the desktop**, and that
  cost a real investigation: it connected and asked `gui state --json`
  straight away, so a boot that had not finished starting `toywm`
  failed it two seconds in with `no window manager running`. Measured
  with `predates.py` at **1 run in 6 on HEAD and 3 in 6 on a busier
  tree** -- pre-existing, and at those counts the two rates are not
  distinguishable, which is exactly why the fix is the poll rather than
  an argument about whose change it was. It calls `wait_for_desktop()`
  now (6 of 6 after), and deliberately NOT `enter_gui()`: that also
  turns on the per-frame layout log, and a tool measuring what an idle
  desktop does is the last one that should be given more to log.
- **`serial_backpressure_test.py`** -- a COM1 consumer that stops
  reading must not stop the MACHINE. It attaches a client to the serial
  socket, goes deliberately deaf, asks the guest for several KB
  (`sh dmesg`), and requires the taskbar clock to keep ticking. Written
  after the entry `docs/bugs.md` carried as "a filesystem write during
  the desktop's STARTUP stalls the clock", which was neither the
  filesystem's fault nor the compositor's: `serial_putc()` waited
  unbounded on a THRE bit that QEMU's socket chardev stops setting when
  its peer stops draining, so the kernel spun there **with interrupts
  still on** -- ticks advancing, PIC clean, scheduler still picking the
  compositor, nothing running. The write only supplied the log volume.
  **Two things to know before editing it.** Its positive control is in
  the docstring and fires on exactly one check (1 distinct image in 8
  against 4-5), which is what makes it a test rather than a
  demonstration. And **do not make it drain the socket to "fix" a
  failure** -- being deaf IS the fixture, and a drainer makes it pass
  against a kernel that still hangs. Run on demand; it needs a guest
  started by `vm.py` (a socket serial, not `-serial file:`, which never
  applies backpressure and so cannot see this at all).
- **`icons_test.py`** -- application icons from a `.qoi` file to the
  screen (9 checks), with the host as the oracle again: Pillow decodes
  the same file, scales it the same way, composites it over the sampled
  wallpaper, and the guest has to agree. Covers all three draw sites
  (desktop, Start menu row, taskbar button -- the last matched through
  the entry's `AppId=`), the missing-file fallback, and that the cache is
  a CACHE (`gui icons` reports the count; twelve forced repaints must not
  change it). **Read its docstring before editing the corner check**: the
  first version asserted the corner was "not the tile colour", which a
  plain `ugfx_blit()` satisfies by writing black, and only the positive
  control found that. In `gui_regress.py`.
- **`mines_test.py`** -- Minesweeper (20 checks), and the protocol
  property it was built to prove: **a secondary click reaching a ring-3
  client**. The check that matters is a PAIR -- a right-click on the
  BOARD must flag a cell and leave no context menu open, and a
  right-click on the TITLE BAR must still open the window menu; either
  half alone passes under a half-broken split. Game rules are asserted
  against the client's own `mines: state ...` log line rather than
  pixels (a screenshot cannot tell a flood fill that opened 51 cells
  from one that opened 3), and geometry comes from its `mines: layout
  ...` lines, menu rows included, so nothing here re-derives the app's
  sizing. Two things its docstring records, both learned the expensive
  way. **The flag's pixel check compares the SAME cell before and
  after** -- the first version compared the flagged cell against a
  different one and passed for the wrong reason, because the cell it
  flagged happened to be revealed and a revealed cell differs from a
  covered one whether or not a flag ever drew. And **the clock is
  asserted to ADVANCE, never to reach N after N seconds**: the guest's
  timer runs at whatever rate TCG manages, measured here at well under
  half real time under load. Both positive controls were run -- disabling
  the WM's content forwarding reddens four checks and nothing else;
  narrowing the first-click safe zone from the 3x3 neighbourhood to the
  clicked cell reddens exactly one, on the right assertion. In
  `gui_regress.py`.
- **`calendar_test.py`** -- the tray clock's calendar popup: opening,
  the grid, paging, and `desktop.week_start`. **The grid is checked
  against the HOST's `datetime`**, which shares no code with the guest's
  `cal_days_from_civil()` -- a test that re-derived the first column the
  same way the code does would agree with a wrong implementation. **Every
  open/close check is paired with the panel's own pixels**, because
  `gui calendar --json` reporting `open: true` is exactly what a popup
  that draws nothing also reports (this repo's "it responds is not it is
  drawn" trap). The comparison box is the panel rect, whose bottom edge
  is the taskbar's top edge -- a box containing the once-a-second clock
  could never settle. **The year boundary is exercised** (paging back
  past January), since "month - 1" with no wrap is the obvious bug, and
  **the week_start check restores `monday` afterwards**, since a written
  setting outlives the run and changes the machine for every later tool.
  Three positive controls were run: disabling today's highlight reddens
  one check, ignoring the setting reddens three, and removing the year
  wrap reddens two -- each on the right assertion and nothing else. In
  `gui_regress.py`.
- **`volume_test.py`** -- the taskbar's volume flyout: the slider, mute,
  the wheel and the output-device list. **The slider's real assertion is
  `config get volume`, not the popup's own reading**: the write is
  DEBOUNCED, so a build that moved the number on screen and never
  flushed it would pass every check phrased against the popup. **The
  wheel check has a negative half** -- a notch over the tray icon moves
  the level, the same notch over the desktop must NOT, which is what
  fails if the handler forgets to ask where the pointer is and then eats
  every scroll in every app. The pointer is WARPED
  (`DebugConsole.warp_cursor`), never `gui move`: an injected position
  survives one `wm_run()` iteration and the wheel would arrive with the
  cursor back where it was. Open/close is paired with the panel's own
  pixels, same reason as the calendar's. `gui_regress.py` boots it with
  `--audio both` (see `EXTRA_VM_ARGS`), because the device rows are
  empty and unwritable on a machine with no sound card -- it says so and
  skips them rather than failing, so the rows going missing on a machine
  that HAS a card still reads as a failure.
- **`tray_press_test.py`** -- the notification area's press feedback:
  a rounded fill while an item is held, and nothing on hover. Driven on
  the CLOCK, the one tray item that is registered whatever hardware the
  guest has. Every assertion is a pixel value read from the item's
  four-column left padding strip -- **a band the glyphs never reach, so
  the seconds ticking underneath cannot move it**, which is what lets an
  ordinary pixel comparison work on a control that redraws once a
  second. **The hover half is the point**: the cursor is PARKED on the
  item with the button up (`DebugConsole.warp_cursor`, never `gui move`)
  and that strip must not move by one value -- a build that added a
  hover state as well would pass every "the press works" check. The
  direction of the wash is READ, not assumed (`uui_state_bg()` darkens a
  light panel and lightens a dark one; this guest's panel is dark, and
  an assertion phrased as "darker" would fail on a correct build). It
  also asserts the fill is ROUNDED -- its corner pixel must sit nearer
  the panel's colour than its interior -- that a drag off the item
  disarms and a re-entry re-arms, and that the press does NOT latch: the
  calendar it opened is still up while the clock is back at rest. Three
  positive controls were run: removing the fill reddens three checks,
  squaring its corners reddens exactly one, and adding a hover state
  reddens the two hover checks and the two latch checks -- each on the
  right assertion. Skips cleanly with no Pillow. In `gui_regress.py`.
- **`brightness_test.py`** -- the taskbar's brightness flyout, driven
  on a machine with NO backlight, which is every QEMU adapter. It
  asserts the degraded path honestly: the tray item exists, the panel
  is painted and repainted away, the sentence it shows is the
  registry's own (`config set brightness` refuses with the same words),
  and the slider and the wheel WRITE NOTHING -- checked against `config
  get`, since a build that skipped the availability check would push a
  value the registry refuses and show a level the hardware never took.
  It also drives `desktop.tray_brightness` through all three values:
  `auto` must HIDE the sun on this backlight-less guest, `never` must
  too, and `always` must show it anyway. An absence is weak evidence, so
  it is taken twice -- the compositor's `tray_hidden` AND `gui taskbar
  --json`'s `tray_x` moving by the icon's width, which comes from the
  same walk that draws the strip and so cannot report a tray narrower
  than it painted.
  **It PINS the setting to `always` for everything else it does**: on
  the default `auto` there is no sun in a QEMU tray to click, and checks
  1-5 would have nothing to drive. It puts `auto` back on the way out,
  since an extra tray item shifts the taskbar geometry other tools
  measure. Its setting writes go through the console's own `sh`, never
  `vm.py exec` -- two readers on one serial socket steal each other's
  replies and the write silently does not happen, which reads exactly
  like the feature being broken.
  The positive half (a real panel dimming) runs on the bare-metal
  laptop by hand: `config set brightness 40` reads the PWM back. In
  `gui_regress.py`.
- **`network_tray_test.py`** -- the taskbar's network item: the icon's
  state, the read-only panel behind it, and the visibility setting.
  **Its strongest check is the one that does not ask the compositor**:
  the interface name and address the panel reports must also appear in
  `/bin/ifconfig`'s output, which walks the same `QUERY_NETDEV` class
  through a completely different program -- a compositor reporting its
  own view back to a test proves only that it is self-consistent.
  It also pins the rule the widget exists for: `link_known` is
  THREE-valued, a driver that cannot answer is not a driver saying
  "down", and the e1000 in a default QEMU guest is exactly that case --
  so `connected` must follow the ADDRESS. The positive control for
  that (deriving `connected` from the link flag instead) reddens those
  two checks and nothing else. Then: the panel is PAINTED rather than
  merely flagged open, a second click on the icon closes it, a click
  outside dismisses it and restores the pixels underneath, the volume
  flyout and the Super key close it through the overlay table's `close`
  op, and `never`/`auto` hide and show it with `gui taskbar --json`'s
  `tray_x` moving by the icon's width. In `gui_regress.py`.
- **`modeset_test.py`** -- a runtime resolution change under a live
  desktop, on the default adapter. **The oracle is the DEVICE**: a QMP
  screendump's own pixel size is QEMU's scanout geometry, and it must
  equal both the requested mode and what `gui state` reports -- the
  two disagreeing is the stale-`gfx_width()` bug the work replaces.
  Then the Start menu must paint at the new size, a maximized Notepad
  must fill the new screen with its pixels in the corner outside the
  old mode, an unlisted mode must be refused with the screen untouched,
  and the boot mode must come back. It restores the setting, because a
  stored resolution is applied at the next boot. In `gui_regress.py`.
- **`filemanager_harness_hostcheck.py`** -- `filemanager_test.py`'s own
  wait and toolbar logic, on the host against a scripted console and a
  click-recording QMP stand-in: a toolbar item the app did not report
  records a failed check and clicks nothing (it once raised `KeyError`
  out of the suite); a report split across two serial sweeps parses
  whole; a wait whose predicate never holds returns None rather than
  the layout it rejected; a state the app already reported answers
  a later wait after a grace period, since the app dedupes its whole
  report block and repeats nothing; that cached answer is OFF once a
  newer report has begun, so a frame whose tail has not arrived cannot
  let the previous one answer for a state the app has left; the same
  frame STAYS ineligible across the waits that follow, since a wait
  timing out changes nothing about the app; and a respawned window
  inherits none of the old one's geometry. Seconds, no guest.
- **`filemanager_test.py`** -- the File Manager: two panes, marking, and
  real file operations. **A check runs only after its prerequisite**:
  the view toggles are driven through `toolbar_click()`, which requires
  the item's reported rect, and a step whose prerequisite failed is
  printed as `SKIP` with the reason rather than attempted -- the
  two-pane, tree-hidden view is restored (and verified) before anything
  later runs, and the run stops with a failing summary if it cannot be
  -- including when the context menu was never confirmed closed, since
  an open menu takes the press wherever it lands and a restoring
  toolbar click would dismiss it rather than toggle anything.
  **It puts both panes in Details first** (from the
  toolbar, which is itself a check): the app opens in icons view, and
  every row this tool aims at is row-height arithmetic. **The result of every operation is checked
  through `ls`, not through the app** -- the manager is the thing under
  test, so it cannot also be the witness; a version that updated its own
  list and spawned nothing would pass its own view. Four checks are
  shaped so a broken version cannot survive them. **Delete is a PAIR**:
  F8 then Esc must leave the file on disk, and only then does F8-then-
  Enter remove it -- the first half is what fails on a missing
  confirmation. **The marked copy requires the UNMARKED file to be
  absent**, or "it copied everything" would pass too. **Backspace must
  also select the directory it just left**, which only a correct `up()`
  produces. And **the active-pane pixel check samples the OTHER pane's
  header as its control**, so a paint that marked both would fail. It
  drives the app with `gui key <code>` rather than QMP keystrokes, so
  nothing here depends on the guest's keyboard layout. Two harness traps
  it paid for: a batch of log lines can hold SEVERAL frames, so the
  parser takes only the last (reading each field's last occurrence
  across the batch mixed one frame's `active` with another's
  `selected`), and characters typed with `settle=False` outrun the
  client so a name field commits empty -- which reads exactly like a
  broken `mkdir`. In `gui_regress.py`.

  **Its type-ahead phase is 4b, deliberately not renumbered in**:
  inserting a numbered phase would have renumbered eight, which is this
  repo's docs-edit hazard applied to a test file. It also forced phase 5
  to establish its own selection with Home rather than inheriting the
  phase above's -- the insertion broke it on the first run, which is the
  argument for the rule rather than a reason to move the phase.
- **`wallpaper_mode_test.py`** -- `desktop.wallpaper_mode`, at a screen
  mode where the setting can actually be seen. **It sets 1024x768
  because at the default it would measure nothing**: both stock
  wallpapers are exactly 1280x720 and so is the boot mode, and at a
  matching aspect ratio `fit` and `fill` produce the SAME pixels -- so a
  check taken there passes whether the setting works or not. Its first
  check asserts that premise rather than assuming it, so a future
  wallpaper that breaks it says so here instead of quietly making the
  tool pointless.

  **The pair is what makes it evidence.** One check requires a MODE
  change to log no decode (`uui_image_draw()` re-fits the same decoded
  source every frame, so re-decoding would spend a whole JPEG to reach
  identical pixels); on its own that passes just as well for a broken
  log line or an absent wallpaper. The next changes the NAME and
  requires a decode to appear, from the same log in the same run.
  Neither half means anything alone. The round trip is the third: `fit`
  then `fill` must restore the original bytes, which "the pixels
  changed" cannot tell from "the pixels changed to something else".
  Every setting it touches is restored, the resolution included. In
  `gui_regress.py`.
- **`imgview_test.py`** -- JPEG decoding all the way to a screen (15
  checks), and the only one of the three decoder checks that can see a
  pixel. Its oracle is the host: `data/wallpapers/aurora.jpg` is
  1280x720 and so is the screen, so the default wallpaper must match
  libjpeg's decode of that file PIXEL FOR PIXEL, with nothing resampled
  in between -- and the control beside it is that the same samples must
  NOT match the other wallpaper. Then Image Viewer: it lists a directory
  by PROBING its files, decodes, reports where the picture landed, and
  the fit modes are asserted by geometry and by pixels (letterbox bars
  must be one flat colour, which a cropped picture cannot satisfy).
  Finally "Set as wallpaper" is followed across a process boundary --
  the viewer writes `/etc/desktop.conf`, the desktop notices through the
  filesystem generation counter, and the background becomes the other
  image. Both positive controls in its docstring were run: a swapped
  Cb/Cr reddens exactly the four pixel comparisons and no layout check.
  It restores the original wallpaper on the way out, and its comment
  says why that matters -- leaving "no wallpaper" behind fails the NEXT
  run's first check, which cost a confusing red run. In
  `gui_regress.py`.
- **`blank_window_test.py`** -- opens EVERY app in the registry and
  requires its window to contain more than a flat fill. Reads the app
  list from the KERNEL (`gui apps`), so an app added tomorrow is covered
  without editing it. Exists because UI Demo shipped completely blank
  and a 35-check suite passed it: every check asserted on the app's LOG,
  and the widgets were live, hit-testable and simply never painted. In
  `gui_regress.py`.
- **`window_geometry_test.py`** -- windows come back where you left
  them (`userland/wm/wm_geometry.c`). Most of it drives the RESTORE half
  from a hand-written `/etc/windows.conf` rather than from a previous
  run, on purpose: a round-trip test alone passes if both halves are
  broken the same way -- save the wrong rect, restore the wrong rect,
  get it back. Writing a known geometry and demanding exactly that one
  cannot be satisfied by a symmetric bug. The SAVE half is covered
  separately by dragging a window and reading the file.

  **Its checks were written twice, and the first set was nearly
  worthless**: with restore disabled entirely, five of six still passed,
  because "the window is on screen" and "reopening matches" are both
  true of default placement. They assert exact sizes that can only have
  come from the file now -- 4 of 6 go red under that control, and the
  drag check goes red under the save control. Worth reading before
  adding a check here. In `gui_regress.py`.
- **`diskmark_test.py`** -- drives the **Disk Mark** GUI benchmark and
  asserts on it. **The load-bearing check is the NUMBERS, not the run
  finishing**: a build whose throughput arithmetic truncated to zero
  logged "all four passes complete" and drew four tiles reading
  `0.0 MB/s`, with correct IOPS beside them, so everything an "it ran"
  check looks at was green.

  **It also covers the spawn, the report file and the reap**, since the
  app does no I/O itself -- it runs `/bin/diskbench` and polls what that
  writes.

  Two more that earned their place the same way. **The results must be
  DRAWN, not merely logged** -- each tile compared as pixels against its
  own before-image, which is the Calculator-with-invisible-buttons
  failure. And **the title bar is the CONTROL**: it must NOT change. That
  one immediately found a real bug -- a 4 MiB slice took over a second
  on an emulated disk, so the client missed the compositor's pings and
  ran the whole benchmark reading `Disk Mark (Not Responding)`. The fix
  was to bound a slice in TIME rather than in bytes, which self-tunes to
  the device.

  Geometry comes from the client's own `diskmark: layout ...` lines,
  never re-derived here -- hand-computed coordinates cost two
  build-and-test cycles while the app was being written. On demand
  rather than in `gui_regress.py`: it does real disk I/O for tens of
  seconds, and a benchmark run beside eleven other VMs measures
  contention. `blank_window_test.py` covers "it draws" in the gate,
  since that one opens every app in the registry.
- **`doom_sound_test.py`** -- **DOOM's effects and music, judged on the
  HOST**, and the only thing that exercises `userland/backends/doom/dg_sound.c`,
  `dg_music.c` and `opl_toyos.c` at all. On demand twice over: it needs
  an IWAD (not in the repository) and boots its own AC97 guest, and it
  SKIPS cleanly without a WAD.

  **Its load-bearing check is the PAIR OF BOOTS, not either one.** Doom
  has two independent audio paths and one recording cannot separate
  them -- music-only and effects-only builds both make noise. So it
  plays the attract demo normally and then again with `-nomusic`, and
  requires COVERAGE to collapse: OPL music is continuous, so run 1
  fills nearly every 50 ms window, while run 2 is the same gunshots
  with silence between them. If music were dead, run 1 would already be
  bursty; if effects were dead, run 2 would be silent. Neither failure
  can satisfy both. `-nomusic` reaching the game is why
  `userland/gui/apps/doom.c` forwards its argv.

  What it cannot see: whether the music is the RIGHT music. Nothing
  checks pitch or melody against the WAD's MIDI, so a wrong instrument
  bank or a transposed OPL passes.
- **`audio_test.py`** -- AC'97 and the PCM stream, judged on the HOST:
  the guest plays two seconds of A440 (`/tests/tone`) and QEMU's wav
  audiodev (`vm.py --audio-wav`) records what the DEVICE emitted to a
  host file, where the frequency is measured by zero-crossing count --
  an oracle sharing no code with the driver, the core or the tone
  generator. Three failure modes fail three different checks: a dead
  DMA engine records silence, a wrong rate the wrong pitch, and a
  broken consumed-chunk zeroing more than 2s of tone (the app's own
  STOP bounds that leak, so the `sound` KTEST is the first-line guard
  -- it reddened under the positive control before the recording did).
  **Measure within BURSTS**: under TCG the guest falls behind wall
  clock and QEMU pads the recording with host-side silence, so the
  file's timeline says nothing about the guest. The ac97 KTESTs run
  un-skipped only here, which makes "0 skipped" the load-bearing
  assertion (the ahci lesson). On demand, not in the gate: it boots
  its own guest with extra hardware.

  **It boots TWICE, and the second boot is the sharper half.** One
  recording cannot hold two tones and be judged by frequency, so the
  WAV phase gets its own: the guest runs `/bin/aplay /tests/sine1k.wav`,
  which is the whole of `userland/lib/usnd.h` -- the RIFF chunk walk,
  the 44.1 -> 48 kHz resampler, the mixer and the sink. The fixture is
  **44.1 kHz on purpose**: a build that did not resample at all would
  play its 1 kHz tone at 1088 Hz, and the 30 Hz tolerance is set to
  reject exactly that while accepting the interpolator's own error.
  Nothing else in this repo can see a broken resampler -- `/tests/
  usnd_test` checks frame counts, which a wrong-rate build gets right.
  **`--card hda` runs the same three boots against QEMU's
  `ich9-intel-hda` + `hda-output`** and expects the `hda` driver and
  KTESTs; `ondemand_sweep.py` names both rows. The oracle found the
  HDA driver's first real bug in one run -- an amplifier-capability
  field read in the wrong order set "0 dB" to -53 dB, and a tone at
  4% amplitude measured as silence.

  **Its last boot is FOUR RESTARTS, and it asserts on a hole's SHAPE.**
  Each chime is played only once the log says soundd released the card,
  so every one is a fresh open and START. The mixer used to fill the
  whole ring on that first pass whatever the client had produced, and
  the recording showed a hole inside the chime of exactly 2, 8, 10 or 12
  chunks. TCG's padding is the reason not to assert "no gaps": the check
  fails only on a gap that is a WHOLE NUMBER OF CHUNKS (10.67 ms), which
  the mixer makes and TCG does not. Measured 2026-09-23: red 2 runs of
  2 with only soundd's fill rule reverted (holes of 21.3-298.6 ms, every
  one a whole number of chunks), green 3 of 3 on the fixed soundd.
  **`--only restart` runs that boot alone**, about a minute instead of
  the whole tool, which is what makes a positive control affordable.
  Each chime prints how long the release wait and aplay took, so a stall
  names its step instead of reading as the tool hanging.

  **IT SETS THE MASTER VOLUME ITSELF** (`establish()`), because its
  image is a copy of whatever `disk.img` holds. A `volume=25` left there
  by an earlier run turned ten checks red with nothing wrong in the
  build (the AC97 driver played 25% as silence then). A
  failure here that the old build does not share should be retried on a
  `make clean-disk` image before it is believed.
- **`soundd_test.py`** -- the sound daemon, judged on the HOST by the
  one question no in-guest check can answer: are TWO programs audible at
  the same time? A mixer that silently served one client and dropped the
  other passes every "did it crash", "did aplay return 0" and "is the
  service running" check there is. So the guest plays `sine1k.wav` and
  `sine440.wav` from two separate processes AT ONCE and the host
  requires BOTH frequencies in the one recording.
  **Zero crossings cannot do this** -- two mixed tones cross zero at
  neither of their frequencies -- so it measures energy at each
  frequency with a Goertzel filter, and samples a THIRD frequency
  nobody played as the control: if 700 Hz reads as loud as 440, the
  measurement is noise and the two real readings mean nothing. It
  measures the loudest half-second rather than the whole file, for
  `audio_test.py`'s reason (TCG pads the recording with host silence).
  **`--positive-control` is built in**: it stops the daemon first, so
  the second `aplay` gets -EBUSY and only one tone can reach the card.
  That run MUST show one frequency and not two -- it is what proves the
  measurement can distinguish the states at all, and it reproduces the
  exact bug the daemon exists to fix. On demand, not in the gate.

  One trap it encodes: **no trailing `&`**. `spawn` at the ring-0 shell
  already returns as soon as the child exists and that shell has no job
  control, so an ampersand arrives as a second ARGUMENT -- it failed
  once as "cannot open file", with the daemon working perfectly.
- **`usb_audio_test.py`** -- USB Audio Class 1.0 playback, on the same
  host-side oracle `audio_test.py` uses, pointed at a different bus. The
  guest plays A440 through an isochronous OUT endpoint and QEMU's wav
  audiodev records what the DEVICE emitted; a configured endpoint that
  never gets a packet records silence, a wrong packet size records the
  wrong pitch. **Three boots, and the middle one is why `vm.py` grew a
  second recording.** With `--audio both` each card writes its OWN wav
  file, so "the tone is in the USB file and not the AC97 one" is an
  assertion -- on one audiodev the file holds their mix and device
  selection is untestable. Its positive control (making `sound_select()`
  ignore the name) put **4.05 s at 436 Hz in the USB file and 0.00 s in
  the AC97 one**, which is exactly the signature a working selection
  cannot produce. It also unplugs the device mid-run (`device_del`),
  which is a state no PCI card can reach, and checks that the choice
  came back from `/etc` on a reboot -- from the FIRST LINE of `config
  get`, since boot chatter mentions `ac97` too and a substring match
  passed on a guest whose setting had not been restored at all. On
  demand, not in the gate: it boots its own guest with extra hardware.

  **It reported "the emulated device plays nothing" (filed 2026-09-21),
  and the device played.** `/tests/tone` opens the stream directly, soundd holds
  it from boot until 2 s after its last client, and the tool spawned the
  tone without waiting -- so the app was refused, wrote no verdict, and
  the device recorded silence. It waits for the release now
  (`wait_card_free()`), as `audio_test.py` always had to. Two more
  staled with the OS: a USB DAC registers as `usb-<vid><pid>` since
  f57e1134 (`USB_DEV`, QEMU's being 46f4:0002), and the tool sets volume
  100 on its image copy, `audio_test.py`'s `establish()`.
- **`audio_loopback_test.py`** -- **the only tool here that judges the
  SOUND rather than the driver.** A cable from the Creative G6's
  headphone out into the motherboard's line in, so the analogue output
  of a passed-through DAC can be recorded and counted. Everything else
  in this section measures what the driver DID; this measures what came
  out. It exists because `docs/bugs.md` records `usbaudio.so` as
  "crackles", which is not a measurement, in an entry that is itself a
  warning about trusting the wrong number -- packets per second went UP
  while the audio got worse.

  **How it counts a crackle.** A sine obeys `x[n] = 2cos(w)x[n-1] -
  x[n-2]` exactly, so every sample is predictable from the two before
  it and any break in the stream -- a gap, a repeated packet, a torn
  buffer -- shows up as a residual spike. It reports DROPOUTS (the
  envelope collapsing: the ring ran dry) separately from CLICKS (the
  waveform tearing while the level holds: the samples were there and
  wrong), because those have different causes.

  **`--selftest` is the positive control and runs by default**: the
  HOST plays a tone and records it, proving the loop carries signal
  before any guest measurement is believed. It doubles as the
  analyser's NEGATIVE control -- a known-clean tone must report zero
  dropouts and zero clicks, so a glitch count on a guest capture is
  real rather than a detector artefact. That control earned its place
  immediately: the first run reported one dropout and one click on a
  perfect signal, because the capture's leading and trailing silence
  and the tone's own hard edges were being counted. Run the selftest
  BEFORE passing the G6 through -- once QEMU holds the device the host
  cannot open it for playback. Then `--record SECONDS` captures while
  the guest plays, and `--analyse WAV` re-reads a capture later.

  **It SKIPS when the loop is open, and that is deliberate.** The cable
  is not normally connected; an absent cable and a silent guest are
  both flat ADC hiss, so failing would convict the OS of a fault in the
  test rig. Two traps it encodes, both measured: the ALC892 comes up
  with **+30 dB `Capture` AND +30 dB `Line Boost`** -- 60 dB on a line
  input fed by a headphone amp, which clips into mush and reads as a
  broken cable -- and every capture opens with a **-26 dBFS ADC
  start-up transient decaying over ~0.7 s**, louder than the tone's
  noise floor, so anything hunting for a level finds it first. The
  measured baseline of the rig at unity gain (G6 `Speaker` 107) is
  0.02% THD, 70 dB channel separation and a ~60 dB SNR ceiling set by
  the ADC, not by the G6. `local_info.txt` has the full calibration.

  **`--crackle` is the A/B the bug needed**, and it is one mode rather
  than a second tool because the finding IS a comparison -- a number
  from one driver alone is what misled `docs/bugs.md` for weeks. It
  encodes a steady 1 kHz tone as an MP3 (the DECODER has to be in the
  producer; a WAV of the same tone is pristine through both drivers and
  measures nothing), boots a guest with the G6 passed through, and plays
  it once through the in-kernel `usb-audio` and once through the ring-3
  `usbaudio.so` -- one cable, one stimulus, one analyser, so the
  difference is attributable. It proves the cable BEFORE the guest takes
  the device, because once QEMU holds the G6 the host cannot play
  through it to check. Needs `lame` and SKIPS without it, the same rule
  that keeps ffmpeg out of the gate. **It is a REPORT, not a gate**: the
  crackle is a known open bug, and a check that is permanently red is
  one people learn to ignore, so it returns non-zero only when a leg
  produced no audio at all -- that is the rig going wrong, not the bug
  still existing.
- **`devclaim_test.py`** -- a ring-3 process takes the sound card OFF
  THE KERNEL, reads its registers, and gives it back (stage 2 of
  `docs/umdf-design.md`). It boots its own guest with an
  `ich9-intel-hda` because `hda` is the only driver whose device the
  default headless boot both HAS and can let go of -- so this is the
  only run in which the kernel actually releases a device it was
  driving. **The
  assertions are about the DRIVER's state, read from outside the
  claim**, because the in-guest test's unbound-device leg is green on
  any machine and would pass against a claim that recorded a pid and
  unbound nothing: `lspci -k` says `kernel driver: hda` before, the
  process reads `HD Audio 1.0` out of BAR0 while it holds it, and
  `lspci -k` says `kernel driver: hda` again afterwards. The round trip
  is `sound: hda0 registered` appearing TWICE in one boot -- once at
  probe, once when `DEV_RELEASE_REBIND` re-probes -- which nothing but
  a real remove-and-reprobe produces. `--no-card` is the positive
  control: the same run with no controller, where the in-guest test
  must SAY it skipped the unbind leg, so a tool whose HDA assertions
  silently matched nothing cannot look like a pass. On demand, not in
  the gate: it boots its own guest with extra hardware.
- **`hdacodec_test.py`** -- a ring-3 driver reads the HD Audio codec
  graph and has to reach the SAME ANSWER the kernel does (stage 3 of
  `docs/umdf-design.md`). It boots its own guest with an
  `ich9-intel-hda`, for the same reason `devclaim_test.py` does, and
  runs `/bin/lscodec`: claim, map BAR0, take a DMA buffer for the
  command ring, bring the controller up, walk the graph, release with
  `DEV_RELEASE_REBIND`. **A plausible graph is not evidence** -- a stub
  that invented one would satisfy "it named a vendor and a route" -- so
  the load-bearing assertion is the AGREEMENT: the kernel's `hda`
  driver logs the pin and DAC it picked over its own CORB/RIRB, ring 3
  prints the pin and DAC it picked over a granted DMA buffer, and the
  two must match. Around it: `lspci -k` reports the card `(claimable)`
  before the run, `dmesg` shows the DMA grant raising BUS MASTERING,
  and `sound: hda0 registered` appears once more than the pre-claim
  baseline. The inverse of the claimable check SKIPS rather than
  passing quietly on a machine where every bound driver is removable,
  which a plain QEMU guest is -- `dev_claim.c`'s KTEST covers that
  half. `--no-card` is the positive control: `lscodec` must SAY there
  is no controller and exit non-zero. On demand, not in the gate: it
  boots its own guest with extra hardware.
- **`mixer_test.py`** -- the PER-APPLICATION volume, from the tray
  flyout down to what `soundd` actually applies. It boots its own guest
  with an `ich9-intel-hda` because `volume_test.py` cannot cover this:
  that guest has no sound card, so `soundd` exits with "no sound
  device", there is no beacon, no roster, and the per-app section is
  correctly absent from the panel it drives. **The assertions are a
  ROUND TRIP through four components that share no code** -- a row
  appears only while a client plays and carries the name `soundd` mixes
  it under (not the `snd.<pid>` its ring is called), a click a quarter
  along the track moves that row, `/etc/sound.conf` gains the key so
  the value outlives the widget, and the daemon reads it back and says
  so. That last one is counted as a DIFFERENCE from a baseline rather
  than as "is the line present": `soundd` reloads the moment the flyout
  writes, so the line already exists from the client still playing
  then, and the first version of the check passed without the second
  stream ever being mixed. It runs on a COPY of `disk.img` -- the
  flyout genuinely writes `/etc/sound.conf`, and a crash between the
  write and the cleanup would leave `aplay` quietened for every later
  tool on the real image. On demand, not in the gate.
- **`install_test.py`** gained an `mbr` medium (`--media mbr`) covering
  `install --mbr` end to end, and **three fixes to the tool itself, all
  of which made a healthy system report as broken**. `--instance auto`
  was passed through to EVERY `vm.py` call, and vm.py resolves `auto`
  per invocation -- so `start` took one slot and each later `exec`
  resolved to a DIFFERENT free one with no guest in it; every command
  came back empty, ten checks failed naming nothing, and the guest was
  left running to poison the next run. It resolves the slot ONCE now.
  The readiness check tested `"ready" not in out` against vm.py's
  refusal message, **"al*ready* running"** -- so a guest that never
  started looked like one that came up. And the first `exec` now waits
  on the console ANSWERING rather than on `start` returning, since on
  live media vm.py reports ready before the debug console takes
  commands. Its `--positive-control` zeroes the installed boot sector
  and must redden the six boot checks.
- **`osk_test.py`** -- the on-screen keyboard (`userland/wm/osk.c`).
  **Its load-bearing check is a ROUND TRIP THROUGH THE FILESYSTEM**: it
  types `mkdir /<name>` into the Terminal with keycap clicks and then
  asks the shell whether the directory exists, which covers the
  hit-test, `wm_client_send_key()`, the client, the line editor and the
  disk in one assertion. Ink rising in a Notepad window would not --
  a blinking caret moves those pixels, which is the trap CLAUDE.md
  names. **Shift and Ctrl get DISCRIMINATING checks** because both fail
  silently: `mkdir /Zz` distinguishes a modifier that stuck (`ZZ`) from
  one that never armed (`zz`), and the Ctrl check types `xyz`, presses
  Ctrl-C and then a real command -- if Ctrl were sent as the modifier
  BIT rather than folded to a control code, a literal `c` would land in
  the line and nothing would be created. Its control is breaking that
  fold, which reddens two checks. **Warp the cursor, never `click_at`
  alone** -- the WM accelerates an injected delta, so an open-loop move
  misses the tray item and reads as a dead control. Run by
  `gui_regress.py`.
- **`msi_test.py`** -- the Local APIC, and the xHCI delivering through
  MSI-X instead of its shared pin. **Its load-bearing check is that
  interrupts ARRIVE, and enumeration cannot show that**: every control
  transfer in the xHCI driver polls the event ring, so a controller
  whose interrupts go nowhere still finds its devices, registers them
  and logs "running" -- the whole boot looks perfect. Only an
  asynchronous HID report needs a real interrupt, so the tool moves the
  mouse and requires the controller's interrupt count AND its decoded
  reports to rise; with INTx disabled by the MSI-X programming, an
  interrupt that arrives can only have come from the vector. **Its
  control is a boot flag** -- `make iso KCMDLINE="nomsi"` puts the
  machine back on the 8259 -- and it is documented in the tool rather
  than run by it, since baking a flag rewrites the shared media
  (`ahci_test.py` makes the same call about `noahci`).

  **A SECOND PHASE covers virtio**, in its own VM slot: virtio-input and
  virtio-net take a vector each, and the same "enumerates fine, delivers
  nothing" hazard applies, so the checks are again counts that must rise
  -- decoded input events, and an ICMP round trip to QEMU's own SLIRP
  gateway that can only arrive through the receive queue. **The two
  phases must not share a slot**: `QMPSession` leaves the QMP port in
  `TIME_WAIT` for about a minute, so reusing it either waits that out or
  is refused by `port_guard` as a clash with the guest just killed
  (`kbd_test.py` hit the same thing and solved it the same way).

  **What a control here MEASURED, and it is worth not re-deriving:**
  gating `virtio_irq_is_ours()` on the ISR byte under MSI-X changed no
  count at all, because QEMU's `virtio_irq()` writes the ISR before
  dispatching the vector. The control that DOES go red is returning 0
  from it outright -- 0 events decoded against 33 vectors delivered --
  and that is what shows the tool can see a dead path. On demand.
- **`nvme_test.py`** -- one QEMU NVMe controller with TWO namespaces
  and no other disk: namespace 1 a copy of `disk.img` (SeaBIOS boots it,
  the root mounts from `nvme0p3`), namespace 2 blank with 4096-byte
  blocks (`mkpart`, `mkfs`, mount, copy). Two boots, on demand.

  Its load-bearing checks: `ktest nvme` with **0 skipped and every
  KTEST in `nvme_test.c` run** (counted from the source, so a stale
  filter fails); the `sum`s after a reboot; and TRIM measured from the
  HOST, asserting the 4K image GREW by the 40 MiB written before it
  asserts the delete shrank it -- without the growth, the shrink passes
  vacuously (`docs/roadmap-details.md` has the tool where that
  happened). The KTEST `a completion raises the interrupt` is what
  proves MSI-X delivers: a dead vector fails no read, because a
  sleeping waiter wakes at its deadline and reaps anyway. The control
  that turned it red was a handler that never counted.
- **`sector4k_test.py`** -- a **virtio disk with 4096-byte logical
  sectors** (`logical_block_size=4096`) beside the IDE root: `mkpart`,
  `mkfs -t fat32`, `mkfs`, mount both, copy a multi-block file onto each,
  reboot, compare `sum`s. Two boots, on demand.

  **Its oracles are the ones the guest cannot fake**: the GPT header at
  byte 4096 with first-usable LBA 6, the FAT32 BPB's 4096, and mtools
  extracting the file byte-identical. And it greps `dmesg` for the
  block layer's "not whole ... blocks" refusal and virtio's IOERR line,
  which is what went red when the control removed the alignment check
  and reverted TFS3 to one-sector reads. **A mount point that failed to
  mount is still a directory on the root**, so a `cp` into it reads back
  fine; the read-back checks require the `mount` listing to show each
  path on its own partition, which is how the first version passed a
  failed TFS3 mount.
- **`ahci_test.py`** -- boots with the filesystem on a **SATA drive
  behind an ICH9 host bus adapter**, which is the only thing here that
  reaches `kernel/drivers/ahci.c` at all. Several boots, on demand.

  **Its load-bearing check is `0 skipped`.** Every `ahci` KTEST guards
  itself on a controller being on the PCI bus, so on the default IDE
  boot all five skip and the suite is green because nothing ran -- the
  exact failure mode this repo keeps finding. Asserting the skip count
  is what makes the KTESTs mean anything.

  The other three that a broken driver would not pass: **`df` reporting
  a persistent TFS3 root** (a kernel that ignored the controller still
  BOOTS, just into ramfs, so "it booted" proves nothing); **a REBOOT**
  between the write and the read, since a write that only reached a
  buffer passes a same-boot read-back; and **mtools reading the same
  `/boot/grub/grub.cfg` off the host**, an oracle sharing no code with
  either the AHCI driver or `fat32.c` -- `mtype` on the image versus the
  guest's own `cat`, the same call `fat32_test.py` and
  `regex_hostcheck.py` make. That phase SKIPS without mtools.

  **TRIM gets its own oracle, and it is the host filesystem.** A drive
  that acknowledges DSM and discards nothing is indistinguishable from a
  working one inside toy-os -- that exact failure shipped once in
  `ata.c`, where the range list went out over PIO and never arrived. So
  the tool writes 40 MiB, deletes it, and requires the sparse image's
  ALLOCATED size (`st_blocks`) to come back to baseline. Both halves are
  asserted: a check that only looked for the blocks coming BACK passes
  trivially on an image that never grew.

  A later boot rewrites the image's `grub.cfg` with mtools to add
  `noahci`, and asserts the driver STILL runs while the block layer does
  not take it -- the precedence rung, and the only way to test a boot
  word without `make iso KCMDLINE=`, which would rebuild the media every
  other tool shares.

  Its positive control is to stop `build_prdt()` advancing the buffer
  address between entries. That reddens exactly the multi-entry PRDT
  KTEST and NOTHING ELSE -- the machine boots, mounts, writes and
  reboots perfectly, because TFS3's 4 KiB blocks are one PRD entry. It
  is a good demonstration of why the check is written as a comparison
  against single-sector reads rather than as "the data came back".
- **`poweroff_test.py`** -- proves the machine stops through **its own
  ACPI tables** rather than a hardcoded port, on two chipsets.
  It boots its own guests and deliberately ends three of them.

  **"Did it shut down" is exactly the assertion a broken version
  passes.** The old implementation wrote `outw(0x604, 0x2000)`, and
  QEMU's own FADT happens to name port 0x604 and sleep type 0 -- so the
  hardcode and a correct table walk stop the guest identically. What
  separates them is the kernel log: `acpi: S5 via PM1a ...` is printed
  by the path that read the FADT and the DSDT's `_S5_`, and
  `power: falling back to the QEMU/Bochs PM1a_CNT port trick` only when
  that path declined. The check is the first line's presence AND the
  second's absence, plus the QEMU process really exiting -- which is a
  fact about the host that the guest cannot fake.

  **Two chipsets, because one is not a sample.** i440fx (the default)
  presents a revision-0 RSDP, an RSDT, a 116-byte FADT and NO reset
  register; `-machine q35` presents a 244-byte revision-3 FADT with a
  real reset register at port 0xcf9 and an MCFG beside it. The reset
  half can only be tested on the second, and its assertion is that the
  machine COMES BACK (two "debug console ready" lines on one QEMU
  process), not merely that the process ended.

  Its positive control is to make `acpi_poweroff()` return 0 at its
  first line: the two log lines swap over, the guest still powers off,
  and exactly the two discriminating checks go red. What it cannot
  cover is VirtualBox and real hardware, which is what motivated the
  feature -- what is testable here is that the numbers come from the
  tables rather than from a constant, which is the property those
  platforms need.
- **`virtio_boot_test.py`** -- boots with **no IDE controller at all**
  and the filesystem on virtio-blk, then writes a file, REBOOTS, and
  reads it back (6 checks). Builds its own QEMU; on demand, not in the
  gate. It exists because the `virtio` KTESTs cannot cover this: on
  those boots ATA still owns the filesystem, so nothing exercises
  `block_virtio.c`'s adapter or `vfs.c`'s precedence rule. And the
  reboot is the point -- a write that only reached a cache passes a
  same-boot read-back, so the round trip is what proves the bytes
  landed on the disk. Its positive control is to make
  `virtio_blk_write_sectors()` return success without issuing anything:
  that reddens exactly the round-trip check.
- **`fullscreen_test.py`** -- the fullscreen state and the display lease
  (`docs/scanout-design.md`), on virtio-gpu because the lease needs a
  cursor plane. Spawns `/tests/fsclient`, a write-only client that goes
  fullscreen at once, and asserts the reported state and rect, the
  taskbar rows in the client's colour, `gui fb --json` naming the
  lessee, frames still flowing while the compositor's present count
  stands still (the compositor is not presenting, so they can only be
  the client's own flips), the Start menu taking the lease and giving
  it back, F11 and Alt+F4. Its positive control was the overlay rule
  dropped from the policy, which left the lease standing under the
  Start menu -- one check red, the right one.
- **`virtio_gpu_test.py`** -- the ONLY thing here that boots
  `-vga virtio`, which is the whole reason it exists: every other GUI
  tool and `make test` launch the default adapter, so the virtio-gpu
  KTESTs would skip on every run and the suite would stay green either
  way. It launches a guest through `vm.py --vga virtio`, runs
  `ktest virtio-gpu` INSIDE it (the driver's own state -- active
  display, surface geometry, a flush being exactly two commands) and
  reads the PIXELS from outside (the desktop is drawn, and it keeps
  changing, which is the control that separates a live display from one
  frozen after its first frame). Its pixel-format oracle is a second
  boot on `-vga std`: the same OS drawing the same desktop on a
  known-good layout. That comparison is the only check that survived
  the positive controls -- "is anything on screen" passes on a black
  screen, and a red/blue channel test passes on a format that rotates
  channels rather than swapping two. The cursor plane is checked by
  what CAN be observed (the commands complete, and showing the pointer
  repaints no framebuffer pixels), because a device-composited cursor
  is handed to the display client out of band and never appears in a
  `screendump` at all. On demand, not in the gate.
- **`panic_store_test.py`** -- the panic store (`kernel/panic_store.h`):
  a kernel panic's log survives the warm reset that follows it, and
  logd appends it to the DEAD boot's file. **IT PANICS ITS GUEST ON
  PURPOSE**, through `config set kernel.crash gp-fault` on a disk COPY
  whose GRUB line gains `faultinject` (`install_grub.add_boot_word()`),
  so it needs `vm.py --reboot` and lives in the sweep. It checks that a
  cold boot recovers nothing, that the countdown restarts after ~10 s
  rather than at once, that the dead boot's log ends with the crash
  announcement and the `#GP` report, and that a cleared record is not
  filed again. The trigger runs in the BACKGROUND: a panicked guest never
  answers, and waiting on the exec started the countdown's clock a whole
  timeout late. `--positive-control` restarts COLD after the panic, where
  every recovery check must go red.
- **`netheal_test.py`** -- `/bin/netheal`, which reboots a machine once
  when it comes up with no network. **THIS TOOL REBOOTS ITS GUEST TWICE
  ON PURPOSE**, which is the property under test, so it needs
  `vm.py --reboot` (the default `-no-reboot` ends QEMU on a guest
  reboot instead) and lives in the sweep rather than the gate. The
  assertion that matters is not that it reboots but that it **STOPS**:
  the counter in `/var/lib/netheal` is the whole safety design, and
  without it a machine with genuinely no NIC reboots forever. So the
  tool checks the counter reaches the ceiling, then samples uptime
  twice and requires it to GROW -- a machine still rebooting keeps
  resetting it. It also checks the give-up line is logged once, and
  that a boot WITH an address clears the counter back to zero. Its
  positive control leaves the feature off, where a clean report would
  mean the tool is watching nothing.

- **`logrotate_test.py`** -- `logd`'s per-boot retention:
  `/var/log/boot/<n>.log`, `storage.log_keep`, and `log -p N`.
  **REBOOTS ITS GUEST FIVE TIMES ON PURPOSE**, because retention across
  boots cannot be checked inside one, so it needs `vm.py --reboot` and
  lives in the sweep rather than the gate. Four assertions, each aimed
  at a way the retention can look fine and not be: the numbering has no
  gap and no reuse (a reused number silently OVERWRITES the evidence
  somebody rebooted to keep); `log -p 1` and `log -p 3` return
  DIFFERENT text, which a resolver ignoring `N` could not; `log -p 99`
  is refused rather than answering with the wrong boot; `log -p -n 2`
  still honours `-n`, since the optional count could swallow it; and
  lowering `storage.log_keep` actually deletes rather than stranding
  the files above the new limit. It finishes by squeezing
  `storage.log_max` to 1 MiB and checking a boot that fills its share
  stops and says so. Its positive control sets `log_keep 1` -- the old
  two-file behaviour -- where a clean report would mean it is not
  watching the depth. It RESTORES the two settings in a `finally`,
  because a machine left at 1 MiB truncates every later tool's log and
  that reads as a hang.

- **`shutdown_sync_test.py`** -- a write made just before `reboot` must
  survive it. **REBOOTS ITS GUEST FOUR TIMES**, since the property is
  precisely what survives a reboot, so it lives in the sweep. It exists
  for a measured bug: `system_reboot()` flushed the ATA SECTOR CACHE and
  nothing else, which is a layer BELOW the filesystem -- `storage.sync =
  batched` keeps a TFS3 journal transaction open across writes, so
  blocks that transaction still owns never reached the cache and a flush
  could not save them. A `config set` followed at once by `reboot` left
  `/etc/storage.conf` at ZERO BYTES: the new value lost, and the old one
  too, because `fs_write()` truncates first and the truncation landed
  while the data did not. The three assertions separate the cases that
  fail differently -- updating an existing config file, creating one
  that did not exist, and an ordinary file, so the property is shown to
  be the filesystem's rather than one setting's. **The gap between the
  write and the `reboot` is the whole experiment**: a `sync` or a second
  of idle commits the transaction on its own and nothing is proved,
  which is why the helper issues them as one command list. Its positive
  control points the probe at `/tmp`, a tmpfs mount that CANNOT survive
  a reboot by design -- a clean report there means the tool is not
  reading back across the reboot at all.

- **`usb_test.py`** -- an xHCI controller and a HID boot keyboard and
  mouse (`vm.py --usb xhci` / `--usb xhci+mouse` / `--usb xhci+hub`, or
  `make run USB=xhci+mouse`). Five phases: keyboard, ring wrap, mouse,
  HOT-PLUG (QMP `device_add`/`device_del` on the running guest -- the
  only headless stand-in for a human plugging a mouse in, and the
  detach check must scope its lsdev assertion to the Input sources
  section, because the capture also carries the kernel's own
  `usb-mouse unregistered` log line), and HUB (both HID devices behind
  a `usb-hub`, which is what exercises route strings; QEMU's hub is
  full-speed, so the TT path stays hardware-only).

  **It is self-controlling, and that was measured before a line of the
  driver was written.** QEMU activates a keyboard handler the moment
  `usb-kbd` is attached and routes host keystrokes to THAT device, so a
  guest whose USB driver does not work receives nothing at all -- not
  over USB and not over PS/2 either. Proven on a build with no USB
  support: with the devices attached a typed `touch /marker` left no
  file, and without them the identical sequence left one. So every
  keystroke assertion here already has its control, with no scaffolding
  and no `i8042=off` needed. It is also why the axis is OFF by default:
  adding it to a tool written against PS/2 silently deprives that tool
  of input, which reads exactly like a guest bug.

  **The load-bearing check is the RING WRAP, not the first keystroke.**
  A driver that ignores the event ring's cycle bit, or never re-posts a
  consumed TRB, works perfectly for exactly one lap of the ring -- 256
  TRBs, about 128 keystrokes -- and then goes permanently deaf. A test
  typing a short marker cannot tell that from a working driver. Phase 2
  types 25 files, well past a full ring, and asserts the LAST one as
  well as the first. Its positive control is worth repeating: with the
  cycle flip removed, files 8 onward vanish while the FIRST file still
  passes.

  The oracle throughout is the FILESYSTEM read back over the serial
  console, never the screen -- a keystroke that worked leaves bytes on
  disk, and the serial console does not care who owns the keyboard.

  Two harness traps it encodes, both of which cost a wrong diagnosis.
  **`send_key` silently drops a character it has no qcode for**, so
  `touch /usb_one.txt` quietly created `usbone.txt` and read as a dead
  driver; anything not a bare lowercase letter or digit is spelled out,
  and a shifted character goes through `combo()`. And **the serial
  socket must be drained as the test types**: with the text target every
  spawn logs to COM1, nothing else reads it, the buffer fills, the
  guest's serial write blocks and the whole kernel stalls -- which
  presents as a driver dying after N keystrokes and recovers the instant
  anything reads the socket, so a dump taken afterwards looks perfectly
  healthy. Measured: 5 of 25 files without the drain, 25 of 25 with it.

  `--kvm` is not optional before believing a change here. The INTx storm
  the acknowledge path guards against is invisible under TCG -- the
  virtio-input version of it hung 3 boots in 3 under KVM and 0 in 3
  without. `--phase keyboard|wrap|mouse` runs one phase. On demand, not
  in the gate.

- **`virtio_input_test.py`** -- the virtio keyboard, mouse and tablet
  (`vm.py --virtio-input`, which is the only thing that attaches them).
  The guest keeps its PS/2 pair as well, deliberately: the input core is
  supposed to take several sources at once. Five checks earn their
  place. **The guest must finish enumerating all three devices with the
  POINTER MOVING through the whole boot** -- motion is injected over QMP
  from the instant QEMU starts, so the tablet has events waiting the
  moment its INTx is enabled. That is the check the virtio-input INTx
  ordering bug was found by, and it only runs with `/dev/kvm`: the race
  hung 3 boots out of 3 under KVM and 0 out of 3 under TCG, so on a
  machine without KVM it prints a SKIP with the reason rather than a
  green check that cannot fail. It also asserts the ENUMERATION rather
  than the boot's exit code, because a first version tested `vm.py
  start`'s return and passed on a guest that had wedged partway through
  -- readiness comes from the serial console, which is up before
  virtio-input runs. The tablet must report `abs` and the mouse must NOT, or the
  capability is telling us nothing. Two devices must SHARE an interrupt
  line -- QEMU routes the mouse and tablet onto IRQ 10 together, which
  is the case `kernel/arch/x86_64/irq.c`'s handler chain was rewritten
  for. An absolute position must land EXACTLY where the arithmetic says
  (the tablet's 0..32767 range scaled to the screen). And Super must
  open the Start menu, which is one assertion covering the whole path
  from the virtqueue through the input core, the key ring and the
  kernel's raw-input forwarder to the ring-3 compositor.

  It reads the IRQ assignments from `lsdev`, NOT from `dmesg`: the boot
  line saying which line each device took has rolled out of the kernel's
  ring buffer by the time a desktop has been up a few seconds. An oracle
  that expires fails for reasons unrelated to the code. On demand, not
  in the gate.
- **`live_boot_test.py`** -- boots `toy-os-live.iso` with NO disk and
  asserts a shipped binary RUNS, plus that `df` reports the image's real
  size and says it does not persist. Not in `gui_regress.py` (it builds
  its own QEMU); run it after touching the block layer, TFS3's geometry
  or the live path. **"It booted" proves nothing here** -- the kernel
  falls back to ramfs and still reaches a shell and a desktop, which is
  exactly what an empty live boot looks like.

  **Repaired 2026-08-25, after being unrunnable for an unknown period.**
  It parsed `df`'s `used:`/`total:` LINES, which became a TABLE when
  `df` moved to `/bin`, so every number came back 0 and three checks
  failed against a live boot that was working perfectly. Nobody saw it
  because the live image could not be BUILT: `LIVE_IMG_MB` was a
  hardcoded 24 and the seed tree had reached ~33 MiB. Both are fixed --
  the size is derived from the tree now -- and the lesson is the one
  `check_tool_commands.py` cannot catch: **it sees a command that was
  renamed away, never one whose OUTPUT changed.**

  **A SECOND PHASE, 2026-08-27: the mount POLICY** (`--phase policy`,
  and part of a bare run). "Live only when asked for, or when there is
  no disk" is what stops a live session displacing an installed system,
  and it was enforced by asking `ata_present()` -- the LEGACY IDE probe
  -- so on a machine whose only disk is AHCI or virtio the kernel
  concluded "no disk" and took over anyway. **Every live test here
  booted with NO DISK, which is the one configuration where the right
  and the wrong predicate agree.** The phase boots one scratch ISO (it
  builds its own, with the `live` word left out) twice and requires
  OPPOSITE outcomes -- declined with an AHCI disk attached, used without
  one -- so each run is the other's control. Reported by a user booting
  real hardware, which is where this class of bug lives.

  It reads the kernel's own `fs:` lines off a serial log rather than
  driving the console, and it builds its ISO from `build/kernel.bin`, so
  **build before running it** -- a tool that restores a working tree
  without rebuilding (`predates.py`) leaves it testing the other
  kernel.
- **`boot_rate.py`** -- reboots a BARE-METAL machine N times and counts
  how often a substring appears in its `dmesg`. An intermittent fault is
  a RATE, and `docs/bugs.md` asks for exactly this by name for the USB
  enumeration bug ("reboot that machine 5-10 times counting `dmesg |
  grep -c polls`"). Three things it gets right that doing it by hand
  does not: it waits for the machine to go DOWN and come back rather
  than sleeping, so it cannot read the previous boot's log; it stops
  dead if the machine does not return, because every boot counted after
  a machine you have lost is a lie in the denominator; and it prints a
  line per boot as well as the rate, since an intermittent that CLUSTERS
  is a different animal from one evenly spread and a total hides that.
  It does not flash -- measure one kernel, and change kernels
  deliberately between runs. On demand only: it needs hardware.

- **`compositor_death_test.py`** -- the compositor death path (M41's
  R7, 10 checks). Killing the compositor must not panic the kernel, and
  must not take the desktop with it. Two things it encodes. **The
  teardown is conditional on that compositor BEING the desktop** -- while
  the ring-0 WM is registered it still owns the screen, so a stand-in
  compositor leaving is a second consumer going away, not a desktop
  dying; the first version tore down live windows and `compositor_test`
  caught it as UI Demo going silent. And its positive control reddens
  exactly TWO of the ten, because the other eight are regression cover
  for stage 4a's role-clear path rather than tests of R7 -- read that
  before trusting a green run. In `gui_regress.py`.
- **`compositor_test.py`** -- M41 stage 2's raw input path to a
  registered ring-3 compositor (`userland/tests/compclient.c`), 16
  checks. Its design point: every injected input is asserted TWICE, once
  in the compositor's log and once in UI Demo's, because "the compositor
  received the click" is equally satisfied by an implementation that
  stole the input stream outright -- and stage 2's whole shape is that
  both paths run at once. Run it after touching `userland/wm/wm.c`'s loop,
  `win_role.c`'s compositor registration, or the `WIN_EV_RAW_*`
  events. In `gui_regress.py`.
- **`screen_surface_test.py`** -- a ring-3 compositor's SCREEN surface
  (M41 stage 4b): the back buffer, the clip rect, the damage box, the
  blit, the publish path and R2's verify diff, driven through
  `userland/tests/screenclient.c`. 14 checks. Run it after touching
  `userland/ui/ugfx.[ch]`, `SYS_SBRK`, or `kernel/include/kernel/
  uaddr.h`. Two things it encodes. Every geometric check asserts an
  EXACT number, not "it changed" -- a damage box that covers only the
  last rect passes any did-it-change test, and a clipped blit that
  offsets its destination but not its SOURCE draws the right count of
  pixels in the right box with the wrong contents. And its first check
  is a real gate on the ring-3 HEAP: the client reports geometry only
  if sbrk handed over a full screen of back buffer.

  **And a race worth knowing about, because it is the shape of the
  tool's one intermittent.** The client prints `screenclient: step <k>`
  and that step's reply as two SEPARATE writes. `one()` used to sweep the
  log once and scan the section after the marker, so a sweep landing
  between the two reported "no reply" for a client that was working
  perfectly -- measured 2 runs in 4 once boot got slightly slower, with
  every other check in the tool passing. It polls now (6 in 6). A marker
  is not a guarantee that what follows it has arrived. In
  `gui_regress.py`.
- **`desktop_entries_test.py`** -- the `.desktop` entry system: the
  `ShowIn=` key and live reload, 13 checks. Its reusable lesson is in the
  ShowIn checks: they assert an entry is **LOADED but filtered** (`gui
  apps` versus `gui menu`), never just "absent from the menu" -- the
  first version asserted only absence and passed with the filter
  disabled outright, because `write` truncates and the check was racing
  the transient invalid file. In `gui_regress.py`.
- **`settings_test.py`** -- the ring-3 System Settings app and, through
  it, the settings registry. Run it after touching
  `kernel/lib/setting.c`, `SYS_SETTING`/`SYS_SYSINFO`, or
  `uui_sidebar`/`uui_radio_list`/`uui_spinbox`/`uui_statusbar`/
  `uui_layout`'s `hidden` handling. ~70 checks. (It was `cpanel_test.py` until the app was renamed on
  2026-08-19 -- Control Panel is Windows' name, and this shows exactly
  the SETTINGS registry.) Two things it encodes. A change is verified by reading the
  BYTES ON DISK through the console's own `sh cat`, not by believing the
  app -- and note `/bin/config get` does NOT work for this, because a
  spawned program's stdout goes to its parent's pipe rather than the
  kernel log (only stderr is readable from outside). And its
  hidden-page check measures the SAME RECT in both states: the first
  version compared the widget's band against the WHOLE page's ink, a
  baseline so much larger that a positive control (making the layout
  ignore `hidden` again) reddened nothing at all. Recorded numbers:
  55% of the shown ink survives when hidden works, 98% when it does not.

  It also drives the TIMEZONE DROPDOWN, which is where three things are
  covered that nothing else reaches: a choice's display name (the app
  logs `settings: choice <n> <name> raw <token> shown <display>`, since
  a screendump cannot tell a missing display name from a value that
  reads like one), HOVER as pixels, and keyboard type-ahead. Three
  fixture rules that cost a run each. The timezone is set BEFORE the app
  starts, because the app caches a setting's value when it builds a
  page. The page is reached by navigating AWAY and BACK, because the
  layout log is deduped per frame and re-opening the page already shown
  logs nothing at all. And the row hovered must not be the SELECTED row
  -- selection correctly outranks hover, so hovering the current city
  measures nothing and reads as a dead hover.

  **NOTHING IS AIMED AT WHERE IT WOULD BE UNSCROLLED.** Three rules,
  each of which cost a check that measured nothing. A SIDEBAR ROW is
  clicked through `open_row()`, which scrolls it into view and derives
  its current position from the app's reported offset: the row dump is
  taken once, at the top, so row 30 of 31 is reported at a y below the
  window -- and a click there lands on the TASKBAR, whose button for
  this window MINIMIZES it, after which the app draws nothing, reports
  nothing, and every later check reads the state it had before. A
  CONTROL is scrolled fully inside the PAGE VIEWPORT by `reveal()`,
  which is the page rect and not the window: a control whose lower half
  hangs past the scroll view is drawn clipped and a press there is
  clipped away too, which is why the mouse-speed spinbox stepped up and
  never down. And a PAGE is confirmed open by its own `settings: page`
  report before its controls or its pixels are read, since an empty
  control list means either a page with no controls or a page that never
  opened. `click()` refuses a point outside the content area outright,
  with a check naming the coordinate, so the whole class fails loudly.

  It also drives the SCREENSAVER PAGE, whose option rows are not
  registry settings at all: they are synthesised from the chosen saver's
  descriptor (`userland/lib/usaver.h`) and written to a file of its own,
  so the checks name the SAVER the rows claim to belong to rather than
  counting them -- "there are controls" is satisfied by the two registry
  settings alone. Two things it encodes. The saver is chosen BY NAME
  through a scan, because the popup is a `uui_listbox` with no
  `describe` and a row pitch worked out once from a font size is the
  constant this repo has re-measured three times; and the rows are read
  from the window `pick_saver()` already drained, because the layout
  report is deduped per frame and the rebuilt page is described exactly
  once. Asking for "any saver but this one" picked `blank`, which ships
  with no descriptor and correctly shows nothing -- the swap check now
  names a saver that has options.
- **`screenshot_test.py`** -- screen capture end to end: `/bin/screenshot`,
  the Screenshot app, `--pointer`, `-w`, and the region band.

  **The assertion that matters is a ROUND TRIP, not "a file appeared".**
  A capture that wrote a well-formed image of the WRONG THING passes
  every did-it-produce-a-file check there is, and both ways it could be
  wrong are real: the compositor's back buffer can be a frame old, and
  its alpha byte is zero, which every alpha-aware consumer reads as
  fully transparent (that one shipped, as a black preview). So the file
  is pulled OFF the guest's TFS3 volume with `tfs3_writer.py read` and
  compared pixel by pixel against a QMP screendump, by **Pillow** -- a
  decoder sharing no code with ours.

  **The cursor box is its own control.** Outside it the two images must
  match exactly; INSIDE it they must differ, or the comparison is
  passing because the capture is a copy of the screendump rather than
  because it is correct. `--pointer` is then checked as a COUNT OF
  DISTINCT COLOURS in that box (a cursor is an antialiased arrow: three
  wallpaper shades become twenty-six), not as "the files differ", which
  two captures of a moving desktop would satisfy with no cursor in
  either.

  It asks QEMU which image the guest has (`info block`) rather than
  assuming `disk.img`, because `gui_regress.py` boots a per-tool copy
  and a hardcoded path would read a file the guest never wrote.

- **`screensaver_test.py`** -- the idle clock in the compositor, the
  two settings, and the savers it spawns. A saver is a PROCESS, so
  `gui idle` reports its pid and the checks ask for that rather than
  guessing from pixels; the window it owns has to reach the screen's
  size before anything is asserted about it, because a pid is not a
  window and a window is not yet a fullscreen one. Both input paths are
  driven separately -- a key and a pointer move arrive at different
  places in the frame loop, and a clock reset wired to only one would
  look correct from whichever the test happened to use. The blank saver
  is checked as PIXELS, since "a window opened" is not "the screen is
  dark". **The lever is `gui idle start`**, which skips the WAIT and
  nothing else: the shortest timeout a person can configure is one
  minute, and it still refuses while the timeout is zero, so "off means
  off" stays testable. It puts both settings back, including after a
  failed check.

  **And it checks that a saver's OPTIONS reach its pixels**, which is
  the half neither `/tests/usaver_test` (does the file parse) nor
  `settings_test.py` (does the app write it) can see: a descriptor wired
  up and a `#define` still in place look identical from both. The tint
  is asserted as a COMPARISON BETWEEN CHANNELS -- amber is redder than
  it is blue, ice the other way -- because an absolute value would need
  a threshold picked from one machine, and a saver ignoring the option
  draws white, where the two are equal. The star count is asserted at
  the two ends of its declared range, twenty times apart, which no
  frame-to-frame variation in a moving field can cover. The conf file is
  written with `tosh -c` and its redirection, UNQUOTED: the kernel
  shell's `spawn` passes a quoted word through with its quotes, so tosh
  re-lexes it as one word and runs a command called "echo colour=amber".
- **`settings_harness_hostcheck.py`** -- `settings_test.py`'s own
  geometry and waits, on the host against a scripted console: a sidebar
  row is aimed at where it IS rather than where it would be unscrolled
  (the row dump is taken once, at the top, and the last rows are
  reported below the WINDOW, where a click hits the taskbar and
  minimizes the app); the scroll offset is read from the newest report
  and waited for, since it is logged only on a change; a page is
  confirmed open by its own report before anything reads its controls;
  and a control counts as reachable only when its WHOLE rect is inside
  the scroll view, because the clip is at the viewport edge and a
  straddling control is half routable. Seconds, no guest.
- **`iso_guard.py`** -- refuses to boot a stale `toy-os.iso`, called
  from `vm.py` and `qmp_test.py`'s `launch_qemu_cmd()`. `make all`
  without `make iso`, or a `make iso` that FAILED, otherwise leaves the
  whole suite testing the previous build and reporting a clean PASS --
  which is the worst possible direction and has cost time in many
  sessions. Each source tree is checked against the artifact it feeds
  (`userland/` -> `build/userland`, not `kernel.bin`), and the seeding
  step is witnessed by `build/.seeded` rather than `disk.img`'s mtime,
  because seeding is content-hash based and a byte-identical rebuild
  correctly rewrites nothing. `TOYOS_ALLOW_STALE_ISO=1` bypasses it, for
  deliberately booting an older image -- e.g. building an earlier commit
  to prove a failure predates your work.

  **It guards the STAGING tree and single staged FILES too, which is the
  same trap one layer down.** `make all` writes `build/` and stops;
  `seed/sync/` is populated by the `seed` step `make iso` runs. So
  `check_staging_fresh()` refuses a `remote.py flash` from stale
  staging, and `check_staged_file()` refuses a `remote.py put` that
  names one `seed/sync/` file older than its `build/` counterpart --
  naming the `build/` path to send instead. The single-file case was
  added 2026-09-22 after a rebuilt sound plugin was pushed twice from
  staging while the fix sat in `build/`: the second push reproduced the
  first push's symptom exactly, which reads as the fix not working
  rather than as the fix never having been sent. Same
  `TOYOS_ALLOW_STALE_ISO=1` bypass, because it is the same mistake.

  It also WARNS (never refuses) when a COPY of `disk.img` passed with
  `--disk` is older than the last seed. Testing against a copy is the
  documented way to dodge QEMU's write lock and to stop `make iso`
  re-seeding an image underneath a running VM -- but `make iso` re-seeds
  the real `disk.img` with the newly built `/bin` binaries AND installs
  the new kernel on it, so a copy taken before a rebuild is an entire
  earlier build: it boots the previous kernel too, with nothing
  mismatched to give it away. (Before the disk carried a kernel this was
  the narrower "new kernel from the ISO against the old userland"; the
  warning names whichever case applies.) Either way it reads exactly
  like a bug in the app. Measured 2026-08-19, in the
  worst possible place: a POSITIVE CONTROL for `/bin/ls`'s truncation
  message, where the guest ran the previous `ls` and the message did not
  appear -- a control that fails reads as "the feature is broken", not
  "the fixture is stale". A warning rather than a refusal because a copy
  is often deliberately old (a fixture staged by `tfs3_writer.py`, an
  image kept for a reproduction); it names the re-copy command.

  **It asks the compiler which artifact a source feeds; it does not
  guess from the directory.** Pairing by tree is a guess, and it was
  wrong for `kernel/include/api/build_date.h` -- which sits under
  `kernel/` and is included only by `userland/wm/desktop.c`, so it never
  rebuilds the kernel. `gen_version.sh` rewrites that file whenever the
  DAY changes, so this refused a perfectly current image on the first
  build after every midnight and then self-healed as soon as anything
  touched the kernel, which is why it went unnoticed. The `.d` files
  `-MMD` already writes carry the real answer (and `check_deps.py`
  proves that tracking is live), so the mapping is read from them. A
  source no `.d` mentions keeps the directory pairing, which is the
  conservative fallback -- so this can only ever make the guard quieter
  about a file the compiler positively placed elsewhere, never about a
  new one.
- **`thumbcache_test.py`** -- the File Manager's thumbnail RATE, and the
  disk cache under it (7 checks). **THE ASSERTION IS A RATE, NOT A
  DURATION**: decoding used to be two per 500 ms tick, so four a second
  was that design's CEILING by construction, and a measured rate above
  it is something it could not have produced however fast the host is --
  where a wall-clock bound would be a bet on the emulator. Measured at
  24/s on TCG; the bar is 8. It reads the app's own drain report
  (`files: N thumbnail(s) in M ms, C from the cache`) off the WIRE
  through `DebugConsole.logs()`, **never `sh dmesg`** -- the kernel ring
  holds a few hundred lines and any tool that has turned the layout log
  on fills it in about a second, so the first version of this check
  asked dmesg and found NOTHING with the app working perfectly. It is
  its own tool rather than a section in `filemanager_test.py` because it
  needs to START A FRESH APP TWICE, once cold and once against a
  populated cache: a second process is what proves the DISK was read
  rather than an in-memory table. Two fixture traps it paid for:
  `/etc/files.conf` remembers the view mode, so it removes that file to
  get the icons view rather than inheriting whatever ran last; and
  toy-os's `touch` does NOT move an existing file's mtime, so the
  staleness check copies a different icon over one instead -- the first
  version went red against working code because the input never reached
  the branch. In `gui_regress.py`.
- **`wait_for.sh`** -- wait for a PID to exit or a file to appear, with a
  MANDATORY timeout. It exists because the hand-rolled version has a
  failure mode that outlives the session: `while ps aux | grep -q
  "[f]oo"; do sleep 15; done` can NEVER exit, because the waiting
  shell's own command line carries the word `foo` somewhere -- the
  reporting `grep` after the loop is enough -- so `ps` sees it and the
  condition stays true. The `[f]oo` trick hides the grep from itself and
  does nothing about the shell around it. **Two of those ran for two
  hours in one session while `preflight.sh` reported none**, because its
  detector looked for `while [` and these were `while ps ... | grep`
  (both are fixed). This matches no process NAMES at all: a pid is
  `kill -0` and a file is a test, neither of which can see the waiter.
  Expiry is exit 2, distinct from the awaited thing failing, because a
  waiter that gave up silently reads as success. A pid that is already
  gone is exit 0, not an error -- that is the condition already true,
  which is the common case when a waiter is armed a moment too late.
  **Prefer not needing it**: a backgrounded command's own completion
  notification is the signal, and a waiter beside it is redundant even
  when it works (CLAUDE.md). Named by no runner -- it is a helper, not a
  test.
- **`shortcut_test.py`** -- global keyboard shortcuts: the default
  bindings, the Super gesture, and rebinding one through System Settings
  (15 checks). **It asserts on what the compositor LAUNCHED**, from its
  own `wm: shortcut -> <command>` line, not on a window appearing -- a
  window is also what a Start-menu click gives you, and this is about the
  key path. Four traps it pins, each of which has already bitten:
  **the command paths drift from `data/wm/applications/`** (two of four
  were wrong first time, and the symptom was `pid -1` in a log nobody
  read, so the first check lists each one on the disk); **Super held over
  another key must not open the Start menu**, or every Super shortcut
  opens it on the way out; **capture needs the compositor to stand down**,
  checked by recording a combination that IS already bound and asserting
  no file manager appeared; and **the rebinding is end to end** -- Apply,
  then press the NEW key -- because `usetting_get()` returns 1 for
  success and reading it as a syscall result made every binding fall
  through to its fallback, so a rebinding was stored, reloaded and
  ignored while every cheaper check passed. It leaves
  `/etc/shortcuts.conf` removed, so the machine is as it was found. In
  `gui_regress.py`.
- **`taskmgr_test.py`** -- the ring-3 Task Manager: `uui_table`, resize
  reflow, and ending a process (12 checks). Its resize check asserts the
  table grew by ROUGHLY WHAT THE WINDOW GREW BY, not merely that it
  changed -- the bug it was written after grew the width correctly and
  the height by 16 px against 300, so "it changed" was satisfied. On its
  first run it found a pre-existing bug in `uui_listbox` (see the
  widget-`hit` trap in the widget section above).

  **IT IS ALSO THE ONLY PLACE IN THE SUITE THAT USES THE KEYBOARD ON A
  TABLE, and it was not, for most of its life.** Every check in it drove
  by MOUSE -- rows, headers, buttons -- so the app's entire key handler
  could be absent and all of them passed, which is exactly what happened:
  Task Manager declared neither `uapp_desc.focus` nor `on_key`, so the
  table's arrows, Home/End, paging and type-ahead had never worked there.
  Two checks cover it now, and the second is the load-bearing one: an
  arrow must reach the table at all, and a typed letter must select by
  NAME rather than by the PID column. **Ask which input device a tool's
  checks use** -- a suite that only clicks cannot see a dead keyboard.
- **`single_instance_test.py`** -- one copy of an app, and relaunching
  it raising the copy that exists (`WIN_REQ_ACTIVATE`,
  `UAPP_SINGLE_INSTANCE`; 9 checks). Run it after touching TWP's create
  path, `wm_client.c`'s window list or `uapp_run()`'s startup. Two of
  its checks are worth copying: the multi-instance CONTROL (UI Demo
  declares no app id, so two windows is the right answer there, and an
  over-eager match reddens exactly that check), and identifying the
  raised window by **`client_pid`, not by title** -- with the raise
  disabled a brand-new window is frontmost too, so the title-only
  version of that check stayed green through the positive control.
- **`gui_regress.py`** -- runs every GUI test tool, each against
  its own freshly-copied disk image and its own VM, and prints one
  pass/fail table (~1.5 minutes; ~300 checks across ~25 tools, a snapshot rather than a maintained count). This is the standard check
  after touching `apps/ui/`, `userland/`, or anything the WM draws.
  Tools are **STARTED longest-first** (`COST_S`/`pick_order()`), because
  a parallel run cannot end before its slowest member does and
  `forcequit` (71s) used to sit eleventh of fourteen and finish alone --
  that sort alone took a run from 1:56 to 1:30. The summary table is
  still printed in declared dependency order; only the start order
  changed, and a tool missing from `COST_S` is assumed SLOW so a new
  one can never become the straggler by omission.
  `-k NAME` for a subset, `--logs DIR` to keep each tool's full output,
  `--list` to see what's in it. The per-tool fresh image and fresh VM
  are the parts that matter: several tools write files, and every one
  of them expects an empty desktop -- a tool inheriting the previous
  one's state fails in ways that look exactly like real widget bugs.
  It runs **half the host's cores' worth of tools at a time** (`cores//2`
  counts hardware threads, ~one guest per physical core), capped at
  12 (`-j N` to change, `-j1` for the old serial behaviour -- that took
  107s), each in its own **VM slot**:
  `vm.py --instance N` derives that VM's pidfile, serial socket, QMP
  port and VNC display from one number, and the slot is LEASED for as
  long as the VM lives rather than derived from the tool's position in
  the list. Use `--instance` yourself any time you need a second
  headless VM alongside one that's already up -- **`--instance auto` takes the lowest FREE slot and
  prints which one**, the right thing when a `gui_regress.py` may be
  running in another terminal. It prints because a slot that differs
  per run must still be replayable (`--instance <that number>`), and
  it is a narrowing rather than a lock -- `port_guard.py` at the
  launch is what makes a residual clash loud instead of silent.
  **It does NOT fix CPU contention**: a tool run beside the full
  suite is port-safe but competes for cores, and an animating app
  can fail a settled-frame comparison under that load (measured --
  `gfxdemo` failed two checks with five guests up and passed 23/23
  alone). Slot 0 is the plain
  `.vm.pid`/`.vm.serial`/4445 every existing caller assumes.
  **WHERE THE WALL-CLOCK TIME ACTUALLY GOES, measured rather than
  assumed, because the obvious answers were both wrong.** A full run
  WAS ~425 tool-seconds across ~25 tools (it is ~408 now, see the end of
  this entry), so the wall clock was the SLOWEST SINGLE TOOL, not the
  total: at `-j4` it was 76s and at `-j8` 72s,
  because both are pinned by the same one tool. **KVM (`--kvm`) buys
  almost nothing either** -- 64s -- since what the slow tools spend
  their time on is WAITING for real timeouts, which no amount of guest
  CPU shortens. The lever that worked was cutting the floor itself:
  `forcequit_test.py` waits out the WM's not-responding timeout about
  ten times, so shortening that timeout for its run (`gui pingtimeout`,
  the 663d63b pass) took it from 72s to ~35s. A later pass made every
  tool wait on an OBSERVABLE rather than a fixed sleep -- `enter_gui()`
  polls the desktop ready instead of sleeping ~3s per tool, and
  `menubar`/`taskmgr` poll the app's own layout/log reports -- taking the
  sum to ~408 tool-seconds and leaving `notepad` (~41s) the floor. The
  general lesson is worth more than the seconds: **on a fan-out like
  this, look at the maximum, not just the sum** -- but once the max is
  cut, the sum over the job count starts to bind, which is why the two
  passes complemented each other and why the `-j` cap could then rise to
  12 for hosts with cores to spare.

  `damage_sweep.py` is deliberately NOT in it (much slower under
  `gui damage verify on`, and it has its own `--positive-control`
  protocol) -- run that separately.

  **A SECOND CONCURRENT RUN IS REFUSED**, with an flock under `build/`.
  Every tool takes a VM slot and slots start at 0 for every run, so two
  suites at once fight over the same pidfiles, serial sockets and QMP
  ports -- and it does not fail as a port clash. `port_guard` refuses
  some launches and the rest surface MINUTES later as `BrokenPipeError`
  or a screenshot that was never written, in whichever tool happened to
  be mid-command; seven tools "failed" that way in one run here, none of
  them at fault. An flock rather than a pidfile check, so a run killed
  with -9 leaves nothing to clean up.
- **`resize_edges_test.py`** -- a window resizes from all EIGHT edges
  and corners, and each one shows the right cursor. 8 cases, each
  naming the dimensions that must CHANGE *and* the ones that must be
  ANCHORED -- the second half is what has teeth, because pinning the
  wrong corner moves a window without resizing it and looks almost
  right in a screenshot. **IT RESETS THE WINDOW BEFORE EVERY CASE**,
  after the first version dragged it into the screen corner and then
  reported the WM as broken when the clamps correctly refused to grow
  it further; the reset is asserted, so a fixture failure says so
  rather than masquerading as a bug. It also takes the TOPMOST window
  of its title rather than `DebugConsole.window()`'s lowest, which is
  what a re-run against a guest that still has one hands you -- reading
  one window's geometry while clicking another's reddens the whole
  board with nothing wrong at all. Two checks beyond the drags: the
  TITLE BAR still drags end to end while the band just past its ends
  resizes (the guard for a corner zone growing back into the bar, which
  is what a "resize cursor far from the corner" report means), and the
  outside band ENDS -- asserted from both sides, since "nothing resizes
  out here" also passes when the band is broken outright.
  `--positive-control` prints every before/after geometry.
- **`resize_stride_test.py`** -- a resized window is composited at the
  size it was DRAWN at. 4 checks. **THE ASSERTION IS TWO NUMBERS, NOT
  PIXELS**: a shear is a stride disagreement, and reading the numbers
  beats hunting a diagonal in a PNG. It originally compared the kernel's
  record of each buffer against the compositor's; stage 6b deleted the
  kernel's record, so it now checks that the compositor's content size
  tracks every one-pixel step -- which it can only know from the frame
  -- with the buffer's GENERATION moving as the control that the buffer
  was really replaced. **It steps by ONE pixel and that is
  load-bearing**: a buffer's length is page-rounded, so a larger step
  takes a different path through the client's buffer handling.
  `--control` prints the edit that reddens it.
- **`uapp_test.py`** -- the TWP resize handshake and focus events, via
  `winclient` (which contains no resize code -- it sets
  `.flags = UAPP_RESIZABLE` and nothing else, so what is under test is
  Toykit's and TWS's). 8 checks. Its focus check is a ROUND TRIP:
  capture a Terminal's content focused, take focus away and require it
  to CHANGE, give focus back and require it to match the first capture
  EXACTLY -- "it changed" alone is satisfied by almost anything.
- **`doom_test.py`** -- DOOM runs, draws, animates and takes input. 6
  checks. **ON DEMAND, not in `gui_regress.py`**: it needs an IWAD, and
  the IWAD is deliberately not in this repository, so a checkout without
  one gets a clean SKIP rather than a failure.

  Two of its checks are shaped by mistakes made writing it. **"It
  animates" samples seven frames, not two** -- Doom's attract mode cycles
  demo, title, credits, and the title screen is legitimately STATIC, so a
  two-sample comparison lands on it often enough to flake. **The frame
  rate comes from the app's own report, not from the screen**, which the
  positive control proves is the right choice: breaking the blit turns
  three pixel checks red while the rate check stays green and the number
  goes UP, because it is no longer paying for a blit.

- **`fetch_wad.py`** -- puts a Doom IWAD at
  `data/doom/doom1.wad`, which `make iso` then stages onto the image.
  **`data/`, not `seed/sync/`**: the latter is a staging directory the
  build rewrites, so a 4 MB download placed there survives until the
  next `make clean` and then silently vanishes.
  The shareware `doom1.wad` by default; `--from` takes a local
  `freedoom1.wad` or a retail `DOOM.WAD` you already own.

  **It validates before it writes**, and that is the whole reason it is
  a script rather than a `curl` line in a README: a download that returns
  an HTML error page is still a 200, and 341 bytes of `<?xml` named
  `doom1.wad` fails much later and much more confusingly than it should.
  It checks the `IWAD` magic and a plausible size, and refuses a PWAD
  with its own message (a patch is not a game).

- **`keyup_test.py`** -- key RELEASES reaching a ring-3 client, across
  all five layers that carry one: the driver's transition queue, the
  kernel's raw-event push, the compositor's raw-input queue, the WM's
  routing and Toykit's `on_key_up`. 10 checks, also via `winclient`,
  which keeps a model of what is currently HELD -- nothing else in the
  tree does, because every real app acts on the press.

  **Its load-bearing check is the one that holds a key down.**
  `QMPSession.send_key()` wraps QMP's `send-key`, which presses and
  releases in one go, so a test built on it cannot tell a working
  release path from a guest that invented the release itself.
  `key_down()`/`key_up()` (added to `qmp_test.py` for this) carry the
  edges separately, so "the client still believes W is down a second
  later, and reports it up only when told" becomes assertable. It also
  checks that a MODIFIER reaches a client at all -- Ctrl produces no
  character, so before this it reached one by no path whatsoever -- and
  that a release carries what the PRESS produced rather than what the
  key would produce now (press W, press Shift, release W must report
  `'w'`).

  **One check passes vacuously under the obvious positive control**, and
  the docstring says so: 2b asserts that no release arrives while a key
  is down, which a build delivering no releases at all satisfies
  perfectly. It is worth having -- it catches a release synthesised from
  a timeout -- but only paired with 2c.
- **`damage_sweep.py`** -- drives a fixed sequence of window
  interactions (open, raise, drag, minimize/restore, resize by the grip,
  overlays, close) with `gui damage verify on`, and reports every
  distinct damage violation with the interaction that produced it.
  `--random N --seed S` walks the same interactions in orders nobody
  thought to list.

  **IT ALSO DRIVES THE TRAY'S VOLUME PANEL, AND THAT PART NEEDS A GUEST
  WITH SOUND HARDWARE.** That panel is the one overlay whose geometry
  changes with NO input: its rows come from the registered sound devices
  and from the per-application roster, so it grows and shrinks under an
  open panel and has to cover what the larger one drew. On a guest with
  no sound card no application row ever appears and the steps test
  nothing -- so it prints `NOT COVERED:` for them instead of passing
  quietly, and says the flags to fix it (`vm.py --audio-wav <path>
  --audio both start`). Added 2026-09-22 after the maintainer
  photographed a sliver left behind by exactly this on bare metal;
  nothing in the sweep had touched the tray before. **That sliver does NOT reproduce
  under QEMU at all** -- it needed a tray icon coming and going, which
  the laptop has and a guest does not -- and it was found and FIXED on
  the hardware instead (`docs/decisions.md`, "An overlay's damage is
  checked where it MOVES"). The coverage stays: a panel resizing under
  an open popup is worth exercising whether or not it is broken today.

  **THE VERIFIER IS MORE SENSITIVE THAN A PERSON WATCHING, AND ON BARE
  METAL IT CATCHES THIS ONE.** Driving it by hand on the laptop --
  `guictl damage verify on`, open the tray volume panel, leave it --
  reports 3 violations in 20 seconds of IDLE, where the maintainer
  watching the same screen saw nothing that time. So a quiet screen is
  not evidence and the verifier is: reach for `guictl damage verify on`
  plus `dmesg` before concluding a damage fault is gone. It is not free
  -- every frame renders twice while it is on -- so turn it off again.

  **IT REPORTS THE FIRST DIFFERING PIXEL'S COLOURS, and that line says
  what KIND of fault it is.** `wm:   first px 0x2c7385 -> 0x2d7487`: a
  wallpaper colour turning into chrome means a pass did not DRAW
  something; a shift of a point or two per channel means the same
  content was BLENDED a different number of times, which is a
  completely different hunt. It also prints the cursor position and the
  owning window on the two lines ABOVE the report, so grepping for
  `DAMAGE BUG` alone throws most of the evidence away -- grep
  `"first px|cursor now|diff is in|DAMAGE BUG"`.

  **TURN SHADOWS OFF BEFORE READING THOSE COLOURS** (`config set
  shadows off`). A shadow reaches further than what casts it, so its
  faint outer pixel is the FIRST in scan order and the reported colours
  are a one-or-two-point blend difference -- which reads as "composited
  twice" when the actual fault is a whole panel missing. That misread
  cost a round on the tray-panel bug. With shadows off the same fault
  reported a border colour against wallpaper, which is a different
  investigation entirely. Turn them back on afterwards.

  **AND IT LOGS TWO THINGS THAT ONLY APPEAR WHILE IT IS ON**: an
  overlay opening or closing (`wm: overlay <name> opened`), because an
  overlay that transitions BETWEEN the two render passes makes them
  disagree about a whole panel for an innocent reason; and a tray
  popup's anchor lookup falling back (`tray_item_rect(N) FAILED`),
  because that silently moves the panel somewhere else. Both are gated
  on verification so a tooltip's ordinary hover traffic does not drown
  the log.

  **Read the two counts in its summary.** A *violation* is a real missed
  damage declaration and fails the run; a *report the WM declared void*
  is one the compositor itself could not conclude anything from, printed
  so it stays visible (`-v`) but not counted. Before that distinction
  existed the tool reported 22 violations on a desktop with no damage bug
  in it at all -- every one of them the verifier comparing two renders of
  a CLIENT window whose content had moved underneath it, which a
  compositor with no buffer-release protocol cannot prevent. See
  `docs/decisions.md`.

  **Its positive control is an injected defect, not a flag** --
  `--positive-control` only inverts the exit code. Remove a
  `wm_damage_rect()` call in something the WM draws itself (the taskbar
  strip in `wm_client.c`'s `on_window_created()` is a good one) and
  rebuild; three checks should redden, all at the taskbar's y. Injecting
  it in CLIENT content proves nothing, since that is masked out by
  design.

- **`flake_hunt.py`** -- one GUI tool run N times, reporting which
  CHECKS failed and how often (`python3 tools/flake_hunt.py menubar -n 6
  --keep /tmp/flake`). The sibling of `damage_hunt.py`: that one varies
  a SEED, this one varies nothing and asks whether a tool is
  intermittent. Reach for it the moment a tool fails once and passes on
  re-run -- a rate is the diagnosis, a verdict is not, and this repo has
  a recorded case of a real bug coming back clean six times before
  reproducing five times running. Scores a run that never printed a
  summary as `error`, not `pass`: a run that measured nothing must not
  look like a good one. **IT DOES NOT RESET `disk.img` BETWEEN
  RUNS, so any rate involving the filesystem is contaminated** -- a
  KTEST run leaves state behind, so runs 2..N inherit run 1's and the
  rate climbs for reasons that have nothing to do with the code.
  Measured: `ktest` reported fs checks failing 2-of-4 that way, and
  passed 3 of 3 when each run got its own `make clean-disk && make iso`.
  Until it does that itself, believe only its FIRST run for anything
  touching storage. Also the way to check a fix -- and to catch a
  fix that starts a DIFFERENT check failing, which is what happened when
  the menubar flake was fixed. **`flake_hunt.py ktest -n N` drives the
  IN-KERNEL suite** the same way, reporting each failure as
  `<suite>/<test>`: the 285 ktests run inside the live kernel, so a
  handful are sensitive to what else the machine is doing, and telling
  that from a regression needs a rate per ASSERTION rather than a
  verdict. One caveat unique to ktest -- a run leaves state on
  `disk.img`, so a test that leaks blocks fails the NEXT run's `fsck`
  checks; a rate that CLIMBS run over run is a dirty fixture, and
  `make clean-disk && make iso` between batches is the control. For one
  suite, booting once and repeating `vm.py exec "ktest <suite>"` is much
  faster, and is how the `heap-debug` flake's 1-in-15 rate was measured.
- **`damage_hunt.py`** -- `damage_sweep.py` over MANY seeds, a fresh
  disk copy and its own `vm.py --instance` slot each, as one pass/fail
  table; non-zero if any seed violated the invariant. One seed is one
  ORDERING, and this bug family lives in orderings, so "does any of a
  batch fail" is the question worth asking -- and the four-line shell
  loop that answers it had been written from scratch twice, getting the
  slot/`--sock`/`--qmp-port` triple wrong each time. **A seed reports
  `pass`, `fail` or `error`, and the third one is load-bearing** -- a
  sweep that crashed (a guest too slow to accept a QMP connection at
  high `-j`, a serial socket dropping mid-run) measured NOTHING, and
  this tool used to score exactly that as a PASS. `-j` still defaults
  to 1, but for a plainer reason than before: each slot boots its own
  guest, and four booting at once is enough to lose two of them. Not in
  `gui_regress.py`, same reason `damage_sweep.py` isn't. The earlier
  "parallel VMs report a violation `-j 1` doesn't" claim is retired --
  it failed to reproduce six times, and the mechanism that made `-j 2`
  special was this tool putting 9 GB of tmpfs behind each slot (see the
  next bullet).
- **A copy of `disk.img` must stay SPARSE, and `shutil.copyfile` does
  not.** The image is ~4 MB of data in a 9 GB sparse file, so a
  hole-filling copy costs 9 GB -- of RAM, when the destination is
  `/tmp` on a tmpfs. That silently turned `damage_hunt.py -j N` into
  "N x 9 GB of host memory pressure" and killed `-j 4` outright with
  ENOSPC. Use `cp --reflink=auto --sparse=always` (what the tool does
  now); `cp --reflink=auto` is already what this file recommends
  elsewhere for the same file.
- **`watch_vm.sh`** -- attach a VIEW-ONLY VNC viewer to a headless VM,
  so a run can be watched live without interfering with it.
  `tools/watch_vm.sh [slot...]`; the display derives from the VM slot
  exactly as `vm.py --instance N` does (slot N is `:5+N`, TCP
  `5905+N`). View-only is the point, not a preference -- a connected
  viewer's real mouse motion goes into the same emulated PS/2 device
  the synthetic input uses, and the two fighting looks exactly like a
  flaky test. Remmina's quick-connect URI (`remmina -c
  vnc://localhost:5905`) has NO view-only option, so this writes a
  saved profile with `viewonly=1` and launches that instead, which is
  the whole reason it's a script rather than a line in this file.
- **`screenshot_diff.py`** -- Pillow-based pixel diff between two
  screenshots with a pass/fail `--threshold` (default 0.2%) and an
  optional `--out` diff-highlight image, for catching a rendering
  regression manual eyeballing might miss.
- **`corrupt_diff.py`** -- characterises HOW two copies of a file
  differ, which is what a checksum cannot say. Prints every differing
  run, whether it is block-ALIGNED (512/4096/8192), and what the bad
  bytes ARE: zeros (a lost block), a run copied from elsewhere in the
  same file (a block served from the wrong place -- it reports the
  source offset and the delta), or neither. Written for the silent
  download corruption: a checksum said "wrong", and this said "1458
  bytes, in ONE 1460-byte window, holding the stream's own data from
  832 bytes earlier" -- which named the layer in one run after a
  session of counting checksums. **Extract the file with
  `tfs3_writer.py read` rather than serving it out of the guest**: the
  first attempt came through the guest's own `httpd`, which truncated
  and returned two different lengths, so the analysis would have
  measured the extraction. The check is that the extracted file's
  digest matches what the guest itself reported.
- **`tfs2_writer.py`** -- **REMOVED** along with the TFS2 backend, and
  nothing guards against a TFS2 disk any more: one booted today is read
  as blank and REFORMATTED. To read one, check out a commit before the
  removal. `docs/tfs2-spec.md` is kept because several
  `docs/decisions/` entries reason from it.
- **`tfs3_writer.py`** -- the host-side TFS3 tool: format
  (writes superblock backups + GDT snapshots, wipes a stale TFS2
  signature per the wipefs rule, keeps images sparse by skipping/
  hole-punching the zeroed inode tables) / ls / read / write / mkdir /
  delete / sync (`once/` + `sync/` convention) / trim / info /
  corrupt (`--leak`, `--free-referenced`, `--bad-link-count`,
  `--smash-superblock`, `--stage-journal[-torn]` -- known damage for
  fsck/backup/journal-replay testing, same reasoning as
  tfs2_writer's). Spec: `docs/tfs3-spec.md`; the kernel backend
  (`kernel/fs/tfs3.c`) is kept in lockstep.

  **IT WRITES THE WHOLE BLOCK MAP** -- direct, single, double and triple
  indirect. It stopped at single-indirect (~4.05 MB) until 2026-08-31,
  described in its own docstring as "a deliberate cap", which it was
  right up until `/install/kernel.bin` (~4.7 MB) meant a live image could
  not carry the payload it installs from. Adding one level would have
  moved the cliff to 4 GiB rather than removing it, so all three went in,
  behind one recursive walker/builder pair. **The triple level is
  UNEXERCISED** -- it starts past 4 GiB -- and says so where it is
  defined. The general shape is worth keeping: a second implementation of
  a format is only as complete as the biggest thing anyone has fed it.
  **`format --fs-version {1,2}`** picks the on-disk layout: v2 (32
  journal slots, GDT at 42, group 0 at 58) is the default and what a
  fresh image gets; v1 (four slots, GDT at 14, group 0 at 30) exists so
  the layout the kernel still mounts stays PRODUCIBLE and therefore
  testable -- `tools/tfs3_v1_test.py` is its caller.

  **`trim` CORRUPTED EVERY LIVE IMAGE FOR AS LONG AS disk.img HAS BEEN
  PARTITIONED, and two bugs were stacked so that neither showed.** It
  punches holes through free blocks, and it does so with `fallocate()`
  on the raw fd -- so unlike every other read and write in that file it
  does NOT go through `Tfs3Image`, which is what adds `base_lba * 512`.
  It punched at a VOLUME-relative offset, which on a partitioned image
  is one partition-start early: 1 MiB, 256 blocks, landing on blocks
  that were in use.

  The second bug hid the first. It walked each group's bitmap to
  `BLOCKS_PER_GROUP`, but **TFS3's last group may be PARTIAL** and the
  bits past the volume's end read as free -- so it asked to punch ~71 MB
  beyond the volume, which the offset error pulled back INSIDE the file.
  A trim-past-the-end became a corrupt-the-beginning, and the two
  errors together produced a plausible-looking `punched N blocks` line.

  What it destroyed: `/usr/wm/applications`, `/etc/services.d` and
  `/usr/wm/startup`. So the live CD booted with a full `/bin`, a
  working shell, and a **desktop with no apps** -- the Start menu is
  built from those `.desktop` files. Reported from real hardware.
  `disk.img` was never affected because **`trim` runs only on the live
  image** (one line in the Makefile), which is why a disk boot looked
  perfect throughout.

  Nothing here caught it: `live_boot_test.py`'s "a directory the image
  shipped is readable" reads `/bin/wm/apps`, which sat far enough from a
  free run to survive. It names the three that did not now. The fix adds
  a bounds guard, because **a punch is destructive and silent** -- it
  cannot fail loudly the way a bad write can, and that guard is what
  surfaced the second bug.
  **TFS3 is the default format for FRESH images** (blank-disk policy
  in `vfs.c` and `seed_disk.py`); an existing TFS2 disk.img keeps
  mounting as TFS2 -- `make clean-disk && make iso` is the deliberate
  move. Use whichever writer matches the image's magic (both refuse
  the other's images; `trim` before gzipping a release image means
  the MATCHING tool's trim).
- **`seed_disk.py`** -- the format-aware seeding front-end the
  Makefile's `seed` target calls: probes the image's magic, delegates
  `sync` to the matching writer, and formats a blank image with the
  default (tfs3) -- the same policy the kernel's blank-disk path
  applies at boot.

  **A BLANK IMAGE IS PARTITIONED AND BOOTABLE NOW** -- a GPT with a 1 MiB
  BIOS boot partition, a 64 MiB FAT32 `/boot` and TFS3 in the rest, which
  is what `make iso` produces for a fresh `disk.img`. `--partition mbr`
  writes the legacy table, which needs no BIOS boot partition
  (`core.img` goes in the pre-partition gap) and so gets two. **There is
  no `--flat` any more**: the kernel refuses a whole-disk volume
  (`docs/rootfs-design.md`), so this tool has no way to write one, and
  even the live ISO's RAM image carries a table. An image that already
  IS flat is still seeded -- a host tool may legitimately want files in
  one -- with a loud warning that the kernel will not mount it. `--layout`
  sizes and TYPES the partitions (`1M:bios,64M:esp,rest`). It prints
  `filesystem in partition N -- LBA ..., ... sectors`, which is what
  `partition_test.py` reads rather than assuming partition 1.
  `install_grub.py` fills the other two.

  **An EXISTING image keeps its shape.** It asks `mkpart_test.py`'s
  `volume_of()` and seeds into whatever it finds, so a checkout does not
  change layout under anybody -- `make clean-disk && make iso` is the
  opt-in, the same one that moved TFS2 to TFS3. A partitioned image whose
  partition 1 is empty (a `mkpart`ed disk nobody formatted) gets formatted
  IN the partition rather than treated as blank, which is the same
  refusal the kernel makes at boot.
- **`install_grub.py`** -- puts GRUB **and the kernel** onto `disk.img`,
  which is what makes an ordinary `make run` a DISK boot rather than a CD
  boot. Three writes: `boot.img` at LBA 0 (patched with `core.img`'s LBA,
  and keeping the existing partition table), `core.img` embedded in the
  BIOS boot partition -- or, on an MBR, in the gap before the first
  partition -- with its own block list patched, the part
  `grub-bios-setup` normally does, and `/boot/kernel.bin` +
  `/boot/grub` written into the FAT32 partition with `mtools` -- no root,
  no loop device, no mount.

  **Why `/boot` is FAT32 at all:** GRUB cannot read TFS3 and never will
  without a module written into its own GPLv3 source tree. Every real
  system with this problem answers it the same way (Linux's separate
  `/boot`, UEFI's FAT32 ESP, Windows' System Reserved) -- see
  `docs/decisions.md`.

  **`--stage-payload <dir>`** writes the four files `/bin/install` needs
  -- `kernel.bin`, `grub.cfg`, `boot.img`, `core.img` -- into a directory
  instead of onto a disk. The build points it at `seed/sync/install`, so
  every toy-os filesystem carries `/install` and can install itself. The
  `core.img` it makes has `(hd0,gpt2)/boot/grub` baked in, which is what
  makes the ESP-is-partition-2 layout a constraint rather than a
  preference. A checkout without GRUB's BIOS target stages nothing and
  says so; that build boots, it just cannot install itself.

  **`--check`** answers "is this image bootable" for a script;
  **`boot_medium(disk)`** is the same question as a function and is what
  the Makefile, `vm.py`, `qmp_test.py`, `boot_smoke_test.py` and
  `serial_console.py` all ask before choosing `-boot order=c` or the CD.
  **`--optional`** is what the build passes: an image with no boot
  partition is reported and skipped rather than being an error, so a
  `disk.img` predating this layout keeps working (booting the ISO).
  It never partitions and never reformats an existing `/boot`.
- **`tfs3_writer_test.py`** -- does the host seeder's OVERWRITE hand
  every block back? Host-only, no VM, about a second: it formats a
  scratch image, writes one file per block-map level (direct,
  single-indirect, double-indirect), rewrites each with different
  content of the same size, and asserts the allocation bitmap did not
  grow. **The sizes ARE the test.** A free path can be right for one
  level and wrong for the next, and the seed tree has exactly one file
  over the single-indirect ceiling (`/install/kernel.bin`, ~4.8 MB) --
  so a leak of its two double-indirect tables sat in the seeding path
  with every existing test green, reddening `ktest`'s `fsck` checks on
  any second gate run against one image. Triple-indirect is deliberately
  NOT covered: it starts past 4 GiB. To watch it go red, restore
  `delete_path()`'s old `img.free_block(node["ptrs"][12])` in place of
  the pointer walk -- the double-indirect case fails by exactly 2 and
  the other two stay green. Run by `preflight.sh`, because a leak here
  corrupts the gate's own fixture.

- **`tfs3_v1_test.py`** -- **repaired 2026-08-25, it had been 0 of 8**:
  it drove `cat`, `mv` and `dmesg` against an image with no `/bin` on
  it, months after those became `/bin` programs, and its image was flat
  once the kernel stopped mounting those. It uses `rescue cat` and
  friends now and builds a partitioned image. Boots a freshly built TFS3 **v1** image and
  proves the kernel still mounts and uses the older on-disk layout (8
  checks). Run it after touching TFS3's geometry, journal, or any
  operation's credit count. It exists because v2 made v1 support
  simultaneously untested AND untestable -- once `format` wrote v2,
  nothing in the repo could produce a v1 image at all, so
  `tfs3_writer.py format --fs-version 1` was added alongside it. Same
  rule as `ata nodma` keeping the PIO path reachable: a fallback
  nothing can reach is a guess. Its load-bearing check is that a
  cross-parent DIRECTORY move is refused there (v1's four journal slots
  cannot hold the five-block transaction) and that the refusal changed
  NOTHING -- the only end-to-end view of the credit reservation.
- **`fs_switch_test.py`** -- boots a COPY of disk.img and proves the
  multi-backend story end-to-end: probe mounts the image's own
  format, `fsformat` live-switches both ways (wipefs rule included),
  writes work on each side, files survive reboots, fsck ends clean.
  Run it after touching anything in `kernel/fs/`; it exercises the
  probe/format/remount/reboot cycle no KTEST can (the suite runs
  inside one booted kernel).
- **`install_test.py`** -- does toy-os install itself onto another disk,
  and does that disk then BOOT? Two phases, and the second is the one
  that matters: it boots the installed image with **nothing else
  attached** and asserts the machine that comes up is that one -- by its
  root partition's sector count, which is the source's size otherwise.
  Without that, a guest with the ISO still in the drive boots the ISO's
  kernel and mounts the target's root, which reads exactly like a
  successful install.

  **The disk arrangement is the point, and it is inverted from the way it
  reads: the SYSTEM goes on virtio and the blank TARGET on IDE.** Disk
  precedence is virtio-blk, then AHCI, then ATA, so a blank virtio disk
  beside an IDE root outranks it, the root scan finds no filesystem, and
  the guest comes up on ramfs with no `/bin`. Inverting it is what lets
  this tool run against a stock `make iso` with no `KCMDLINE` (compare
  `hires_test.py`, which does need one).

  **It runs BOTH media** (`--media disk|live|both`, default both), because
  they are different code paths and the live one is how a real machine
  gets toy-os: `toy-os-live.iso`'s root is a RAM image with no `/boot` at
  all, which is why the install payload lives in `/install`. The live
  half needs `make live-iso` and is skipped with a message when the ISO
  is absent.

  `--positive-control` zeroes each installed boot sector before its boot
  phase, so those checks must go RED; a clean run proves nothing until
  that has been seen. On demand only (`ondemand_sweep.py` names it) -- it
  boots four guests and takes a few minutes.
- **`mkpart_test.py`** -- writes a synthetic legacy MBR or GPT partition
  table onto a disk image, for testing `kernel/drivers/partition.c`'s
  parser (`parttable` shell command). Its mount-preserving guarantee
  was designed for (and verified against) TFS2 images; a TFS3 image
  deliberately leaves its first 32 KiB untouched for exactly this, so
  coexistence is by-design there, but the tool hasn't been re-verified
  against one -- check before trusting it on TFS3. TFS2-mount-preserving: patches
  only the partition-table byte ranges TFS2 itself never touches
  (reads the existing LBA 0 sector first rather than blindly
  overwriting it), so the real filesystem underneath still mounts
  normally afterward instead of `tfs_init()` seeing foreign magic and
  auto-reformatting. `--mbr`/`--gpt`; see its own docstring for the
  CRC32/GUID-encoding details and `docs/decisions.md` for why GPT
  verification needed a host-compiled unit test instead of a live
  boot (TFS2's own journal header collides with the GPT header's LBA).

  **`--layout SIZE[:KIND][,...]` writes a REAL, usable table instead** --
  partitions laid end to end from LBA 2048 (1 MiB alignment), sized by
  a `K`/`M`/`G` suffix or bare sectors, with exactly one allowed to be
  `rest`. The optional `:KIND` is the partition TYPE -- `data` (the
  default), `bios` (a BIOS boot partition, where GRUB's `core.img` is
  embedded) or `esp` (an EFI System Partition, the FAT32 `/boot`). The
  kernel reads those types too, and refuses to mount or format either
  of the last two. `--gpt --layout` also writes the BACKUP header and entry
  array in the last 33 sectors, which the synthetic mode does not: a
  partition editor on another system reads the backup to cross-check
  the primary and "repairs" a disk that has none. This is what
  `seed_disk.py --partition` calls, and its per-partition GUIDs are
  derived from the index rather than random, so `make iso` stays
  reproducible (the KERNEL randomises them -- a disk written on a
  running machine has no such requirement).

  **`--print-volume` prints `<base_lba> <sectors>`** for the image's
  filesystem volume -- the whole image when there is no table. It is the
  shell-callable form of `volume_of()`, which is the one place the host
  answers "where does the volume start"; every tool that reaches into a
  disk image asks it rather than hardcoding 2048. **It is not "partition
  1" any more**: a bootable image's partition 1 holds GRUB, so
  `volume_of()` finds the partition carrying a TFS3 superblock and falls
  back to the first one that is not the firmware's.

  Note the GPT-verification caveat above is now HISTORY rather than a
  live limitation: TFS3 is the default and reserves volume blocks 0-7,
  so `kernel/drivers/partition_test.c` round-trips a real GPT through
  the writer and the parser inside a running kernel.

- **`check_licenses.py`** -- every directory under `userland/ports/` and
  every font in `data/fonts/` must carry a license file **and** be named
  in `LICENSE`.

  **The inventory had drifted twice before this existed.**
  `userland/ports/doom/` is GPL-2-or-later source vendored into an MIT
  repository and `LICENSE` did not mention it at all; the font section
  described "two complete third-party font files" when there were five,
  with `vera-mono.ttf` and both bold faces unlisted and Vera a third
  license family. Neither was a violation on its own — every per-file
  notice was present, which is what the licenses actually require —
  but a reader of `LICENSE` could not learn that GPL code was in the
  tree.

  **What it cannot check, and says so in the file:** whether the license
  NAMED is the license the code is really under. Nothing static can read
  a directory and know it is GPL-2-or-later rather than GPL-3. That is a
  human reading the vendored license file, which is why the entries in
  `LICENSE` quote the version language rather than paraphrasing it.

- **`highmem_test.py`** -- the WHOLE KTEST suite on an 8 GiB guest
  (`ktest_run.py --mem 8192`), failing if an above-4-GiB check skipped
  rather than ran, which is what those do on every other runner's
  256 MiB boot. The whole suite rather than `mm` because a machine that
  size moves the 64-bit PCI window to 768 GiB, which is what broke every
  virtio test the first time it ran. Proves the high frames are managed,
  mapped and zoned, and that the kernel heap, user pages, page tables
  and window buffers come from them. Then a second phase asks the same
  question from OUTSIDE the kernel: one `/tests/memtest` is spawned on
  its own guest and the high zone's free count must drop by more than
  128 MiB. That half is what would survive a KTEST fixture drifting away
  from the path a process really takes. Named by `ondemand_sweep.py`.
- **`highmem_consume.py`** -- the same question at SCALE, and the proof
  stage of "More than 4 GiB of RAM". Six `/tests/memtest` copies at 1 GiB
  each on an 8 GiB guest, with the high zone's free count POLLED for its
  minimum -- a copy frees everything as it exits, so a reading taken
  afterwards reports an empty machine. Measured: 5.00 GiB of high frames
  held at once, which the ~2.9 GiB low zone could not have supplied.
  Every copy verifies its own address-derived pattern, so two of them
  sharing a frame is caught rather than assumed, and `meminfo --audit`
  must be clean afterwards.

  **Its positive control is to put one consumer back on `DMA32`.**
  Reverting `proc_syscalls.c`'s heap fault-in took the peak from 5.00 GiB
  to 0.01 GiB and killed every copy, which is the shape to re-run before
  trusting a clean pass here.

  **It counts memtest results as a DELTA**, because the klog ring keeps
  whatever an earlier run left in it and a stale `PASSED` satisfies the
  exit condition while copies are still holding memory -- which then
  reads as a leak. That is `mem_stress.py`'s "replace, never append"
  rule arriving from a different direction. Not a gate: it boots its own
  8 GiB guest and takes about a minute. Named by `ondemand_sweep.py`.
- **`ondemand_sweep.py`** -- runs the ~30 test tools that **neither**
  `preflight.sh` nor `gui_regress.py` covers, and reports which have
  rotted.

  **The gap it fills is measured, not theoretical.** Two of those tools
  were found red by accident in one session, having failed for an
  unknown period: `fs_switch_test.py` (10 checks, because `df` and
  `stat` became `/bin` programs while it still parsed the ring-0
  output) and `init_test.py` (16 of 31, same class). Neither was a
  regression; both were rot, and nothing in the repo could notice.

  **A SKIP IS COUNTED SEPARATELY FROM A PASS.** `doom_test` exits 0
  with no IWAD and `hires_test` passes vacuously on a default-mode ISO
  -- counting either as a pass reports coverage that does not exist.
  Preconditions are checked before running, so those report as skips
  with a reason. The `hires` precondition reads the **built**
  `iso/boot/grub/grub.cfg`'s `multiboot2` line, not the repo-root
  template: the template still holds the `@KCMDLINE@` placeholder and
  documents `video=` in four comments, so a substring search over it is
  true on every checkout.

  **Serial by default (`-j1`)**, and that is not a conservative
  guess: nearly all of these take the shared VM slot, the physical
  console, or boot their own machine. This is not `gui_regress`.

  **A TOOL THAT CHANGES `disk.img` RUNS LAST** (`DIRTIES_IMAGE`).
  `console_bleed_test.py` deletes `/etc/services.d/toywm` and cannot put
  it back -- there is nothing in the guest to copy it from -- so every
  tool after it would boot a machine it did not configure, which is this
  file's own "a green line that means nothing" hazard pointed at itself.
  The sweep says at the end that `make iso` is needed rather than
  re-seeding the image on somebody's behalf.

  NEVER A GATE. Several need hardware, Docker or a fetched IWAD, and a
  check that cannot pass on a clean checkout is one people learn to
  ignore. Run it before a release, or when you want to know whether the
  on-demand half of this directory still works. `mkpart_test.py` is
  excluded because despite the name it is a WRITER that takes a
  disk-image argument, so running it bare is an argparse error rather
  than a result.

  **Five tools were missing from it for an unknown period** and were
  found the same way its own first run found two red ones -- by
  enumerating `tools/*_test.py` and subtracting what each runner names:
  `net_test.py`, `audio_test.py`, `cursor_ibeam_test.py`,
  `serial_backpressure_test.py` and `console_bleed_test.py`. Adding a
  tool to `tools/` does not add it here, and nothing checks that it
  did.

- **`fileop_test.py`** -- `lib/ufileop` through `/bin/cp`, `/bin/mv` and
  `/bin/rm`. The copy loop and the tree walk are ONE implementation
  shared by those three and by the File Manager, and this is what keeps
  "testable as text at a prompt" true after the GUI stopped spawning
  children. Asserts on the listing read back with `ls` -- a different
  reader from the program that did the work. Its positive control is the
  deepest-first unlink ordering: reverse it and `rm -r` leaves every
  directory behind, which is the one check that reddens.

- **`predates.py`** -- answers "did this failure exist before my
  changes?" by measuring rather than guessing: stashes the tree,
  rebuilds at HEAD, runs the command you name, restores, and prints
  both results.

      python3 tools/predates.py "python3 tools/init_test.py"

  CLAUDE.md's rule is that *"it predates me" is a measurement*, and the
  procedure for taking it was written down in prose with a footgun in
  it. This is that procedure as a script. It stashes with `-u` (without
  it your new files stay in the tree and the "HEAD" build is not HEAD
  -- it silently measures HEAD-plus-your-new-files), recovers **by
  SHA** rather than by index or a bare `pop`, restores in a `finally`
  so a Ctrl-C still puts the tree back, drops the stash only after a
  **clean** apply, and refuses to run mid-rebase or mid-merge.

  It answers "was this already red", and says so: pre-existing is not
  the same as unrelated, since a change can make a latent bug reachable
  without causing it.

  **TWO TRAPS IT NOW HANDLES ITSELF, both of which produced a wrong
  answer here before it did.** The `-u` that makes the HEAD build honest
  also takes UNTRACKED files away -- so a tool written this session does
  not exist during the HEAD run, and the failure reads as the tool being
  broken rather than absent (an argparse "unrecognized arguments" is
  what it actually looks like). It names such files up front now, and
  says the comparison is not a measurement.

  And **restoring the SOURCE does not restore the ARTIFACTS**: `build/`
  and any boot image still hold what HEAD produced, so whatever is run
  next silently tests the other kernel while every file on disk says
  otherwise. That reported a working fix as broken here, twice. With
  `--build` it rebuilds the working tree after restoring, so the tree is
  left as it was found -- including what was built from it.

- **`check_tool_commands.py`** -- static check that every guest command
  a tool drives still exists, against the same authority `check_docs.py`
  uses (the seeded `/bin` tree plus both shells' builtins).

  **It found a real one on its first run**: `kvm_soak.py` drove
  `delete`, which is `rm` now -- so its "force a reload by deleting"
  half had been a no-op and it had been leaving `zz*.desktop` litter on
  `disk.img`, which is the dirty-fixture class that makes *other* tools
  fail.

  **Its limits are stated in the file, because an oversold check is
  worse than none.** It cannot see a command whose OUTPUT changed,
  which is the rot that actually bit `fs_switch_test` -- the name `df`
  stayed valid the whole time. `ondemand_sweep.py` catches that by
  running things; this is the cheap subset that costs a second.

- **`check_tool_coverage.py`** -- static check that every
  `tools/*_test.py` is named by a runner: `preflight.sh`,
  `gui_regress.py`, or `ondemand_sweep.py`'s `TOOLS` table.

  **The gap is measured, not theoretical.** `ondemand_sweep.py` exists
  because tools rot when nothing runs them -- and then FIVE were found
  outside the sweep itself (`net_test.py`, `audio_test.py`,
  `cursor_ibeam_test.py`, `serial_backpressure_test.py`,
  `console_bleed_test.py`), one of them failing a check that measured
  PRE-EXISTING against the commit before it was found. Adding a tool to
  `tools/` adds it to nothing that runs it, and nothing noticed. Same
  shape as `check_dispatch.py` and `check_widget_ops.py`: the repo had
  the right pattern and no check that it was being followed.

  **It matches by FILENAME only**, deliberately. Counting a bare stem
  as well would let any quoted word in any runner stand for coverage --
  a precondition wrong in the permissive direction, which manufactures
  coverage that does not exist and makes silence meaningless. That is
  the same mistake `ondemand_sweep.py`'s own `hires` precondition
  documents.

  **Its limits, stated because an oversold check is worse than none:**
  it does not check that the runner can actually RUN the tool (the
  sweep's `wants_vm` column is a real thing to get wrong, and getting
  it wrong makes a tool fail on a missing socket rather than on its own
  subject -- three of the five needed it), nor that the tool passes.
  Green means "nothing is orphaned". Waive with a reason in `EXEMPT`;
  two are (`mkpart_test.py`, and `qmp_test.py`, a library with an
  unfortunate name).

- **`regex_hostcheck.py`** -- compiles `userland/tests/regex_cases.h`
  twice, once against the real `userland/libc/regex.c` and once against
  the host's glibc `<regex.h>`, and compares.

  **It catches what a self-test structurally cannot: an expected value
  that is simply wrong.** `/tests/regex_test` asserts that the engine
  agrees with spans written by the same person at the same time; glibc
  shares no code, no author and no assumptions. On the first run it
  confirmed 71 of 74 cases and isolated exactly three deliberate
  divergences (`\t` as a tab, a stricter unmatched `)`, no
  back-references), each of which is listed with its reason in
  `KNOWN_DIVERGENCES` -- an unexplained one fails the run. Same idea as
  `uimg_hostcheck.py` against libjpeg.

  ON DEMAND: it needs a host gcc and glibc, and the gate must not start
  requiring either.

- **`remote_test.py`** gained TFTP OPTION coverage (2026-09-01): both
  directions at a negotiated 1428-byte block, and the fallback when a
  client asks for nothing.

  **The fallback is the half that breaks silently.** A transfer that
  quietly drops to 512-byte lockstep still succeeds and only looks slow,
  so a round-trip check cannot see it -- which is why one check reads
  the server's own log for `blksize 1428, window 3` rather than
  inferring agreement from the bytes arriving.

- **`check_config_size.py`** -- no shipped `data/etc/**` or
  `data/usr/share/services/**` file may exceed `ETC_CONFIG_BUF_MAX`.
  Run by `preflight.sh`.

  **The failure it prevents names the wrong thing.** A config file over
  the buffer is read SHORT, so the keys past the cut are not seen -- and
  what init then reports is `dhcp has no Exec=, will not start it`,
  which reads as a broken service when the truth is a long comment.
  Comments count toward the budget; that is the whole trap, because the
  file that fails looks nothing like too much configuration.

  It reads the limit FROM the header rather than repeating it, since a
  limit copied into a checker drifts from the code it checks. It
  deliberately does not cover files written at RUNTIME (`resolv.conf`,
  the settings files) -- those grow as the system runs, and the rewrite
  path refuses rather than truncating, so they fail safely but only in
  the moment.

  **It also checks the per-field text caps**, which is a different
  failure from the file being too big: a `Description=`/`Label=` in
  `data/etc/settings.d`, or a `Label.`/`Desc.`/`Unit.` in a
  `data/wm/savers/*.saver`, longer than the fixed array it is copied
  into arrives TRUNCATED MID-WORD with the file itself far under every
  size limit here. The saver caps come from `userland/lib/usaver.h` and
  the settings ones from the ABI, each read from its own header.

  **Everything here walks `git ls-files`**, so a file that has not been
  `git add`ed is invisible to it -- which is how a positive control on
  a brand-new descriptor directory changed nothing at all and looked
  like a checker that did not work.

- **`mouse_buttons_test.py`** -- that all five pointer buttons reach a
  ring-3 client, and that a SHORT press reaches it too. Run by
  `gui_regress.py`, on a guest with a USB mouse (`--usb xhci+mouse`).

  **The check that matters is the TAP**, `QMPSession.tap()`: a press and
  a release with no wait between them. `click()` sleeps 100ms, which
  gives a level-sampling input path every chance to look and pass --
  and the input path here sampled a level at three stages, so a tap
  whose two edges landed between two looks cancelled out and the click
  never happened (docs/decisions.md, "A pointer button is an edge, not a
  level"). Check 2 -- the same button HELD -- is what passes on the
  broken build, and the pair is what makes the difference legible.

  **Two harness traps it paid for.** A down and an up in ONE
  `input-send-event` send NOTHING: QEMU's PS/2 mouse accumulates button
  state and only queues a packet at the batch's sync, so the two net out
  -- caught because the primary-button tap went missing on a build where
  clicking plainly worked. And the two commands are PIPELINED, both
  written before either reply is read, because a round trip between them
  is long enough for the guest to look.

  **ITS POSITIVE CONTROL DOES NOT REDDEN, AND THAT IS MEASURED RATHER
  THAN AN OVERSIGHT.** With the fix reverted this tool passed all ten
  checks on a PS/2 guest and again on a USB one: the PS/2 path
  interrupts per packet and the idle loop that polls wakes on those
  interrupts, and a USB guest is paced by the endpoint's polling
  interval, so under emulation something always looks in between. The
  deterministic control for the lost edge is the KTEST `input`/"a press
  and its release both survive one polling pass"; this tool's job is the
  end-to-end claim -- five buttons by name, and the thumb pair
  navigating the File Manager -- not the race.

- **`hid_parse_hostcheck.py`** -- `kernel/lib/hid_parse.c` against HID
  report descriptors CAPTURED off real devices, on the host. Run by
  `ondemand_sweep.py`; needs gcc and nothing else.

  **The fixtures are captured, not invented**: a Logitech G305
  receiver's mouse and keyboard interfaces (read with
  GET_DESCRIPTOR(0x22) at bind time) and QEMU's usb-mouse and usb-kbd.
  A descriptor written by whoever wrote the parser proves only that the
  two agree with each other.

  **Why the host rather than a guest.** The parser decides where a
  device's buttons and axes sit inside its reports, and its failure is
  not a missing feature but a pointer flying across somebody's screen,
  with a person as the only oracle. The descriptors are small and
  fixed, so the whole question can be asked on the host -- which is
  also the only way to cover a device nobody here can plug in.

  It asserts the derived layout field by field (offsets written out by
  hand from the decoded descriptor), that a keyboard's keycodes are an
  ARRAY rather than a bitmap, a round trip through a synthesised report
  including a NEGATIVE axis delta, and that every malformed input --
  truncated, empty, garbage, an impossible report size -- is REFUSED
  rather than turned into a plausible-looking layout. The refusals
  matter most: a refusal sends the driver back to the boot protocol,
  and a plausible-looking layout is the one that gets used.

- **`hover_test.py`** -- that a hover change REPAINTS rather than only
  recording damage. Run by `gui_regress.py`.

  **It counts frames, not pixels, and that is the whole point.** The
  obvious test -- warp the cursor onto a menu row and photograph it --
  cannot see this bug: the tray clock forces a repaint once a second, so
  a settled screenshot finds the highlight correctly placed, having
  arrived up to a second late. That IS the symptom, and settling
  launders it away. `gui state`'s `scene repaints` counter answers the
  real question.

  It asserts BOTH directions: eight hover changes must repaint, and
  eight moves with no overlay open must not -- without the second half a
  WM that repainted on every move would pass.

  Its moves are INJECTED rather than warped, against CLAUDE.md's usual
  advice, because it tests whether a change caused a FRAME rather than
  whether a state survives a capture -- and an injected move is one
  console round trip against a warp's several hundred milliseconds,
  which is what keeps the tray clock out of the measurement. The first
  version's "moving within one row must not repaint" check was wrong for
  the matching reason: an injected position snaps back to the real
  pointer next iteration, so each move changed the hover twice.

- **`guictl_test.py`** -- `/bin/guictl`, the ring-3 front end for the
  window manager's `gui` diagnostics. ATTACHES to a running `vm.py`
  guest and needs a desktop up.

  **Its load-bearing check asks the same subcommand twice** -- once
  through `guictl` (ring 3, `SYS_DIAG`, polled) and once through
  the serial debug console's own `gui` (ring 0, waits in place) -- and
  requires the two answers to match. A guictl-only check would pass
  against a program that printed a plausible answer of its own; the
  console shares no code with it below the window server.

  The second is that an unknown subcommand is REPORTED, which was dead
  for months: the kernel cleared the incoming flags before reading them,
  so `WIN_DEBUG_F_UNKNOWN` never arrived and `gui nosuchthing` printed
  nothing on the serial console either. A check asserting only that
  known commands answer would not have seen it.

- **`utween_hostcheck.py`** -- compiles `userland/lib/utween.c`, the
  desktop's one easing tween, with the host gcc and checks the
  PROPERTIES a motion must have rather than an oracle (there is no
  reference easing library, and the curve's exact shape is a taste): it
  starts at `from` and lands EXACTLY on `to`, never moves back, covers
  more than half the distance in the first half of the time (ease-out),
  goes inactive exactly at the end, and a retarget mid-flight continues
  from the value at that instant without a jump. Possible on the host
  because the tween takes its clock as an argument. Its first run caught
  a rounding overshoot (100 -> 0 visited -1 for a frame) before any
  guest had drawn with it. `--positive-control` compiles a LINEAR curve
  and must fail on the ease-out check. Needs only gcc.

- **`hash_hostcheck.py`** -- compiles `/lib/libhash.so`'s two algorithms
  (`userland/dynlib/uhash.c` plus `kernel/lib/kcrc.c`) with the host gcc
  and judges them against Python's `hashlib` and `zlib`. ~2,150 vectors:
  every size from 0 to 129, the powers-of-two boundaries, and forty
  random sizes, each fed through in seven different chunk sizes so a
  block boundary lands INSIDE an `update()` call rather than between
  two.

  **The oracle is in the standard library**, so unlike the other
  hostchecks here this needs nothing but gcc -- no Pillow, no ffmpeg.

  `--positive-control` edits a COPY of each source (one wrong rotate in
  the SHA-256 round, one wrong polynomial bit in the CRC) and requires
  the sweep to go red; the originals are never touched, which is what
  keeps the control out of shipped code. Measured: 2,137 of 2,150
  vectors disagree under it.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.

- **`divti3_hostcheck.py`** -- compiles `userland/libc/divti3.c` with
  the host gcc and judges the 128-bit division helpers against Python's
  arbitrary-precision integers. 17,022 vectors: every pairing of
  sixteen edge values (the 32-, 64- and 127-bit boundaries, powers of
  two, all-ones) and then 4,000 random pairs whose two widths are drawn
  INDEPENDENTLY, so `a/b` spans "both small", "both large" and "one of
  each" -- which is what decides whether the 64-bit fast path or the
  shift-subtract loop runs.

  **The oracle is arbitrary precision**, so it is the answer by
  construction rather than by a second implementation agreeing. Needs
  nothing but gcc.

  **One case is excluded and the exclusion is the interesting part.**
  `INT128_MIN / -1` has no representable quotient, so C leaves it
  undefined; Python's mathematically-correct `+2^127` would fail a
  correct implementation. libgcc returns `INT128_MIN` there and so does
  `divti3.c` -- confirmed against the host's libgcc rather than assumed.
  Divide-by-zero is not covered either: it raises #DE by design, which
  a process expecting to keep running cannot probe.

  `--positive-control` breaks the 64-bit fast path in a COPY and
  requires the sweep to go red. It is deliberately the FAST PATH rather
  than the loop, so the control also proves the sweep reaches small
  operands at all -- a sweep of large random values alone would never
  enter that branch. Measured: 2,332 of 17,022 disagree under it.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.

- **`https_test.py`** -- drives `/bin/wget`'s HTTPS path against a TLS
  server on this machine: a threaded Python `http.server` with a
  self-signed certificate, reached through SLIRP's 10.0.2.2. **Nothing
  leaves the machine**, which is what lets it run offline; verifying
  against a PUBLIC certificate is a different question and is not this
  tool's.

  Nine checks, and the ORDER is the design -- each is only meaningful
  because the one before it can fail. A verified fetch returns the body
  over TLS 1.3 with no warning; the same fetch is REFUSED when the store
  holds the wrong anchor; refused DIFFERENTLY when the store is empty
  (a machine that was never told whom to trust is not the same as a
  certificate nobody vouches for); `-k` fetches and says so; the entropy
  gate refuses TSC jitter by name; and plain `http://` still works,
  which is the regression check for routing wget through the library.

  **It reboots between the three trust-store states** rather than
  editing `/etc/ssl/certs` under a running kernel, and it clears the
  store by LISTING it rather than by deleting the names it wrote -- an
  image built with `EXTRAS=1` already holds 121 Mozilla roots, and
  leaving those behind turns the "empty store" check into a verification
  failure, which is a fixture the test did not establish wearing the
  costume of a regression. It drives a COPY of `disk.img`, sparsely
  copied.

  `--positive-control` trusts the WRONG anchor while still expecting the
  verified fetch to succeed; three checks must go red, and a run that
  stays green is a run where the certificate is not being verified at
  all. Measured: 6 of 9 pass under it.

  ON DEMAND: it needs `openssl` to make the certificates and SKIPS
  cleanly without it.

- **`hwdata_test.py`** -- drives `hwdata update` against a plain
  `http.server` on this machine, reached through SLIRP's 10.0.2.2, the
  same arrangement `https_test.py` uses. **Nothing leaves the machine**;
  whether pci-ids.ucw.cz still serves what it used to is not this tool's
  question and could not be answered offline anyway.

  Twelve checks, and the load-bearing one is the SECOND: a truncated
  body is refused and the old database survives byte for byte. That is
  the whole reason the command exists rather than a note saying to run
  `wget -O <path>`, which opens with `O_TRUNC` and leaves a half-file
  that still parses. A large ERROR PAGE is refused separately, because
  the size floor alone would pass it and the vendor-line count is what
  does not.

  The survival checks compare a `sum` of the file taken through an
  INDEPENDENT program, not the byte count the updater printed -- and the
  final one `cat`s the new file, because "hwdata says it wrote 1.6 MB"
  proves it wrote a file, not that the file is the one `lspci` reads.
  The served body carries a marker vendor the real database does not
  have, so that check cannot be satisfied by the copy already there.

  `--positive-control` serves the TRUNCATED body to the good-fetch check
  as well; four checks must go red. Measured: 8 of 12 pass under it.

  ON DEMAND: it launches its own guest against a sparse copy of
  `disk.img` and needs no host tools.

- **`fetch_ca_bundle.py`** -- fetches Mozilla's CA roots into
  `data/etc/ssl/certs/mozilla-roots.pem`, as the `ca-bundle` row of
  `fetch_extras.py`'s table, so `make iso EXTRAS=1` stages a real trust
  store. Not vendored: the bundle is MPL-2.0, and fetching is not
  distributing.

  **It takes curl's PEM conversion rather than Mozilla's own
  `certdata.txt`**, which is an NSS source file in a bespoke format
  needing a parser. The trade is explicit -- one more party in the
  chain, against writing and maintaining a certdata parser here.

  **The fetch itself is verified against the host's trust store.**
  Downloading a root store over an unauthenticated connection would be a
  joke at its own expense. It also refuses a file too small or too large
  to be a root store and counts BEGIN/END blocks before writing, because
  a captive-portal page is the shape of thing that otherwise lands here;
  and it writes to a temporary name and renames, so an interrupted build
  cannot leave a truncated store that fails for only some sites.

- **`libc_diff.py`** -- tolibc's formatter and number parsers against
  glibc, case by case. No guest: it compiles `kernel/lib/kfmt.c` and
  `userland/libc/stdlib.c` with the host gcc, links them beside glibc,
  and runs ~8,900 generated cases through both. Needs gcc and nothing
  else.

  **It exists because tolibc's bar is COMPLETENESS**, which a
  happy-path test cannot hold: such a test checks the cases whoever
  wrote it thought of, which are the cases they got right. It found
  `%hhu` of 256 printing 256, a sign emitted outside its padded field,
  `%.3o` ignoring the precision, `%5c` ignoring the width, `%0+d`
  emitted literally because flags were order-dependent, and
  `strtol("0", &end, 0)` reporting no conversion at all. It also found
  that `libc3_test.c` ASSERTED one of those bugs -- a self-referential
  suite defends what it got wrong, and a differential one cannot.

  **IT ONCE COMPARED tolibc AGAINST ITSELF AND REPORTED PERFECT
  AGREEMENT.** `userland/include/stdio.h` makes `vsnprintf` a static
  inline around `k_vsnprintf` and `#define`s `snprintf` to
  `k_snprintf`, so putting that directory on the probe's include path
  made both sides the same code. The probe is compiled in its own
  translation unit with no tolibc headers, declaring the entry points
  by hand -- and it now asserts at startup that `snprintf` and
  `k_snprintf` are different functions, always, not behind a flag. That
  check is the one that would have caught it.

  **Symbol clash**: tolibc defines `strtol`, `abort`, `qsort` and
  twenty more names glibc also defines. They are renamed at the C level
  (`#define strtol toy_strtol` ahead of an `#include` of the source)
  rather than with objcopy, because that also redirects tolibc's
  internal calls and leaves genuine externals alone.

  `strtod` is compared **within one ULP** by default: it applies the
  decimal exponent by repeated multiplication and is a couple of ULP
  off by construction, which its own comment says. Demanding equality
  would report that known limit on every run and bury the categorical
  bugs the harness is for. `--exact-float` shows the gap (two cases).

- **`gen_signames.py`** -- `signames.c` for the dash port, from
  `kernel/include/abi/signal_abi.h`. Replaces dash's own
  `src/mksignames.c` for two independent reasons, either of which would
  be enough: that file is **GPL-2 from GNU Bash** and its output is
  LINKED, so using it would pull the GPL into the shell binary; and it
  reads the **HOST's** `<signal.h>`, so a dash built with it would list
  Linux's signals and miss this kernel's numbering. The names are parsed
  out of the header rather than written down, so a signal added there
  appears on the next build. Run by the Makefile, not by hand.

- **`dash_test.py`** -- does the vendored dash BEHAVE like a shell? 18
  constructs run as real scripts and compared against real output:
  pipelines, redirection, here-documents, functions, parameter
  expansion, arithmetic, `case`, loops, command substitution, `test`,
  `trap EXIT`, positional parameters and exit status.

  **`dash_gap.py` proves dash compiles and links, which is a statement
  about the C LIBRARY rather than about the shell.** Before this, the
  whole of the evidence that dash worked was `dash -c pwd` printing
  `/` -- one builtin, one exit path, none of the parts a shell is for.

  **The scripts are FILES on the disk, not `dash -c` strings.** A shell
  test is made of quotes, and sending them through this script, then
  `vm.py`, then `tosh`, then dash gives four lexers a turn at them --
  which mangled every early attempt, a pipe arriving at `echo` as a
  literal argument. `tools/tfs3_writer.py` writes each script into a
  SPARSE copy of `disk.img` and dash is handed a path, which removes all
  four.

  **One case is deliberately not run**, and it is named in the file as
  `HANGS_THE_GUEST`: a pipeline whose reader fails to exec spins the
  writer at 100% CPU forever and the guest stops answering at all
  (`docs/bugs.md`). It becomes an ordinary case the day the pipe path
  reports the failure.

  A **warm-up** command runs before the table and its output is thrown
  away: the first exec after boot comes back empty while the guest is
  still emitting boot lines, which otherwise fails whichever case
  happens to be first and reads as a bug in that case.
  `--positive-control` expects a line no shell would print and requires
  the run to go red.

- **`dash_gap.py`** -- what the vendored dash port still needs from
  tolibc, measured by compiling it. No guest and no network: it runs
  dash's six build-time generators on the host, then compiles all 32
  sources against `userland/include/` and reports what the compiler
  refuses. Needs gcc and nothing else.

  **It exists because the requirement list kept being DERIVED, and was
  wrong in both directions twice.** `docs/roadmap.md` named `getrlimit`,
  `getpwnam`, `sysconf`, `times`, `fnmatch` and `glob` -- every one an
  `AC_CHECK_FUNCS` probe dash has a fallback for and never needs -- and
  named none of the ten missing headers, `SIGPIPE`, `uid_t`/`gid_t`,
  `DT_LNK`, the errno constants or `htonl`. A list somebody has to keep
  true is the shape this repo keeps deleting; this re-measures instead.

  **`-nostdinc` IS THE WHOLE POINT, and leaving it out invents a pass.**
  `USERLAND_CFLAGS` carries `-ffreestanding`, which does NOT stop
  `#include <sys/ioctl.h>` finding `/usr/include`. Measured without it
  this reported 17 of 32 sources compiling, plus two tolibc "bugs" that
  were really glibc's declarations colliding with tolibc's. With it,
  31 of 32 failed on missing headers. Any port built here needs the
  flag for the same reason.

  **Header discovery ITERATES**, because a missing header is a fatal
  error: gcc stops there and never sees the includes below it, so one
  pass found six of the ten. It shims what it found and asks again
  until nothing new appears. The shims are measurement scaffolding --
  enough to see the symbols behind a header, not a proposal for what
  tolibc should ship -- and a header with no shim is reported as such
  rather than counted as understood.

  `--positive-control` withdraws a shim the run just proved was needed
  and requires it to come back as a gap. It does this inside the temp
  copy, never in `userland/include/`, so a crash mid-control cannot
  leave the tree broken.

- **`umd_hostcheck.py`** -- compiles `userland/lib/umd.c` with the host
  gcc and renders every `docs/commands/*.md` page through it at 40, 80
  and 132 columns, plus the three field lookups (`umd_title`,
  `umd_field`, `umd_section_para`). No guest, no Pillow, no oracle
  beyond the pages themselves.

  **What it asserts is invariants, not an expected rendering.** A
  golden file for 109 pages would have to be regenerated every time
  anyone edits prose, and nobody would read the diff. Instead: the
  output is pure ASCII; every line fits the requested width unless it
  was COPIED verbatim rather than wrapped (a code block, or one word
  longer than the screen -- neither of which a wrapper may break);
  `**` does not survive into wrapped prose; the Synopsis block --
  the text `check_docs.py` already pins to the program's `cmd_usage()`
  -- comes out intact; and the lead paragraph of every Description has
  BALANCED backticks.

  **Comparison is on letters and digits only.** The renderer changes
  punctuation on purpose (backticks and asterisks go, an em dash
  becomes `--`), so comparing raw text would report every one of those
  as a wrapping failure.

  It found five real bugs the day it was written -- a paragraph's line
  break not being fed as whitespace, so the last word of one line and
  the first of the next were joined; unwrapped headings; a table of
  empty headers read as a rule row, which made the first DATA row the
  header for every row after it; a code block re-opened per line; and a
  cell splitting at a `\|` inside its own code span. It also found the
  SIX PAGES whose Description opened with a fragment of a table cell,
  unclosed backtick and all, since the day they were written.

  `--positive-control` renders every page with the wrap disabled and
  requires the width check to fail. Measured: 667 violations.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.

- **`term_scheme_hostcheck.py`** -- reads every
  `data/usr/share/terminal/*.scheme` on the HOST and reports what each
  colour will actually be once the Terminal has loaded it. No guest, no
  compiler, a fraction of a second.

  **What it is for is the PERMUTATION.** A scheme file is written in
  ANSI order -- black, red, green, yellow, blue, magenta, cyan, white,
  then the eight bright forms -- which is the order every published
  palette is published in, so one can be copied in without being
  rearranged by hand. A cell in the emulator holds an `enum vga_color`,
  where blue is 1 and red is 4. `userland/term/term_conf.c` permutes
  between them through the kernel parser's own `ansi_color()`.

  That step is the part nobody can check by looking, and it has already
  been wrong once: `Foreground=`/`Background=` name a `Color<N>` like
  everything else in the file, and taking one as a VGA slot put
  Solarized Dark's foreground on light red. It rendered, it looked like
  a colour scheme, and it was not the one in the file.

  So the ANSI table is written out here a SECOND TIME rather than parsed
  out of `ansi.c` -- an oracle that read the table it is checking would
  agree with it by construction. If `ansi.c`'s table changes this must
  fail and be corrected by hand; that is the point of the copy. It also
  refuses a file with a missing or malformed key, an index outside
  0..15, or a default pair whose two colours are equal (which parses
  perfectly and draws invisible text).

  `--positive-control` additionally fails any scheme the permutation
  actually MOVES, and reports how many -- two, the Solarized pair, being
  the only shipped schemes whose default pair is not already at 0 and 7.
  A control that reddens nothing has measured nothing, so it exits
  non-zero when none moved. Named by `ondemand_sweep.py`.

- **`ugfx_text_hostcheck.py`** -- compiles the real
  `userland/ui/ugfx_text.c` and `userland/ui/uui_textbox.c` with the
  host gcc against a SYNTHETIC proportional face and sweeps ~4,400
  checks over it. The face is deliberately hostile -- `i` and `l` are
  3px where `W` and `M` are 20 -- so a surviving `* char_w` is off by
  6x rather than by a rounding error, and kerning is non-zero on real
  pairs (`AV`, `To`) because a measurement that ignores it agrees with
  drawing only when no such pair appears.

  Three things, and the second is the one that matters. The
  MEASUREMENT against a Python oracle that shares no code with it.
  The ROUND TRIP: the x at which the widget DRAWS character `i` must
  hit-test back to `i`, for every character at every caret position --
  which is what catches `draw()` and `index_at_x()` drifting apart, the
  failure that put a click on a different glyph than the pointer. And
  the WINDOW: the caret stays inside the field, the value never scrolls
  further than it must, and nothing is drawn past the inner edge.

  `--positive-control` restores the cell arithmetic and requires the
  suite to go RED (569 of 4,407 do). **That control did not fire the
  first time it was written**, and for the reason this repo keeps
  writing down: it wrote the modified copy to `tmp/uui_textbox.c` while
  the driver includes `ui/uui_textbox.c`, so the bug never reached the
  compiler and a green run meant nothing. It writes to `tmp/ui/` and
  puts that `-I` first now, and asserts the edit changed something. In
  `ondemand_sweep.py`.
- **`utext_hostcheck.py`** -- compiles `userland/ui/utext.c` with the
  host gcc, beside a NAIVE implementation of the same wrap rule written
  out longhand in the driver, and requires the two to agree over a
  corpus: this repository's own Markdown, a 4,000-line synthetic
  document, a 60 KB paragraph with no newline in it at all, and
  `data/pci.ids` when it is there.

  **It exists because the editor's wrap is now ACCELERATED.** utext used
  to rescan the document from character 0 on every draw, every metric
  and every click; it keeps a sparse checkpoint table now (`struct
  utext_wrap`) so that a 1.6 MB file costs a screenful of work per frame
  instead of a documentful. An accelerator is exactly the code that is
  right on the corpus somebody tried and wrong on the one they did not,
  and its failure is a caret one character out -- invisible in a
  screenshot, obvious against an oracle.

  **The load-bearing check is a ROUND TRIP.** The point at which the
  oracle says character `i` is drawn must hit-test back to `i`, for
  every character on the screen, at five scroll positions from the top
  of the document to the bottom -- so lookups that start from a
  checkpoint deep in the file are exercised, not just the first screen.
  That is what catches `draw()` and `index_at_point()` drifting apart.
  It also checks the total line count at several widths, that
  `utext_scroll_top`/`_bottom` reach the actual ends, that a selection
  reports its TRUE length when the caller's buffer is too small for it,
  that a paste drops `\r`, and that `utext_putc` REFUSES at capacity
  rather than dropping the oldest character.

  It found a pre-existing off-by-one the hour it was written:
  `index_at_point()` tested "have we reached the target cell" BEFORE
  applying a pending wrap, while `draw()` applies it first, so clicking
  the first character of a wrapped continuation line placed the caret
  one character late -- on every wrapped line, and only there.

  `--positive-control` moves one checkpoint in the index by three
  characters and requires the round trip to go red.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.

- **`doc_test.py`** -- `/bin/doc` in a guest, over the serial debug
  console. Boots its own VM.

  **The load-bearing check is the pair of searches.** `-k` and `-K`
  would both pass a test that only asked "did anything come back", so
  the tool finds -- ON THE HOST, from the pages themselves -- a word
  that appears in some page's BODY and in no page's name, title,
  category or first sentence, then requires `-K` to find it and `-k`
  not to. A `doc` with both flags wired to the same code passes
  everything else here. Choosing the word on the host rather than
  naming one is what stops the check quietly ceasing to discriminate
  as the pages are edited.

  It also proves the pages REACHED THE IMAGE, which is a different
  thing from being staged (CLAUDE.md's rule about `seed/sync/`), and
  that a name that does not exist SUGGESTS rather than only refusing.

  **What it deliberately cannot see**, said in its own docstring rather
  than faked: the "not a terminal" half -- that `doc ls > out.txt`
  dumps and drops its colour. The kernel debug console has no
  redirection and quotes do not survive it, so `tosh -c` is out of
  reach, and ANSI escapes never arrive as text anyway because
  `kernel/lib/ansi.c` parses them first -- the same reason
  `ls_test.py` cannot assert on colour.

- **`pager_test.py`** -- `userland/lib/upager.c` under BOTH its front
  ends, by driving a GUI Terminal. `/bin/less` had no test at all until
  its paging moved into a library for `/bin/doc` to share.

  **The load-bearing check is the STATUS BAR.** Everything else passes
  with the pager replaced by `cat`: text appears, the screen changes
  when a key is typed (that key echoes at the prompt), the window
  survives. What only a running pager produces is a reverse-video band
  across the bottom row. Measured with a positive control that makes
  `upager_run()` dump instead of paging: the bar check goes red for
  both front ends and the other five stay green, which is exactly why
  the docstring names it.

  Two things the measurement had to get right, and the first version
  got both wrong. **The background is uniform too**, so "the widest run
  of one colour" is satisfied by an empty row -- the background is
  found as the commonest colour in the band and excluded. And it is
  COVERAGE rather than a run: reverse video paints the bar in the
  foreground colour and its text in the background one, so the longest
  unbroken run across a padded bar is one character wide.

  **It picks the LONGEST page on the host** for the `doc` half. The
  first version used `doc ls`, which renders to 25 lines and fits one
  screen -- so "space turns the page" was asking a correct pager to do
  something it must not.

- **`module_test.py`** -- loadable kernel modules the way a person uses
  them: `modload`/`modunload`/`lsmod` on `hello.ko`, the two refusals
  (an unexported symbol named in the log, a truncated file), and the
  check no KTEST can make -- unloading the e1000 MODULE takes the
  network away and loading it again brings it back, with `/bin/netd`
  re-leasing on its own. Asserts the DOWN half before the UP half, since
  a reload that silently did nothing would pass on the boot-time lease.
  ATTACHES to a running `vm.py` guest; ~30 s.
- **`sum_test.py`** -- `/bin/sum` itself, in a guest, against digests
  computed on the HOST. It ATTACHES to a running `vm.py` guest.

  **The load-bearing check is the cross-ring one**: `/bin/hello` in the
  guest is `build/userland/bin/hello.elf` on the host byte for byte, so
  the guest's line must equal what `zlib`/`hashlib` say here. A
  guest-only comparison would pass just as happily with a consistently
  wrong implementation on both ends of it.

  `-c` is asserted BOTH ways -- a manifest `sum` wrote must verify, and
  one carrying a right CRC with a wrong SIZE must fail -- because a
  verifier that says OK to everything is indistinguishable from a
  working one on the OK alone. That second case is also what proves the
  size field in `cksum`'s line is being compared at all.

  **Its fixtures are polled for the LAST TOKEN of the line, not for any
  content.** The debug console's `spawn` returns as soon as the child
  starts, and a read landing mid-write returned `3830823995 ` with the
  rest still to come -- the verify behind it then reported a malformed
  manifest, which reads exactly like a broken parser. Two checks failed
  that way on a `sum` that was correct.

- **`grep_test.py`** -- `/bin/grep` driven by typing into a GUI Terminal
  and asserting through the filesystem, reusing `terminal_probe.py`'s
  `Terminal` helper.

  **Its load-bearing check is the PIPE**, because every other check
  names a file on the command line and would pass with grep unable to
  read stdin at all. The pipe cannot be reached from the kernel debug
  console: that shell splits on spaces and hands `|` to the program as
  an argument, so `dmesg | grep partition` there runs `dmesg` with three
  arguments and fails in a way that reads as a broken grep.

  It asserts against `/tests/sample.txt` (401 numbered lines) rather
  than `dmesg`, which was the first version and was wrong: dmesg's ring
  is finite, so the boot lines a pattern names AGE OUT, and the check
  passed on a fresh boot and failed minutes later with grep working
  perfectly.

- **`multidisk_test.py`** -- two disks on two different drivers, which is
  the one configuration nothing else here boots. Every other tool
  attaches exactly one disk, and that is precisely the shape where
  "which driver ran" and "which disk is root" cannot disagree -- so the
  short-circuiting `if (!blk_virtio_init() && !blk_ahci_init())
  blk_ata_init();` was invisible to all of them while it made a
  machine's second drive not exist.

  Ten checks: both disks enumerated and named, `root=` overriding the
  driver precedence, the OTHER disk's partitions named too, exactly one
  device marked as the root, a named device resolving where an unknown
  one is refused by name, and an unknown `root=` reporting what it does
  have and booting anyway. Its positive control is the old short circuit
  restored, which reddens six of them and whose detail shows the
  bug outright -- `have ahci0` and no `ata0` at all.

  **IT IS ALSO THE ONLY GUEST WITH SOMEWHERE SAFE TO WRECK A VOLUME.**
  TFS3's post-commit journal KTEST ends with the volume read-only
  until something replays it, which on the root would fail every test
  after it -- so that test skips unless a second TFS3 mount exists,
  and this tool mounts one at `/mnt2` and runs `ktest fs` there. Three
  of the ten checks are that: the second volume really mounts (it
  could not, when every backend declared `max_mounts = 1`), the suite
  runs with nothing skipped, and what went read-only was `/mnt2` and
  not `/`. It `fsck repair`s first, as `ktest_run.py` does, or the
  blocks a killed boot leaked fail four unrelated fsck assertions.

  It builds its own boot image per phase (`make iso KCMDLINE=...`, then
  a copy), so **it rewrites `disk.img`'s GRUB line** and puts it back at
  the end. **So it cannot run BESIDE another tool**: its rebuilds land
  mid-run under anything else booting `toy-os.iso`, which `iso_guard`
  then refuses as stale (seen with `install_test.py` alongside). On
  demand, not in any gate.

- **`net_test.py`** -- the network stack end to end, on both NICs, with
  the verdict taken on the HOST. The KTESTs in `kernel/net/net_test.c`
  drive the protocols through `eth_input()` and need no network at all,
  which is what makes them fast and what makes them silent about
  whether a frame ever reached a wire.

  **Two independent oracles, and neither shares a line with the guest.**
  QEMU's user-mode network (SLIRP) answers the pings, so a wrong ARP, a
  wrong checksum or a wrong destination is simply never replied to. And
  every frame is dumped to a pcap (`-object filter-dump`) and decoded
  here, with the IPv4 and ICMP checksums RECOMPUTED from the bytes on
  the wire -- the same call `regex_hostcheck.py` and `fat32_test.py`
  make, because SLIRP can be lenient about something a decoder cannot.
  The decoder is thirty hand-written lines rather than scapy: a
  dependency the machine might not have is a check that silently stops
  running.

  The e1000 (the card QEMU's default machine has always
  had, so it needs no flag); **virtio-net, which is the only thing here
  that reaches `kernel/drivers/virtio/virtio_net.c`**; two cards at
  once, moved onto DIFFERENT subnets so that "the traffic left through
  the card that owns the subnet, and not the other one" is a per-device
  counter rather than "a ping worked"; a machine with `-nic none`, where
  the assertion is that `ping` reports instead of hanging; and the ARP
  retransmit rate, which is a regression test with a measurement behind
  it -- two pings at an unanswered address put **104 frames** on the
  wire before `kernel/net/arp.c` rate-limited requests, and 5 after.

  Then the four that came with UDP. **A real Python socket on the host**
  receives the guest's datagram, echoes it, and the guest reads the echo
  -- a round trip where neither end shares a line with the other; the
  capture is then checked for a valid UDP checksum, which covers a
  pseudo-header that is on no wire and is the one thing a stack can get
  wrong while agreeing with itself perfectly. **ICMP port unreachable**,
  judged from the capture after the HOST sends to a port nothing is
  bound to (`hostfwd`, the only way to make a datagram arrive at the
  guest). **DHCP on 192.168.76.0/24**, which is the load-bearing DHCP
  check: on QEMU's default network a real lease and a hardcoded
  10.0.2.15 are indistinguishable. Nobody types anything -- init runs
  the client -- so this is also the check that a machine configures its
  own network at boot.

  **Phase 9 is LINK-LOCAL**, on the one segment SLIRP cannot be: a
  socket netdev whose only peer is the test itself, so nothing answers
  DHCP. Two boots, because the question worth asking is not "did it pick
  an address" but "does it give one up when somebody already has it" --
  the first boot claims one unopposed, the second is ANSWERED for that
  exact address by `LinkPeer` and must end up somewhere else. The peer
  is the oracle as well as the neighbour: the ARP probes (sender
  0.0.0.0) and the announcements (sender = the claimed address) are read
  off the wire on the host. Predicting the address here instead would be
  checking this OS's arithmetic against a copy of itself.

  And **DNS**, which SKIPS when the
  host itself cannot resolve -- an offline machine is not a bug in this
  OS -- while still checking the "no nameserver configured" path, which
  needs nothing but the guest.

  **Phase 11 is TCP**, against python's own `http.server` on the host --
  local rather than a site on the internet, because the suite must not
  depend on this machine having connectivity (the DNS phase, which
  genuinely does, SKIPS instead). An independent server will not
  complete a handshake the guest gets wrong, and the capture is checked
  for a real three-way handshake plus TCP checksums recomputed here over
  the pseudo-header -- the same trap UDP has, made worse by TCP having
  no length field of its own.

  **The last phase is `inetd`**, and it carries the two checks the
  serial server cannot pass. `/bin/cat` is run as an echo server --
  it copies fd 0 to fd 1 and knows nothing about sockets, so bytes
  coming back are the only available proof that the accepted connection
  really landed on the child's standard streams. Then a client connects
  and **says nothing**, which parks a one-at-a-time server in `read()`
  forever, and a second client is required to get a complete response.
  "Both were answered eventually" is what a serial server passes, so the
  first connection is left hanging on purpose rather than closed. The
  control was run: against plain `httpd` the second request times out.
  A third check asks for **ten connections in a row, each compared
  against the staged bytes** -- a server that answers three and wedges
  passes both of the others.

  **It also found a harness bug that had been reading as an OS limit.**
  Nothing here drained the serial socket while a server held the
  console, so the guest filled COM1's buffer and stalled in its own
  write -- which looks exactly like a server exhausting sockets after N
  connections, and was diagnosed that way first. The server phase had been
  passing with four connections of margin, so one extra log line per
  request was enough to turn it red. `Shell.drain_start()` is the fix,
  the same thing `usb_test.py` does while it types.

  It found four real defects while being written: the ARP storm above; a
  sequence number burned by every retried send, so a capture showed a
  ping starting at 4; sockets never released on close, so the third run
  of a program opening four of them could open none; and one in the
  harness worth knowing, since it looked like a guest bug -- SLIRP is a
  NAT and rewrites the source port, so the guest's own ephemeral port is
  visible only in the capture. Boots ten guests against a COPY of
  `disk.img`; on demand, not in any gate.

- **`ntp_test.py`** -- network time end to end, against a server on this
  machine's own loopback. `kernel/core/ktime_test.c` proves the wall
  clock can be set and read; it says nothing about whether a packet on
  the wire produces the right number, which is this half.

  **Nothing leaves the machine, and it works with the cable out.** The
  server is a plain Python UDP socket on `127.0.0.1`, reached because
  SLIRP maps whatever the guest sends to `10.0.2.2` onto the host's
  loopback -- `net_test.py`'s UDP phase does the same. It binds an
  unprivileged port rather than 123, which is the whole reason `/bin/ntpd`
  has a `-p` flag: a test server cannot bind 123, and the alternative was
  a suite that reached a real time server on the maintainer's connection.

  **The oracle is the host, and it judges both directions.** The reply
  carries an instant the test CHOSE -- a date in 2013, BACKWARDS from any
  plausible boot, so a guest that ignored it and let its clock run
  forward cannot pass by accident. And the REQUEST is validated here:
  version 4, mode 3, 48 bytes, a non-zero nonce. A client that sent
  version 3 would be answered by a real server and would still be wrong.

  **It asserts on state, never on what the guest printed.** An earlier
  version parsed `ntpd`'s own messages and reported a working client as
  dead, because a spawned program's stdout does not dependably reach the
  serial console this drives (`settings_test.py` records the same trap).
  What the guest DID is visible in two places that cannot lie: the
  datagram the host received, and the clock afterwards.

  `--positive-control` answers with a deliberately wrong epoch conversion
  -- 1900 where 1970 was meant, the single likeliest client bug. It lands
  the guest in 2079 and the clock checks must go red; the tool INVERTS
  its verdict under the flag, so a control that changes nothing is
  itself reported as a failure.

  **Its reboot check SKIPS under QEMU, and says so rather than passing.**
  QEMU re-seeds its emulated MC146818 from the HOST clock on machine
  reset, so a guest's CMOS write cannot outlive a reboot however correct
  it is -- measured, by watching the guest's own "rtc: hardware clock
  reads" boot line come back matching the host to the second. The write
  itself is proved in the same boot by the KTEST that reads the hardware
  back through `rtc_read()`.

  **On the bare-metal laptop it does survive, measured 2026-09-03**: a
  sync, a reboot, and `clock.steps` back at 0 with the time still
  correct -- so the clock had come from the CMOS rather than from
  anything that ran after boot. Boots one guest against a COPY of
  `disk.img`; on demand.

- **`partition_test.py`** -- boots toy-os with its filesystem **inside**
  an MBR or GPT partition. The only thing that exercises `vfs.c`'s
  boot-time partition scan and `block_part.c`'s window end to end; the
  `partition` KTEST suite proves the encoder and parser agree on a RAM
  disk and cannot prove either of those.

  **Its load-bearing check is `df`, not "it booted".** A kernel that
  ignored partitions entirely still boots -- it finds nothing at LBA 0,
  refuses to format a partitioned disk and runs RAM-only, which looks
  like a bad image rather than a missing feature. And one that found
  the partition but got the window wrong would mount something and
  report the DISK's size. So the assertion is that the mounted volume
  is 256 MiB while the image is 2 GiB, which only a correct window
  produces. It also reboots (a write that reached only a cache would
  pass a same-boot read-back), reads `parttable` from inside a
  partition (which the old `ata_read_sector()` path could not do), and
  finishes by driving `/bin/mkpart` in the guest -- including the
  refusal without `confirm`.

  ON DEMAND, not in the gate: two images, four boots. Same category as
  `virtio_boot_test.py` and `live_boot_test.py`.

- **`fat32_test.py`** -- FAT32 and the mount table, checked against an
  **independent implementation**. `kernel/fs/fat32_test.c` formats a
  512 KiB RAM volume and drives the backend directly, which proves the
  driver agrees with ITSELF -- and a shared misreading of the format
  passes both halves, because the same person wrote the writer and the
  reader. So the oracle here is the HOST: `mtools` reads back what the
  guest wrote and `fsck.fat` audits the volume afterwards, neither
  sharing a line with `kernel/fs/fat32.c`. Same call as
  `regex_hostcheck.py` (against GLIBC) and `uimg_hostcheck.py` (against
  libjpeg).

  It also covers the half no KTEST can reach: that the ESP on the REAL
  disk -- a volume written by mtools, holding GRUB's own files --
  mounts at `/boot` on an ordinary boot, and comes up READ-ONLY.

  **Three checks that discriminate**, each replacing one that would
  pass on a broken driver. "`ls /boot` lists something" passes on a
  driver that mangles every long name, so it reads GRUB's own
  `grub.cfg` and looks for text only a correct chain walk produces. "A
  file written reads back" passes on a driver whose format is privately
  wrong, so a 185 KiB BINARY is extracted with mtools and compared byte
  for byte against the build artifact. And "the volume still works"
  passes on one leaking clusters or cross-linking chains, so `fsck.fat`
  has the last word.

  ON DEMAND: two boots against a COPY of `disk.img` (never the real
  one), and it SKIPS cleanly without `mtools`. Needs `dosfstools` for
  the audit half.

- **`run_release.sh`** -- standalone QEMU launcher shipped as a GitHub
  Release asset (not part of the build), for running from just a
  release download with no checkout. Gunzips `disk.img.gz` if needed,
  boots with `make run`'s same device/display flags. When cutting a
  release: rebuild `disk.img` fresh (`make clean-disk` first), then
  `gzip -k -9 disk.img` before attaching it -- it's a large SPARSE file
  (9GB apparent, ~2MB of real data on a freshly-trimmed image), and
  GitHub's 2GB-per-asset limit plus plain bandwidth sense both rule out
  the raw file. Run the matching writer tool's trim
  (`tools/tfs3_writer.py trim disk.img` for a fresh-built image,
  `tfs2_writer.py` for an old TFS2 one) before gzipping
  -- sparseness is only ever lost, and an untrimmed image compresses
  whatever stale data it is still carrying. See
  `docs/decisions.md`'s versioning entry for the full v0.0.9 writeup.

- **`check_syscalls.py`** -- NO TWO ROWS OF `syscall_table.c` RESOLVE TO
  THE SAME NUMBER. The table is built with designated initializers, so a
  row's index IS its syscall number -- a good property with one hole: C
  lets the same index be written twice and silently keeps the LAST one.
  That happened on 2026-09-12. `SYS_SIGPROCMASK` was given 108 by
  reading the bottom of `abi/syscall_abi.h` and adding one, but the
  numbers there are NOT in file order and 108 was already `SYS_FORK`.
  The build was clean, the new syscalls worked, and `fork()` dispatched
  to `sigprocmask()` -- so every fork returned 0 and every caller
  believed it was the child. The only symptom was `fork_test` dying with
  no fault and no log; the only thing that showed it was `strace`
  printing the wrong name for the call. It reads the row NAMES out of
  the table and their values out of the header, so nothing guesses which
  `SYS_*` constants are numbers -- having a row is the definition, and
  the flags and limits sharing the prefix are simply not rows. In
  `preflight.sh`.
- **`check_text_measure.py`** -- fails the build on a character COUNT
  used as a text WIDTH. `ugfx_char_w()` is the WIDEST advance in the
  face, so `n * ugfx_char_w()` measured a string correctly only while
  the interface face was monospace; the day it became `liberation-sans`
  it became the width of the widest possible string of that length, and
  ~23 sites across the toolkit, the WM and four apps were wrong at once
  -- a Start menu 1.8x too wide, labels cut early, a file-view column
  pitch (and its selection highlight) 1.7x over, and a text field whose
  caret and click landed on a grid the glyphs were not on.

  **The discriminator is a NAME versus a LITERAL**, and that is the
  whole reason the check is usable: a variable paired with the cell
  width is nearly always a count of characters in real data (a
  `strlen`, a `width_chars` field, a `MAX_CHARS` budget), while a
  literal is a layout unit -- `char_w / 2` of padding, a two-cell
  gutter -- which `ugfx.h` explicitly permits and which there are ~76
  of. Flagging those too would have meant 76 waivers and a check nobody
  reads. It tracks aliases (`int cw = ugfx_char_w()`) and scopes them
  per function, which is load-bearing rather than tidy: an `int w =
  ugfx_char_w()` in one function otherwise makes every `w` in the file
  look like a cell width, and it reported a callback's own parameter as
  a bug until it did.

  Its honest limit: a LITERAL count used as a text pitch
  (`14 * ugfx_char_w()` for a filename column) reads as a layout unit
  and is not flagged. Two of those existed and both are fixed by hand.
  Waive with `text-measure-ok: <reason>`; the legitimate reason is a
  monospace bracket, where dividing by the cell is the right answer.
  `--positive-control` proves the matcher fires. In `preflight.sh`.
- **`check_widget_ops.py`** -- refuses a `struct uui_widget_ops` table
  with a slot it needs left NULL, and it exists because FOUR widgets
  shipped with short tables on one day (`uui_dropdown`, `uui_checkbox`,
  `uui_textview`, `uui_textbox`), every function they needed already
  written. Two rules: a table with `draw` needs `natural_size` and
  `set_geometry` (a layout cannot place what it cannot measure), and a
  table with `press` needs `release` (`uui_route.c` names a widget to
  its app only when it has one). Both failures are SILENT and surface in
  a different file from their cause -- one of them presented as
  "`uui_layout` stops after four children" and cost most of a session,
  when `uui_layout_run()` has no early exit at all. Waive in place with
  a `widget-ops-ok: <reason>` comment, the same mechanism
  `check_dispatch.py` uses. In `preflight.sh`.

  **It also refuses a widget index derived from the array's own
  LENGTH** -- `#define WIDGET_TREE_SPLIT (g_widget_count - 3)`, and the
  chained `(WIDGET_TREE_SPLIT - 1)` form. Appending one widget shifted
  three of those at once in the File Manager, so hiding the tree hid the
  tree splitter and hiding the pane splitter hid the CONTEXT MENU: a
  right-click that stopped working in single-pane view, diagnosed from a
  screenshot rather than from any test. Look the widget up by its id.

  **And a widget that DRAWS A SCROLLBAR must not route `hit` through
  its row hit.** `_hit(...) >= 0` answers "which row", which excludes
  the bar column -- and `uui_route.c` gates press AND wheel on that
  slot, so the bar cannot be dragged and the wheel is dead anywhere
  that is not a row. It reads as an unimplemented scrollbar rather than
  as a routing bug, because the rows themselves work. Five widgets
  shipped it: `uui_listbox`, `uui_table` and `uui_fileview` each fixed
  it and left a comment, and `uui_sidebar` and `uui_tree` still had it
  in 2026-09-03 -- three comments having failed to stop it twice is
  what earned a check. Answer `uui_hit(x, y, w, h, ...)` there; `>= 0`
  stays right for a widget with no bar.
- **`check_key_routing.py`** -- its sibling, and the inverse question:
  `check_widget_ops.py` catches an ops slot nobody filled, this catches
  a filled slot nobody can REACH. An app whose `struct uapp_desc` names
  a widget whose ops table has a `.key` must declare `.focus` or
  `.on_key`, because those are the only two doors in `uapp.c`. Its
  second rule is the same shape from the pointer's side: an app that
  declares `.widgets` and calls `uui_menubar_press()` is hand-routing a
  menu bar the router should own, so a popup row's click also lands on
  the widget beneath (Image Viewer and Player shipped that way). It
  exists because type-ahead was added to `uui_table`, tested, and
  shipped doing nothing in Task Manager -- which declared neither, so no
  key had ever reached the widget and its arrows and paging had been
  dead since the app was written. Nothing caught it because every check
  in `taskmgr_test.py` drives by MOUSE. **The key-capable set is
  DERIVED** from the `.key` slots in `userland/ui/*.c` rather than
  listed, so a new widget is covered the day it gains a key handler --
  and `uui_menubar_ops`, which has no `.key` and is reached through
  `uui_router_overlay_key()`, correctly does not count. **It reads the
  code, not the comments**, which its own positive control caught: with
  `.on_key = on_key,` commented out the first version still matched the
  text and reported the app as routed. Waive with a `key-routing-ok:
  <reason>` comment; `diskmark.c` is the one waiver, because an OPEN
  popup takes keys through the overlay path regardless. In
  `preflight.sh`.
- **`check_drivers.py`** -- refuses a driver that declares itself to
  nothing. Every `.c` under `kernel/drivers/`, and any file anywhere
  that calls a device class registry, must carry a `DRIVER_DECLARE(...)`
  or a `driver-none: <reason>` comment; `*_test.c` is exempt, since a
  KTEST beside a driver is not a second driver. It exists because THREE
  drivers were absent from `lsdrv` when it was written -- `i8042` (the
  PS/2 keyboard and mouse, the input path every default boot uses),
  `virtio_input.c` and `virtio_rng.c` -- and nothing was looking.

  **The half it CANNOT check is the half that mattered most**, and that
  is why the declaration moved into the `.drivers` linker section in the
  same change: `DRIVER_REGISTER` used to be a call inside `init()`, and
  `e1000_init()` returns at its "no card on this bus" check before
  reaching it -- so a build containing the driver listed no driver, and
  no amount of grepping for the call would have said so. Data cannot be
  skipped by a return. **It strips comments before matching**, which its
  own positive control needed: `kernel/core/kernel.c` merely NAMES
  `net_register()` in a comment, and matching that sends the reader at
  the wrong file. In `preflight.sh`.


  **A TRAILING `// dispatch-ok:` ON THE SWITCH'S OWN LINE COUNTS.** It
  did not, until a construct that had been "waived" that way for months
  grew past the limit and was reported -- the checker only looked at the
  lines ABOVE, where a trailing comment can never be. A waiver mechanism
  that silently does not waive is worse than none: it reads as
  protection at the call site and provides none.

  **IT SKIPS `userland/ports/`.** The rule is about how code in this
  project is structured and its remedies are both EDITS -- rewrite the
  chain, or waive it with a comment -- and vendored source is the one
  place an edit is forbidden by policy. Doom's `p_spec.c` has a
  72-branch switch written in 1993; it is not a finding, for the same
  reason the Makefile turns `-Wall` off for that directory.
- **`check_initcalls.py`** -- an init that is both an `INITCALL` and a
  hand call (it would run twice), an `INITCALL` at a level
  `kernel_main()` never walks (it would never run), or a `PCI_DRIVER`
  probe called by hand (the bus already calls it). Static, over the
  source, because the in-kernel `initcall` KTEST can see "never ran"
  at boot but not "ran twice". Run by `preflight.sh`.
- **`check_chains.py`** -- a file that gained a `vga_write_dec`/`_hex`
  or `klog_write_dec`/`_hex` chain call beyond its frozen count, or a
  file that started one. The 109 that exist stay (62 in
  `apps/shell_sys.c`, which the roadmap wants in ring 3); the point is
  that `*_printf` is what new code writes. The baseline is a ratchet:
  the script prints the lower number when a file drops. Run by
  `preflight.sh`.
- **`check_copy_user.py`** -- a `vmm_copy_*_user()` result compared
  with `< 0`. The helpers return 1/0 and never negative, so that branch
  is dead and the handler runs on an unfilled buffer; twelve socket
  syscalls shipped that way with the contract stated in the header
  above them. Static, over `kernel/` and `apps/`. Run by `preflight.sh`.
- **`dup_scan.py`** -- where is the tree actually duplicating code?
  Normalises away comments, blanks, brace-only lines and whitespace,
  hashes a sliding window (`--window`, 6 by default), and reports every
  window occurring in two DIFFERENT files, each extended to its maximal
  run and claimed so one 40-line clone is reported once rather than as 35
  overlapping windows. Vendored `ports/` is skipped -- that is somebody
  else's duplication. **It ALWAYS exits 0**: whether a block is worth
  extracting is a judgement call, and a check that failed the build on it
  would be wrong most of the time.

  **What it cannot see, said plainly because a clean run is the most
  misreadable kind of result**: the same idea written twice in different
  words, and an API read two different ways by its callers. The menubar
  sentinel in `docs/conventions/gui.md` -- six apps, three readings, one
  latent keyboard-only bug -- was found by COUNTING CALLERS of one
  function, not by this. A clean run means "no copy-paste", never "no
  duplication".

  First answer, 2026-09-17: 71 duplicated lines across 27 GUI files and
  75 across 88 `/bin` programs, i.e. almost nothing -- which is why the
  work that came out of that audit was a contract fix rather than an
  extraction. In `ondemand_sweep.py`.
- **`loc.py`** -- how big this project is, honestly: source lines with
  generated files, comments and blank lines all excluded, and the
  with-comments figure beside it. Not `wc -l`, because the answer
  depends entirely on knowing which files are GENERATED and that list is
  not guessable -- `kernel/drivers/font_ttf.c` alone is ~16,700 lines of
  baked glyph tables from `genttf.py`, a fifth of the tree, and says
  nothing about how much code anyone wrote. The list is `GENERATED` at
  the top of the script; **anything a `tools/gen_*` writes into the
  source tree has to be added to it**, since a missed entry inflates the
  count silently, which is this script's one failure mode. Its comment
  stripper tracks string and character literals, so a `"//"` inside a
  format string is not read as a comment -- a naive stripper
  under-counts exactly the files that do the most string work.
  `--by-dir` groups by directory instead of language, `--files N` lists
  the largest N. Reports the OS itself separately from `tools/`, which
  is a fifth of the tree and ships in nothing. NOT in `preflight.sh`: a
  line count is a fact to look up, not a gate, and this file's own rule
  is that a number nobody has to keep true is the only safe kind.

  **`--by-origin` is the view the other two hide.** The default buckets
  by language and `--by-dir` by top-level directory, so `userland/ports/`
  -- which holds MORE lines than the OS written here, mbedtls alone
  being ~116k -- counted as ours. That is the same silent inflation the
  `GENERATED` list exists to prevent, arriving from a different
  direction, and it made "how big is this project" unanswerable without
  doing arithmetic by hand.


---

## Host tools this repo expects (not in `tools/`)

Installed on the maintainer's machine rather than checked in. Nothing
here is required to BUILD toy-os — `make all`, `make iso` and
`preflight.sh` work without every one of them — but each removes a
rederive-from-scratch cost, which is the same bar `tools/` holds itself
to. Arch package names; all are in the official repos.

- **`bear`** — generates `compile_commands.json` from the real build:
  `bear -- make all` (start from `make clean`, or it only captures what
  actually recompiled). Gitignored, because it holds machine-specific
  absolute paths.

  **This is what makes `clangd` work on this repo**, and it earns its
  keep on a codebase of this shape: `userland/ui/` alone is a couple of
  dozen widgets whose ops tables and helper signatures are easy to guess
  wrong.
  Writing `uui_tree.c` without it cost a build cycle to five wrong
  guesses in one file — `uui_scrollbar_draw`'s arity, three `uui_widget_ops`
  function-pointer types, and `KEY_UP` where this kernel spells it
  `KEY_ARROW_UP`. Regenerate it after adding a source directory.

- **`ccache`** — wired into the Makefile as `CC = $(CCACHE) gcc`, which
  falls back to plain `gcc` when it is not installed, so a checkout
  without it builds identically. It pays for itself on the GATE rather
  than on an ordinary edit: `preflight.sh` and `make verify` both start
  with `make clean`, so every run is a full rebuild of a tree that
  mostly did not change. Measured here: **2.37s → 0.40s** for
  `make clean && make all`. It hashes preprocessed source plus flags, so
  a CFLAGS change correctly MISSES the cache — which matters, because
  the `.d` files do not track flags at all (CLAUDE.md).

- **`ruff`** — `ruff check tools/`, configured by `ruff.toml` at the
  repo root. Deliberately narrow (`F` + `E9`): the default ruleset
  reports ~320 findings here and essentially all are style. What is
  selected is the class this repo actually gets bitten by — **harness
  bugs that report a healthy system as broken**. Not in `preflight.sh`,
  for the same reason Docker is not: the gate must not start requiring
  a tool a checkout may not have.

- **`shellcheck`** — `shellcheck tools/*.sh`. Small surface (six
  scripts) and directly relevant: the repo has already been bitten by a
  `pkill ...; rm ...` chain aborting under `errexit` because `pkill`
  exits 1 when nothing matched, which shellcheck flags directly. It
  found one real thing on first run: an unguarded `cd` in
  `preflight.sh`, which would have run the whole gate — `make clean`
  included — in the caller's directory if it ever failed.

- **`clang-tidy` / `scan-build`** — a second opinion on the C, usable
  only once `bear` has produced the compilation database. Building the
  kernel with `clang` occasionally is worth it for the same reason: a
  different compiler's warnings find real bugs a single toolchain hides.

- **`docker`** — `qemu_matrix.py` needs it, and nothing else does.

Deliberately NOT used: `gcovr`/`lcov` (coverage needs runtime support a
freestanding kernel does not have) and `valgrind` (same reason).

## `tools/idle_cpu.py` -- how much CPU does toy-os burn doing NOTHING?

Boots a VM, lets the desktop settle, and measures the **host's** CPU
time for the QEMU process over a window in which nothing is sent to the
guest. Also reports what `ps` says `toywm`'s state is, once, before the
window opens.

    python3 tools/idle_cpu.py                # 30 s window
    python3 tools/idle_cpu.py --window 60    # longer, less noise

**WHY THE HOST'S CLOCK AND NOT THE GUEST'S.** The guest cannot answer
this. `ps` bills whichever process was current at each timer tick, and
on a machine where something is always runnable that measures who got
scheduled rather than work done -- parking the compositor changed its
reported CPU by ZERO while moving its state from `ready` to
`block(event)`. The host's view of the emulator sits outside that
accounting entirely.

**QUOTE DIFFERENCES, NEVER THE ABSOLUTE NUMBER.** A large part of the
reading is TCG itself: translating the guest's 100 Hz tick costs host
cycles no guest change can remove. Worse, it tracks the host's own load
at the time -- the compositor's wait measured 42% of a core against
37-38% on 2026-08-27 and 0.76 s against 0.57 s per 30 s on 2026-08-29,
and only the ratios are comparable. Run it twice against the same host
with one thing changed, and take three samples an arm: the pairs above
are tight enough to separate only because they were.

**Do not run it beside anything else.** A second guest or a build
competing for cores lands directly in the number.

## `tools/timer_bench.py` -- what a timer configuration costs and buys

Boots one build on a copy of its disk and reports three things: IDLE
(timer interrupts and periodic ticks a second over a quiet desktop, the
share of time the tick was stopped, and the QEMU process's host CPU),
LATENCY (`/tests/timer_bench`'s sleep overshoot at 1/3/7/16 ms, alone
and against two busy processes) and WORK (units one busy process
completes in 2 s, and two). It is what `option hz`'s default was chosen
on.

    python3 tools/timer_bench.py                         # this checkout's build
    python3 tools/timer_bench.py --kvm --runs 3 --label hz250 --json out.jsonl

To compare rates, build each one and point the tool at its image:
`make iso HZ=250`, copy `disk.img` somewhere, repeat, then run the tool
once per copy with `--disk`. `KCMDLINE="highres=off"` measures the
periodic path of the same build.

**THE IDLE COUNTERS ARE THE KERNEL'S OWN** (`config get clock.tick_*`,
QUERY_CLOCK), so a build from before them reports host CPU only. **SAY
WHICH ACCELERATOR** -- TCG and KVM disagree on everything here, and a
KVM guest without APICv pays VM exits per timer interrupt that TCG does
not model. WORK moves a few percent between runs of ONE build; believe
only a difference bigger than that, with `--runs`. Named by no runner:
it produces a comparison, not a verdict.

`tools/gen_kconfig.sh` writes the other half -- `build/gen/kconfig.h`
from build.conf's `option hz/tick/highres`, at Makefile parse time and
only when a value changed, so the `.d` files rebuild exactly what
includes it.
