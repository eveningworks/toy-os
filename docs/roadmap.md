# Roadmap / Ideas for what's next

Forward-looking "not built yet" items, moved out of README.md to keep that file focused on "what toy-os can do today." Completed items are struck through and linked to the CHANGELOG build that finished them --
this list is actively maintained, not a stale wishlist. See `docs/decisions.md` for *why* existing things are built the way they are, and `CHANGELOG.md`/`CHANGELOG-archive.md` for the full history.

## At a glance

**In progress / up next**
- [ ] Migrate Notepad's Save/Load buttons to `ui_button_group` (`apps/ui/ui_button.c`/`ui_button_group.c`) -- not blocked on anything (Calculator is already a real second-caller-worthy precedent), deliberately left out of that change because it also tightens Save/Load's click hitbox to match the drawn button exactly, a small behavior change that didn't belong bundled into a no-behavior-change refactor. See `docs/decisions.md`. (Notepad's filename field itself was separately migrated to `ui_textbox` -- see `CHANGELOG.md`'s `[Unreleased]` entry -- this item is only about the Save/Load buttons.)
- [ ] Multi-architecture support (e.g. RISC-V 64 alongside x86_64) -- assessed, not started: roughly a tenth of the codebase is architecture-specific and it's already well-insulated behind `kapi.h`, but boot/interrupts/paging/port-I/O are a real per-arch project. Full breakdown, proposed `kernel/arch/<arch>/` layout, and a phased plan in `docs/arch-portability.md`
- [ ] Real disk-hosted ELF binaries -- load from `/bin` at runtime instead of every `.elf` being a GRUB module baked into the ISO (investigated, not started -- see the full A/B breakdown below)
- [ ] TCP/IP networking -- the infra pieces are done (PCI enumeration, IRQ registration, contiguous/DMA memory, the socket/fd syscall surface, IRQ-driven DMA example); no NIC driver or protocol stack yet

**Backlog**
- [ ] `wintest` (`SYS_WIN_*`) windows made non-modal, sharing scheduler time with the kernel-space window manager instead of taking the CPU exclusively
- [ ] `g_next_kernel_rsp` reentrancy fixed properly, so a real blocking syscall doesn't need to spin-poll from ring 3 the way `echotest` does today
- [x] ~~`fs_write()` offset-based partial writes~~ -- done, see CHANGELOG.md's TFS2 multi-GB rework entry (`Unreleased`): `fs_write_range(path, offset, buf, len)` / `fs_read_range()` now exist alongside the original whole-file `fs_write()`/`fs_read()` (kept as-is for small-file callers like Notepad/shell/editor.c).
- [ ] A real C library on top of `filetest`'s fd-aware syscalls: CRT0 (argc/argv), TLS (FS.base), FPU/SSE context-switch save/restore
- [ ] Notepad: editable filename (currently fixed to `notepad.txt`)
- [ ] VFS: multiple filesystem backends mounted at once, not just one chosen at boot
- [ ] AHCI/SATA driver -- today's `ata.c` depends on the legacy IDE controller real modern hardware increasingly lacks
- [ ] GPT/MBR partition table parsing -- `disk.img` is one raw TFS2 blob today, not a partitioned disk. Explicitly re-confirmed as a "build it anyway, later" item when TFS2 was reworked for multi-GB files (this session): not required for large-file support (TFS2 v2's own block addressing handles that), purely for future flexibility (e.g. hosting more than one filesystem image on one disk).
- [ ] Full end-to-end multi-GB (e.g. 8GB) file write/read stress test over TFS2 v2 -- the new block-addressed on-disk format (12 direct + single/double/triple indirect blocks) was verified via a boot-time self-test that proves triple-indirect pointer construction/traversal at a ~4.6GB offset, plus real small-file read/write/persistence-across-reboot, but a genuine full-size 8GB write/read pass hasn't been run yet (would take a long time over emulated PIO/DMA in a headless sandbox). Worth doing once real multi-GB files (e.g. actual video) are on hand to test with.
- [ ] A FAT16/FAT32 driver -- real interop with other OSes' tools and USB drives, distinct from the AHCI/SATA item above (that's the controller; this is the on-disk format)
- [ ] ACPI table parsing (RSDP/MADT/FADT) -- also unlocks a real software poweroff (today's `system_reboot()` only resets via the 8042 controller; there's no poweroff at all)
- [ ] APIC + HPET timer, replacing the PIT + remapped 8259 PIC toy-os uses today -- a prerequisite for SMP and for timing finer than the PIT's 100 Hz tick
- [ ] SMP (multi-core) -- large undertaking, and a prerequisite is ACPI/MADT parsing to even discover the other cores
- [ ] Inter-process IPC (message passing) -- today's ring-3 processes are isolated from each other with no way to communicate
- [ ] Virtio drivers (disk/net) -- QEMU's paravirtualized devices, as a modern addition alongside the legacy ATA/e1000 paths already used
- [ ] A benchmarking harness -- so a future change that regresses boot time or a hot path (e.g. `gfx_present()`) gets caught instead of just "feeling" slower
- [ ] Stretch: port a small classic game (e.g. Doom) as an end-to-end stress test of real disk-hosted ELF binaries + libc, once both exist
- [ ] USB support (mice/keyboards via a HID class driver) -- needs a USB host controller driver (UHCI/EHCI/xHCI, found the same way the e1000 NIC already is, via the existing PCI enumeration) before any device can even be enumerated. Low priority: PS/2 already covers mouse/keyboard for every target so far, real hardware and QEMU alike.
- [ ] Desktop icons -- double-click to launch an app from the desktop background itself, not just the Start menu
- [ ] Right-click context menus (desktop and window chrome) -- needs a small reusable popup-menu widget; same rendering/hit-testing pattern the existing `widgets.h` pieces already use
- [ ] More compositor work beyond today's partial dirty-rect blit -- see "Dirty-rectangle rendering" further down for exactly what's done and what's still a full-scene redraw
- [ ] Basic image support (a JPEG or similar decoder, plus a way to blit a decoded image into the framebuffer) -- also the prerequisite for real GUI/window-chrome visual polish, see `docs/decisions.md` for that discussion
- [ ] A reusable file picker dialog (open/save), wired into Notepad first as the initial caller
- [ ] File manager app -- needs a proper filesystem API surface first (list/stat/create/delete as real syscalls or a library layer, not the fixed ad hoc calls the shell uses today), then the app built on top of that
- [ ] Desktop calendar: a small popup panel above the taskbar, opened by clicking the clock, showing a month grid (view-only, no events yet) -- built as a reusable `widget_calendar` piece the same way `widget_scrollback`/`widget_button` are, so any future app can embed it too
- [ ] Control panel window with pluggable "applets" (Windows-style) -- first applet: display settings (font size + color theme), since both already exist as the `fontsize`/`color` shell commands, so the applet is mostly a GUI wrapper around logic that's already implemented and tested
- [ ] Shutdown (Start menu item, alongside "Exit to shell") -- deliberately not built this round; the design decisions that *are* settled: it needs a Yes/No confirm first, and that confirm dialog should be a reusable `widget_confirm_dialog` piece (message + two callbacks) once it's actually built, not one-off code in `wm.c`. What's still open is the power-off mechanism itself -- a QEMU/common-VM I/O-port trick (`outw(0x604, 0x2000)` etc., works today in this exact dev/test setup) vs. a real ACPI-based poweroff (blocked on the ACPI table parsing item above) vs. a plain halt-and-message screen as the simplest always-correct fallback.

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
  window -- not a reimplementation of it. A short list of commands that
  don't return or draw straight to the physical screen (`gui`, `run`,
  `ring3test`, `elftest`, `guitest`, `wintest`, `schedtest`, `echotest`)
  print an explanation instead of running; everything else, including
  the ring-3 test commands, works for real.
- ~~Process exit/teardown so a faulted or crashed ring-3 process doesn't
  halt the whole kernel~~ -- done (see CHANGELOG.md's "process
  exit/teardown" entry, `crashtest`). `ring3test`/`elftest` still
  require a reboot after their deliberate fault, but on purpose now,
  not for lack of a recovery path -- they drop to ring 3 with their own
  raw, manual iretq instead of `process_run_ring3()`, so there's
  nowhere for the kernel to recover them TO (see `process.h`).
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
- Real disk-hosted ELF binaries -- an executable a user could drop
  into a `/bin` directory and have `run`/the shell actually load and
  execute from the persistent filesystem, instead of every `.elf`
  today being a GRUB Multiboot2 module baked into the ISO at build
  time (see `apps/README.md` and `multiboot.c`'s `multiboot_get_
  module()` for how that currently works) and found by a hardcoded
  module index. `lspci` was the proposed first candidate, since it's
  a natural "small, self-contained, easy to verify" first real binary
  -- currently it's a plain kernel-space shell built-in
  (`cmd_lspci()` in `apps/shell_sys.c`, calling `pci_device_at()`/
  `pci_class_name()` directly), not a process at all.

  Investigated (no code changes yet -- this is a planning pass, at the
  user's explicit request, before committing to an implementation).
  Two genuinely separate capabilities are bundled up in "support ELF
  binaries from the filesystem," worth landing as two builds rather
  than one:

  - **(A) A real syscall-based ELF program, launched the existing
    (GRUB-module) way.** Mechanically this is the easy half --
    `elf_load()`/`process_run_ring3()` already don't care where the
    ELF blob came from, and a new `*_test.c`-style harness could load
    an `lspci.elf` from a Multiboot2 module exactly like `elftest`/
    `filetest` do today. The real gap: a ring-3 process can only reach
    the kernel through the `int 0x80` syscall table
    (`kernel/include/syscall_abi.h`) -- it can't call `pci_device_at()`
    directly the way kernel-space shell code can, so this needs new
    syscalls (something like `SYS_PCI_COUNT`/`SYS_PCI_INFO`) added the
    same way `SYS_LISTDIR`/`SYS_GETTIME` were for `newsyscalltest`
    (build 420-adjacent). This alone would prove out "a real syscall-
    driven userland program that does something other than file I/O,"
    independent of the filesystem-loading question below.
  - **(B) Loading that binary from `/bin` on the persistent disk at
    runtime**, once (A) exists. This is the harder half, and has its
    own real prerequisite: TFS2's on-disk record format
    (`kernel/drivers/tfs.c`, `docs/tfs2-spec.md`) caps a single file
    at `FS_DATA_MAX` = 2048 bytes today, and every existing test ELF
    (1112-3320 bytes) already brushes or exceeds that. Discussed three
    ways to fix this and settled on **multi-slot chaining for large
    files only**: an ordinary small file keeps today's exact
    2048-byte/one-slot footprint (both on disk and in the in-RAM
    `files[FS_MAX_FILES]` table -- the other two options either bloat
    every one of the 32 slots' static RAM cost by the same amount
    regardless of whether that slot is ever used for something big
    [simply growing `FS_DATA_MAX`], or add an entirely separate
    fixed-size table just for binaries alongside the existing one [a
    dedicated "binaries region"]), while a file that needs more spans
    multiple slots via a chain -- more on-disk format complexity (a
    "next slot" pointer, `tfs2-spec.md` would need updating and its
    reference Python parser would need to follow chains), but no
    wasted RAM or disk for the common case of small text files.
    Besides the format change, (B) also needs: a `fs_read()`-sourced
    load path (`elf_load()` already accepts a flat blob, so this is
    mostly wiring, but the blob would need copying out of TFS2's live
    in-RAM table into a scratch buffer before executing it, rather
    than executing in place, since that memory isn't stable the way a
    GRUB module's reserved region is); a `/bin` + `run` convention
    (the shell's `run <name>` only checks a small in-kernel function
    table today -- see `apps/apps.c`); and, since there's no in-guest
    compiler, some way to actually get a built ELF's bytes onto
    `disk.img` in the first place (most likely a host-side tool built
    on the same byte-exact TFS2 writer logic `docs/tfs2-spec.md`'s
    reference reader already demonstrates reading, or an "install from
    the GRUB module into `/bin` on first boot" bootstrap step).

  Not started -- this entry is the plan, for whenever (A) and then (B)
  actually get picked up as their own builds.
