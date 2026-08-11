# Changelog

All notable changes to toy-os, in the order they happened. Each entry
notes what was added and, where relevant, what broke and how it got
fixed -- several of the more interesting bugs here were only found by
actually testing in QEMU rather than assumed to work.

Entries above `## Build 502` (below) keep their old "Build N
(tier, +delta)" headings for reference -- that history isn't
rewritten. From here forward, changes accumulate under
`## [Unreleased]` instead, in the [Keep a Changelog](https://keepachangelog.com/)
style: no per-change version bump, just entries appended as they
happen. When a real release is cut, `tools/set_version.sh <version>`
stamps this section with the version and date and opens a fresh empty
one above it. See `docs/decisions.md` for the switch and why.

Earlier history (Milestone 1 through Build 172) lives in
`CHANGELOG-archive.md` -- moved there once this file passed ~4,200
lines, following the split-by-era plan stated above. Same content,
same grep-ability, just not all in one file that keeps growing
forever.

## [Unreleased]

### Changed
- Versioning switched from a per-change build-number scheme
  (`tools/bump_build.sh <fix|feature|major>`, a git tag `build-N` on
  every push) to semantic versioning with a `-dev` suffix during
  development. `VERSION` (repo root) now holds a plain semver string
  -- `0.1.0-dev` to start -- read by `tools/gen_version.sh` into
  `kernel/include/version.h`/`TOYOS_VERSION` exactly like `BUILD_NUMBER`
  was before. `tools/bump_build.sh` is retired; `tools/set_version.sh
  <version>` replaces it, used only when starting a new dev round
  (`0.2.0-dev`) or cutting a real release (`0.2.0`, which also stamps
  this CHANGELOG section and opens a fresh `## [Unreleased]`). Git tags
  move from `build-N` per push to `vX.Y.Z` at real releases only.
  `about`/the GUI About window now show `toy-os v0.1.0-dev` instead of
  `toy-os build 502`. Requested directly, to stop needing a
  fix/feature/major judgment call and a tag on every small change --
  see `docs/decisions.md`'s entry on this for the full reasoning.
- Commit messages going forward list each changed/added file with a
  one-line note in the body (e.g. `kernel/drivers/keyboard.c - added
  SE layout remap`), so a commit is skimmable on GitHub without
  opening the full diff. No other workflow change -- still a direct
  push to `main`, same as before.

## Build 502 (fix, +1) -- CLAUDE.md/qmp_test.py: catch up on QMP keyboard gotchas, prep for a new chat

Asked to start a fresh chat (this one had gotten long) and update
CLAUDE.md/other docs first so the new session picks up where this one
left off, without carrying the full conversation history.

Build 501's testing surfaced two QMP-testing gotchas this repo's docs
hadn't caught up to yet: `CLAUDE.md`'s keyboard bullet still said "no
shift handling" for QMP keyboard input, which stopped being true the
moment `combo()` (multiple qcodes in one `send-key` call, pressed and
released together) got used to test shifted Nordic letters (Å/Ä/Ö);
and neither `CLAUDE.md` nor `tools/qmp_test.py`'s own docstring
mentioned that rapid `send_key()` calls with no delay between them can
silently drop keystrokes (previously only noted in a session's own
scratch notes -- see build 490's Notepad-textfield testing -- never
written down anywhere that survives between sessions).

- **`tools/qmp_test.py`**: new `QMPSession.combo()` method -- sends
  multiple qcodes as one simultaneous press/release, exactly a
  Shift/Ctrl/Alt combo (no separate "hold key down" primitive existed
  before this). Module docstring gains three gotchas: rapid
  `send_key()` keystroke drops, punctuation qcode names (`send_key()`
  takes qcode names like `bracket_left`/`semicolon`/`dot`, not the
  literal character), and `combo()`'s existence/use.
- **`CLAUDE.md`**: keyboard bullet corrected (shift combos work now,
  via `combo()`) and split out a new bullet for the rapid-keystroke-
  drop gotcha, so both survive into whatever session reads this file
  next rather than needing rediscovery.

No kernel/app code changes -- `make all` still builds clean (build 502
was cut so `version.h`/`about`/`dmesg` reflect the doc-only nature of
this change same as any other bump), no QEMU/QMP verification needed
beyond confirming `tools/qmp_test.py` still parses.

## Build 501 (feature, +10) -- Nordic keyboard layout + Å/Ä/Ö font glyphs

Asked to add Nordic keyboard/character support (Ä/Ö/Å). Researched the
keyboard driver, font rendering, and character-representation code
first (no prior "layout"/"locale"/"unicode"/"nordic" anywhere in the
repo): the keyboard driver was hardcoded US QWERTY (two flat 128-entry
scancode tables, no layout abstraction at all), the font covered only
ASCII 32-126, and -- the real landmine -- this build has no
`-funsigned-char`, so any codepoint >= 0x80 is negative as `char` and
was silently rejected by `gfx_draw_char()`'s range check and five
`key >= 32 && key < 127`-shaped "printable char" gates across `apps/`
and `userland/echo.c`. Presented three choices: encoding (Latin-1
single bytes vs. UTF-8), how the layout is selected (persisted `/etc`
setting + shell command vs. build-time-only vs. an AltGr/compose-key
approach), and whether to fix the signed-char landmine as part of this
same build. Went with: **Latin-1/ISO-8859-1** (Ä=0xC4, Ö=0xD6, Å=0xC5,
ä=0xE4, ö=0xF6, å=0xE5 as single bytes -- keeps every "1 char = 1 cell
= 1 glyph" assumption in `scrollback_cell`, `fs.h`, and the syscall ABI
intact, unlike UTF-8), **persisted `/etc` setting + shell command**
(`keyboard <us|se>`, same pattern as `timezone`/`fontsize`), and **yes,
fix the landmine now** (Nordic letters don't actually work end-to-end
otherwise).

- **`tools/genttf.py`**: bakes 6 extra glyphs (`EXTRA_CHARS`) after the
  contiguous ASCII block -- `FONT_TTF_GLYPH_COUNT` 95 -> 101,
  `font_ttf_extra_codepoints[]` records which Latin-1 codepoint each
  extra glyph is, in baked order. Re-run against the same
  `/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf`
  this script already used (JetBrains Mono includes Nordic letters).
- **`kernel/drivers/gfx.c`**: new `font_ttf_glyph_index()` maps a
  codepoint to its glyph slot (ASCII via `c - 32`, the 6 extras via a
  linear scan of `font_ttf_extra_codepoints[]`) -- `gfx_draw_char()`
  now takes `c` through `(unsigned char)` before this lookup instead of
  comparing the signed `char` directly, fixing the signed-char landmine
  at its root.
- **`kernel/include/keyboard.h`**: `CHAR_*`/`IS_NORDIC_CHAR()`/
  `IS_PRINTABLE_KEY()` macros (the shared "is this a printable
  character, including Nordic letters" gate every app below now uses
  instead of a bare range check), `enum keyboard_layout`
  (`KB_LAYOUT_US`/`KB_LAYOUT_SE`), `keyboard_set_layout()`/
  `keyboard_get_layout()`/`keyboard_layout_name()`.
- **`kernel/drivers/keyboard.c`**: `scancode_ascii_se[]`/
  `scancode_ascii_shift_se[]` -- copies of the US tables with only
  scancodes 0x1A/0x27/0x28 (the physical keys under Å/Ä/Ö on a real
  Swedish/Finnish keyboard) remapped; `keyboard_feed_byte()` picks the
  active pair by `current_layout`. `keyboard_read_line()`'s own
  `c >= 128` skip-special-keys guard widened to let `IS_NORDIC_CHAR()`
  through (it's in the same codepoint range as the `KEY_*` special
  codes it's meant to filter, just not the same values).
- **`kernel/include/keyboard_config.h`** / **`kernel/core/keyboard_config.c`**
  (new): persistence layer, same split as `tz.c`/`font_config.c` --
  `keyboard_config_init()` loads `/etc/toyos.conf`'s
  `keyboard_layout=<us|se>` key at boot, `keyboard_config_save()`
  writes it. Wired into `kernel_main()` after `font_config_init()`.
- **`apps/shell_sys.c`**: new `cmd_keyboard()` (`keyboard` alone shows
  the current layout, `keyboard <us|se>` sets + persists it), help text
  entry.
- **`apps/shell.c`**, **`apps/shell_internal.h`**: `keyboard` dispatch
  entry + declaration.
- **`kernel/include/kapi.h`**: added `keyboard_config.h` to the apps/
  boundary.
- **Signed-char landmine fixed at all six call sites**, not five --
  `apps/terminal.c`, `apps/notepad.c`, `apps/widgets.c`,
  `apps/editor.c` now use `IS_PRINTABLE_KEY()`; `userland/echo.c` (a
  freestanding ring-3 program with no kernel headers) keeps its own
  copy of the same check. The sixth site, **`apps/shell.c`'s own
  `shell_read_line()`**, had a *differently-worded* gate
  (`c < 128`, not `key >= 32 && key < 127`) that grepping for the other
  five's exact phrasing missed entirely -- found only by QMP-testing
  actual keystrokes in `se` mode and noticing the console cursor didn't
  even advance when a Nordic letter was typed at the shell prompt, not
  by code review. Worth remembering next time a "fixed every instance
  of X" claim needs verifying: test the behavior, don't just re-grep
  the pattern already fixed. Also fixed once found.

Verified via QMP: `make clean && make all && make iso` clean,
`boot_smoke_test.py` PASS. `keyboard se` + typing the 3 remapped
physical keys unshifted/shifted renders `åöä ÅÖÄ` correctly at the
shell prompt (screenshot); `keyboard us` afterward regresses correctly
back to `[;'`. `keyboard se` + `reboot` + `keyboard` (no args) shows
`se` after the reboot -- persistence confirmed, and `cat
etc/toyos.conf` shows `keyboard_layout=se`. In GUI mode: typed
`aåöäÅÖÄ` into Notepad's scrollback body, and a Nordic-lettered
filename into its text-field widget (`noåtattxt`) -- Saved, then
`ls`/`cat` from the shell confirm the file exists with that exact name
and its exact content round-tripped through TFS2 byte-for-byte.
`filetest`/`sockettest` regression-checked clean (unrelated to this
change, but touch shared syscall/console paths). Screenshots delivered
and saved to `screenshots/2026-08-10/`.

## Build 491 (fix, +1) -- README: what disk-hosted ELF binaries (starting with lspci) would require

Asked how to support running ELF binaries from a `/bin` directory on
the persistent filesystem, starting with `lspci`, and whether anything
needs to happen first -- as a "plan it, don't build it yet" question.
First cleared up a terminology mix-up (asked "EFI binaries" -- toy-os
has always used ELF64, not UEFI/PE; the two are unrelated formats and
boot models, confirmed with the user before researching further).

Researched the codebase: every existing `.elf` (`hello.elf`,
`file_test.elf`, etc.) is a GRUB Multiboot2 module baked into the ISO
and found by a hardcoded module index (`multiboot.c`'s
`multiboot_get_module()`) -- no code path anywhere reads an ELF's
bytes via `fs_read()` and executes them. `lspci` itself is a plain
kernel-space shell built-in (`cmd_lspci()`, `apps/shell_sys.c`), not a
process. Identified this as two separable capabilities worth landing
as two builds: (A) a real syscall-based ELF program launched the
existing GRUB-module way -- mechanically easy (`elf_load()`/
`process_run_ring3()` already don't care where the blob came from),
but needs new PCI syscalls added to `syscall_abi.h` since a ring-3
process can only reach the kernel through `int 0x80`, not by calling
`pci_device_at()` directly; and (B) actually loading from `/bin` on
disk at runtime, which needs TFS2 to support files bigger than its
current 2048-byte (`FS_DATA_MAX`) cap (every existing test ELF already
brushes or exceeds it), a `fs_read()`-sourced load path, a `/bin` +
`run` convention, and a way to get a built ELF onto `disk.img` at all
(no in-guest compiler).

Presented two choices: how far to build this session (plan only vs.
one build vs. two separate builds), and how TFS2 should store files
bigger than 2048 bytes. Went with: **plan only** (write up the
research and phased approach, implement in a future session), and
**multi-slot chaining for large files only** -- an ordinary small file
keeps today's exact 2048-byte/one-slot cost (both on disk and in the
static in-RAM `files[FS_MAX_FILES]` table), a file that needs more
spans multiple slots via a chain, rather than growing `FS_DATA_MAX`
for all 32 slots regardless of use (would multiply the table's static
RAM cost by the same factor for every file, not just big ones) or
adding a wholly separate fixed-size table just for binaries.

- **`README.md`**: new entry in **Ideas for what's next** capturing
  the (A)/(B) split, the syscall gap, the TFS2 storage decision and
  why the other two options were passed over, and everything else (B)
  would still need (a `fs_read()`-sourced load path with a scratch-
  buffer copy since TFS2's in-RAM pointers aren't as stable as a GRUB
  module's reserved region, a `/bin` + `run` convention, and a way to
  install a built ELF onto `disk.img` with no in-guest compiler).

No code changes -- docs only, no build/QEMU verification needed.

## Build 490 (feature, +10) -- widgets.h: text field + checkbox, wired into Notepad's filename

Asked to add more reusable widgets so GUI apps stop hand-rolling the
same UI pieces (per the project's own standing instruction to build
new/old features as widgets when that's the better choice, and to ask
first). Investigation before proposing choices found the README's
"Notepad's Save/Load buttons and its toolbar are hand-rolled" note was
already stale -- `widget_button`/`widget_hit` already back Notepad's
toolbar, Calculator's grid, and wm.c's title-bar buttons, and the
scrollback/scrollbar widgets already back Notepad/Terminal's scrolling
text. The one real, already-identified gap was a text input field
(README explicitly named Notepad's fixed filename as the reason one's
needed). Presented two choices: scope (text field alone vs. text field
+ checkbox vs. a full hand-rolled-UI audit across every app first) and
whether to wire the new field into a real caller immediately. Went
with: **text field + checkbox** (the checkbox built explicitly ahead
of any real caller, by direct request -- see `docs/decisions.md`'s new
entry, an acknowledged one-off exception to this file's normal
"only once a second caller needs it" rule), and **wire the text field
into Notepad's filename right away** rather than leave it unused.

- **`apps/widgets.h`** / **`apps/widgets.c`**: new `struct text_field`
  (`widget_textfield_init/set_active/key/draw`) -- fixed 48-byte
  buffer, cursor, an explicit `active` flag the widget itself never
  changes (the owning app decides when to (de)activate it); handles
  printable-ASCII insert, backspace/delete, left/right/home/end,
  leaves Enter (and everything else) unhandled/uncommitted so the
  caller decides what Enter means. New `widget_checkbox_width/draw/hit`
  -- box + optional label, checked/unchecked, no group/exclusivity
  logic (that's a radio-button concept, not this).
- **`apps/notepad.c`**: replaced the fixed `"notepad.txt"` with an
  editable `struct text_field` in the toolbar -- clicking it activates
  editing; Enter, or a click anywhere else in the window, deactivates
  it. Save/Load now read/write whatever's in the field instead of a
  constant. Save now actually checks `fs_write()`'s return value
  (previously always reported "Saved." even on failure) and refuses an
  empty filename outright ("Bad filename.") rather than trying
  `fs_write("", ...)`.

Verified: `make clean && make all && make iso` clean, zero warnings
introduced; `tools/boot_smoke_test.py` passes. Full QMP GUI test:
opened Notepad from the Start menu, clicked the filename field
(caret appeared), cleared "notepad.txt" and typed "mynote.txt", clicked
into the text area (field's caret correctly disappeared, confirming
deactivation-on-click-elsewhere), typed "hello widgets", clicked Save
(status showed "Saved."), then dropped back to the shell and
independently confirmed via `cat mynote.txt`/`stat mynote.txt` that
the file was written under the new name with the right content and a
fresh timestamp -- not just that the GUI *looked* right, the actual
disk state was checked through a completely separate code path.
Screenshots in `screenshots/2026-08-10/` (`widgets-*`).

## Build 480 (feature, +10) -- TFS2: journaling + timestamps for the persistent filesystem

Asked to add journaling and timestamps ("date codes") to the
persistent filesystem, plus a name for it, and a spec doc so a
separate Linux app could browse a disk image. Presented three choices
up front: a name (TFS2 vs. more evocative alternatives), a journaling
design (write-ahead log vs. shadow/double-buffer vs. minimal
commit-flag-only), and which timestamp fields to track (created+
modified vs. modified-only vs. created+modified+accessed). Went with:
**TFS2** (keeps the existing "TFS" lineage, version-bumped); a
**write-ahead log with a single journal slot** (real crash recovery,
not just torn-write detection -- see `docs/decisions.md`, and cheap
here since every mutating call only ever touches one table slot, so
one journal slot is always enough); **created + modified** timestamps
(not accessed -- an access timestamp would mean even `cat`/browsing
triggers a disk write, which cuts against the toy nature of this
project). No backward compatibility with "TFS1" (the pre-journal,
pre-timestamp format) or the flat pre-directories format before that --
an old disk is detected via the superblock magic/version and
reformatted fresh, same policy those formats already used against
each other.

- **`kernel/drivers/tfs.c`**: the bulk of the change. New on-disk
  layout -- superblock magic bumped `"TFS1"` -> `"TFS2"` (version
  reset to 1, tracking TFS2's own future revisions); a journal region
  (LBA 1: header sector -- magic/commit-flag/target-slot/checksum;
  LBA 2-6: one record's worth of staged data) sits between the
  superblock and the table, which now starts at LBA 7 instead of LBA
  1. `persist_record()` is now a 4-step write-ahead sequence (stage in
  the journal -> commit via a single-sector header write -> apply to
  the real table slot -> clear the journal) instead of one direct
  write; a new `replay_journal()`, called from `tfs_init()` before the
  table load, finishes (or discards, via an FNV-1a checksum) whatever
  was left pending by an unclean shutdown. Every record grew two new
  fields, `created`/`modified` (`struct rtc_time`, `timer.h` --
  set via `tz.c`'s `rtc_read_local()`, the same local-time source
  `SYS_GETTIME`/`time` use) -- didn't need to grow `FS_RECORD_SECTORS`
  (still 5 sectors/2560 bytes, there was headroom). `tfs_touch()`/
  `tfs_mkdir()` set both on a genuinely new entry (touching an
  existing file stays a no-op, unchanged); `tfs_write()` bumps
  `modified` on every real content change. New `tfs_stat()` backs the
  new `fs_stat()` API.
- **`kernel/include/fs.h`**: new `struct fs_timestamps { struct
  rtc_time created, modified; }` and `int fs_stat(const char *path,
  struct fs_timestamps *out)`.
- **`kernel/include/fs_ops.h`** / **`kernel/drivers/vfs.c`**: new
  `.stat` vtable entry / `fs_stat()` dispatch wrapper, same shape as
  every other `fs_*` call.
- **`apps/shell_fs.c`** / **`apps/shell_internal.h`** / `apps/shell.c`:
  new `stat <path>` shell command (type, size, created/modified) --
  registered in `help`'s output too (`apps/shell_sys.c`).
- **`docs/tfs2-spec.md`** (new): byte-exact on-disk format spec for a
  host-side reader -- disk layout table, superblock/journal/record
  field offsets, `rtc_time` encoding, path/directory semantics,
  explicit guidance for what a *browsing* tool should (and shouldn't)
  do with the journal region, and a reference read-only Python parser.
  The reference parser was run against a real TFS2 image produced by
  this build's own QMP testing and correctly listed every file/
  directory with matching sizes and timestamps -- not just written
  against the spec, actually verified byte-accurate.

Verified: `make clean && make all && make iso` clean, zero warnings
introduced; `tools/boot_smoke_test.py` passes. Full QMP round trip:
fresh disk -> `mkdir`/`write`/`stat` (timestamps shown correctly) ->
clean QMP `"quit"` -> reboot with the same disk -> `"fs: loaded
persistent filesystem from disk"` (no replay message, confirming a
clean shutdown leaves nothing pending) -> `cat`/`stat`/`ls` all show
identical content and timestamps to before the reboot. Also re-ran
`filetest`/`sockettest` (the syscall path, which round-trips through
the same new `tfs_write()`/`tfs_read()`) post-reboot with no
regressions. `docs/tfs2-spec.md`'s reference Python parser was
extracted from the doc itself and run against the same test image,
correctly listing `/etc`, `/etc/timezones`, `/docs`, `/docs/notes.txt`,
and `/filetest.txt` with matching sizes/timestamps. Screenshots in
`screenshots/2026-08-10/` (`tfs2-*`).

## Build 470 (major, +50) -- IRQ-driven Bus-Master DMA for ata.c, plus two real bugs it exposed

Asked to improve `ata.c` specifically because it was called out (build
390's README wording) as a reasonable structural template that still
didn't cover IRQ- or DMA-driven I/O -- the next disk-side milestone
alongside the ongoing TCP/IP-prerequisite chain (builds 390/400/410/
420: PCI enumeration, IRQ registration, contiguous/DMA memory,
socket-fd syscalls). Presented two choices up front: scope (docs only
vs. DMA-with-polling vs. IRQ-only vs. both IRQ-driven completion and
Bus-Master DMA transfer) and DMA-unavailable fallback (fail closed vs.
fall back to the existing PIO path). Went with: both (a real
IRQ-driven Bus-Master DMA path, not a polling shortcut), falling back
to PIO automatically whenever DMA can't be stood up (no PCI IDE
controller found, BAR4 isn't I/O-space, or the contiguous 2-frame
allocation fails) -- `ata_read_sector()`/`ata_write_sector()`'s public
signatures never changed, so `fs.c`/`tfs.c` needed zero edits either
way.

Implementing this surfaced two real, independent bugs -- both integral
to actually making the feature work, not separate fixes:

- **The `g_next_kernel_rsp` reentrancy hazard, hit for real.** A naive
  `hlt`-until-IRQ wait hung forever on every `SYS_WRITE`/`SYS_READ`
  syscall (`filetest` froze mid-run): `int 0x80` is wired as an
  interrupt gate (`idt_set_gate(128, isr128, 0, 0xEE)`), which clears
  IF for the whole syscall, so no interrupt -- not even the timer --
  could ever fire to wake it. Naively adding `sti` before the wait
  would have reintroduced the exact reentrancy bug already documented
  (and previously hit and abandoned) in `syscall.c`'s own
  `SYS_READ_KEY` comment: `isr_dispatch()`'s epilogue does
  `mov rsp, [rel g_next_kernel_rsp]` unconditionally before every
  `iretq`, so a nested interrupt firing mid-syscall would corrupt the
  outer handler's resume point. Stopped and presented three options
  rather than picking one silently; went with a context-aware hybrid:
  a new `g_isr_depth` counter (`kernel/core/idt.c`, exposed as
  `isr_in_progress()`/`isr_reset_depth()` via `kernel/include/idt.h`)
  tracks whether the CPU is currently inside any interrupt handler.
  `ata.c`'s `wait_dma_irq()` genuinely blocks via `hlt` when it's
  safe (the common case -- most real disk I/O in this OS is `apps/`
  code calling `fs_write()`/`fs_read()` directly from kernel space,
  never inside an interrupt), and falls back to bounded polling of the
  Bus-Master status register's own IRQ bit when called from inside a
  syscall (the hardware still raises that bit regardless of the CPU's
  interrupt-enable state -- IF only gates whether the CPU *services*
  an IRQ, not whether the chipset sets the status bit). The depth
  counter is forcibly reset to 0 in `process_run_ring3()`'s
  longjmp-style resume branch (`kernel/core/process.c`) -- the one
  point in this kernel where "definitely not inside any interrupt" is
  guaranteed true, since every caller of that function is plain
  kernel-space code (confirmed via grep), never itself nested in an
  interrupt -- which is what makes it safe to force the counter back
  to 0 there instead of trusting now-unreachable decrements on the
  abandoned call stack.
- **PCI Bus Master Enable never set -- DMA "succeeded" while moving no
  real data.** Every transfer reported success (`bm_status` showed the
  IRQ bit set, no error bit) yet `disk.img` stayed all-zeros even
  after a clean shutdown. Root-caused by careful isolation: confirmed
  PIO-only writes DID persist (ruling out a QEMU/methodology issue),
  then host-side pre-seeded `disk.img` with a valid superblock and
  traced the DMA read's bounce buffer -- it held stale/garbage memory,
  not the real on-disk bytes, proving no actual bus cycle ever
  happened despite the hardware status registers reporting completion.
  The classic, easy-to-miss cause: the PCI Command register's "Bus
  Master Enable" bit (config offset 0x04, bit 2) was never set for the
  IDE controller -- without it, a PCI device's I/O-mapped DMA control
  registers keep accepting reads/writes and can still report nominal
  success, but the device never issues real memory read/write cycles.
  Fixed with a new `pci_enable_bus_master()` (`kernel/drivers/pci.c`/
  `kernel/include/pci.h`, plus a `config_write16()` read-modify-write
  helper alongside the existing `config_read16()`), called from
  `ata_init_dma()` right after the contiguous allocation succeeds.
  Explicitly noted in `pci.h`'s own doc comment as a call any future
  DMA-capable driver -- a NIC, chiefly -- will need too.

- **`kernel/include/ata.h`** / **`kernel/drivers/ata.c`**: rewritten
  around a dual-path design -- `ata_init()` now calls a new
  `ata_init_dma()` after confirming a drive is present, which looks
  for the IDE controller on the PCI bus (class 0x01, subclass 0x01),
  checks BAR4 is a usable I/O BAR, allocates a contiguous 2-frame
  block via build 410's `pmm_alloc_contiguous(2)` (one frame for a
  single-entry PRD table, one as a bounce buffer -- every transfer
  here is exactly one 512-byte sector, so one PRD entry is always
  enough), calls `pci_enable_bus_master()`, registers `ata_irq_handler`
  for IRQ14 via build 400's `irq_register_handler()`, and unmasks it.
  `dma_transfer()` programs the PRDT, issues `CMD_READ_DMA`/
  `CMD_WRITE_DMA`, starts the bus master, waits via `wait_dma_irq()`,
  then stops the engine and clears status regardless of outcome. The
  original PIO implementation is untouched, renamed to
  `pio_read_sector()`/`pio_write_sector()`, and used automatically
  whenever `g_dma_available` is 0. New `ata_dma_active()` diagnostic
  getter. Public API (`ata_present()`/`ata_read_sector()`/
  `ata_write_sector()`) unchanged.
- **`kernel/include/idt.h`** / **`kernel/core/idt.c`**: new
  `isr_in_progress()`/`isr_reset_depth()`, backed by a `g_isr_depth`
  counter incremented at the top of `isr_dispatch()` and decremented
  at every normal-return path.
- **`kernel/core/process.c`**: `process_run_ring3()`'s resume branch
  calls `isr_reset_depth()` -- see the reentrancy writeup above.
- **`kernel/drivers/pci.c`** / **`kernel/include/pci.h`**: new
  `pci_enable_bus_master()` (and a `config_write16()` helper).

Verified: `make clean && make all && make iso` clean, zero warnings
introduced; `tools/boot_smoke_test.py` passes. Full QMP end-to-end
persistence test against a real disk image (not just the in-memory
`tfs.c` cache, which proves nothing about real disk I/O on its own):
booted with a fresh disk, wrote a file from the kernel-space shell
path (genuine `hlt`-blocking DMA), clean QMP `"quit"` shutdown,
rebooted with the *same* disk image, confirmed
`"fs: loaded persistent filesystem from disk"` (not "formatted a
fresh...") and that the written file's content read back correctly --
proof the fix moves real bytes, not just that status registers look
happy. Also ran `filetest` and `sockettest`/`newsyscalltest` (the
syscall/polling-DMA path) post-reboot, all passing with no hang and no
regression. Screenshots in `screenshots/2026-08-10/`.

## Build 420 (feature, +10) -- socket-fd abstraction + SYS_SOCKET/SYS_SEND/SYS_RECV

Fourth milestone toward TCP/IP networking (see build 380's README
entry, build 390's PCI enumeration, build 400's IRQ registration, build
410's contiguous memory) -- the last item on that list before an actual
NIC driver: `syscall.c`'s fd table was filesystem-only, with no way for
a socket to exist as a first-class fd. Presented three choices: build
scope (fd/syscall surface only vs. also a real in-kernel loopback
transport vs. just the fd-table refactor with no new syscalls yet), fd
table shape (extend into a tagged union vs. a second parallel socket
table), and syscall API shape (a minimal SYS_SOCKET+SYS_SEND/SYS_RECV
now vs. the full BSD-style surface including SYS_CONNECT/SYS_BIND/
SYS_LISTEN/SYS_ACCEPT). Went with: fd/syscall surface only -- there's
still no NIC driver, so a real transport (even an in-kernel loopback
one) was judged premature; a tagged union in the existing fd table
(`FD_KIND_FILE`/`FD_KIND_SOCKET`), one shared fd namespace matching how
real Unix does it, rather than a second table to keep in sync; and the
minimal syscall set -- no connection semantics yet, since there's
nothing to connect to.

- **`kernel/include/syscall_abi.h`**: `SYS_SOCKET` (16, RDI = domain,
  RSI = type -- both reserved for future use, must be 0 for now,
  rejected otherwise), `SYS_SEND` (17, RDI = fd, RSI = buffer, RDX =
  length), `SYS_RECV` (18, same shape). `SYS_SEND`/`SYS_RECV` always
  return -1 for now -- no transport exists yet -- deliberately, not a
  bug; the ABI (which register holds what) is meant to not need a
  breaking change once a real NIC driver lands.
- **`kernel/core/syscall.c`**: `struct open_file` is now a tagged union
  -- `enum fd_kind { FD_KIND_FILE, FD_KIND_SOCKET }` plus a union of a
  `file` arm (the pre-existing name/mode/offset fields, untouched) and
  an empty `socket` arm (no real per-socket state yet). `SYS_SOCKET`
  allocates a slot the same way `SYS_OPEN` always has (first free `!used`
  slot), tagged `FD_KIND_SOCKET`. `SYS_WRITE`/`SYS_READ` now check
  `kind == FD_KIND_FILE` before touching `.file.*`, rejecting a socket
  fd with the same "bad fd" outcome as any other invalid fd -- proves
  the two kinds actually stay separate rather than a socket fd
  accidentally being treated as a file. `SYS_CLOSE` and
  `syscall_process_exit_cleanup()` needed zero changes -- both only
  ever looked at `used`/`owner_pml4`, so a socket fd already closes and
  gets reclaimed on process exit correctly, for free.
- **`userland/socket_test.c`** (new) / **`kernel/core/socket_test.c`**
  (new) / **`kernel/include/socket_test.h`** (new) / `sockettest` shell
  command (`apps/shell.c`, `apps/shell_sys.c`) / thirteenth GRUB module
  (`grub.cfg`, `Makefile`): a ring-3 test program proving the surface,
  not real data transfer -- `SYS_SOCKET(1, 0)` (nonzero domain)
  correctly rejected, `SYS_SOCKET(0, 0)` returns a real fd,
  `SYS_SEND`/`SYS_RECV` on it correctly return -1, `SYS_WRITE`/
  `SYS_READ` on it correctly get rejected, `SYS_CLOSE` succeeds, and
  reusing the closed fd afterward is correctly rejected. Exits 0 only
  if every one of those checks matched what's documented.

Verified: `make clean && make all && make iso` clean, `tools/
boot_smoke_test.py` PASS, and (via QMP, screenshots below) `sockettest`
itself passes every check with exit code 0 -- plus `filetest` and
`newsyscalltest` both still pass unchanged, confirming the fd-table
tagged-union refactor didn't disturb existing file-fd behavior.
Screenshots: `sockettest_all_checks_pass.png`,
`sockettest_filetest_regression_ok.png`,
`sockettest_newsyscalltest_regression_ok.png`.

## Build 410 (feature, +10) -- contiguous/DMA-friendly physical memory

Third milestone toward TCP/IP networking (see build 380's README entry,
build 390's PCI enumeration, build 400's IRQ registration) -- `pmm.c`'s
frame allocator only ever handed out one 4KB frame at a time, but NIC
descriptor rings want a handful of physically contiguous pages. The
kernel already identity-maps the low 4GB (`vmm.c`), so no address-
translation headache once contiguous frames exist -- just needed a
"give me N contiguous frames" allocator, which didn't exist yet.
Presented two choices: how `pmm` should find N contiguous frames (scan
the existing bitmap for a run of N free bits; carve out a small
dedicated always-contiguous region reserved at boot; or replace the
whole bitmap allocator with a buddy/segregated-free-list allocator),
and whether freeing a contiguous run should get its own symmetric
function or be left to the caller looping `pmm_free_frame()`. Went
with: a linear bitmap scan (no new data structure, and this only runs
rarely -- a driver setting up a descriptor ring once at init, not a hot
path); the buddy-allocator option flagged as a future improvement
instead of built now (see the new README.md entry below) -- nothing in
this kernel has exercised the bitmap enough yet to know fragmentation
is a real problem worth that added complexity; and a symmetric
`pmm_free_contiguous()`, so a caller frees a DMA buffer as the one
block it allocated instead of remembering to loop `pmm_free_frame()`
`count` times itself.

- **`kernel/include/pmm.h` / `kernel/core/pmm.c`**:
  `pmm_alloc_contiguous(count)` -- `count == 1` fast-paths straight to
  the existing `pmm_alloc_frame()`; for `count > 1`, a linear scan of
  the same bitmap `pmm_alloc_frame()` uses (always from frame 0, not
  `pmm_alloc_frame()`'s rolling `alloc_hint`, since this is a rare,
  not-hot-path call where simplicity wins over skipping already-scanned
  ground) looking for a run of `count` consecutive free bits, marking
  all of them used and updating `free_frames`/`alloc_hint` together;
  returns 0 if no run that long exists anywhere in the managed range.
  `pmm_free_contiguous(phys_addr, count)` mirrors `pmm_free_frame()`'s
  per-frame logic across `count` frames starting at `phys_addr` --
  frames pmm doesn't recognize as allocated are silently skipped, same
  as `pmm_free_frame()`.
- **`kernel/core/pmm.c` / `kernel/core/kernel.c`**: `pmm_selftest()`,
  called once from `kernel_main()` right after `pmm_init()`. No driver
  calls the new functions yet (the intended first caller is a future
  NIC descriptor ring), so without an explicit self-test a regression
  here would only be caught by reading the code, not by anything a boot
  actually exercises. Allocates a run of 4 frames, checks the returned
  address is 4KB-aligned and that all 4 underlying bitmap bits actually
  flipped to used (not just that a plausible-looking address came
  back), checks `free_frames` dropped by exactly 4, frees the run and
  checks the bits cleared and the count came back, then allocates a
  second run of 4 and checks it lands at the *same* address -- proof
  the free actually cleared those bits rather than the count just
  happening to be right. Logs a single `klog_write()` pass/fail line;
  cheap enough (a handful of frames, once) to leave in permanently
  rather than treat as throwaway.
- **`README.md`**: struck through the PCI-enumeration, IRQ-registration,
  and contiguous-memory sub-bullets under "Basic TCP/IP networking" in
  **Ideas for what's next** as done (all three of this build's
  predecessors plus this one), and added the buddy/segregated-free-list
  allocator as its own flagged-not-built future improvement within the
  contiguous-memory bullet.

Verified: `make clean && make all && make iso` clean, `tools/
boot_smoke_test.py` PASS, and the self-test's pass line
(`toy-os: PMM contiguous-allocation self-test passed`) present in the
serial log right after `toy-os: physical frame allocator initialized`,
confirming the new allocator actually works end-to-end at boot, not
just that it compiles.

## Build 400 (feature, +10) -- generic hardware-IRQ registration mechanism

Second milestone toward TCP/IP networking (see build 380's README
entry and build 390's PCI enumeration) -- a NIC needs its own promptly-
serviced IRQ line, and `isr_dispatch()`'s old hardcoded if/else chain
(timer/keyboard/mouse special-cased, every other IRQ silently EOI'd
and ignored) had no way for a new driver to plug in without adding yet
another one-off branch. Presented three choices on migration scope
(additive-only vs also migrate keyboard+mouse vs full migration
including the timer), EOI responsibility (framework-automatic vs each
handler's own), and table shape (single handler per IRQ vs a small
chain for line-sharing). Went with: full migration -- one uniform
dispatch path for every hardware IRQ, no special cases left at all;
automatic EOI (a forgotten EOI on a real IRQ line is a classic bug
that silently stops all further interrupts on that line, worth
removing the chance of); and a simple fixed 16-entry array, one
handler per IRQ, matching every other fixed-capacity table in this
kernel (`history[]`, `fd_table[]`, PCI's device list) -- QEMU's
topology gives every relevant device its own dedicated line (confirmed
by build 390's `lspci`), so IRQ-sharing/chaining isn't a case that
comes up here.

- **`kernel/include/irq.h` / `kernel/core/irq.c`** (new):
  `irq_register_handler(irq, handler)` / `irq_dispatch(irq, regs)` --
  a 16-entry table (`irq_handler_fn`, taking the same saved-register-
  block pointer `isr_dispatch()` gets, so the timer's handler can still
  hand off to `scheduler_tick()`). `irq_dispatch()` looks up and calls
  whatever's registered, then unconditionally sends the PIC EOI itself
  -- a no-op-but-still-EOIs for any IRQ nothing's registered for, same
  observable behavior an unhandled IRQ had before this existed.
- **`kernel/core/idt.c`**: `isr_dispatch()`'s three-branch hardcoded
  chain (`vector == 32`/`33`/`44`) plus the do-nothing-but-EOI fallback
  for everything else collapsed into one branch (`vector >= 32 &&
  vector < 48`) that just calls `irq_dispatch()`. `idt_init()` now
  registers three small wrapper functions -- `timer_irq_handler()`
  (still calls `pit_handle_irq()` + `scheduler_tick(regs)`, just from
  inside its own registered handler instead of a dispatch-level special
  case), `keyboard_irq_handler()`/`mouse_irq_handler()` (both still
  just `i8042_poll()`, since keyboard and mouse share the 8042 data
  port regardless of which IRQ fired) -- right where the existing
  `pic_clear_mask()` calls already wire up which lines are unmasked, so
  driver-owning code in `timer.c`/`i8042.c` didn't need to move or
  change at all.
- **`docs/decisions.md`**: new entry on why one-handler-per-IRQ (not a
  chain) and framework-automatic EOI were chosen.

Verified: `make clean && make all && make iso` clean, no new warnings;
`boot_smoke_test.py` passes, no panic. QMP session exercised all three
migrated paths end to end -- `uptime` run twice showed the tick counter
still advancing (timer IRQ), `schedtest` showed clean interleaved
`ABAB...` preemptive output and a normal exit (timer IRQ driving
`scheduler_tick()`, the most demanding test of the migration), typing
commands at the shell worked throughout (keyboard IRQ), and in GUI mode
the cursor tracked a `goto()` move correctly and a click opened the
Start menu (mouse IRQ, both motion and button state) -- confirming the
full migration didn't regress any of the three real subsystems it
touched.

## Build 390 (feature, +10) -- PCI bus enumeration + `lspci`

First real milestone toward TCP/IP networking (see build 380's README
entry on what that would take overall) -- PCI enumeration was called
out there as the self-contained first piece, since virtually no NIC
lives at a fixed legacy port the way ATA does. Presented three choices
on scan method (brute-force flat scan vs bus-0-only vs recursive
bridge-aware), how much per-device info to collect in this first pass
(IDs+class only vs full info including BARs/IRQ), and how to expose it
(diagnostic-only vs a real `lspci` shell command vs both). Went with:
brute-force flat scan (simplest, no bridge-topology logic needed, same
code path works on real hardware or QEMU); full info now (IDs, class,
BARs, IRQ) since the config-space read plumbing is already in hand;
and a real `lspci` command plus a `kapi.h` capability for future driver
code to call directly.

- **`kernel/include/io.h`**: new `outl()`/`inl()` (32-bit port I/O) --
  PCI config-space access (CONFIG_ADDRESS/CONFIG_DATA, ports 0xCF8/
  0xCFC) is defined in terms of 32-bit reads/writes; only 8-/16-bit
  variants existed before this (ATA's data register needed 16-bit,
  nothing needed 32-bit until now).
- **`kernel/include/pci.h` / `kernel/drivers/pci.c`** (new): `pci_init()`
  brute-force-scans every bus/device/function (256 x 32 x 8 = 65536
  config-space reads, each cheap) via legacy CONFIG_ADDRESS/
  CONFIG_DATA port I/O -- not the newer memory-mapped ECAM mechanism,
  which needs ACPI/MCFG table parsing just to locate and isn't needed
  for anything this kernel does. Records up to `PCI_MAX_DEVICES` (32)
  devices found: vendor/device ID, class/subclass/prog-if/revision,
  header type, interrupt line, and all 6 BARs (decoded only as far as
  "I/O or memory, and the base address" -- NOT size-probed, deferred
  to whichever future driver actually needs to map one). `pci_class_name()`
  gives a human-readable label for the class/subclass pairs an ordinary
  PC or QEMU machine actually presents (network/mass-storage/display/
  bridge/etc.), falling back to "unknown device" rather than trying to
  cover the full PCI class-code table.
- **`kernel/core/kernel.c`**: `pci_init()` called once, unconditionally,
  right after `pmm_init()` -- there's no natural lazy caller the way
  ATA has `tfs.c`'s `fs_init()`, since PCI enumeration has no dependent
  yet (the future NIC driver and `lspci` both just read what was
  already recorded at boot).
- **`kernel/include/kapi.h`**: new `#include "pci.h"`.
- **`apps/shell_sys.c`**: new `cmd_lspci()` -- lists every device found
  at boot in the traditional `bus:dev.func  vendor:device  class name`
  shape, plus IRQ line and any nonzero BARs. New `print_hex_digits()`
  helper (fixed-width lowercase hex, unlike `vga_write_hex()`'s
  arbitrary-width/leading-zero-trimmed output) so the vendor:device
  columns actually line up. Wired into `shell_internal.h`,
  `shell.c`'s `dispatch()`, and `HELP_LINES`.
- **`docs/decisions.md`**: new entry on why brute-force flat scanning
  (not bridge-aware recursion) was chosen, and why BARs are decoded
  but not size-probed at this stage.

Verified: `make clean && make all && make iso` clean, no new warnings;
`boot_smoke_test.py` passes (serial log shows "PCI bus enumerated",
no panic); QMP session ran `lspci` against QEMU's default PIIX4/i440FX
topology and got exactly what's expected -- host bridge, ISA bridge,
IDE controller (matches `ata.c`'s target), an ACPI bridge device, the
VGA controller, and an Intel e1000 ethernet controller (8086:100e) at
irq 11 with both a memory and an I/O BAR -- confirming the eventual NIC
target is already visible and fully decoded. `help`/`help` pagination
confirmed to show the new `lspci` line correctly.

## Build 380 (fix, +1) -- README: what basic TCP/IP networking would require

Asked what basic TCP/IP networking support would need, as a "don't
build it yet, just tell me" question. Researched the codebase (no PCI
enumeration anywhere -- see `ata.h`'s own comment; `isr_dispatch()` in
`idt.c` is a hardcoded if/else chain with no general IRQ-handler
registration; `pmm.c`'s frame allocator has no contiguous-multi-frame
allocation; the fd table in `syscall.c` is filesystem-only, no socket
concept or `SYS_SOCKET`-style syscalls; `pit_ticks()` exists but no
sleep/delay primitive) and wrote up the findings plus a realistic
phased path (PCI enum -> pick `rtl8139` as the easiest first NIC to
target in QEMU -> IRQ registration -> minimal Ethernet/ARP/IP/UDP
before ever touching TCP -> TCP + socket syscalls).

- **`README.md`**: new entry in **Ideas for what's next** capturing
  this -- the four pieces of infrastructure with zero precedent today
  (PCI enumeration, general IRQ-handler registration, contiguous/DMA
  physical memory, socket-like fd abstraction + syscalls), the smaller
  timer-sleep gap, and the phased path, framed as its own multi-session
  project comparable in scope to the filesystem or window manager, not
  a single build bump.

No code changes -- docs only, no build/QEMU verification needed.

## Build 379 (fix, +1) -- pin the CLI editor's status bar to the last row, fix its stray cursor

Reported from testing on their own machine (screenshots of `edit` in
CLI mode): the status bar wasn't staying put at the bottom of the
screen the way it does in the GUI Terminal, and there was an odd
cursor-looking block sitting on its own line right below the status
bar.

Both traced back to `apps/editor.c`'s `editor_render()`, which (see
build 377's own comment, since removed) just did `vga_clear()` then
dumped the whole buffer via `vga_putc()` in order, printing the status
line last and letting the console's own wrap/scroll handle everything.
That's exactly why both symptoms showed up:

1. The status line, printed last, landed wherever the content
   happened to end -- floating right after a short file, or scrolling
   up out of a fixed position on a long one. Never actually pinned to
   the last row the way real nano (or this editor's own GUI Terminal
   renderer, `widget_scrollback_draw()`) does.
2. The physical console has its own blinking cursor (`vga.c`'s
   `cursor_show_and_reset_blink()`) that sits wherever the last
   `vga_putc()` call left it. Since the old code ended by printing the
   status line WITH a trailing `\n`, that's exactly where the physical
   cursor ended up -- a spurious blank row below the status bar, right
   next to the editor's own reverse-video logical cursor. Two
   cursor-looking things on screen for two unrelated reasons.

Fix: `editor_render()` now does its own line-wrapping pass (mirroring
`widgets.c`'s `scrollback_measure()`/`widget_scrollback_draw()`
windowing, just against text rows/columns instead of pixels) --
reserves the console's last row for the status bar, prints only a
window of content lines that fits above it (scrolled to keep the
buffer's cursor inside the window, recomputed fresh on every redraw --
no persistent scroll state, since the CLI editor has no scrollbar/
wheel to drive one), pads with blank lines when there's less content
than room, and prints the status line with NO trailing `\n` so the
physical cursor lands right after the status text on the last row
instead of a row below it -- the same place it'd sit after any
ordinary `vga_write()` that doesn't end in `\n`.

- **`kernel/include/vga.h` / `kernel/drivers/vga.c`**: new
  `vga_cols()`, peer of the existing `vga_rows()` -- current console
  width in text columns (80 in legacy text mode, `gfx_width()/
  gfx_char_w()` in framebuffer mode). `editor_render()`'s wrapping math
  needs both dimensions; `vga_rows()` alone (all `console_page()`
  needed) wasn't enough.
- **`apps/editor.c`**: `editor_render()` rewritten -- two-pass windowed
  redraw (pass 1 finds the cursor's wrapped line/column and the
  buffer's total wrapped line count; pass 2 prints only the window,
  padding down to the status row afterward). Caught one bug of its own
  during testing: the first version of the padding math double-counted
  the row when content exactly filled the window (33 lines against a
  32-row window, say), pushing the status line one row past the bottom
  of the screen and making it invisible entirely. Fixed by tracking
  the actual console row the redraw ends on directly (`line -
  first_line`) instead of a separately-computed "how many lines did I
  print" count that could disagree with it.

Verified: `make clean && make all && make iso` clean, no new
warnings; `boot_smoke_test.py` passes; QMP session tested a short file
(status bar pinned to the bottom row with no floating gap) and a long
file both under and exactly at the window's row count (the specific
case that caught the padding bug), confirmed the cursor-follow
windowing scrolls correctly moving up/down through a 45-line file, and
confirmed F3 exits cleanly back to a normal shell prompt with no
leftover artifacts.

## Build 378 (fix, +1) -- split `apps/shell.c` into shell.c/shell_fs.c/shell_sys.c

Asked, as a forward-looking question (not tied to any specific new
feature), whether any files should be split for easier future
development, and whether `apps/` should be reorganized into `cli/`/
`gui/` subdirectories. Answered with a few options and a
recommendation for each; went with: split `shell.c` now (by category,
not one-file-per-command), and leave the `apps/` directory layout
flat for now.

**Why split now:** `shell.c` had grown to 906 lines and ~24 `cmd_*`
handlers spanning every command category (filesystem, system info,
appearance, the REPL loop itself) in one file -- past CLAUDE.md's own
"every hand-written file is currently under 800 lines... that's
comfortable" reference point, and the kind of file where finding/
editing the right handler was starting to mean scrolling past a lot of
unrelated code, the same signal that drove the original `wm.c` ->
`apps/wm/` split.

**Why not the `cli/`/`gui/` directory reorg (yet):** `kapi.h`/`wm.h`
already draw the real CLI-vs-GUI line architecturally (which header an
app includes), independent of where its file sits on disk -- a
directory split wouldn't add a boundary that doesn't already exist. It
would also mostly be bookkeeping right now: several files don't
cleanly fit one bucket (`editor.c` serves both CLI and the GUI
Terminal; `apps.c`/`gui_apps.c` are registries used by both the
launcher and the shell's `apps`/`run` commands), and at only 14 files
in `apps/` today the churn (new Makefile wildcard/pattern rules per
subdirectory, every `#include` path fixed) isn't worth it yet.
Revisit once there are meaningfully more apps in each bucket.

- **`apps/shell_internal.h`** (new): the sharing boundary for the
  three-file split, mirroring `apps/wm/wm_internal.h`'s pattern
  exactly (plain `extern`s, not accessor functions -- still one
  component, not a real boundary; see `docs/decisions.md`). Declares
  the shared state (`shell_fg`, `cwd`, `history`/`history_count`,
  `LINE_MAX`/`HISTORY_MAX`) and every `cmd_*`/`resolve_path()`
  function split across the three files.
- **`apps/shell_fs.c`** (new): the filesystem commands moved out of
  `shell.c` unchanged -- `cmd_ls`/`cmd_cat`/`cmd_touch`/`cmd_mkdir`/
  `cmd_write_or_append`/`cmd_edit`/`cmd_rm`/`cmd_pwd`/`cmd_cd`, plus
  `list_cb()` (ls's per-entry callback). No logic changes, only made
  non-`static` and declared in `shell_internal.h` so `dispatch()` (in
  `shell.c`) can still call them.
- **`apps/shell_sys.c`** (new): the system-info/settings commands
  moved out unchanged -- `cmd_help`/`cmd_time`/`cmd_timezone`/
  `cmd_uptime`/`cmd_about`/`cmd_echo`/`cmd_meminfo`/`cmd_dmesg`/
  `cmd_reboot`/`cmd_apps`/`cmd_run`/`cmd_fontsize`/`cmd_color`/
  `cmd_history`, plus their private helpers (`console_page()`,
  `MONTHS`, `HELP_LINES`/`TEST_HELP_LINES`, the `dmesg_*` pagination
  statics, `color_from_name()`, `print_fontsize_choices()`,
  `apps_list_cb()`).
- **`apps/shell.c`**: trimmed to the REPL loop (`shell_main()`/
  `shell_read_line()`/`history_add()`/`redraw_line()`), the single
  dispatcher (`dispatch()`/`shell_dispatch()`), and the state every
  command shares (`shell_fg`, `cwd`, `history`/`history_count`,
  `resolve_path()`) -- now ~300 lines, down from 906. `dispatch()`
  itself is untouched line-for-line except calling into the split
  files' now-shared functions instead of file-local `static` ones.
  `#include "editor.h"` dropped (only `cmd_edit`, now in
  `shell_fs.c`, needed it).
- No Makefile change needed -- both new files live directly under
  `apps/`, which the existing non-recursive `apps/*.c` wildcard
  already picks up (unlike the `apps/wm/` split, which needed its own
  `WM_C` wildcard/pattern rule for the subdirectory).

Verified: `make clean && make all && make iso` clean, no new
warnings; `tools/boot_smoke_test.py` passes; QMP session exercised
commands from both split files against the live shell (`cd`/`pwd`/
`cat`/`touch`/`ls` from `shell_fs.c`, `time`/`fontsize`/`history` from
`shell_sys.c`) end to end with no behavior change from build 377.

## Build 377 (feature, +10) -- a nano/pico-style full-screen text editor (`edit`/`nano`)

Asked for a nano/pico-style editor under CLI mode, and what would need
to exist first to make it easier -- plus "nice if it also works in the
GUI Terminal." Presented choices up front on sequencing (build the
shared abstraction first vs. CLI-only then Terminal later), keybinding
fidelity (minimal vs. closer to real nano vs. in-between), and whether
to share the text buffer with Notepad or keep it separate. Went with:
build the shared cursor-aware core first so both surfaces land in one
pass; minimal fidelity (arrows/Home/End/Delete navigation and editing,
F2 save, no Ctrl-key shortcuts, no search/cut/paste); and share via
`widgets.h`'s `text_scrollback` widget, upgrading it with a real cursor
rather than writing a second buffer implementation just for this.

- **`kernel/include/keyboard.h` / `kernel/drivers/keyboard.c`**: new
  special key codes -- `KEY_ARROW_LEFT`/`KEY_ARROW_RIGHT` (extended
  scancodes 0x4B/0x4D, same 0xE0-prefix pattern Up/Down/PageUp/PageDown
  already used), `KEY_HOME`/`KEY_END`/`KEY_DELETE` (0x47/0x4F/0x53,
  same pattern), and `KEY_F2`/`KEY_F3` (0x3C/0x3D, NOT extended --
  plain scancodes the ASCII table already silently ignored, same shape
  as the shift-key checks). F2 = save, F3 = exit -- the editor's
  stand-ins for nano's Ctrl+O/Ctrl+X, since this keyboard driver still
  has no Ctrl-key chording (scoped out of this pass on purpose).
- **`apps/widgets.h` / `apps/widgets.c`**: `text_scrollback` gains a
  `cursor` field (a logical buffer index, distinct from the old
  append-only write point) plus a cursor-aware API alongside the
  original append-only `putc()`/`backspace()`: `cursor_left/right/up/
  down/home/end()` and `insert_at_cursor()`/`delete_at_cursor()`/
  `backspace_at_cursor()`. The two APIs coexist because `putc()`/
  `backspace()`/`clear()` keep `cursor` pinned to the append point, so
  `apps/terminal.c`'s shell-mode scrollback (the original, and until
  now only, caller) sees zero behavior change. `widget_scrollback_draw()`'s
  cursor rendering now draws at `tb->cursor` generically instead of
  assuming "always the end". Up/Down move by scanning for the nearest
  `'\n'` boundaries on the fly rather than caching line positions --
  cheap enough at `SCROLLBACK_CAP`'s size (8192), same "recompute, don't
  cache" tradeoff the rest of this widget already makes.
- **`apps/editor.h` / `apps/editor.c`** (new): the shared editing core
  (`editor_load()`/`editor_save()`/`editor_handle_key()`, all thin
  wrappers over `text_scrollback`'s new cursor API) plus `editor_run()`,
  the physical-console entry point -- a classic blocking keyboard-read
  loop, full-screen redraw via `vga_clear()` + `vga_write()` on every
  keystroke (needs no column-width/row-tracking logic of its own at all
  -- `vga_putc()` already wraps/scrolls), cursor shown as a single
  reverse-video character. Registered as the shell's `edit`/`nano`
  commands (`apps/shell.c`'s new `cmd_edit()`).
- **`apps/notepad.c`**: gained real cursor movement as a side effect of
  the `text_scrollback` upgrade -- Notepad was exactly the "second real
  caller" `widgets.h`'s own "add a primitive once something needs it"
  rule calls for. Arrow keys, Home/End, and Delete now move/edit at the
  cursor instead of being ignored/always appending at the end. Known
  gap: moving the cursor off-screen (e.g. Up repeatedly while scrolled)
  doesn't auto-scroll the view to follow it.
- **`apps/terminal.c`**: `edit`/`nano` is intercepted before reaching
  `shell_dispatch()` (unlike every other command Terminal supports) and
  switches the window into a small non-blocking "editor sub-mode"
  (`st->in_editor`) -- `editor_run()`'s own blocking loop is exactly the
  class of command Terminal excludes everywhere else (see its
  `BLOCKED_CMDS` list), so this drives the SAME `editor_handle_key()`
  one keystroke at a time from `terminal_key()`'s existing non-blocking
  callback instead, rendering with `widget_scrollback_draw()` (now
  cursor-aware) rather than `editor_run()`'s console redraw. Same edit
  logic, two renderers -- the same GUI-vs-CLI split every other pair in
  this codebase already makes.
- **Two real bugs found only by testing in QEMU, not by code review**:
  (1) `editor_run()`'s `struct text_scrollback` was originally a stack
  local -- at ~16KB (`SCROLLBACK_CAP`'s 8192 cells), that silently blew
  the kernel's 16KB boot stack several calls deep into `shell_main()`'s
  own call chain, corrupting nearby memory (manifested as the font size
  randomly shrinking and later keystrokes silently not registering --
  no crash message, just quietly wrong behavior). Fixed the same way
  `apps/notepad.c`'s own top comment already warns about: a static
  instance, not a stack local. (2) The editor's exit key was originally
  Esc alone -- worked fine at the physical console, but `apps/wm/wm.c`
  intercepts Esc globally to leave the whole GUI desktop before any
  window's `on_key` callback ever runs, so a Terminal-embedded editor
  session could never actually receive it. Fixed by adding F3 as the
  real, documented exit key (Esc still works, but only at the physical
  console, and isn't advertised since it's not reliable on both
  surfaces).
- **Verified in headless QEMU on both surfaces**: physical console --
  typed text, moved the cursor with arrows, inserted/backspaced/forward-
  deleted mid-buffer, saved with F2, exited with F3, `cat`'d the file
  back to confirm the save landed correctly, confirmed the shell stayed
  fully responsive afterward (this is what caught bug #1 above). GUI
  Terminal -- opened via the Start menu, ran `edit` on the same
  already-saved file, confirmed it loaded correctly, edited and saved
  with F2, exited with F3 back to Terminal's normal shell mode (not
  kicked out to the physical console -- this is what caught bug #2
  above), `cat`'d the file from inside Terminal to confirm the
  round-trip. Notepad -- confirmed arrow-key cursor movement and
  mid-buffer insert both work correctly. Screenshots in
  `screenshots/2026-08-10/editor_*.png` and
  `notepad_cursor_movement_and_insert.png`.

## Build 367 (feature, +10) -- `/etc/timezones` city database, separate from the toyos.conf selection

Asked whether the handful of hardcoded timezone cities could become a
clear-text "database" file under `/etc`, with `/etc/toyos.conf`'s
`timezone=<city>` key staying as just the *selection* on top of it.
Presented choices for seeding, file format, and scope up front. Went
with: auto-seed the file on first boot if it's missing rather than
shipping it pre-populated on the ISO; simple CSV-style rows
(`name,offset_minutes,dst_rule`); and file-editing only for this pass
-- no new shell command for managing entries, `write`/`append` (already
existing commands) are enough to hand-edit the database.

- **`kernel/core/tz.c`**: the old compile-time `const struct tz_city
  TZ_CITIES[]` (7 hardcoded entries) is now a runtime-loaded, fixed-
  capacity table (`TZ_MAX_CITIES` = 32) populated from `/etc/timezones`
  by a new `tz_load_or_seed_db()`, called first thing in `tz_init()`
  (before the existing `/etc/toyos.conf` selection lookup, which is
  otherwise unchanged). The same 7 cities that used to be the hardcoded
  array now live in a `TZ_DEFAULT_CITIES[]` array used only as seed
  data (written out to `/etc/timezones` the first time it doesn't
  exist) and as an in-memory-only fallback if the file exists but
  parses to zero valid rows (e.g. emptied by hand) -- that fallback
  deliberately does NOT rewrite the file, since a file that fails to
  parse right now might just be mid-edit.
- **New parser, `tz_load_cities()`**: one `name,offset_minutes,dst` row
  per city (e.g. `helsinki,120,eu`), `#` comments (whole-line or
  trailing), blank lines skipped, whitespace trimmed per field -- same
  spirit as `etc_config.c`'s key=value parser (see **Build 357**), just
  three comma-separated fields instead of one key=value pair. A
  malformed row (missing a comma, an empty or too-long name) is skipped
  rather than aborting the whole load, so one bad hand-edited line
  doesn't cost every other city. `dst` accepts `eu`/`us`; anything else
  (including empty or a typo) means no DST rather than a load error --
  don't guess, but don't refuse to load the row either.
- **`struct tz_city`'s `name` field** changed from `const char *`
  (pointing at a string literal) to a fixed `char name[TZ_NAME_MAX]`
  array (`TZ_NAME_MAX` = 20), since city names are now parsed from a
  file at runtime rather than known at compile time. `kapi.h`-facing
  behavior (`tz_city_count()`, `tz_city_name()`, `tz_set_index()`,
  `tz_find_by_name()`) is unchanged -- they just read from the loaded
  table instead of the old hardcoded array.
- **Database vs. selection stays a clean split**: `/etc/timezones` is
  every city this build knows about; `/etc/toyos.conf`'s `timezone=`
  key is which one is active, unchanged from **Build 357**. Editing
  `/etc/timezones` (adding/removing/renaming a city) only takes effect
  on the next boot, since the table is loaded once in `tz_init()` --
  there's no live-reload command in this pass, by design (file-editing
  only was the chosen scope).
- **Verified in headless QEMU**: a fresh disk auto-seeds `/etc/timezones`
  on first boot with the expected 7 default CSV rows (confirmed via
  `cat /etc/timezones`); overwriting the file with a single custom row
  (`sydney,600,none`) via `write`, rebooting, and running `timezone
  sydney` correctly selected it (previously "Unknown timezone" before
  the reboot, since the in-memory table doesn't reload without one) and
  `time` showed the correct +10h offset; the persisted `timezone`
  selection in `/etc/toyos.conf` survived the reboot and matched the
  new city correctly. Also reconfirmed with a full `make clean && make
  all`, `make iso`, and `tools/boot_smoke_test.py`. Screenshots in
  `screenshots/2026-08-10/timezones_db_*.png`.

## Build 357 (feature, +10) -- shared /etc config engine, consolidated into toyos.conf

Asked for a centrally managed config-file structure: a shared tool any
setting reads/writes through instead of each hand-rolling its own
parser, using name=value with `#` comments. Presented choices for file
layout, API shape, and migration scope up front. Went with: everything
defaults into one shared `/etc/toyos.conf` today, but the engine takes
a `path` on every call so a future setting with enough keys of its own
can still get a dedicated file; stateless get/set calls (no open/close
handle); and migrated both existing settings (`timezone`, `font_size`)
onto it right away rather than leaving them on their old parsers next
to the new shared one.

- **`kernel/include/etc_config.h` / `kernel/core/etc_config.c`** (new):
  `etc_config_get(path, key, out, out_size)` /
  `etc_config_set(path, key, value)`. File format: `key=value` per
  line, `#` starts a comment to end of line (whole-line or trailing),
  blank lines ignored, whitespace trimmed around key and value. Every
  call re-reads and re-parses the whole file -- no in-memory handle to
  manage, which matters given there's no heap allocator to lean on for
  one. `etc_config_set()` does a read-modify-write into a 512-byte
  stack buffer (`ETC_CONFIG_MAX`, comfortably under `fs.h`'s 2048-byte
  `FS_DATA_MAX` per-file ceiling): if the key already exists its line
  is replaced in place (every other line, including comments, keeps
  its exact position); otherwise a new line is appended. Replacing a
  key's own line drops any comment that line itself had -- every other
  line's comment is untouched since it's copied through verbatim.
- **`kernel/core/tz.c`**: `/etc/timezone` (bare city-name text) ->
  `/etc/toyos.conf`'s `timezone=<city>` key. `tz_init()` now checks the
  new file first via `etc_config_get()`; if not found, it checks BOTH
  older bare-text locations in turn (`/etc/timezone`, then the
  pre-`/etc` `/timezone` from before directories existed at all --
  read directly via `fs_read()`, not `etc_config_get()`, since neither
  legacy file is actually `key=value`) and migrates whichever is found
  into the new file via `etc_config_set()`, deleting the old one.
  `tz_set_index()` now calls `etc_config_set()` instead of writing the
  bare city name directly.
- **`kernel/core/font_config.c`**: `/etc/fontsize`'s `font_size=<n>` ->
  the same key inside `/etc/toyos.conf`. Since the OLD file was already
  `key=value` (unlike `tz.c`'s bare text), its migration path reuses
  `etc_config_get()` directly against the old path instead of a
  separate bare-text read -- one fewer thing to hand-roll. This file's
  own hand-rolled parser (the one that motivated waiting for "a second
  real caller" before generalizing, per its old top comment) is gone;
  `name_to_size()`'s generic loop over `gfx_font_size_name()` (added in
  **Build 347**) is the only bit of size-specific logic left here.
- **Verified in headless QEMU**: `fontsize 14` + `timezone helsinki`
  both land as separate lines in one `cat /etc/toyos.conf`
  (`font_size=14` / `timezone=helsinki`); hand-writing a non-`key=value`
  line into the file (`write /etc/toyos.conf`) and then changing
  `fontsize` confirmed that line is preserved byte-for-byte across the
  read-modify-write rather than dropped; simulating a pre-upgrade
  `/etc/timezone` (bare `berlin`) + a deliberately-mismatched legacy
  `/etc/fontsize` and rebooting confirmed the timezone migrated into
  `/etc/toyos.conf` (`timezone=berlin`, old file deleted, `timezone`
  shows `berlin` as the active `*` selection) while the mismatched
  fontsize file was correctly left untouched rather than silently
  dropped (no recognized key = ignore, not guess). Also reconfirmed
  with a full `make clean && make all` and `tools/boot_smoke_test.py`.
  Screenshots in `screenshots/2026-08-10/etcconfig_*.png`.

## Build 347 (feature, +10) -- numeric `fontsize`, eight point sizes

Two requests: make `fontsize` take numbers "like they usually work"
instead of tiny/small/medium/large, and think about where `/etc`
config structure should go from here given a second setting
(`/etc/fontsize`) now exists alongside `/etc/timezone`. Presented
choices for both up front. Font sizing: went with expanding to a bigger
set of granular point sizes (8/10/12/14/16/18/20/24) rather than just
relabeling the same 4 tiers -- more baked glyph data, but numeric
selection actually feels like a real font-size picker instead of 4
coarse aliases. `/etc` structure: left it as-is -- still one
hand-rolled parser per settings file, no shared key=value layer -- per
the explicit choice not to generalize yet.

- **`tools/genttf.py`**: `SIZES` replaced -- `("tiny", 16, 9, 18, 15)`
  etc. is now `("8", 8, 4, 10, 8)` .. `("24", 24, 14, 27, 22)`, 8 entries
  instead of 4. The `name` field doubles as both the enum suffix and the
  user-facing point-size string now (previously two independent
  concepts that happened to be spelled the same way for tiny/small/
  medium/large), and `PIXEL_SIZE` is that same number passed straight
  to FreeType -- no more separate "what FreeType size produces this
  named tier" mapping to keep in sync. CELL_W/CELL_H/BASELINE_Y for the
  new sizes were derived from a formula fit to the *old* 4 sizes'
  hand-tuned values (`cell_w = int(font.getlength("M"))`,
  `baseline_y = round(ascent * 0.89)`,
  `cell_h = baseline_y + round(descent * 0.6)`, using
  `ImageFont.getmetrics()`'s ascent/descent at each PIXEL_SIZE) --
  confirmed against the old table: it reproduces old "tiny" (16px) as
  exactly (9, 18, 15), and comes within 1-2px of old small/medium/large,
  which is the same minor descender clipping those already had rather
  than a new problem. Regenerating requires `fonts-jetbrains-mono`
  installed locally (see the script's own top comment); ran it in the
  cloud sandbox after `apt-get install fonts-jetbrains-mono` there.
- **`kernel/include/font_ttf.h` / `kernel/drivers/font_ttf.c`**
  (GENERATED): regenerated via `tools/genttf.py`. `enum font_size` is
  now `FONT_SIZE_8 .. FONT_SIZE_24` (8 entries); `font_ttf_variants[]`'s
  `name` fields are `"8".."24"`.
- **`kernel/drivers/gfx.c`**: default `cur_font_size` changed from
  `FONT_SIZE_SMALL` to `FONT_SIZE_18` -- the old named sizes don't exist
  anymore, and 18pt (10x21 cell) is the closest by on-screen area to the
  old default's 11x22.
- **`kernel/core/font_config.c`**: `name_to_size()` no longer hardcodes
  a `tiny`/`small`/`medium`/`large` string list -- it loops
  `gfx_font_size_name(i)` for every baked size and compares against
  that, so it stays correct automatically if sizes are ever added or
  removed again without a second hardcoded list to keep in sync. The
  `/etc/fontsize` file format itself (`font_size=<value>\n`) is
  unchanged -- only what `<value>` looks like changed, from a name to a
  number (`font_size=12`, confirmed via `cat /etc/fontsize` in QEMU).
- **`apps/shell.c`**: `cmd_fontsize()`'s size-name matching gets the
  same generic-loop treatment as `font_config.c` (no hardcoded string
  list). New `print_fontsize_choices()` helper prints every baked
  size's name comma-separated, shared by the usage line and the
  "unknown size" error, so what's shown to the user can't drift from
  what's actually baked in. Help text (`help`) updated:
  `fontsize <n>  - set font size in points: 8, 10, 12, 14, 16, 18, 20,
  or 24`.
- **`/etc` structure**: deliberately left alone this round --
  `font_config.c`'s parser is still single-purpose, not a shared
  key=value reader `tz.c` also goes through. The generic-loop change
  above was needed either way (hardcoding 8 numbers instead of 4 names
  in two places would've been worse, not better), so it's not really
  "keeping duplication" so much as "not building shared /etc
  infrastructure neither setting has asked for yet."
- **Verified in headless QEMU** (`tools/qmp_test.py`): `fontsize` with
  no args shows the usage line + all 8 choices + current size;
  `fontsize 24`/`fontsize 8`/`fontsize 12` each resize the console
  correctly (font readable at all three, including 8pt); `fontsize
  bogus` gives a clean error listing all 8 valid sizes instead of
  silently no-op'ing; `cat /etc/fontsize` after a change shows
  `font_size=12`, confirming persistence; GUI mode (taskbar, Start
  button) also re-renders correctly at the new size. Also reconfirmed
  with a full `make clean && make all` and
  `tools/boot_smoke_test.py`. Screenshots in
  `screenshots/2026-08-10/fontsize_*.png`.

## Build 337 (feature, +10) -- dirty-rectangle blitting + cursor sprite

The window manager's own roadmap item (README.md's "Ideas for what's
next"): double buffering (build ~250s) killed the flicker, but
`gfx_present()` still blitted the entire framebuffer every single
frame, and `wm_run()` still triggered a full repaint on every mouse
*move*, not just clicks/drags -- so sliding the mouse across an
otherwise idle desktop cost a full window/taskbar/menu redraw plus a
full-screen blit, every tick. Presented three scopes up front
(blit-only dirty rect / cursor sprite + blit dirty rect / full
dirty-rect compositor); went with the middle one -- targets the actual
complaint (cursor movement forcing a full repaint) without touching
how windows/widgets draw themselves, which keeps the existing
"whole-screen redraw is simpler with overlapping windows" design
intact for real scene changes.

- **`kernel/drivers/gfx.c`**: `gfx_present()` no longer blits
  `width * height` pixels unconditionally. A bounding-box dirty region
  (`dirty_x0/y0/x1/y1`) is tracked at the one place every drawing
  primitive in this file bottoms out at -- `gfx_put_pixel()` -- so
  `gfx_fill_rect()`/`gfx_draw_rect()`/`gfx_draw_char()`/
  `gfx_draw_string()`/`gfx_clear()` all get dirty-tracked automatically,
  no per-call-site bookkeeping needed anywhere else. `gfx_present()`
  now blits only that bounding box and resets it; if nothing was drawn
  since the last present, it returns immediately. `gfx_scroll_up()`'s
  double-buffered branch bypasses `gfx_put_pixel()` (a raw row memmove
  within `back_buffer`), so it marks its region dirty by hand via a new
  `dirty_mark_rect()` helper -- not currently reachable with double
  buffering on (the console, `gfx_scroll_up()`'s only caller, leaves it
  off), but kept correct on its own terms rather than relying on that
  caller-side invariant holding forever.
- **`apps/wm/wm_render.c`**: new cursor-sprite save/restore
  (`save_cursor_under()`/`restore_cursor_under()`, a 20x20 box anchored
  2px above/left of the cursor's own (x, y) -- oversized on purpose to
  cover `draw_cursor_h()`/`draw_cursor_v()`'s asymmetric bounding boxes,
  which draw up to 1px above/left of their anchor point) and a new
  `wm_render_cursor_move(mx, my)` entry point: restores whatever was
  under the cursor's old position, draws the cursor at the new one
  (saving what's newly under it for next time), and presents -- touching
  only two small boxes instead of redrawing the scene. `resolve_cursor_kind()`
  factors the H/V/diag/normal cursor-shape decision out of
  `wm_render_frame()` so both render paths pick the same shape the same
  way. `wm_render_frame()` (the full path) now goes through the same
  `draw_cursor_at()` helper the cursor-only path uses, which is what
  keeps `cursor_under`'s saved pixels correct across a full repaint --
  without that, a cursor-only move right after a full redraw would
  restore stale content instead of what the full redraw actually drew
  underneath.
- **`apps/wm/wm.c`**: `wm_run()`'s loop no longer sets `redraw_pending`
  on mouse movement alone. It now takes one of two paths each tick:
  `redraw_pending` (set by a click, an in-progress drag/resize, a
  window open/close, a key/wheel event delivered to an app, or the
  once-a-second clock tick) still gets the full `wm_render_frame()`;
  mouse movement with nothing else pending gets
  `wm_render_cursor_move()` instead; neither pending means the tick
  does no rendering work at all (an improvement over before, which
  still skipped rendering but is worth stating since the mouse-moved
  case used to always force a redraw).
- **Verified in headless QEMU** (`tools/qmp_test.py`): moved the cursor
  repeatedly with nothing else happening (no ghosting/stale pixels left
  behind); opened the Start menu and Calculator (full-path redraw);
  dragged the Calculator's window (full-path redraw across multiple
  frames, no artifacts at the old position); watched the taskbar clock
  advance across a tick (periodic full redraw still correct); hovered
  the resize corner to confirm the diagonal resize cursor still renders
  correctly through `resolve_cursor_kind()`. Screenshots in
  `screenshots/2026-08-10/dirtyrect_*.png`. Also reconfirmed with
  `tools/boot_smoke_test.py` and a full `make clean && make all`.

## Build 327 (feature, +10) -- `dmesg`, and a categorized/professional `help`

Two requests: a Linux-dmesg-style command, and a cleaner, more
professional-looking `help` that doesn't mix everyday commands with
the ~13 ring-3/syscall/scheduler diagnostic ones.

- **`kernel/include/klog.h` / `kernel/core/klog.c`** (new): a 16KB
  ring buffer behind `klog_write()`/`klog_putc()`/`klog_dump()`. Every
  kernel-side diagnostic message used to go only to the physical COM1
  serial port (`serial_write()`, `kernel/core/serial.c`) with nothing
  kept in memory -- a real dmesg needs something to actually query.
  `klog_write()` is a thin decorator, not a replacement: it still
  forwards every raw byte to `serial_write()`/`serial_putc()`
  unchanged (so `tools/qmp_test.py`'s and `tools/boot_smoke_test.py`'s
  `serial.log` capture is completely unaffected), and additionally
  buffers a timestamped copy. `serial.c` itself never changed -- still
  just the raw UART driver, doesn't know klog exists. Timestamps
  (`[secs.hh]`, PIT-tick-based -- 100Hz is this kernel's real tick
  rate, so hundredths is honest resolution) are added once per
  *logical* line, not once per call -- tracked via an `at_line_start`
  flag, since some call sites (e.g. `idt.c`'s panic path) build one
  line across several separate `serial_write()`/now-`klog_write()`
  calls with no `\n` until the last one.
- **Mechanically renamed every `serial_write()` call site to
  `klog_write()`** across all ~15 files that had one (`kernel.c`,
  `idt.c`, `syscall.c`, `tfs.c`, `scheduler.c`, and every `*_test.c`),
  swapping `#include "serial.h"` for `#include "klog.h"` in the 14 that
  no longer call `serial_init()` directly (only `kernel.c` still does,
  and keeps both includes). This is what makes `dmesg` show real,
  complete kernel history from boot -- not just new call sites.
- **`kapi.h`**: added `klog.h` (for `klog_dump()`) to the apps-facing
  surface.
- **`apps/shell.c`**: new `dmesg` command -- dumps the ring buffer,
  paginated the same way `help` already was (`-- more --`, `'q'` to
  quit; unpaginated when running inside a GUI Terminal's non-blocking
  sink, same as `help`'s existing sink check) via a small
  `dmesg_putc_cb` callback. Pagination state lives in file-scope
  statics rather than threaded through the callback, matching this
  codebase's existing plain-callback convention (see `fs_list()`/
  syscall.c's "SYS_LISTDIR scratch state" comment) since
  `klog_dump()`'s callback deliberately has no userdata slot.
- **`help` split in two**: `HELP_LINES` is now a shorter, categorized
  list (General / Files & filesystem / System info / Appearance,
  `dmesg` included) with the ~13 ring3test/elftest/syscalltest/etc.
  diagnostic commands removed entirely. `TEST_HELP_LINES` holds them,
  unchanged in content, behind a new `help tests` (an optional argument
  to `cmd_help()`, the same pattern `timezone <city>` already uses --
  no new top-level command name to remember). Each list points at the
  other at top/bottom.
- `README.md`: updated the shell command list to include `dmesg` and
  the `help`/`help tests` split.
- Verified in headless QEMU: `dmesg` after a fresh boot shows all 6
  init messages, correctly timestamped, in order. Ran `syscalltest`
  and confirmed its diagnostic lines (`syscall_test: calling
  process_run_ring3()`, `syscall: exit() called by ring-3 process`,
  ...) then showed up in a follow-up `dmesg` -- proving the retrofit
  actually works end to end, not just for boot messages. `help`
  paginates cleanly through all four categories; `help tests` shows
  the full diagnostic-command list with a pointer back to `help`.
  Screenshots in `screenshots/2026-08-10/`.

## Build 317 (feature, +10) -- header dependency tracking, a boot smoke test, and CI

Requested after a conversation about what would minimize testing
effort and make future features easier to add: three build/test
infrastructure gaps, picked from a shortlist because they're either
things this project had already been bitten by (the stale-`.o` header
bug) or things that were entirely manual up to this point (every
verification depending on a session remembering to run it).

- **Header dependency tracking.** `Makefile`: added `-MMD -MP` to
  `CFLAGS`/`USERLAND_CFLAGS`, and a `-include $(wildcard .../*.d ...)`
  line pulling in the generated dependency fragments. Editing a shared
  header (`apps/widgets.h`, `apps/gui_apps.h`, ...) now correctly
  rebuilds every `.o` that includes it, not just the ones whose own
  `.c` changed -- this is the exact failure mode that used to require
  "always `make clean && make all` before testing a GUI change" (a
  stale `.o` compiled against an old struct layout silently desyncing
  from freshly-rebuilt ones -- see the build-293-era Start-menu
  corruption bug). One real subtlety this surfaced: `kernel/include/
  kapi.h` includes `version.h`, and `version.h` gets regenerated on
  literally every `make all`/`make iso`
  (`tools/gen_version.sh`) -- an unconditional rewrite there would have
  bumped `version.h`'s mtime every single build and made every file
  that includes `kapi.h` (nearly everything) look "out of date" and
  rebuild every time, defeating the entire point. Fixed by making
  `gen_version.sh` idempotent (only writes when `BUILD_NUMBER`'s value
  actually changed, via a tmp-file + `cmp -s` compare) -- verified this
  mattered by testing all three cases: touching a shared header (only
  its real dependents rebuild), running `bump_build.sh` then building
  (only files that include `version.h` rebuild, generalizing what used
  to be a manual force-delete of `about.o`/`shell.o` in the `version:`
  target -- now removed, no longer needed), and a plain no-op rebuild
  (nothing recompiles, `version: build N (unchanged)`).
- **`tools/boot_smoke_test.py`** (new): boots `toy-os.iso` headlessly
  (no display, no QMP), polls `serial.log` for the expected kernel init
  sequence or a `PANIC:`, exits 0/1 in a few seconds. Doesn't replace
  `tools/qmp_test.py` for anything touching rendering/input/window
  behavior, but most kernel/driver-level changes don't need the full
  QMP GUI-testing dance to sanity-check -- they need "does it still
  boot cleanly," which this answers far faster. Verified both the pass
  path (real boot, `-v` shows all 6 patterns matched in ~1s) and the
  fail path (`--timeout 0.05` correctly times out and exits 1, listing
  exactly which patterns were missing).
- **`.github/workflows/build.yml`** (new): runs `make clean && make all
  && make iso` plus the new boot smoke test on every push/PR to `main`,
  so a build break or boot regression is caught automatically instead
  of depending on a session remembering to verify locally first. Uses
  the same apt package list (`nasm`, `grub-pc-bin`, `grub-common`,
  `xorriso`, `qemu-system-x86`, `mtools`) already known to work from
  setting up this exact toolchain in the cloud sandbox this session.
- `CLAUDE.md`/`tools/qmp_test.py`: updated the now-outdated "always
  `make clean`" advice in both places to reflect that a plain `make
  all` is safe after a header change as of this build, documented the
  two new tools, and noted CI as a second, automatic check behind
  local verification (not a replacement for it).
- Verified: `make clean && make all && make iso` clean, no new
  warnings; `tools/boot_smoke_test.py -v` passes against the resulting
  ISO; a no-op `make all` immediately after does nothing (`version:
  build N (unchanged)`, no recompilation).

## Build 307 (fix, +1) -- CLAUDE.md: when to split a file, generalized from the wm.c precedent

Requested after a conversation about how large this project can grow
before it gets harder to work with -- the answer was "splitting a file
once it gets big is already the right instinct (see the `apps/wm/`
split), keep doing that," but that instinct only lived implicitly in
one example, not as a stated convention a future session would
necessarily connect to a new situation.

- `CLAUDE.md`: new bullet in "Conventions worth knowing before
  editing," right after the existing `apps/wm/` split bullet it
  generalizes from. No hard line-count rule -- the signal is a file
  mixing more than one real concern, or long enough that finding the
  right part of it gets slow -- but gives a concrete reference point
  (every hand-written file here is currently under 800 lines) and
  explicitly excludes generated data files like `font_ttf.c` (11,800+
  lines of baked glyph data, wrong axis to judge by line count).
  Points at the `apps/wm/` pattern (split by concern, `_internal.h` of
  `extern`s if still one component) as the template to reuse rather
  than reinventing one each time, and calls out `CHANGELOG.md` itself
  as the same instinct applied to docs (split by era if it ever gets
  unwieldy to search -- not needed yet at ~2,900 lines).
- No code changes -- docs/build-metadata only.

## Build 306 (fix, +1) -- docs/decisions.md: topic-indexed "why is this built this way" answers

Requested: a docs/ directory with specs/documentation for future
sessions and contributors. Checked what's already there first --
README.md/apps/README.md cover architecture, CLAUDE.md covers
workflow/environment quirks, and README.md already has an actively
maintained "Ideas for what's next" (forward-looking roadmap, completed
items struck through and linked to the CHANGELOG build that finished
them) -- so a new docs/roadmap.md would have just duplicated that.
What's genuinely missing: CHANGELOG.md records *what* happened and
*why* per entry, chronologically, but isn't indexed by topic, so
answering "why is X built this way" means scrolling/searching 150KB+
of history. Design-scope comments scattered through source files
("deliberately NOT...", "Honest limitations, not solved here" in
tfs.c, wm_internal.h, terminal.h, calc_engine.c, theme.h, fs_ops.h)
have the same problem -- each answers one question, but there's no
single place listing which questions already have answers.

- `docs/decisions.md` (new): ten short entries (a few sentences each),
  one per decision that seemed likely to get re-litigated by a future
  session -- VFS single-backend-not-mount-points, no recursive delete,
  write-through-not-journaled persistence, `kapi.h` as the only apps/
  boundary, `widgets.h`/`theme.h` staying minimal until a second real
  caller, the window manager as one event loop not decoupled
  components, Terminal wrapping the real shell instead of
  reimplementing it, why `ring3test`/`elftest` still need a reboot
  post-teardown, and the build-number tier scheme. Each links to the
  CHANGELOG.md section or source file with the actual reasoning --
  deliberately a pointer file, not a second copy of it, so it can't
  drift the way a restated copy would.
- `README.md`: linked docs/decisions.md from the intro, and added one
  new "Ideas for what's next" bullet (VFS mount-point support, if a
  second filesystem ever needs to coexist with the first rather than
  replace it) -- the only genuinely new forward-looking item found
  that wasn't already on that list.
- `CLAUDE.md`: new `## docs/` section explaining what belongs there
  (and, just as importantly, what doesn't -- forward-looking items
  stay in README's existing list, not a second one) and when to add an
  entry going forward.
- No code changes -- docs/build-metadata only. `make clean && make
  all` still clean (sanity-checked; nothing under kernel/ or apps/
  touched).

## Build 305 (fix, +1) -- tools/device_git.sh: sweep every stale git lock, not just index.lock

Hit while committing build 304 over the device bridge: the first
version of this script (build 294) only cleared a stale
`.git/index.lock`. A real `git commit` also creates `.git/HEAD.lock`
and `.git/objects/maintenance.lock`, which the bridge fails to delete
the exact same way -- so after one commit, the *next* device_git.sh
invocation cleared index.lock (as designed) but then still hit `fatal:
cannot lock ref 'HEAD': Unable to create '.../HEAD.lock': File
exists`, because that lock was never swept. Had to clear it by hand
mid-session to get build 304 committed at all.

- `tools/device_git.sh`: now sweeps every `*.lock` file under `.git/`
  (via `find .git -name '*.lock'`) before running the real command,
  instead of hardcoding `index.lock` by name. Covers HEAD.lock,
  objects/maintenance.lock, and any other lock git might leave in a
  spot not enumerated ahead of time.
- Verified: reproduced the HEAD.lock failure this session (a `commit`
  through the old script left both index.lock and HEAD.lock behind;
  the next invocation only cleared the former and hit the latter).
  Confirmed the updated script's `find`-based sweep picks up both in
  one pass.

## Build 304 (feature, +10) -- VFS layer: filesystem split into a dispatch layer + swappable backend

Requested in advance of actually needing a second filesystem: refactor
the filesystem so a future one can be added without touching fs.h,
kapi.h, or any existing caller. Previously kernel/drivers/fs.c *was*
the filesystem -- one file mixing the public fs_* entry points with
the on-disk TFS format, the in-memory table, and path handling, all
directly. There was no seam to plug a second filesystem into short of
editing fs.c itself.

- `kernel/include/fs_ops.h` (new): defines `struct fs_ops`, a vtable of
  9 function pointers (`init`/`touch`/`write`/`mkdir`/`del`/`read`/
  `is_dir`/`exists`/`list`) mirroring fs.h's public API exactly. This
  is the seam -- a future filesystem implements this struct and is
  otherwise free to work however it wants internally. Deliberately
  *not* a mount-point scheme (no routing by path prefix to multiple
  simultaneously-active backends) -- there's exactly one active
  backend at a time, chosen once at boot. Nothing needs multiple
  filesystems mounted at once yet, and mount-point routing is
  meaningfully more code (cross-mount path resolution, boundary
  conflicts) for a capability that would sit unused; see the header's
  top comment for the full reasoning.
- `kernel/drivers/fs.c` -> `kernel/drivers/tfs.c` (renamed, not
  rewritten): this is the exact original flat/directory filesystem
  ("TFS") logic, unchanged in behavior -- disk format, FS_DISK_VERSION,
  record layout, path normalization rules, the no-recursive-delete
  limitation, all identical. Only the public entry points changed
  shape: `fs_touch`/`fs_write`/etc. became `static tfs_touch`/
  `tfs_write`/etc., `fs_init` became `tfs_init` and now returns
  `int` (1 = persisted to real disk, 0 = RAM-only) instead of setting
  a flag a separate accessor read. The file exposes exactly one public
  symbol: `const struct fs_ops tfs_ops`, its vtable.
- `kernel/include/tfs.h` (new): declares `extern const struct fs_ops
  tfs_ops` -- backend-internal, meant to be included only by vfs.c
  (and any future backend registration code), never by apps/ or
  anything going through fs.h.
- `kernel/drivers/vfs.c` (new): implements every fs.h entry point
  (`fs_init`/`fs_touch`/`fs_write`/`fs_mkdir`/`fs_delete`/`fs_read`/
  `fs_is_dir`/`fs_exists`/`fs_list`/`fs_is_persistent`) as a one-line
  forward to whichever `const struct fs_ops *` is active. `fs_init()`
  is the one place that decides which backend that is -- today always
  `&tfs_ops`, since there's only one. Adding a second filesystem later
  means writing its own tfs.c-shaped file with its own `fs_ops`
  struct, `#include`-ing its header here, and either swapping which
  one `fs_init()` assigns or picking between them (e.g. probing a
  magic number on disk) -- no other file in the kernel or apps/ needs
  to change.
- `kernel/include/fs.h`: unchanged in API (every existing caller --
  kernel.c, syscall.c, tz.c, font_config.c, the shell, Notepad, the
  `about` screens -- needed zero changes), just updated comments that
  referenced `fs.c` to point at `tfs.c`/`vfs.c` instead, plus a new
  top-of-file paragraph explaining the split.
- `kernel/include/kapi.h`: updated the one-line comment on its `fs.h`
  include to say "backend-agnostic API" instead of "the in-memory
  filesystem" (already misleading pre-disk-backing, more so now).
- Verified: `make clean && make all && make iso` clean, no new
  warnings. Booted headlessly in QEMU (see CLAUDE.md's QMP testing
  section) with a fresh `disk.img`: boot log shows `fs: formatted a
  fresh persistent filesystem on disk` (proves `tfs_init()`'s return
  value reaches `vfs.c` and back out through `fs_is_persistent()`).
  From the shell, `mkdir docs`, `write docs/notes.txt hello vfs
  world`, `ls docs`, `cat docs/notes.txt`, and `ls` all round-tripped
  correctly through the new dispatch layer, and `about` reported
  "Storage: disk-backed (files persist across reboots)". Killed QEMU,
  relaunched against the same `disk.img` (a real reboot, not just a
  shell restart): boot log now shows `fs: loaded persistent filesystem
  from disk`, and `cat docs/notes.txt` still returned "hello vfs
  world" -- the write-through path works unchanged end to end.
  Screenshots in `screenshots/2026-08-10/`.

## Build 294 (fix, +1) -- Documented the device-bridge git index.lock quirk

While starting a fresh session in this repo, found that `git` commands
run over the Cowork device bridge (`device_bash`) leave behind a stale
`.git/index.lock`: git creates the lock, then tries to `unlink()` it
when the command finishes, but the device bridge blocks `unlink()` on
mounted files the same way it blocks `rm` (already documented for
regular files). The command itself still succeeds -- you just see a
`warning: unable to unlink ... Operation not permitted` -- but the lock
is left behind, and the *next* git command that needs to write the
index (`add`, `commit`, ...) fails hard with `fatal: Unable to create
'.../index.lock': File exists`, which looks exactly like a genuinely
stuck git process but isn't one. `mv` (rename) is allowed through the
bridge even though `rm` (unlink) isn't, so the fix is to rename the
stale lock out of the way rather than delete it.

- `CLAUDE.md`: new bullet under "Working in the cloud sandbox vs. the
  user's machine" explaining the quirk and pointing at the new script;
  `tools/` section updated to list it.
- `tools/device_git.sh`: new wrapper -- clears a stale `.git/index.lock`
  (via `mv` into `.git/_to_delete/`) if present, then runs the real
  `git` command. No-op when there's no stale lock, so it's safe to use
  for every git invocation made over the device bridge, not just the
  ones expected to write.
- Verified: reproduced the lock getting left behind after a plain `git
  status`/`git add -A --dry-run` via `device_bash` in this session,
  confirmed `mv` (unlike `rm`) succeeds on the lock file through the
  bridge, and confirmed a subsequent `git add`/`git status` then runs
  clean. Repo itself was already clean and up to date with
  `origin/main` at build 293 -- no code changes in this entry, tooling
  and docs only.

## Build 293 (feature, +10) -- Notepad converted to the shared scrollback widget (scrollbar phase 4/4)

Last of the four-phase scrollbar plan (builds 263, 273, 283 gave
Terminal keyboard, visual, and wheel scrolling). This phase converts
Notepad from its original flat `char[NOTEPAD_MAX]` buffer + manual
col/row draw loop over to the same `struct text_scrollback` (widgets.h)
Terminal uses -- which means Page Up/Page Down, the draggable
scrollbar, and the mouse wheel all now work in Notepad too, for free:
none of that is new code, it's the exact widget/callback plumbing
builds 263/273/283 already wrote and tested, just pointed at a second
app.

- `apps/notepad.c`: `struct notepad_state` now holds a `text_scrollback
  tb` instead of `char text[NOTEPAD_MAX]; int len;`. `notepad_key()`
  routes backspace/enter/printable characters through
  `widget_scrollback_backspace()`/`_putc()` instead of manipulating the
  flat buffer directly, and gained a `KEY_PAGE_UP`/`KEY_PAGE_DOWN`
  branch identical in shape to terminal.c's. `notepad_draw()` calls
  `widget_scrollback_draw()` instead of its own wrapping loop. New
  `notepad_layout()` (mirrors terminal.c's `term_layout()`) reserves a
  scrollbar strip along the text area's right edge, below the toolbar
  row -- skipped entirely on narrow windows, same as Terminal.
  `notepad_click()` now branches on whether the click landed in the
  toolbar (existing Save/Load handling) or the text/scrollbar area
  (new: track-click paging); `notepad_drag_start()`/`notepad_drag()`
  (thumb dragging) and `notepad_wheel()` are new, and all three are
  close copies of terminal.c's equivalents adjusted for the toolbar's
  vertical offset.
- `apps/notepad.h`: added declarations for the three new callbacks.
- `apps/gui_apps.c`: registered `on_drag_start`/`on_drag`/`on_wheel`
  for Notepad's registry entry (it already had `on_click`).
- Save/Load: `fs_write()`/`fs_read()` (fs.h) only know about flat
  null-terminated buffers, not this widget, so two small new helpers
  bridge the boundary -- `notepad_serialize()` flattens the
  scrollback's ring buffer into a plain byte string for `fs_write()`,
  and `notepad_load_text()` (the inverse) clears the scrollback and
  replays a loaded file back through the ordinary `widget_scrollback_putc()`
  path, same as if it had been typed. `notepad_serialize()`'s output
  buffer is a new file-scope static (`g_save_buf`, `SCROLLBACK_CAP`/
  8KB), deliberately not a stack local: the window manager runs on the
  kernel's 16KB boot stack (see boot.asm), not a per-process kstack,
  and an 8KB stack array on top of whatever call depth already got to
  `notepad_click()` would be a real overflow risk.

Notepad's old hard 1024-character cap is gone as a side effect --
capacity is now `SCROLLBACK_CAP` (8192) like Terminal's, with the same
"ring buffer drops the oldest character once full" behavior instead of
a hard stop, which is arguably a nicer failure mode anyway.

Tested in QEMU: opened Notepad, typed 25 lines (more than the default
window fits), confirmed it starts bottom-pinned and the scrollbar
thumb tracks that; dragged the thumb to the top and confirmed the
earliest lines became visible with the thumb at the top of the track;
scrolled the wheel down and confirmed it returned to the bottom-pinned
state; Page Up/Page Down produced the same paging Terminal's do. For
Save/Load, typed 5 lines, clicked Save, typed 3 more (diverging from
the saved snapshot), then clicked Load and confirmed the 3 extra lines
were discarded and exactly the original 5-line snapshot came back --
a real round trip through the filesystem, not just an in-memory check.
Also re-opened Calculator and About afterward to confirm the registry
change didn't disturb them, and that build 283's About-reported
version still matches reality (293 wasn't baked into that particular
screenshot's build, taken mid-session before this entry's version
bump, but the same `make clean && make all` discipline from build
273's incident was followed throughout, and a final clean rebuild
confirmed no stale-object symptoms before delivery).

Screenshots: `screenshots/2026-08-10/scrollbar_p4_notepad_dragged_top.png`,
`screenshots/2026-08-10/scrollbar_p4_notepad_wheel_bottom.png`,
`screenshots/2026-08-10/scrollbar_p4_notepad_saved.png`,
`screenshots/2026-08-10/scrollbar_p4_notepad_diverged.png`,
`screenshots/2026-08-10/scrollbar_p4_notepad_loaded_roundtrip.png`

This closes out the scrollbar plan (builds 263, 273, 283, 293): both
Terminal and Notepad now support keyboard, visual-drag, and
mouse-wheel scrolling through one shared, twice-reused implementation.

## Build 283 (feature, +10) -- mouse scroll wheel support (scrollbar phase 3/4)

Third of the four-phase scrollbar plan (build 263: keyboard, build 273:
visual scrollbar widget). This one adds the mouse wheel, so Terminal
can now be scrolled with any of the three inputs originally asked for.

- `kernel/include/mouse.h` / `kernel/drivers/mouse.c`: the PS/2 driver
  only ever parsed plain 3-byte packets before. Added the standard
  "IntelliMouse" detection handshake to `mouse_init()` -- setting the
  sample rate to 200, then 100, then 80 in a row (a specific magic
  sequence real hardware and every PS/2-emulating VM recognizes),
  then reading the device ID back (0xF2): ID 3 means the device
  switched into reporting 4-byte packets with a signed wheel-notch
  count in the 4th byte, ID 0 means it's a plain mouse and nothing
  changes. `mouse_feed_byte()` now sizes packets dynamically off
  that (`packet_size`, decided once at init) and, on a 4-byte device,
  accumulates the wheel byte into a new counter exposed by
  `mouse_get_wheel_delta()` (returns ticks since last call, resets to
  0 -- a consume-once accumulator, same shape as `keyboard_try_getchar()`).
- `apps/gui_apps.h`: added another optional callback, `on_wheel(win,
  delta)`, called on whichever window currently has keyboard focus
  (same rule as `on_key`) when the wheel moves.
- `apps/wm/wm.c`: `wm_run()`'s loop now also polls
  `mouse_get_wheel_delta()` each tick alongside the existing
  `keyboard_try_getchar()` poll, and dispatches to the focused
  window's `on_wheel` the same way keys dispatch to `on_key`.
- `apps/terminal.h` / `apps/terminal.c`: added `terminal_wheel()`,
  registered as Terminal's `on_wheel` -- 3 lines per notch (an
  ordinary desktop-scrolling speed), reusing the same
  `widget_scrollback_scroll()` PgUp/PgDn and the scrollbar already
  use, so all three inputs stay in perfect agreement about what a
  "line" of scrolling means.
- Notepad still not touched -- phase 4.

Tested in QEMU: QEMU's QMP `input-send-event` supports synthetic
`wheel-up`/`wheel-down` button presses, which its PS/2 mouse emulation
turns into real wheel packets once a guest driver has done the
IntelliMouse handshake -- a genuine end-to-end test of the new driver
code, not a simulation of one. Opened Terminal, built up scrollback
past a screenful with repeated `help`, confirmed it starts pinned to
the bottom; scrolled the wheel up 5 notches and confirmed the view
moved to earlier content (older commands' output became visible, the
scrollbar thumb moved up); scrolled down 20 notches and confirmed it
returned to the bottom-pinned state (clamped there, not past it).
Also re-verified ordinary window-titlebar dragging and Notepad
(typing, mouse clicks) are unaffected by the mouse driver changes.

Screenshots: `screenshots/2026-08-10/scrollbar_p3_wheel_start_bottom.png`,
`screenshots/2026-08-10/scrollbar_p3_wheel_scrolled_up.png`,
`screenshots/2026-08-10/scrollbar_p3_wheel_back_to_bottom.png`

## Build 273 (feature, +10) -- visual draggable scrollbar widget (scrollbar phase 2/4)

Second of the four-phase scrollbar plan (build 263 did keyboard
Page Up/Page Down). This one adds an actual scrollbar: a track +
thumb on Terminal's right edge, click-to-page on the empty track,
and drag-the-thumb-to-scroll.

- `apps/widgets.h` / `apps/widgets.c`: new generic vertical scrollbar
  widget -- `widget_scrollbar_draw()`, `widget_scrollbar_hit()`
  (returns which zone a point is in: thumb / above / below / none),
  `widget_scrollbar_thumb_rect()`, and
  `widget_scrollbar_offset_for_drag()` (converts a drag position back
  into a `scroll_offset`). All four share one internal
  `scrollbar_geometry()` helper so the drawn thumb, the hit-test, and
  the drag math can never disagree with each other. Thumb height is
  clamped to a `SCROLLBAR_MIN_THUMB_H` (16px) floor so it stays
  grabbable even with a huge scrollback.
- `apps/gui_apps.h`: extended the `gui_app` callback contract with two
  new optional callbacks, `on_drag_start`/`on_drag`, so an app can
  claim a multi-tick drag gesture (like dragging a scrollbar thumb)
  instead of a single click -- mutually exclusive with `on_click` per
  button-press.
- `apps/gui_apps.c`: switched `gui_app_registry[]` from positional to
  designated initializers, since adding the two new optional fields
  would otherwise have silently misaligned every existing entry.
- `apps/wm/wm_internal.h`, `apps/wm/wm.c`, `apps/wm/wm_input.c`: added
  a `content_dragging` window-index state (parallel to the existing
  `dragging`/`resizing`) so the window manager can drive an app-owned
  content-area drag across ticks without knowing anything about
  scrollbars specifically -- it just calls `on_drag_start` on
  button-down and `on_drag` every subsequent tick the button stays
  held.
- `apps/terminal.h` / `apps/terminal.c`: wired the scrollbar in --
  reserves a `gfx_char_w()+4`-pixel strip on the content area's right
  edge (skipped below a minimum window width), draws the scrollbar
  next to the text, and added `terminal_click()` (track paging),
  `terminal_drag_start()`/`terminal_drag()` (thumb dragging). Page
  Up/Page Down from build 263 now measures against the narrowed
  text width so keyboard and scrollbar scrolling never disagree about
  how many columns are on screen.
- Notepad still not touched -- phase 4.

Found and fixed one real bug during QEMU testing: the first version of
the designated-initializer `gui_app_registry[]` entry for Terminal
left out `.on_click`, so clicking the scrollbar's empty track (page
up/down) silently did nothing.

Also hit a testing-only gotcha worth recording: `tools/qmp_test.py`'s
`QMPSession` tracks the cursor position client-side and assumes a
fixed starting point, so spawning a fresh session mid-sequence (or
sending a single very large `move_rel` instead of `goto()`'s chunked
steps) drifts the tracked position away from the real one and makes
clicks land on the wrong thing -- looks exactly like a UI bug but
isn't. Fixed by keeping precision-dependent interaction sequences
inside one `QMPSession`, and by recalibrating (drive the real cursor
to a screen corner with plenty of chunked negative movement, then
tell the tracker `pos = corner`) at the start of any script that
can't guarantee it's the first one run against a given QEMU instance.

Also ran into the Makefile's documented lack of header-dependency
tracking (see its `version:` target comment) firsthand: testing right
after resuming this session showed the Start menu's item labels
corrupted for every app but the first (`About`/`Calculator` rendered
as garbage, `Terminal` as what turned out to be raw function-prologue
bytes reinterpreted as text) -- a stale `wm_render.o` compiled against
the pre-phase-2 (smaller) `struct gui_app` layout, indexing into an
array actually built with the new, bigger layout. `make clean && make
all` fixed it immediately; this wasn't a real code bug, just a reason
to always clean-rebuild before trusting a GUI test after touching a
shared header, per CLAUDE.md's existing advice.

Tested in QEMU through the real GUI (after a clean rebuild): opened
Terminal, ran `help` several times to build up scrollback past one
screen, confirmed the thumb starts pinned to the bottom; dragged it to
the top and confirmed the view scrolled to the very start of the
output with the thumb at the top of the track; dragged it back to the
bottom and confirmed both view and thumb returned correctly; clicked
the empty track above the thumb twice and confirmed it paged up by a
screenful each time. Also re-verified Notepad, Calculator, and About
still open and work normally, and that ordinary window-titlebar
dragging is unaffected by the new `content_dragging` state.

Screenshots: `screenshots/2026-08-10/scrollbar_p2_thumb_bottom.png`,
`screenshots/2026-08-10/scrollbar_p2_dragged_to_top.png`,
`screenshots/2026-08-10/scrollbar_p2_dragged_to_bottom.png`,
`screenshots/2026-08-10/scrollbar_p2_track_click_pageup.png`,
`screenshots/2026-08-10/scrollbar_p2_regression_calculator.png`,
`screenshots/2026-08-10/scrollbar_p2_regression_normal_drag.png`

## Build 263 (feature, +10) -- keyboard Page Up/Page Down scrolling (scrollbar phase 1/4)

First of a four-phase plan (asked for scrollbars in Terminal and
Notepad, with keyboard, a visual draggable scrollbar, and mouse wheel
all wanted): the cheapest, most immediately useful piece first.

- `kernel/include/keyboard.h` / `kernel/drivers/keyboard.c`: added
  `KEY_PAGE_UP`/`KEY_PAGE_DOWN`, decoded from their `0xE0`-prefixed
  scancodes (0x49/0x51) exactly like the existing arrow keys.
- `apps/widgets.h` / `apps/widgets.c`: refactored `text_scrollback`'s
  internal "how many wrapped lines does this content have, given a
  column width" pass-1 walk out of `widget_scrollback_draw()` into a
  shared `scrollback_measure()` helper, and exposed it as
  `widget_scrollback_metrics(tb, cw, ch, &total_lines, &visible_rows)`
  -- lets a caller find the scroll range (and get `tb->scroll_offset`
  clamped to it) without rendering anything. Needed now for computing a
  sensible page-scroll step size in Terminal; will also be what a
  future visual scrollbar widget (phase 2) sizes its thumb from,
  without duplicating this walk a third time.
- `apps/terminal.c`: `on_key` now handles `KEY_PAGE_UP`/`KEY_PAGE_DOWN`
  by scrolling one screenful (`visible_rows - 1`, so consecutive pages
  overlap by a line -- an ordinary terminal-scrolling convention) via
  the existing `widget_scrollback_scroll()`.
- Notepad not touched yet -- it doesn't use `text_scrollback` at all
  today (see build 253's terminal.c entry); that conversion is phase 4.

Tested in QEMU through the real GUI: opened Terminal, ran `help` (long
enough to scroll), pressed Page Up twice and confirmed the view
scrolled up and clamped correctly at the very top (further Page Up did
nothing further), then Page Down and confirmed it returned to the
expected middle position. Screenshots below.

Screenshots: `screenshots/2026-08-10/scrollbar_p1_help_output.png`,
`screenshots/2026-08-10/scrollbar_p1_pgup_clamped_at_top.png`,
`screenshots/2026-08-10/scrollbar_p1_pgdn.png`

## Build 253 (major, +50) -- GUI terminal-emulator app: `apps/terminal.c` (terminal-emulator phase 4/4)

Last of four planned phases (see builds 183, 193, 203) toward "can we
add a terminal emulator CLI to GUI as an app" -- and the payoff: a real
`Terminal` entry in the Start menu that runs the actual shell, not a
reimplementation of it.

- `apps/terminal.h` / `apps/terminal.c` (new): a `gui_app` (registered
  in `apps/gui_apps.c`) whose window holds a `struct text_scrollback`
  (widgets.h, build 203) and a `struct vga_sink` (vga.h, build 183)
  wired to it. `on_key` builds up a line buffer character by character
  (backspace, printable chars, up/down-arrow history -- same pattern as
  shell.c's own `shell_read_line()`, just echoing into the widget
  instead of the physical console), and on Enter hands the finished
  line to `shell_dispatch()` (shell.h, build 193) with the sink
  installed, so every `vga_write()`/`vga_putc()` call the real command
  handlers make lands in this window instead of the physical screen.
  `on_draw` is a single `widget_scrollback_draw()` call sized to
  whatever the window's content area currently is -- since the widget
  reflows from scratch on every draw (see its build-203 entry), making
  the window resizable (`resizable = 1`) needed no extra code at all.
- `shell.h` / `shell.c`: added `shell_cwd()`, a read-only accessor for
  the shell's (single, shared) current-directory string, so the
  terminal's own prompt ("`/> `" etc) matches whatever the physical
  shell would show -- there's only one shell "session" in this kernel,
  so both contexts seeing the same `cd` state is correct, not a bug to
  work around.
- A short blocklist (`BLOCKED_CMDS` in terminal.c): `gui`, `run`,
  `ring3test`, `elftest`, `guitest`, `wintest`, `schedtest`, `echotest`.
  These either never return, draw straight to the physical framebuffer
  bypassing the sink entirely (SYS_WIN_*/SYS_GUI_INIT and the window
  manager's own screen takeover don't route through `vga_putc()` the
  way `SYS_WRITE` does), or would block the calling context (and thus
  freeze the whole window manager, not just this window) for their
  entire run. Typing one prints an explanation instead of calling
  `shell_dispatch()`. Everything else -- `ls`/`cd`/`cat`/`echo`/etc,
  and even the other ring-3 test commands (`crashtest`, `filetest`,
  `newsyscalltest`, `syscalltest`, `writetest`, `ptrtest`) and `reboot`
  -- runs for real, unmodified, because `SYS_WRITE` already goes
  through `vga_putc()`, which already respects the active sink (build
  183) with zero terminal-specific code needed.

Tested in QEMU, all through the real GUI (`gui` -> Start -> Terminal,
no test scaffolding this time -- the shipped app itself): opened the
window and confirmed the banner+prompt; ran `about` and `ls` and got
real kernel/filesystem output back (not a stub); ran `echo hello from
terminal` and got it echoed correctly; typed `gui` and got the
blocked-command message instead of a frozen window manager -- and
confirmed the WM kept responding afterward, proving the real `gui`
command was never actually invoked; typed `xyzabc`, backspaced it down
to `xyz`, pressed Enter (got "Unknown command: xyz", the real shell's
own error message), then pressed Up and confirmed `xyz` came back via
history; pressed Esc out of the GUI entirely and confirmed the physical
console shell still works normally afterward (its own `about` output
matched, unaffected by anything the terminal window did).

Screenshots: `screenshots/2026-08-10/phase4_terminal_open.png`,
`screenshots/2026-08-10/phase4_real_commands_running.png`,
`screenshots/2026-08-10/phase4_blocked_command_message.png`,
`screenshots/2026-08-10/phase4_backspace_and_history.png`,
`screenshots/2026-08-10/phase4_physical_shell_regression_check.png`

## Build 203 (feature, +10) -- reusable scrollback text widget (terminal-emulator phase 3/4)

Third of four planned phases toward a GUI terminal-emulator app (see
build 183's entry for the overall plan). Phases 1-2 made the shell's
real dispatcher safe and reachable from a GUI callback; this phase adds
the piece needed to actually *show* what it writes inside a window --
there was no reusable scrollback/text-cell widget anywhere in the
codebase (apps/notepad.c's ad hoc `char[]` + manual line-wrap loop was
the closest thing, and it has no scrolling and only ever draws a
single fixed color).

- `apps/widgets.h` / `apps/widgets.c`: added `struct text_scrollback` --
  a fixed-capacity (`SCROLLBACK_CAP` = 8192 cells) ring buffer of
  `{char, vga_color}` cells with `widget_scrollback_init/putc/
  backspace/clear/set_color/scroll/draw()`. Appending past capacity
  overwrites the oldest cell in place (O(1), no shifting) rather than
  growing or shifting the buffer, matching the physical console's own
  scrolloff behavior. Line wrapping isn't cached -- `draw()` walks the
  raw character stream twice on every call (once to find the total
  wrapped-line count and clamp the scroll position, once to render the
  visible window), the same "recompute from source, don't cache and
  invalidate" tradeoff notepad_draw() already makes, just with
  scrolling added. `scroll_offset` (0 = pinned to the newest output,
  like a normal terminal; >0 = scrolled up that many wrapped lines) is
  clamped to the buffer's actual current line count inside `draw()`
  itself, so a caller never needs to know the content size to request a
  scroll.
- `kernel/include/vga.h` / `kernel/drivers/vga.c`: added `vga_color_rgb()`,
  a public wrapper around vga.c's existing (previously file-private)
  color palette table, so the widget renders each cell in the exact
  same RGB the physical console would have used for that `vga_color`
  rather than picking its own colors that would look inconsistent next
  to it.
- No existing code changed -- purely additive. `apps/notepad.c` (the
  only other file touching `widgets.h`) needed no changes; its own
  `widget_button()` calls are untouched.

Tested in QEMU: temporarily wired a throwaway `widgettest` shell
command (draws a text_scrollback with 40 lines of wrapped, per-line
colored sample text straight onto the framebuffer, bypassing the WM --
stripped back out before this build was finalized, not part of the
shipped diff) and stepped through three states, screenshotted at each:
pinned to the bottom with the cursor visible; scrolled up 15 lines with
the cursor correctly hidden (it's outside the visible window); and
re-pinned plus 20 characters backspaced, confirming the write position
moved back correctly. Wrapping, per-cell color, and scrolling all
matched expectations in every screenshot. Also re-verified
`apps/notepad.c`'s Save/Load toolbar buttons (the pre-existing
`widget_button()` consumer) render and open correctly after the
`widgets.h` changes -- confirms the new addition didn't disturb the
widget file's existing user.

Screenshots: `screenshots/2026-08-10/phase3_scrollback_wrap_color_cursor.png`,
`screenshots/2026-08-10/phase3_scrollback_scrolled_no_cursor.png`,
`screenshots/2026-08-10/phase3_scrollback_backspace_repin.png`,
`screenshots/2026-08-10/phase3_widget_button_regression_check.png`

## Build 193 (feature, +10) -- exported shell dispatch + blocking-command isolation (terminal-emulator phase 2/4)

Second of four planned phases toward a GUI terminal-emulator app (see
build 183's entry for phase 1 and the overall plan). This phase makes
shell.c's real command dispatcher actually *safe and reachable* to call
from a non-blocking GUI callback, which phase 1's sink alone didn't
guarantee -- two commands used to block on keyboard input mid-command,
which would hang a GUI app calling in.

- `apps/shell.h` / `apps/shell.c`: added `void shell_dispatch(char
  *line, const struct vga_sink *sink)` -- installs `sink` (via
  `vga_set_sink()`), runs `line` through the exact same `dispatch()`
  shell_main()'s REPL uses, then restores whatever sink was active
  before. This is the reuse path a terminal-emulator app calls into
  instead of duplicating shell.c's ~40 command handlers.
- `console_page()` (used by `help`'s pagination) and `cmd_timezone()`'s
  no-args interactive picker both used to block on `keyboard_getchar()`/
  `keyboard_read_line()` mid-command -- harmless for the interactive
  console loop (which already blocks on its own read loop between
  commands anyway) but fatal for a GUI app's non-blocking `on_key`
  callback driving `shell_dispatch()`, since there's nowhere for that
  blocking read to safely happen. Both now check the new
  `vga_sink_active()` (added to vga.h/vga.c) and skip the blocking part
  when a sink is installed: `console_page()` just dumps every line
  unpaginated (a GUI caller's own scrollback widget, phase 3, handles
  "doesn't fit on one screen" instead), and `cmd_timezone()` prints the
  city list plus a pointer to `timezone <city>` instead of prompting.
  Physical-console behavior (no sink installed, the only mode that
  exists until phase 4) is completely unchanged -- both gates are
  no-ops when `vga_sink_active()` is false.
- Known caveat for phase 4 to handle, not fixed here: some commands
  reachable through `dispatch()` don't return at all (`ring3test`,
  `elftest`, `reboot`) or take over the whole physical screen directly
  via `gfx_*` calls rather than going through the sink (`gui`,
  `guitest`, `wintest`, `schedtest`'s ring-3 processes) -- calling
  those from inside a terminal app's `on_key` callback would freeze or
  visually clobber the GUI, not do anything useful. The terminal app
  itself will need to special-case or block a short list of these
  rather than handing everything through unfiltered.

Tested in QEMU: temporarily wired a throwaway `sinktest` command (a
capturing sink, stripped back out before this build was finalized --
not part of the shipped diff) that ran `meminfo`, `timezone` (no args),
and `help` through `shell_dispatch()` back to back. All three returned
without hanging -- proving both blocking-command gates actually engage
under a sink instead of just compiling -- and the captured text matched
each command's real output, then flushed cleanly to the physical
console once the sink was restored afterward. Also re-ran `help`'s
pagination and `timezone`'s interactive picker on the physical console
(no sink) after stripping the test code back out, confirming zero
behavior change there -- both screenshots below.

Screenshots:
`screenshots/2026-08-10/phase2_sinktest_capture_no_hang.png`,
`screenshots/2026-08-10/phase2_physical_console_regression_check.png`

## Build 183 (feature, +10) -- vga.c output-sink redirection (terminal-emulator prerequisite, phase 1/4)

First of four planned phases toward a GUI terminal-emulator app (asked
"can we add a terminal emulator CLI to GUI as an app"). Scoped this out
with a research pass first: shell.c's ~40 command handlers are all
hard-wired to the global `vga_*` console singleton, and reusing them
from a GUI window meant either duplicating them (fast but permanent
drift from the real shell) or giving the console a pluggable output
target. Chose the latter -- more upfront work, but the terminal app
ends up running the *actual* shell dispatcher, not a copy.

- `kernel/include/vga.h` / `kernel/drivers/vga.c`: added `struct
  vga_sink` (`putc`/`backspace`/`clear`/`set_color`/`rows` callbacks
  plus a `void *ctx`) and `vga_set_sink()`. When a sink is installed,
  every `vga_putc`/`vga_write`/`vga_clear`/`vga_backspace`/
  `vga_set_color`/`vga_rows` call routes to it instead of the physical
  console (legacy 0xB8000 or framebuffer text backend) -- transparently,
  with zero changes needed to any of shell.c's existing command
  handlers, since they only ever call the same `vga_*` functions they
  already did. `vga_write_dec`/`vga_write_hex`/`vga_write_exit_code`
  needed no changes at all, since they're built entirely on top of
  `vga_putc`/`vga_write`. Only one sink can be active at a time (no
  stack -- nothing needs nesting yet); `vga_set_sink(0)` restores the
  physical console, and `vga_set_sink()` returns the previously-active
  sink so a caller can nest safely if a future need arises.
  `vga_cursor_tick()` is a no-op while a sink is active (a sink owns
  its own cursor presentation, or has none -- nothing for the physical
  console's blink logic to do). Also added `vga_current_fg()` (reads
  the physical console's current color; sinks track their own via
  `set_color` if they implement it) for phase 4's terminal app to use
  when initializing its own color state.
- No behavior change with no sink installed (the only mode that
  exists until phase 4 wires one up) -- every `vga_*` call takes the
  exact code path it always did.

Tested in QEMU: `about`/`meminfo` on the physical console produce
identical output to before (screenshot below) -- confirms the sink
plumbing adds a branch on an always-null pointer and nothing else
changes. No sink-active behavior to test yet; that lands with phase 2
(exporting shell's `dispatch()` with a sink parameter) and phase 4
(the terminal app that actually installs one).

Screenshot: `screenshots/2026-08-10/phase1_vga_sink_regression_check.png`

