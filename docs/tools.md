# tools/

The dev/build helper scripts — not compiled, not shipped as part of the
OS. CLAUDE.md keeps a one-line index of these; this file is the full
reference for each one: what it does, why it exists, and the traps it
encodes.

**The bar for adding one:** does it fix a rederive-from-scratch cost?
That is the reasoning that produced every tool below. Add freely when it
does — and add a line to CLAUDE.md's index, which `tools/check_docs.py`
verifies.

Dev/build helper scripts, not compiled or shipped as part of the OS:
`genfont.py`/`genttf.py` (font generation, pre-existing), `gen_kbs.py`
(generates the `seed/sync/etc/kbs/<layout>` keyboard-layout data files
from Linux's own XKB data -- see `docs/decisions.md` on layouts being
data files, not a compiled-in enum), `qmp_test.py`
(QEMU/QMP GUI testing helpers — see `docs/testing.md`), `boot_smoke_test.py` (fast
non-GUI boot check — see `docs/testing.md`), `gen_version.sh`/`set_version.sh`
(versioning — see CLAUDE.md's `version.h`/`VERSION` bullets),
`ktest_run.py` (drives the in-kernel test suite over serial and turns
it into an exit code -- what `make test` and CI run), `vm.py` (start a
headless VM and run shell commands against it, getting text back — see
`docs/testing.md`).

The rest, added once the build/test/delivery loop had enough repeated
manual steps to be worth automating:
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
  that WRAPS onto a second line or runs past 140 characters, a stale
  decisions index, a link to a doc that does not exist, and a tool in
  `tools/` that CLAUDE.md never mentions. The one-line rule is checked
  rather than stated for the usual reason: the roadmap reached 2,939
  lines by accumulating a paragraph per item, and nothing noticed. The duplicate check earns its
  place on its own -- two of this repo's own roadmap edits duplicated an
  entry and a third silently deleted three. The tool check enforces a
  rule this file already stated and nothing verified; it is deliberately
  a NAME check, so it says a tool is mentioned, not that what is written
  about it is still true. It does NOT flag `Milestone N` in prose
  (historical, and `docs/roadmap-details.md` ends with a legend for
  those); the noise would be what stopped anyone running it. In
  `preflight.sh` and CI.
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
- **`gui_flow.py`** -- named, composable QMP click-flows on top of
  `qmp_test.py`'s `QMPSession` (`GuiFlow` class: `enter_gui()`,
  `open_app(name)`, `run_system_action(label)`, `screenshot_named()`),
  so a testing session doesn't hand-derive Start-menu row pixel math
  from scratch every time. `APP_ORDER` must stay in sync with
  `apps/gui_apps.c`'s registry order.
- **`shell_flow.py`** -- the same idea as `gui_flow.py`, for the
  PHYSICAL (pre-`gui`) shell instead of the GUI: `ShellFlow.
  run_command(cmd, subdir=...)` types a full command -- including
  spaces/hyphens/underscores/a few other punctuation chars
  `qmp_test.py`'s `send_text()` can't handle on its own -- presses
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
  256-record table on TFS2-legacy images only -- TFS3, the default
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
  `guard_test`, `malloc_test`) as one pass/fail table, asserting BOTH an
  exit code and required output. In `preflight.sh`. It fills a real gap:
  `make test` runs inside the kernel and `gui_regress.py` covers the
  windowed clients, so nothing ever ran a plain `/tests` binary except a
  person typing `run <name>`. **Read its `EXCLUDED` list before adding
  to it** -- a test that faults on purpose, blocks on the serial port,
  needs a desktop, or needs a parent to spawn it will fail in a way that
  says nothing about the code under test. `pipe_test` is the worked
  example: it exits 3 under `run` because its `waitpid` finds no parent,
  and passes fine under the KTEST that spawns it properly.
- **`faulttest_run.py`** -- the ring-3 diagnostics that FAULT ON
  PURPOSE, which `usertest_run.py` correctly excludes and which
  therefore nothing ran at all. A faulting binary has no exit code and
  no output of its own, so the assertion is the KERNEL's report, read
  out of the serial log: each entry names required AND forbidden
  substrings, which is where the value is -- a stack overflow and a
  null dereference are both page faults, and every entry doubles as the
  positive control for its neighbours. Each test gets its own QEMU,
  because a ring-3 crash takes the serial debug console down with it
  (`vm.py` cannot drive these at all -- `crash_test` included), so the
  command is typed at the PHYSICAL shell over QMP instead. Not in
  `gui_regress.py`; run it after touching the fault path, the ELF
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
  measures nothing. The non-GUI control must stay flat -- if it drifts,
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
- **`init_test.py`** -- init as pid 1 AND as a supervisor, end to end:
  the kernel spawns it, it holds pid 1, an idle init is BLOCKED rather
  than spinning, `kill 1` is refused, abandoned children are adopted AND
  reaped, the process table returns to its baseline, `SYS_SLEEP` passes
  its own checks, the target it read matches the settings REGISTRY, the
  desktop is init's child, a service that cannot start is given up on
  without taking the desktop with it, and killing the desktop brings it
  back with no shell involved. **The slot count is the assertion** for
  the reaping half -- a zombie nobody reaps is invisible until the table
  fills up, and every individual process behaves perfectly either way.
  Two fixtures are load-bearing: `/tests/orphan_test` must be started
  with `spawn`, not `run` (the legacy loader's children have ppid 0
  already, so there is nothing to orphan), and the crash-loop fixture is
  a descriptor naming a nonexistent binary, written onto the disk COPY
  from the host so the machine boots with it already in place. Two
  positive controls, in its docstring: `heir = 0` in
  `reparent_children()` reddens the two adoption checks, and dropping
  init's give-up reddens the crash-loop one while the desktop check stays
  green. **What it does NOT cover, and says so: the `target=text`
  command-line override**, which needs its own ISO -- `make iso
  KCMDLINE="target=text"`. Not in `gui_regress.py`; run it after
  touching the scheduler's parentage, reaping, `SYS_SLEEP`, the target
  setting or the service descriptors.
- **`console_shell_test.py`** -- a `text` boot reaches a RING-3 shell
  prompt and the kernel shell is not involved (`docs/init-design.md`'s
  stage 4). Eleven checks: init is what started `/bin/tosh`, an idle
  tosh is BLOCKED rather than spinning, a typed line spawns a program
  (asserted through the FILESYSTEM -- `file_test` writes
  `/filetest.txt`), a kernel-shell builtin typed at that prompt creates
  nothing, and Ctrl-D is followed by a fresh prompt rather than a dead
  console. Three more assert the SHARED LINE EDITOR at that prompt --
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
  another guest already holds, and picks a free slot for callers that
  ask. It exists because **a QMP port clash does not fail as a port
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
- **`pixel_probe.py`** -- reads exact pixel values out of screenshots,
  and tabulates the same points across several (`--compare a.png b.png
  --at 85,100 --at 215,100`), flagging which moved and which didn't.
  This is how `docs/gui-guidelines.md` says to verify a GUI change, and
  the rule exists because a hover state that shifted the background by
  TWO units out of 255 looked entirely plausible in a PNG. Always
  include a point that should NOT change -- half the assertion is the
  neighbour staying put. `--box N` averages a square, for anti-aliased
  edges where a single pixel is a coin toss.
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
  quietly. **Its workload CHURNS `/usr/wm/desktop` on purpose**: the
  desktop only re-reads when that directory changed and a cached read
  never reaches the drive, so without it the tool does almost no disk
  I/O -- verified by disarming the VFS preemption guard entirely and
  still getting four clean rounds.
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
  from the text itself. **It checks the BUILD ID first and refuses to
  be quietly wrong**: resolving against a different build gives
  confident, plausible, wrong names -- verified, the address that was
  `try_merge_next` in one report is `rtc_read_local` a few commits
  later. `--elf` points it at a userland ELF for a ring-3 crash.
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
- **`uterm_test.py`** -- drives the RING-3 Terminal. Its key check is
  worth copying elsewhere: it distinguishes a BUILTIN (`echo hi`,
  handled inside the shell with no spawn) from an EXTERNAL program
  (`lscpu`, dozens of lines that can only appear if it was spawned and
  its stdout piped back) by INK VOLUME. A terminal that echoed commands
  but never captured output passes every other check and fails that
  one.
- **`notepad_client_test.py`** -- drives the RING-3 Notepad and asserts
  a full round trip: type, save, verify the bytes on disk via `cat` (a
  completely independent path -- the editor claiming success proves
  nothing), clear, reopen, and require the rendered text to match pixel
  for pixel. Two traps it encodes: `ls`'s output on this console is
  interleaved with kernel log lines, so parsing it needs a strict
  entry-shaped regex rather than `split()[-1]`; and a reference
  screenshot must park the caret first, since `load_file()` resets the
  cursor to 0 and a caret bar is a real pixel difference.
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
  it after touching `userland/wm/wm_client.c`, `kernel/proc/win_server.c`,
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
- **`blank_window_test.py`** -- opens EVERY app in the registry and
  requires its window to contain more than a flat fill. Reads the app
  list from the KERNEL (`gui apps`), so an app added tomorrow is covered
  without editing it. Exists because UI Demo shipped completely blank
  and a 35-check suite passed it: every check asserted on the app's LOG,
  and the widgets were live, hit-testable and simply never painted. In
  `gui_regress.py`.
- **`live_boot_test.py`** -- boots `toy-os-live.iso` with NO disk and
  asserts a shipped binary RUNS, plus that `df` reports the image's real
  size and says RAM-only. Not in `gui_regress.py` (it builds its own
  QEMU); run it after touching the block layer, TFS3's geometry or the
  live path. **"It booted" proves nothing here** -- the kernel degrades
  to an empty RAM filesystem and still reaches a shell and a desktop.
- **`demo_test.py`** -- boots `toy-os-demo.iso` and asserts the scripted
  tour actually PERFORMS (6 checks). **On demand only** -- do not add it
  to `preflight.sh`, `gui_regress.py` or CI (standing request: it boots
  its own ISO and the demo is a showpiece, not something an ordinary
  change breaks). Reach for it when the tour is suspect, or after
  touching `apps/demo.c`, `data/wm/demo.script` or shell dispatch/init.
  Its load-bearing check is that a PATH-RESOLVED command really reached
  `elf_run` -- the other five stayed green through a real shipped bug
  where every PATH lookup in the tour failed, because "it booted,
  reached the desktop and opened windows" is satisfied by a tour whose
  every command failed. See `docs/decisions.md`.
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
  `win_server.c`'s compositor registration, or the `WIN_EV_RAW_*`
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
  if sbrk handed over a full screen of back buffer. In
  `gui_regress.py`.
- **`desktop_entries_test.py`** -- the `.desktop` entry system: the
  `ShowIn=` key and live reload, 13 checks. Its reusable lesson is in the
  ShowIn checks: they assert an entry is **LOADED but filtered** (`gui
  apps` versus `gui menu`), never just "absent from the menu" -- the
  first version asserted only absence and passed with the filter
  disabled outright, because `write` truncates and the check was racing
  the transient invalid file. In `gui_regress.py`.
- **`cpanel_test.py`** -- the ring-3 Control Panel and, through it, the
  settings registry (14 checks). Run it after touching
  `kernel/lib/setting.c`, `SYS_SETTING`/`SYS_SYSINFO`, or
  `uui_listbox`/`uui_radio_list`/`uui_statusbar`/`uui_layout`'s `hidden`
  handling. Two things it encodes. A change is verified by reading the
  BYTES ON DISK through the console's own `sh cat`, not by believing the
  app -- and note `/bin/config get` does NOT work for this, because a
  spawned program's stdout goes to its parent's pipe rather than the
  kernel log (only stderr is readable from outside). And its
  hidden-page check measures the SAME RECT in both states: the first
  version compared the widget's band against the WHOLE page's ink, a
  baseline so much larger that a positive control (making the layout
  ignore `hidden` again) reddened nothing at all. Recorded numbers:
  55% of the shown ink survives when hidden works, 98% when it does not.
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
- **`taskmgr_test.py`** -- the ring-3 Task Manager: `uui_table`, resize
  reflow, and ending a process (12 checks). Its resize check asserts the
  table grew by ROUGHLY WHAT THE WINDOW GREW BY, not merely that it
  changed -- the bug it was written after grew the width correctly and
  the height by 16 px against 300, so "it changed" was satisfied. On its
  first run it found a pre-existing bug in `uui_listbox` (see the
  widget-`hit` trap in the widget section above).
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
  pass/fail table (~1.5 minutes, ~300 checks across 23 tools). This is the standard check
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
  It runs **half the host's cores' worth of tools at a time**, capped at
  8 (`-j N` to change, `-j1` for the old serial behaviour -- that took
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
  assumed, because the obvious answers were both wrong.** A full run is
  ~425 tool-seconds across 24 tools, so the wall clock is the SLOWEST
  SINGLE TOOL, not the total: at `-j4` it was 76s and at `-j8` 72s,
  because both are pinned by the same one tool. **KVM (`--kvm`) buys
  almost nothing either** -- 64s -- since what the slow tools spend
  their time on is WAITING for real timeouts, which no amount of guest
  CPU shortens. The lever that worked was cutting the floor itself:
  `forcequit_test.py` waits out the WM's not-responding timeout about
  ten times, so shortening that timeout for its run (`gui pingtimeout`)
  took it from 72s to 34s and the whole suite from 76s to 61s. The next
  floor is `notepad` at 42s. The general lesson is worth more than the
  seconds: **on a fan-out like this, look at the maximum, never the
  sum** -- parallelism and faster guests both act on the sum.

  `damage_sweep.py` is deliberately NOT in it (much slower under
  `gui damage verify on`, and it has its own `--positive-control`
  protocol) -- run that separately.
- **`uapp_test.py`** -- the TWP resize handshake and focus events, via
  `winclient` (which contains no resize code -- it sets
  `.flags = UAPP_RESIZABLE` and nothing else, so what is under test is
  Toykit's and TWS's). 8 checks. Its focus check is a ROUND TRIP:
  capture a Terminal's content focused, take focus away and require it
  to CHANGE, give focus back and require it to match the first capture
  EXACTLY -- "it changed" alone is satisfied by almost anything.
- **`damage_sweep.py`** -- drives the WM through the interactions that
  historically break the damage invariant with `gui damage verify on`,
  and exits non-zero on a violation. Run it after touching anything
  that draws, damages, focuses or changes window chrome. `--random N
  --seed S` adds a seeded random walk (the seed prints on every run, so
  a failure replays exactly); `--positive-control` inverts the exit
  code, for proving the harness detects a real violation before
  trusting a clean run -- a clean sweep otherwise can't be told apart
  from a sweep that isn't checking anything, which has happened here
  for real.
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
- **`tfs2_writer.py`** -- host-side TFS2 v3 read/write tool: get files
  onto (or off of) `disk.img` without booting toy-os. **`trim` returns
  every free block's space to the host** by punching holes through them
  -- run it if `du disk.img` ever looks large. The image is sparse when
  created and only ever loses that: a block written once stays allocated
  on the host even after toy-os deletes the file that owned it, and the
  dev image had reached 8.1 GiB actually allocated against 2.3 MiB in
  use before this existed. The kernel issues ATA TRIM as it frees blocks
  now (`ata_trim()`, plus `discard=unmap` on every `-drive` line), which
  stops new images getting there; `trim` is for images already in that
  state, and for the host-side seeding path, which never boots the
  kernel. Non-destructive: only blocks the filesystem already considers
  free are touched. `format`
  initializes a blank/foreign image as an empty TFS2 v3 filesystem --
  note that running it on a BLANK image opts that image out of the
  TFS3 default; that's `seed_disk.py`'s job to decide, not a thing to
  do casually; `write`/`read`
  for a single file; `ls` for a directory listing; `sync <seed-dir>` to
  mirror a whole seed tree in (`once/` = copy-once, `sync/` =
  content-hash-synced -- see its own docstring and
  `docs/decisions.md`). `write`/`sync` auto-format a blank image first
  (no-op if already formatted), so a completely fresh `disk.img` can be
  seeded in one call with no toy-os boot in between -- the Makefile's
  `seed` target (runs on every `make iso`) goes through
  `tools/seed_disk.py` now, which delegates here only when the image's
  magic says TFS2 (a fresh/blank image gets TFS3 -- see
  `tfs3_writer.py` below). This replaced the old boot-time
  `BIN_BOOTSTRAP`/GRUB-module install (removed from `kernel.c`/
  `grub.cfg` -- see `docs/decisions.md`). Writes in-place by default;
  `--dry-run` on `write`/`sync`/`format` previews without touching the
  image. `delete`/`mkdir`/`cp` manage paths inside the image, so test
  state can be set up and cleaned up entirely from the host rather than
  booting toy-os to type `rm`. Scoped to direct+single-indirect blocks (~4.03 MB/file) -- see
  `docs/decisions.md` for why. `corrupt` injects a KNOWN inconsistency
  (`--leak N`, `--free-referenced N`, `--bad-pointer PATH`) so the
  kernel's `fsck` can be tested against damage whose exact shape is
  known in advance, and `--stage-journal PATH` (+ `--stage-journal-torn`)
  leaves an image in the state a crash mid-`persist_record()` produces,
  which is the only way to exercise `replay_journal()` without an actual
  power loss -- the inconsistencies `fsck` repairs are ones the
  kernel deliberately avoids producing, so without this it could only
  ever be tested against a clean disk and proven to report "clean".
- **`tfs3_writer.py`** -- the TFS3 sibling of `tfs2_writer.py`: format
  (writes superblock backups + GDT snapshots, wipes a stale TFS2
  signature per the wipefs rule, keeps images sparse by skipping/
  hole-punching the zeroed inode tables) / ls / read / write / mkdir /
  delete / sync (`once/` + `sync/` convention) / trim / info /
  corrupt (`--leak`, `--free-referenced`, `--bad-link-count`,
  `--smash-superblock`, `--stage-journal[-torn]` -- known damage for
  fsck/backup/journal-replay testing, same reasoning as
  tfs2_writer's). Spec: `docs/tfs3-spec.md`; the kernel backend
  (`kernel/fs/tfs3.c`) is kept in lockstep and the same bar applies
  as tfs2_writer's: direct+single-indirect write scope only.
  **`format --fs-version {1,2}`** picks the on-disk layout: v2 (32
  journal slots, GDT at 42, group 0 at 58) is the default and what a
  fresh image gets; v1 (four slots, GDT at 14, group 0 at 30) exists so
  the layout the kernel still mounts stays PRODUCIBLE and therefore
  testable -- `tools/tfs3_v1_test.py` is its caller.
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
- **`tfs3_v1_test.py`** -- boots a freshly built TFS3 **v1** image and
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
