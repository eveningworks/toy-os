# Roadmap / Ideas for what's next

Forward-looking "not built yet" items, moved out of README.md to keep that file focused on "what toy-os can do today." Completed items are struck through and linked to the CHANGELOG build that finished them --
this list is actively maintained, not a stale wishlist. See `docs/decisions.md` for *why* existing things are built the way they are, and `CHANGELOG.md`/`CHANGELOG-archive.md` for the full history.

## At a glance

**In progress / up next**
- [ ] Async/continuously-armed process spawning for the GUI Terminal, so `run`/`ls`/any future `/bin` binary can execute from inside `apps/terminal.c` instead of being wholesale-blocked (`BLOCKED_CMDS`). Deliberately deferred, not started: scoped during `ls`'s migration to a real `/bin` binary (see `CHANGELOG.md`'s `[Unreleased]` entry) when the user was shown the real cost and explicitly chose to ship `ls` now, blocked in the Terminal same as `run`, rather than build this first. The blocker is architectural, not a small fix: `elf_run_from_fs()`/`process_run_ring3_args()` (`kernel/core/elf_run.c`, `kernel/core/process.c`) is synchronous and blocking by design -- it doesn't return to its caller until the ring-3 process exits or faults -- and `apps/wm/wm.c`'s `wm_run()` is a plain, uninterrupted kernel-space event loop, never itself scheduler-managed. Running a `/bin` binary from inside a Terminal window without freezing the whole desktop needs: a new public spawn API distinct from today's blocking one, the scheduler continuously armed (today it's demo-only, disarmed outside `schedtest`) so a spawned process can be polled/stepped rather than run to completion in one call, `wm_run()`'s event loop restructured to poll a running background process alongside its existing input/redraw work, and new per-window "process running" state in `apps/terminal.c` (output streaming into the window's scrollback as it arrives, not all at once at exit). Several existing `/bin` binaries (`gui_test`, `win_test`, `echo_test`) would still need individual hazard fixes on top of this (never-exits, draws straight to the framebuffer bypassing the window, wants real concurrency) even once the core mechanism exists.
- [x] ~~Migrate Notepad's Save/Load buttons to `ui_button_group`~~ -- done, bundled with click-to-position/text-selection (see `CHANGELOG.md`'s `[Unreleased]` entry): Save/Load now go through `ui_button_group`, gaining press/release feedback Calculator already had.
- [ ] Multi-architecture support (e.g. RISC-V 64 alongside x86_64) -- assessed, not started: roughly a tenth of the codebase is architecture-specific and it's already well-insulated behind `kapi.h`, but boot/interrupts/paging/port-I/O are a real per-arch project. Full breakdown, proposed `kernel/arch/<arch>/` layout, and a phased plan in `docs/arch-portability.md`
- [x] ~~Real disk-hosted ELF binaries -- load from `/bin` at runtime instead of every `.elf` being a GRUB module baked into the ISO~~ -- done (see `CHANGELOG.md`'s `[Unreleased]` entries): `lspci` is a real ring-3 process, loaded from `/bin/lspci` on the persistent filesystem via `run lspci`, using two new syscalls (`SYS_PCI_COUNT`/`SYS_PCI_INFO`) to do something other than file I/O. `/bin/lspci` is now seeded onto `disk.img` at BUILD time (Makefile's `seed` target, using `tools/tfs2_writer.py`) rather than installed at boot -- the original boot-time bootstrap-install (`install_bin_binaries()`/`BIN_BOOTSTRAP`) was removed once the writer tool made it redundant (see `docs/decisions.md`).
- [ ] TCP/IP networking -- the infra pieces are done (PCI enumeration, IRQ registration, contiguous/DMA memory, the socket/fd syscall surface, IRQ-driven DMA example); no NIC driver or protocol stack yet

**Backlog**
- [x] ~~AltGr handling in the keyboard driver~~ -- done, see `CHANGELOG.md`'s `[Unreleased]` entry: `keyboard_layout_translate()` gained a third per-layout table (level 3/AltGr), `tools/gen_kbs.py` regenerated from XKB, Right Alt tracked via its `0xE0`-prefixed scancode. Level 4 (Shift+AltGr) still not tracked -- a real gap but a small, known one (see the entry).
- [x] ~~A real host-side TFS2 writer tool~~ -- done, see `CHANGELOG.md`'s `[Unreleased]` entry: `tools/tfs2_writer.py` (`write`/`read`/`ls`/`sync` subcommands) writes/reads files directly against `disk.img`, no boot or kernel rebuild needed. `sync` mirrors a whole seed directory in with a copy-once vs. content-hash-synced policy split (`once/` vs `sync/` subtrees). Write support is scoped to direct + single-indirect blocks (~4.03 MB/file, see the CHANGELOG entry) -- extending to double/triple-indirect for larger seeded files is a follow-up if that need ever comes up.
- [ ] `wintest` (`SYS_WIN_*`) windows made non-modal, sharing scheduler time with the kernel-space window manager instead of taking the CPU exclusively
- [ ] `g_next_kernel_rsp` reentrancy fixed properly, so a real blocking syscall doesn't need to spin-poll from ring 3 the way `echotest` does today
- [x] ~~`fs_write()` offset-based partial writes~~ -- done, see CHANGELOG.md's TFS2 multi-GB rework entry (`Unreleased`): `fs_write_range(path, offset, buf, len)` / `fs_read_range()` now exist alongside the original whole-file `fs_write()`/`fs_read()` (kept as-is for small-file callers like Notepad/shell/editor.c).
- [ ] A real C library on top of `filetest`'s fd-aware syscalls: CRT0 (argc/argv), TLS (FS.base), FPU/SSE context-switch save/restore
- [x] ~~Notepad: editable filename (currently fixed to `notepad.txt`)~~ -- already done, this line was stale: the filename toolbar field (`apps/notepad.c`) is a real `ui_textbox`, editable by clicking it, with Save/Load reading/writing whatever's currently typed there (`st->filename.field.buf`), not a hardcoded name. Found already shipped (build that added Notepad's click-to-position/selection + toolbar migration to `ui_textbox`, same session as this correction) while starting work on this item -- corrected here rather than duplicating it.
- [ ] VFS: multiple filesystem backends mounted at once, not just one chosen at boot
- [ ] AHCI/SATA driver -- today's `ata.c` depends on the legacy IDE controller real modern hardware increasingly lacks
- [ ] GPT/MBR partition table parsing -- `disk.img` is one raw TFS2 blob today, not a partitioned disk. Explicitly re-confirmed as a "build it anyway, later" item when TFS2 was reworked for multi-GB files (this session): not required for large-file support (TFS2 v2's own block addressing handles that), purely for future flexibility (e.g. hosting more than one filesystem image on one disk).
- [ ] Full end-to-end multi-GB (e.g. 8GB) file write/read stress test over TFS2 v2 -- the on-disk format itself is no longer the blocker: a real, on-demand `stress <mb>` shell command now exists (`apps/shell_sys.c`, see `CHANGELOG.md`'s `[Unreleased]` entry) that writes/reads/byte-for-byte-verifies genuine (non-sparse) data through `fs_write_range()`/`fs_read_range()`, and was verified correct at `stress 100` (100MB in 72s). Real-hardware testing of `stress 10` found (and a later session fixed, see `CHANGELOG.md`'s `[Unreleased]` entry) a genuine bug on the way: `kernel/drivers/ata.c`'s DMA path had no retry on a transient IRQ-wait timeout, so ordinary host scheduling jitter (present on a real desktop, absent in this project's sandboxed test runs) could fail a whole transfer non-deterministically -- fixed with a bounded retry (`dma_transfer_with_retry()`, `ATA_DMA_MAX_RETRIES` = 3). Re-tested on real hardware afterward with `debug ata/fs on`: `stress 10`/`20` now pass, but `stress 30` still failed outright (`ata: dma write failed after 3 attempts`, logged unconditionally to `dmesg`/the serial console) -- so the retry helps but doesn't fully cover it; whatever's delaying/dropping the IRQ on that machine can apparently outlast 3 quick-succession retries too. See the new TFS2/ATA write-performance item below, which should shrink this failure's exposure as a side effect (far fewer synchronous round trips per MB written = far fewer chances for one to get caught in a jitter window) even though it's aimed at throughput, not this specifically. If it's still not enough after that, the next step is either more retries with backoff between them (give a longer jitter window time to pass) or raising `DMA_WAIT_TICKS` itself. Run `stress 8192` (or `stress 4200` for a faster but still meaningful pass) whenever a session has the wall-clock time to let it finish; `debug ata on` first to see each retry attempt in `dmesg`.
- [ ] TFS2/ATA write performance -- `stress 100` measured ~1.4MB/s (100MB in 72s), far below what DMA over even legacy IDE should manage. Root cause: `kernel/drivers/ata.c`'s DMA write path (`dma_transfer()`) issues a full synchronous `CMD_CACHE_FLUSH` after *every single write* -- and TFS2 writes in small pieces, so a large write is actually thousands of tiny round trips, not a few big ones: `write_block()` (`kernel/drivers/tfs.c`) does one `ata_write_sectors()` (and therefore one flush) per 4KB filesystem block, and `persist_bitmap_bit()` does a second, separate write-plus-flush per newly-allocated block just to persist one bit of the free-block bitmap. A 100MB write is on the order of 25,600 block writes, each paying full flush latency -- flush is inherently synchronous (forces the emulated drive, and in turn QEMU's backing file on the host disk, to actually commit), so it dominates throughput regardless of how fast the DMA transfer itself is. Also plausibly related to the DMA-retry item just above: fewer synchronous round trips per MB written means fewer chances for a delayed/lost IRQ to land inside one. Not implemented yet -- flagged live while investigating the stress-test slowness, user asked for a documented plan rather than an immediate fix. Candidate approaches, roughly in order of expected impact vs. risk:
  - **Batch the cache flush** -- flush once per `fs_write_range()` call (or once every N blocks) instead of once per 4KB block. Straightforward and likely the single biggest win; the tradeoff is a slightly larger data-loss window on power failure between flushes, same as any real filesystem's write-back cache.
  - **Defer bitmap persistence** -- buffer bitmap bit changes in memory during a multi-block write and persist the touched bitmap sector(s) once at the end, instead of one sector write-plus-flush per single-bit change. Halves the flush count for any write that allocates new blocks (which is most of them).
  - **Coalesce contiguous block writes into fewer, larger ATA commands** -- `write_block()` issues one 8-sector (4KB) command per call even when consecutive blocks in a range are contiguous on disk (the common case for a freshly-allocated file); batching contiguous runs into one larger `ata_write_sectors()` call (up to `ATA_MAX_SECTORS_PER_XFER`, or raising that cap -- a PRD can cover up to 64KB/128 sectors, well above today's 8) cuts per-command overhead (`wait_not_busy()`, `select_lba()`, the DMA setup/IRQ round trip) on top of the flush savings above.
  - **Journal-batched flush** -- rely on the existing journal (`FS_JOURNAL_HEADER_LBA`/`FS_JOURNAL_DATA_LBA`) as the actual durability boundary and flush once per logical operation it protects, rather than once per physical block -- the most invasive option since it touches the crash-safety story directly, worth doing last and carefully.
  Whichever combination gets picked, re-run `stress 100` before/after to get a real before/after throughput number, not just "feels faster" -- same spirit as the benchmarking-harness item elsewhere in this list.
- [ ] A FAT16/FAT32 driver -- real interop with other OSes' tools and USB drives, distinct from the AHCI/SATA item above (that's the controller; this is the on-disk format)
- [ ] ACPI table parsing (RSDP/MADT/FADT) -- also unlocks a real software poweroff (today's `system_reboot()` only resets via the 8042 controller; there's no poweroff at all)
- [ ] APIC + HPET timer, replacing the PIT + remapped 8259 PIC toy-os uses today -- a prerequisite for SMP and for timing finer than the PIT's 100 Hz tick
- [ ] SMP (multi-core) -- large undertaking, and a prerequisite is ACPI/MADT parsing to even discover the other cores
- [ ] Inter-process IPC (message passing) -- today's ring-3 processes are isolated from each other with no way to communicate
- [ ] Virtio drivers (disk/net) -- QEMU's paravirtualized devices, as a modern addition alongside the legacy ATA/e1000 paths already used
- [ ] A benchmarking harness -- so a future change that regresses boot time or a hot path (e.g. `gfx_present()`) gets caught instead of just "feeling" slower
- [ ] Stretch: port a small classic game (e.g. Doom) as an end-to-end stress test of real disk-hosted ELF binaries + libc, once both exist
- [ ] USB support (mice/keyboards via a HID class driver) -- needs a USB host controller driver (UHCI/EHCI/xHCI, found the same way the e1000 NIC already is, via the existing PCI enumeration) before any device can even be enumerated. Low priority: PS/2 already covers mouse/keyboard for every target so far, real hardware and QEMU alike.
- [x] ~~Desktop icons -- double-click to launch an app from the desktop background itself, not just the Start menu~~ -- done (see CHANGELOG.md's `[Unreleased]` entry): `apps/wm/desktop.c`/`.h`, one icon per `gui_app_registry` entry, single-click selects, double-click launches. Real wallpaper images and repositioning/dragging icons are NOT done -- see below.
- [x] ~~Right-click context menus (desktop and window chrome)~~ -- done (see CHANGELOG.md's `[Unreleased]` entry): `apps/wm/context_menu.c`/`.h`, a generic reusable popup, wired into the desktop, window chrome (title bar + content area), taskbar app buttons, and Start menu rows. NOT done: per-icon context menus (right-clicking a desktop icon shows the same quick-launch menu as empty space -- see docs/decisions.md for why), keyboard navigation within an open menu.
- [ ] Real wallpaper images for the desktop background (`apps/wm/desktop.c` currently fills a plain color) -- blocked on the image-decoder item above (JPEG or similar), same dependency that item's own entry already notes for window-chrome visual polish
- [ ] Desktop icon repositioning/dragging -- today's icon grid is a fixed left-edge column derived straight from `gui_app_registry`, no per-icon position state to drag
- [ ] Per-icon desktop context menus (Rename/Properties/etc) -- needs icons to have real per-icon identity/state beyond "which registry index" first; see docs/decisions.md
- [ ] More compositor work beyond today's partial dirty-rect blit -- see "Dirty-rectangle rendering" further down for exactly what's done and what's still a full-scene redraw
- [ ] Basic image support (a JPEG or similar decoder, plus a way to blit a decoded image into the framebuffer) -- also the prerequisite for real GUI/window-chrome visual polish, see `docs/decisions.md` for that discussion
- [x] ~~A reusable file picker dialog (open/save), wired into Notepad first as the initial caller~~ -- done (see `CHANGELOG.md`'s `[Unreleased]` entry): `apps/wm/file_picker.c`/`.h`, full directory navigation (double-click to enter/`../` to go up, directories-first sort, a scrollbar), wired into Notepad as Open.../Save As.... Not done: Esc-to-cancel, creating a directory from inside the dialog, drag-to-scroll/mouse-wheel on the list, hover highlighting.
- [ ] File manager app -- needs a proper filesystem API surface first (list/stat/create/delete as real syscalls or a library layer, not the fixed ad hoc calls the shell uses today), then the app built on top of that
- [ ] Desktop calendar: a small popup panel above the taskbar, opened by clicking the clock, showing a month grid (view-only, no events yet) -- built as a reusable `widget_calendar` piece the same way `widget_scrollback`/`widget_button` are, so any future app can embed it too
- [ ] Control panel window with pluggable "applets" (Windows-style) -- first applet: display settings (font size + color theme), since both already exist as the `fontsize`/`color` shell commands, so the applet is mostly a GUI wrapper around logic that's already implemented and tested
- [x] ~~Shutdown (Start menu item, alongside "Exit to shell")~~ -- done (see `CHANGELOG.md`'s `[Unreleased]` entry): a Yes/No confirm via the now-built `confirm_dialog.h` (its second real caller), then `system_poweroff()` (`kernel/core/power.c`) -- the QEMU/Bochs `outw(0x604, 0x2000)` I/O-port trick, with a halt-and-message fallback if it doesn't take. A real ACPI-based poweroff is still open, blocked on the ACPI table parsing item above -- this is deliberately the "works today in this exact dev/test setup" option, not that one.
- [x] ~~Per-subsystem runtime debug-logging switches~~ -- done (see `CHANGELOG.md`'s `[Unreleased]` entry): `kernel/include/debugflags.h`/`kernel/core/debugflags.c`, off by default, `debug <name> on|off` at the shell (no rebuild). Subsystems today: `fs`, `wm`, `ata`.

The items above (from USB through the control panel) are the user's own working notes, folded in and given dependency context; the nine before them (partition tables through the benchmarking harness) came out of comparing notes with [brutal-org/brutal](https://github.com/brutal-org/brutal)'s roadmap -- a similar-scope hobby OS that's further along on hardware discovery (ACPI/APIC/SMP) and has already made some of the same "controller vs. on-disk-format" and "infra before protocol" calls toy-os is facing for storage and networking. Deliberately left out as out of scope for toy-os: an own bootloader (GRUB is fine here), a self-hosted C compiler, and additional CPU architectures (toy-os is x86-64-only by design, per the project description) -- brutal's roadmap has all three, but they're not goals here.

**Recently done**
- [x] Reusable UI widgets (`apps/widgets.h`: button, scrollback, scrollbar, text field, checkbox)
- [x] Dirty-rectangle rendering -- partial: bounding-box blit + a cheap cursor-only fast path, full per-widget scene dirty-tracking still not done
- [x] GUI terminal-emulator app (`apps/terminal.c`, runs the real shell in a window)
- [x] Scrollbars for Terminal and Notepad (Page Up/Down, draggable thumb, mouse wheel)
- [x] Process exit/teardown -- a faulted or crashed ring-3 process no longer halts the whole kernel

Full detail, reasoning, and CHANGELOG links for every item below.

- ~~Scrollbars for Terminal and Notepad~~ -- done (see CHANGELOG.md's
  builds 263, 273, 283, 293): Page Up/Page Down, a real visual
  draggable scrollbar (click the empty track to page, drag the thumb
  to scroll directly), and the mouse wheel all work in both apps now
  -- Notepad picked up all three for free once it was converted to
  the same `text_scrollback` widget Terminal already used.
- ~~GUI terminal-emulator app~~ -- done (see CHANGELOG.md's builds 183,
  193, 203, and 253 for the finished `apps/terminal.c`). `Terminal` in
  the Start menu runs the real shell dispatcher inside a resizable
  window -- not a reimplementation of it. `gui`/`run`/`ring3test`/
  `schedtest` don't return or draw straight to the physical screen and
  print an explanation instead of running; every other command,
  including every `/bin` binary via `run <name>` from the physical
  shell (they moved off dedicated commands like `elftest`/`guitest`/
  `wintest`/`echotest` -- see `docs/decisions.md`'s ELF64-to-`/bin`
  migration entry), works for real.
- ~~Process exit/teardown so a faulted or crashed ring-3 process doesn't
  halt the whole kernel~~ -- done (see CHANGELOG.md's "process
  exit/teardown" entry, `crashtest`). `ring3test` still requires a
  reboot after its deliberate fault, but on purpose now, not for lack
  of a recovery path -- it drops to ring 3 with its own raw, manual
  iretq instead of `process_run_ring3()`, so there's nowhere for the
  kernel to recover it TO (see `process.h`). `elftest`/`hello.elf` used
  to be the other example here until it folded into the recoverable
  `run hello` path (see `docs/decisions.md`).
- `g_next_kernel_rsp`'s reentrancy fixed properly (see the
  `echotest`/`SYS_READ_KEY` entry in CHANGELOG.md) so a genuinely
  blocking read -- or any syscall that wants interrupts on while it
  runs -- becomes safe, instead of every blocking-style syscall having
  to spin-poll from ring 3 like `echotest` does today
- `fs_write()` only supports whole-string append/overwrite (no
  offset-based partial writes, no explicit length -- see `filetest`'s
  entry in CHANGELOG.md); a real libc's `fwrite()` would eventually
  want that
- Toward a real C library on top of `filetest`'s fd-aware syscalls: a
  CRT0 that sets up argc/argv from the initial stack, TLS (FS.base)
  support, and FPU/SSE context-switch save/restore -- none of which
  exist yet (see the `filetest` entry in CHANGELOG.md for the full
  breakdown)
- Making `wintest` (`SYS_WIN_CREATE`/`SYS_WIN_PRESENT`) non-modal: a
  `SYS_WIN_*` window is still handed the CPU exclusively while it runs,
  same limitation `guitest` always had, and it isn't a window inside
  the kernel-space window manager's (`wm.c`) own window list. This
  needs the scheduler to give the kernel-space WM loop and a scheduled
  ring-3 process fair turns -- `scheduler_tick()` currently only
  resumes kernel-space code when nothing is `READY`, and once any
  process is armed, kernel-space code doesn't get scheduled again until
  every process exits (see the CHANGELOG entry on why this stayed
  modal). Also: mouse input isn't piped to ring 3 at all yet, so
  `wintest`'s close button is drawn but not clickable.
- ~~Reusable UI widgets -- buttons, text fields, scrollbars -- so GUI
  apps don't each hand-roll their own drawing and hit-testing~~ -- done
  (see CHANGELOG.md's builds 263-283/377 for the scrollback/scrollbar
  widgets and build 490 for the text field/checkbox): `apps/widgets.h`
  now has `widget_button`, `widget_scrollback_*` (a full scrolling text
  area, cursor-aware editing included), `widget_scrollbar_*`,
  `widget_textfield_*` (single-line editable text, first used by
  Notepad's filename field), and `widget_checkbox_*` (built ahead of a
  real caller -- see build 490's CHANGELOG entry for why). Notepad's
  toolbar/Save/Load buttons already went through `widget_button`
  before this, so this note was stale by the time build 490 landed;
  what's left unwidgeted is smaller things nothing has needed twice
  yet (radio-button-style exclusivity, a dropdown/list, a progress
  bar) -- add the next one only once a second real caller shows up,
  same philosophy `widgets.h`'s own top comment states.
- ~~Dirty-rectangle rendering instead of the current full-screen repaint.
  Double buffering removed the flicker, but each frame still redrew
  everything and blit the whole screen, which was a lot of wasted work
  when only the cursor moved.~~ -- partially done (see CHANGELOG.md's
  build 337 entry): `gfx_present()` now blits only the bounding box of
  what actually changed instead of the whole screen, and mouse-only
  movement (by far the most common case) takes a cheap cursor-sprite
  save/restore path that skips the full window/taskbar/menu redraw
  entirely. Scene redraws (a click, a drag, a resize, a window opening)
  still repaint the whole back buffer, same as before -- true per-widget
  dirty tracking of the *scene itself*, not just the blit, is still the
  "Full dirty-rect compositor" option that was deliberately not taken
  here (see that build's own reasoning).
- Notepad's filename is currently fixed (`notepad.txt`) -- a simple text
  input widget would let you save/load under different names
- The persistent filesystem's on-disk layout (see CHANGELOG.md) is a
  custom fixed-32-file-slot format ("TFS2" as of build 480 -- see
  `docs/tfs2-spec.md` for the byte-exact spec, now including a
  write-ahead journal and created/modified timestamps), and still not
  a format any *other* OS's tools read natively. `docs/tfs2-spec.md`
  closes part of that gap -- a host-side tool can now parse a TFS2
  image directly (a reference read-only Python parser is in that
  spec) without needing FAT16-or-similar compatibility -- but it's
  still TFS2-specific, not a real standard like FAT16 an off-the-shelf
  tool would already understand. AHCI/SATA support would be a
  different direction entirely -- real modern hardware increasingly
  lacks the legacy IDE controller `ata.c` currently depends on.
- The VFS layer (see CHANGELOG.md's build 304, `kernel/include/fs_ops.h`)
  supports exactly one active filesystem backend at a time, chosen once
  at boot -- not multiple backends mounted simultaneously at different
  path prefixes. A real mount-point scheme (`/` on one backend, `/data`
  on another, say) is the natural next step if a second filesystem ever
  actually shows up and needs to coexist with the first, rather than
  replace it; deferred until then since it's meaningfully more code
  (cross-mount path resolution, boundary conflicts) for a capability
  nothing needs yet.
- Basic TCP/IP networking. A large addition, comparable in scope to the
  filesystem or window manager -- not a small feature, and it needs
  four pieces of infrastructure that have zero precedent in this
  kernel today, not just a new driver:
  - ~~**PCI bus enumeration**~~ -- done (see CHANGELOG.md's build 390):
    brute-force `0xCF8`/`0xCFC` config-space scanning (`kernel/drivers/
    pci.c`, `lspci` shell command), confirmed against QEMU's default
    topology -- including the e1000 NIC (`8086:100e`, IRQ 11) this
    whole networking effort is ultimately aimed at.
  - ~~**A real IRQ-handler registration mechanism**~~ -- done (see
    CHANGELOG.md's build 400): `isr_dispatch()` (`kernel/core/idt.c`)
    now dispatches every hardware IRQ through one generic table
    (`irq_register_handler()`/`irq_dispatch()`, `kernel/core/irq.c`)
    instead of a hardcoded if/else chain -- timer, keyboard, and mouse
    all migrated to it, automatic PIC EOI, no chaining/sharing (one
    handler per line, since QEMU's topology gives every device its own
    line). A NIC driver registers the same way.
  - ~~**Contiguous/DMA-friendly physical memory**~~ -- done (see
    CHANGELOG.md's build 410): `pmm_alloc_contiguous(count)`/
    `pmm_free_contiguous(phys_addr, count)` (`kernel/core/pmm.c`) hand
    out/return a run of N physically contiguous 4KB frames via a linear
    scan of the same bitmap `pmm_alloc_frame()` uses -- no new data
    structure. The kernel identity-maps the low 4GB already (see
    `vmm.c`), so no address-translation headache once contiguous frames
    exist. Deliberately minimal: a linear scan is fine for a rare,
    not-hot-path call (a driver setting up a descriptor ring once at
    init); if fragmentation from other allocations ever made long runs
    hard to find, replacing the whole bitmap allocator with a
    buddy/segregated-free-list allocator is the standard fix -- flagged
    here as a future improvement, not built now, since nothing in this
    kernel has exercised the bitmap enough yet to know fragmentation is
    a real problem worth that added complexity.
  - ~~**A socket-like fd abstraction + new syscalls**~~ -- fd/syscall
    surface done (see CHANGELOG.md's build 420); real transport still
    doesn't exist. `syscall.c`'s fd table is now a tagged union
    (`FD_KIND_FILE`/`FD_KIND_SOCKET`) sharing one namespace, and
    `SYS_SOCKET`/`SYS_SEND`/`SYS_RECV` exist and are reachable --
    `SYS_SOCKET` allocates a real socket fd (domain/type reserved for
    future use, must be 0 for now), `SYS_SEND`/`SYS_RECV` always return
    -1 ("no transport yet," deliberately, not a bug), and
    `SYS_WRITE`/`SYS_READ` correctly reject a socket fd. This is
    scaffolding ahead of the actual driver, not a working socket --
    see `sockettest`, which proves exactly this surface and nothing
    more.
  - ~~**A real IRQ-driven DMA transfer example**~~ -- done (see
    CHANGELOG.md's build 470): `ata.c` now does genuine Bus-Master DMA
    (`pmm_alloc_contiguous()`-backed PRDT + bounce buffer) with
    IRQ14-signaled completion, falling back to the original PIO path
    automatically if DMA can't be stood up. This is the reference
    example a NIC driver's own RX/TX ring would follow, and it also
    produced two reusable, general-purpose pieces any future
    DMA-capable driver needs: `pci_enable_bus_master()`
    (`kernel/drivers/pci.c`/`.h` -- a device's DMA control registers
    can report success while moving no real data without this bit
    set, see `docs/decisions.md`) and `isr_in_progress()`/
    `isr_reset_depth()` (`kernel/include/idt.h` -- lets a driver
    genuinely `hlt`-block waiting for an IRQ when it's safe to, and
    fall back to polling the device's own status bit when called from
    inside a syscall, without reintroducing the `g_next_kernel_rsp`
    reentrancy bug -- see `docs/decisions.md`).
  - Smaller gap: there's a tick counter (`pit_ticks()`) but no sleep/
    delay primitive -- TCP needs timeouts and retransmission timers.
  Realistic path, if taken: PCI enum -> pick a simple NIC to target
  (QEMU's `rtl8139` emulation is the classic "easy first NIC driver"
  choice, much simpler than e1000/virtio-net) -> IRQ registration -> a
  minimal Ethernet/ARP/IP/UDP stack before ever touching TCP (TCP's
  state machine and retransmission logic only makes sense once packets
  can reliably get in and out) -> TCP + the socket syscalls. The
  driver-integration pattern itself is in good shape to build on --
  `ata.c`'s probe/init-function/capability-header-through-`kapi.h`
  structure, now including a working IRQ-driven Bus-Master DMA path
  (see above), is a reasonable template, and the existing `*_test.c`
  diagnostic pattern (`echo_test.c` etc.) is a natural fit for early
  loopback/ARP verification -- but this is its own multi-session
  project with its own milestones, not a single build bump.
- ~~Real disk-hosted ELF binaries -- an executable a user could drop
  into a `/bin` directory and have `run`/the shell actually load and
  execute from the persistent filesystem, instead of every `.elf`
  today being a GRUB Multiboot2 module baked into the ISO~~ -- done
  (see `CHANGELOG.md`'s `[Unreleased]` entry). The planning pass
  originally recorded here (two builds, (A) syscall-based ELF then (B)
  loading it from `/bin`) turned out smaller than expected once
  re-checked against the current codebase: (B)'s stated hard
  prerequisite -- TFS2 capping a file at `FS_DATA_MAX` = 2048 bytes --
  was already gone by the time this was picked back up (TFS2 v2's
  block-addressed rework removed it as a side effect, not this change;
  see `kernel/include/fs.h`'s corrected `FS_DATA_MAX` comment), and
  `elf_load()`'s ELF blob turned out to need no separate scratch-buffer
  copy at all (`fs_read()`'s `kmalloc()` buffer is already in the same
  identity-mapped low-4GiB range a GRUB module lives in -- see
  `docs/decisions.md`). Both (A) (`SYS_PCI_COUNT`/`SYS_PCI_INFO`,
  `userland/lspci.c`) and (B) (`kernel/core/elf_run.c`'s
  `elf_run_from_fs()`, wired into the shell's `run` command) shipped
  together as a result. What's still open, deliberately deferred: a
  real host-side TFS2 writer tool -- `/bin/lspci` is installed once at
  boot by copying its GRUB module's bytes in (`kernel_main()`'s
  `install_bin_binaries()`), which means a *second* disk-hosted binary
  still needs its own kernel rebuild + bootstrap-table row rather than
  being dropped onto `disk.img` from outside the OS entirely.
