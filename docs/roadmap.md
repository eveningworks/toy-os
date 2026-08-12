# Roadmap

Forward-looking "not built yet" items only -- what's already built lives in
`CHANGELOG.md`/`CHANGELOG-archive.md` (full history) and `README.md` (what
toy-os can do today), not here. See `docs/decisions.md` for *why* existing
things are built the way they are. This list is always subject to change --
milestones get reordered/reshaped as work actually happens, they're a plan,
not a promise.

Style note (inspired by [brutal-org/brutal](https://github.com/brutal-org/brutal)'s
roadmap): each milestone is just checkboxes and a few words. Full reasoning,
phased test plans, and cross-references for every item are in **Details**
at the bottom of this file, organized the same way.

## Milestones

### Milestone 1 -- Async I/O to the desktop

- [x] Non-blocking DMA start/poll primitive
- [x] Steppable write API
- [ ] Wire it up: `wm_run()` polls a pending write (Notepad Save first)
- [ ] Generalize to reads + the plain shell prompt
- [ ] Async process spawning for the GUI Terminal

### Milestone 2 -- Storage hardening

- [ ] Full multi-GB stress run (`stress 4200` / `stress 8192`)
- [ ] Coalesce contiguous block writes into fewer ATA commands
- [ ] Journal-batched flush
- [ ] GPT/MBR partition table parsing

### Milestone 3 -- AHCI/SATA driver

- [ ] PCI discovery + ABAR mapping
- [ ] Port detection
- [ ] Bring up one port
- [ ] IDENTIFY DEVICE (polled)
- [ ] IRQ-driven read
- [ ] IRQ-driven write + `ata.c` parity
- [ ] Multi-sector transfers (PRDT scatter-gather)
- [ ] Backend selection + fallback

### Milestone 4 -- Desktop visual polish

- [ ] Basic image decoder (JPEG or similar)
- [ ] Real wallpaper images
- [ ] Desktop icon repositioning/dragging
- [ ] Per-icon context menus (Rename/Properties)
- [ ] Full dirty-rect compositor

### Milestone 5 -- Desktop productivity apps

- [ ] Real filesystem API surface (list/stat/create/delete)
- [ ] File manager app
- [ ] Desktop calendar widget
- [ ] Control panel with pluggable applets

### Milestone 6 -- ACPI + real power/timer

- [ ] ACPI table parsing (RSDP/MADT/FADT)
- [ ] Real ACPI-based poweroff
- [ ] APIC + HPET timer (replacing PIT + 8259 PIC)

### Milestone 7 -- SMP (multi-core)

- [ ] Discover other cores via MADT
- [ ] Bring up application processors (INIT-SIPI-SIPI)
- [ ] Per-core GDT/IDT/stack
- [ ] Scheduler aware of multiple cores

### Milestone 8 -- USB (keyboard/mouse)

- [ ] Host controller discovery
- [ ] Bring up xHCI
- [ ] Root port + device detection
- [ ] Control transfers + enumeration
- [ ] HID boot-protocol interrupt transfers
- [ ] Keyboard integration
- [ ] Mouse integration
- [ ] Legacy PS/2 handoff

### Milestone 9 -- Networking

- [ ] NIC driver (rtl8139 first)
- [ ] Sleep/delay primitive (timeouts, retransmission)
- [ ] Ethernet/ARP/IP/UDP stack
- [ ] TCP + wire up the existing socket syscalls

### Milestone 10 -- Runtime + interop

- [ ] Inter-process IPC (message passing)
- [ ] Real C library (CRT0, TLS, FPU/SSE)
- [ ] FAT16/FAT32 driver
- [ ] `g_next_kernel_rsp` reentrancy fixed properly
- [ ] `wintest` made non-modal

## Backlog

Smaller or lower-priority items not yet slotted into a milestone above.

- [ ] VFS: multiple filesystem backends mounted at once
- [ ] Virtio drivers (disk/net)
- [ ] A benchmarking harness
- [ ] Multi-architecture support (RISC-V) -- see `docs/arch-portability.md`
- [ ] Stretch: port a small classic game (e.g. Doom) once disk-hosted ELF + libc exist

---

## Details

Full reasoning, phased test plans, and CHANGELOG/decisions.md pointers for
every item above, in the same order.

### Milestone 1 -- Async I/O to the desktop

Apps run in kernel space, so a disk write from Notepad's Save, the shell's
`stress`, or a future Terminal command all sit inside the same call stack
`wm_run()`'s event loop is also on -- a slow write freezes the whole
desktop, not just the operation. Broken into phases, each independently
testable:

1. [x] ~~Non-blocking DMA start/poll primitive~~ -- done (see `CHANGELOG.md`'s
   `[Unreleased]` entry): `dma_transfer_start()`/`dma_transfer_poll()`
   (`kernel/drivers/ata.c`/`ata.h`), built from the same `dma_issue()`/
   `dma_finish()` halves the existing blocking `dma_transfer()` uses, so the
   blocking path is unchanged. No real caller yet -- proven standalone via
   the `dmatest [lba]` shell command (read-only, byte-compares a blocking
   read against the non-blocking one, reports poll count).
2. [x] ~~Steppable write API~~ -- done (see `CHANGELOG.md`'s `[Unreleased]`
   entry): `fs_write_range_begin()`/`fs_write_range_step()` (`fs.h`,
   dispatched through `fs_ops.h`/`vfs.c` to `tfs.c`'s
   `tfs_write_range_begin()`/`_step()`), built from the same
   `write_range_one_block()` helper `write_range_impl()` uses, so the
   existing blocking path is unchanged. No real caller yet -- proven
   standalone via the `steptest <mb>` shell command (writes via an explicit
   step loop the command drives itself, verifies byte-for-byte against
   readback, reports step count).
3. Wire up one real caller -- `wm_run()` polls a pending write once per
   frame instead of calling `fs_write_range()` and blocking; Notepad's Save
   becomes the first non-blocking caller. Test: QMP -- start a large Save,
   move the mouse/click another window mid-save, screenshot proving the
   desktop kept redrawing instead of freezing.
4. Generalize to reads and to the plain (non-GUI) shell prompt; consider
   sharing plumbing with the Terminal async-spawn item below.

Separately, async/continuously-armed process spawning for the GUI Terminal,
so `run`/`ls`/any future `/bin` binary can execute from inside
`apps/terminal.c` instead of being wholesale-blocked (`BLOCKED_CMDS`).
Deliberately deferred, not started: scoped during `ls`'s migration to a
real `/bin` binary when the user was shown the real cost and explicitly
chose to ship `ls` now, blocked in the Terminal same as `run`, rather than
build this first. The blocker is architectural, not a small fix:
`elf_run_from_fs()`/`process_run_ring3_args()` (`kernel/core/elf_run.c`,
`kernel/core/process.c`) is synchronous and blocking by design -- it
doesn't return to its caller until the ring-3 process exits or faults --
and `wm_run()` is a plain, uninterrupted kernel-space event loop, never
itself scheduler-managed. Running a `/bin` binary from inside a Terminal
window without freezing the whole desktop needs: a new public spawn API
distinct from today's blocking one, the scheduler continuously armed
(today it's demo-only, disarmed outside `schedtest`) so a spawned process
can be polled/stepped rather than run to completion in one call,
`wm_run()`'s event loop restructured to poll a running background process
alongside its existing input/redraw work, and new per-window "process
running" state in `apps/terminal.c` (output streaming into the window's
scrollback as it arrives, not all at once at exit). Several existing
`/bin` binaries (`gui_test`, `win_test`, `echo_test`) would still need
individual hazard fixes on top of this (never-exits, draws straight to
the framebuffer bypassing the window, wants real concurrency) even once
the core mechanism exists. Shares its root cause and likely some plumbing
with the async I/O phases above (both need something steppable instead of
blocking), but scoped separately since this one's about process
scheduling, not I/O completion.

### Milestone 2 -- Storage hardening

Full end-to-end multi-GB (e.g. 8GB) file write/read stress test over TFS2
v2 -- the on-disk format itself is no longer the blocker: a real,
on-demand `stress <mb>` shell command exists (`apps/shell_sys.c`) that
writes/reads/byte-for-byte-verifies genuine (non-sparse) data, and was
verified correct at `stress 400` on real hardware with zero DMA retries
needed, scaling linearly at ~3.5MB/s (28s/55s/85s/115s for 100/200/300/400
MB). At that rate `stress 4200` is roughly 20 minutes and the full
`stress 8192` roughly 40 minutes -- both plausible for a single session to
let finish, not attempted at that scale yet. Run either whenever a session
has the wall-clock time; `debug ata on` first if it's ever worth
double-checking retries stay at zero.

TFS2/ATA write performance, part 2 (part 1 -- flush batching + deferred
bitmap persistence -- already shipped, see `CHANGELOG.md`): coalesce
contiguous block writes into fewer, larger ATA commands -- `write_block()`
(`kernel/drivers/tfs.c`) issues one 8-sector (4KB) command per call even
when consecutive blocks in a range are contiguous on disk (the common case
for a freshly-allocated file); batching contiguous runs into one larger
`ata_write_sectors()` call (up to `ATA_MAX_SECTORS_PER_XFER`, or raising
that cap -- a PRD can cover up to 64KB/128 sectors, well above today's 8)
would cut per-command overhead on top of the flush-batching already done.
Also: journal-batched flush -- rely on the existing journal
(`FS_JOURNAL_HEADER_LBA`/`FS_JOURNAL_DATA_LBA`) as the actual durability
boundary for metadata and flush once per logical operation it protects
rather than once per physical block -- the most invasive option since it
touches the crash-safety story directly (see `docs/decisions.md`'s entry
on why `persist_record()` was deliberately left alone in part 1), worth
doing last and carefully if ever needed.

GPT/MBR partition table parsing -- `disk.img` is one raw TFS2 blob today,
not a partitioned disk. Explicitly re-confirmed as a "build it anyway,
later" item when TFS2 was reworked for multi-GB files: not required for
large-file support (TFS2 v2's own block addressing handles that), purely
for future flexibility (e.g. hosting more than one filesystem image on one
disk).

### Milestone 3 -- AHCI/SATA driver

Today's `ata.c` depends on the legacy IDE controller real modern hardware
increasingly lacks. Moderate step up from `ata.c`, not a new paradigm:
still "one drive, DMA + interrupt," just MMIO-based (mapped ABAR, BAR5)
instead of fixed I/O ports, with a richer per-port command/FIS format
instead of `select_lba()`'s register writes. Reuses existing
infrastructure throughout (`pci.c` enumeration, `pmm_alloc_contiguous()`
for the command list/FIS-receive area, `irq_register_handler()` for
completion). Broken into steps, each with its own pass/fail so a session
can stop at any boundary with something real proven:

1. PCI discovery + ABAR mapping -- find the controller (class 0x01,
   subclass 0x06), map BAR5's MMIO region, read the HBA's
   capability/global registers. Test: `lspci`-adjacent output shows the
   controller found, with version and implemented-port-count fields read
   back correctly.
2. Port detection -- enumerate implemented ports (PI register), read each
   one's SIG/SSTS.DET to find which actually have a drive attached. Test:
   a diagnostic command lists detected ports and per-port drive presence,
   matching whatever QEMU was launched with (`-device ahci` + an attached
   drive).
3. Bring up one port -- allocate its command list + FIS-receive area,
   initialize the structures, start the port's command engine. Test: no
   hang/crash; the port's PxCMD register reads back "running" per the
   AHCI spec.
4. IDENTIFY DEVICE, polled (no IRQ yet) -- issue the first real command (a
   Register FIS wrapping ATA IDENTIFY), poll for completion, parse
   capacity out of the response. Test: a shell command prints the drive's
   real model string and capacity.
5. IRQ-driven read -- wire up the port's interrupt, issue a 48-bit READ
   DMA EXT, confirm completion via IRQ instead of polling. Test: read a
   known sector (e.g. the TFS2 superblock) and byte-compare against the
   same sector read via legacy `ata.c`.
6. IRQ-driven write + read/write parity with `ata.c`'s public API -- same
   shape as `ata_read_sector()`/`ata_write_sector()` so `tfs.c` doesn't
   need to change to use either backend. Test: TFS2's own boot self-test
   (`fs: selftest passed`) passes when routed through the AHCI path.
7. Multi-sector transfers + PRDT scatter-gather sized to a whole 4KB TFS2
   block in one command (matching `FS_BLOCK_SECTORS`). Test: `stress <mb>`
   passes end-to-end over the AHCI path, including the write-batching from
   Milestone 2 -- confirms the new driver behaves correctly under real
   sustained load, not just a handful of manual reads.
8. Backend selection + fallback -- `fs.c` prefers AHCI when a controller's
   found at boot, falls back to legacy IDE, then RAM-only, same fallback
   spirit `fs.c` already has. Test: boot once against a legacy-IDE-only
   QEMU invocation and once against an AHCI one, confirm `dmesg` shows the
   correct backend chosen each time.

### Milestone 4 -- Desktop visual polish

Basic image support (a JPEG or similar decoder, plus a way to blit a
decoded image into the framebuffer) -- the prerequisite for real wallpaper
images and window-chrome visual polish, see `docs/decisions.md` for that
discussion.

Real wallpaper images for the desktop background (`apps/wm/desktop.c`
currently fills a plain color) -- blocked on the image decoder above.

Desktop icon repositioning/dragging -- today's icon grid is a fixed
left-edge column derived straight from `gui_app_registry`, no per-icon
position state to drag.

Per-icon desktop context menus (Rename/Properties/etc) -- needs icons to
have real per-icon identity/state beyond "which registry index" first;
see `docs/decisions.md`.

More compositor work beyond today's partial dirty-rect blit: `gfx_present()`
blits only the bounding box of what actually changed and mouse-only
movement takes a cheap cursor-sprite save/restore path, but a scene redraw
(click, drag, resize, window opening) still repaints the whole back
buffer -- true per-widget dirty tracking of the scene itself, not just the
blit, is still open.

### Milestone 5 -- Desktop productivity apps

File manager app -- needs a proper filesystem API surface first
(list/stat/create/delete as real syscalls or a library layer, not the
fixed ad hoc calls the shell uses today), then the app built on top of
that.

Desktop calendar: a small popup panel above the taskbar, opened by
clicking the clock, showing a month grid (view-only, no events yet) --
built as a reusable `widget_calendar` piece the same way
`widget_scrollback`/`widget_button` are, so any future app can embed it
too.

Control panel window with pluggable "applets" (Windows-style) -- first
applet: display settings (font size + color theme), since both already
exist as the `fontsize`/`color` shell commands, so the applet is mostly a
GUI wrapper around logic that's already implemented and tested.

### Milestone 6 -- ACPI + real power/timer

ACPI table parsing (RSDP/MADT/FADT) -- also unlocks a real software
poweroff (today's `system_poweroff()` only does the QEMU/Bochs
`outw(0x604, 0x2000)` I/O-port trick with a halt-and-message fallback,
deliberately the "works today in this exact dev/test setup" option, not a
real ACPI-based one) and is the prerequisite for discovering other CPU
cores (Milestone 7).

APIC + HPET timer, replacing the PIT + remapped 8259 PIC toy-os uses
today -- also a prerequisite for SMP and for timing finer than the PIT's
100 Hz tick.

### Milestone 7 -- SMP (multi-core)

Large undertaking, and a prerequisite is ACPI/MADT parsing (Milestone 6)
to even discover the other cores. Lightly sketched, not yet scoped to the
AHCI/USB level of rigor -- a reasonable first breakdown once picked up:

1. Discover other cores via the MADT's local APIC entries (needs
   Milestone 6 done first).
2. Bring up application processors via the INIT-SIPI-SIPI sequence,
   starting each one in a small real-mode trampoline that gets it into
   long mode.
3. Give each core its own GDT/IDT/stack -- today's kernel assumes exactly
   one of each.
4. Make the scheduler aware of more than one core (today's
   `scheduler_tick()`/`switch_to()` assume a single running context).

### Milestone 8 -- USB (keyboard/mouse)

Needs a USB host controller driver (UHCI/EHCI/xHCI, found the same way the
e1000 NIC already is, via the existing PCI enumeration) before any device
can even be enumerated. Low priority: PS/2 already covers mouse/keyboard
for every target so far, real hardware and QEMU alike. Substantially
bigger than the AHCI item above -- really two projects stacked on each
other (a host controller driver, THEN a USB device stack, THEN a HID class
driver on top of that), and xHCI (QEMU's modern default, and what most
real hardware presents) is a small scheduler in its own right -- command
ring, event ring, doorbell registers, per-device transfer descriptors --
not a simple DMA-and-interrupt device the way AHCI/legacy IDE are. Broken
into steps, each independently testable against QEMU's emulated USB
devices (`-device qemu-xhci -device usb-kbd -device usb-mouse`):

1. Host controller discovery -- find it via PCI (class 0x0C, subclass
   0x03), read the prog-if to identify UHCI/EHCI/xHCI. Test:
   `dmesg`/`lspci`-adjacent output names the right controller type,
   matching what QEMU was launched with.
2. Bring up ONE controller type (xHCI recommended, since it's QEMU's
   default and closest to real hardware; UHCI is dramatically simpler if
   the goal is first proving out the general model) -- reset it, read its
   capability registers, get it to a "running" state. No device-facing
   work yet. Test: controller reports running without hanging; capability
   register values match what's documented for QEMU's emulated
   controller.
3. Root port + device detection -- notice a device plugged into a root
   port, read its connect/enable status and negotiated speed. Test: a
   diagnostic command reports "device detected on port N" when QEMU's
   emulated `usb-kbd`/`usb-mouse` is attached.
4. Control transfers + enumeration -- implement the endpoint-0
   control-transfer pipeline (SETUP/DATA/STATUS, or the equivalent TRB
   sequence for xHCI), issue GET_DESCRIPTOR (device), SET_ADDRESS,
   GET_DESCRIPTOR (configuration), SET_CONFIGURATION. Test: the real
   vendor/product ID and descriptor strings read back match QEMU's
   emulated device identity, printed via `dmesg`.
5. HID boot-protocol interrupt transfers -- schedule a periodic interrupt
   IN transfer on the HID interface's endpoint, receive raw 8-byte
   boot-protocol reports (the fixed, simple report format -- not full HID
   report-descriptor parsing, a much bigger and unnecessary scope for
   "keyboard and mouse work"). Test: a debug command dumps raw report
   bytes when a key is pressed / the mouse is moved in the QEMU window.
6. Keyboard integration -- translate boot-protocol keyboard reports into
   real key events (feeding the existing `keyboard.c` model, or a new
   parallel event queue if that's cleaner). Test: type in a running shell
   using only the emulated USB keyboard and get correct characters,
   including modifier keys (Shift/Ctrl).
7. Mouse integration -- same for the mouse boot protocol (movement deltas
   + button bits), feeding `mouse.c`'s existing state. Test: move/click
   with the emulated USB mouse and see the GUI cursor respond, same as it
   does today via PS/2.
8. Legacy handoff -- disable BIOS/SMM's USB legacy PS/2 emulation (xHCI's
   USB Legacy Support Extended Capability) so the real driver has
   exclusive control. Test: no duplicate/ghost key events after handoff --
   a scripted keystroke sequence's event count matches expected, not
   double.

Stretch, not required for "keyboard and mouse work": hub support (device
behind a hub, not a root port), multiple simultaneous devices, anything
beyond the boot protocol (full HID report-descriptor parsing for
non-standard devices).

### Milestone 9 -- Networking

A large addition, comparable in scope to the filesystem or window manager.
Most of the infrastructure it needs has zero precedent-free work left --
PCI enumeration, a real IRQ-handler registration mechanism, contiguous/
DMA-friendly physical memory, a socket-like fd abstraction + syscalls
(`SYS_SOCKET`/`SYS_SEND`/`SYS_RECV`, currently scaffolding -- always
return -1, "no transport yet"), and a real IRQ-driven DMA transfer example
are all already done (see `CHANGELOG.md`'s build 390/400/410/420/470 and
`docs/decisions.md`). What's left is the actual driver and protocol stack:

Realistic path: pick a simple NIC to target (QEMU's `rtl8139` emulation is
the classic "easy first NIC driver" choice, much simpler than
e1000/virtio-net) -> IRQ registration (mechanism already exists) -> a
minimal Ethernet/ARP/IP/UDP stack before ever touching TCP (TCP's state
machine and retransmission logic only makes sense once packets can
reliably get in and out) -> TCP + the existing socket syscalls. One
smaller gap along the way: there's a tick counter (`pit_ticks()`) but no
sleep/delay primitive -- TCP needs timeouts and retransmission timers. The
driver-integration pattern itself is in good shape to build on --
`ata.c`'s probe/init-function/capability-header-through-`kapi.h`
structure, including its working IRQ-driven Bus-Master DMA path, is a
reasonable template, and the existing `*_test.c` diagnostic pattern is a
natural fit for early loopback/ARP verification -- but this is its own
multi-session project with its own milestones, not a single build bump.

### Milestone 10 -- Runtime + interop

Inter-process IPC (message passing) -- today's ring-3 processes are
isolated from each other with no way to communicate.

A real C library on top of `filetest`'s fd-aware syscalls: CRT0
(argc/argv from the initial stack), TLS (FS.base), FPU/SSE context-switch
save/restore -- none of which exist yet.

A FAT16/FAT32 driver -- real interop with other OSes' tools and USB
drives, distinct from the AHCI/SATA item (that's the controller; this is
the on-disk format).

`g_next_kernel_rsp` reentrancy fixed properly, so a real blocking syscall
doesn't need to spin-poll from ring 3 the way `echotest` does today.
(Partially addressed already -- `isr_in_progress()`/`isr_reset_depth()`,
see `docs/decisions.md`, cover the DMA-wait case Milestone 1 relies on --
this item is the general fix.)

`wintest` (`SYS_WIN_*`) windows made non-modal, sharing scheduler time
with the kernel-space window manager instead of taking the CPU
exclusively. Needs the scheduler to give the kernel-space WM loop and a
scheduled ring-3 process fair turns -- `scheduler_tick()` currently only
resumes kernel-space code when nothing is `READY`, and once any process is
armed, kernel-space code doesn't get scheduled again until every process
exits. Also: mouse input isn't piped to ring 3 at all yet, so `wintest`'s
close button is drawn but not clickable.

### Backlog

VFS: multiple filesystem backends mounted at once, not just one chosen at
boot. The VFS layer supports exactly one active backend today -- a real
mount-point scheme (`/` on one backend, `/data` on another, say) is the
natural next step if a second filesystem ever actually shows up and needs
to coexist with the first, rather than replace it; deferred until then
since it's meaningfully more code (cross-mount path resolution, boundary
conflicts) for a capability nothing needs yet.

Virtio drivers (disk/net) -- QEMU's paravirtualized devices, as a modern
addition alongside the legacy ATA/e1000 paths already used.

A benchmarking harness -- so a future change that regresses boot time or a
hot path (e.g. `gfx_present()`) gets caught instead of just "feeling"
slower.

Multi-architecture support (e.g. RISC-V 64 alongside x86_64) -- assessed,
not started: roughly a tenth of the codebase is architecture-specific and
it's already well-insulated behind `kapi.h`, but boot/interrupts/paging/
port-I/O are a real per-arch project. Full breakdown, proposed
`kernel/arch/<arch>/` layout, and a phased plan in
`docs/arch-portability.md`.

Stretch: port a small classic game (e.g. Doom) as an end-to-end stress
test of real disk-hosted ELF binaries + libc, once both exist.

---

Deliberately left out as out of scope for toy-os: an own bootloader (GRUB
is fine here), a self-hosted C compiler, and additional CPU architectures
beyond the RISC-V backlog item above (toy-os is x86-64-first by design,
per the project description). [brutal-org/brutal](https://github.com/brutal-org/brutal)'s
own roadmap has all three as goals -- not goals here.
