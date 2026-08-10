# Changelog

All notable changes to toy-os, in the order they happened. Each entry
notes what was added and, where relevant, what broke and how it got
fixed -- several of the more interesting bugs here were only found by
actually testing in QEMU rather than assumed to work.

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

## Build 173 (major, +50) -- process exit/teardown: a crashed ring-3 process no longer halts the kernel

The long-standing "Ideas for what's next" item: `ring3test`/`elftest`
have always required a full reboot after their deliberate fault, since
there was no way to free a process's resources and hand control back.
Picked this off the list when asked "what can we add or change now,"
over a couple of other candidates (reusable GUI widgets, Notepad
save-as).

**Added (`kernel/core/vmm.c`/`vmm.h`, `vmm_destroy_address_space()`):**
walks a PML4's entries 1..511 (deliberately skipping entry 0, the
shared kernel identity map every address space points at the SAME
physical PDPT for -- see `vmm_create_address_space()`), freeing every
present page-table page at every level plus every leaf frame they map,
then frees the PML4 itself. The tear-down mirror of the existing
`vmm_map_user_page()`/`ensure_next_level()` allocation path. Caller
must switch CR3 away first (freeing the frame CR3 still points at is a
use-after-free the instant the next `pmm_alloc_frame()` reuses it).

**Added (`kernel/core/syscall.c`, `syscall_process_exit_cleanup()`):**
the other half of teardown -- kernel-side bookkeeping that isn't part
of any address space and so wouldn't be freed by
`vmm_destroy_address_space()` alone: open fds (`fd_table`), and the
single-slot heap/window "armed for this pml4" globals (the pages
themselves ARE part of the address space, since both `SYS_SBRK` and
`SYS_WIN_CREATE` just map pages into the calling process's own page
tables -- nothing extra to free there). Switches CR3 back to the
kernel's own address space first, then calls
`vmm_destroy_address_space()`.

**Changed (`kernel/core/syscall.c`'s `SYS_EXIT` handler):** now calls
the cleanup above before handing off to `scheduler_on_exit()` or the
legacy restore path -- fixes a **pre-existing leak**: a normal
`exit()` never freed its address space either, before this. Only
visible before now because nothing ever reused that memory; `meminfo`
confirms it's fixed (see Tested below).

**Added (`kernel/core/idt.c`'s fault handler):** a ring-3 fault
(`CS & 3 == 3`) is now recoverable IF there's actually somewhere to
recover to -- a scheduler-managed process (`scheduler_on_exit()`
already knows how to pick the next one), or a `process_run_ring3()`
call with its context still armed (new: `process_context_is_armed()`,
`kernel/include/process.h`). When recoverable: prints
`*** RING-3 PROCESS CRASHED: <exception> ***` (was always
`*** KERNEL PANIC ***` before, even for a plain ring-3 bug) with the
same RIP/CS/error_code/CR2 dump as before, still calls the existing
`ring3_hook` diagnostic mechanism unchanged, then runs the cleanup
above and either `scheduler_on_exit(-1)` or the new
`process_context_recover()` (process.h/process.c -- same
`process_context_restore()` mechanism `SYS_EXIT` already used, reached
from idt.c instead). **`ring3_test.c`/`elf_test.c` are unaffected** --
both drop to ring 3 with their own raw, manual iretq instead of
`process_run_ring3()`, so nothing is ever armed during their fault;
they still halt exactly as before (their on-fault messages were
updated to explain why, since the old "recovery doesn't exist yet"
wording became inaccurate). A genuine ring-0 (kernel-mode) fault always
still halts too -- a kernel bug staying fatal is correct, not a gap
this closes.

**Added (`process.h`, `#define PROCESS_CRASHED (-2)`):** the sentinel
`process_run_ring3()` returns when its process was recovered from a
fault instead of exiting cleanly. Introduced the first negative return
value that scheme has ever produced, which exposed a real display bug:
every `*test` command's trailer (`echo_test.c`, `file_test.c`,
`gui_test.c`, `newsyscalls_test.c`, `syscall_test.c`, `win_test.c`,
`write_test.c`) printed the exit code via
`vga_write_dec((uint32_t)exit_code)`, which would have shown
`4294967294` for -2, not "-2". **Fixed:** new `vga_write_exit_code()`
(`vga.c`/`vga.h`) prints a real (non-negative) code normally, or
"CRASHED" for any negative one -- deliberately generic rather than
importing `process.h`'s exact constant, so a driver-layer file doesn't
depend on kernel/core. All seven call sites switched over; their
"exited cleanly" phrasing (no longer always true) changed to the
neutral "finished."

**Added (`userland/crash_test.c`, `kernel/core/crash_test.c`, new
files; wired in as the shell's `crashtest` command, the thirteenth
Multiboot2 module):** deliberately writes through a wild pointer at a
fixed low address (present in every process's shared kernel identity
map, but never user-accessible, so this reliably page-faults the same
way a real null-pointer bug would) to exercise the whole recovery path
end to end -- this is the first ring-3 fault in the project's history
that DOESN'T require a reboot to recover from.

**Tested in QEMU:** `crashtest` -- fault caught, banner shows
"RING-3 PROCESS CRASHED: Page fault", CR2 correctly shows the wild
address, process torn down, shell resumes immediately, exit code shown
as "CRASHED." Ran `crashtest` 10 times in a row and compared `meminfo`'s
free-frame count before and after: identical (63071 free both times) --
proof every crash's address space is genuinely freed, not leaked.
Regression: `schedtest` (scheduler-managed exit) and `filetest`/
`newsyscalltest` (legacy exit) still complete normally with exit code 0
after a batch of crashes, confirming `fd_table`/heap/window state isn't
corrupted by the new cleanup path. `ring3test` still halts with the
unchanged `*** KERNEL PANIC ***` banner, confirming the new recovery
path correctly stays out of its way.

## Build 123 (fix, +1) -- gitignore `Makefile.new` too

Same issue as the previous entry's `_to_delete/`, spotted by the user
right after that fix landed: `Makefile.new` (the hand-off artifact used
because `Makefile` itself is a protected file the device bridge won't
write to -- see `CLAUDE.md`) had also been swept into the repo by the
same `git add -A`.

**Changed (`.gitignore`):** added `/Makefile.new`. Same caveat as
`_to_delete/`'s entry -- doesn't retroactively drop what's already
committed; see this build's git commands for the one-time
`git rm --cached`.

## Build 122 (fix, +1) -- gitignore the `_to_delete/` device-bridge scratch folder

`_to_delete/` (see `CLAUDE.md`'s "device bridge can't delete files"
note -- a holding spot for files `device_bash` moves aside since it
can't actually `rm` them off the user's machine) had gotten swept up
into the very first push and ended up sitting in the GitHub repo,
which isn't where scratch cleanup state belongs.

**Changed (`.gitignore`):** added `/_to_delete/`. Doesn't retroactively
remove what's already committed on its own -- see this build's
CHANGELOG-adjacent git commands for the one-time `git rm -r --cached`
needed to actually drop it from the repo.

## Build 121 (fix, +1) -- git tag + GitHub Release convention for BUILD_NUMBER bumps

Now that the repo is actually pushed to GitHub (`Drenos/toy-os`, private
-- see the last few entries), asked what "correct versioning" looks
like with a real remote in the loop, since `BUILD_NUMBER` alone doesn't
say which commit a given build number IS. Offered a couple of choices:
tag every bump vs. only major ones vs. no tags at all; and GitHub
Releases (with the built `.iso` attached) for every bump, major bumps
only, or none. Picked "tag every bump" + "release major bumps only".

**Convention (documented in `CLAUDE.md`, no code changes):** every
commit that lands a `BUILD_NUMBER` bump gets a matching git tag,
`build-<N>` (e.g. `build-121`), pushed alongside the commit (`git tag
build-<N> && git push origin main --tags`) -- makes "what commit was
build 121?" a `git show build-<N>` away instead of cross-referencing
commit dates against this file. Major (+50) bumps additionally get a
GitHub Release (title `build-<N>`, body = that bump's CHANGELOG
section, `.iso` attached) so a working ISO of a real milestone is
downloadable without cloning + building; fix/feature bumps get the tag
but no release -- not worth a standing download for a small change.

## Build 120 (feature, +10) -- four new syscalls: SYS_UNLINK, SYS_LISTDIR, SYS_GETTIME, SYS_YIELD

Asked for "a couple more syscalls" as a way to try out the new
GitHub-push workflow end to end; offered four candidates that each
wrap an existing kernel capability that wasn't reachable from ring 3
yet, picked all four rather than narrowing to two.

**Added (`kernel/include/syscall_abi.h`, numbers 12-15):**
- `SYS_UNLINK` -- wraps `fs_delete()` (fs.c). RDI = path pointer.
  Returns 1/0.
- `SYS_LISTDIR` -- wraps `fs_list()` (fs.c). RDI = dir path pointer,
  RSI = pointer to a `struct dirent[]` output array, RDX = its
  capacity (clamped to the new `SYS_LISTDIR_MAX`, 32 -- matches
  `FS_MAX_FILES`). Returns the entry count written, or -1 on a bad
  pointer. `fs_list()`'s callback has no context/userdata parameter,
  so the handler bounces through file-scope globals for the duration
  of one call (same non-reentrancy assumption the SYS_WIN_* state
  already relies on) rather than changing fs.c's signature.
- `SYS_GETTIME` -- wraps `rtc_read_local()` (tz.c) -- the same
  timezone-adjusted wall clock the shell's `time` command and taskbar
  clock already show. RDI = pointer to a `struct rtc_time` (out).
  Returns 1/0.
- `SYS_YIELD` -- no arguments. Reuses `scheduler_tick()` (scheduler.c)
  directly rather than inventing a second reschedule path: the
  syscall handler's `regs` pointer is laid out identically to what
  idt.c hands `scheduler_tick()` on a real 100Hz timer IRQ, so calling
  it from `int 0x80` is indistinguishable from "the timer happened to
  fire right now." A clean no-op (always returns 0) for processes
  outside the preemptive scheduler -- i.e. every existing *test
  command, which all use the older `process_run_ring3()` path -- since
  there's nothing else for those to yield to.

**Added (`userland/newsyscalls_test.c`, `kernel/core/newsyscalls_test.c`,
new files; wired into `grub.cfg`/Makefile as the eleventh Multiboot2
module, and into the shell as `newsyscalltest`):** a combined test
program exercising all four in turn -- create+delete+confirm-gone for
unlink, list `/etc` for listdir, sanity-range-check the returned clock
fields for gettime, and confirm a clean no-op return for yield.
Verified in QEMU: all four phases pass (`newsyscalltest`), and `help`
lists the new command.

**Fixed:** `CHANGELOG.md` and `README.md` had silently gone missing
from the cloud sandbox's checkout somewhere before this session
(neither showed up in `git status`/`git ls-files` after the git-init
work) -- caught only because `make` doesn't touch either file, so
nothing failed loudly. Both are the real, full versions again (this
entry included) as of this commit; the earlier GitHub merge commit
that's already pushed is missing them, so this commit's push is what
actually gets them onto GitHub for the first time.

## Build 110 (feature, +10) -- Windows-style build number, replacing the date scheme

The date-plus-build-counter scheme from two entries back had a real
flaw the user put a finger on: it bumps on literally every `make`, so
it can't tell you which build actually has new code versus which one
is just a rebuild of the same source -- and it gives no sense of how
big a change was. Asked for something more like Windows' build numbers
(22631-style): a single incrementing integer, calculated "every time
you make changes," bigger jumps for bigger changes. Offered a few
choices: keep the date scheme alongside a new number, or replace it
outright (replace); start the counter low and honest or high to mimic
Windows' look (start low, 100); and how to decide each change's
increment -- freeform judgment or fixed named tiers (fixed tiers).

**Added (`tools/bump_build.sh`, new file):** `tools/bump_build.sh
<fix|feature|major>` bumps `BUILD_NUMBER` (repo root, plain integer,
replaces the old `BUILD_VERSION`) by +1/+10/+50. This is the one piece
that can't be automatic -- a bare `make` has no way to know "typo fix"
from "new subsystem" -- so it's a deliberate step run once per real
change (by whoever/whatever is making the change), not once per build.
The tier and delta belong in the change's CHANGELOG entry too, e.g.
this entry's own title.

**Changed (`tools/gen_version.sh`):** no longer generates or increments
anything itself -- just reads `BUILD_NUMBER` and embeds it in
`kernel/include/version.h` as `TOYOS_VERSION`, still run automatically
as the first step of `make all`/`make iso` (same Makefile wiring, same
force-recompile of `about.o`/`shell.o` for the same header-dependency-
tracking-gap reason as before -- see `CLAUDE.md`).

**Changed (`apps/about.c`, `apps/shell.c`):** both `about` screens now
read "toy-os build 110" instead of "toy-os v110" -- "build" matches the
new number's meaning (Windows-style build number, not a semver-ish
version) better than the old "v" prefix did. `about.c`'s `ABOUT_COLS`
shrunk from 44 to 38 -- a bare build number is much shorter than the
date string it replaced, so the About window needs less width for the
version line without wasting the space genuinely long lines elsewhere
need.

**Changed (`CLAUDE.md`, `screenshots/README.md`, `Makefile`):** updated
to describe the new scheme and point at `tools/bump_build.sh` as the
step to run before a final build, in place of the old
"it's fully automatic, never touch it" guidance.

**Verified in QEMU:** built with `BUILD_NUMBER=100` (this scheme's
starting point) then bumped to 110 via `tools/bump_build.sh feature`;
confirmed both the shell's `about` and the GUI About window show
"build 110" and that the About window's width still comfortably fits
the shorter version line with no clipping.

## Fixed-size windows, resize cursors, and a subtle window border

Three related GUI polish requests: make Calculator's window fixed-size
(its button grid has no sensible way to fill extra space, unlike
Notepad's text area or About's static text -- and it doubles as the
first case of a *per-app* window behavior setting, not something the
window manager decides on its own), show a resize cursor over a
resizable window's edge/corner, and add a subtle border to windows.
Asked for a few choices on each: whether Calculator's maximize button
should also be disabled (yes) or just resizing; whether the resize
cursor should be direction-specific (↔/↕/corner) or one generic icon
(direction-specific); and whether "subtle border" meant a 3D bevel, a
drop shadow, or just a crisper flat outline (bevel).

**Added (`apps/gui_apps.h`, `apps/gui_apps.c`):** a `resizable` field on
`struct gui_app` -- 1 for Notepad/About, 0 for Calculator. When 0: no
resize grip, hovering an edge shows the normal cursor instead of a
resize one, dragging an edge does nothing, and the maximize button is
drawn muted and does nothing when clicked (focuses the window like any
other click on it, but doesn't touch its geometry -- chosen over
removing the button so the title bar layout doesn't shift between
fixed and resizable apps).

**Changed (`apps/wm/wm_input.c`):** factored the resize-hit-testing that
used to live inline in `wm_handle_left_click()` out into
`wm_find_resize_zone()` (declared in `wm_internal.h`) -- it's now
called both by the click handler (to start a drag) and by
`wm_render.c` every frame (to decide the hover cursor), instead of two
copies of the same "is this point over a resizable window's edge"
logic. Also gated the maximize button's click handling on
`app->resizable`.

**Added (`apps/wm/wm_render.c`):** three new hand-drawn cursor icons
(`draw_cursor_h`/`_v`/`_diag`, plus a `wm_cursor_kind` enum in
`wm_internal.h`) -- same "tapering wedge with an outlined edge"
construction as the existing normal-pointer cursor, just oriented
horizontally, vertically, or diagonally. `wm_render_frame()` picks
which one to show each frame from `wm_find_resize_zone()` (or, while a
resize is actively in progress, from which edge(s) that drag started
on, so the cursor doesn't flicker back to normal if the mouse outruns
the window edge mid-drag).

**Changed (`apps/wm/wm_render.c`):** window borders are now a subtle
1px 3D bevel (light highlight top/left, dark shadow bottom/right)
instead of a flat single-color outline -- kept as local constants in
`draw_window_chrome()` rather than named `THEME_*` colors, since
nothing else draws a bevel yet (see `theme.h`'s own "don't name a color
that isn't used twice" convention).

**Bug found and fixed along the way:** the resize grip hint (the little
corner marks) was being drawn as part of window chrome, *before* the
app's `on_draw()` -- which repaints its entire content area every
single frame (e.g. `notepad_draw()`'s background fill), painting
straight over the grip a moment after it was drawn. It's likely been
invisible since resizing was first added, unrelated to this change --
just never noticed because a resize still worked by dragging blind
into the last few pixels of the window. Fixed by moving the grip to
its own `draw_resize_grip()`, called after each window's `on_draw()`
in `wm_render_frame()`'s loop instead of inside `draw_window_chrome()`.

**Verified in QEMU:** opened Notepad (resizable) and Calculator (fixed)
side by side. Confirmed Calculator's maximize button renders muted and
does nothing when clicked, dragging its corner doesn't resize it, and
hovering its corner shows the normal cursor rather than a resize one.
Confirmed Notepad's resize grip is now actually visible in the
bottom-right corner (the pre-existing bug above), and that hovering its
right edge, bottom edge, and corner show the horizontal, vertical, and
diagonal resize cursors respectively.

## Font size now persists across reboots

`fontsize` only ever changed the console for the current boot -- the
user asked for it to be saved under `/etc` like `timezone` already is,
and specifically asked for a config file that reads like one
(`font_size=tiny`, not just a bare `tiny`), anticipating more settings
sharing `/etc` someday. Offered three choices: a new key=value file
just for this setting, one shared key=value `/etc/config` file for
every setting (bigger change, would also mean migrating `tz.c`), or a
new file matching `/etc/timezone`'s existing bare-text format exactly.
Went with the first -- new file, key=value -- as the smallest change
that gets the requested format without touching the already-working
timezone code.

**Added (`kernel/include/font_config.h`, `kernel/core/font_config.c`,
new files):** `font_config_init()` (loads `/etc/fontsize` if present
and applies it via `gfx_set_font_size()`) and `font_config_save()`
(persists a size as `font_size=<name>\n`) -- same split as `tz.c`'s
`tz_set_index()` (selects + persists) vs `rtc_read_local()` (applies).
Deliberately a small single-purpose parser (scans for one `=`, reads
the value up to a newline) rather than a general key=value reader --
per this codebase's "wait for a second real caller" convention (see
`apps/widgets.h`'s top comment for the same reasoning applied
elsewhere), the point to factor out a shared config-line parser is
when a *second* setting wants this treatment, not preemptively.

**Changed (`kernel/core/kernel.c`):** boot sequence now calls
`font_config_init()` right after `tz_init()`, then `vga_reflow()` to
apply whatever size was loaded to the console's cell layout (a no-op,
console-layout-wise, if nothing was ever persisted -- `vga_reflow()`
just recomputes `console_cols`/`console_rows` from whatever the current
size already is).

**Changed (`apps/shell.c`):** `cmd_fontsize()` now calls
`font_config_save()` right after `gfx_set_font_size()`/`vga_reflow()`
on every successful `fontsize <size>`.

**Changed (`kernel/include/kapi.h`):** added `font_config.h` to the
apps-facing include aggregate (not actually used by any app yet, but
`tz.h` sits there for the same "future app might want it" reason).

**Verified in QEMU:** `fontsize` with no size still reports "currently:
small" on a fresh disk (no `/etc/fontsize` yet, matches the old
in-memory-only default); `fontsize tiny` then `cat /etc/fontsize`
showed exactly `font_size=tiny`; `reboot` afterward booted straight
into tiny font, and `fontsize` (no args) after reboot reported
"currently: tiny" -- confirming the persisted value survives a reboot
and applies before the shell prompt even appears, not just on-demand.

## GUI windows now size themselves to the active font

The user noticed About and Calculator had a lot of empty space at the
"tiny" font size, and asked whether GUI windows could adapt to font
size instead. They did have to -- confirmed the cause was that every
`gui_app_registry[]` entry (`apps/gui_apps.c`) had a fixed pixel
`default_w`/`default_h`, sized generously enough to fit each app's
content at the *largest* baked font so nothing ever clipped. That meant
smaller fonts just left the leftover margin empty rather than the
window shrinking to match -- exactly what the screenshots showed.
Confirmed scope with the user (all three GUI apps vs. just the two
flagged) before implementing; went with all three for consistency.

**Changed (`apps/gui_apps.h`):** `struct gui_app`'s fixed `default_w,
default_h` fields replaced with a `default_size(int *w, int *h)`
function pointer each app implements. It's called once, at window-open
time (`apps/wm/wm.c`'s `open_app()`), and computes the content-area
pixel size from whatever font is active right then
(`gfx_char_w()`/`gfx_char_h()`). Font size can only change from the
shell before `gui` runs -- there's no live in-GUI font picker -- so this
fully covers it with no live-resize plumbing needed.

**Changed (`apps/about.c`):** new `about_default_size()` computes size
from `ABOUT_COLS`/`ABOUT_ROWS`/`ABOUT_MARGIN`/`ABOUT_LINE_GAP` macros,
the same constants `about_draw()` already used inline (pulled out to
macros so the two can't drift apart). `ABOUT_COLS` (44) is sized with
headroom past the current longest line, since the version line's length
now grows slowly over time (see the previous entry's date/build-id
version scheme).

**Changed (`apps/calculator.c`):** new `calculator_default_size()`
literally reuses `button_rect()`'s own `MARGIN`/`BTN_W`/`BTN_H`/
`BTN_GAP`/`DISPLAY_H`/`DISPLAY_GAP` macros to compute the exact content
size the button grid needs -- there was no risk of the two drifting
apart since it's the same arithmetic, just summed instead of walked
per-button.

**Changed (`apps/notepad.c`):** new `notepad_default_size()` -- Notepad's
text area was already fully dynamic (reflows to whatever size the
window actually is), so this just picks a sensible starting size
(`NOTEPAD_COLS`/`NOTEPAD_ROWS`, new macros) rather than fixing a real
layout constraint like the other two apps.

**Changed (`apps/wm/wm.c`):** `open_app()` now calls
`app->default_size(&content_w, &content_h)` instead of reading
`app->default_w`/`default_h` directly.

**Tool fix along the way (`tools/qmp_test.py`):** `goto()`'s single
relative mouse-move event silently lost precision on large jumps (e.g.
from the default start position straight to a taskbar button) --
traced to the PS/2 relative-mouse protocol encoding each packet's delta
as a signed byte, so a single `input-send-event` bigger than that range
gets truncated/wrapped by QEMU's PS/2 emulation. Fixed by chunking
`goto()`'s movement into <= 100px steps; confirmed fixed by screenshotting
the cursor landing exactly on target after a large jump, where it had
previously landed tens of pixels off.

**Verified in QEMU** at all three affected font sizes (tiny, medium,
large): opened About, Calculator, and Notepad at each size and
screenshotted. Tiny: all three windows now fit their content snugly
(About's text fills the window instead of leaving ~40% blank below it;
Calculator's button grid fills the window edge-to-edge). Medium: sized
proportionally larger, still no wasted space. Large: no clipping or
overflow in either About's text or Calculator's button labels --
confirms the fix also incidentally fixed a latent overflow risk at
large font that the old fixed-520px About window would have hit once
`TOYOS_VERSION` grew past a certain length (the fixed width was sized
assuming the old shorter `0.x.0`-style version string).

## Replaced hand-bumped semver with an auto-generated date/build-id version

`TOYOS_VERSION` had been a hand-edited `0.1.0`-style string since the
very start, bumped manually for every real change per standing project
practice. Since this is a development version rather than something
with real releases, the user asked to replace that with something more
like a build id -- offered a choice between a bare date, a date with a
same-day counter, a full date+time, or keeping the `0.x.0` look with a
date suffix, and between hand-bumping the new format or generating it
automatically at build time; landed on a date-plus-same-day-counter
string (`YYYY.MM.DD.N`), generated automatically.

**Added (`tools/gen_version.sh`, new file):** regenerates
`kernel/include/version.h` with today's date plus a counter that
persists in a new `BUILD_VERSION` file at the repo root -- resets to 1
on a new date, otherwise increments. Counts *builds*, not "real
changes" (running `make` twice with nothing changed still bumps it
twice) -- the deliberate tradeoff for "fully automatic, never
forgotten" over the old scheme, which tracked meaningful changes but
depended on remembering to bump it by hand (and didn't always happen,
per this file's own earlier entries).

**Changed (`Makefile`):** new `version` target (always runs, being
`.PHONY`) runs the script and is now the first prerequisite of both
`all` and `iso`. Also force-deletes `build/apps/about.o` and
`build/apps/shell.o` -- the only two files that embed `TOYOS_VERSION` --
since this Makefile has no header-dependency tracking at all (no
`-MMD`/`-MP`; see `CLAUDE.md`'s existing "make clean && make all"
testing advice, which exists because of exactly this gap). Without
that forced deletion, a version meant to change on literally every
build would often keep showing a stale number from whenever those two
files last happened to recompile for an unrelated reason -- confirmed
this was a real problem before adding the fix (a second `make all` with
no source changes left the old version baked into `kernel.bin` even
though `version.h` itself had already updated), and confirmed fixed
after (`strings kernel.bin` matched `BUILD_VERSION` exactly across
several consecutive builds with nothing else touched).

**Changed (`kernel/include/version.h`, GENERATED):** no longer
hand-edited -- see its own new top comment.

**Changed (`CLAUDE.md`, `screenshots/README.md`):** updated the
"bump the version" guidance to "don't, it's automatic now," and
switched the `screenshots/` convention from one subfolder per
`TOYOS_VERSION` to one per calendar date -- a per-build version string
would otherwise mean a near-new folder every test run instead of one
per testing pass.

**Verified in QEMU** (quick pass only, per the user's request going
into this rather than the usual full testing round): confirmed the
counter increments correctly across repeated `make all`/`make iso`
calls and resets logic is date-keyed; booted once and ran `about`,
which showed `toy-os v2026.08.09.6` matching `BUILD_VERSION` at the
time.

## A fourth, smaller "tiny" font size, and a version bump to 0.7.0

The console had three baked font sizes (small/medium/large, smallest
first at 11x22px). User asked whether the font could go smaller still;
given a choice between a moderate ~9x18px step down and a more
aggressive ~7x14px one, and between adding a new size vs. shrinking
"small" itself, went with adding a genuinely new "tiny" size at a
moderate ~9x18px rather than touching the existing three.

**Changed (`tools/genttf.py`):** added `("tiny", 16, 9, 18, 15)` to the
front of the `SIZES` list that drives font baking -- pixel size and
baseline picked empirically the same way the existing three were
(rendered a few candidates offline, checked for descender/ascender
clipping on the full ASCII range, same "minor deliberate descender
clipping" compromise already documented for the other sizes: `,`, `;`,
`@`, `Q`, `g`/`j`/`p`/`q`/`y`, and bracket/brace bottoms touch the cell
edge slightly, nothing else does). Re-running the generator against the
existing small/medium/large entries produced byte-identical output for
all three -- confirmed with a diff showing zero removed lines, only the
new tiny glyph table inserted -- so this is purely additive.

**Changed (`kernel/drivers/font_ttf.c`, `kernel/include/font_ttf.h`,
GENERATED):** regenerated via `tools/genttf.py` -- `FONT_SIZE_TINY`
added as a new first enum value (nothing hardcodes the old numeric
values, every caller uses the symbolic names, so this didn't need any
other file to change just from the enum shifting).

**Changed (`apps/shell.c`):** `fontsize` now accepts `tiny` alongside
the existing three, in both the usage message and `help`'s listing.
Default stays `small` (`gfx.c`'s `cur_font_size` initializer untouched)
-- `tiny` is opt-in, not a new default.

**Changed (`kernel/include/gfx.h`):** doc comments updated from "one of
the three baked sizes" to four, and `gfx_set_font_size()`'s comment
lists `TINY` alongside `SMALL/MEDIUM/LARGE`.

**Verified in QEMU** (screendumps via `tools/qmp_test.py`): `fontsize
tiny` followed by `help` rendered the whole command list sharp and
legible at the smaller size (zoomed crop confirmed clean anti-aliased
glyphs, no visible artifacting); noted that `gui` mode renders at its
own fixed size independent of the shell's `fontsize` (pre-existing
behavior, confirmed via a "Start" button width measurement matching
`small`'s 11px cell rather than `tiny`'s 9px) -- unrelated to this
change, not something `fontsize` was ever wired to affect.

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.6.0` -> `0.7.0`, per the standing practice in `CLAUDE.md`.

## An /etc config-file convention, and a version bump to 0.6.0

Small, direct follow-up to directory support landing one version ago:
now that real directories exist, the `timezone` config file (`tz.c`)
could move out of the filesystem root into a proper `/etc`, the same
place a real Unix keeps config files. Requested by the user.

**Added (`kernel/core/kernel.c`):** `kernel_main()` now calls
`fs_mkdir("/etc")` right after `fs_init()`, before `tz_init()` runs (a
no-op if `/etc` already exists) -- establishing `/etc` as the general
config-file convention for anything that needs one in the future, not
something timezone-specific.

**Changed (`kernel/core/tz.c`):** `TZ_CONFIG_FILE` moved from the bare
`"timezone"` (which, per fs.c's auto-root behavior, meant `/timezone`)
to `/etc/timezone`. `tz_init()` also migrates a pre-`/etc` config file
if one exists: if `/etc/timezone` isn't there yet but the old
`/timezone` is, it reads the saved city from the old location, writes
it to the new one, and deletes the old file -- so upgrading doesn't
silently reset anyone's already-chosen timezone back to UTC.

**Verified in QEMU** (screendumps via `tools/qmp_test.py`): on a fresh
disk, `/etc` exists immediately after boot with nothing else done;
`timezone helsinki` writes `/etc/timezone` (confirmed via `ls
/etc`/`cat /etc/timezone`) and `time` shows the correct local time.
Separately, simulated the upgrade case directly: deleted `/etc/timezone`,
wrote a bare `timezone` file (landing at `/timezone`) containing
`newyork`, and rebooted -- `tz_init()`'s migration path fired
automatically, `/etc/timezone` came back containing `newyork`, the old
`/timezone` was gone from the root listing, and `time` showed the
correct New York local time with no user action needed.

**Changed (`CLAUDE.md`):** documented `/etc` as the config-file
convention, alongside the existing `kapi.h`/`wm/wm.h` boundary notes.

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.5.0` -> `0.6.0`, per the standing practice in `CLAUDE.md`.

## Directory support for the persistent filesystem, and a version bump to 0.5.0

The filesystem (fs.c) had been a flat table of up to 16 files since the
very first persistent-disk milestone -- no notion of a directory at
all. Requested by the user. Design choices were discussed and settled
before implementing: full arbitrary-depth nesting rather than one level
of folders, a stateful shell `cd`/`pwd` with relative-path support
rather than absolute-paths-only, and a disk reformat (no migration --
disk.img only ever held dev/test files) rather than preserving the old
flat layout.

**Changed (`kernel/include/fs.h`, `kernel/drivers/fs.c`):** every entry
(file or directory) is now identified by a full normalized absolute
path (e.g. `/docs/notes.txt`) instead of a flat 32-byte name -- renamed
`FS_NAME_MAX` to `FS_PATH_MAX` and grew it 32 -> 64 bytes. Directories
are just another table entry with a type byte and no data, not a
separate on-disk structure -- parent/child relationships are derived
from the path string itself at lookup time (same "no separate index to
keep in sync" reasoning the original flat design already used). The
implicit root `/` has no entry of its own. `FS_MAX_FILES` doubled 16 ->
32 since directories now consume slots too -- disk.img (1MB) has ample
room either way; growing `FS_PATH_MAX` didn't even grow the per-slot
on-disk size (still 5 sectors, there was headroom). New API:
`fs_mkdir()`, `fs_is_dir()`, `fs_exists()`; `fs_list()` now takes a
directory path and yields only that directory's direct children (name,
size, is-directory) instead of every file on the whole disk.
`fs_delete()` on a directory only succeeds if it's empty -- no recursive
delete, deliberately, to keep `rm` from being able to take out a whole
tree by accident. A bare name with no leading `/` (e.g. `"notepad.txt"`,
`"timezone"`) is still silently treated as rooted at `/`, so every
existing caller -- Notepad, the `timezone` config file, the userland
`filetest` ELF's `SYS_OPEN("filetest.txt")` -- kept working unchanged
with no code of their own to update. The on-disk superblock's version
byte bumped 1 -> 2 and `fs_init()` now checks it exactly, so a disk
written by the previous (flat, no-type-byte) kernel is correctly
detected as foreign and reformatted rather than being misread as the
new layout.

**Added (`apps/shell.c`):** `mkdir <dir>`, `cd [dir]` (bare `cd` goes to
root -- there's no `$HOME`), and `pwd`. A new `resolve_path()` resolves
a relative-or-absolute argument against the shell's `cwd` into a
normalized absolute path, collapsing `.`/`..` components by hand
(split-and-stack, no recursion) -- this is deliberately shell-side
logic, not something fs.c does; fs.c only ever sees paths already
normalized this way. `ls` now takes an optional directory argument
(default: `cwd`) and marks directories with a trailing `/` instead of a
byte count. `cat`/`touch`/`write`/`append`/`rm` all resolve their
argument the same way, so paths work whether relative (`notes.txt`,
`../sibling`) or absolute (`/docs/notes.txt`). The prompt now shows
`cwd` (e.g. `/docs> `) instead of a bare `> `.

**Verified in QEMU** (screendumps via `tools/qmp_test.py`, against a
freshly wiped `disk.img`): `mkdir docs` + `cd docs` + `pwd` + `touch`/
`write`/`ls`/`cat` all worked relative to `/docs`; nesting a nested
`mkdir sub` + `cd sub` one level deeper and `cd ..`/`cd ../..` all
resolved correctly, including from an absolute starting point
(`cd /docs/sub` then `cd ../..` landing back at `/`); `rm` on a
non-empty directory correctly refused with the file/directory
distinction spelled out in the error, and correctly succeeded once
emptied; `cd`/`cat` on nonexistent paths gave sensible errors instead of
silently doing nothing. After `reboot`, the whole `/docs/sub` tree was
still there. Ran `filetest` (the userland ELF using bare
`"filetest.txt"` via real syscalls) and `timezone helsinki` (the
existing config-file mechanism, bare `"timezone"`) from inside `/docs`
-- both round-tripped correctly and both landed at the filesystem root
as expected, confirmed via `ls /`. The GUI (taskbar clock, Start menu)
still rendered correctly after the fs.c rewrite.

**Changed (`kernel/core/syscall.c`, `kernel/include/syscall_abi.h`):**
mechanical rename of `FS_NAME_MAX` references to `FS_PATH_MAX` --
SYS_OPEN's path handling itself didn't need any logic changes, since it
already just copied a raw NUL-terminated buffer with no path parsing of
its own (path resolution/normalization is entirely fs.c's job).

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.4.0` -> `0.5.0`, per the standing practice in `CLAUDE.md`.

## Timezone selection (toy-os's first config file), and a version bump to 0.4.0

The RTC-backed clock (`time` in the shell, the taskbar clock) was
displaying raw CMOS/hardware time with no adjustment -- correct when
QEMU is run on a host whose hardware clock is set to local time, but
wrong (off by the UTC offset) on a Linux host like the user's, where
the hardware clock is UTC and only userspace applies the local
timezone. Requested by the user (in Finland, so 3 hours behind what
was showing in summer). Options for how to store/select it were
discussed and chosen before implementing: a preset list of cities
rather than raw UTC offsets, automatic DST rather than a fixed offset
the user re-picks twice a year, and supporting both an argument form
and an interactive picker for the command that sets it.

**Added (`kernel/include/tz.h`, `kernel/core/tz.c`, new files):** a
small built-in list of cities (`utc`, `helsinki`, `london`, `berlin`,
`newyork`, `losangeles`, `tokyo`), each a standard-time UTC offset plus
which DST rule (if any) applies -- `TZ_DST_EU` (last Sunday of March
01:00 UTC to last Sunday of October 01:00 UTC, also what the UK uses)
or `TZ_DST_US` (second Sunday of March to first Sunday of November).
DST is checked against the transition *date* only, not the exact
clock-change hour, so the displayed time can be off by up to a few
hours right at the spring/fall boundary itself -- a deliberate
simplification, not worth more precision for a shell clock. The
selected city persists across reboots as a plain file named
`timezone`, written/read through the existing `fs_*` API (`fs.h`) --
this is toy-os's first config file, and deliberately uses the
filesystem that already exists rather than inventing a new storage
mechanism. `rtc_read_local()` wraps `rtc_read()` (unchanged, still raw
UTC) with the selected city's offset and DST adjustment, including
proper day/month/year rollover (`rtc_add_minutes()`, with a leap-year-
aware days-in-month table) for offsets that cross a day boundary.

**Changed (`apps/shell.c`):** `time` now calls `rtc_read_local()`
instead of `rtc_read()` and shows the active city in parentheses.
Added a `timezone` command: `timezone <city>` sets it directly (exact
lowercase match, e.g. `timezone helsinki`, same convention as `color
<name>`); `timezone` alone prints a numbered list (current choice
marked with `*`) and prompts for a number, reusing `keyboard_read_line`
rather than adding a new input-reading mechanism.

**Changed (`apps/wm/wm_render.c`):** the taskbar clock (`draw_clock_area`)
also switched from `rtc_read()` to `rtc_read_local()`, so it matches
whatever the shell's `time` says.

**Verified in QEMU** (screendumps via `tools/qmp_test.py`, against a
freshly wiped `disk.img`): defaults to `utc` with no config file
present, matching the host's UTC time exactly; `timezone helsinki`
immediately shows the correct UTC+3 (EEST, summer DST correctly
detected); `timezone` with no args lists all seven cities with
`helsinki` marked current; picking `newyork` from that list shows the
correct UTC-4 (EDT, the other DST rule); `ls`/`cat timezone` show it as
an ordinary 8-byte file containing `helsinki`; after `reboot`, `time`
immediately shows Helsinki local time again with no re-selection
needed, confirming the persistence round-trip; the GUI taskbar clock
matches the shell's `time` after switching into `gui` mode.

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.3.0` -> `0.4.0`, per the standing practice in `CLAUDE.md`.

## A blinking cursor for the framebuffer console, and a version bump to 0.3.0

The legacy 80x25 VGA text-mode backend gets a blinking cursor for free
from real hardware (`legacy_update_cursor()` just tells the CRT
controller where to blink, not whether) -- but the framebuffer backend
that's actually active in this environment (GRUB hands the kernel a
linear RGB framebuffer, and `vga.c` renders text onto it pixel-by-pixel
via `gfx_draw_char()`) had no such hardware to lean on, and drew no
cursor of any kind. Requested by the user.

**Added (`kernel/drivers/vga.c`):** a solid block the size of one glyph
cell, painted at the current `(row, col)` and toggled on a timer --
`cursor_paint()`/`cursor_hide()`/`cursor_show_and_reset_blink()`
(internal) and the new public `vga_cursor_tick()`. `fb_putc()` and
`fb_backspace()` now hide the cursor before moving `(row, col)` and
show it again after, since `gfx_draw_char()` only implicitly erases a
cursor left in the *same* cell it's about to overwrite -- the
newline/carriage-return/backspace paths change position without
drawing over the old cell. `fb_clear()` resets the on-screen flag along
with everything else it clears. No-op in legacy text mode throughout
(`vga_cursor_tick()` returns immediately if `fb_mode` is false), where
the hardware cursor already handles this.

**Added (`kernel/drivers/keyboard.c`):** `keyboard_getchar()`'s
`hlt`-based wait loop now calls `vga_cursor_tick()` on every wake-up.
`hlt` halts until the *next interrupt*, not specifically a keypress --
in practice that's usually the 100Hz PIT tick when otherwise idle at a
prompt -- so this was a convenient existing hook to drive a blink
without adding any scheduler/multitasking machinery. `vga_cursor_tick()`
gates its own real work internally (toggling only once every 50 ticks,
~500ms, for a ~1s full blink cycle), so the extra call costs nothing on
the many wake-ups where it doesn't toggle.

**Verified in QEMU** (screendumps via `tools/qmp_test.py`): a first
round of four screenshots spaced ~0.95s apart at an idle prompt all
looked identical (cursor never visible), which looked like a bug --
turned out to be aliasing, since ~0.95s is close to the ~1s blink
period and kept landing on the same phase. Screenshots at a tighter
0.4s interval showed the block clearly alternating on/off, and a
before/after pair 0.55s apart at a clean idle `>` prompt confirmed the
same thing without that artifact. Also confirmed visually while typing
(`help` typed but not submitted): the cursor tracks the current column,
sitting right after the last typed character.

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.2.0` -> `0.3.0`, per the standing practice in `CLAUDE.md`.

## `.editorconfig`, and a `screenshots/` folder organized by version

Two small additions from the same pass, picked because they were
low-risk enough not to need QEMU testing: `.editorconfig` was the
one thing on a "what can be added almost for free" list that fit;
the screenshots folder was a direct, separate request.

**Added (`.editorconfig`):** indent style/size and charset/newline
rules matching what the codebase already does by convention (4-space
indent everywhere, including NASM `.asm` sources -- verified against
`kernel/core/boot.asm` with `cat -A`/`grep -P "^\t"` before writing the
rule, since an initial guess that `.asm` used tabs like the `Makefile`
turned out to be wrong).

**Added (`screenshots/`, new folder):** `screenshots/v0.2.0/` with the
session's existing QEMU testing screendumps renamed to describe what
they show (`calculator_7_plus_3.png`-style, not `shot13.png`), plus
`screenshots/README.md` documenting the convention going forward: one
subfolder per `TOYOS_VERSION`, created the first time a testing pass
happens under that version rather than pre-created empty. Referenced
from `CLAUDE.md` so future sessions know it exists.

## A simple shell pager, and a version bump to 0.2.0

`help` prints roughly 44 lines, but the console only shows 32 rows at
the default (small) font size -- 22 at medium, 18 at large -- so by the
time `cmd_help()` finished, the console's existing scroll-on-overflow
behavior had already pushed the first third or so of the list off the
top with no way to get back to it. Reported by the user after actually
hitting it.

**Added (`kernel/include/vga.h`, `kernel/drivers/vga.c`):** `vga_rows()`
-- returns the current console height in text rows (25 in legacy 80x25
text mode, or `gfx_height()/gfx_char_h()` in framebuffer mode, which
varies with the active font size). Needed so a caller can know how many
lines fit on screen before deciding when to pause.

**Added (`apps/shell.c`):** `console_page(lines, count)` -- prints an
array of strings one at a time, pausing with a `-- more (press any key,
'q' to quit) --` prompt whenever a screenful has gone by. Any key
continues to the next page; `q`/`Q` stops early and returns straight to
the prompt. General enough for any future command whose output might
outgrow one screen, not just `help` -- `cmd_help()` itself is now just
`console_page(HELP_LINES, HELP_LINE_COUNT)`, with the previous 44
`vga_write()` calls turned into a `HELP_LINES[]` array of the same
strings so the pager has something to page through.

Deliberately scoped to the shell rather than built into `vga_write()`
itself -- a global pager would also pause early boot messages (printed
before the keyboard driver is ready to supply the blocking
`keyboard_getchar()` call this relies on) and interfere with anything
capturing serial output non-interactively. Keeping it shell-side avoids
both, at the cost of needing to opt commands in via `console_page()`
rather than getting paging for free everywhere.

**Verified in QEMU** (screendumps via the new `tools/qmp_test.py`):
`help`'s first screenful stops at `-- more --` with nothing scrolled
off; pressing a key continues to the remaining lines and returns to the
prompt; pressing `q` on the first page quits immediately back to the
prompt instead of showing the rest; `about` and `ls` both run correctly
immediately after paged output with no leftover state.

**Changed (`kernel/include/version.h`):** `TOYOS_VERSION` bumped
`0.1.0` -> `0.2.0`. Per the user's request, this now gets bumped for
any real change going forward (see `CLAUDE.md`), not just headline
milestones -- it had drifted for a while, with the persistent
filesystem, Calculator, the wm.c split, and widgets.h/theme.h all
landing under `0.1.0` before this was corrected.

## CLAUDE.md, tools/qmp_test.py, .gitignore, and `make help`

Four small, low-risk additions aimed purely at future development speed
rather than the OS itself -- none of them touch code that runs on
toy-os, so none needed QEMU testing beyond confirming they work as dev
tools.

**Added (`CLAUDE.md`):** guidance for future Claude sessions working in
this repo -- not a restatement of the architecture (`README.md`/
`apps/README.md` already cover that), but the environment quirks and
workflow gotchas that don't live in any file yet: the `kapi.h`/
`wm/wm.h` boundary conventions, the `Makefile`-is-a-protected-file
constraint when working through the Cowork device bridge, the
can't-delete-files-remotely workaround (`_to_delete/` + tell the user),
and a full section on QEMU/QMP headless testing gotchas (see below).

**Added (`tools/qmp_test.py`):** a committed, reusable `QMPSession`
helper class (`goto()`/`click()`/`send_key()`/`send_text()`/
`screenshot()`) for driving toy-os in headless QEMU over QMP. Past
testing sessions each independently hand-rolled similar one-off scripts
in the cloud sandbox -- never committed, so lost between sessions, and
each one re-paying the cost of discovering the same gotchas fresh (most
notably: `-display none` silently breaks `input-send-event` mouse
routing even though it still reports success, and this kernel's mouse
driver is PS/2/relative, not USB HID/absolute, so `-device usb-tablet`
is the wrong device class entirely). Verified working end-to-end this
session: launched QEMU, imported the module, opened the Start menu via
`goto()`/`click()`, and confirmed `screenshot()`'s ppm-to-png conversion
against a live instance before committing it.

**Added (`.gitignore`):** ignores build outputs (`build/`, `iso/`,
`toy-os.iso`, `userland/*.elf`), the local persistent disk image
(`disk.img` -- dev state, not a repo asset, see its existing comment in
the Makefile), and QEMU/QMP test scratch (`*.ppm`, log files). No git
repository exists in this project yet, but one clearly will eventually
given the project's size and the CHANGELOG discipline already in place
-- this is in place so the first `git add .` doesn't accidentally
commit 30+ MB of regenerable binaries.

**Added (`Makefile`'s `help` target):** `make help` lists the six
existing targets (`all`/`iso`/`run`/`run-nographic`/`clean`/
`clean-disk`) with one-line descriptions each. Pure `@echo` output, no
interaction with build logic -- `all` remains the default target
(verified with a clean `make` and no arguments still building
`kernel.bin` as before).

## A shared apps/theme.h for the color values that had already converged by hand

Small follow-up to the widgets.h extraction below. Auditing `gfx_rgb(...)`
call sites across the GUI apps turned up several exact-duplicate RGB
triples nobody had coordinated on purpose: `gfx_rgb(225, 225, 230)` (the
light-gray button background) appeared identically in both
`calculator.c` and `notepad.c`; `gfx_rgb(20, 20, 20)` (near-black body
text) appeared identically in `wm_render.c`, `calculator.c`,
`notepad.c`, and `about.c`; `gfx_rgb(255, 255, 255)` (pure white),
`gfx_rgb(60, 60, 60)` (borders), `gfx_rgb(235, 235, 235)` (window
background), and `gfx_rgb(245, 245, 245)` (panel background) each
repeated across two or more files as well.

**Added (`apps/theme.h`):** six function-like macros --
`THEME_WHITE`/`THEME_TEXT`/`THEME_BORDER`/`THEME_BUTTON_BG`/
`THEME_WINDOW_BG`/`THEME_PANEL_BG` -- each expanding to the exact same
`gfx_rgb(r, g, b)` call the value already had. Deliberately not
exhaustive: only values that were already identical in two or more
places got a name; single-use colors (the close button's red, the
taskbar's dark background, the focused-titlebar blue) were left as
plain `gfx_rgb()` calls rather than given a name that wouldn't remove
any actual duplication.

**Changed (`apps/wm/wm_render.c`, `apps/calculator.c`, `apps/notepad.c`,
`apps/about.c`):** every call site with one of the six duplicated values
swapped to the matching `THEME_*` macro -- a byte-for-byte identical
substitution (each macro expands to the exact RGB triple it replaced),
so this is a naming change only, not a visual or behavioral one.
Verified by diffing the `gfx_rgb()`/`THEME_*` call sites against the
pre-change values and confirming a clean rebuild with no new warnings;
no QEMU re-test was needed since nothing about what gets drawn changed.

## Splitting wm.c into apps/wm/, and a shared widgets.h/widgets.c module

`apps/wm.c` had grown into the largest hand-written file in the repo
(553 lines) with a lot of internal grouping already visible via its own
section comments -- clicking, dragging/resizing, and every drawing
function all living in one file made it harder to find the right spot to
change. Separately, three independent, near-identical hand-rolled
implementations of "clickable rectangle with a centered label" had
accumulated: the window manager's title-bar/taskbar/Start buttons,
Calculator's button grid, and Notepad's Save/Load toolbar. This pass
addressed both, in one go per the plan discussed with the user (new
`apps/wm/` subfolder, and widgets extracted in the same pass rather than
as separate follow-up work).

**Added (`apps/wm/`, new folder):** `wm.c`/`wm.c`'s previous single-file
contents split three ways by concern, mirroring `kernel/`'s
core-vs-drivers convention of grouping a cluster of related files under
one subfolder:
- `wm.h` -- the public API (`struct window`, `window_set_state`/
  `window_get_state`, `window_content_x/y/w/h`, `window_invalidate`,
  `wm_run()`), unchanged from before other than its top comment. Other
  apps now include it as `#include "wm/wm.h"`.
- `wm.c` -- shared state (now non-`static`, since the other two files
  need it), the app-facing helpers, window lifecycle (`open_app`,
  `close_window`, `bring_to_front`), and `wm_run()`'s main loop.
- `wm_input.c` -- mouse click handling (`wm_handle_left_click()`,
  renamed from `handle_left_click` since it's now a genuine cross-file
  entry point) and the per-tick drag/resize update
  (`wm_update_drag_resize()`, extracted from what used to be inline in
  `wm_run()`'s loop).
- `wm_render.c` -- all drawing (window chrome, taskbar, Start menu,
  cursor) plus the shared layout-metric helpers (`title_buttons()`,
  `btn_size()`, `start_btn_w()`, etc) that `wm_input.c` also needs for
  hit-testing the exact same regions this file draws -- "layout truth
  lives with rendering." Its entry point is `wm_render_frame()`, renamed
  from `render_frame`.
- `wm_internal.h` -- the private glue between the three `.c` files:
  `extern` declarations for the shared state, and prototypes for the
  cross-file helpers. Deliberately not part of `wm.h`'s public API, and
  never included outside `apps/wm/`.

This is reorganization for readability, not decoupling -- the three
files still share mutable state directly through `wm_internal.h`'s
`extern`s, same single-threaded event loop as before. Only the three
genuine cross-file entry points got a `wm_` prefix
(`wm_handle_left_click`, `wm_update_drag_resize`, `wm_render_frame`);
every other shared-but-private helper (`bring_to_front`, `open_app`,
`close_window`, `title_buttons`, `btn_size`, `start_btn_w`, `win_btn_w`,
`start_menu_w`) kept its original bare name.

**Added (`apps/widgets.h`, `apps/widgets.c`):** two functions factoring
out the repeated button pattern -- `widget_hit(x, y, w, h, px, py)` (a
plain bounds check) and `widget_button(x, y, w, h, label, bg, fg)`
(fills the rect with `bg`, centers `label` in `fg` if non-NULL; pass
`label = NULL` for icon-only buttons like the window manager's
minimize/maximize/close, then draw the icon on top separately).
Deliberately minimal and explicitly documented as not the start of a
general widget toolkit -- no focus management, layout engine, text
fields, or scrollbars; the next primitive belongs here only once a
second real caller needs it. It's a peer-level apps-internal header like
`wm/wm.h`, included directly by whichever `.c` file needs it -- not
folded into `kapi.h`.

**Changed (`apps/wm/wm_render.c`, `apps/calculator.c`, `apps/notepad.c`):**
all three retrofitted to draw their buttons via `widget_button()` and
hit-test via `widget_hit()` instead of their previous hand-rolled
`gfx_fill_rect()` + manual label-centering math, and manual
`cx >= bx && cx < bx + bw && ...` bounds checks. One small, intentional
visual change fell out of this: the taskbar's Start button and
per-window buttons now center their labels (matching Calculator's and
Notepad's buttons) instead of the old fixed left-padding.

**Changed (`Makefile`):** `apps/*.c`'s wildcard is non-recursive, so
`apps/wm/*.c` needed its own (`WM_C`), folded into `C_SOURCES`/
`C_OBJECTS`, plus a new pattern rule (`$(BUILD)/apps/wm/%.o: apps/wm/%.c`)
and mkdir target for `$(BUILD)/apps/wm`.

**Changed (`apps/about.c`, `apps/calculator.c`, `apps/notepad.c`,
`apps/gui.c`):** `#include "wm.h"` -> `#include "wm/wm.h"` to match the
new location; `calculator.c` and `notepad.c` additionally gained
`#include "widgets.h"`.

**Verified in QEMU (screendumps + scripted QMP mouse/keyboard input):**
clean rebuild with no new warnings; the taskbar/Start button/clock/
cursor render correctly; the Start menu lists all three GUI apps and
opens each one; Calculator's button grid computes correctly through
`widget_hit()`-based clicks (checked `7 - 3 = 4` and `7 + 3 = 10`);
Notepad's Save/Load round-trips through `fs_write`/`fs_read` via its
`widget_button()`-drawn toolbar; window dragging (by the title bar),
minimize, maximize/restore, close, and focus/z-order switching between
overlapping windows (About docked over Notepad) all still work exactly
as before the split.

## A real persistent filesystem: legacy PIO ATA driver + an on-disk layout for fs.c

Files written with `write`/`touch`/`append` (or the `SYS_OPEN`/`SYS_WRITE`
ring-3 syscalls) used to vanish the instant the kernel reset -- `fs.c`
was a plain in-memory `struct file files[16]` array, zeroed by
`fs_init()` on every boot. This makes it genuinely persistent: it
survives not just the in-VM `reboot` command but a full QEMU process
kill and relaunch (tested -- see "Verified" below), the same as
power-cycling real hardware would.

**Added (`kernel/drivers/ata.c`, `kernel/include/ata.h`):** a minimal
legacy PIO ATA/IDE driver -- primary bus, master drive only, 28-bit LBA,
polling (`ata_init`/`ata_present`/`ata_read_sector`/`ata_write_sector`).
Chosen deliberately over AHCI: legacy ATA lives at fixed ports
(0x1F0-0x1F7), so it needs no PCI enumeration at all, and it's exactly
what QEMU's `-drive ...,if=ide` presents -- AHCI would additionally need
PCI config space access, an MMIO BAR, and command-list/FIS structures,
a much bigger jump. IRQ14 (primary ATA's interrupt) is never unmasked
(see `idt.c`'s `pic_clear_mask()` calls, which only enable IRQ 0/1/2/12),
so this is safe to write as pure polling with no ISR at all -- same
shape as `i8042.c` and the PIT.

`ata_init()`'s detection follows the standard bail-out-early-at-every-step
algorithm: a floating bus (`status == 0xFF`) means no controller at all;
`status == 0` right after selecting a drive and issuing IDENTIFY means a
controller but no drive; a nonzero LBA-mid/LBA-high after that means
something non-ATA is there (an ATAPI drive, say) instead of a plain
disk. Every wait loop is bounded (100000 iterations), not infinite --
booting with no `-drive` at all falls straight through to
`ata_present() == 0` rather than hanging.

**Added (`kernel/include/io.h`):** `inw`/`outw` -- ATA's data register
transfers a whole sector as 256 16-bit words, and the existing
`inb`/`outb` (used by the PIC/PIT/8042 so far) only move a byte at a
time.

**Changed (`kernel/drivers/fs.c`):** now two layers instead of one, on
purpose. The in-memory `files[]` table is still exactly what every
existing caller (Notepad, the shell's `ls`/`cat`/`write`/`append`/`rm`,
the `SYS_OPEN`/`SYS_READ`/`SYS_WRITE`/`SYS_CLOSE` syscalls) reads and
writes through `fs_read()`/`fs_list()` -- none of that code changed at
all, same as the payoff `fs.c`'s API stability was supposed to buy back
when the file-I/O syscalls were added. What's new: `fs_init()` now calls
`ata_init()` and either loads an existing on-disk filesystem (superblock
magic `"TFS1"` recognized) or formats a fresh one (blank/foreign disk),
and every mutating call -- `fs_touch()`/`fs_write()`/`fs_delete()` --
writes its one changed record straight through to disk immediately
after updating `files[]`. `fs_is_persistent()` is a new query apps can
use to tell which mode they're in (see `about`'s new "Storage:" line
below); `fs.c`'s file-level top comment has the full on-disk layout
(fixed 16-slot table, mirroring `files[]`'s own shape) and the honest
limitations (write-through with no journaling -- a crash mid-write to
one record could still leave it inconsistent; the layout is tied to the
current `FS_MAX_FILES`/`FS_NAME_MAX`/`FS_DATA_MAX` values with no
migration path if they change).

**Added (`Makefile`):** a separate `disk.img` (1MiB, raw), created once
by `make run`/`run-nographic` and deliberately left alone by `make
clean` (a new `make clean-disk` target wipes it explicitly) -- the
whole point of it existing is surviving rebuilds. It's genuinely
separate from `toy-os.iso`: GRUB/QEMU only ever mount that as a
read-only CD-ROM (El Torito), which was never going to be writable no
matter what `fs.c` did on top of it. `-drive file=disk.img,format=raw,if=ide`
attaches it to the primary IDE bus (QEMU's default `-cdrom` placement
puts the boot CD on the *secondary* bus, so `ata.c`'s primary-bus
IDENTIFY never sees it and can't mistake it for a plain disk).

**Changed (`apps/shell.c`'s `about`, `apps/about.c`'s GUI About window):**
both now show a "Storage:" line -- "disk-backed (files persist across
reboots)" or "RAM only (no disk found...)" -- via the new
`fs_is_persistent()`. About's GUI window grew from 5 lines to 6 to fit
it (`apps/gui_apps.c`'s registered height bumped 300 -> 340).

**Verified:** rebuilt clean (`nm -u build/kernel.bin` empty, same check
as the file-I/O entry below -- no undefined symbols, confirming the
16-bit port I/O and everything else here compiled to plain inline
instructions, no library calls). Booted in QEMU with `disk.img`
attached: first boot logged `fs: formatted a fresh persistent
filesystem on disk`; wrote a file, confirmed `about` reports
"disk-backed"; **killed the QEMU process entirely and relaunched it
against the same disk.img** (not just the in-VM `reboot` command) --
second boot logged `fs: loaded persistent filesystem from disk` and
`cat`/`ls` confirmed the file's exact contents survived. Deleted it,
relaunched again, confirmed the deletion also persisted. Ran `filetest`
(the ring-3 `SYS_OPEN`/`SYS_WRITE`/`SYS_READ`/`SYS_CLOSE` syscall demo)
with the disk attached and, after another full relaunch, confirmed its
file was on disk too -- proving the syscall path and the shell path
share the same persistent store correctly. Separately booted with no
`-drive` at all: logged `fs: no disk found -- files are RAM-only, won't
survive reboot`, `about` correctly reported RAM-only, and the existing
regression suite (`writetest`, `ptrtest`, `schedtest`) still passed
back to back, confirming the no-disk fallback doesn't hang or break
anything that worked before.

## Small addition -- a version number, and a Calculator app (GUI + engine split)

Two independent small additions bundled together since they touched
adjacent files:

**Version number.** `kernel/include/version.h` defines a single
`TOYOS_VERSION` string ("0.1.0" -- semantic-versioning-shaped, bumped by
hand; CHANGELOG.md remains the actual record of what changed at each
point). Included via `kapi.h` so both the shell's `about` command
(`apps/shell.c`) and the GUI About window (`apps/about.c`) show it --
the two string literals ("toy-os v" TOYOS_VERSION ...) rely on C's
adjacent-string-literal concatenation, so there's no runtime string
building involved at all.

**Calculator.** A basic 4-function GUI app (+, -, *, /, %, sign toggle,
decimal point, clear, clear-entry, backspace), usable from both the
on-screen button grid and the keyboard. Added as `apps/calculator.c`
(the gui_app adapter -- button layout, click hit-testing, keyboard
mapping) split from `apps/calc_engine.c` (the actual arithmetic, with
zero dependency on wm.h/gfx.h), registered in `apps/gui_apps.c` the same
one-line way as Notepad/About.

The split matters for a reason specific to this kernel: there's no
floating point anywhere in toy-os (`-mno-sse -mno-sse2`, no FPU state
save/restore on any context switch -- see the file I/O entry above and
README's "Ideas for what's next"), so a calculator here can't just use
`double` the way one normally would. `calc_engine.c` instead represents
every value as a plain `int64_t` scaled by 10^4 (`CALC_SCALE`), with
every arithmetic op landing on native 64-bit CPU instructions --
confirmed by checking `nm -u build/kernel.bin` after building, which
comes back empty (no undefined `__muldi3`/`__divti3`/etc symbols a
soft-float or 128-bit-int fallback would need, which this freestanding
`-nostdlib` kernel doesn't link against anyway). Multiply and the
divide/percent operators' intermediate steps are checked with GCC's
`__builtin_mul_overflow`/`__builtin_add_overflow`/`__builtin_sub_overflow`
(compile-time intrinsics, not library calls) and fall back to the
calculator's "Error" state instead of silently wrapping on overflow.

`apps/calc_engine.h` documents the exact two ways to extend it: a new
binary operator is one case in `apply_op()` (which already receives both
scaled operands and returns success/failure), while a new unary
operator (a future square root key, say) doesn't fit that shape and
gets its own small function called directly from `calc_input()`. Adding
a new button in `apps/calculator.c` is one entry in its `BUTTONS[]`
table -- the same table drives both drawing and click hit-testing, so
there's no separate place that can drift out of sync with it.

**Deliberately left out, and easy to add later if wanted:** memory
functions (M+/M-/MR/MC -- would need one more `int64_t` register plus a
handful of new `calc_input()` codes, following the exact same pattern
as the existing ones), a square root key (the unary-operator path
`calc_engine.h` already documents, likely via integer Newton's method
to stay off floating point same as everything else here), and an
on-screen backspace button (backspace only works from the keyboard right
now -- `'B'` is a valid `calc_input()` code already, just not on
`BUTTONS[]` yet).

**Verified:** rebuilt clean (no warnings from any of the new files,
confirmed `nm -u` shows no undefined symbols as above), booted in QEMU,
confirmed `about` shows the version in the shell, opened the GUI,
confirmed the About window shows it there too, then opened Calculator
from the Start menu and, via simulated mouse clicks and keyboard input
through QEMU's QMP socket, checked: `123 + 45 = 168` (multi-digit chained
entry), `3.5 * 2 = 7` (decimal multiply), `10 / 4 = 2.5` (division),
`5 / 0` -> `Error` (and that `C` recovers from it), `7`, sign-toggle ->
`-7`, `99` then CE -> `0`, keyboard `12` + backspace -> `1`, keyboard
`17 % 5 = 2`, and keyboard `6 * 7` + Enter -> `42` (Enter as a `=`
shortcut).

## Real file I/O: SYS_OPEN / SYS_READ / SYS_CLOSE, and SYS_WRITE goes fd-aware

A step toward supporting a real C library (see the earlier "what would
libc need" investigation this covers part of): the first syscalls that
let a ring-3 process touch a real, named file in the in-memory
filesystem (`fs.c`), rather than only ever writing to the console.
`SYS_WRITE`'s ABI is a deliberate breaking change here -- it used to
always mean "write to the console" (`RDI` = buffer, `RSI` = length);
now `RDI` is an fd (1/2 still mean console -- same behavior as before
-- and >= 3 means a file opened via `SYS_OPEN`), `RSI` = buffer, `RDX`
= length. Chosen over adding a parallel syscall since the ABI is still
young and internal to this project -- every existing caller
(`write_test.c`, `write_bad_test.c`, `counter_a.c`, `counter_b.c`,
`echo.c`) was updated to pass 1 (stdout) as the new first argument.

**Added (`kernel/include/syscall_abi.h`, `kernel/core/syscall.c`):**
- `SYS_READ` (9) -- `RDI` = fd, `RSI` = buffer, `RDX` = length. Reads
  from the fd's current position (tracked per-fd, advanced by each
  call), returns bytes read, 0 at EOF, or -1 on a bad fd/pointer.
- `SYS_OPEN` (10) -- `RDI` = pointer to a NUL-terminated path, `RSI` =
  `SYS_O_WRITE`/`SYS_O_CREAT`/`SYS_O_TRUNC` flags (bitwise OR, default
  read-only). Returns a small fd (>= 3) or -1 (bad pointer, not found
  without `SYS_O_CREAT`, or the 8-slot open-file table is full).
- `SYS_CLOSE` (11) -- `RDI` = fd. Frees the slot, returns 0 or -1.
- An open-file table (`fd_table[8]` in `syscall.c`) matching the
  existing per-process-scoped pattern already used for the heap
  (`SYS_SBRK`) and window state (`SYS_WIN_CREATE`): each slot records
  the owning process's CR3 so fds can't be shared or hijacked across
  processes.
- `kernel/core/file_test.c` / `userland/file_test.c` -- the demo
  (`filetest` shell command, module index 9 in `grub.cfg`): opens a
  file for writing (creating + truncating it), writes a fixed string,
  closes it, reopens it read-only, reads it back, echoes what it read
  to stdout (fd 1), and exits 0 only if the read-back bytes matched
  byte-for-byte what was written -- proving the round trip through
  `fs.c` actually works, not just that each syscall returns success in
  isolation.

**Bug found and fixed during testing:** `userland/file_test.c`'s first
draft hardcoded the byte length of each string literal it passed to
`sys_write()` (e.g. `sys_write(1, "filetest: read back: ", 22)`) --
every one of those hardcoded lengths was off by one (counted the
string including its NUL terminator, which C string literals don't
include in what a human recounts by eye). The result wasn't a crash --
`SYS_WRITE`'s console path (`vga_putc()`) happily "printed" the extra
NUL byte as a `?` glyph, so the bug only showed up as a stray `?`
character in the QEMU output, easy to miss without actually looking at
a screenshot rather than just checking the exit code. Fixed by using
the same `my_strlen()` helper `write_test.c` already uses for its one
string, for every literal in `file_test.c`, instead of hand-counting.

**Honest limitations, not fixed here (see README's "Ideas for what's
next"):** `fs_write()` (`fs.c`) only understands NUL-terminated C
strings via `k_strlen()`, not explicit-length byte buffers -- there's
no offset-based partial write in the underlying filesystem, so a
`SYS_WRITE` to a file fd always appends (never overwrites at an
arbitrary offset), and a buffer containing an embedded NUL byte
truncates early, same as it would passed to any C string function.
`SYS_READ` doesn't have this problem -- `fs_read()` already returns an
explicit-length pointer, so reads are exact including any byte value.

**Verified:** rebuilt clean, booted in QEMU, ran `filetest` (screen
output showed the exact written string read back, `round trip OK`,
exit code 0) and re-ran the full existing regression suite --
`writetest`, `ptrtest`, `schedtest`, `echotest` -- back to back in the
same boot to confirm the breaking `SYS_WRITE` ABI change didn't
silently break any of them.

## Small addition -- a real per-window protocol: SYS_WIN_CREATE / SYS_WIN_PRESENT

The previous entry's `SYS_GUI_INIT` (see `gui_test.c`) hands a ring-3
process the ENTIRE real framebuffer, mapped straight into its own
address space -- the process draws directly onto the real screen, with
no concept of a "window" at all. This adds a genuinely different
protocol: a ring-3 process now only ever sees its own private w*h pixel
buffer, and asks the kernel to composite that buffer -- plus a real
title bar and close button, drawn by the kernel, not the process -- onto
the real screen. That's a real client/server split, the shape a real
windowing protocol actually has, instead of "hand over the whole
screen."

**Added (`kernel/include/syscall_abi.h`, `kernel/core/syscall.c`):**
- `SYS_WIN_CREATE` (7) -- `RDI` = pointer to a `struct win_request`
  (in: desired `w`/`h`/`x`/`y`; out: `pitch`/`bpp`). Allocates and maps
  a private, zeroed pixel buffer at the fixed `WIN_BUF_VADDR`, capped at
  640x480 (`WIN_MAX_W`/`WIN_MAX_H`). Self-arming -- unlike `SYS_SBRK`,
  no separate kernel-side "reset" call is needed before spawning the
  process, since the buffer's virtual address is the same for every
  process and `SYS_WIN_CREATE` itself records which process (by CR3)
  is allowed to call `SYS_WIN_PRESENT` next.
- `SYS_WIN_PRESENT` (8) -- no arguments. Composites the created buffer
  onto the real screen at its recorded position, with a kernel-drawn
  title bar (fixed "App" label) and a close button using the same
  hand-drawn diagonal-cross icon as `apps/wm.c`'s close button (see the
  earlier close-button CHANGELOG entry for why a font glyph doesn't
  work in a small button -- same reasoning, duplicated rather than
  shared since kernel/core has no existing reason to link against the
  GUI app layer).
- `kernel/core/win_test.c` / `userland/win_test.c` -- the demo, same
  shape as `gui_test.c`/`userland/gui_test.c`: fills its buffer with a
  color, presents it, cycles color on every keypress (via the existing
  `SYS_READ_KEY`), 'q'/Esc exits. New shell command: `wintest`.

**Implementation note -- why the buffer is tracked per-page, not as one
contiguous physical range:** `pmm_alloc_frame()` doesn't promise
contiguity between calls (see `pmm.h`), even though it happens to
return contiguous frames in the common case (a bitmap scan with nothing
else competing). Rather than rely on that, `SYS_WIN_CREATE` records
each backing page's physical address individually
(`g_win_frames[WIN_MAX_PAGES]`), and `SYS_WIN_PRESENT` translates each
pixel's offset to its actual frame before reading it. Since the pitch
is always `w * 4` (a multiple of 4) and pages are 4096 bytes (also a
multiple of 4), no 4-byte pixel ever straddles a page boundary, so this
per-pixel translation is exact, not an approximation.

**Honest scope, same limitation as `SYS_GUI_INIT`:** this is still
modal. There's no concurrency between a `SYS_WIN_*` process and the
kernel-space window manager (`wm.c`) -- it isn't a window inside
`wm.c`'s own window list, and only one can be on screen at a time.
Making that concurrent needs the scheduler to give the kernel-space WM
loop and a scheduled ring-3 process fair turns, which `scheduler.c`'s
current design doesn't do: `scheduler_tick()` only resumes kernel-space
code when `current_index == -1` (nothing currently running), and once
any process is `READY`, `find_next_ready()` finds that same process
again on every subsequent tick (there's nothing else to find), so
kernel-space code doesn't get scheduled again until every process
exits. Read `scheduler.c`'s tick logic closely before assuming
otherwise -- this was checked, not guessed, while scoping this feature,
which is why it stayed modal rather than attempting real concurrency
this round. Also not yet done: mouse input isn't piped to ring 3 at
all, so the close button is drawn but not clickable -- only
keyboard-driven exit (`q`/Esc) works.

**Verified:** clean rebuild + QEMU. Ran `wintest`, confirmed the window
renders with real chrome (screenshotted), pressed a key and confirmed
the content re-composites with a new color (screenshotted), and
confirmed `q` exits cleanly back to a working shell. Reran
`syscalltest` and `echotest` afterward in the same boot to confirm nothing else broke, and `ptrtest`/`schedtest` in a fresh boot -- all still pass.

## Small addition -- general-purpose syscalls: SYS_READ_KEY, SYS_SBRK, and an interactive demo

Every syscall so far either printed on a process's behalf (`SYS_WRITE`)
or was scoped to the experimental modal GUI path (`SYS_GUI_INIT`/
`SYS_GUI_POLL_KEY`, see `gui_test.c`). This adds two general-purpose
ones any ring-3 process can use, plus `echotest`, a new shell command
that demonstrates both together: type something and watch a real
ring-3 process (not the shell, not the kernel) echo it back live,
character by character, until Esc.

**Added (`kernel/include/syscall_abi.h`, `kernel/core/syscall.c`):**
- `SYS_READ_KEY` (5) -- returns a queued key, or -1 if none is waiting.
  Non-blocking, same contract as the existing `SYS_GUI_POLL_KEY`.
- `SYS_SBRK` (6) -- a plain heap-bump allocator. `RDI` = increment in
  bytes (0 or positive only, no shrinking); returns the previous break,
  i.e. a pointer to that many freshly mapped, zeroed bytes, or -1 if no
  heap was armed for the calling process or physical memory ran out.
  Backed by `pmm_alloc_frame()` + `vmm_map_user_page()`, mapping new
  pages lazily as the break crosses into them, not all up front.
- `syscall_reset_heap(pml4_phys, heap_base)` (`syscall.h`) -- arms
  `SYS_SBRK` for one process before it runs, same
  single-process-at-a-time scope as `process_run_ring3()` itself (not
  scheduler-aware).
- `kernel/core/echo_test.c` / `userland/echo.c` -- the demo. Loads and
  runs a small ring-3 program (9th Multiboot2 module -- see `grub.cfg`)
  that allocates a line buffer with `sbrk()`, reads keys, and echoes
  each one back via `SYS_WRITE`. New shell command: `echotest`.

**The interesting bug this surfaced:** the first version of
`SYS_READ_KEY` was genuinely blocking -- `sti` then reuse
`keyboard_getchar()`'s existing `hlt`-until-a-key-arrives loop, same
pattern the shell's own `keyboard_read_line()` already uses safely at
ring 0. It worked for exactly one keystroke and then hung forever.
Cause: `int 0x80`'s IDT gate is an interrupt gate (see `idt.c`), so the
CPU clears `IF` on entry; `sti` lets a real keyboard IRQ preempt the
syscall handler while it's still on the stack, and that nested IRQ1
handler overwrites `g_next_kernel_rsp` (`idt.c`) -- a single global
"where to resume" pointer that's correct for the scheduler's use (see
`scheduler.c`'s design comment) but was never meant to be reentrant.
By the time the outer `int 0x80` handler's own epilogue ran, it resumed
into a stale frame instead of back into ring 3. Fixed by making
`SYS_READ_KEY` non-blocking instead (matching `SYS_GUI_POLL_KEY`) and
having `echo.c` spin-poll it in a loop -- sidesteps the whole hazard
rather than fixing `g_next_kernel_rsp`'s reentrancy, which is a bigger
job for another day.

**Also fixed while chasing that:** `vga_putc()` didn't special-case
`'\b'` -- `SYS_WRITE` forwards whatever bytes a process sends straight
through it one at a time, and a literal backspace byte was just being
drawn as `font_ttf`'s glyph for character 8 (garbage) instead of
erasing. `vga_putc()` now forwards `'\b'` to the existing
`vga_backspace()` (which already both moves the cursor back and blanks
the cell), so any process's own backspace handling -- not just the
kernel's `keyboard_read_line()` -- renders correctly.

**Verified:** clean rebuild + QEMU. Ran `echotest`, typed text,
backspaced across it, hit Enter, typed more, and confirmed it's
echoed back live and Esc exits cleanly back to a working shell
(exit code 0). Reran `syscalltest`/`writetest` afterward to confirm
the shell and other syscalls still work normally post-exit. Also
reran `ptrtest` and `schedtest` in a fresh boot -- both still pass
(pointer validation still rejects the bad pointer; the scheduler demo
still completes) -- confirming none of the syscall dispatcher changes
affected the existing paths.

**Note on the Makefile:** this adds a new userland program
(`userland/echo.c` -> `userland/echo.elf`), so `Makefile` needed new
build rules and a new `module2` line in `grub.cfg` -- both included in
this round's file list, but `Makefile` couldn't be written via the
remote-devices bridge (protected file) the same way the rest were, so
it needs applying by hand (see the assistant's message for the diff).

## Small fix -- close button's X icon no longer clips

The window title bar's minimize/maximize/close buttons had a fixed
`BTN_SIZE 18` that never got touched when font size became runtime-
selectable (previous entry). Minimize and maximize were already
hand-drawn pixel icons (a short bar, a square outline), so they scaled
fine regardless. The close button wasn't: it drew the literal font
glyph `'x'` via `gfx_draw_char()` inside that fixed 18x18px box -- fine
back when the font was a fixed 16x16 cell, but once font cells started
ranging from 11x22 (small) up to 20x40 (large), a full glyph crammed
into an 18px button clipped badly at anything above "small".

Fixed two things in `wm.c`:
- `BTN_SIZE` (a compile-time macro) replaced with `btn_size()`, derived
  from `WM_TITLEBAR_H` (itself already font-aware) so all three
  title-bar buttons grow and shrink with the active font size instead
  of staying pinned at 18px.
- The close icon no longer uses `gfx_draw_char()`. It's now a small
  hand-drawn diagonal cross (`draw_close_icon()`), same approach as the
  existing minimize/maximize icons -- drawn with `gfx_put_pixel()`
  relative to the button's actual size, so it scales cleanly at every
  font size instead of depending on glyph metrics at all.

**Verified:** clean rebuild + QEMU. Screenshotted the close button on
Notepad/About windows at both the default "small" font and after
`fontsize large` -- the X now sits centered and fully inside the red
button at both sizes, matching the visual style of the other two
buttons. Also reran the non-halting regression commands (`syscalltest`,
`writetest`, `ptrtest`, `schedtest`) in a fresh boot to confirm nothing
outside the window manager was affected -- all passed.

## Small addition -- fix taskbar/window layout bugs, make font size runtime-selectable

Two problems surfaced once real windows were actually opened on the new
font (previous entry): the taskbar's Start/window buttons overlapped and
clipped each other's labels ("Star", "Notepa" with the tail eaten by the
next button's background), and About's fixed-text window overflowed its
own right edge and, worse, its bottom (5 lines no longer fit the
window's fixed height once the font's line height grew). Root cause in
both cases: layout math using hardcoded pixel constants (`START_BTN_W
70`, `WIN_BTN_W 100`, About's `default_w/h` in `gui_apps.c`, a `menu_w
160` for the Start menu) that had been sized for whatever the font
happened to be at the time they were written, with nothing tying them to
the font's actual on-screen size -- they went stale the moment that
changed. Fixed by deriving every one of them from `gfx_char_w()`/
`gfx_char_h()` (label length x char width + margin) instead of a fixed
number, so they can't drift out of sync with the font again.

Also added the ability to change the font size at runtime (`fontsize
<small|medium|large>`), both because it's generally useful and because
fixing the bugs above properly required font size to stop being a
compile-time constant anyway.

**Added:**
- `tools/genttf.py` now bakes THREE sizes (small 11x22, medium 16x32 --
  the previous entry's size, large 20x40) into one `font_ttf.c/h`, keyed
  by a `struct font_ttf_variant { glyphs, w, h, name }` array rather than
  a single flat glyph table.
- `gfx.c`/`gfx.h`: `FONT_CHAR_W`/`FONT_CHAR_H` (compile-time macros) are
  gone, replaced by `gfx_char_w()`/`gfx_char_h()` (query the currently
  active size) and `gfx_set_font_size()`/`gfx_font_size()`/
  `gfx_font_size_name()`. `gfx_draw_char()`/`gfx_draw_string()` now look
  up the active `font_ttf_variant` on every call instead of using a
  fixed glyph table and size.
- `vga.c`/`vga.h`: `vga_reflow()` -- recomputes the framebuffer console's
  cached `console_cols`/`console_rows` from the current font size and
  clears the screen. Needed because those two are cached (recalculating
  on every `vga_putc()` would be wasteful) and would otherwise still
  reflect whatever font was active at boot.
- Shell command `fontsize <small|medium|large>` -- calls
  `gfx_set_font_size()` then `vga_reflow()`.
- **Default font size changed from "medium" to "small"** -- the medium
  size (this project's second font iteration) read as a bit large once
  real GUI content was on screen; `fontsize medium`/`large` are still one
  command away.

**Fixed (the actual layout bugs):**
- `wm.c`: `START_BTN_W`/`WIN_BTN_W` replaced with `start_btn_w()`/
  `win_btn_w()`, computed from `"Start"`'s length and the 7-char taskbar
  label cap respectively, times `gfx_char_w()`, plus fixed padding. The
  Start menu's width is now `start_menu_w()` (room for a 12-char app
  name) instead of a fixed 160px, used consistently by both
  `draw_start_menu()` and the click hit-test in `handle_left_click()` --
  those two now can't drift apart the way the old duplicated-constant
  version could have.
- `wm.h`'s `WM_TITLEBAR_H` macro (used in 9 places across `wm.c`) now
  expands to `(gfx_char_h() + 8)` instead of `(FONT_CHAR_H + 8)` -- all 9
  call sites got the fix for free since it's still just a macro, only
  now backed by a function call instead of a compile-time constant.
- `apps/gui_apps.c`: About's window size bumped from 340x170 to 520x300
  -- big enough for its fixed ~24-char, 5-line text even at the largest
  baked font, accepting a little extra margin at smaller sizes rather
  than risking clipping at the largest one. Notepad's content area was
  already fully dynamic (recomputes its own column/row count from the
  window size in `notepad.c`), so it didn't need a size change, only the
  `FONT_CHAR_W/H` -> `gfx_char_w()/gfx_char_h()` swap.
- `notepad.c`'s `TOOLBAR_H`/`BTN_W` macros and `about.c`'s `line_h`,
  same treatment.

**Verified:** clean rebuild. Booted in QEMU, scripted mouse clicks over
QMP (`input-send-event`, relative motion + button events -- the PS/2
mouse driver has no absolute-positioning concept, so clicks had to be
driven the same way a real mouse would) to open Notepad and then About
exactly as originally reported, and screendumped the result: taskbar
buttons no longer clip or overlap, About's text fits cleanly inside its
border. Verified `fontsize large` from the shell -- console reflows
correctly (confirmed via screendump) and GUI mode's taskbar resizes its
buttons to match on the very next frame, with no separate "apply" step
needed since nothing caches font metrics outside of `vga.c`'s console
column/row count. Reran the full returning-command regression chain
(`syscalltest`, `writetest`, `ptrtest`, `schedtest` back to back) at the
new default (small) font size -- all four still pass; this change only
touches drawing/layout code, nothing in the process-isolation or
scheduler paths.

## Small addition -- real anti-aliased font (JetBrains Mono, baked at build time)

The 16x24 (nearest-neighbor 3x scale of an 8x8 source) font from the
previous entry was still fundamentally blocky pixel art -- bigger, but not
sharper. Replaced it with a real TrueType face rendered offline and baked
into a static bitmap, so the anti-aliasing is genuine (done once by an
actual font rasterizer, FreeType via Pillow) rather than synthesized on
target.

**Why not render TrueType at runtime:** parsing `.ttf` outlines and
rasterizing Bezier curves needs floating point, and the kernel is built
with `-mno-sse -mno-sse2` -- using floats in kernel code would mean
adding FPU/SSE state save-restore to every context switch and interrupt
(`isr_common`, `context_switch.asm`) first, plus there's no heap
allocator yet for a rasterizer's scratch buffers. None of that exists
today, and starting it just for font rendering would dwarf the actual
goal. Baking the font offline sidesteps all of it -- the kernel just
copies pre-computed alpha bytes, the same kind of static data
`font8x8_basic` already was.

**Added:**
- `tools/genttf.py` -- the same "generate at build time, bake into a C
  array, commit the result" pattern `genfont.py` established for the
  original hand-drawn 8x8 font, except the source of truth is now a real
  `.ttf` (JetBrains Mono Regular, SIL OFL 1.1 -- `tools/OFL.txt` carries
  the required license text) rendered with Pillow/FreeType at a fixed
  16x32px cell size, baseline-aligned, one 0-255 grayscale alpha byte per
  pixel.
- `kernel/drivers/font_ttf.c` / `kernel/include/font_ttf.h` -- the baked
  output: `font_ttf_glyphs[95][32][16]`, ASCII 32-126.
- `gfx.c`'s `gfx_draw_char()` rewritten around the new format: instead of
  the old 1-bit-glyph-plus-nearest-neighbor-upscale loop, it now
  alpha-blends each glyph pixel's 0-255 coverage value between the
  caller's fg/bg colors, per channel. New helpers `unpack_channel()` /
  `pack_channel()` convert between the framebuffer's native packed pixel
  format (whatever bit positions/sizes `gfx_init()` read from the
  Multiboot2 framebuffer tag) and plain 0-255-per-channel space, since
  blending has to happen in the latter.
- `gfx.h`'s `FONT_CHAR_W`/`FONT_CHAR_H` updated to 16/32 to match the
  baked glyph size -- these are now the font's native resolution, not an
  upscale factor of something smaller.

**Removed:** `kernel/drivers/font8x8.c` / `kernel/include/font8x8.h` (the
1-bit hand-drawn font) and their one remaining include, in `gfx.c`.
`tools/genfont.py` (the generator for that font) is left in the tree for
history but nothing includes its output anymore.

**Verified:** clean rebuild, no new warnings. Booted in QEMU and
screendumped both the shell/text console and `gui` mode's window
manager -- glyphs render with real anti-aliased edges (visibly smooth
diagonals and curves, not stair-stepped pixels), and the window manager's
title bars, taskbar, and Start button all still lay out correctly with
the new cell size (nothing hardcodes the old dimensions -- everything
already went through `FONT_CHAR_W`/`FONT_CHAR_H`). Reran the full
returning-command regression chain (`syscalltest`, `writetest`,
`ptrtest`, `schedtest` back to back in one boot) to confirm the font
swap -- which touches `gfx.c`, shared by every console/GUI code path --
didn't disturb anything in the process-isolation or scheduler work; all
four completed correctly.

## Small addition -- higher-resolution framebuffer + bigger font

The framebuffer was requested at 800x600 with an 8x8 font scaled 2x (16x16
on-screen cells) since Milestone 5 or so. Bumped both up: GRUB now gets
asked for 1280x720 (`kernel/core/boot.asm`'s multiboot2 framebuffer tag),
and the font scale went from 2x to 3x (`FONT_CHAR_W`/`FONT_CHAR_H` in
`gfx.h`, 16 -> 24) so text stays comfortably legible on the bigger canvas
instead of shrinking relative to it. Both are nearest-neighbor integer
scaling with no interpolation, so "bigger" doesn't cost any sharpness --
same crisp square pixels as before, just larger ones.

**Also changed:**
- `gfx.c`'s `GFX_MAX_PIXELS` (the fixed-size double-buffer backing array
  used by the window manager) bumped from `1024*768` to `1920*1080`, so
  double buffering -- and the flicker-free repaints it enables -- keeps
  working at the new resolution, with headroom if GRUB/QEMU ever pick
  something bigger than the 1280x720 preference.
- `Makefile`'s `run`/`run-nographic` targets: added `-vga std` (pins the
  emulated video card explicitly rather than relying on QEMU's
  per-host/per-distro default) and `-m 256` (was unset, i.e. QEMU's
  128 MiB default -- the bigger back buffer plus four scheduler kernel
  stacks made a little more headroom worth having). `run`'s existing
  `-display gtk,zoom-to-fit=off` was already doing the right thing here
  (native resolution, one real pixel per emulated pixel, no window-level
  scaling to blur the font) and didn't need to change.

**Verified:** clean rebuild, no new warnings. Booted headless in QEMU and
confirmed via screendump that the framebuffer really did come up at
1280x720 (not just requested -- the actual mode GRUB negotiated). Checked
both consumers of the resolution: the shell/text console renders at the
new size with the bigger font, and `gui` mode's window manager (taskbar,
Start button, clock) still lays out correctly across the full new
1280x720 desktop with no leftover 800x600 assumptions anywhere in
`apps/` or `kernel/` -- the only hardcoded 800/600 in the whole tree was
the one line in `boot.asm` that requests the mode in the first place.

## Milestone 16 -- Preemptive round-robin scheduler

Every milestone through M15 ran at most one ring-3 process at a time:
`process_run_ring3()` (`process.c`) drops to ring 3 and only gets control
back when that process calls the exit syscall, via a setjmp/longjmp-style
save/restore of the CALLER's kernel context. That's real and useful, but
it's fundamentally a function call, not scheduling -- nothing else can
run while a process is "in flight" through it. This milestone adds
honest preemptive multitasking on top, without touching that mechanism.

**Added:**
- `kernel/core/scheduler.c` / `kernel/include/scheduler.h`. The two
  mechanisms coexist, chosen per-syscall by whether the exiting process
  is scheduler-managed (`scheduler_current_pid()`, checked by
  `syscall.c`'s `SYS_EXIT` handler). Every M8-M15 test command keeps
  using `process_run_ring3()` untouched.
- The core trick: `isr_common` (`isr.asm`) already saved a process's full
  register state (15 GP regs + vector + error code + the CPU-pushed
  rip/cs/rflags/rsp/ss) onto whatever stack was active when the
  interrupt fired, called `isr_dispatch(regs)`, then -- unmodified
  through M15 -- just popped those same registers back off *the same*
  stack and `iretq`'d, resuming exactly what was interrupted. This
  milestone generalizes that last step: `isr_common` now reloads `rsp`
  from a global, `g_next_kernel_rsp` (defined in `idt.c`), immediately
  before the pop+`iretq` sequence. `isr_dispatch` sets it to `regs` (a
  no-op -- resume what was interrupted) at the very top of the function,
  for every vector, unconditionally. Only `scheduler_tick()` (vector 32,
  the timer, and only when armed) or `scheduler_on_exit()` (called from
  `syscall.c`'s `SYS_EXIT` handler) ever override it, to point at a
  different saved register block instead -- another process's, or back
  to whatever kernel code (the shell, blocked in `scheduler_demo_run()`'s
  wait loop) was running before any process got the CPU.
- Four process slots (`MAX_PROCS`), each with its own dedicated 8KiB
  kernel stack, used as the CPU's `TSS.RSP0` (via
  `gdt_set_kernel_stack()`) while that process is the one running -- so
  if it's interrupted, its register block lands on *its own* stack, not
  shared with any other process. Switching processes is just: point
  `g_next_kernel_rsp` at the other one's saved block, switch `CR3`
  (`vmm_switch_address_space()` -- safe mid-ISR because every process's
  PML4 shares kernel entry 0), and repoint `RSP0` for next time.
- Launching a process for the first time synthesizes its very first
  trapframe directly (`spawn_from_module()`), laid out identically to a
  real one `isr_common` would have saved (r15..rax zeroed, rip/cs/
  rflags/rsp/ss set to the ELF's entry point and a fresh user stack) --
  this unifies "first launch" and "resume after preemption" into one
  mechanism, since the ordinary epilogue can't tell the difference.
- `userland/counter_a.c` / `counter_b.c` -- two tiny freestanding ring-3
  programs, no yield syscall anywhere in this project, that each print
  their own letter ('A' or 'B') 20 times with a long busy-spin (spanning
  several 100Hz timer ticks) between prints. If the output interleaves on
  screen instead of printing all 20 As followed by all 20 Bs, the only
  possible explanation is the timer preempting one process mid-spin and
  handing the CPU to the other.
- Shell command `schedtest` (`scheduler_demo_run()`) -- spawns both
  counter processes, arms the scheduler, blocks (`hlt` loop) until both
  exit, then disarms it again so every other command behaves exactly as
  it did before this milestone.

**Safety for M8-M15 (disarmed by default):** `scheduler_armed` starts
false and is only ever set true, briefly, inside `scheduler_demo_run()`;
it's set back to false before that function returns, even on failure.
`scheduler_tick()` returns immediately when disarmed, leaving
`g_next_kernel_rsp` at its default (no-op). `scheduler_on_exit()` is only
ever reached via `syscall.c`'s `scheduler_current_pid()` guard, which
returns 0 unless `scheduler_demo_run()` spawned something -- so every
existing test command's exit path is completely untouched.

**Verified, not just assumed to work:** built clean (`make iso`, no new
compiler warnings beyond two pre-existing benign linker notices about the
executable-stack section and the kernel's RWX load segment, both present
since long before this milestone). Ran `schedtest` in QEMU: the serial
log shows "demo armed, waiting for both processes to exit", two `exit()`
calls, then "demo complete, disarmed" -- and a screendump of the actual
framebuffer output reads `ABABABABABABBABABABABAABBABABABABABABA`,
genuinely interleaved rather than 20 As followed by 20 Bs, exactly the
proof the whole design depends on. Directly regression-tested the
coexistence claim: ran `syscalltest`, `writetest`, and `ptrtest` (all
M12-M14, using the legacy longjmp-return path) immediately followed by
`schedtest` in the same boot -- all four completed correctly back to
back, including `ptrtest`'s pointer-validation rejection still firing
correctly. Also separately reconfirmed `ring3test` and `elftest` (the
M8/M10 tests that deliberately fault and halt the machine by design)
still panic with the expected ring-3 diagnostics and halt as intended,
completely unaffected.

**No new bugs found during this verification pass** -- the design's
careful reuse of lessons from earlier milestones (every level of the
page-table walk needing the USER bit from M8, `sti` before a non-`iretq`
return from M12) held up on the first real test.

## Milestone 15 -- GUI in user space, first step (experimental)

A deliberately narrow first step toward moving GUI content out of kernel
space. Until now, everything graphical -- the window manager, Notepad,
About -- ran in kernel space at ring 0; separately, five ring-3 test
programs (`ring3test` through `ptrtest`) proved process isolation works
but never touched anything visual.

**Added:**
- `SYS_GUI_INIT` -- maps the real linear framebuffer directly into the
  calling process's own address space at a fixed virtual address
  (`GUI_FB_VADDR`), using the same `vmm_map_user_page()` loop as every
  other per-process mapping in this project, just repeated once per
  framebuffer page (a few hundred pages at 800x600x32bpp). Validates the
  output-info pointer with `vmm_validate_user_range()` first, same as
  `write`.
- `SYS_GUI_POLL_KEY` -- a non-blocking keyboard read, reusing
  `keyboard_try_getchar()` (already used internally by the kernel-space
  window manager's own event loop).
- `gfx_framebuffer_phys()` / `_pitch()` / `_bpp()` -- small accessors
  added to `gfx.c` so `syscall.c` can find the real framebuffer without
  reaching into that driver's internals directly.
- `userland/gui_test.c` (GRUB's fifth module, shell command `guitest`)
  -- the first ring-3 process in this project to draw real pixels and
  read real input with zero kernel-space drawing code involved once
  it's running. Fills the screen with a color, cycles it on each
  keypress, exits cleanly via `exit` on `q`.

**Verified with unusual precision, not just "it looks right":** checked
that the color on screen after each keypress matches the *exact* 24-bit
arithmetic `gui_test.c` performs on its own color value (e.g.
`0x224477 + 0x335577 = 0x5599EE`, then `+ 0x335577 = 0x88EF65`,
confirmed against the actual RGB of each screenshot) -- proof the pixels
came from ring-3 code doing real computation, not something kernel-side
that happened to coincidentally look right. Also confirmed both GUI
tracks coexist without interference: ran `guitest`, then in the same
boot separately opened the kernel-space window manager (`gui`, Notepad)
-- both worked correctly. `ptrtest` and `writetest` reconfirmed
untouched. Deterministic across 3 repeated stress-test boots.

**Honest scope, stated in the code and both READMEs, not left implicit:**
this is **not** the window manager moved to user space. There's no
scheduler, so the process has the *entire real screen* to itself while
it runs -- modal, the same way every other ring-3 test here runs one
process at a time with nothing else happening concurrently. It isn't a
window inside `wm.c` (no `struct window` involved at all), can't be
dragged or minimized, and `wm.c` has no idea it exists. Turning this
into an actual multi-window user-space GUI needs a scheduler (so a GUI
process can run alongside the compositor's own loop instead of blocking
it) and a real windowing protocol (per-window pixel buffers and
window-scoped input events, instead of "hand over the whole screen") --
both noted as next steps, neither started. See `apps/README.md`'s "GUI
in user space" section for the fuller version of this.

## Small addition -- `history` command

The shell already tracked the last 8 commands (for up/down arrow
recall), but had no way to just list them. Added `history`, which prints
them numbered, oldest first. Required moving the `history[]`/
`history_count` storage earlier in `shell.c` (it previously lived below
`dispatch()`, declared where `shell_read_line()`'s arrow-key handling
uses it) so the new `cmd_history()` could see it too -- one file-scope
declaration now, shared by both. Verified the empty case ("no commands
yet"), the numbering (including that a `history` call logs itself before
printing, same as most real shells), wraparound past the 8-entry cap
(typed 10 commands, confirmed only the last 8 show, oldest two correctly
dropped), and that up/down arrow recall still works unchanged after
moving the declarations. Full console/GUI/Notepad regression clean.

## Milestone 14 -- Syscall pointer validation

Closes the safety gap flagged (and deliberately left open) in
Milestone 13: the `write` syscall trusted its buffer pointer completely.

**Added:**
- `vmm_validate_user_range()` and `vmm_current_pml4()` in `vmm.c`. The
  validator walks the calling process's own page tables -- read via
  `CR3`, which a syscall doesn't change -- and confirms every 4KiB page
  covering `[buf, buf+len)` is present *and* user-accessible, checked at
  every level of the walk (PML4E, PDPTE, PDE, PTE), not just the leaf.
  That's the same lesson Milestone 8's bug taught the hard way (a
  correctly-flagged leaf page is still blocked if a parent several
  levels up is missing the USER bit) -- applied here from the start
  rather than rediscovered, since checking only the leaf would have
  quietly missed the exact class of bug that motivated writing this
  function in the first place. Without it, kernel-only memory is still
  *present* in every process's page tables (`PML4` entry 0 is shared --
  see Milestone 10), just not user-accessible, so a process could hand
  the kernel an address it could never legally read itself and get the
  kernel, running at full privilege, to read it on the process's behalf.
- `syscall.c`'s `SYS_WRITE` handler now calls the validator before
  touching the buffer; an invalid pointer gets `-1` back in `RAX`
  instead of being dereferenced.
- `userland/write_bad_test.c` (GRUB's fourth module, shell command
  `ptrtest`) -- proves the fix actually works, the same way
  `ring3test`/`elftest` prove isolation: deliberately passes address
  `0x1000` (real, present in every process via the shared low range, but
  never user-accessible) to `write`, and only passes if the kernel
  *rejects* it rather than reading it.

**Testing note, not a bug:** re-verified `writetest` (a genuinely valid
pointer) still succeeds exactly as before adding validation -- the risk
with this kind of change is rejecting something that should have been
allowed, which is just as real a failure as not rejecting something that
shouldn't be, and easy to miss if only the "should reject" case gets
tested. `ring3test`, `elftest`, and `syscalltest` all reconfirmed
untouched and working. Deterministic across 5 repeated `ptrtest` stress
runs, full console/GUI/Notepad regression clean.

**Still not a real process model:** `exit` and `write` remain the only
two syscalls -- still no memory allocation, no file I/O, no way for a
process to read anything back, and no scheduler.

## Milestone 13 -- write syscall

The second syscall, following `exit` in Milestone 12. Small compared to
that one, but it's the first syscall that lets a process actually *do*
something visible on its own, rather than only prove the round-trip
plumbing works.

**Added:**
- `kernel/include/syscall_abi.h` -- syscall numbers shared between the
  kernel and userland test programs in one place. Small refactor: before
  this, `SYS_EXIT` was defined separately in `syscall.h` and hardcoded
  again in `exit_test.c`; a second syscall number was a good moment to
  stop that from spreading.
- `SYS_WRITE` in `syscall.c` -- buffer pointer in `RDI`, length in `RSI`
  (capped at `SYS_WRITE_MAX`, 1024 bytes, per call), writes each byte to
  the console via `vga_putc`, returns the byte count via `RAX`. Simpler
  than `exit`: it doesn't touch `process_context_restore()` at all, just
  performs the write and returns normally, resuming ring 3 right after
  the `int 0x80`.
- `userland/write_test.c` -- the first userland program whose console
  output the *process* produces, not the kernel narrating on its behalf.
  Calls `write` with a real string, then `exit(0)`. Loaded as GRUB's
  third Multiboot2 module.
- `kernel/core/write_test.c` (shell command `writetest`) -- runs it via
  `process_run_ring3()`, same pattern as `syscall_test.c`.

**A real bug, and a genuinely useful one to know about:** the first
build of `write_test.c` failed to *link*, not run --
`relocation truncated to fit` against its own string literal. Cause: the
default x86-64 code model assumes a program's code and data live close
enough together in address space to reach via a 32-bit relocation;
`userland/link.ld` places every test program at `VMM_USER_BASE`
(512GiB), and `write_test.c` was the first one to reference anything
outside its own code (a string in `.rodata`) -- neither `hello.c` nor
`exit_test.c` ever had a reason to, since both only ever used hardcoded
integer immediates. Fixed by adding `-mcmodel=large` to
`USERLAND_CFLAGS` (confirmed via `objdump` that the fix produces a real
`movabs` full 64-bit load instead of a truncated relocation), applied to
all userland builds now so this doesn't resurface the next time a test
program needs a global.

**A real safety gap, left in on purpose rather than silently ignored:**
`write`'s buffer pointer is trusted completely -- there's no check that
it actually falls within the calling process's own mapped, readable
pages before the kernel dereferences it. Fine for now (every userland
program here is one this project wrote and trusts), but a real kernel
must validate user-supplied pointers before touching them; documented in
the source and the README rather than left as a silent gap.

**Still not a real process model:** `exit` and `write` are the only two
syscalls -- no memory allocation, no file I/O, and no way for a process
to read anything back (only ever write). No scheduler either; still one
process "in flight" at a time.

## Milestone 12 -- Syscalls (exit)

The payoff of Milestones 8-11: until now, `ring3_test_run()` and
`elf_test_run()` could only ever end in a deliberate fault-and-halt --
there was no way for ring-3 code to ask the kernel to do anything,
including stop. This adds a real syscall path, with `exit` as the first
(and so far only) syscall.

**Added:**
- `int 0x80` as the syscall entry point. Its IDT gate needs `DPL=3`
  (`0xEE`, not the `0x8E` every other gate here uses) -- otherwise ring 3
  invoking it gets an immediate `#GP` instead of ever reaching the
  handler, since a software interrupt's gate DPL is the *minimum*
  privilege allowed to invoke it via the `int` instruction.
- `kernel/core/context_switch.asm` -- a minimal hand-written
  setjmp/longjmp pair (`process_context_save`/`process_context_restore`).
  With no scheduler yet, this is what lets `process_run_ring3()` drop
  into ring 3 and later get control back from deep inside the syscall
  handler -- running on a completely different stack (the TSS's kernel
  stack, switched to automatically on any ring3-to-ring0 transition) --
  the same way an ordinary function call returns to its caller.
- `kernel/core/process.c` -- `process_run_ring3()`, wrapping the
  save/drop-to-ring3/resume sequence into one reusable call that returns
  the process's exit code, same as `ring3_test_run()`'s and
  `elf_test_run()`'s inline versions did the drop-to-ring-3 part, but
  now actually returning instead of only ever faulting.
- `kernel/core/syscall.c` -- dispatches `int 0x80`. The only syscall
  implemented is `SYS_EXIT` (code in `RDI`), which jumps straight back
  into whichever kernel code called `process_run_ring3()` instead of
  rejoining the normal interrupt epilogue that would resume ring 3.
  Anything else is currently a no-op.
- `userland/exit_test.c` -- a second real, separately compiled and
  linked ELF64 test program (verified with `objdump` before trusting it:
  loads `RAX=1`, `RDI=0x2a`, executes `int $0x80`) that calls
  `exit(42)` instead of deliberately faulting like `hello.c` does.
  Loaded as GRUB's *second* Multiboot2 module -- `multiboot_get_module()`
  gained an index parameter to support more than one, and `pmm.c`'s
  module reservation (see Milestone 11's bug) now loops over all of them
  instead of just the first.
- `kernel/core/syscall_test.c` (shell command `syscalltest`) --
  demonstrates the whole thing: loads `exit_test.elf`, runs it via
  `process_run_ring3()`, and prints the exit code it gets back. Unlike
  `ring3test`/`elftest`, this command *returns* -- verified the shell
  stays fully responsive to further typed commands afterward, not just
  that it printed a prompt.

**Two real bugs this caught, both in the context-switch design itself:**
1. The first working version of `process_context_restore()` used a plain
   `ret`, trusting that stack memory at the saved `RSP` still held the
   original return address. It didn't: `process_run_ring3()` immediately
   pushes more data after the save call returns (building the `iretq`
   frame), and those pushes legitimately reuse that exact, "freed"
   stack slot. Confirmed by instrumenting both the save and restore
   sides with serial output and directly comparing the memory content at
   the saved address -- it had changed from a valid kernel code address
   to `0x1b` (exactly `SEL_USER_DATA`, i.e. part of the `iretq` frame
   that got pushed on top of it). Fixed the way real `setjmp`/`longjmp`
   do it: capture the return address directly into the saved struct as
   data (read once, before anything else can overwrite it), and `jmp` to
   that saved value directly rather than trusting the stack.
2. Even after that fix, the shell stopped responding to the keyboard
   after a successful `syscalltest` run -- exit code printed correctly,
   but no further input worked. Cause: bypassing the normal interrupt
   epilogue (which would `iretq`, restoring `RFLAGS` and re-enabling
   interrupts as a side effect) meant interrupts -- disabled on entry to
   any interrupt gate, including `int 0x80`'s -- never got re-enabled.
   IRQ1 (keyboard) could never fire again after the first exit syscall.
   Fixed with an explicit `sti` right before the final jump.

**Still not a real process model:** `exit` is the only syscall --
nothing exists yet for a process to read input, write output, or
allocate memory beyond what it already has mapped. There's also still no
scheduler; only one process can be "in flight" through
`process_run_ring3()` at a time.

## Milestone 11 -- ELF loader

Continuing process isolation from Milestones 8-10. Until now, the
"program" running in a private ring-3 address space was 17 hand-encoded
machine-code bytes written directly into a page -- real ring 3, real
isolation, but not a real program.

**Added:**
- `kernel/core/elf.c` -- a minimal ELF64 loader. Parses `PT_LOAD`
  program headers only (no relocations, no dynamic linking, no
  section/symbol-table parsing), allocating a real physical frame per
  page from `pmm.c` and mapping each one via `vmm_map_user_page()`.
  Handles partial pages correctly (a segment's `p_memsz` can exceed its
  `p_filesz` for .bss, and `p_vaddr` doesn't have to start on a page
  boundary).
- `userland/hello.c` -- a real, separately compiled and linked ELF64
  test program: no libc, no crt0, just a `_start` that writes a marker
  value to a fixed address and executes `hlt` (same proof pattern as
  `ring3_test.c`, now via a real binary). `userland/link.ld` places it
  at `VMM_USER_BASE` to match what `vmm.c` expects.
- GRUB loads `hello.elf` as a **Multiboot2 module** (`grub.cfg`'s
  `module2` line) -- the standard mechanism for handing a
  bootloader-loaded file to the kernel, essentially a minimal initrd.
  `multiboot_get_module()` (new in `multiboot.c`) finds it.
- `kernel/core/elf_test.c` (shell command `elftest`) -- creates a
  private address space, calls `elf_load()` to load `hello.elf` into it,
  maps a marker page and a stack page alongside the loaded segments, and
  drops to ring 3 at the ELF's real entry point. Same fault-based proof
  mechanism as `ring3_test.c`.
- Makefile: builds `userland/hello.elf` with its own flag set
  (`USERLAND_CFLAGS` -- no `-mcmodel=kernel`, since this runs as an
  ordinary ring-3 program, not part of the kernel image) and copies it
  into the ISO for `make iso`.

**A real bug this caught -- and how it was actually diagnosed:** the
first `elftest` run faulted with `entry point: 0x0` instead of the
correct `0x8000000000`. Before touching any kernel code, the ELF's raw
bytes were checked two independent ways: `od -A x -t x1z` on the file
itself, and a standalone userspace C program using the *exact same*
struct definitions and casting logic as the kernel's parser. Both
confirmed the file and the parsing logic were correct in isolation --
which meant the bug had to be something *else* corrupting the header
between when GRUB loaded it and when the kernel read it. The actual
cause: `pmm_init()` only reserved memory through the kernel's own image
end (`_kernel_end`) -- but GRUB places a Multiboot2 module wherever it
likes in physical memory, with no relationship to where the kernel
happens to sit. `elf_load()`'s very first `pmm_alloc_frame()` call was
handing back the module's *own* memory, and the segment loader zeroed
that frame before copying into it -- wiping out the ELF header while
still reading from it, later in the same function. Fixed by having
`pmm_init()` also call `multiboot_get_module()` and reserve whatever it
finds. Re-verified deterministic across 5 repeated stress-test boots
after the fix, full console/GUI/Notepad regression also clean.

**Still not done:** syscalls. `elf_test_run()` halts after the fault for
the same reason `ring3_test_run()` does -- there's still no way for a
process to ask the kernel to do anything, including exit cleanly.

## Milestone 10 -- Per-process page tables

Continuing the process-isolation work from Milestones 8 and 9. Until
now, `ring3_test`'s ring-3 code ran in the *same* address space as the
kernel -- real, but not yet actual process isolation.

**Added:**
- `kernel/core/vmm.c` -- every process now gets its own PML4 (top-level
  page table). Entry 0 is shared with the kernel's own PML4 (the same
  physical PDPT/PD structures that identity-map the low 4GiB), which is
  required, not a shortcut: an interrupt doesn't switch `CR3` on entry,
  so if a process's page tables didn't include a working mapping for
  kernel code, the very first interrupt while that process is running
  would fault trying to execute the handler. What's genuinely private
  per-process is a separate virtual range starting at 512GiB
  (`VMM_USER_BASE`) -- other processes' page tables have no entry
  pointing there at all, unlike the kernel's shared low-4GiB region.
- `p4_table` exported from `boot.asm` (`global p4_table`), so `vmm.c` can
  read the kernel's own top-level table to copy from.
- `ring3_test.c` rewritten to actually use this: it allocates its three
  frames from the PMM as before, but now creates a private address space
  with `vmm_create_address_space()`, maps them at `VMM_USER_BASE`
  instead of using their raw physical addresses, and switches `CR3`
  before the `iretq`. The diagnostic output now prints both the physical
  frame address and the virtual address it's mapped to side by side --
  visibly different (e.g. `phys 0x446000 -> virt 0x8000000000`), which
  is the concrete proof this is real address translation and not the
  identity-mapping shortcut Milestone 8 relied on. The fault handler
  also now switches `CR3` back to the kernel's own address space before
  halting, as correct practice for whenever this stops being one-shot.

**Bugs/non-bugs found:**
- Real: the same "USER bit needed at every level of the page-table walk"
  lesson from Milestone 8 applied directly to `vmm.c`'s
  `ensure_next_level()` -- every intermediate table it allocates sets
  USER, not just the final leaf page. Got this right the first time
  *because* it was already a documented lesson from the earlier bug.
- Not a kernel bug: one test run appeared to hang with no fault logged
  at all. Turned out to be the QEMU test harness dropping the first
  couple of keystrokes of the typed shell command ("ring3test" landed as
  "3test", an unknown command). Slowing the synthetic keypresses down
  fixed it; reproduced correctly and deterministically across 5 repeated
  boots afterward.

**Still not done:** an ELF loader (the "program" is still 17 hand-encoded
bytes, just running in a real private address space now) and syscalls,
so there's still no way for a process to ask the kernel to do things --
including exiting cleanly -- other than faulting into a halt.

## Milestone 9 -- Physical frame allocator

The next step in process isolation, following on from Milestone 8.

**Added:**
- `kernel/core/pmm.c` -- a bitmap physical frame allocator, 4KiB
  granularity, built once at boot from the Multiboot2 memory map. Only
  memory GRUB reports as "available" is ever handed out; everything from
  address 0 through the kernel image's actual end is reserved regardless
  of what the memory map claims about that range (firmware has no idea a
  kernel is sitting there). `pmm_alloc_frame()` / `pmm_free_frame()` are
  the whole API.
- `_kernel_end`, a new linker symbol (`linker.ld`) marking the first
  physical address after everything the kernel image occupies -- used by
  `pmm.c` to reserve the right range even as the kernel's size changes,
  rather than a hardcoded guess.
- `multiboot_mmap_foreach()` -- exposes the memory map as a callback
  iterator so `pmm.c` (and anything else that needs it later) can walk
  it, instead of only `multiboot_print_meminfo()` being able to parse
  it.
- `meminfo` now also prints live frame allocator stats (total/used/free,
  in both frame count and MB).
- `ring3_test.c` updated to allocate its three test pages
  (code/data/stack) from the real allocator via `pmm_alloc_frame()`
  instead of static `.bss` arrays -- removing one of the "not really
  real" caveats called out in Milestone 8, and doubling as the
  allocator's integration test. Re-verified deterministic across 5
  repeated boots after the change.

**A mistake caught before it shipped:** while adding
`multiboot_mmap_foreach()`, an overly broad find-and-replace accidentally
deleted part of the neighboring framebuffer-tag parser in the same file.
Caught immediately by compiling `multiboot.c` standalone
(`gcc -c multiboot.c`) before touching anything else -- the file was
rewritten cleanly and reverified before moving on.

**Still not done:** per-process page tables (there's still one shared
address space -- `pmm.c` provides the frames, but nothing yet builds a
separate `CR3` per process from them), an ELF loader, and syscalls. See
the README's "Process isolation" section.

## Milestone 8 -- Process isolation: GDT, TSS, paging, ring 3 (first steps)

The beginning of real process isolation. Everything before this ran as
kernel code in one shared address space at ring 0, with no memory
protection and no privilege boundary between any of it.

**Added:**
- `kernel/core/gdt.c` -- a real GDT (kernel code, kernel data, user code,
  user data segments) plus a TSS, replacing the minimal 2-entry GDT
  boot.asm used just to reach long mode. Crucially sets `TSS.RSP0`:
  without a valid kernel stack for the CPU to switch to, the first
  interrupt firing while running in ring 3 -- even a routine timer tick
  -- triple-faults immediately.
- `kernel/core/paging.c` -- can mark a single 4KiB page user-accessible
  by splitting the 2MiB huge page that covers it into individual 4KiB
  pages. Everything else in that 2MiB region keeps its original
  supervisor-only mapping unchanged.
- `kernel/core/ring3_test.c` (shell command `ring3test`) -- maps a small
  code+data+stack region, drops to ring 3 via a manual `iretq`, and runs
  17 hand-encoded machine-code bytes (no toolchain needed) that write a
  marker value into their own mapped memory, then deliberately execute
  `hlt`. Ring 3 can't execute `hlt`; the resulting fault is caught by the
  kernel's exception handler, which prints diagnostics proving both that
  the code executed (marker == `0xdeadbeef`) and that it was genuinely
  running at ring 3 (`CS` register's RPL bits) when blocked.
- `idt.c`'s panic handler now prints `RIP`, `CS` (with ring number),
  `error_code`, and `CR2` (for page faults) on every kernel panic, not
  just a bare message -- and supports a pluggable ring-3-fault hook
  (`idt_set_ring3_fault_hook`) so `ring3_test.c` can add its own
  diagnostics without `idt.c` needing to know anything about that test.
- `vga_write_hex()` added to the console API for the above.

**Bug found and fixed:** the first working version faulted with a *page
fault* trying to fetch the very first ring-3 instruction, not the
expected `hlt`-triggered `#GP`. Cause: x86-64 paging ANDs the `USER` bit
down the *entire* page-table walk, not just the final page --
`boot.asm`'s original PML4E and PDPTE entries were only
`present | writable`, so even a correctly-flagged leaf 4KiB page was
still blocked by a parent several levels up. Fixed by adding the `USER`
bit to those parent entries too (safe: actual access is still gated
entirely by the leaf-level entries, exactly as before).

**Deliberately not done yet** (see README's "Process isolation"
section for the full explanation): one shared address space rather than
per-process page tables, no physical frame allocator (the test's pages
are static `.bss` arrays, which work only because they're already
identity-mapped), no ELF loader (the "program" is hand-encoded bytes),
and no syscalls -- so `ring3test` halts on purpose after the fault
rather than recovering back to the shell.

## Milestone 7 -- Notepad save/load, window resize

**Added:**
- Notepad toolbar with Save/Load buttons (`apps/notepad.c`), persisting
  to a fixed filename (`notepad.txt`) via the in-memory filesystem. First
  real use of the `on_click` GUI-app callback.
- Window resizing: drag the right edge, bottom edge, or bottom-right
  corner of any non-maximized window. Minimum size clamped; a small
  resize-grip hint is drawn in the corner. Apps needed zero code changes
  to support this, since `on_draw` always queries the window's current
  content size fresh each frame rather than caching it.
- Window titles truncate instead of overlapping the minimize/maximize/
  close buttons when a window is resized narrower than its title needs.

**Bug found and fixed:** intermittently (roughly 1 in 8-10 runs),
entering the GUI would reset the whole machine back to BIOS instead of
launching. Traced via serial-log breadcrumbs to a race in
`mouse_init()`: its hardware handshake busy-polls the PS/2 data port
directly and synchronously, and if a keyboard interrupt fired during
that exact window (e.g. from typing "gui" + Enter right as the window
manager starts), the shared IRQ dispatcher (`i8042_poll()`, added in
Milestone 6) could steal the byte `mouse_init()` was waiting for --
occasionally bad enough to desync into a triple fault. Fixed by
disabling interrupts for the short handshake window. Confirmed via 8
repeated boot-and-launch cycles, all clean, after the fix.

## Milestone 6 -- Fixed GUI flicker (double buffering)

**Added:** `gfx_set_double_buffered()` / `gfx_present()` in `gfx.c`. The
window manager now draws each frame into an off-screen buffer and flips
it to the display in one pass, instead of clearing and repainting
directly into the visible framebuffer (which is what caused the
flicker). The text console leaves double buffering off, so its output
still appears immediately. Removed the redraw-rate throttle that had
been added earlier purely to make the flicker less noticeable -- once
the actual cause was fixed, the throttle only added cursor lag.

**Bug found and fixed:** while testing the fix above, keystrokes typed
into the GUI started being silently dropped whenever the mouse was
moving at the same time. Root cause: the keyboard (IRQ1) and mouse
(IRQ12) handlers were both reading PS/2 port `0x60` directly and
blindly, but the two devices share that one port -- whichever IRQ fired
first could eat a byte meant for the other, desyncing the mouse's
3-byte packet decoder in the process. Fixed by routing both IRQs through
one dispatcher, `kernel/drivers/i8042.c`, which reads the controller's
status register first and routes each byte to `keyboard_feed_byte()` or
`mouse_feed_byte()` based on the AUX status bit. This bug had existed
since the mouse driver was first added; it just took frequent enough
mouse traffic (from chasing the flicker) to expose it reliably.

## Milestone 5 -- Window manager: movable windows, Start menu, two demo apps

**Added:**
- `apps/wm.c` -- a real (if small) window manager. Windows can be
  dragged by their title bar, minimized, maximized/restored, and closed.
  Z-order and keyboard focus follow whichever window is frontmost (shown
  via a blue vs. grey title bar).
- A taskbar with a Start menu (bottom-left) listing every registered GUI
  app, plus a button per open window, plus a live clock.
- `apps/gui_apps.h` / `gui_apps.c` -- the GUI-app registry and the
  event-driven interface (`on_open` / `on_draw` / `on_key` / `on_click`)
  apps implement instead of running their own loop.
- Two demo GUI apps: **Notepad** (`apps/notepad.c`, a simple text editor
  -- also the keyboard-focus-routing test: type into it while another
  window is open and confirm keystrokes only land in whichever's on top)
  and **About** (`apps/about.c`, static content, the minimal example of
  what a GUI app can be).
- `apps/gui.c` became a one-line wrapper around `wm_run()`.
- GUI exit key changed from `Q` to `Esc`, since `Q` now needs to be
  typeable into Notepad.

## Milestone 4 -- Restructure: kernel/core, kernel/drivers, apps/

Everything had been flat under `kernel/`. Split into:
- `kernel/core/` -- boot, interrupts, timer, serial, `kernel_main`.
  Hardware bring-up only; knows nothing about apps.
- `kernel/drivers/` -- console, framebuffer graphics, font, keyboard,
  mouse, the in-memory filesystem.
- `kernel/include/kapi.h` -- the one header apps are meant to include,
  aggregating the driver APIs they're allowed to use, so drivers can be
  reshuffled internally without every app needing an edit.
- `apps/` -- programs, each with a single entry point, registered in
  `apps/apps.c`. `kernel_main()` now calls `apps_start()` instead of
  naming the shell directly -- it doesn't know or care what apps exist.
  Adding a new app became "write the file, add one line to the
  registry" (see `apps/README.md`).
- Added `apps` (list registered apps) and `run <name>` (launch any of
  them) shell commands.
- Explicitly scoped at the time: this is a *source-level* module
  boundary (separate files, a defined API surface), not process
  isolation -- everything still ran in one address space at ring 0. (See
  Milestone 8 for the actual start of changing that.)

## Milestone 3 -- Graphical mode

**Added:**
- A linear RGB framebuffer requested from GRUB via a Multiboot2 tag,
  with automatic fallback to legacy 80x25 VGA text mode if none is
  available. The console (`vga.c`) renders through whichever backend is
  active without the rest of the kernel needing to know which.
- An original 8x8 bitmap font, generated by `tools/genfont.py` (glyphs
  authored as readable ASCII art, with a rendered preview for visual
  checking before committing).
- A basic desktop: background, a window, a taskbar with a live clock,
  and a mouse cursor tracking real PS/2 movement.

**Bug found and fixed (twice):**
1. Requesting the framebuffer at boot made GRUB switch to graphics mode
   *before* the kernel even started, which silently broke the legacy
   text-mode shell (blank screen, though the kernel was actually running
   fine underneath). Fixed by making the console auto-detect and render
   through the framebuffer with the bitmap font when one's available.
2. Text still looked "blurry" after that fix -- but not because of
   display/compositor scaling (ruled out across several rounds: 100%
   desktop scale, local display, only the QEMU window affected, `Esc`
   and `GDK_BACKEND=wayland` overrides all tried). The actual cause: the
   hand-authored font glyph bitmaps were simply malformed (e.g. "GRUB"
   rendered as "GRJ8", lowercase `t` looked like a Greek tau). Rewrote
   the font properly using `tools/genfont.py`, checking a rendered
   preview this time before shipping it.

## Milestone 2 -- Filesystem, memory info, more shell commands

**Added:** an in-memory filesystem (`ls`, `cat`, `touch`, `write`,
`append`, `rm`), `meminfo` (parses the Multiboot2 memory map), `reboot`,
`color`, and command history via the up/down arrow keys.

## Milestone 1 -- Boots to a working shell

**Added:** GRUB2/Multiboot2 boot with a hand-written 32-bit-to-long-mode
transition, VGA text-mode console, PS/2 keyboard driver, PIT timer +
CMOS RTC, and a line-input shell with `help`, `clear`, `time`, `uptime`,
`echo`, `about`.

No cross-compiler needed: since the target and host are both x86-64,
plain system GCC with `-ffreestanding` and kernel-appropriate flags
works fine.
