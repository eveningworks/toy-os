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
- Docs catch-up after the tray/NX/shell_flow work above: `CLAUDE.md`'s
  QMP-testing gotcha list now points at `shell_flow.py` from the
  `send_text()` keyboard gotcha it directly solves, and adds a gotcha
  of its own -- don't `pkill`/kill-by-pattern across every
  `qemu-system-x86_64` process, since that can't distinguish a
  QMP-headless test instance from the user's own interactive `make
  run` window; only kill the PID your own launch's `-pidfile` wrote.
  `docs/roadmap.md`'s Milestone 9 entry no longer claims the tray/
  clock tick is damage-scoped (it was, briefly, then reverted -- see
  below) and its Milestone 2 "Details" prose (NX/W^X bullets) was
  still describing them as not-yet-done despite both being checked off
  further up the same file. New `docs/decisions.md` entry ("The
  taskbar/tray falls back to full-screen repaint on purpose, not as an
  oversight") for the two real bugs an earlier scoped-damage attempt
  caused and why the fix was reverting the optimization, not patching
  around it -- `docs/roadmap.md` linked to this entry before it
  actually existed.

### Added
- `tools/shell_flow.py`: a `gui_flow.py`-style helper for the physical
  (pre-`gui`) shell -- `ShellFlow.run_command(cmd, subdir=...)` types a
  full command (spaces/hyphens/underscores/etc handled automatically)
  and screenshots the result, instead of a testing session
  hand-interleaving `send_text()`/`send_key('spc')`/
  `combo(['shift','minus'])` calls character by character every time.
  Prompted by two real mistakes in the same session (a dropped space,
  a hyphen typed where `run nx_test`'s underscore was needed) while
  testing the NX-enforcement entry below. Deliberately returns a
  screenshot path, not parsed text -- this kernel's console picks a
  framebuffer (glyphs-as-pixels) backend whenever GRUB provides one,
  the normal case here, so there's no legacy-VGA-text-buffer
  memory-read shortcut to plain text; see the module's own docstring.
  Verified: `run_command("run nx_test")` typed the full command
  correctly (including the underscore) in one call.
- NX bit enforcement + W^X for userspace process pages (Milestone 2,
  docs/roadmap.md). Previously every mapped page anywhere -- kernel or
  user, code or data -- was present+writable(+user), full stop; a
  user ELF's `.data`/`.bss`/stack were as executable as its `.text`,
  and every PT_LOAD segment got mapped identically regardless of its
  real ELF permission bits (`p_flags` was parsed but never read).
  Scoped to userspace process pages only, matching this session's
  choice -- the kernel's own `boot.asm` identity map (flat 2MiB huge
  pages, no code/data split) is unchanged and stays RWX; that's the
  separate, larger "W^X on kernel... mappings" roadmap item.
  - `kernel/core/boot.asm`: `enable_paging` now also sets EFER.NXE
    (bit 11 of the `0xC0000080` MSR) alongside the existing long-mode
    bit -- required once, globally, for the CPU to honor PTE bit 63 at
    all.
  - `kernel/core/vmm.c`/`vmm.h`: new `PAGE_NX` bit and
    `vmm_map_user_page_flags(pml4_phys, vaddr, paddr, writable,
    executable)`. The existing `vmm_map_user_page()` is now a thin
    wrapper defaulting to writable+NOT executable -- the correct,
    secure default for every pre-existing call site (a process's
    stack, SYS_SBRK heap growth, the GUI framebuffer, a window's pixel
    buffer -- all data, never code), so those all become non-executable
    for free with no call-site changes. `kernel/core/ring3_test.c`'s
    hand-assembled code page is the one call site needing
    executable=1 explicitly, via the new `_flags` variant.
  - `kernel/core/elf.c`: `load_segment()` now actually reads
    `ph->p_flags` (new `PF_X`/`PF_W` constants) and maps each PT_LOAD
    segment's pages with its own real writable/executable bits via
    `vmm_map_user_page_flags()`, instead of the old blanket
    present+writable+user every segment used to get.
  - `userland/link.ld`: this is what makes the above mean anything --
    an explicit `PHDRS` block now emits three separate, page-aligned
    (`ALIGN(4096)`) `PT_LOAD` segments (`.text` R+X, `.rodata` R-only,
    `.data`+`.bss` R+W) instead of one merged segment covering
    everything. NX/W^X is enforced per 4KiB page, so without this
    split every userland ELF would still have `.text` and `.data`
    sharing pages and nothing to differentiate. Confirmed via
    `readelf -lW`: 3 distinct `PT_LOAD` entries with the expected `R
    E`/`R`/`RW` flags, each `VirtAddr` exactly 4096-aligned. Also
    incidentally fixes every userland `.elf`'s `ld: ... has a LOAD
    segment with RWX permissions` build warning (kernel.bin's own
    warning is unchanged/expected -- out of scope, see above).
  - New `userland/nx_test.c` (+ Makefile/`RUN_ALLOWED_BINS` wiring,
    same pattern as `crash_test.c`): copies a tiny valid instruction
    (`ret`, 0xC3) into a writable `.bss` buffer and calls it as a
    function -- exactly the shape of a real exploit's second stage.
    Verified via QMP (`run nx_test`): the kernel reports `RING-3
    PROCESS CRASHED: Page fault`, `error_code=0x15` -- decodes to
    Present + User + Instruction-Fetch, the specific signature of an
    NX violation, not a generic unmapped-page fault -- and the
    injected code never executes (no "UNEXPECTEDLY SURVIVED" message).
    Process torn down, control returned cleanly to the shell, same
    recoverable-fault path `crash_test` already exercises.
  - Regression-verified via QMP: `run ls`, `run crash_test` (still
    faults exactly as before, unrelated kernel-only-page violation),
    and a GUI-spawned `ls` via Terminal's async `run` (scheduler.c's
    separate spawn path) all behave identically to before this change.
  - Found (not caused) during verification: `run hello` page-faults on
    a write to `USERLAND_MARKER_ADDR` (`kernel/include/
    userland_contract.h`), an address only ever mapped by a
    `kernel/core/elf_test.c` that no longer exists in this tree --
    confirmed by building unmodified `main` and reproducing the
    identical crash there too. Pre-existing, unrelated to this change;
    left as-is (either `hello.c`'s marker write or the file's own
    stale top comment needs updating, a separate small cleanup).

### Removed
- Legacy on-disk-config migration code, by explicit request -- this
  project is pre-1.0 and the user is fine just recreating a fresh
  `disk.img`/`/etc` state instead of carrying forward-migration code
  for formats nothing still produces. A research pass first confirmed
  `kernel/drivers/tfs.c` (the user's initial suspicion) actually has
  *no* removable migration code -- an old-version disk is already just
  reformatted, identical to a blank/foreign one, no special-case logic
  exists to strip. The two real, removable migrations were elsewhere:
  - `kernel/core/font_config.c`: the block reading old `/etc/fontsize`
    and migrating it into `/etc/toyos.conf`'s `font_size` key (then
    deleting the old file) -- removed; `font_config_init()` now just
    reads `toyos.conf` directly. `kernel/include/font_config.h`'s
    stale reference to the old file removed too.
  - `kernel/core/tz.c`: the block reading either old `/etc/timezone`
    or `/timezone` (bare-text city name, two different pre-`/etc`-
    consolidation locations) and migrating into `toyos.conf`'s
    `timezone` key -- removed; `tz_init()` now just reads
    `toyos.conf` directly. The file's own top comment's "small tour of
    how /etc has evolved" narration (describing the now-gone migration
    path) trimmed to match.
  - `kernel/include/fs.h`'s `FS_DATA_MAX` -- already marked `Vestigial`
    in its own comment, a leftover per-file ceiling from TFS2 v1 that
    nothing referenced anymore (confirmed by grep before removing).
    Three comments in `kernel/core/etc_config.c`/`apps/editor.c`/
    `apps/editor.h` that explained "why this isn't bounded by
    `FS_DATA_MAX`" reworded to not reference the now-gone symbol name.
  - `docs/decisions.md`'s `/etc` consolidation entry updated -- it
    referenced "the migration logic" in the past tense pointing at
    code that no longer exists.

  Verified: `make clean && make all && make iso` + `boot_smoke_test.py`
  all pass. Live via QMP: set `fontsize 24` and `timezone helsinki`
  through the shell, rebooted, confirmed both persisted correctly
  through the simplified (`toyos.conf`-only) init paths -- the font
  was visibly larger and `timezone`'s picker showed `helsinki` marked
  as the active selection.

### Added
- Real GDB debugging via `make debug` -- boots toy-os frozen at CPU
  reset (QEMU's `-s -S`) so a host `gdb` can attach
  (`target remote localhost:1234`) for real breakpoints, single-step,
  and register/memory inspection. No kernel-side GDB protocol code
  needed at all -- QEMU's own built-in stub emulates the CPU directly,
  independent of the guest OS (see `docs/decisions.md` for why an
  in-kernel serial stub, the first framing of this idea, was
  unnecessary). `CFLAGS`/`USERLAND_CFLAGS` gain `-g` (kept at `-O2`,
  not dropped to `-Og`) so `kernel.bin`/every userland ELF carry real
  DWARF symbols -- function names and source lines, not just raw
  addresses. Verified end-to-end: `break kernel_main` + `continue` over
  a real `gdb` session correctly ran the CPU from reset through
  GRUB/multiboot2 and stopped exactly at `kernel_main`, with a working
  backtrace showing source file/line.
- `make run-audio` -- same as `make run`, plus `-audiodev pa,id=snd0
  -machine pcspk-audiodev=snd0` so the PC speaker (`beep`, see below)
  is actually audible -- confirmed working on a real machine.
  `make run` itself is unchanged (no default audio backend assumed --
  the right one is host-specific). Also fixes a stale `help` target
  claim that `run` opens a GTK window; it's been SDL for a while
  (`-display sdl,grab-mod=rctrl`, see `CLAUDE.md`).
- MBR + GPT partition table parsing (Milestone 3, `docs/roadmap.md`):
  new `kernel/include/partition.h`/`kernel/drivers/partition.c`,
  `partition_read_table()` -- reads LBA 0 via `ata_read_sector()`,
  checks the `0x55AA` signature, and either parses up to 4 legacy MBR
  entries or (if a protective `0xEE` entry is found) reads LBA 1 as a
  GPT header, validates its CRC32, and reads its partition entry array
  (type/unique GUIDs, LBA range, UTF-16LE name). Read-only, parse-only
  -- `disk.img` is still one raw TFS2 blob at LBA 0 (see
  `docs/tfs2-spec.md`), never consulted by the mount path. New
  `parttable` shell command (`apps/shell_sys.c`) prints whatever was
  found, formatted like `lspci`. New `tools/mkpart_test.py` writes a
  synthetic MBR or GPT onto a disk image for testing, TFS2-mount-
  preserving (patches only the partition-table byte ranges TFS2 itself
  never touches, so the real filesystem underneath still mounts
  normally instead of being auto-reformatted).
  Verified two different ways for the two cases -- see
  `docs/decisions.md` for why they had to differ: the MBR path (and
  the "no partition table" case) live, via QMP -- patched `disk.img`,
  booted, ran `parttable` from the shell, confirmed the printed
  type/LBA/sector fields matched exactly what was written, for both a
  plain MBR and a protective-MBR-only (GPT-signaling) disk. The GPT
  header-parsing path itself (CRC32 validation, entry array read) was
  verified via a host-compiled unit test including the real,
  unmodified `partition.c` against a synthetic image, instead of a
  live boot -- `kernel/drivers/tfs.c`'s `tfs_selftest()` unconditionally
  overwrites LBA 1 (the GPT header's mandated location) with a real
  journal header on every single boot, before the shell is ever
  reachable, so a custom GPT header there can never survive to be read
  by a live `parttable` call. Confirmed correct CRC32, both partitions'
  type/unique GUIDs, LBA ranges, and names exactly matching what was
  written.
- PC speaker beep (Milestone 19, `docs/roadmap.md`): new
  `kernel/drivers/speaker.c`/`speaker.h`, `speaker_beep(freq_hz,
  duration_ms)` -- programs PIT channel 2 (ports `0x42`/`0x43`, same
  square-wave mode 3 channel 0 already uses for the system timer) and
  gates it through to the physical speaker via port `0x61` bits 0-1,
  restoring the port's prior value afterward rather than just clearing
  those bits. Exposed via a new `beep` shell command (`apps/shell_sys.c`)
  -- a fixed 800Hz/200ms tone, "simplest possible output" by explicit
  request rather than a freq/duration-adjustable command. Blocks for
  the tone's duration by busy-waiting on `pit_ticks()` (10ms
  resolution) -- no scheduler-aware sleep/delay primitive exists in
  this kernel yet, same gap noted under Milestone 15. Verified:
  `boot_smoke_test.py` passes, and `beep` from the shell prints "beep!"
  and returns control to the prompt promptly (headless QEMU has no
  audio device attached, so the tone itself can't be verified
  programmatically -- the PIT/port-0x61 programming completing cleanly
  and the busy-wait duration behaving as expected is what's testable
  here).
- Stack canaries (Milestone 2, `docs/roadmap.md`): `-fstack-protector-strong`
  is on for both the kernel (`CFLAGS`) and userland (`USERLAND_CFLAGS`)
  now, previously explicit `-fno-stack-protector` in both. Uses
  `-mstack-protector-guard=global` (a plain extern `__stack_chk_guard`)
  rather than GCC's TLS-based default, since this kernel has no
  FS/GS-base infrastructure for that default to read; the guard value
  is a fixed compile-time constant, not random, since there's no
  entropy source yet either. New `kernel/core/stack_protector.c`
  (kernel-side `__stack_chk_guard`/`__stack_chk_fail`, the latter
  printing a panic banner and halting -- no "recoverable" case for a
  kernel-side canary trip) and `userland/stack_chk.c` (userland's
  version, linked into every userland ELF now -- `SYS_WRITE` a message
  then `SYS_EXIT(2)`, an ordinary process exit from the kernel's point
  of view). New `userland/stack_smash_test.c` self-test (seeded as
  `/bin/stack_smash_test`, run via the shell's `run stack_smash_test`)
  deliberately overflows a local buffer to prove the canary actually
  catches a real overflow, not just "the kernel still boots" -- see
  `docs/decisions.md` for a real gotcha hit writing it (the overflow
  function needs `__attribute__((noinline))`, or GCC inlines it into
  `_start` and moves the canary check past code that already exited
  the process). Verified: `make clean && make all && make iso` +
  `boot_smoke_test.py` pass with the flag on kernel-wide (no
  false-positive trip during boot's own self-tests), and
  `run stack_smash_test` from the shell prints "stack smashing
  detected", exits with code 2, and returns cleanly to the prompt.

### Changed
- CLAUDE.md and `tools/` now support a direct local checkout (this
  session ran that way for the first time, not through Cowork's device
  bridge) as a first-class mode alongside the existing Cowork one,
  instead of assuming Cowork throughout. Detected via `git config
  user.name` (empty = Cowork device-bridge session, which has no git
  identity configured at all; set = direct local checkout). Confirmed
  directly: `git push`/`gh release create`/`gh release upload` all
  work fine from a local checkout (used them repeatedly this session,
  including cutting and then patching the `v0.1.0` release), unlike
  Cowork's sandbox where they're genuinely blocked by an egress proxy.
  See `docs/decisions.md` for the full writeup.
  - `CLAUDE.md`: splits "Working in the cloud sandbox vs. the user's
    machine" into a detection bit + two subsections; "Delivering
    changes" branches by mode and gains explicit PII and
    tooling-belongs-in-`tools/` standing instructions.
  - `tools/preflight.sh`: closing message is now mode-aware (checks
    git identity) instead of unconditionally pointing at
    SendUserFile/`device_commit_files`.
  - `tools/qmp_test.py`: `launch_qemu_cmd()` now returns a `qemu-system-x86_64
    ... -daemonize -pidfile <path>` command instead of one meant to be
    backgrounded with `setsid nohup ... &`/`disown -a` -- the old
    pattern turned out to be unreliable in this sandboxed environment
    specifically (spurious non-zero exit codes on the launching call,
    and the process not reliably surviving to the next tool call,
    which left a stale `serial.log`/QMP port from an earlier run
    looking like a fresh boot and caused real confusion mid-session
    chasing a phantom bug). Verified the new command launches,
    daemonizes, and accepts a QMP connection.
  - Companion update to `~/.claude/skills/toy-os-feature-workflow/`
    (outside this repo, not tracked here) doing the same mode-split for
    the workflow steps.

### Added
- Draggable desktop icons (Milestone 9, `docs/roadmap.md`): each desktop
  icon now has real per-icon grid position state (`apps/wm/desktop.c`'s
  `icon_col`/`icon_row`, previously a fixed left-edge column derived
  straight from `gui_app_registry`), draggable to any cell in a real
  multi-column grid and snapping to the nearest cell on release.
  Positions persist across reboot in `/etc/desktop.conf`, keyed by app
  name (so a `gui_app_registry` reorder doesn't scramble saved
  positions) via `kernel/include/etc_config.h`'s shared reader/writer
  (newly exposed to apps through `kapi.h`). The grid geometry and
  drag-to-reposition session are a new reusable widget,
  `apps/ui/ui_icon_grid.h`/`.c` (`icon_grid_cell_rect`/
  `icon_grid_nearest_cell`, `struct icon_drag` +
  `icon_drag_start/update/end`), mirroring `wm_input.c`'s window-drag
  shape (mouse-down arms it with a grab offset, a per-tick update
  tracks the cursor, mouse-up commits) -- built as its own widget file
  rather than desktop.c-local state, by explicit request, ahead of the
  second real caller a future file manager's icon view (Milestone 10)
  is expected to be; see `apps/README.md`'s "Shared widgets" section
  for why that's called out as a deliberate exception. Click/double-
  click-to-launch behavior is unchanged -- a plain click (no movement
  before release) just re-commits the icon to the cell it's already
  in. Two icons dragged onto the same cell simply overlap; no swap/
  displace logic yet. Verified via QMP: dragged the Notepad icon to a
  new cell (screenshot), exited to shell and re-entered GUI mode to
  confirm the position persisted (screenshot), and double-clicked the
  moved icon to confirm launch still works post-drag (screenshot) --
  see `screenshots/2026-08-12/desktop-icon-drag-*.png`.

### Fixed
- Draggable desktop icons (see the Added entry above), two issues found
  in real use right after landing:
  - Grid columns were sized to the single longest label across the
    WHOLE registry ("Task Manager"), so even a column with only short
    labels next to it (e.g. "Notepad"/"About") got that label's full
    pitch -- a much bigger gap than any actual adjacent pair of icons
    needed. `apps/wm/desktop.c`'s `current_grid()` now uses a fixed
    icon-size-driven column width (`DESKTOP_ICON_COL_W`, matching the
    existing row height for square cells) instead of a label-driven
    one -- the standard real-desktop tradeoff (fixed grid pitch
    regardless of label length; an unusually long label may run past
    its cell into a neighboring column's icon in the same row, an
    accepted quirk of freeform placement, not a bug).
  - Dropping an icon onto a cell another icon already occupied made
    them silently overlap (the `apps/ui/ui_icon_grid.h` version 1
    entry above called this out as a known "future refinement," but it
    turned out to matter immediately in practice). `desktop.c` now
    searches outward from the drop cell for the nearest free one
    (`nearest_free_cell()`, ring by ring) instead of overlapping.
  - Verified via QMP: dragged two icons into a second column (screenshot
    confirms the tight grid pitch), then dropped a third icon directly
    onto an already-occupied cell (screenshot confirms it settled into
    an adjacent free cell instead of stacking) -- see
    `screenshots/2026-08-12/desktop-icon-drag-tight-grid-fix.png` and
    `desktop-icon-drag-no-stack-fix.png`.

### Added
- Real per-window damage-region compositor (Milestone 9, Phase 1+2 of
  the plan -- Phase 3, skipping `on_draw()` for unaffected windows, is
  a deliberate follow-up, not done here). The window manager used to
  redraw the entire screen on any scene change at all, down to a
  once-a-second clock tick; it now tracks a single scene-wide damage
  bounding box per frame and clips the repaint to it.
  - `kernel/drivers/gfx.c`/`gfx.h`: new `gfx_set_clip_rect()`/
    `gfx_clear_clip_rect()`, gating `gfx_put_pixel()` (not
    `gfx_get_pixel()`, deliberately -- see the code comment) on top of
    the existing dirty-pixel-bbox blit optimization, which is
    unchanged and still does its own job one layer lower.
  - `apps/wm/wm_render.c`: `wm_damage_rect()` accumulates the
    per-frame damage bbox; `compute_window_damage()` diffs each
    window's position/size/visibility against new `last_x/y/w/h/
    last_visible` fields on `struct window` (`apps/wm/wm.h`) to catch
    drags/resizes/minimize/restore automatically. `wm_render_frame()`
    applies the accumulated region as the active clip, redraws
    everything within it back-to-front (desktop, windows in z-order,
    taskbar, menus), then resets it.
  - `apps/wm/wm.c`/`desktop.c`: explicit damage reports for changes
    the automatic geometry diff can't see on its own --
    `bring_to_front()` (z-order swap), `open_app()`/`close_window()`
    (taskbar layout change), `window_invalidate()` (now does something,
    was a no-op stub before), focused-window key/wheel delivery, and
    desktop icon drag.
  - Why redraw-in-region instead of computing exact exposed
    sub-rectangles, and the two real bugs QMP testing caught (taskbar
    staleness on close/open/reorder; a stale highlight sliver during
    icon drag) -- see `docs/decisions.md`.
  - Verified via QMP: two overlapping windows, dragging one off the
    other with the revealed area redrawing correctly, closing a window
    with the taskbar updating correctly, and a desktop icon drag with
    no stale-pixel artifact -- screenshots in
    `screenshots/2026-08-12/compositor-*.png`.

### Added
- Compositor Phase 3: skip a window's `on_draw()` (and chrome/resize-
  grip) entirely when it doesn't intersect the frame's damage region,
  instead of calling it and letting `gfx_set_clip_rect()` clip its
  writes away for free. Completes the Milestone 9 dirty-rect compositor
  plan the Phase 1+2 entry above started.
  - `apps/wm/wm_render.c`: new `window_intersects_damage()`;
    `wm_render_frame()`'s per-window loop now skips
    `draw_window_chrome()`/`on_draw()`/`draw_resize_grip()` for any
    visible window whose rect doesn't overlap the accumulated damage
    box (only when a damage box was actually reported this frame -- no
    damage still means "unknown, be safe," draw everyone, same
    full-screen fallback as before).
  - `apps/wm/wm.c`: fixed a latent bug in `bring_to_front()`'s damage
    reporting, surfaced by actually skipping draw calls -- it only
    damaged the newly-promoted window's rect, never the
    previously-frontmost window's, even though that window's titlebar
    tint (focused blue vs. unfocused gray) changes too on every z-order
    swap. Harmless under Phase 1+2 (the call still ran, just had its
    pixels clipped away, and they happened to land inside the damaged
    box in every case tested so far); became a real visible stale-tint
    bug the moment the call itself started being skipped. Fixed by
    damaging the previously-frontmost window's rect too.
  - Verified via QMP: opened two non-overlapping windows, swapped focus
    between them repeatedly via taskbar clicks with both titlebar tints
    confirmed correct after every swap; dragged, minimized, and closed
    windows and confirmed no stale pixels or missed redraws -- see
    `screenshots/2026-08-12/compositor-phase3-*.png`.

### Added
- Taskbar notification area (tray): a small right-to-left strip of
  text items next to the Start/window buttons, with a dynamic
  registration API so a GUI app can plug a live-updating item into it
  at runtime instead of the WM only ever drawing hardcoded chrome. The
  existing hardcoded taskbar clock (`draw_clock_area()`) is now itself
  tray item 0, registered through the same API, proving it end-to-end
  rather than shipping an API with no real caller.
  - New `apps/wm/wm_tray.c`/`wm_tray.h`: a fixed `TRAY_MAX_ITEMS` (6)
    array of `{active, text[TRAY_TEXT_MAX]}` slots. `tray_init()`
    registers the clock as slot 0 from `wm_run()`'s setup;
    `tray_update_clock()` replaces the old inline `rtc_read_local()`
    call at the once-a-second tick in `wm.c`; `draw_tray()` (called
    from `wm_render.c`'s `draw_taskbar()` in place of the removed
    `draw_clock_area()`) draws every active item right-to-left from
    the taskbar's right edge, same visual position the clock always
    had.
  - `apps/wm/wm.h`: new app-facing API -- `tray_register(initial_text)`
    (returns a handle or -1 if full), `tray_set_text(id, text)`,
    `tray_unregister(id)` -- following the same "push, don't poll"
    shape apps already know from `window_set_state()`, not the
    WM-polls-a-handle shape `window_start_write()` uses (a tray item's
    text only changes when the app itself decides it has, so there's
    nothing for the WM to poll).
  - Damage scoping: every registration/update/unregister call just sets
    `redraw_pending`, relying on the full-screen fallback -- matches
    every other still-unscoped piece of WM chrome (menus, dialogs, see
    `docs/roadmap.md`'s Milestone 9 entry). An earlier version of this
    entry scoped these to just the taskbar strip via `wm_damage_rect()`;
    see the "Fixed" entry directly below for the two real bugs that
    caused, and why it was reverted.
  - Verified via QMP: clock renders at its usual position and keeps
    ticking (`18:49:03` -> `18:49:19` across two screenshots), taskbar
    Start/window buttons unaffected with a window open -- see
    `screenshots/2026-08-12/tray-clock-*.png`.

### Fixed
- The tray entry above originally scoped every tray registration/
  update/unregister to just the taskbar strip via `wm_damage_rect()`
  instead of relying on the full-screen fallback -- shipped, then
  caught live on the user's own machine (not QMP-testable, since it
  only shows up once real time passes and the real PS/2 mouse moves
  around): entering GUI mode showed a black desktop with no icons at
  all, and the mouse cursor visibly stopped tracking correctly.
  - Root cause #1 (black desktop): `tray_init()` runs during
    `wm_run()`'s setup, before the main loop starts. Registering the
    clock there called `wm_damage_rect()` for the taskbar strip *before
    the very first frame*, which poisoned `wm_render_frame()`'s "no
    damage reported yet -- unknown, be safe, draw everything"
    full-screen fallback into a taskbar-only clip. `desktop_draw()`
    (icons) and the window-chrome loop still ran, but every pixel they
    wrote outside that strip was silently clipped away, so the first
    frame -- the only one that mattered, since nothing else re-damages
    the whole desktop afterward -- never actually drew the desktop.
  - Root cause #2 (cursor tracking): the once-a-second clock tick used
    to report no damage at all, which meant it forced a full-screen
    fallback redraw every single second -- an implicit, unadvertised
    safety net that kept `wm_render.c`'s cursor-under-pixels snapshot
    (`cursor_under`, used by the cheap `wm_render_cursor_move()` path)
    resynced against the real screen every second. Scoping the tick's
    damage to just the taskbar strip silently removed that safety net,
    so any drift in the cheap cursor-move path stopped self-correcting.
  - Fix: `apps/wm/wm_tray.c`'s `tray_damage()` no longer calls
    `wm_damage_rect()` at all -- it just sets `redraw_pending`, same as
    the clock always did before this feature existed. The registration
    API itself (`tray_register()`/`tray_set_text()`/`tray_unregister()`)
    is unchanged; only this internal damage-scoping optimization was
    reverted.
  - Verified via QMP: entering GUI mode now shows the full desktop
    (icons + navy background) on the very first frame, no click needed
    to "unstick" it -- see `screenshots/2026-08-12/tray-fix-*.png`.

## [0.1.0] - 2026-08-12

### Fixed
- The initial `v0.1.0` GitHub Release was published missing `toy-os.iso`
  -- `tools/run_release.sh` requires it next to `disk.img`/`disk.img.gz`
  (it errors out immediately if absent), and v0.0.9's release included
  it, but it was left off this time. Uploaded to the existing release
  as a follow-up (`gh release upload v0.1.0 toy-os.iso`), no retag
  needed.

### Changed
- Repo history rewritten (`git-filter-repo`, all 91 prior commits) to
  remove the maintainer's real name and personal email addresses from
  both commit authorship and `LICENSE`'s copyright line -- requested
  directly, for privacy. Every commit now carries a generic `toy-os
  <noreply@toy-os.local>` identity; `LICENSE` reads "toy-os
  contributors". All commit hashes and the `v0.0.9` tag changed as a
  result (force-pushed). See `docs/decisions.md` for the full
  mechanics, the verification method, and the standing convention this
  sets for every commit going forward.

### Added
- `tools/run_release.sh` -- standalone QEMU launch script shipped as a
  GitHub Release asset (v0.0.9 onward), for anyone running from just a
  release download with no repo checkout. Gunzips `disk.img.gz` if
  needed, then boots with the same device/display flags the
  Makefile's `run:` target uses.

### Fixed
- v0.0.9's release process surfaced two real gotchas, now documented
  in `docs/decisions.md`'s versioning entry: `disk.img` is a large
  sparse file (~9GB apparent, ~370KB real data) that must be gzipped
  before shipping as a release asset (raw upload both exceeds GitHub's
  2GB-per-asset limit and wastes bandwidth on zeros), and the Cowork
  cloud sandbox's outbound git proxy blocks `git push`/`gh release
  create` outright regardless of the repo token embedded in the
  remote URL (`access denied by the git proxy: ... not in this
  session's authorized repository set`) -- confirmed by a real failed
  push attempt, not assumed. Publishing a release now always ends with
  handing the user exact commands to run from their own machine.

### Added
- Wired `wm_run()` to poll a pending write instead of blocking -- Phase 3
  of the async-I/O roadmap item (see `docs/roadmap.md`). Phases 1
  (non-blocking DMA start/poll primitive) and 2 (steppable write API)
  landed earlier as `ata.c`/`fs.h` primitives with no real caller yet
  (proven standalone via the `dmatest`/`steptest` shell commands -- see
  their own CHANGELOG entries); this phase is the first real caller of
  Phase 2's `fs_write_range_begin()`/`fs_write_range_step()`. A new
  WM-global single slot (`pending_write`/`pending_write_win`,
  `apps/wm/wm_internal.h` -- same "-1/NULL means none" idiom as
  `dragging`/`resizing`) holds the handle; `wm_run()`'s main loop
  (`apps/wm/wm.c`) calls `fs_write_range_step()` once per frame instead
  of ever calling `fs_write_range()`/`fs_write()` and blocking, so one
  frame's extra cost is bounded to a single filesystem block's write
  latency, not the whole file. Two new public entry points in `wm.h`:
  `window_start_write()` (registers a handle, refuses a second
  concurrent one) and `window_write_pending()` (lets an app check before
  starting a new write); completion is delivered back via a new
  `gui_apps.h` callback, `on_write_complete(win, success)`, called once
  polling reaches `FS_STEP_DONE`/`FS_STEP_FAILED`.
  - Notepad's Save As... (`apps/notepad.c`) is the first real caller:
    `notepad_picker_saved()` now calls `fs_delete()` (reclaim any
    existing file's blocks -- `fs_write_range_begin()`/`step()` extend a
    file but never shrink it, unlike `fs_write()`'s own
    truncate-then-write, so this avoids stale trailing bytes on an
    overwrite with shorter text) then `fs_write_range_begin()` +
    `window_start_write()`, and shows "Saving..." with the Save As...
    button disabled until `notepad_write_complete()` fires.
  - `ui_button` (`apps/ui/ui_button.h`/`.c`, `ui_button_group.c`) gained
    a `disabled` field/`ui_button_set_disabled()` for this -- asked
    first, per the project's own "add as a widget" preference, since a
    disabled/dimmed button is a generic, reusable capability, not a
    Notepad-only concern. `ui_button_group_press()`/`_click()` skip a
    disabled button entirely (same as never being hit).
  - `bring_to_front()`/`close_window()` (`wm.c`) both keep
    `pending_write_win` accurate across window reordering/closing (they
    already shuffle `windows[]` by copying struct contents between fixed
    slots, not by moving identity) -- `close_window()` also refuses to
    close the window a write belongs to, and `wm_exit_requested` is
    deferred (not abandoned) until a pending write reaches a terminal
    result, since its handle owns kernel heap state that only gets freed
    then.
  - A real bug caught by QMP testing, not code review: `apps/notepad.c`
    originally cached the window pointer passed to `window_start_write()`
    once, in `notepad_open()`. That's unsound in this WM -- `struct
    window *` isn't a stable per-window identity here, since
    `bring_to_front()` reorders by copying window *contents* between
    fixed `windows[]` slots rather than moving pointers. After clicking
    a second window in front of Notepad and then Save As..., the stale
    cached pointer silently resolved to the WRONG window by the time the
    write was registered: the write itself still completed correctly on
    disk (verified via `tools/tfs2_writer.py ls`), but
    `on_write_complete()` never fired for the right window, leaving
    Notepad's Save As... button permanently disabled. Fixed by capturing
    the window pointer fresh in `notepad_click()` when Save As... is
    pressed instead (safe because the file picker it opens is modal --
    no other window can be reordered while it's open, per
    `wm_handle_left_click()`'s own dispatch order).
  - Verified via QMP: seeded an 8191-byte file directly onto `disk.img`
    with `tools/tfs2_writer.py` (host-side, no boot needed) to get
    content Notepad could Open instantly rather than needing to type it
    through the emulated keyboard; opened it in Notepad, opened a second
    window (About), clicked Save As... to overwrite the same file, and
    clicked About's taskbar button immediately after confirming Save --
    caught "Saving..." with the Save As... button visibly disabled, and
    separately caught the desktop successfully switching focus to About
    while the write was still in flight (exercising the
    `bring_to_front()`/`pending_write_win` reindexing path live, not
    just by inspection). Confirmed the finished file's size/modified
    timestamp matched via `tools/tfs2_writer.py ls` after each run.
    `tools/preflight.sh` (build + boot smoke test) passed throughout.
    Screenshots in `screenshots/2026-08-12/`.
  - Not done this round (Phase 4, next): generalizing to reads and the
    plain (non-GUI) shell prompt.

- Steppable read API + wired it into `wm_run()`'s poll -- Phase 4 of the
  async-I/O roadmap item (see `docs/roadmap.md`), the read counterpart
  to Phase 2/3 above. `fs.h` gained `fs_read_range_begin()`/
  `fs_read_range_step()`, dispatched through `fs_ops.h`/`vfs.c` to a new
  `tfs.c` backend (`tfs_read_range_begin()`/`_step()`, built from a
  `read_range_one_block()` helper split out of the existing blocking
  `read_range_impl()` the same way Phase 2 split `write_range_impl()`).
  One real difference from the write side: a read can legitimately
  finish having copied fewer bytes than requested (the EOF clamp), so
  `fs_read_range_step()` takes an extra `uint32_t *out_total` out-param
  the write side doesn't need.
  - `wm_run()` (`apps/wm/wm.c`) gained a second WM-global single slot,
    `pending_read`/`pending_read_win` (`wm_internal.h`), polled once per
    frame right after the existing `pending_write` poll -- same
    bounded-per-frame-cost reasoning as Phase 3, a separate slot (not
    shared with `pending_write`) since nothing stops a read and a write
    being in flight for two different windows at once, even though
    nothing exercises that yet. Two new `wm.h` entry points mirroring
    `window_start_write()`/`window_write_pending()`: `window_start_read()`
    and `window_read_pending()`. Completion delivers via a new
    `gui_apps.h` callback, `on_read_complete(win, success, total)` --
    the extra `total` argument (vs. `on_write_complete`'s bare
    `success`) is how an app learns the actual byte count once the
    handle's already freed. `bring_to_front()`/`close_window()` keep
    `pending_read_win` accurate the same way they already did for
    `pending_write_win`.
  - Notepad's Open... (`apps/notepad.c`) is the first real caller:
    `notepad_picker_opened()` now checks the file exists (`fs_exists()`/
    `fs_is_dir()` -- unlike Save As..., Open never creates), clamps the
    read length against `fs_size()` and a new `g_load_buf[SCROLLBACK_CAP]`
    scratch buffer (mirroring Save's `g_save_buf`), then calls
    `fs_read_range_begin()` + `window_start_read()` and shows
    "Loading..." with the Open... button disabled until
    `notepad_read_complete()` fires and hands the loaded bytes to the
    existing `notepad_load_text()`. A 0-byte file short-circuits to an
    instant load (nothing to step).
  - `steptest <mb>`'s existing readback-verification pass (proven
    standalone since Phase 2, `apps/shell_sys.c`) now goes through the
    new `fs_read_range_begin()`/`_step()` instead of blocking
    `fs_read_range()`, reporting both write- and read-step counts --
    reused rather than adding a second diagnostic command, since it
    already had the large multi-block file and readback loop this
    primitive needed to prove itself against.
  - Verified via QMP: `steptest 3` passed (768 write `step()` calls, 768
    read `step()` calls, byte-for-byte verified) at the physical shell
    prompt; in the GUI, seeded a 6000-byte file directly onto `disk.img`
    with `tools/tfs2_writer.py`, opened it in Notepad via Open... (typed
    the path into the picker's filename field, since punctuation needs
    explicit QMP qcodes rather than `send_text()`'s letters/digits-only
    helper), confirmed the loaded text matched byte-for-byte and the
    status line read "Loaded.". Also confirmed the file picker's
    existing "must already exist" validation in Open mode correctly
    refuses a nonexistent filename (pre-existing `file_picker.c`
    behavior, unaffected by this phase). `tools/preflight.sh` (build +
    boot smoke test) passed throughout. Screenshots in
    `screenshots/2026-08-12/`.
  - The plain (non-GUI) shell prompt, deferred above, turned out not to
    need `wm_run()`-style ambient polling at all -- `shell_main()`
    (`apps/shell.c`) is a REPL with no per-frame tick to hang a pending
    op off of. The actual gap is narrower and already documented in
    `keyboard.c`'s own `keyboard_getchar()` comment: a blocking command
    doesn't get `debug_console_poll()`/`vga_cursor_tick()` serviced at
    all until it returns, unlike the shell's idle wait at the prompt or
    the GUI's `wm_run()` loop. Closed by making `cat` (`apps/shell_fs.c`)
    -- the one shell command with no size cap on how much it blocks
    reading, unlike Notepad's Open... above which is capped at
    `SCROLLBACK_CAP` -- use `fs_read_range_begin()`/`fs_read_range_step()`
    in its own loop instead of a single blocking `fs_read()`, servicing
    `debug_console_poll()`/`vga_cursor_tick()` between blocks. `cat` now
    `fs_size()`s the file and `kmalloc()`s a buffer sized to it (a
    reused static pointer, same pattern as `tfs.c`'s own `g_read_buf`
    behind `fs_read()`) rather than relying on `fs_read()`'s internal
    staging buffer, so it can drive the stepped API directly; this
    preserves `fs_read()`'s existing "up to available RAM" ceiling
    rather than shrinking it to some fixed cap. No `hlt`/throttling
    between steps (unlike `wm_run()`'s poll, gated on its own idle wait)
    -- `cat` has real work to do and wants to finish as fast as the disk
    allows.
    - Verified via QMP: seeded a 550,000-byte file and a 14-byte file
      onto `disk.img` with `tools/tfs2_writer.py`; `cat`'d the small
      file (exact match), a nonexistent path (`cat: no such file:` as
      before), and the large one (content correct throughout, shell
      returned cleanly to the prompt afterward -- no hang).
      `tools/preflight.sh` passed throughout. Screenshots in
      `screenshots/2026-08-12/`.
  - Milestone 1's async-I/O item is now fully closed except the
    separately-tracked Terminal async-spawn item below.

- Async/continuously-armed process spawning for the GUI Terminal
  (Milestone 1 phase 4b, docs/roadmap.md) -- `ls` and an explicit
  allowlist of verified-safe `/bin` binaries via `run` now execute from
  inside a Terminal window without freezing the desktop, instead of
  being wholesale-blocked. Same root cause as the async I/O phases above
  (a blocking call inside `wm_run()`'s single event loop), different
  mechanism (process scheduling, not I/O completion):
  - `kernel/core/scheduler.c`/`scheduler.h`: `scheduler_armed` is now set
    once in `scheduler_init()` and never unset, replacing the old
    demo-only flag `scheduler_demo_run()` flipped on/off around its own
    wait loop -- safe because an armed tick over an empty process table
    is a byte-for-byte no-op (find_next_ready() finds nothing, resumes
    exactly what was interrupted), the same invariant the old disarmed
    default relied on. New public API: `scheduler_spawn(path, args)`
    (thin wrapper over the previously-`static` `spawn_from_fs()`,
    extended to build a real argv via a newly-exposed
    `elf_build_argv_on_stack()` instead of always zeroing rdi/rsi) and
    `scheduler_poll(pid, &exit_code)` (`enum sched_poll_result`:
    RUNNING/EXITED/INVALID). A process that exits now becomes
    `SCHED_ZOMBIE` (holding its exit code) instead of being freed
    straight to `SCHED_UNUSED` -- `scheduler_poll()` is the explicit reap
    step, same two-phase shape `wait()`/`waitpid()` has.
  - `kernel/core/elf_run.c`/`elf_run.h`: the static `build_argv_on_stack()`
    helper promoted to a public `elf_build_argv_on_stack()` so
    scheduler.c's `spawn_from_fs()` can reuse the exact same argv layout
    `elf_run_from_fs()`'s legacy blocking path already uses.
  - `apps/wm/wm.c`/`wm.h`/`wm_internal.h`: a third WM-global poll slot,
    `pending_proc`/`pending_proc_win`, mirroring `pending_write`/
    `pending_read`'s exact shape from Milestone 1 phases 3-4 --
    `window_start_process()`/`window_process_pending()`, a new
    `gui_apps.h` callback `on_process_exit(win, exit_code)`,
    `bring_to_front()`/`close_window()` keeping `pending_proc_win`
    accurate the same way, `close_window()` refusing to close a window
    with a process pending, and `wm_exit_requested` deferred while one is
    in flight (a live `vga_sink` would otherwise silently swallow the
    physical shell's own prompt output on return). Unlike the I/O pair,
    this poll doesn't make anything appear on screen -- a spawned
    process's `SYS_WRITE` output already lands in the owning window's
    scrollback via `vga_putc()`'s active sink the instant each syscall
    runs, independent of `wm_run()`'s frame rate; the poll only detects
    completion.
  - `apps/terminal.c`/`terminal.h`: new per-window state
    (`st->running_pid`, blocking all keyboard input while set, same as
    `st->in_editor` does for `edit`/`nano`; `st->saved_sink`). `ls`
    always spawns async now (mirrors `shell_sys.c`'s `cmd_ls_bin()`
    flag/path parsing, via `resolve_editor_path()` for the positional
    argument). `run <name>` does too, but only for names on a new
    explicit allowlist, `RUN_ALLOWED_BINS` -- the opposite of
    `BLOCKED_CMDS`'s blocklist approach: `crash_test`, `exit_test`,
    `file_test`, `hello`, `lspci`, `newsyscalls_test`, `socket_test`,
    `write_bad_test`, `write_test`, each checked against its own
    `userland/*.c` source (not assumed safe by name) for the two real
    hazards -- reading stdin (no stdin routing to a spawned process
    exists yet, so one blocked on it would hang forever, and
    `close_window()`'s new refusal above would strand the whole window)
    or touching the framebuffer/its own window directly. Excluded:
    `echo` (loops on `SYS_READ_KEY` waiting for an Esc that never
    arrives), `gui_test`/`win_test` (framebuffer/own-window takeover),
    `counter_a`/`counter_b` (infinite-loop-by-design `schedtest` demo
    processes). `crash_test` deliberately faults -- verified safe anyway:
    `idt.c`'s fault handler was already scheduler-aware from M16 (its
    `recoverable` branch checks `scheduler_current_pid()`), tearing the
    process down and reporting "RING-3 PROCESS CRASHED" through whatever
    sink is active with exit code -1, exactly like a legacy
    `run crash_test` from the physical shell.
  - Verified via QMP: `schedtest` still spawns/interleaves/exits its two
    counter processes correctly with the scheduler now permanently
    armed, and the physical shell's own legacy `run <name>`/`ls` are
    unaffected (confirmed `run exit_test` -> exit code 42 and `ls`
    listing correctly both still work the old blocking way). In the GUI
    Terminal: `ls` lists a real directory and reports "Process finished.
    Exit code: 0"; `run exit_test` reports exit code 42; `run crash_test`
    reports the crash message and "Exit code: CRASHED" without freezing
    or hanging the window; `run gui_test` (not on the allowlist) and
    `schedtest`/`gui` (still in `BLOCKED_CMDS`) get their expected
    refusals; the window closes normally afterward with no stuck state,
    and exiting the GUI back to the physical shell afterward works
    cleanly (`ls` there still works too). Also incidentally confirmed a
    PRE-EXISTING, unrelated quirk while testing: `run hello` from the
    physical shell page-faults (not the `hlt`-based crash `hello.c`'s own
    comment describes) because `USERLAND_MARKER_ADDR`
    (`userland/userland_contract.h`) happens to collide with
    `ELF_RUN_HEAP_VADDR` (`elf_run.c`), a page `elf_run_from_fs()` never
    actually maps unless the binary calls `sbrk()` -- confirmed
    unaffected by this change (reproduced identically via `run exit_test`
    succeeding normally right after), not investigated further as
    out-of-scope for this item.
    `tools/preflight.sh` (build + boot smoke test) passed throughout.
    Screenshots in `screenshots/2026-08-12/`.

## [0.0.9] - 2026-08-12

### Fixed
- Notepad's filename field (`widget_textfield_draw`, `apps/widgets.c`)
  no longer overflows past its own border when the text is longer than
  the field -- reported from a screenshot showing "notepad.txt" running
  into the Save button. Root cause: `gfx_draw_string()` only clips at
  the screen/window edge, not at an arbitrary width -- it doesn't take
  one -- so the field's `w` parameter was never actually enforced,
  despite a doc comment claiming it was ("clipped the same way every
  other text-drawing call already is"). Fixed by having
  `widget_textfield_draw()` clip to what fits itself, sliding the
  visible window just far enough to keep the cursor in view while the
  field is active (typing past the visible edge now scrolls, like a
  real text input) -- confirmed by typing a name well past
  `FIELD_COLS` and watching the border hold. See `docs/decisions.md`.
- The Start menu's width (`start_menu_w()`, `apps/wm/wm_render.c`) was
  hardcoded to "12 chars, room for the longest app name" -- true when
  written, silently wrong the moment this same change added "Exit to
  shell" (13 chars) below the app list, overflowing past the menu's
  right border with no compiler warning. Now scans both
  `gui_app_registry` and `wm_system_actions` for the actual longest
  label. Caught by screenshot, not by re-reading the code -- see this
  file's own testing conventions.
- Notepad's filename field text sat flush against the field's own
  top/bottom border with zero vertical margin -- reported from a
  screenshot as "white background... overflows to the textbox's
  outline." Not actually drawing outside the field's bounds (`h` and
  the glyph height matched exactly): the bug was that zero margin
  meant the glyph's own opaque background painted directly over the
  border pixels on any row where a character existed, visibly erasing
  the border line under the text. `apps/notepad.c`'s `bh` (the
  field/button row height) was being computed as exactly
  `gfx_char_h()` with no slack -- new `ROW_VPAD` constant adds a few
  pixels of real vertical breathing room via `TOOLBAR_H`'s formula.
  Verified via QMP screenshot at font sizes 8, 18 (default), and 24 --
  border stays intact and visible above/below the text at every size.

### Added
- `ls` migrated off its kernel-space shell built-in onto a real,
  disk-hosted ELF64 binary (`/bin/ls`, `userland/ls.c`) -- the third
  binary to run through `elf_run_from_fs()` (after `lspci`/the ~13
  test binaries), and the first to actually need arguments. User asked
  for this plus GNU-coreutils-flavored `-l`/`-al` and
  `--color=auto`-by-default behavior; scoped via `AskUserQuestion` into
  three real infrastructure additions rather than one-off hacks:
  - **Real argc/argv at ELF entry.** `process_run_ring3()`
    (`kernel/core/process.c`/`.h`) is now a thin argc=0/argv=0 wrapper
    around a new `process_run_ring3_args(pml4_phys, entry, user_rsp,
    argc, argv)`, which seeds RDI/RSI (SysV's first two integer
    arguments) before `iretq` -- any `/bin` binary can now declare
    `void _start(int argc, char **argv)` and receive them like an
    ordinary function call. `elf_run_from_fs()` (`kernel/core/elf_run.c`/
    `.h`) gained an `args` parameter (space-separated, no quoting) and a
    `build_argv_on_stack()` helper that lays argv[0]=path plus each
    `args` token onto the process's one identity-mapped stack page:
    strings written downward from the page's top, the argv pointer
    array below them, `user_rsp` set to the pointer array's own address
    so a subsequent `push` from ring 3 only ever touches fresh, lower,
    previously-unused space. Real bug hit and fixed here: several
    path-taking syscalls (`SYS_LISTDIR` chief among them) validate a
    full `FS_PATH_MAX` (64) byte range starting at whatever pointer
    userland passes, not just up to its NUL -- `argv[0]` landing close
    enough to the stack page's literal top made that validation run off
    the mapped page and fail. Fixed by reserving `FS_PATH_MAX` bytes of
    never-written padding at the page's true top before laying out any
    argv strings.
  - **`SYS_SET_COLOR` syscall** (`kernel/include/syscall_abi.h`/
    `kernel/core/syscall.c`) -- RDI/RSI are foreground/background
    `enum vga_color` values, wraps `vga_set_color()` directly (same
    thing the shell's own `color` command does from kernel space).
    Rejects (-1) an out-of-range value rather than clamping it.
  - **Real per-entry timestamps for `SYS_LISTDIR`** -- `struct dirent`
    gained a `struct rtc_time modified` field (reusing the same struct
    `SYS_GETTIME` already hands to ring-3); the kernel-side handler now
    calls `fs_stat()` once per entry to fill it. No permission-bits or
    owner concept exists in this filesystem at all, so `ls -l` shows
    real type/size/mtime only, no invented placeholder columns.
  - `userland/ls.c` -- no libc, same shape as `lspci.c`. `-a` is
    accepted but a no-op (no dotfile-hiding convention on this
    filesystem, so there's nothing for it to additionally reveal;
    accepted so a habitual `ls -la` doesn't error). Default output
    colors each name via `SYS_SET_COLOR` (directories vs. files),
    unconditionally -- matching `--color=auto`'s look without a flag
    to gate it, per this feature's scope. `-l` shows a type char
    (`d`/`-`), right-aligned size, `MM/DD/YYYY HH:MM:SS` mtime (same
    shape `stat`'s own `print_stat_timestamp()` already uses), then
    the (still-colored) name.
  - `apps/shell_sys.c` gained `cmd_ls_bin()` -- ls's own dedicated
    dispatch entry (same precedent as `cmd_lspci()`), splitting
    `-a`/`-l`/`-al`/`-la` flags from an optional positional directory
    argument and resolving that argument (or defaulting to `cwd`)
    through `resolve_path()` before crossing into ring 3 -- `fs.c`/
    `fs.h` has no cwd concept at all, and neither does `userland/ls.c`,
    so this is the one place a relative path becomes absolute.
    `cmd_run()` also gained its own name/args split (previously only
    ever passed a bare binary name to `elf_run_from_fs()`).
  - `apps/shell_fs.c`'s old `cmd_ls()`/`list_cb()` (direct `fs_list()`
    call from kernel space) are deleted, per the user's explicit
    request -- `ls` has exactly one implementation now, not two.
  - **Real, documented regression, not an oversight:** `ls` joined
    `apps/terminal.c`'s `BLOCKED_CMDS` (GUI Terminal) alongside `run`.
    Every path through `elf_run_from_fs()`/`process_run_ring3_args()`
    is synchronous and blocking -- it would freeze the Terminal
    window's whole event loop until the process exits, same hazard
    `run` was already blocked for. User was shown the real cost of
    building async/continuously-armed spawn support this session (new
    public spawn API, scheduler changes, `wm_run()` restructuring,
    new per-window process-running state) and explicitly chose to ship
    `ls` now and track that infrastructure on `docs/roadmap.md`
    instead of building it this round. Directory listing from inside
    the GUI Terminal is unavailable until that lands.
  - `Makefile`: `LS_ELF`/build rule pair mirroring `lspci`, `SEED_BINARIES`
    entry, added to `all`/`seed`/`iso`/`clean`'s prerequisite lists.
  - Verified via QMP against a clean `make clean && make all && make
    iso`: `ls`, `ls -l`, `ls -a`, `ls -al /bin` all produce correct,
    colored output with real sizes/timestamps from the physical shell;
    `run lspci` (the zero-arg `process_run_ring3()` path) still works
    unchanged, confirming the argc/argv plumbing didn't regress
    existing callers; `ls` inside the GUI Terminal shows the expected
    blocked-command message instead of hanging the window. (Testing
    aside, unrelated to this feature: the QEMU test VM's disk had a
    Swedish keyboard layout persisted from earlier keyboard-layout
    testing this session, which briefly looked like a `-`/`=` key
    corruption bug before `keyboard us` explained it -- included here
    only so a future session doesn't rediscover the same red herring.)
- The remaining ~13 GRUB-module-loaded ELF64 test binaries
  (`elf_test`/`hello.elf`, `syscall_test`, `write_test`,
  `write_bad_test`, `ptr_test`/(folded away, see below), `gui_test`,
  `echo_test`, `win_test`, `file_test`, `newsyscalls_test`,
  `crash_test`, `socket_test`, plus `counter_a`/`counter_b`) moved off
  GRUB modules onto build-time-seeded `/bin` entries, the same
  mechanism `lspci` got in the entry below -- prompted by the user
  asking what happens if these ELF64 binaries are run under real Linux
  (answer: they'd crash or misbehave -- toy-os's syscall convention
  rides `int $0x80`, which 64-bit Linux only recognizes as the legacy
  32-bit compat entry point, so the kernel would dispatch through the
  wrong syscall table with the wrong register convention; the ELF
  itself is otherwise structurally valid and loadable). What changed:
  - `Makefile`'s `SEED_BINARIES` list now has 14 `elf:/bin-name` pairs
    (`lspci` + the 13 above); `seed:` stages all of them into
    `seed/sync/bin/` before one `tfs2_writer.py sync` call. `iso:` no
    longer copies any per-binary `.elf` file into `iso/boot/` --
    `grub.cfg` is down to just `multiboot2 /boot/kernel.bin` + `boot`,
    no `module2` lines at all.
  - `kernel/core/elf_run.c`'s `elf_run_from_fs()` -- the one generic
    loader `run <name>` already used for `lspci` -- is now what every
    `/bin` binary runs through. It now also calls
    `syscall_reset_heap(as, ELF_RUN_HEAP_VADDR)` unconditionally before
    running (cheap bookkeeping, arms `SYS_SBRK`) -- found by inspecting
    `echo_test.c`, which called this itself before its old
    dedicated-command loader ran it; without this, migrating
    `echo_test` to the generic path would have silently broken its
    heap-based `sbrk()` use.
  - `kernel/core/scheduler.c` gained `spawn_from_fs(const char *path)`,
    replacing `spawn_from_module(int module_index)` outright (its only
    caller, `scheduler_demo_run()` for `schedtest`, is the only one
    that needs two processes running concurrently under the real
    preemptive scheduler -- a one-shot `run <name>` can't do that, so
    this couldn't just fold into `elf_run_from_fs()` the way the
    others did). Sources ELF bytes via `fs_read()` instead of
    `multiboot_get_module()`, same no-copy-needed reasoning
    `elf_run_from_fs()` already used. `schedtest` now spawns
    `/bin/counter_a` + `/bin/counter_b`.
  - The 11 now-redundant kernel-side test harnesses and their headers
    (`elf_test`, `syscall_test`, `write_test`, `ptr_test`, `gui_test`,
    `echo_test`, `win_test`, `file_test`, `newsyscalls_test`,
    `crash_test`, `socket_test` -- `kernel/core/*.c` + `kernel/include/
    *.h` pairs) are deleted, along with their dedicated shell commands
    in `apps/shell.c` (`elftest`, `syscalltest`, `writetest`,
    `ptrtest`, `guitest`, `echotest`, `wintest`, `filetest`,
    `newsyscalltest`, `crashtest`, `sockettest`) -- each is a real
    `/bin` binary now, run via `run <name>` (e.g. `run write_test`).
    `ring3test` (no ELF file at all, tests raw paging/GDT/ring-3
    isolation) and `schedtest` are the two exceptions, kept as
    dedicated commands since neither maps onto the generic
    `elf_run_from_fs()` path. `elftest`/`hello.elf` specifically tested
    a raw manual-`iretq` ring-3 entry, distinct from the recoverable
    `process_run_ring3()` path every other binary already used -- user
    chose to fold it into the generic `run hello` path anyway, trading
    that one narrow bit of coverage for one less special case; verified
    the fault (a deliberate privileged instruction from ring 3) is
    still caught and reported as `Exit code: CRASHED`, not a kernel
    crash.
  - `kernel/core/pmm.c`'s module-reservation loop and Multiboot-info
    reservation are unchanged in code but now dormant (zero modules
    exist) -- comments updated to say so and to point at
    `spawn_from_fs()` instead of the old `multiboot_get_module()`-based
    spawn path they used to reference.
  - `apps/terminal.c`'s `run` command stays blocked wholesale inside
    the GUI Terminal window (unchanged from before this migration) --
    several of the newly-independent `/bin` binaries (`gui_test`,
    `win_test`, `echo_test`) fall into the same "takes over the
    physical framebuffer" / "blocks forever without yielding" hazards
    the old dedicated commands were blocked for, and a per-target
    allowlist couldn't be verified safe in the GUI context in the time
    available (see `docs/decisions.md`). Every `/bin` binary can still
    be run from the physical shell.
  - `tools/gui_flow.py`'s `ITEM_H` constant (Start-menu row height) was
    found to be stale -- `32` when the real value is `24`
    (`gfx_char_h() + 6` at the default font size) -- discovered because
    it made `open_app()` misclick past the intended row (clicking
    "Terminal" was actually landing on "Calculator"). Fixed and
    reverified live (`open_app("Terminal")` now opens Terminal). This
    was a pre-existing bug in the tool, unrelated to this migration,
    caught only because this migration's testing leaned on it.
  - Verified via QMP: all 14 `/bin` binaries present after a clean
    `make clean && make all && make iso`; `run <name>` for each from
    the physical shell (`hello` crash-recovers, `exit_test` returns 42,
    `write_test`/`write_bad_test` exercise real syscalls, `crash_test`
    crash-recovers, `file_test`/`newsyscalls_test`/`socket_test`
    self-check, `counter_a` runs solo); `schedtest` produces genuinely
    interleaved concurrent output; Terminal (GUI) still blocks `run`
    wholesale while ordinary commands (`ls`, etc.) work normally inside
    it; `dmesg` trail is clean, no bootstrap-install lines, everything
    routed through the one `elf_run: calling process_run_ring3() for
    /bin/...` log line. `boot_smoke_test.py` passes.
- `/bin/lspci` is now seeded onto `disk.img` at BUILD time (Makefile's
  new `seed` target, wired into `iso:`) instead of installed at BOOT
  time -- the follow-through on `tools/tfs2_writer.py` now that it can
  do it. What changed:
  - `tools/tfs2_writer.py` gained a `format` subcommand -- initializes
    a blank/foreign image as an empty TFS2 v2 filesystem, mirroring
    `tfs.c`'s `tfs_init()` format path byte-for-byte (superblock,
    cleared journal header, a bitmap with the reserved metadata region
    pre-marked allocated, `FS_MAX_FILES` blank table records written
    through the normal journal stage-commit-apply-clear sequence).
    `write`/`sync` now auto-format a blank image first (a no-op if it's
    already a valid TFS2 image), so a completely fresh, untouched
    `disk.img` can be seeded in a single call -- no toy-os boot needed
    in between anymore.
  - The Makefile's new `seed` target (`$(DISK_IMG) $(LSPCI_ELF)`
    prerequisites, `.PHONY`) stages `$(LSPCI_ELF)` into
    `seed/sync/bin/lspci` and runs `tfs2_writer.py sync $(DISK_IMG)
    seed`. Wired as an `iso:` prerequisite, so `make iso` alone now
    produces a `disk.img` with `/bin/lspci` already on it -- `sync`'s
    content-hash compare makes every call after the first a fast no-op
    unless `lspci.elf` actually changed, so this stays cheap on every
    build, not just the first. `seed/sync/` is `.gitignore`d (a
    build-generated staging copy, not a source file).
  - `kernel/core/kernel.c`'s `install_bin_binaries()`/`BIN_BOOTSTRAP`
    table and its GRUB-module-based install (added when disk-hosted
    ELF binaries first shipped, see this file's earlier `[Unreleased]`
    entry) are removed -- redundant now that the build-time seed step
    covers the same job without needing a boot cycle or a kernel
    rebuild per binary. `grub.cfg`'s `module2 /boot/lspci.elf lspci`
    line and the `iso:` recipe's `cp $(LSPCI_ELF) iso/boot/lspci.elf`
    step are removed too -- `lspci.elf` is still built (needed to seed
    the disk image) but no longer shipped as a GRUB module.
  - Verified in the cloud sandbox: `make clean && make all && make
    iso` from scratch produces a `disk.img` with `/bin/lspci` on it
    without ever booting toy-os (checked directly with `tfs2_writer.py
    ls`); `boot_smoke_test.py` still passes; booted the result and
    confirmed `run lspci` works and `dmesg` shows `fs: loaded
    persistent filesystem from disk` (not `fs: formatted a fresh...`,
    since the host tool formatted it first) with no `bin: installed
    ...` line at all (that log line's code is gone). See
    `screenshots/2026-08-11/lspci_seeded_at_build_time_no_bootstrap.png`.
- `tools/tfs2_writer.py`: a host-side TFS2 v2 read/write tool -- the
  "option 2" deferred from the real-disk-hosted-ELF-binaries work
  below, now built. Lets a file get onto `disk.img` (or be read back
  out) without booting toy-os, a kernel rebuild, or the
  `BIN_BOOTSTRAP`/GRUB-module bootstrap-install path that approach
  still relies on. Four subcommands, one script (`write`/`read`/`ls`/
  `sync`):
  - `write <disk.img> <tfs-path> <local-file>` and `read <disk.img>
    <tfs-path>` -- single-file in/out. `write` refuses to overwrite an
    existing path without `--force`; both support `--dry-run`.
  - `ls <disk.img> [tfs-path]` -- lists a directory's direct children
    with full detail (type, size, created/modified), same semantics as
    the kernel's own `fs_list()`.
  - `sync <disk.img> <seed-dir> [--dest /]` -- mirrors a whole seed
    directory tree in at once, split into two policy subtrees:
    `<seed-dir>/once/...` (copy-once -- written if missing, never
    touched again once present; for config files a user might edit
    after first boot) and `<seed-dir>/sync/...` (content-hash-synced --
    written if missing, rewritten only if the local file's SHA-256
    differs from what's on the image, otherwise left alone; for
    binaries/assets rebuilt between runs). Missing parent directories
    are created automatically (mirrors a chain of `fs_mkdir()` calls).
  - Change detection deliberately uses a content hash, not local vs.
    on-disk mtime comparison -- TFS2 timestamps are toy-os's own RTC
    wall-clock time (see `fs.h`'s `fs_stat()` comment), not something
    comparable to the host machine's clock without assuming a
    particular skew; hashing sidesteps that entirely.
  - Implemented directly against `docs/tfs2-spec.md` (superblock/
    journal checks, table records, block addressing, the free-block
    bitmap, the journal's stage-commit-apply-clear write sequence) --
    the `read`/`ls` code paths are close to the spec's own reference
    reader, extended with the write-side mirror of `tfs.c`'s
    `alloc_block()`/`free_tree()`/`persist_record()`.
  - Scope: writes only allocate direct + single-indirect blocks (12 +
    1024 blocks, ~4.03 MB max per file) -- plenty for ELF binaries and
    config/text files, everything this was built for. A file needing
    double/triple-indirect refuses cleanly with a clear error rather
    than silently truncating; extending write support to those is
    listed in `docs/roadmap.md`'s backlog if a real need for
    multi-megabyte seeded files comes up.
  - Verified in the cloud sandbox: wrote/overwrote/read back a config
    file and `lspci.elf` via `write`/`read`/`ls`, ran `sync` against a
    seed directory (confirmed `once/` skips an already-present file
    even after its local content changed, `sync/` rewrites only when
    content actually differs, both create missing parent directories),
    then booted the resulting `disk.img` for real and confirmed the
    shell's own `ls`/`cat`/`run` see exactly what the host tool wrote
    -- including a round-trip check that `sync`'s content-hash compare
    correctly recognized `/bin/lspci` as already matching what the
    kernel's own `install_bin_binaries()` had written during that same
    boot, a real interop check between the two write paths landing
    byte-identical records. See
    `screenshots/2026-08-11/tfs2_writer_host_write_verified_in_shell.png`.
- Real disk-hosted ELF64 binaries: `lspci` now exists as a genuine
  ring-3 process, loaded from `/bin/lspci` on the persistent filesystem
  and run via `run lspci` -- not a kernel-space shell built-in
  (`cmd_lspci()`/`pci_device_at()`) and not a GRUB-module test harness
  either. This is `docs/roadmap.md`'s real-disk-hosted-ELF-binaries
  item, planned in an earlier session and re-scoped this session before
  building: re-checking that old plan against the current codebase
  found its two stated blockers were already gone or smaller than
  described (see `docs/decisions.md`'s new entry), so both halves
  shipped together instead of as separate builds.
  - New syscalls `SYS_PCI_COUNT`/`SYS_PCI_INFO` (`syscall_abi.h`,
    `kernel/core/syscall.c`) -- the first syscalls added specifically
    so a real userland ELF can do something other than file I/O.
    `SYS_PCI_INFO` hands back a `struct pci_device` (`pci.h`) by value,
    the same struct `pci_device_at()` already returns kernel-side --
    reused directly rather than declaring a syscall-private copy, the
    same precedent `SYS_GETTIME` already set for `timer.h`'s
    `struct rtc_time`.
  - `userland/lspci.c`: a real freestanding, no-libc ELF64 program
    (same shape as `newsyscalls_test.c`) that calls the two syscalls
    above and prints the same `bus:device.function vendor:device class
    name` format the `lspci` shell command already uses. Carries its
    own small local copy of `pci_class_name()`'s class/subclass -> name
    table, since that function lives in kernel/drivers/pci.c and can't
    be called from ring 3 -- only linked-in code and syscalls are
    reachable from there.
  - `kernel/core/elf_run.c`/`elf_run.h`: `elf_run_from_fs(path)`, the
    disk-hosted counterpart to `file_test.c`/`newsyscalls_test.c`'s
    GRUB-module-sourced `elf_load()` + `process_run_ring3()` pattern,
    just with `fs_read()` standing in for `multiboot_get_module()`.
    Turned out to need no separate scratch-buffer copy of the ELF
    blob -- `fs_read()`'s `kmalloc()`'d buffer is already in the same
    identity-mapped low-4GiB physical range a GRUB module lives in
    (see `heap.c`'s own top comment), so `elf_load()` takes its address
    directly with just a cast. Exposed through `kapi.h` like every
    other `*_test.h`-style single-entry-point header.
  - The shell's `run <name>` (`apps/shell_sys.c`'s `cmd_run()`) now
    falls through to `/bin/<name>` + `elf_run_from_fs()` when
    `app_run()` (the kernel-space `shell`/`gui` registry, `apps/apps.c`
    -- unrelated to this, still only two entries) doesn't recognize the
    name, instead of immediately reporting "no such app."
  - `kernel_main()` gains `install_bin_binaries()`
    (`kernel/core/kernel.c`): a one-time boot bootstrap that copies
    `lspci.elf`'s GRUB module bytes into `/bin/lspci` via
    `fs_write_range()` the first time it boots against a given disk
    image (a no-op on every later boot once the file exists) -- there's
    no in-guest compiler and no host-side TFS2 writer tool yet (see
    `docs/roadmap.md`'s new backlog entry -- deliberately deferred,
    the user's own call), so this is how a binary's bytes get onto
    `/bin` at all today. Uses `fs_write_range()`, not `fs_write()` --
    an ELF's bytes contain embedded `0x00` bytes, and `fs_write()`
    treats its `data` argument as a NUL-terminated C string.
  - `lspci.elf` added as `grub.cfg`'s 14th `module2` line (index 13)
    and to the `Makefile`'s userland-ELF build/`iso`/`clean` targets --
    the same manual per-binary wiring every existing `userland/*.elf`
    already needs (this directory isn't wildcarded, unlike
    `kernel/core/*.c`, which is why `elf_run.c` above needed no
    `Makefile` changes at all).
  - Corrected three stale comments that cited `fs.h`'s `FS_DATA_MAX`
    (2048) as a real per-file ceiling (`kernel/core/etc_config.c`,
    `apps/editor.c`, `apps/editor.h`) -- found while re-verifying the
    old ELF-binaries plan against the current filesystem. TFS2 v2's
    block-addressed rework removed that ceiling as a side effect, not
    as part of this change; `FS_DATA_MAX` itself is now vestigial
    (kept defined, `fs.h`'s comment says so) since nothing in `tfs.c`
    references it anymore.
  Verified in QEMU via QMP: `run lspci` from the physical shell prints
  the exact same six-device list `dmesg`'s own `pci:` lines show,
  ending "Process finished. Exit code: 0"; `dmesg` afterward shows
  `elf_run: calling process_run_ring3() for /bin/lspci` and
  `syscall: exit() called by ring-3 process`; `run bogus` still
  correctly reports "no such app: bogus"; a second boot against the
  same `disk.img` does NOT re-print the `bin: installed` line (the
  exists-check works). Screenshots:
  `screenshots/2026-08-11/lspci_bin_first_real_disk_hosted_elf.png`,
  `screenshots/2026-08-11/lspci_run_dmesg_trail_and_bogus_app.png`.
- dmesg (`klog_write()`) coverage extended to six areas that had zero
  boot/probe-time logging before this: PCI enumeration
  (`kernel/drivers/pci.c`), the keyboard driver
  (`kernel/drivers/keyboard.c`), the mouse driver
  (`kernel/drivers/mouse.c`), the VGA/framebuffer console driver
  (`kernel/drivers/vga.c`), the CMOS/RTC hardware clock
  (`kernel/core/kernel.c`), and the window manager
  (`apps/wm/wm.c`) -- found via a line-count/coverage audit that also
  flagged PCI's own `lspci` shell command as an existing precedent for
  the log line format below. Also added `klog_write_dec()`/
  `klog_write_hex()` (`kernel/include/klog.h`/`kernel/core/klog.c`),
  small klog-routed mirrors of `vga_write_dec()`/`vga_write_hex()`
  (`vga.h`) -- klog messages needing a numeric value had no formatting
  helper of their own before this, since every existing `klog_write()`
  call site only ever needed a plain string.
  - `pci_init()` now logs one line per discovered device (bus:device.
    function, vendor:device, class name -- the exact
    `bus:device.function vendor:device class` shape `cmd_lspci()`
    already prints to the console, reusing its own local
    fixed-width-hex helper rather than `klog_write_hex()`'s
    leading-zero-trimmed format, which wouldn't keep columns aligned)
    plus a final device-count summary.
  - `mouse_init()` logs whether the connected PS/2 mouse answered the
    IntelliMouse "magic knock" (wheel support, 4-byte packets) or not
    (plain 3-byte packets) -- the one thing that handshake actually
    determines and previously went nowhere but a local variable.
  - `vga_init()` logs which console backend it ended up on: linear
    framebuffer (with the resolution) or the legacy text-mode fallback
    -- runs early enough to log safely (`serial_init()` already ran in
    `kernel_main()` by the time `vga_init()` is called).
  - `keyboard_set_layout()` logs the layout it was just set to --
    covers both call sites for free (boot-time `keyboard_config_init()`
    applying a persisted layout, and the `keyboard <us|se>` shell
    command switching it live) without needing a log line at each
    caller.
  - `kernel_main()` gains a one-shot CMOS/RTC boot-time readout, logged
    once right after the persisted config (timezone/font/keyboard) is
    loaded -- deliberately NOT logged from `rtc_read()` itself
    (`kernel/core/timer.c`), which the taskbar clock/`tz.c` call
    continuously on every redraw; logging there would flood the ring
    buffer. Uses raw `rtc_read()`, not `tz.h`'s `rtc_read_local()` --
    unadjusted UTC hardware time, matching what a real kernel's own RTC
    probe logs before any timezone config is even in the picture.
  - `apps/wm/wm.c` logs entering/exiting GUI mode (with resolution),
    each app window opening/closing (by name), and the no-framebuffer
    failure path -- notable window-manager lifecycle events that
    previously left no trace in `dmesg` at all. Deliberately does NOT
    log the single-instance re-focus path (clicking an already-open
    app's Start-menu entry again) -- that happens on every such click,
    not just once, and isn't a lifecycle event worth the ring-buffer
    space.
  Verified in QEMU via QMP: `dmesg` after a fresh boot shows all six
  new boot-time lines in order (PCI device list + count, VGA mode,
  RTC reading) with correct ring-buffer timestamps; entering GUI mode,
  opening and closing a window, and exiting back to the shell each
  produced the expected `wm:`/`mouse:` lines in real time; running
  `keyboard se` from the shell produced the `keyboard:` line
  immediately. Screenshots:
  `screenshots/2026-08-11/dmesg_new_pci_vga_rtc_wm_mouse_lines.png`,
  `screenshots/2026-08-11/dmesg_keyboard_layout_switch_line.png`.
- Documentation audit: `README.md`, `apps/README.md`, and
  `docs/arch-portability.md` had all drifted out of date after the
  recent kernel-heap/JSON, `apps/ui/` widget migration, and desktop/
  context-menu work -- a research pass found and fixed the stale
  bits. `README.md`: "four apps" -> five (Task Manager added), the
  project-layout tree's `apps/widgets.h`/`.c` and `apps/wm/` file
  list updated, a new bullet for the kernel heap + JSON library.
  `apps/README.md`: the entire "Shared widgets (widgets.h/widgets.c)"
  section rewritten to describe `apps/ui/`'s one-file-per-widget
  layout and its `ui.h` umbrella include; the window-manager file
  list extended with `desktop.c`/`context_menu.c`/`start_menu.c`; a
  Task Manager entry added to the app list. `docs/arch-portability.md`:
  refreshed line counts (`tfs.c`, `apps/wm/*`, `calc_engine.c`,
  `ata.c`, `kapi.h`, the repo-wide C total), replaced the
  `apps/widgets.h`/`.c` reference with `apps/ui/*`, and added
  `heap.c`/`heap.h` and `json.c`/`json.h` to the architecture-neutral
  inventory. `docs/tfs2-spec.md` was fully rewritten from scratch --
  it still described the pre-rework TFS2 v1 format (inline 2048-byte
  file data, no block allocator); it now documents the current v2
  block-addressed layout (superblock version 2, the 12-direct +
  single/double/triple-indirect pointer scheme, the free-block
  bitmap region, the 9 GiB sparse `disk.img`) including a rewritten
  Python reference reader that can walk the indirect-pointer chain to
  dump a file's actual content, not just list entries. `CLAUDE.md`
  was also brought current in the same pass (the `apps/widgets.h`
  bullet, a header-dependency example, the `WM_C`/`UI_C` Makefile
  wildcard note, and a `## tools/` listing for `preflight.sh`/
  `deliver.py`/`gui_flow.py`/`screenshot_diff.py` that was missing
  entirely). The `toy-os-feature-workflow` skill's own `SKILL.md` was
  updated to match (the `apps/widgets.h` widget reference, and steps
  4/6 now mention the four tools above) and delivered as an updated
  `.skill` file for the user to re-save, since skills can't be edited
  directly from this session. No code changed in this pass.
- Start menu (`apps/wm/wm_render.c`/`wm_input.c`/`wm.c`) gains real
  graphical feedback: hovering a row highlights it (recomputed fresh
  from the mouse position every frame -- `wm.c`'s main loop now forces
  a full redraw on mouse movement while the menu's open specifically
  so this stays live, not just on the next unrelated repaint), and
  clicking a row shows a distinct warm-colored flash for ~100ms
  (`START_MENU_FLASH_TICKS`, `wm_input.c`) before the menu actually
  closes -- previously a click ran the row's action and closed the
  menu in the very same frame, with no visible confirmation the click
  landed. The row's action still runs immediately on click, same as
  before; only closing the menu is deferred. New `start_menu_flash_index`/
  `start_menu_flash_until` state (`wm.c`) and `wm_update_start_menu_flash()`
  (`wm_input.c`, called every tick from `wm_run()`'s loop) drive the
  deferred close. Verified via QMP: hover tracks the mouse live across
  rows, a click shows the gold flash on the clicked row, and the menu
  closes cleanly afterward.
- Calculator (`apps/calculator.c`) gains a small expression-so-far
  line above the main display, e.g. "12 +" while an operator is
  pending -- built entirely from state `calc_engine.h`'s
  `struct calc_state` already tracked (`accumulator`/`pending_op`),
  just not shown anywhere before this. Blank when nothing's pending
  (right after `calc_reset()` or right after '='). Needed one small
  `calc_engine.h`/`.c` addition: `render_scaled()` (internal formatting
  helper) is now the public `calc_format_scaled()`, so `calculator.c`
  can format the accumulator itself instead of duplicating that logic.
  Calculator's default window height grew slightly to fit the new line
  (`EXPR_H`/`TOP_H`, `calculator.c`) -- it's a fixed, non-resizable
  window, so this only affects the size a freshly-opened Calculator
  starts at. Verified via QMP: "2 +" appears after `2` then `+`, and
  clears correctly once `=` computes the result.
- New `struct ui_button`/`ui_button_group` (`apps/ui_button.c`/`.h`,
  `apps/ui_button_group.c`/`.h`) -- a self-contained button *object*
  that owns its own geometry, label, colors, and `pressed` state,
  modeled on Brutal OS's `libs/brutal-ui/button.c`/`.h` (pointed at
  directly this session) but sized down for toy-os's immediate-mode,
  no-allocator GUI: no generic view base class, no view-tree/mounting,
  no layout DSL, no hover tracking (the WM doesn't dispatch
  mouse-enter/leave, only press/click/release). `ui_button_group`
  handles the part every multi-button app used to hand-roll itself --
  hit-testing a set of buttons and tracking which one is currently
  down -- over a caller-owned array, so it works for a grid, a row, or
  a lone pair without knowing anything about layout itself.
  `apps/calculator.c` is the first real caller: its old
  `int g_pressed_index` + private `button_at()` hit-test loop are gone,
  replaced by a `struct ui_button_group` driving the same `on_press`/
  `on_click`/`on_release` behavior through generic code. Notepad's
  Save/Load buttons deliberately weren't migrated in this same change
  -- see `docs/decisions.md`. Verified via QMP: press-and-hold shows
  the pressed border, releasing in place springs it back, dragging
  onto a second button re-presses correctly with no phantom input, and
  dragging off entirely un-presses cleanly -- same three cases the
  original `on_press`/`on_release` mechanism was verified against,
  now passing through the new object instead of calculator.c's own
  bookkeeping.
- Start menu gains a second group of items below the app list:
  currently just "Exit to shell", separated by a 1px divider. It
  replaces the old hardcoded "Esc always exits the window manager"
  shortcut in `wm_run()` (`apps/wm/wm.c`) -- discoverable now instead
  of a hidden key, and it frees Esc up for a future modal-cancel use
  (a confirm dialog, say) instead of double-booking it as "exit
  everything, no matter what's open or focused." These aren't real
  `gui_app_registry` entries (they don't open a window) -- a new
  `wm_system_actions[]` array (`struct start_action { label,
  on_select }`) holds them, rendered and hit-tested by
  `gui_app_registry_count + wm_system_action_count` total menu rows
  instead of just the app count. A "Shutdown" item (with a Yes/No
  confirm dialog) was also requested this session but the actual
  power-off mechanism was deliberately deferred -- see
  `docs/roadmap.md`.
- The text cursor in Notepad/Terminal (`widget_scrollback_draw()`'s
  cursor, `apps/widgets.c`) is now a thin `CURSOR_BAR_W`-px vertical
  bar instead of a solid block covering the whole character cell --
  requested as "a bit more modern," and now matches
  `widget_textfield_draw()`'s caret (same width, same shared
  `CURSOR_BAR_W` constant in `apps/widgets.h`) instead of the two
  looking like two different cursor styles in the same app.

- The mouse cursor (`draw_cursor_normal()`, `apps/wm/wm_render.c`) is
  now a proper anti-aliased arrow sprite instead of the old hard-edged
  blocky staircase shape -- requested as "a bit more modern," same as
  this round's text-cursor change. Built the same way the font
  renderer already does anti-aliasing (`font_ttf.c`/`gfx_draw_char()`):
  a hand-designed arrow polygon rendered at 16x supersample via
  Python/PIL, downsampled, with a second dilate/erode pass to derive a
  separate outline-ring alpha mask -- both baked as literal 13x19 byte
  arrays pasted into the C source (not a new build-time tool; a one-off
  asset this small isn't worth a `tools/gen_*.py` script). Needed a new
  public `gfx_blend_pixel(x, y, color, alpha)` (`kernel/drivers/gfx.c`/
  `gfx.h`) -- the same per-channel blend math `gfx_draw_char()` already
  used internally, just exposed for a non-glyph caller. `CURSOR_BOX_SIZE`
  grew from 20 to 22px to fully cover the taller 19px sprite. Verified
  via QMP screenshot at zoom.
- Calculator's on-screen buttons now show real press/release visual
  feedback (a 2px inset border + 1px label nudge while held) --
  Calculator already used the shared `widget_button()` (`apps/widgets.c`/
  `.h`), so the actual gap was that nothing anywhere gave visible
  feedback for a held button. Built as a general window-manager
  mechanism rather than a Calculator-only hack, since any app with
  buttons will eventually want this: two new optional `gui_apps.h`
  callbacks, `on_press(win, cx, cy)` (fired every tick the button's
  held, including the initial press, returning 1 only when which
  button is "hot" actually changed) and `on_release(win)`; a new
  `content_pressed` index in the window manager's state
  (`apps/wm/wm_internal.h`/`wm.c`), driven each tick in
  `wm_update_drag_resize()` (`apps/wm/wm_input.c`) alongside the
  existing `content_dragging` mechanism it deliberately mirrors.
  Dragging off a held button before releasing correctly un-presses it
  without triggering the button's action, same as a real OS button --
  verified via QMP: press-and-hold shows the inset border, release
  springs it back, drag-off-then-release shows no phantom extra input.
  `widget_button()` gained a `pressed` parameter (all 7 existing call
  sites -- notepad.c's 2, wm_render.c's 5 chrome buttons -- pass `0`,
  unaffected).
- Title-bar minimize/maximize/close buttons (`apps/wm/wm_render.c`/
  `wm_input.c`/`wm.c`) gain real hover and press feedback, and close
  moved from act-on-click to Windows/KDE-style delayed commit:
  mouse-down on any of the three now only ARMS it (a lighter tint plus
  the same inset `pressed` look `widget_button()` already gives
  Calculator's buttons), and the actual minimize/maximize/close only
  fires on mouse-up while the cursor's still over that same button --
  dragging off before releasing cancels with no effect at all, not
  even a restack, same as any real desktop's title-bar buttons.
  Hovering (mouse not held) shows a lighter tint too, recomputed live
  from the cursor position every tick, same "derive live, don't
  persist a stale answer" approach as the Start menu's own hover. New
  `title_btn_armed_win`/`title_btn_armed_kind`/`title_btn_pressed_active`
  and `title_hover_win`/`title_hover_kind` state (`wm.c`), driven each
  tick by new `wm_update_title_btn_press()`/`wm_update_title_hover()`
  (`wm_input.c`, mirroring `content_pressed`'s per-tick handling in
  `wm_update_drag_resize()`). Verified via QMP: hovering close/minimize
  shows the tint, pressing close shows the inset pressed look,
  dragging off before releasing drops back to plain (window stays
  open), and a clean press-release on close/minimize/maximize each
  commit correctly.
- New `apps/ui/` directory holds toy-os's small retained-widget-object
  library -- `ui_button.c`/`.h` and `ui_button_group.c`/`.h` moved here
  from `apps/` (same content, no behavior change), plus a new
  `ui_textbox.c`/`.h` and an umbrella `ui.h` that `#include`s all three
  so a GUI app writes one `#include "ui/ui.h"` instead of hunting down
  a header per widget -- picks up future widgets automatically too.
  `struct ui_textbox` wraps `widgets.h`'s `struct text_field` the same
  way `ui_button` wraps `widget_button()`: owns its own geometry
  (repositioned live via `ui_textbox_set_geometry()`, same contract as
  `ui_button_set_geometry()`) around the existing
  `widget_textfield_*()` calls, which still do the actual drawing/
  editing. First real caller: Notepad's filename field, migrated off a
  raw `struct text_field` -- `st->filename` is now a `struct
  ui_textbox`, its text/cursor/active state reached through
  `st->filename.field.*`. `Makefile` gained a `UI_C`/`UI_OBJ` wildcard
  and pattern rule (same shape as `WM_C`/`WM_OBJ` for `apps/wm/`).
  Verified via QMP: Notepad's field still activates on click, accepts
  typing/backspace, and Save/Load still read/write the typed filename
  correctly -- no regression from the widgets.h migration.
- New kernel-space heap allocator: `kmalloc()`/`kzalloc()`/`kfree()`
  (`kernel/core/heap.c`/`kernel/include/heap.h`), first-fit over a
  doubly-linked, address-ordered free list with real pointer-adjacency
  coalescing on free (not list-order adjacency -- separate
  `pmm_alloc_contiguous()` growth regions aren't guaranteed physically
  adjacent to each other). Built directly on `pmm.c`'s physical frame
  allocator: since `boot.asm` already identity-maps the whole low 4GiB
  for kernel/supervisor use, any frame `pmm_alloc_contiguous()` returns
  is immediately a valid kernel pointer with no separate page-table
  mapping step needed. Grows in >=64KiB chunks (`HEAP_MIN_GROW_PAGES`)
  as needed; not exposed to ring-3 (separate from the existing
  `SYS_SBRK` per-process user heap) and not interrupt-safe/reentrant
  (matches the kernel's existing single-threaded assumptions -- no ISR
  calls into it). `heap_init()` + `heap_selftest()` run from
  `kernel_main()` right after `pmm_init()`; the self-test allocates
  three different-sized blocks, checks `kzalloc()` actually zeroes,
  frees in an order that exercises both-direction coalescing, then
  re-allocates to confirm the coalesced space is reusable. This is the
  first real kernel-space allocator toy-os has had -- built specifically
  to unblock multi-instance GUI apps (see below). Verified via
  `tools/boot_smoke_test.py`: "toy-os: kernel heap initialized" and
  "heap: selftest passed" both appear in the right spot in the boot
  sequence.
- GUI apps can now open more than one window at once. `gui_apps.h`'s
  `struct gui_app` gained two fields: `multi_instance` (default 0 =
  old behavior, reopening from the Start menu just focuses/restores
  the one window that can ever exist; 1 = every open always creates a
  brand-new window, bounded only by `MAX_WINDOWS`) and `on_close`
  (optional, called once right before a window's slot is removed from
  `windows[]`, with `window_get_state()` still valid inside it -- lets
  a multi-instance app `kfree()` its per-window state).
  `apps/wm/wm.c`'s `open_app()`/`close_window()` now respect both.
  Calculator (`apps/calculator.c`) is the first app to opt in: its old
  single static `g_calc`/`g_buttons`/`g_group` globals are gone,
  replaced by a `kzalloc()`'d `struct calculator_instance` per window
  (freed in the new `calculator_close()`), so each open Calculator
  window has fully independent state. Every other app (Notepad, About,
  Terminal) is unaffected -- both new fields default to unset/0/NULL.
  Window titles for multiple windows of the same app stay identical on
  purpose (no "(2)" suffix) -- explicit choice, not a limitation.
  Verified via QMP: opened two Calculator windows from the Start menu
  (cascaded, both titled "Calculator"), typed a digit into the first,
  confirmed the second still showed a fresh "0", closed the second via
  its title-bar X and confirmed the first's state and taskbar entry
  were untouched, with no panic in the serial log.
- The Start menu popup (app list + system actions, hover/click-flash
  feedback) is now its own component: `apps/wm/start_menu.c`/
  `start_menu.h`, factored out of `wm.c`/`wm_input.c`/`wm_render.c`
  now that it had grown real state and behavior of its own. Not an
  independent module with a clean boundary -- it still reaches into
  `wm_internal.h` for shared WM state (`screen_h`/`taskbar_h`/
  `redraw_pending`/`gui_app_registry`/`open_app()`), same pattern
  `wm_input.c`/`wm_render.c` already use (see `apps/wm/wm.c`'s top
  comment). A new `geometry()` static helper inside `start_menu.c`
  shares the row-layout math between drawing and click-handling, which
  used to be hand-duplicated between `wm_render.c` and `wm_input.c`.
  Public entry points: `start_menu_open_now()`, `start_menu_draw()`,
  `start_menu_handle_click()`, `start_menu_update()`, plus the
  `start_menu_open`/`wm_system_actions`/`wm_system_action_count`
  state. Pure refactor, no behavior change -- verified via QMP: Start
  menu opens from the taskbar button, hover tracks the mouse live,
  clicking a row shows the gold flash then closes the menu, same as
  before the split.
- New Task Manager app (`apps/taskmgr.c`/`.h`) -- lists every open
  window (title + normal/minimized/maximized state) and shows system
  memory (physical RAM total/used via `pmm_*`, kernel heap total/used
  via `heap_*` -- both already existed in `kapi.h`, just never had a
  UI). Redraws every tick along with the taskbar clock, so the numbers
  stay live without a manual refresh. No CPU column -- GUI apps aren't
  scheduled processes in this kernel, so there's no real per-app CPU
  number to show yet (see docs/decisions.md). Needed one small new
  `apps/wm/wm.h` addition: `wm_window_count()`/`wm_get_window()`, a
  read-only accessor pair so an app outside `apps/wm/` can list windows
  without reaching into `wm_internal.h` (which stays WM-private).
  Verified via QMP: opened Task Manager, confirmed it lists itself,
  opened two Calculator windows and watched "Heap used" and the window
  list update live, closed them and confirmed both returned to their
  prior values.
- **Fixed a real heap-corruption bug found by the above**: the kernel
  heap's `kfree()` (`kernel/core/heap.c`) could silently corrupt a
  still-in-use block's size field when freeing its list-previous
  neighbor, if that neighbor happened to still be allocated -- the
  coalescing helper only checked whether the block being merged *in*
  was free, not whether the block being merged *into* was. This sat
  completely invisible until Task Manager displayed `heap_used_bytes()`
  for the first time and showed an impossible ~16 exabyte figure.
  Root-caused, fixed (added a `b->prev->free` guard before merging),
  and `heap_selftest()` strengthened to check `heap_used_bytes() == 0`
  after freeing everything, specifically so this class of bug can't
  regress silently again. See docs/decisions.md for the full story.
- New dev tooling in `tools/`, aimed at making the build/test/delivery
  loop this project's own `CLAUDE.md`/skill describes faster and less
  error-prone: `preflight.sh` (one command running
  `make clean && make all && make iso` + the boot smoke test + a git
  status summary -- "am I safe to deliver?" in one pass instead of
  three commands run by hand), `deliver.py` (builds the file-list/
  device-path/protected-file manifest and a commit-message skeleton for
  the delivery step, catching the `Makefile`/`*.yml` protected-file
  exception before a real `device_commit_files` call would reject it),
  `gui_flow.py` (named QMP click-flows -- `open_app("Calculator")`
  instead of hand-deriving Start-menu row pixel math every session),
  and `screenshot_diff.py` (pixel-diffs two screenshots with a
  pass/fail threshold, for catching a rendering regression manual
  eyeballing might miss).
- **TFS2's on-disk format now supports multi-gigabyte files.**
  Previously every file was capped at 2048 bytes, stored inline in one
  fixed-size table record; now each file record holds a small set of
  block-number pointers (12 direct + single/double/triple indirect,
  the same scheme real Unix filesystems have used for decades) into a
  new block-addressed region of the disk, backed by a free-block
  bitmap. `disk.img` grew from 1MiB to a sparse 9GiB (`Makefile`) to
  have room for it -- **this is an incompatible on-disk format change**
  (version byte bumped 1 -> 2): an old disk.img is detected as foreign
  and reformatted from scratch, same "no migration, just reformat"
  policy this project has always used for format bumps, but it means
  existing saved files are lost the first time this boots against an
  old image. Run `make clean-disk` once to get a correctly-sized fresh
  one.
  New `ata_read_sectors()`/`ata_write_sectors()` (`kernel/drivers/
  ata.c`/`.h`) transfer up to 8 sectors (one 4096-byte filesystem
  block) in a single ATA command instead of one command per 512-byte
  sector, for both the PIO and DMA paths -- the DMA path reuses the
  bounce buffer that was already a full 4096-byte frame (only 512 of
  it was ever used before), so no new allocation was needed.
  Also added the new streaming API this all exists to support:
  `fs_read_range()`/`fs_write_range()`/`fs_size()` (`fs.h`), for
  reading/writing a file in bounded chunks instead of needing the
  whole thing in RAM at once -- because `fs_read()`'s existing "whole
  file in one buffer" contract literally cannot work for a file bigger
  than available RAM (256MB in the normal QEMU config), no matter how
  large the on-disk format gets. `fs_read()`/`fs_write()` themselves
  are unchanged for every existing caller (Notepad, the shell,
  editor.c) -- small files still work exactly as before, just
  reassembled from blocks into one heap-allocated staging buffer
  instead of being one inline blob already in RAM.
  Verified via QEMU: a new `fs: selftest passed (triple-indirect
  addressing verified)` boot-time check (`tfs.c`'s `tfs_selftest()`,
  same "prove it every boot" pattern as `heap_selftest()`/
  `pmm_selftest()`) writes/reads/deletes a small chunk at a ~4.6GB
  offset -- past direct+single+double indirect's combined ~4GB
  capacity, so it only passes if the triple-indirect chain was built
  and walked correctly, without needing to actually write gigabytes of
  data at boot. Also verified interactively: saved a Notepad file,
  restarted QEMU from cold, reloaded it -- confirming the new format
  round-trips correctly through a real reboot, not just within one
  boot's RAM state. A full end-to-end 8GB write/read wasn't run in
  this session (would take a long time over emulated PIO/DMA) -- see
  docs/roadmap.md.
- New heap-backed JSON parser/serializer: `kernel/core/json.c`/
  `kernel/include/json.h` -- full nested objects/arrays, coexisting
  with `etc_config.h`'s flat name=value format rather than replacing
  it (existing `/etc/toyos.conf` settings are untouched; JSON is
  available for a future config file that genuinely needs nesting).
  No floating point (`JSON_NUMBER` is `int64_t`) -- this kernel is
  built with `-mno-sse -mno-sse2` and no soft-float, matching
  `apps/calc_engine.h`'s same reasoning; a fractional literal parses
  but truncates. The recursive-descent parser caps nesting at
  `JSON_MAX_DEPTH` (32) specifically because the kernel stack is a
  fixed 16KB. `json_read_file()`/`json_write_file()` go through the
  new `fs_size()`/`fs_read_range()`/`fs_write_range()` streaming API
  (this session's earlier TFS2 entry) rather than the old whole-file
  `fs_read()`/`fs_write()`, so a large JSON document isn't capped by
  that. Verified via a `json_selftest()` run at every boot (parse a
  nested document with strings/numbers/bools/arrays/escapes, check
  every accessor, round-trip it through `json_write()` and re-parse) --
  `json: selftest passed` appears in the boot log right after the
  heap self-test.
- **`apps/widgets.c`/`.h` no longer exist -- every widget moved into
  its own file under `apps/ui/`**, by explicit request (previously
  `ui_button`/`ui_button_group`/`ui_textbox` lived there while
  scrollback/scrollbar/checkbox/the base `widget_hit`/`widget_button`
  primitives stayed behind in the older file). New files: `ui_primitives.c`/
  `.h` (the base `widget_hit()`/`widget_button()`), `ui_scrollback.c`/`.h`
  (the `text_scrollback` console/text-editing widget), `ui_scrollbar.c`/
  `.h` (its companion scrollbar), `ui_checkbox.c`/`.h`. `struct text_field`/
  `widget_textfield_*()` (the single-line text-input implementation)
  moved directly into `ui_textbox.c`/`.h`, folded into the object that
  was already its only real caller, instead of staying split across two
  files. Deliberately a pure file-move, not a rename or redesign --
  every function keeps its old `widget_*` name and signature, same
  precedent `ui_button_group.c` already set ("moved here from apps/,
  same content, no behavior change"). `ui_scrollback`/`ui_scrollbar`
  stay plain stateful structs/free functions rather than gaining an
  owned-geometry wrapper like `ui_button`/`ui_textbox` -- every real
  caller (Notepad, Terminal, the editor) already recomputes its content
  rect live from the window's current size every frame, so there's
  nothing an owned-geometry object would save. See `docs/decisions.md`.
  Verified via QMP across all three affected apps after a clean rebuild:
  Notepad (filename field + Save/Load buttons + multiline editing +
  cursor bar all still work), Terminal (scrollback rendering, colors,
  the real shell running inside it), Calculator (button press/release
  visual feedback, a real computation via the migrated `widget_button()`
  chain) -- no behavior differences found.
- **Preliminary desktop background + icon grid, and a reusable
  right-click context menu, wired into four surfaces.** New
  `apps/wm/desktop.c`/`.h`: fills the area below the taskbar (previously
  a bare color fill inline in `wm_render_frame()`) and draws one icon
  per `gui_app_registry` entry in a left-edge column -- a hand-drawn
  filled square with the app name's first letter stands in for a real
  icon image (no image decoder yet, see `docs/roadmap.md`). Single-click
  selects (highlight only); a second click on the same icon within
  `DESKTOP_DOUBLE_CLICK_TICKS` (~300ms) launches it, matching a real
  desktop's double-click-to-open convention -- previously an idle
  roadmap item ("Desktop icons"), now built. New `apps/wm/context_menu.c`/
  `.h`: a generic reusable popup (label + callback + caller-supplied
  `ctx` pointer per row, same peer-file pattern as `start_menu.c`) --
  any caller can open one anchored at the cursor position, clamped to
  stay fully on screen. Wired into all four places requested: right-click
  the desktop background shows a quick-launch menu (one row per
  registered app); right-click any window (title bar or content area)
  shows Minimize/Maximize-or-Restore (only for resizable apps, matching
  the title-bar button's own rule)/Close, mirroring the title-bar
  buttons without needing to land exactly on one of them; right-click a
  taskbar app button shows "Close window"; right-click a Start menu row
  shows "Open" (closes the Start menu first, same as a left-click would).
  A right-click always closes whatever popup was already open before
  deciding what (if anything) the new click should show, so right-clicks
  never stack menus. `wm.c`'s main loop gained the same edge-triggered
  detection the left button already had (`buttons & 0x2`, mouse.h's
  right-button bit, previously read but never dispatched anywhere).
  Deliberately not included this round (see `docs/roadmap.md`):
  per-icon context menus (right-clicking an icon shows the same
  desktop-wide quick-launch menu as empty space), real wallpaper images,
  and repositioning/dragging icons. Verified via QMP: desktop icons
  render and launch correctly (single-click selects, double-click
  opens), all four right-click surfaces show the correct menu with
  correct items (including Calculator's window menu correctly omitting
  "Maximize" since it's non-resizable), menu clamping keeps a
  near-bottom-edge taskbar menu fully on screen, and every action
  (launch/close/minimize) actually executes and updates the screen
  correctly afterward.
- **Click-to-position and text selection in `text_scrollback`, and an
  interactive serial debug console on COM1.** Two related requests from
  the same round -- user chose, via `AskUserQuestion`: build the text
  editing on the *shared* `apps/ui/ui_scrollback.c` widget (used by
  Notepad, Terminal, and `apps/editor.c`) rather than a Notepad-only
  one, migrate Notepad's Save/Load buttons to `ui_button_group` (what
  Calculator already uses) as part of the same pass, do click-to-position
  and full selection (drag + shift+arrow) in one go rather than phased,
  and build the debug interface as an interactive command console over
  serial rather than a one-shot dump or a continuous trace stream.
  - `ui_scrollback.h`/`.c`: new `widget_scrollback_index_at_point()`
    (inverts a pixel position back to a buffer index, mirroring the
    existing measure-pass's wrap/newline capture convention exactly so
    a click round-trips to the same visual cursor spot); new selection
    API (`widget_scrollback_selection_start/clear/present/range`,
    `widget_scrollback_delete_selection()`); `widget_scrollback_draw()`
    gained a `sel_bg` parameter (widgets stay theme-agnostic -- callers
    supply the color, same as everywhere else in `apps/ui/`) and now
    paints a highlight rect behind selected characters.
    `THEME_SELECTION_BG` added to `apps/theme.h`.
  - `kernel/include/keyboard.h`/`kernel/drivers/keyboard.c`: new
    `KEY_SHIFT_ARROW_*`/`KEY_SHIFT_HOME`/`KEY_SHIFT_END` codes, emitted
    from the extended-scancode handler based on live shift state at
    scancode-processing time (same timing convention the existing
    shift table already uses for letters).
  - `apps/notepad.c`/`.h`: click positions the cursor; drag extends a
    selection (reusing the existing `on_drag_start`/`on_drag`
    mutual-exclusivity contract -- claiming every text-body mouse-down
    unconditionally means a drag that never moves is just a plain
    click, no separate code path needed); shift+arrow/Home/End extends
    a selection the same way; Backspace/Delete/typing over an active
    selection replaces it instead of acting at the cursor. Save/Load
    buttons migrated from hand-rolled hit-testing to `ui_button_group`,
    gaining press/release visual feedback (`notepad_press`/
    `notepad_release`) Calculator already had but the old buttons
    didn't.
  - New `kernel/core/debug_console.c`/`.h`: a line-buffered interactive
    command console over serial -- `help`/`meminfo`/`lsdev`/`lsfs
    [path]` -- non-blocking `debug_console_poll()` called from
    `keyboard_getchar()`'s idle hlt-wait (physical shell) and
    `wm_run()`'s main loop (GUI desktop), so a serial session stays
    responsive whichever one is active, without new kernel-thread/
    scheduling machinery. Output goes through `klog_write()`, so
    console responses also land in `dmesg`. Exposed to `apps/` via
    `kapi.h` (`debug_console_poll()` only -- `apps/wm/wm.c` doesn't
    reach into `kernel/core` directly).
  - `kernel/include/serial.h`/`kernel/core/serial.c`: added RX support
    -- a ring buffer, `serial_try_getc()`, and a new `serial_irq_init()`
    (IRQ4 registration + PIC unmask + UART IER enable) called from
    `kernel_main()` right after `idt_init()`, deliberately split out of
    `serial_init()` itself (which runs *before* `idt_init()`, so
    registering/unmasking that early would've been undone by
    `idt_init()`'s own masking pass). Found live, not by inspection: an
    early version registered the IRQ and unmasked it at the PIC
    correctly but still received nothing, because `serial_init()`'s
    original `outb(COM1+1, 0x00)` leaves the UART's own Interrupt
    Enable Register at 0 -- disabling interrupt generation at the chip
    itself, independent of the PIC. `serial_irq_init()` now also sets
    IER bit 0.
  - `tools/gui_flow.py`: fixed a second Start-menu click-math bug past
    the `ITEM_H` fix from a prior round -- the derived top-Y formula
    (`(SCREEN_H - TASKBAR_H) - ITEM_H * total_items`) was close enough
    to work for a middle row but landed too close to the row-0/row-1
    boundary specifically, misclicking "About" when asked for
    "Notepad". Replaced with `MENU_TOP_Y = 536`, measured directly from
    a live screenshot rather than re-derived from constants that had
    already been wrong once.
  - Verified via QMP: click-to-position (typing exactly where clicked,
    mid-string); drag-select producing a visible highlight; shift+arrow
    selection extending correctly from Home; Backspace deleting an
    active selection (both drag- and shift-arrow-created); Save then
    Load round-tripping real content through the filesystem; the
    serial debug console answering `meminfo` with real live data while
    the GUI desktop was active. Screenshots in `screenshots/2026-08-11/`.
  - Known limitation, documented rather than solved: no output lock
    exists anywhere in this kernel, so a concurrent `klog_write()` from
    elsewhere could in principle interleave with the debug console's
    own output. Accepted for a debug-only tool rather than adding new
    synchronization machinery for it.
- **Data-driven keyboard layouts (`/etc/kbs/<name>`), replacing the
  compiled-in two-layout enum.** Reported bug: on the Finnish/Swedish
  physical keyboard, the key next to the right Shift (US legend `/`)
  should type `-`/`_`, not `/`/`?` -- the old `se` layout only remapped
  the three Å/Ä/Ö keys, nothing else. User chose, via
  `AskUserQuestion`: `name=value` files (reusing the shape of
  `etc_config.h`'s existing format, though not its parser -- see
  below), scancode translation moved into its own new
  `kernel/core/keyboard_layout.c` rather than staying in
  `keyboard.c` (the driver's job is raw scancodes/shift-state, not
  owning character tables), layout files generated from Linux's own
  XKB layout data rather than hand-typed, and seeded onto `disk.img`
  at build time so they ship in the repo/ISO like `/bin/lspci` already
  does.
  - New `tools/gen_kbs.py`: runs `xkbcli compile-keymap --layout
    <name>` (part of `libxkbcommon-tools`, no X server needed) and
    emits an `/etc/kbs/<name>` file from it. Only translates shift
    levels 1-2 (base + Shift) -- this driver has no AltGr handling at
    all, so level 3/4 symbols would be unreachable anyway -- and only
    characters this kernel's font can render (ASCII + the six Nordic
    Latin-1 letters). Dead keys (e.g. Swedish's acute/grave accent
    key) aren't composed, mapped instead to their plain undead glyph,
    same simplification XKB's own `nodeadkeys` variants make.
    `python3 tools/gen_kbs.py <xkb-layout> --write` regenerates
    `seed/sync/etc/kbs/<name>`; adding a third layout later is running
    this once, not an afternoon with a scancode chart.
  - New `kernel/include/keyboard_layout.h` / `kernel/core/keyboard_layout.c`:
    `keyboard_layout_load(name)` reads `/etc/kbs/<name>` (own small
    parser, not `etc_config_get()` -- that one's `ETC_CONFIG_MAX` write
    buffer is far too small for a ~130-line layout file, and it strips
    a trailing `#...` as a comment even on a real data line, which
    would silently eat the `#` character itself as a mapped value);
    falls back to `/etc/kbs/us`, then to a small compiled-in US table,
    if the requested (or even `us`) file can't be read -- so the
    keyboard is never left producing nothing. Returns whether the
    requested file was actually found, so the shell's `keyboard`
    command can tell the user when it silently fell back.
    `keyboard_layout_translate(scancode, shift)` is the one lookup
    `keyboard.c`'s `keyboard_feed_byte()` now calls.
  - `keyboard.c`/`keyboard.h`: removed `enum keyboard_layout`,
    `scancode_ascii[]`/`scancode_ascii_se[]` and their shifted
    variants, `keyboard_set_layout()`/`keyboard_get_layout()`/
    `keyboard_layout_name()` -- the driver now only tracks shift/
    extended-prefix state and calls `keyboard_layout_translate()`.
  - `keyboard_config.c`/`.h`: persistence now works from a plain layout
    name string (`"keyboard_layout=<name>"` in `/etc/toyos.conf`, as
    before) instead of the enum; always calls
    `keyboard_layout_load()` at boot (even with nothing persisted yet)
    so the tables are populated before the first keypress, not left
    zeroed until something explicitly loads a layout.
  - `apps/shell_sys.c`'s `keyboard` command: `keyboard <name>` now
    accepts any name with a matching `/etc/kbs/` file, not just a
    hardcoded `us`/`se` check; reports "not found ... reverted to us"
    on an unknown name instead of a fixed error list.
  - `kapi.h` gained `keyboard_layout.h` (apps/ only ever reach the
    driver surface through `kapi.h` -- see `CLAUDE.md`).
  - Real bug caught mid-implementation, not by review: an early cut of
    `tools/gen_kbs.py`'s key list only covered the printable
    alphanumeric block and omitted Escape/Backspace/Tab/Enter
    entirely -- so the moment the shell loaded layouts from these
    generated files instead of `keyboard.c`'s old compiled-in tables,
    Enter stopped submitting a command at all (silently swallowed,
    every keystroke just kept appending to the prompt). Caught by
    actually testing in QMP (typing `keyboard se` produced
    `keyboardse` with no newline), not by reading the generator's
    code. Fixed by adding the four control keys to the generator's key
    list with their own keysym-name mappings.
  - Verified via QMP: `keyboard` alone reports the current layout;
    `keyboard se` switches and the right-Shift-adjacent key now types
    `-`/`_` (previously `/`/`?`); `keyboard xyz` reports "not found"
    and reverts to `us`; Å/Ä/Ö still type correctly under `se`;
    Backspace/Enter both still work after switching layouts; the
    choice survives a QMP `system_reset` (persisted layout reloads at
    boot). Screenshots in `screenshots/2026-08-11/`.

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

### Added
- `df` shell command (`cmd_df()`, `apps/shell_sys.c`) -- shows
  total/used/free space on the persistent filesystem, sourced from a
  new `fs_disk_usage()` VFS call (`kernel/include/fs.h`/`fs_ops.h`,
  dispatched through `vfs.c` to `tfs_disk_usage()` in
  `kernel/drivers/tfs.c`, which walks `g_bitmap`/`g_ram_bitmap` to
  count free blocks). Displays in KB, not bytes or MB -- MB was tried
  first but rounds any real usage under 1MB down to a misleading flat
  "0" (today's whole seeded `/bin` + `/etc` content is under 1MB);
  bytes would overflow `vga_write_dec()`'s `uint32_t` parameter on a
  multi-gigabyte disk. Found live-testing this command, not by review
  -- see the function's own comment. Verified via QMP: shows
  `total: 9436876 KB / used: 88 KB / free: 9436788 KB` against the
  real seeded disk.
- Command history now persists across reboot. `history_save()`/
  `history_load()` (`apps/shell.c`) write/read a dedicated bare-line
  `/etc/history` file (one entry per line, most-recent-last), loaded
  once at the top of `shell_main()`. Deliberately its own file, not a
  key in the shared `/etc/toyos.conf` -- history entries can
  legitimately contain `=`, and `etc_config.c`'s parser strips a
  trailing `#...` as a comment even mid-line, both of which would
  corrupt real command text; modeled on `tz.c`'s own manual
  line-splitting for the same reason. Verified via QMP: ran `meminfo`/
  `ls`/`df`, restarted QEMU against the same `disk.img` (simulating a
  reboot), confirmed the up arrow recalled `meminfo` from the previous
  session.
- Shutdown confirmation dialog -- clicking the Start menu's "Exit to
  shell" now opens a Yes/No modal ("Exit to shell? Unsaved changes
  will be lost.") instead of exiting immediately. New
  `apps/wm/confirm_dialog.c`/`.h`, following `context_menu.c`/
  `start_menu.c`'s existing screen-absolute WM-overlay pattern (not an
  `apps/ui/` widget -- those are content-relative to a window's own
  origin, which doesn't exist for a WM-level popup). Geometry computed
  once at open time from the message/button label lengths;
  `confirm_dialog_handle_click()` is checked first in
  `wm_input.c`'s `wm_handle_left_click()` chain (most modal first) and
  swallows clicks outside Yes/No without dismissing. Verified via QMP:
  dialog opens centered with both buttons; clicking No dismisses and
  stays in the GUI; clicking Yes actually exits to the physical shell
  ("Back from GUI mode." on serial).
- AltGr handling in the keyboard driver -- Finnish/Swedish `@ # $ { }
  [ ] \ |` (XKB level 3) are now reachable, closing the backlog item.
  `keyboard_layout_translate()` (`kernel/include/keyboard_layout.h`/
  `kernel/core/keyboard_layout.c`) gained an `altgr` parameter and a
  third per-layout table (`g_table_altgr[128]`, populated from new
  `sc_XX_altgr=` lines), taking priority over shift when the pressed
  scancode has an AltGr entry, falling through to shift/base otherwise
  (an unmapped AltGr slot doesn't eat the keystroke).
  `kernel/drivers/keyboard.c` tracks a new `altgr_pressed` flag off the
  `0xE0`-prefixed Right Alt press/release scancodes (`0x38`/`0xB8`,
  extended -- Left Alt is the same byte pair *without* the prefix and
  stays unused). `tools/gen_kbs.py` now reads XKB level 3 (still not
  level 4/Shift+AltGr -- see its own top comment on why that's a small,
  known, acceptable gap) and regenerated `seed/sync/etc/kbs/{us,se}`.
  Verified via QMP: switched to `se`, held AltGr (`alt_r` qcode) with
  `2`/`4` via `QMPSession.combo()`, got `@`/`$` exactly as the
  generated table specifies.
- `stress <mb>` shell command (`apps/shell_sys.c`) -- a real,
  non-sparse write/read/verify pass over `<mb>` megabytes through
  `fs_write_range()`/`fs_read_range()`, built to eventually satisfy
  the roadmap's "full end-to-end multi-GB write/read stress test"
  item. Unlike the boot-time `tfs_selftest()` (which only proves
  triple-indirect *addressing* -- 64 bytes written at a ~4.6GB offset),
  this writes a genuine per-chunk-varying pattern across the whole
  requested size in 1MB chunks (one static reused buffer, O(1) RAM
  regardless of `<mb>`), reads it all back, and byte-for-byte verifies
  every chunk, so a corrupted or misplaced chunk is actually
  detectable. Deliberately on-demand, not part of the boot self-test --
  a real multi-GB pass over this kernel's PIO/DMA ATA path takes real
  wall-clock time. Verified via QMP at `stress 100`: 100MB written,
  read back, and verified correct in 72s. That measured rate
  extrapolates to roughly 50 minutes for a 4.2GB pass (just past the
  ~4004MB triple-indirect boundary) and ~100 minutes for the full 8GB
  target -- both well beyond what this session's interactive testing
  loop could run to completion, so the literal multi-GB pass itself is
  still not done; what shipped is the verified-correct tool to run it
  in one command whenever that time is available (see
  `docs/roadmap.md`).
- Shutdown (Start menu item, alongside "Exit to shell"), closing that
  half of the roadmap's Shutdown item -- the other half (a real
  ACPI-parsed poweroff) is still open, see below. `system_poweroff()`
  (`kernel/core/power.c`/`power.h`) writes QEMU/Bochs's well-known
  ACPI PM1a_CNT I/O-port shortcut (`outw 0x604, 0x2000`), falling back
  to a halt loop with an on-screen message if the write doesn't take
  (real hardware, or an emulator without this legacy behavior) so the
  machine always ends up in a safe, inert state either way. Reuses
  `confirm_dialog.h` (`apps/wm/start_menu.c`'s new `action_shutdown()`)
  -- the same Yes/No popup "Exit to shell" already goes through, per
  that dialog's own top comment anticipating this as its second
  caller. Deliberately NOT a real ACPI shutdown: it doesn't parse the
  FADT/PM1a_CNT address out of the guest's own ACPI tables, just writes
  the value real hardware would only accept after that parsing -- the
  ACPI table parsing roadmap item stays open, and a real poweroff on
  real hardware still needs it. Verified via QMP: "Shutdown" appears
  below "Exit to shell" in the Start menu; clicking it opens "Shut
  down? Unsaved changes will be lost." with Yes/No; clicking Yes
  actually powered the QEMU process off (the QMP socket itself broke
  with a clean shutdown, no fallback-halt message logged) rather than
  just returning to the shell.
- A reusable Open/Save file-picker dialog (`apps/wm/file_picker.c`/
  `.h`), replacing Notepad's old always-visible inline filename field
  with real Save As.../Open... buttons, like a real desktop OS. Same
  screen-absolute WM-overlay pattern as `confirm_dialog.c`/
  `context_menu.c`/`start_menu.c` (not an `apps/ui/` widget -- those
  are content-relative to a window's own origin, which doesn't apply
  to a WM-level popup), backed directly by `fs_list()`/`fs_is_dir()`/
  `fs_exists()` (GUI apps already call `fs_*` straight from kernel
  space, no syscall layer needed). Full navigation, not just a flat
  listing: directories-first alphabetical sort, double-click a folder
  to enter it, a `../` row to go up, a scrollbar (`ui_scrollbar.h`,
  click-to-page -- no drag-to-scroll or mouse-wheel yet, the WM doesn't
  route wheel events to a screen-level modal today) for more entries
  than fit. Typing an absolute or cwd-relative path directly into the
  filename field works too, same as a real dialog's field; typing/
  choosing an existing directory navigates into it instead of erroring.
  `apps/notepad.c`'s Save/Load buttons became Open.../Save As... that
  pop this instead -- every Save is now a Save As (no "current file"
  tracked between saves), per the user's own choice when this was
  scoped. Deliberately not built this round, same "add once a real
  need shows up" bar every popup here uses: Esc-to-cancel (the
  physical Escape key isn't wired to a scancode in
  `kernel/drivers/keyboard.c` at all yet -- `confirm_dialog.h` hit the
  same gap first), creating a new directory from inside the dialog,
  hover highlighting on list rows.
  - Real bug caught by QMP testing, not by review: the first cut of
    double-clicking the `../` row correctly updated `g_cwd`/re-listed
    the parent directory internally, but forgot to set
    `redraw_pending` on that specific path (the sibling
    directory-double-click branch did) -- so the navigation "worked"
    with nothing on screen reflecting it until some unrelated later
    click forced a repaint. A screenshot taken right after the
    double-click looked like a dead button; the underlying state had
    actually moved. Fixed by moving `redraw_pending = 1` into
    `fp_refresh_listing()` itself (every caller changes what's on
    screen) instead of relying on each call site to remember it
    individually.
  - `tools/gui_flow.py`'s `TASKBAR_H`/`ITEM_H`/`MENU_TOP_Y` Start-menu
    click-geometry constants turned out stale independent of this
    change -- pixel-measured against a live screenshot while testing
    the new "Shutdown" row and found the running kernel's default font
    metrics are `gfx_char_h()=21` today, not the `18` these constants
    were calibrated for (`TASKBAR_H` 29 not 32, `ITEM_H` 27 not 24).
    Corrected in the same pass `SYSTEM_ACTIONS` picked up "Shutdown"
    (see gui_flow.py's own updated comments for the re-measurement
    method, and above for the actual Shutdown feature).
  - Verified via QMP: Save As... on real typed text saves to `/`,
    reopening via Open... and loading shows "Loaded." with the exact
    text back; double-clicking `bin/` in the listing enters it and
    shows its real contents (`/bin`'s seeded binaries) with a working
    scrollbar; double-clicking `../` returns to `/` showing its
    original listing. Screenshots in `screenshots/2026-08-11/`.
- Per-subsystem runtime debug-logging switches
  (`kernel/include/debugflags.h`/`kernel/core/debugflags.c`) -- OFF by
  default, flippable at the shell with `debug <name> on|off` (`debug`
  alone lists every subsystem and its state), no rebuild needed.
  Replaces the previous ad hoc pattern (this same session's file
  picker debugging: temporary `klog_write()` calls added at the point
  of suspicion, then hand-deleted again once the bug was confirmed
  fixed) with a standing, named, always-in-the-tree gate: wrap a
  `klog_write()` in `if (dbgflag_enabled(DBGFLAG_WM)) { ... }` and
  leave it there permanently. Subsystems today: `fs`, `wm`, `ata`
  (`DBGFLAG_NAMES` in `debugflags.c` -- add more by extending the enum
  + name table, nothing else needs updating). `apps/shell_sys.c`
  gained `cmd_debug()`, wired into `shell.c`'s dispatch and
  documented under `help tests`.

### Fixed
- `kernel/drivers/ata.c`'s Bus-Master DMA path had no retry on a
  transient transfer failure -- reported live: `stress 10` (and other
  multi-MB runs) "usually" (non-deterministically) failing on real
  hardware/QEMU with `write failed at chunk N`, while the exact same
  code path never failed once in this project's own sandboxed test
  runs. Root cause: `wait_dma_irq()`'s completion wait is bounded to
  3s (`DMA_WAIT_TICKS`) -- on a real desktop, host scheduling jitter
  (other processes briefly starving the QEMU process of CPU) can delay
  the completion IRQ past that bound even though the transfer itself
  is fine, and the driver treated one missed IRQ identically to a
  genuine hardware error: the whole transfer failed outright, with no
  second attempt, taking down whatever multi-block operation it was
  part of. Fixed by wrapping `dma_transfer()` in a new
  `dma_transfer_with_retry()` (`ATA_DMA_MAX_RETRIES` = 3) that
  re-issues the whole command from scratch on failure before giving
  up -- a real, persistent drive error still surfaces as a hard
  failure once every retry is exhausted (always logged via `klog`,
  independent of the `ata` debug switch above), it just no longer
  fails on a single transient miss. Per-attempt detail (which retry
  succeeded, or that one failed) is gated behind `debug ata on` so
  normal operation stays quiet. Verified in the sandbox: `debug ata
  on` + `debug` (listing) + `stress 6` all round-tripped correctly via
  QMP (screenshot in `screenshots/2026-08-11/`); the sandbox's own DMA
  never actually needed a retry (no host jitter to trigger it here),
  which is expected -- this fixes a real-hardware timing condition the
  sandbox doesn't reproduce, not a sandbox-visible bug.

### Improved
- TFS2/ATA write throughput -- `stress 100` was measured at ~1.4MB/s
  (100MB in 72s) on real hardware, root-caused (see the ATA-retry
  entry just above, found while investigating the same "why is stress
  so slow" question) to `kernel/drivers/ata.c`'s DMA write path issuing
  a full synchronous `CMD_CACHE_FLUSH` after every single write, with
  TFS2 itself writing in 4KB pieces -- a 100MB write was on the order
  of 25,600 individual block writes, each paying full flush latency,
  plus a second write-and-flush per newly-allocated block just to
  persist one bit of the free-block bitmap. New `ata_flush_begin()`/
  `ata_flush_end()` (`ata.h`) let a caller batch a run of writes into
  one flush at the end instead of one per write -- every write still
  reaches the drive immediately, only the FLUSH command is deferred, so
  a read-back mid-batch still sees correct data. `write_range_impl()`
  (`kernel/drivers/tfs.c`) wraps its whole per-call block-writing loop
  in one such batch (`write_batch_begin()`/`write_batch_end()`), and
  `persist_bitmap_bit()` now defers the bitmap sector write itself
  during a batch too (not just its flush), coalescing what used to be
  one redundant sector write per allocated block into one write per
  distinct dirty sector (`g_bitmap_dirty[]`, flushed once at
  `write_batch_end()`). Every begin() is matched by an end() on every
  exit path, including the out-of-space/read/write-failure early
  returns (`write_range_impl()` now tracks success via a local `ok`
  flag through a single cleanup point instead of returning directly
  mid-loop) -- an unmatched begin() would otherwise leave every future
  write silently unflushed.
  - Deliberately NOT applied to `persist_record()`'s journal-protected
    metadata writes -- those need each write durable before the next is
    issued for `replay_journal()`'s crash-recovery guarantee to hold;
    batching the flush there could let the drive's real write order
    diverge from what the journal protocol assumes. See the new
    `docs/decisions.md` entry for the full reasoning on where this
    line is drawn.
  - Measured in the sandbox (real hardware should see more, since
    flush latency -- not present at meaningful cost on this sandbox's
    own fast backing storage -- is what actually dominates the
    original slowness): `stress 100` 72s -> 53s (~26% faster);
    `stress 300` (crosses into double-indirect block addressing, still
    verified byte-for-byte correct) ~124s vs. the old rate's ~216s
    extrapolation (~43% faster). Screenshot in `screenshots/2026-08-11/`.
  - Coalescing contiguous block writes into fewer/larger ATA commands,
    and journal-batched flush for metadata, are still open -- see
    `docs/roadmap.md`'s follow-up item.
  - Confirmed on the real hardware that originally hit both the DMA
    timeout failures and the slow throughput: `stress 100`/`200`/`300`/
    `400` all PASSED with zero DMA retries needed, scaling linearly at
    ~3.5MB/s (28s/55s/85s/115s) -- both this fix and the DMA-retry fix
    above are doing their job together, not just in the sandbox. See
    `docs/roadmap.md`'s multi-GB stress-test item for the updated
    full-scale time estimate.

### Added
- Non-blocking DMA start/poll pair for `kernel/drivers/ata.c` --
  Phase 1 of the async-I/O roadmap item (see `docs/roadmap.md`), asked
  for after explaining why disk writes/reads block the whole kernel
  today: apps run in kernel space, so a write from Notepad's Save, the
  shell's `stress`, or `wm_run()`'s own event loop all sit inside the
  exact same call stack that's waiting on the drive -- a slow write
  freezes the whole desktop, not just the operation. `dma_transfer()`
  (the existing blocking call every real disk read/write already goes
  through) is unchanged in behavior, but its body is now two shared
  halves -- `dma_issue()` (program the PRD, kick the command off) and
  `dma_finish()` (stop the bus-master engine, ack the drive's IRQ,
  copy a read's data out of the bounce buffer or flush a write) --
  with `wait_dma_irq()`'s blocking wait sandwiched in between, same as
  before. `dma_transfer_start()`/`dma_transfer_poll()` (`ata.h`) call
  the same two halves but let the CALLER decide how to wait: `start()`
  kicks a transfer off and returns immediately, `poll()` does one
  non-blocking check (`ATA_POLL_PENDING`/`ATA_POLL_DONE`/
  `ATA_POLL_FAILED`) and returns right away either way, with the same
  bounded-timeout logic `wait_dma_irq()` already had (3s via
  `DMA_WAIT_TICKS`, or `ATA_POLL_LIMIT` iterations when called from
  inside a syscall) now living in the poll loop instead of a single
  blocking call. Only one transfer can be in flight at a time
  (`g_pending.in_flight`) -- there's one PRD/bounce buffer to share,
  same constraint the blocking path always had.
  - No real caller uses this yet -- `fs.c`/`tfs.c` still go through the
    unchanged blocking `dma_transfer_with_retry()` path. This is
    deliberately scoped to just the driver-level primitive; a
    steppable `fs_write_range()` (Phase 2) and wiring `wm_run()` to
    poll one so the GUI stays responsive during a save (Phase 3) are
    the next two roadmap steps, not built this round.
  - Proven via a new diagnostic shell command, `dmatest [lba]` (default
    lba 0) -- read-only, so it's always safe to run: reads the given
    sector once through the existing trusted blocking path
    (`ata_read_sector()`) and once through the new non-blocking
    start/poll pair, byte-compares the two, and reports how many
    `dma_transfer_poll()` calls it took. Reports "DMA path not active"
    rather than failing on a PIO-only machine (this primitive has no
    PIO equivalent -- see `ata.h`). Verified via QMP: `dmatest` (lba 0)
    passed with 0 poll calls, `dmatest 9` passed with 1 -- confirming
    the poll loop genuinely returns PENDING at least once rather than
    trivially completing on the first check every time. `stress 5`
    re-verified passing afterward too, confirming the `dma_transfer()`
    refactor didn't change the existing blocking path's behavior.
    Screenshots in `screenshots/2026-08-12/`.
- Steppable write API -- Phase 2 of the async-I/O roadmap item (see
  `docs/roadmap.md`), continuing straight on from Phase 1 above.
  `write_range_impl()` (`kernel/drivers/tfs.c`, the shared engine
  behind `fs_write_range()`/`fs_write()`) had its per-block loop body
  pulled out into `write_range_one_block()`; `write_range_impl()`
  itself just calls it in a tight loop same as before (unchanged
  behavior for every existing caller), and a new
  `tfs_write_range_begin()`/`tfs_write_range_step()` pair calls the
  same helper but lets a caller advance it one block at a time from
  OUTSIDE this file instead. Threaded all the way up the existing VFS
  dispatch layer so it's a real, reusable API, not a TFS2-only
  shortcut: two new `struct fs_ops` function pointers
  (`kernel/include/fs_ops.h`), `vfs.c` dispatch wrappers, and two new
  public entry points in `fs.h` -- `fs_write_range_begin()` (returns an
  opaque handle, or NULL on the same setup failures `fs_write_range()`
  already reports via 0) and `fs_write_range_step()` (returns
  `FS_STEP_PENDING`/`FS_STEP_DONE`/`FS_STEP_FAILED`, cleaning the
  handle up automatically on either terminal result). On
  `FS_STEP_DONE` the file's size/modified-time/on-disk directory
  record are all updated, identical to a successful blocking
  `fs_write_range()` call -- nothing about the file afterward reveals
  which API wrote it.
  - No real caller uses this yet -- `fs_write_range()`/`fs_write()` and
    everything built on either are still fully blocking, unchanged.
    Phase 3 (wiring `wm_run()` to poll one so the GUI stays responsive
    during a save) is next.
  - Proven via a new diagnostic shell command, `steptest <mb>` -- write
    `<mb>` megabytes through an explicit `begin()`/`step()` loop this
    command drives itself (standing in for what `wm_run()` would
    eventually do once per frame), read it back through the ordinary
    `fs_read_range()`, verify byte-for-byte, report total step count.
    Verified via QMP: `steptest 3` passed, 768 `step()` calls for 3MB
    (exactly 3MB / 4KB-per-block, confirming one step per block as
    intended). `stress 5` and `dmatest` re-verified passing afterward
    too, confirming the `write_range_impl()` refactor didn't change
    the existing blocking path's behavior -- `dmatest` needed 658 poll
    calls this run (vs. 0 the first time in Phase 1's own testing),
    which is expected variance (disk busier right after a write-heavy
    `stress`/`steptest` run), not a regression -- the poll loop
    correctly kept waiting rather than timing out.
    Screenshots in `screenshots/2026-08-12/`.

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

