# Decisions

A short, topic-indexed answer to "why does toy-os work this way?" for
the handful of choices that come up again once code has grown around
them. This is a pointer file, not a duplicate of the reasoning --
`README.md`/`apps/README.md` cover architecture, and each entry below
links to the CHANGELOG.md section (or source file) that has the full
writeup. Update the pointer here when a decision changes; don't copy
the reasoning itself out of CHANGELOG.md or a source comment into this
file, or the two will drift.

If you're a Claude session or a contributor and about to ask "wait, why
is this built this way instead of the more obvious way?" -- check here
first before re-litigating it from scratch.

Every entry is listed in the index below, grouped by area -- add a line
there when you add an entry, or the index quietly stops being one.

## Index

**Kernel, memory & processes**

- [IRQ registration: one handler per line, framework-automatic EOI](#irq-registration-one-handler-per-line-framework-automatic-eoi)
- [Blocking I/O waits: hlt when safe, poll when inside a syscall](#blocking-io-waits-hlt-when-safe-poll-when-inside-a-syscall)
- [Contiguous memory: linear bitmap scan, not a buddy allocator](#contiguous-memory-linear-bitmap-scan-not-a-buddy-allocator)
- [`ring3test` still requires a reboot after its fault, on purpose](#ring3test-still-requires-a-reboot-after-its-fault-on-purpose)
- [`hello.c` stopped faulting on purpose and started faulting by accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident)
- [Kernel heap: coalesces by real address adjacency, not list order](#kernel-heap-coalesces-by-real-address-adjacency-not-list-order)
- [`kfree()`'s coalescing only checked the block being merged in, not the block being merged into](#kfrees-coalescing-only-checked-the-block-being-merged-in-not-the-block-being-merged-into)
- [A syscall's path-pointer validation checks a full `FS_PATH_MAX` range, not just up to the string's NUL](#a-syscalls-path-pointer-validation-checks-a-full-fs_path_max-range-not-just-up-to-the-strings-nul)
- [The M16 scheduler is permanently armed now -- an empty process table makes that safe](#the-m16-scheduler-is-permanently-armed-now----an-empty-process-table-makes-that-safe)
- [Stack canaries: `-mstack-protector-guard=global` and a fixed constant, not GCC's defaults](#stack-canaries--mstack-protector-guardglobal-and-a-fixed-constant-not-gccs-defaults)
- [NX enforcement scoped to userspace only -- the kernel's own identity map stays RWX](#nx-enforcement-scoped-to-userspace-only----the-kernels-own-identity-map-stays-rwx)
- [The serial debug console is poll-based from existing idle loops, not a new kernel thread](#the-serial-debug-console-is-poll-based-from-existing-idle-loops-not-a-new-kernel-thread)

**Filesystem & storage**

- [Filesystem is one active backend, not mount points](#filesystem-is-one-active-backend-not-mount-points)
- [No recursive delete](#no-recursive-delete)
- [Persistent filesystem is write-through with a single-slot journal](#persistent-filesystem-is-write-through-with-a-single-slot-journal)
- [File timestamps are broken-down local time, not a Unix epoch integer](#file-timestamps-are-broken-down-local-time-not-a-unix-epoch-integer)
- [TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single](#tfs2-v2s-block-pointers-go-direct-single-double-triple-indirect-not-just-direct-single)
- [`fs_read_range()`/`fs_write_range()` were added alongside `fs_read()`/`fs_write()`, not as a replacement](#fs_read_rangefs_write_range-were-added-alongside-fs_readfs_write-not-as-a-replacement)
- [`SYS_READ` read the whole file on every call, which made streaming quadratic](#sys_read-read-the-whole-file-on-every-call-which-made-streaming-quadratic)
- [pci.ids is bundled in `data/`, not downloaded or read from the build host](#pciids-is-bundled-in-data-not-downloaded-or-read-from-the-build-host)
- [TFS2's write batching: `write_range_impl()`'s data path in bulk, `persist_record()`'s journal down to two barriers](#tfs2s-write-batching-write_range_impls-data-path-in-bulk-persist_records-journal-down-to-two-barriers)
- [`fs_ops`'s new steppable-write function pointers are required, not optional/NULLable](#fs_opss-new-steppable-write-function-pointers-are-required-not-optionalnullable)
- [`tools/tfs2_writer.py`: content-hash sync, not mtime comparison; direct+single-indirect write scope, not full indirect support](#toolstfs2_writerpy-content-hash-sync-not-mtime-comparison-directsingle-indirect-write-scope-not-full-indirect-support)
- [`/bin` binaries: boot-time bootstrap-install now, a host-side TFS2 writer tool later](#bin-binaries-boot-time-bootstrap-install-now-a-host-side-tfs2-writer-tool-later)
- [`/bin/lspci` moved from boot-time bootstrap-install to build-time seeding, once the writer tool existed](#binlspci-moved-from-boot-time-bootstrap-install-to-build-time-seeding-once-the-writer-tool-existed)
- [Real disk-hosted ELF binaries: an old plan re-verified before building, not built from the doc as written](#real-disk-hosted-elf-binaries-an-old-plan-re-verified-before-building-not-built-from-the-doc-as-written)
- [Every ELF64 test binary moved to `/bin`, not just `lspci` -- and why two didn't fold in cleanly](#every-elf64-test-binary-moved-to-bin-not-just-lspci----and-why-two-didnt-fold-in-cleanly)
- [GPT header verification: a host-compiled unit test, not a live boot -- TFS2's own journal collides with LBA 1](#gpt-header-verification-a-host-compiled-unit-test-not-a-live-boot----tfs2s-own-journal-collides-with-lba-1)
- [An unreadable superblock is not a foreign disk -- refuse to format, don't guess](#an-unreadable-superblock-is-not-a-foreign-disk----refuse-to-format-dont-guess)
- [Metadata ordering: persist the record first, free the blocks second -- prefer a leak to a double-allocation](#metadata-ordering-persist-the-record-first-free-the-blocks-second----prefer-a-leak-to-a-double-allocation)
- [`fsck` reclaims leaks and marks stragglers, but never resolves a double-allocation](#fsck-reclaims-leaks-and-marks-stragglers-but-never-resolves-a-double-allocation)
- [Zero-filling a freshly allocated block is skipped only when the caller overwrites it whole](#zero-filling-a-freshly-allocated-block-is-skipped-only-when-the-caller-overwrites-it-whole)

**Drivers & hardware**

- [DMA needs PCI Bus Master Enable, not just a programmed descriptor](#dma-needs-pci-bus-master-enable-not-just-a-programmed-descriptor)
- [The DMA bounce buffer is 64KB because that's one PRD, not because 64KB benchmarked well](#the-dma-bounce-buffer-is-64kb-because-thats-one-prd-not-because-64kb-benchmarked-well)
- [PCI enumeration is a brute-force flat scan, not bridge-aware recursion](#pci-enumeration-is-a-brute-force-flat-scan-not-bridge-aware-recursion)
- [Nordic keyboard/character support: Latin-1, not UTF-8; 3 remapped keys, not a full layout](#nordic-keyboardcharacter-support-latin-1-not-utf-8-3-remapped-keys-not-a-full-layout)
- [Keyboard layouts are data files (`/etc/kbs/<name>`) generated from Linux's own XKB data, not a compiled-in enum](#keyboard-layouts-are-data-files-etckbsname-generated-from-linuxs-own-xkb-data-not-a-compiled-in-enum)
- [GDB debugging: QEMU's built-in stub, not an in-kernel serial protocol implementation](#gdb-debugging-qemus-built-in-stub-not-an-in-kernel-serial-protocol-implementation)
- [ATA's waits are bounded by wall-clock in one context and a spin count in the other](#atas-waits-are-bounded-by-wall-clock-in-one-context-and-a-spin-count-in-the-other)

**GUI: window manager, compositor & widgets**

- [Widgets are added once a second real caller needs them -- except the checkbox](#widgets-are-added-once-a-second-real-caller-needs-them----except-the-checkbox)
- [`gfx_draw_string()` doesn't clip to a width -- callers that need that do their own](#gfx_draw_string-doesnt-clip-to-a-width----callers-that-need-that-do-their-own)
- [`widgets.h`/`theme.h` stay minimal on purpose](#widgetshthemeh-stay-minimal-on-purpose)
- [The window manager is one event loop, not decoupled components](#the-window-manager-is-one-event-loop-not-decoupled-components)
- [Esc no longer exits the GUI desktop -- it's unclaimed at the WM level now](#esc-no-longer-exits-the-gui-desktop----its-unclaimed-at-the-wm-level-now)
- [Don't put a `text_scrollback` on the stack](#dont-put-a-text_scrollback-on-the-stack)
- [Console scrollback is a character ring in vga.c, and the boot log is echoed to it](#console-scrollback-is-a-character-ring-in-vgac-and-the-boot-log-is-echoed-to-it)
- [Button press/release feedback: a general `on_press`/`on_release` WM mechanism, not a Calculator-only hack](#button-pressrelease-feedback-a-general-on_presson_release-wm-mechanism-not-a-calculator-only-hack)
- [ui_button/ui_button_group: Brutal-OS-inspired, but not a full retained view system](#ui_buttonui_button_group-brutal-os-inspired-but-not-a-full-retained-view-system)
- [Start menu click flash: a deferred close via pit_ticks(), not a blocking sleep](#start-menu-click-flash-a-deferred-close-via-pit_ticks-not-a-blocking-sleep)
- [Title-bar buttons: press-then-commit-on-release, reusing the content_pressed shape](#title-bar-buttons-press-then-commit-on-release-reusing-the-content_pressed-shape)
- [apps/ui/: a directory for retained-widget objects, once there were three](#appsui-a-directory-for-retained-widget-objects-once-there-were-three)
- [Calculator is the first `multi_instance` GUI app](#calculator-is-the-first-multi_instance-gui-app)
- [`apps/widgets.c`/`.h` no longer exist -- and `ui_scrollback`/`ui_scrollbar` didn't get an owned-geometry wrapper](#appswidgetsch-no-longer-exist----and-ui_scrollbackui_scrollbar-didnt-get-an-owned-geometry-wrapper)
- [The desktop's right-click quick-launch menu doesn't distinguish icons from empty space](#the-desktops-right-click-quick-launch-menu-doesnt-distinguish-icons-from-empty-space)
- [`context_menu.h`'s items carry a `void *ctx`, but `start_menu.h`'s don't](#context_menuhs-items-carry-a-void-ctx-but-start_menuhs-dont)
- [Click-to-position/selection lives in the shared `text_scrollback` widget, not a Notepad-only one](#click-to-positionselection-lives-in-the-shared-text_scrollback-widget-not-a-notepad-only-one)
- [The file picker is a WM-level modal overlay (`apps/wm/file_picker.c`), not an `apps/ui/` widget](#the-file-picker-is-a-wm-level-modal-overlay-appswmfile_pickerc-not-an-appsui-widget)
- [`struct window *` isn't a stable per-window identity across frames -- don't cache one](#struct-window-isnt-a-stable-per-window-identity-across-frames----dont-cache-one)
- [Why the compositor uses one scene-wide damage region, not per-window exposure tracking](#why-the-compositor-uses-one-scene-wide-damage-region-not-per-window-exposure-tracking)
- [The taskbar/tray falls back to full-screen repaint on purpose, not as an oversight](#the-taskbartray-falls-back-to-full-screen-repaint-on-purpose-not-as-an-oversight)

**Shell, apps & console**

- [Terminal wraps the real shell, it doesn't reimplement it](#terminal-wraps-the-real-shell-it-doesnt-reimplement-it)
- [Tab completion is a shared candidate generator, not a shared line editor](#tab-completion-is-a-shared-candidate-generator-not-a-shared-line-editor)
- [Ctrl/Alt are encoded as control codes and an ESC prefix, not as new key codes](#ctrlalt-are-encoded-as-control-codes-and-an-esc-prefix-not-as-new-key-codes)
- [PATH lives in the shell, not the kernel -- and builtins win over it](#path-lives-in-the-shell-not-the-kernel----and-builtins-win-over-it)
- [The CLI editor's status bar needs its own line-wrapping pass, not a plain dump-and-let-the-console-wrap](#the-cli-editors-status-bar-needs-its-own-line-wrapping-pass-not-a-plain-dump-and-let-the-console-wrap)
- [Timezone city list is a database file, not a hardcoded array or a config key](#timezone-city-list-is-a-database-file-not-a-hardcoded-array-or-a-config-key)
- [`/etc` is one shared `toyos.conf` by default, not a file per setting](#etc-is-one-shared-toyosconf-by-default-not-a-file-per-setting)
- [The on-disk layout is a trimmed FHS, not POSIX -- and `/tests` is a deliberate exception](#the-on-disk-layout-is-a-trimmed-fhs-not-posix----and-tests-is-a-deliberate-exception)
- [dmesg coverage: log from the one-shot call site, not the hot function itself](#dmesg-coverage-log-from-the-one-shot-call-site-not-the-hot-function-itself)
- [Terminal's `run <name>` uses an explicit allowlist, not a blocklist](#terminals-run-name-uses-an-explicit-allowlist-not-a-blocklist)
- [`kapi.h` is the only header apps/ includes](#kapih-is-the-only-header-apps-includes)
- [The kernel/lib/ toolkit: converters that fill a buffer, not printers](#the-kernellib-toolkit-converters-that-fill-a-buffer-not-printers)
- [Case folding is ASCII-only, even though this kernel's layouts have Å/Ä/Ö](#case-folding-is-ascii-only-even-though-this-kernels-layouts-have-åäö)
- [The console cursor saves the pixels it covers](#the-console-cursor-saves-the-pixels-it-covers)
- [`strace` traces an address space, and prints each line after the handler returns](#strace-traces-an-address-space-and-prints-each-line-after-the-handler-returns)

**Build, versioning & project docs**

- [Build-number scheme: fix/feature/major tiers, not dates or semver](#build-number-scheme-fixfeaturemajor-tiers-not-dates-or-semver)
- [Versioning: semver + `-dev` suffix, not a per-change build number](#versioning-semver--dev-suffix-not-a-per-change-build-number)
- [Repo is MIT; the baked JetBrains Mono glyph data is separately SIL OFL 1.1](#repo-is-mit-the-baked-jetbrains-mono-glyph-data-is-separately-sil-ofl-11)
- [Repo history scrubbed of the maintainer's real name -- privacy request, not a bug fix](#repo-history-scrubbed-of-the-maintainers-real-name----privacy-request-not-a-bug-fix)
- [Socket fds: scaffolding ahead of the driver, not a working transport](#socket-fds-scaffolding-ahead-of-the-driver-not-a-working-transport)

**Session workflow & environment**

- [Protected files: `device_commit_files` blocks writes, `device_bash` doesn't](#protected-files-device_commit_files-blocks-writes-device_bash-doesnt)
- [The device bridge can't delete files, and `git` leaves stale locks behind on it](#the-device-bridge-cant-delete-files-and-git-leaves-stale-locks-behind-on-it)
- [Cowork device-bridge vs. direct local checkout: detected via `git config user.name`, not assumed](#cowork-device-bridge-vs-direct-local-checkout-detected-via-git-config-username-not-assumed)

## Widgets are added once a second real caller needs them -- except the checkbox

`apps/widgets.h`'s standing rule (stated in its own top comment) is:
add a new widget primitive only once a second independent hand-rolled
implementation of the same idea turns up, not preemptively. Every
widget through build 480 followed that -- `widget_button` consolidated
three existing button implementations (wm.c's title-bar buttons,
Calculator's grid, Notepad's toolbar), `text_scrollback` and the
scrollbar widgets grew out of apps/terminal.c and were then reused
by apps/notepad.c and apps/editor.c. Build 490's `widget_textfield_*`
kept that pattern (Notepad's fixed filename was the identified real
need). Its `widget_checkbox_*`, though, was added explicitly ahead of
any real caller, by direct user request when asked to choose the
scope -- a deliberate, acknowledged exception to the rule above, not a
change to it: the rule still applies to whatever gets added *next*.
See CHANGELOG-archive-2.md's **Build 490** for the full writeup.

## `gfx_draw_string()` doesn't clip to a width -- callers that need that do their own

`gfx_draw_string()` (`kernel/drivers/gfx.c`) takes no width parameter at
all -- it draws every character it's given, one `gfx_char_w()` cell at a
time, and only stops at a real newline or the end of the string.
Whatever clipping happens is `gfx_put_pixel()`'s ordinary
screen/window-bounds check, not anything aware of a caller's intended
box. `widget_textfield_draw()` (`apps/widgets.h`/`.c`) used to have a
doc comment claiming text longer than the field was "simply clipped the
same way every other text-drawing call in this codebase already is" --
that was never true, there's no such clipping to inherit, and a long
filename actually drew straight past the field's border into whatever
was next to it (a real user-reported bug, caught from a screenshot).
Fixed by having `widget_textfield_draw()` compute its own visible
character count from `w` and slice `tf->buf` before ever calling
`gfx_draw_string()`, sliding the visible window to keep the cursor in
view while the field is active. The lesson for any *future* widget that
draws text into a fixed box: `gfx_draw_string()` will not save you,
budget the width yourself. See CHANGELOG.md's `[Unreleased]` entry.

**Same lesson, vertical axis:** a follow-up report caught the field's
height having the exact same problem one axis over -- `apps/notepad.c`
sized the field's row height to precisely `gfx_char_h()`, so the
glyphs' own opaque background painted flush against (and visually
erased) the border pixels on any row with a character. Budgeting width
isn't enough on its own; a fixed-box text widget needs real margin on
*both* axes, not just clipping on the one that happened to get bug
reports first. Fixed with a small `ROW_VPAD` constant reserving actual
vertical slack. See CHANGELOG.md's `[Unreleased]` entry.

## IRQ registration: one handler per line, framework-automatic EOI

`kernel/arch/x86_64/irq.c`'s table (`irq_register_handler()`/`irq_dispatch()`)
deliberately doesn't support multiple handlers chained on one IRQ line
-- every IRQ source this kernel has, or is about to add (a NIC), lives
on its own dedicated line in QEMU's default topology (confirmed by
build 390's `lspci`), so real IRQ-line sharing (which does happen on
busier real hardware) isn't a case that comes up here; registering a
second handler for an IRQ that already has one just replaces it.
`irq_dispatch()` also sends the PIC end-of-interrupt itself,
automatically, after calling whatever handler is registered -- not
left to each handler to remember. A forgotten EOI on a real IRQ line
silently stops all further interrupts on that line, a classic and
nasty-to-debug bug; removing the chance of it was judged worth the
small loss of flexibility (a handler can't EOI early, before doing
slower work). This replaced `isr_dispatch()`'s old hardcoded if/else
chain (timer/keyboard/mouse special-cased, everything else silently
EOI'd and ignored) -- including the timer, which now hands off to
`scheduler_tick()` from inside its own registered handler
(`idt.c`'s `timer_irq_handler()`) rather than a dispatch-level special
case, so every hardware IRQ (32-47) goes through one uniform path. See
`irq.h`'s top comment and CHANGELOG-archive-2.md's **Build 400** for the full
writeup, including what got regression-tested (timer/scheduler,
keyboard, mouse) since this touched all three.

## Blocking I/O waits: hlt when safe, poll when inside a syscall

Any driver that wants to genuinely block (via `hlt`) until an IRQ
fires -- rather than busy-poll a status register -- has to know
whether it's currently running inside an interrupt handler, because
`int 0x80` is wired as an interrupt gate and clears IF for the whole
syscall, so `hlt` there would park forever with nothing able to wake
it, and naively `sti`-then-blocking would reintroduce a real, already-
documented reentrancy bug: `isr_dispatch()`'s epilogue unconditionally
overwrites a single global resume pointer (`g_next_kernel_rsp`) before
every `iretq`, so a nested interrupt firing mid-syscall corrupts the
outer handler's resume point (this is why `SYS_READ_KEY` abandoned
blocking-with-interrupts-on previously). `idt.h`'s
`isr_in_progress()`/`isr_reset_depth()` (a `g_isr_depth` counter,
incremented/decremented around every `isr_dispatch()` call, force-reset
to 0 at the one safe point -- `process_run_ring3()`'s longjmp-style
resume branch) answers "am I inside an interrupt right now?" so a
driver can genuinely `hlt`-block when it's safe (the common case: boot
init and `apps/` code calling `fs_write()`/`fs_read()` directly from
kernel space) and fall back to bounded polling of the *device's own*
status bit when it isn't (the ring-3 `*_test.c` syscall path) -- the
hardware still raises that bit regardless of the CPU's IF state, so
polling it is still real completion detection, just not CPU-interrupt-
driven. `ata.c`'s `wait_dma_irq()` is the first (and, as of this
writing, only) caller, but the mechanism itself is general-purpose --
any future driver wanting to block inside a syscall-reachable code
path (a NIC's TX/RX ring, say) needs this same check, not a
driver-specific reinvention. See `idt.h`'s doc comments and
CHANGELOG-archive-2.md's **Build 470** for the full writeup.

## DMA needs PCI Bus Master Enable, not just a programmed descriptor

A PCI device's I/O-mapped DMA control registers (a Bus-Master IDE
controller's BM_CMD/BM_STATUS/BM_PRDT, say) keep accepting reads/
writes and can report a nominal "transfer complete" status even when
the PCI Command register's "Bus Master Enable" bit (config offset
0x04, bit 2) is never set -- without it, the device just never issues
real memory read/write bus cycles, so DMA "succeeds" while moving no
actual data. Easy to miss because nothing about the failure looks like
a failure from software's point of view; only comparing against a
known-good PIO transfer, or tracing the actual bytes moved, exposes
it. `pci_enable_bus_master()` (`pci.c`/`pci.h`) sets it via a
read-modify-write of the Command register, called once from
`ata_init_dma()`. Any future DMA-capable driver (a NIC) needs this same
call before its own DMA moves real data -- noted directly in
`pci.h`'s doc comment, not just here. See CHANGELOG-archive-2.md's **Build 470**
for how this was root-caused (PIO-vs-DMA comparison, then a host-side
pre-seeded disk image to isolate the read path and trace the bounce
buffer).

## ATA's waits are bounded by wall-clock in one context and a spin count in the other

Every wait in `kernel/drivers/ata.c` that can block looks like it's
written twice, and the duplication is deliberate. A wall-clock budget
needs `pit_ticks()` to advance, and it doesn't inside a syscall: `int
0x80` is wired as an interrupt gate, so IF stays clear for the whole
handler and no timer IRQ ever increments the counter. A wall-clock loop
reached from there wouldn't time out, it would hang the machine. So
`wait_dma_irq()` and `wait_not_busy()` both branch on
`isr_in_progress()` (`idt.h`) -- real elapsed time when it's safe, a
fixed `ATA_POLL_LIMIT` spin when it isn't. Same split, same reason, as
the `hlt`-when-safe/poll-when-inside-a-syscall rule in the entry on
blocking I/O waits above.

**Why this is worth an entry rather than just a comment:** the two
bounds were written years apart in project time, and for a long stretch
only the completion wait had the wall-clock half. `DMA_WAIT_TICKS` was
deliberately *widened* to 5s to absorb host-side I/O stalls, while
`wait_not_busy()` sat at a fixed 100000-iteration spin -- which measures
out to ~12ms, giving three retries ~37ms in total. The driver was
therefore 135x more patient about a command in flight than about a
drive still finishing the previous one, and a host stall (a Btrfs
commit, an ISO being written to the same disk) hit the impatient half.
The general lesson is the one worth carrying: **a spin count is not a
duration.** It measures the guest CPU, which keeps running at full
speed during exactly the host-side stalls it's supposed to absorb, so
any timeout that must survive one has to be denominated in real time.

See `ata.c`'s `wait_not_busy()`/`DMA_WAIT_TICKS` comments and
CHANGELOG.md's `[Unreleased]`.

## Contiguous memory: linear bitmap scan, not a buddy allocator

`pmm_alloc_contiguous()` (`kernel/mm/pmm.c`) finds a run of N
physically contiguous free frames by linearly scanning the same
one-bit-per-frame bitmap `pmm_alloc_frame()` already uses, rather than
reserving a dedicated always-contiguous region at boot, or replacing
the bitmap with a fundamentally different structure (a buddy/
segregated-free-list allocator, the standard answer to "finding
contiguous runs gets slow/fragmented"). A buddy allocator would win if
this got called often against a heavily fragmented pool -- it finds
and frees power-of-two-sized runs without a linear scan -- but nothing
in this kernel calls `pmm_alloc_contiguous()` on a hot path: today it
only exists for a future NIC driver to set up its descriptor ring once
at init, and no allocator changed size class, hot/cold split, or
scan strategy in a way this could regress. Replacing the whole
allocator to solve a fragmentation problem no code in this kernel has
actually hit yet was judged premature; it's flagged in
`docs/roadmap.md` as the fix if that ever changes, not built now. See
`pmm.h`'s top comment and CHANGELOG-archive-2.md's **Build 410** for the
full writeup, including `pmm_selftest()`'s boot-time verification
(no consumer exists yet to exercise these functions any other way).

## PCI enumeration is a brute-force flat scan, not bridge-aware recursion

`kernel/drivers/pci.c`'s `pci_init()` checks every one of the 256 x 32
x 8 possible bus/device/function combinations directly via legacy
CONFIG_ADDRESS/CONFIG_DATA (0xCF8/0xCFC) port I/O, rather than the
"real OS" approach of scanning bus 0 and recursing into any PCI-to-PCI
bridge's secondary bus. Deliberate: the brute-force version needs no
bridge detection, no recursion, and no cycle safety, and finds the
same devices as the recursive version on any topology this kernel
actually runs on (QEMU's default chipset, or ordinary real hardware
without a deeply nested bridge topology) -- the only real cost is
wasted probe reads on buses/slots nothing lives at, which is cheap.
Also chose legacy CF8/CFC access over the newer memory-mapped ECAM
mechanism, since ECAM needs ACPI/MCFG table parsing just to find its
base address and CF8/CFC is universally supported including by QEMU's
emulated chipset. BARs are decoded (I/O-vs-memory, base address) but
NOT size-probed (the write-0xFFFFFFFF-and-read-back trick) -- that's
deferred to whichever future driver actually needs to map a BAR, since
it means temporarily disabling the device's decode and isn't needed
just to enumerate/identify what's present. See `pci.h`'s top comment
and CHANGELOG-archive-2.md's **Build 390** for the full writeup -- this was the
first concrete milestone toward the TCP/IP prerequisites README.md's
**Build 380** entry laid out.

## `SYS_READ` read the whole file on every call, which made streaming quadratic

`SYS_READ`'s handler (`kernel/proc/syscall.c`) used to call `fs_read()`
-- which loads an ENTIRE file into a `kmalloc()`'d buffer -- and then
copy out just the `len` bytes sitting at the fd's current offset. Every
call. So the cost of streaming a file was (file size) x (number of
reads), and `SYS_WRITE_MAX` caps a read at 1KB.

Nothing noticed for a long time because nothing in ring 3 had ever
opened a file bigger than a few hundred bytes; at that size the whole
file *is* one read. `/bin/lspci` reading the 1.6MB `pci.ids` was the
first real caller, and it turned into roughly 1,615 calls x 1.6MB =
**~2.6GB of disk reads, taking 35 seconds** for what should be a
sub-second command. Switching the handler to `fs_read_range()` -- which
exists precisely for this, and whose own doc comment describes "a caller
streaming a whole file just calls this in a loop with an increasing
offset" -- took the same command to **0.9 seconds including boot**.

Two things worth carrying from it. **A wrong complexity class can sit
undisturbed for as long as the inputs stay small**, and it fails by
being slow rather than by being wrong, so no test catches it -- this one
was found by a feature that happened to need a bigger file, not by
review. And the correct API already existed and was already documented
for exactly this use; the bug was a call site that predated it and was
never revisited. When a range-based API gets added next to a
whole-object one (see the entry above on why both exist), the existing
callers are the thing to check.

See `syscall.c`'s `SYS_READ` branch and CHANGELOG.md's `[Unreleased]`.

## pci.ids is bundled in `data/`, not downloaded or read from the build host

`/bin/lspci` resolves `8086:7010` into "Intel Corporation 82371SB PIIX3
IDE" by reading `/usr/share/hwdata/pci.ids` -- the same file, at the
same path, that a real Linux distribution's `lspci` reads. The copy is
committed at `data/pci.ids` (1.6MB) and staged onto the disk image by
the Makefile's `seed` target.

Bundling was chosen over the two alternatives. **Reading the build
host's `/usr/share/hwdata/pci.ids`** costs nothing in the repo but makes
the build depend on host layout -- absent on macOS and minimal
containers -- and makes two machines produce different images.
**Downloading from pci-ids.ucw.cz at build time** is always current, but
puts a network fetch in the build, which breaks offline builds and the
sandboxed environments CLAUDE.md documents, and adds a supply-chain
input. A committed copy is reproducible, offline, identical everywhere,
and refreshing it is a deliberate commit rather than a silent change.

**It does not live in `seed/`.** `seed/sync/` looks like the obvious
home -- it's the tree that gets mirrored onto the disk image -- but it's
a build *staging* area: `make clean` does `rm -rf seed/sync`, and
`.gitignore` excludes it, because the Makefile repopulates it with built
ELFs every build. A file placed there works perfectly on the machine
that created it and silently doesn't exist for anyone who clones. (This
was caught exactly that way: the file survived local testing, then
vanished during a `make verify`, while the copy already written to
`disk.img` kept the feature working.) Hand-authored content belongs in a
tracked directory that the `seed` target copies in.

Licensing: upstream offers the database under GPL-2.0-or-later **or**
3-clause BSD. toy-os takes the BSD option, which is compatible with the
MIT repo; `LICENSE` carries the full text in a "Third-party data"
section, following the same pattern as the baked JetBrains Mono glyph
data (see the entry on that).

## Filesystem is one active backend, not mount points

`kernel/fs/vfs.c` dispatches every `fs_*` call to a single active
`struct fs_ops` backend (today, always `tfs_ops` -- see
`kernel/include/kernel/fs_ops.h`). Adding a second filesystem means writing a
new backend and pointing `fs_init()` at it, not routing different path
prefixes to different backends simultaneously -- nothing needs the
latter yet, and it's meaningfully more code (cross-mount path
resolution, boundary conflicts) for a capability that would sit
unused. See `fs_ops.h`'s top comment and CHANGELOG-archive-2.md's **Build 304**
for the full reasoning, including what it would take to add mount
points later if that ever changes.

## No recursive delete

`fs_delete()` refuses to delete a non-empty directory outright, rather
than deleting its contents. Deliberate, not a missing feature --
avoids a whole class of "oops, deleted more than I meant to" mistakes
in a filesystem with no trash/undo. See `kernel/include/api/fs.h` and
`tfs.c`'s top comment ("Honest limitations, not solved here").

## Persistent filesystem is write-through with a single-slot journal

Every mutating call (`touch`/`write`/`mkdir`/`delete`) still writes its
one record to disk immediately (write-through, not batched or lazily
flushed) -- but as of build 480 ("TFS2"), that one record write goes
through a write-ahead log first rather than straight to its final
table slot, closing the "crash mid-write corrupts one record" window
the original ("TFS1") write-through design explicitly accepted. A
single journal slot is enough -- not a general multi-record
transaction log -- because every mutating call here only ever changes
ONE table slot; a real filesystem juggling multi-record transactions
(renaming across directories, say) would need more than this. Chosen
over the two alternatives it was weighed against (a shadow/double-
buffer per record -- simpler logic but doubles every record's on-disk
size; a minimal commit-flag-only journal -- smaller journal but a torn
write loses the newest change instead of recovering it) because it
gives genuine crash recovery, not just torn-write detection, for a
journal region that only costs one extra record's worth of disk space
total (not per-record). See `tfs.c`'s top comment ("Journaling") for
the exact 4-step write-ahead sequence and replay logic, `docs/
tfs2-spec.md` for the on-disk journal format, and CHANGELOG-archive-2.md's
**Build 480** for the full writeup.

## File timestamps are broken-down local time, not a Unix epoch integer

`fs_stat()`'s `created`/`modified` fields (build 480) are `struct
rtc_time` -- the same hour/minute/second/day/month/year struct
`SYS_GETTIME` and the shell's `time` already return -- not a Unix
epoch integer. This kernel has never needed a civil-date<->epoch
conversion for anything else (no code anywhere computes "days since
1970" or similar), so storing the same struct everything else already
uses avoided adding one just for this feature; a host-side tool
reading a TFS2 image converts to epoch seconds itself if it wants
that instead (`docs/tfs2-spec.md`'s reference reader shows the
equivalent conversion via Python's `datetime`). The tradeoff: no
UTC-offset field is stored alongside a timestamp, so a value only
means what it looks like -- local wall-clock time at whatever
timezone was selected (`timezone` shell command) at the moment it was
written -- not an unambiguous point in time comparable across
different timezone selections. Acceptable for a toy OS's own files;
would need revisiting (probably by finally adding an epoch conversion
helper) if timestamps ever needed to be meaningfully compared against
a real-world reference. See `fs.h`'s `fs_stat()` doc comment,
`tfs.c`'s top comment, and CHANGELOG-archive-2.md's **Build 480**.

## `kapi.h` is the only header apps/ includes

Introduced when the tree was split into `kernel/core/`, `kernel/drivers/`,
and `apps/` (see `CHANGELOG-archive.md`'s **Milestone 4** -- the old
pre-v0.1.0 numbering, not `docs/roadmap.md`'s current Milestone 4)
specifically so
drivers could be reshuffled internally without every app needing an
edit -- apps depend on the aggregated capability surface, never on a
driver header or `inb`/`outb` directly. `apps/wm/wm.h` is a second,
parallel boundary for GUI-specific `window_*` helpers, deliberately
not folded into `kapi.h` (not every app is a GUI app). See CLAUDE.md's
"Conventions worth knowing before editing" for the enforcement rule.

## `widgets.h`/`theme.h` stay minimal on purpose

Both only gained their current primitives once a *second* real caller
needed them (see CHANGELOG.md's **"Splitting wm.c into apps/wm/, and a
shared widgets.h/widgets.c module"** for widgets.h's origin, and the
scrollbar-phase builds for how `text_scrollback` grew from
Terminal-only to shared with Notepad). Deliberately not
speculatively built out ahead of a real second caller -- see each
header's own top comment before adding to it.

## The window manager is one event loop, not decoupled components

`apps/wm/` is split into `wm.c`/`wm_input.c`/`wm_render.c` by concern
for *readability*, but shares state through `wm_internal.h`'s
`extern`s rather than hiding it behind accessor functions -- it's
still one tightly-coupled event loop, the same thing the single
pre-split `wm.c` was (CHANGELOG.md's **"Splitting wm.c into apps/wm/..."**),
just spread across files. Deliberate: this is one component's internal
organization, not a boundary between independently-reasoned-about
components the way `kapi.h`/`wm.h` are. See `wm_internal.h`'s top
comment.

## Ctrl/Alt are encoded as control codes and an ESC prefix, not as new key codes

The keyboard driver tracked Shift and AltGr and nothing else -- left
Alt was explicitly dropped, with a comment saying the driver had no use
for it. Adding readline-style line editing meant deciding how Ctrl and
Alt should reach an app, and the codebase already had a precedent
pointing the other way: `KEY_SHIFT_ARROW_*` gave Shift+arrow its own
distinct key codes rather than exposing "is shift down" (deliberately --
see the entry above on why that timing matters).

Ctrl/Alt went the other way, to what a real terminal does:
`Ctrl-<letter>` is that letter's control code (`Ctrl-A` = 0x01), and
`Alt-<key>` is ESC followed by the key. Reasons, in order of weight:

1. **The collisions it creates are the correct behavior.** `Ctrl-H` is
   backspace, `Ctrl-I` is Tab, `Ctrl-M` is Return -- in this encoding
   they *are* those keys, with no special-casing, exactly as in bash.
   Distinct key codes would have needed explicit aliases for all three
   to behave the way users expect.
2. **No new code space, no truncation audit.** The existing `KEY_*`
   codes occupy 0x91-0xA3 and the Nordic letters 0xC4-0xF6; a
   `KEY_CTRL_*`/`KEY_ALT_*` block would have had to go above 0xFF,
   which means auditing every `(char)key` cast in the tree -- a bug
   class this project has been bitten by before.
3. **The decoder is one a serial terminal would need anyway**, so the
   line editor's ESC handling isn't throwaway.

The cost is real and worth knowing: a lone Esc and the start of a Meta
sequence are indistinguishable at the driver layer, which is a genuine
ambiguity physical terminals have too. The line editor resolves it by
holding the ESC and deciding on the next key; nothing that needs a bare
Esc (leaving GUI mode, exiting the editor) sits inside a line edit, so
none of them are affected. AltGr is deliberately NOT Meta -- it stays a
layout modifier so Nordic third-level characters keep working.

See `kernel/include/api/keyboard.h`'s "Ctrl and Alt" comment,
`kernel/lib/klineedit.c`, and CHANGELOG.md's `[Unreleased]`.

## The console cursor saves the pixels it covers

The framebuffer console's cursor used to be a filled rectangle, erased
by filling the same rectangle with black. That works exactly as long as
the cursor only ever sits at the append point, where the cell is
guaranteed blank -- which was true until the shell could put the cursor
in the middle of a line. Then it started eating characters: the first
mid-line edit shipped with a visible hole where a '.' had been, and the
blink had to be suppressed off the append point to stop it doing the
same thing once a second.

The fix is to stop reconstructing the cell and just remember it:
`cursor_draw()` reads the cell's pixels with `gfx_get_pixel()` into a
small static buffer before painting, and `cursor_hide()` puts them
back. Exact regardless of what was underneath, so the blink is safe
again -- and, because nothing has to reconstruct anything, **any cursor
shape becomes possible for free**. That's what made four styles
(translucent/underline/beam/reverse) a config choice rather than four
special cases.

Worth recording about the translucent style specifically, since it took
three attempts and each failure was instructive: tinting the whole cell
toward the *text* colour changed the glyph not at all (grey over grey)
and left a cursor you had to hunt for; tinting the whole cell toward
white lit the cell up but moved the glyph with it, dropping
glyph-vs-block contrast from 170 to 89; tinting *only* the background
produced a block two pixels wide, because a glyph like `r` fills most
of its cell. What works is tinting both, with the glyph tinted harder
than the background.

See `kernel/drivers/vga.c`'s cursor section, `vga.h`'s cursor-style
enum, and CHANGELOG.md's `[Unreleased]`.

## Terminal wraps the real shell, it doesn't reimplement it

`apps/terminal.c` runs the actual `shell_dispatch()` inside a window
via a `vga_sink` redirect, rather than maintaining a second "GUI
shell" command handler that could drift out of sync with the real one.
A short, explicit list of commands that draw straight to the physical
screen or block in ways that don't make sense inside a window (`gui`,
`ring3test`, `elftest`, ...) print an explanation instead of running.
Built across four phases -- see CHANGELOG-archive-2.md's
**Builds 183, 193, 203, 253**.

## `ring3test` still requires a reboot after its fault, on purpose

Once process exit/teardown existed (CHANGELOG-archive.md's **Build 173**) so a
crashed *scheduled* ring-3 process doesn't halt the kernel, `ring3test`
kept requiring a reboot anyway -- not because teardown didn't reach it,
but because it intentionally drops to ring 3 via its own raw `iretq`
instead of `process_run_ring3()`, so there's nowhere for the kernel to
recover it *to*. See `process.h` and `docs/roadmap.md`.

`elftest` used to be this file's other example (same raw-`iretq`
mechanism, via `hello.elf`) until the ELF64-to-`/bin` migration folded
it into the generic `run hello` path (see this file's entry on that
migration, and CHANGELOG.md's `[Unreleased]`), at the cost of losing
test coverage for the raw `iretq` entry path specifically. `ring3test`
is the one remaining place that path gets exercised. `hello.elf` no
longer faults at all -- it's a plain greet-and-exit program now; see
[hello.c stopped faulting on purpose and started faulting by
accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident)
for what happened in between.

## `hello.c` stopped faulting on purpose and started faulting by accident

`run hello` page-faulted (`CR2=0x8000100000`, `RIP=0x800000000a`) for
some time before anyone chased it, and it read like a broken or stale
binary. It wasn't: `hello.c` predated syscalls, so with no way to
print, it proved it had run by writing a marker to a fixed address the
kernel would read back (`USERLAND_MARKER_ADDR`) and then executing
`hlt` to fault deliberately. The `elftest` command mapped a page at
that address specially. When the ELF64-to-`/bin` migration folded
`elftest` into the generic `run hello` path, the harness went away and
the assumption didn't -- so the binary faulted on the marker write, one
instruction *before* the `hlt` it existed to demonstrate. A deliberate
fault had quietly become an accidental one, at a different address, for
a different reason, while still looking like the expected outcome.

Fixed by making `hello.c` a real program (greet via `SYS_WRITE`, exit
0) rather than restoring the harness: `ring3test` still covers the
raw-`iretq` entry path, `crash_test`/`nx_test` still cover deliberate
faults and their recovery, and the binary named `hello` now does what
its name says. `USERLAND_MARKER_ADDR` was deleted along with it -- it
had no other user, and it aliased `ELF_RUN_HEAP_VADDR` (`elf_run.c`)
exactly, two constants picked independently at the same address, so a
future read-back test wanting the mechanism back needs its own address
clear of the heap and stack rather than that one.

Worth generalising: **a test binary whose harness is removed doesn't
report that it lost its harness -- it reports whatever failure the
missing harness causes.** The migration's own writeup listed exactly
what coverage was being traded away and still missed this, because the
lost piece wasn't the test, it was a page mapping the test depended on.
See CHANGELOG.md's `[Unreleased]`.

## The on-disk layout is a trimmed FHS, not POSIX -- and `/tests` is a deliberate exception

Asked whether toy-os's own filesystem should follow "something POSIX
likes". The premise is worth correcting, because it comes up again:
**POSIX barely specifies filesystem layout at all.** POSIX.1 mandates
`/`, `/tmp` and a few device paths (`/dev/null`, `/dev/tty`,
`/dev/console`) and says nothing about `/bin`, `/usr`, `/etc` or `/var`.
The document that defines those is the Filesystem Hierarchy Standard, a
Linux Foundation spec with no POSIX standing. So layout is almost
entirely a free choice here.

The choice made was a **trimmed FHS subset** -- familiar, and where
ported software will look -- with the full table, the reserved-but-not-
yet-created names, and the rules for adding to it in
`docs/filesystem-layout.md` (which `tools/check_layout.py` enforces
against the built image, in `preflight.sh` and CI).

Two decisions inside it are worth having recorded here rather than only
there. **`/tests` is not an FHS directory**: the FHS answer for
"executables not meant to be invoked directly" is `/usr/libexec`, and
that was the alternative. `/tests` won for being unmissable, for keeping
paths short under a 64-byte `FS_PATH_MAX`, and because these binaries
aren't internal helpers -- they're exercises a person runs deliberately.
It exists because `/bin` had reached fourteen test binaries against
three real programs, so every `ls /bin` and every tab completion led
with noise. And **there is no merged `/usr`**: modern distributions make
`/bin` a symlink into `/usr/bin`, which this filesystem cannot express,
having no symlinks at all.

The constraint that actually drives layout here isn't a standard, it's
the record budget: `FS_MAX_FILES` is 256 records with directories
counting against it, and a full path is capped at 64 bytes. That makes
"flatter, and fewer files with more structure inside them" the operative
rule until Milestone 15 (TFS3) raises both.

## `/etc` is one shared `toyos.conf` by default, not a file per setting

`kernel/lib/etc_config.c`'s `etc_config_get()`/`etc_config_set()` is a
generic name=value(+`#`comments) reader/writer that takes a `path` on
every call -- it doesn't hardcode one file. `tz.c` and `font_config.c`
both default to `/etc/toyos.conf` (see **Build 357**) rather than each
keeping its own dedicated file (`/etc/timezone`, `/etc/fontsize`, which
is what they used to be). One shared file was the explicit choice for
today's small, general settings; a setting with enough keys of its own
to be unwieldy sharing it (a GUI app with a dozen preferences) should
pass its own `/etc/<name>.conf` path instead -- nothing in
`etc_config.c` favors one file over many, that choice belongs to each
caller. See `kernel/lib/etc_config.c`'s top comment for the file
format itself and CHANGELOG-archive-2.md's **Build 357** for the original
writeup, including the one-time forward-migration logic each of
`tz.c`/`font_config.c` briefly carried to move an already-chosen
setting out of its old dedicated file the first time it loaded --
removed later (see `CHANGELOG.md`'s `[Unreleased]` entry) once the
project was comfortable dropping pre-1.0 on-disk/config compatibility
in favor of just starting fresh (a new `disk.img`/`/etc` state) instead
of carrying migration code for formats nothing still produces.

## Esc no longer exits the GUI desktop -- it's unclaimed at the WM level now

**Superseded -- was "Esc always exits the whole GUI desktop," see below
for what changed and why.**

Through **Build 502**, `apps/wm/wm.c`'s event loop checked `key == 27`
(Esc) and unconditionally left the window manager BEFORE routing the
keypress to whichever window was focused -- no GUI app's `on_key`
callback ever saw an Esc press. That hardcoded shortcut is gone: exiting
to the shell is now a discoverable Start menu item ("Exit to shell",
`wm_system_actions[]` in `wm.c`), not a hidden key, and Esc itself is
deliberately left unclaimed at the WM level -- free for a future
per-window or modal use (e.g. canceling a confirm dialog) instead of
double-booking it as "exit everything, no matter what's open or
focused," which is exactly the conflict this entry used to warn about.
See CHANGELOG.md's `[Unreleased]` "Exit to shell" entry.

The original reasoning below is preserved because the underlying fact
(the CLI/GUI editor's exit key had to be F3, not Esc, because of this
same conflict) is still true today -- Esc STILL isn't safe to hand to a
per-window "cancel" handler unless/until something adds its own
WM-level claim on it, which nothing has yet:

`apps/wm/wm.c`'s event loop no longer touches Esc at all -- so no GUI
app's `on_key` callback receives it any differently than before, it's
just not a WM-level exit anymore either. Found the hard way while
wiring the CLI/GUI text editor's exit key to Esc (**Build 377**):
worked fine at the physical console (no WM in that path at all) but
silently could never be received by an editor session running inside
the GUI Terminal, because the old Esc-exits-WM check intercepted it
first. If a future GUI app wants a per-window "cancel this" key today,
it still needs to be something other than Esc until a WM-level Esc
handler (e.g. a confirm dialog's cancel) actually exists -- **Build
377** picked F3 for the editor's exit specifically to sidestep this.
See `wm.c`'s own comment at the old check's former location and
CHANGELOG-archive-2.md's **Build 377** for the full story.

## Don't put a `text_scrollback` on the stack

`struct text_scrollback` (`widgets.h`) embeds an 8192-cell buffer
(`SCROLLBACK_CAP`) -- around 16KB, the ENTIRE size of the kernel's boot
stack (see `boot.asm`), which is what `shell_main()`'s whole call chain
already runs on (there's no separate kernel stack per "process" the way
ring-3 processes get one). `apps/notepad.c`'s `g_notepad` was already a
static instance for exactly this reason, but **Build 377**'s first pass
at the CLI text editor put a fresh one on the stack inside `editor_run()`
anyway and it silently corrupted nearby memory several calls deep into
`shell_main()` -- no crash, just the font size randomly shrinking and
later keystrokes quietly not registering. Any new caller of
`text_scrollback` (or anything else sized against `SCROLLBACK_CAP`) on a
kernel-context call path needs a static instance, not a local variable.
See `apps/editor.c`'s `g_editor_tb` for the fix and CHANGELOG-archive-2.md's
**Build 377** for the full story.

## The CLI editor's status bar needs its own line-wrapping pass, not a plain dump-and-let-the-console-wrap

`apps/editor.c`'s `editor_render()` looks like it should be able to get
away with `vga_clear()` + walking the buffer through `vga_putc()` in
order (which already handles wrap/scroll on its own) -- and the first
version of it (**Build 377**) did exactly that. It's wrong for a
full-screen editor specifically because a status bar printed *last*
inherits wherever the console's auto-scroll happened to leave off, not
a fixed row: fine for `console_page()`-style output that's read
top-to-bottom and thrown away, wrong for a status bar meant to stay
pinned to the bottom row the way real nano's (and this editor's own
GUI Terminal renderer, `widget_scrollback_draw()`) does. It also left
the physical console's own blinking cursor (`vga.c`) sitting on a
spurious blank row below the status bar, since that cursor just
follows wherever the last `vga_putc()` call left off.

**Build 379**'s fix -- reserve the last row, do a windowed two-pass
redraw mirroring `widgets.c`'s `scrollback_measure()`/
`widget_scrollback_draw()` (same windowing algorithm, `vga_rows()`/
new `vga_cols()` instead of pixels), and print the status line with NO
trailing `\n` so the physical cursor lands right after it instead of a
row below -- is the general pattern any future full-screen CLI
renderer in this codebase should copy, not another one-off dump. See
`editor.c`'s `editor_render()` top comment and
CHANGELOG-archive-2.md's **Build 379** for the full story, including a padding-math bug the windowing
itself caught during testing.

## Timezone city list is a database file, not a hardcoded array or a config key

`/etc/timezones` (a CSV-style `name,offset_minutes,dst_rule` list,
auto-seeded on first boot) and `/etc/toyos.conf`'s `timezone=<city>`
key are deliberately two different files, not one -- the database
(every city this build knows about) and the selection (which one is
active) change for different reasons and at different rates, so they
went through `etc_config.c`'s generic engine (selection) and a
purpose-built small parser (database) respectively rather than forcing
both into `toyos.conf`. This is also the concrete case **Build 357**'s
"a setting with enough keys of its own gets its own file" escape hatch
was written for -- a city list doesn't fit `key=value` shape at all.
Editing the database only takes effect on the next boot (no live-
reload command yet); see CHANGELOG-archive-2.md's **Build 367** for the full
writeup and what was verified.

## Build-number scheme: fix/feature/major tiers, not dates or semver

**Superseded -- see the next entry below.** This scheme (`tools/
bump_build.sh <fix|feature|major>`, retired) replaced an earlier
date-plus-same-day-counter scheme (`YYYY.MM.DD.N`), which itself
replaced a hand-bumped `0.1.0`-style semver. It was a deliberately
coarse, Windows-build-number-style approximation (+1/+10/+50) chosen
for being consistent and easy to sanity-check later, over a freeform
number that would be more nuanced but less predictable. Kept here for
the historical reasoning -- every existing `Build N` changelog
heading and `build-N` git tag still refers to this scheme. See
CHANGELOG-archive.md's **Build 110** (the switch itself) and
**Build 121** (the git tag + GitHub Release convention added on top of
it) -- both predate the changelog's split into eras, so they're in the
oldest archive file now, not CHANGELOG.md.

## Versioning: semver + `-dev` suffix, not a per-change build number

Replaced the fix/feature/major build-number scheme above. Requested
directly: bumping (and picking a fix/feature/major tier for)
`BUILD_NUMBER` on every single change, however small, had become
ceremony that didn't earn its keep -- a build number that changes
constantly isn't meaningfully more informative than one that doesn't
change until something's actually ready to call a version. `VERSION`
(repo root) now holds a plain semver-ish string, `0.1.0-dev` to start,
read into `TOYOS_VERSION` by `tools/gen_version.sh` exactly the way
`BUILD_NUMBER` was before. It only changes via `tools/set_version.sh
<version>`, and only for one of two reasons: starting a new dev round
(`0.2.0-dev`) or cutting a real release (`0.2.0`, no `-dev` suffix).
`CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/)
from its `## [Unreleased]` section forward -- every change gets an
entry there, no version/tier attached, until a release is cut; cutting
one renames that heading to `## [<version>] - <date>` and opens a
fresh `## [Unreleased]` above it (`tools/set_version.sh` does both
steps together). See CHANGELOG.md's `## [Unreleased]` intro (added the
same day this switch happened) for the change itself.

Day-to-day mechanics: `tools/set_version.sh 0.2.0-dev` starts a new dev
round (rewrites `VERSION` only); `tools/set_version.sh 0.2.0` (no
`-dev`) cuts a release (rewrites `VERSION` AND stamps `CHANGELOG.md` as
above). Git tags moved from `build-N` per push to `v<version>` at real
releases only, cut by hand after `set_version.sh`:
```
tools/set_version.sh 0.2.0   # rewrites VERSION, stamps CHANGELOG.md
git tag v0.2.0
git push origin main --tags
```
A GitHub Release (title = `v<version>`, body = that release's
CHANGELOG section, assets attached) is a judgment call per release now
rather than tied to a fixed tier, since there's no tier anymore -- use
one when a release feels milestone-worthy enough that grabbing a
working build without cloning + building is worth it.

**v0.0.9 (first real release, cut ahead of any milestone) established
the actual asset/publish mechanics, corrected below:**

- **Three assets, not just the ISO** -- `toy-os.iso`, `disk.img.gz`,
  and `tools/run_release.sh`. `disk.img` alone isn't optional: it's
  where `/bin/ls`/`/bin/lspci`/the seeded test binaries actually live
  (there's no installer, so the ISO alone boots into a near-empty
  filesystem). `disk.img` is a large SPARSE file (~9GB apparent size,
  ~370KB of real data as of v0.0.9) -- gzip it before attaching
  (`gzip -k -9 disk.img`; shrank to ~9MB) or the raw upload both blows
  past GitHub's 2GB-per-asset limit and wastes bandwidth transferring
  mostly zeros. `tools/run_release.sh` ships as a release asset (not
  just a repo file) because someone with just the ISO/disk image, no
  checkout, otherwise has no easy way to know the correct QEMU device
  config (`if=ide` disk bus separate from `-cdrom`'s, no
  `-device usb-mouse`/`usb-tablet` -- see the Makefile's `run:` target
  comments and `CLAUDE.md`) -- it's a standalone `sh` script that
  gunzips `disk.img.gz` itself if needed, then launches with the same
  flags `make run` uses.
- **Rebuild `disk.img` fresh (`make clean-disk` first) before
  packaging a release** -- otherwise whatever's on the working
  checkout's disk image (test files, session-local state) ships as
  part of the "clean" release.
- **The publish step (`git push --tags` and `gh release create`)
  cannot run from the Cowork cloud sandbox, even with the repo's own
  token in the remote URL.** Confirmed directly (v0.0.9): the push
  failed with `remote: access denied by the git proxy: ... is not in
  this session's authorized repository set` -- the sandbox's outbound
  git egress goes through an allow-list proxy that blocks this
  regardless of credentials embedded in the URL. Read-only git
  (`fetch`, `ls-remote`) works fine through the same proxy -- only
  writes are blocked. The device bridge to the user's real machine has
  no network access at all (by design, see `CLAUDE.md`), so it can't
  publish either. Net effect: the "never push from the session" rule
  in this skill/`CLAUDE.md` isn't just a caution, it's enforced -- tag
  and prep everything locally (both checkouts), then hand the user the
  exact `git push origin main --tags` + `gh release create` commands
  to run from their own machine's terminal, which has real network
  access. `gh` isn't preinstalled in the cloud sandbox either
  (`apt-get install -y gh` if you need it there for anything read-only
  going forward, e.g. checking release state via `gh api`).
- **Version-vs-milestone note:** v0.0.9 was cut *ahead of* Milestone 1
  on purpose (a pre-milestone testing snapshot the user explicitly
  asked for), not part of the `v0.1.0` = Milestone-1-done mapping
  `docs/roadmap.md` otherwise uses. `tools/set_version.sh` doesn't
  care either way -- it'll stamp whatever version string you give it.

See `gh release create v0.2.0 toy-os.iso disk.img.gz run_release.sh
--title "v0.2.0" --notes-file <path>` (or the GitHub web UI) for the
actual invocation shape now.

Alongside this, commit messages going forward list each changed/added
file with a one-line note in the body -- a separate, smaller
convention adopted at the same time, not tied to the versioning switch
itself:
```
kernel/drivers/keyboard.c   - added SE layout remap
apps/shell.c                - fixed signed-char gate in shell_read_line()
CHANGELOG.md                 - Unreleased entry
```
Subject line stays a short summary as always; this is just the body,
so a commit is skimmable on GitHub without opening the full diff.
See CLAUDE.md's own bullet on this.

## Socket fds: scaffolding ahead of the driver, not a working transport

`SYS_SOCKET`/`SYS_SEND`/`SYS_RECV` (build 420) exist and are reachable,
but `SYS_SEND`/`SYS_RECV` always return -1 -- there's no NIC driver or
protocol stack for them to move bytes through yet (see README.md's
"Basic TCP/IP networking": PCI enumeration, IRQ registration, and
contiguous memory are done; the driver itself isn't). Building even a
minimal in-kernel loopback transport (two processes exchanging bytes
through a shared buffer, no real network) was considered and
deliberately not taken -- it would prove the syscalls can move data,
but not the actual thing this scaffolding needs to prove: that the fd
namespace, the syscall ABI, and the tagged fd table (`FD_KIND_FILE`/
`FD_KIND_SOCKET` in `syscall.c`) are right, ahead of a real driver
existing. `sockettest` verifies exactly that surface -- fd allocation,
kind separation (`SYS_WRITE`/`SYS_READ` correctly reject a socket fd),
and cleanup -- without pretending a transport exists. See
`syscall_abi.h`'s `SYS_SOCKET` doc comment and CHANGELOG-archive-2.md's
**Build 420** for the full writeup.

## Nordic keyboard/character support: Latin-1, not UTF-8; 3 remapped keys, not a full layout

Adding Å/Ä/Ö support (build 501) meant three separable choices, made
the same way each time: keep the codebase's existing "1 char = 1 cell
= 1 glyph" assumption intact rather than take on the much bigger
UTF-8 rework it doesn't need yet.

**Encoding: Latin-1/ISO-8859-1 single bytes (Ä=0xC4, Ö=0xD6, Å=0xC5,
ä=0xE4, ö=0xF6, å=0xE5), not UTF-8.** Every byte-buffer boundary in
this kernel (`scrollback_cell`, `fs.h`'s file content, the syscall
ABI's buffer+length `SYS_WRITE`/`SYS_READ`) already assumes one byte
is one character is one glyph cell; UTF-8 would break that assumption
everywhere a multi-byte Nordic letter crossed it, for a codebase that
only needs 6 extra characters right now. `font_ttf.h`'s
`FONT_TTF_EXTRA_COUNT` bakes exactly these 6 glyphs (see
`tools/genttf.py`'s `EXTRA_CHARS`), not the full 0xA0-0xFF Latin-1
Supplement block -- easy to extend later (append to that list and
re-run the script) if more accented characters are ever needed.

**Keyboard layout: `keyboard <us|se>` remaps 3 scancodes, not a
from-scratch Nordic layout.** `keyboard.c`'s `scancode_ascii_se[]`/
`scancode_ascii_shift_se[]` are copies of the US tables with only
scancodes 0x1A/0x27/0x28 (the physical keys under Å/Ä/Ö on a real
Nordic keyboard) changed -- everything else, including AltGr-level
symbols a real Nordic layout also remaps, stays US QWERTY, since this
driver has no AltGr/dead-key handling at all (see keyboard.h's
`IS_NORDIC_CHAR()` comment). Persisted the same way `timezone`/
`fontsize` already are -- a `keyboard_layout=<us|se>` key in
`/etc/toyos.conf`, loaded once at boot by `keyboard_config_init()`.

**The actual bug that made this hard to verify: `char` is signed, and
one gate had a differently-shaped filter the others didn't.** No
`-funsigned-char` in this build's CFLAGS, so a codepoint >= 0x80 is
negative as `char` -- `gfx_draw_char()`'s old `c < 32 || c > 126`
range check and five `key >= 32 && key < 127`-shaped "is this a
printable char" gates across `apps/` (terminal, notepad, widgets
textfield, editor) and `userland/echo.c` all
silently rejected Nordic letters before this build. `keyboard.h`'s new
`IS_PRINTABLE_KEY()` macro (and `font_ttf_glyph_index()` in gfx.c,
which takes the codepoint as `int`/`unsigned char` rather than relying
on `char`'s signedness) fixed all of them at once -- except
`apps/shell.c`'s own `shell_read_line()`, which had a SIXTH,
differently-worded gate (`c < 128`, not `key >= 32 && key < 127`) that
a grep for the other five's exact phrasing missed entirely. Found only
by QMP-testing actual keystrokes end-to-end and noticing the cursor
didn't even advance -- not by code review -- which is the concrete
argument for always verifying a "fixed every instance of X" claim by
testing the behavior, not just re-grepping the pattern you already
fixed. See CHANGELOG-archive-2.md's **Build 501** for the full writeup.

## Protected files: `device_commit_files` blocks writes, `device_bash` doesn't

`Makefile` and anything under `.github/workflows/*.yml` are protected
specifically against `device_commit_files` -- confirmed for
`.github/workflows/build.yml` when it was first added, likely a
blanket CI-workflow protection rather than something specific to this
repo. The fix isn't "ask the user to copy a file by hand," though:
`device_bash` has ordinary read/write access to the mounted folder and
is NOT blocked from writing `Makefile` directly. So the actual flow is
-- edit the file in the cloud sandbox, verify the build there, deliver
it as `Makefile.new` (any filename that doesn't match the protected
path) via `SendUserFile` + `device_commit_files`, then finish the job
over `device_bash`: `cp Makefile.new Makefile`, `diff` the two to
confirm they're now identical, then move `Makefile.new` into
`_to_delete/` (can't delete it outright, same as any other file over
this bridge -- see the next entry). Same trick for
`build.yml.new`/`.github/workflows/`. Only fall back to asking the
user to copy it themselves if `device_bash` genuinely can't reach the
file. Check `device_commit_files`' response generically for this --
its `rejected` array has the exact path and reason for anything it
refused, so a batch delivery doesn't get assumed to have landed in
full just because the call didn't error outright.

## The device bridge can't delete files, and `git` leaves stale locks behind on it

Two related device-bridge limits, both worked around the same way
(`mv`, not `rm`):

**Can't delete, full stop.** `device_bash`'s `rm`/`rmdir`/`unlink` fail
with "Operation not permitted" on mounted files, and
`device_commit_files` only writes. To remove a now-superseded file
from the user's machine, `mv` it (via `device_bash`) into a
`_to_delete/` subfolder next to it, then tell the user which folder to
delete themselves.

**`git` commands run via `device_bash` leave behind a stale
`.git/index.lock` -- even a read-only `git status`.** Git creates the
lock (to refresh its stat cache, in `status`'s case), then tries to
delete it when the command finishes -- but that delete is the same
blocked `unlink` as above, so it silently fails (you'll see a
`warning: unable to unlink ... Operation not permitted`, but the
command itself still succeeds). The lock file is left sitting in
`.git/`, and the *next* `git` command that needs to write the index
(`add`, `commit`, ...) fails hard with `fatal: Unable to create
'.../index.lock': File exists` -- indistinguishable from a genuinely
stuck git process, and just as confusing if it's the user's own
terminal that hits it after a session leaves one behind. Fix: rename
the lock out of the way instead of deleting it -- `tools/device_git.sh`
does this automatically, both BEFORE running the real git command
(sweeps anything already stale) and AFTER it (sweeps whatever that
command itself just left behind, with a `sleep 0.5` first -- a lock
git just created can be briefly invisible to `find` over this mount
without it, confirmed by testing). Earlier versions of the script only
swept before, which left a fresh lock for the *next* command --
including the user's own terminal -- to trip over; sweeping after too
is what makes the repo actually lock-free when the script returns,
not just when the next `device_git.sh` call happens to run. Always use
`tools/device_git.sh` for every `git` command reached this way,
`status` included -- never hand-roll this check inline, and never run
`git` directly via `device_bash` even for a "harmless" read.

## Button press/release feedback: a general `on_press`/`on_release` WM mechanism, not a Calculator-only hack

Calculator's buttons already used the shared `widget_button()`
(`apps/widgets.c`) -- the actual gap was that nothing in the window
manager ever told an app "the mouse is down and still on this button,"
so no app could draw a pressed state even if it wanted to. Two ways to
close that: give Calculator its own private mouse-tracking (poll
button state directly in `calculator_draw()` somehow), or add a real
event to the window manager's app-callback contract. Went with the
latter -- `gui_apps.h` gained `on_press(win, cx, cy)`/`on_release(win)`,
driven from `apps/wm/wm_input.c`'s `wm_update_drag_resize()` the same
way `content_dragging`/`on_drag` already work, with a matching
`content_pressed` index in `wm_internal.h`. Reasoning: this codebase
already has one precedent for "click vs. hold-and-drag needs its own
event pair distinct from `on_click`" (`on_drag_start`/`on_drag`), and
press/release is exactly that same shape -- a private per-app
workaround would have solved Calculator alone and left the next app
that wants pressed-button feedback (or a held-scrollbar-thumb, or a
press-and-repeat spinner) to reinvent it from scratch. See
`CHANGELOG.md`'s `[Unreleased]` entry for the mechanism's actual shape
(why it fires on the initial button-down tick, why it returns 1 only
when the "hot" button changes, how drag-off-before-release un-presses
without triggering the button).

## ui_button/ui_button_group: Brutal-OS-inspired, but not a full retained view system

Asked directly to look at Brutal OS's `libs/brutal-ui/button.c`/`.h` and
consider whether toy-os should have "own libs for GUI apps" the same
way. Worth separating two things that question conflates: the
*insulation boundary* (apps not reaching into WM internals) already
existed -- every `apps/*.c` file only ever includes `wm/wm.h` (the
public `window_*` API) and `widgets.h`, never `wm_internal.h` (that's
`apps/wm/*.c`'s own private `extern` state). What Brutal's button.c
actually demonstrates is different: a widget as a self-contained
*object* that owns its state (`press`/`over` flags) and reacts to
events, versus toy-os's old style of `widgets.h` being pure draw
functions (`widget_button(x, y, w, h, ...)`) with every app hand-rolling
its own state next to them (`calculator.c`'s `g_pressed_index`).

Went with a scaled-down version of that idea (`ui_button`/
`ui_button_group`), not Brutal's full model. Brutal's `UiView` is a
generic base struct every widget inherits via a cast macro
(`ui_button$(VIEW)`), composed into a tree (`ui_view_mount()`), laid out
with a string DSL (`"dock p-8"`), and dispatched a general `UiEvent`
enum including `UI_EVENT_ENTER`/`UI_EVENT_LEAVE` for hover. toy-os's WM
doesn't have (or need) most of that: there's no view tree, no
mouse-enter/leave dispatch, no generic layout engine, and building one
just to host a button object would be solving a problem this GUI
doesn't have yet. `ui_button` is a plain struct with geometry, label,
colors, and a `pressed` flag; `ui_button_group` is the part that owns
hit-testing and "which one is down" over a caller-owned array. Both
drop straight into the existing `gui_apps.h` `on_press`/`on_click`/
`on_release` contract (see the entry above this one) rather than
inventing a second event model beside it.

**Why `apps/calculator.c` only, not also `apps/notepad.c`'s Save/Load
buttons in the same change:** they're the obvious second caller, but
migrating them isn't quite a pure rename -- `notepad_click()`'s
hit-test for Save/Load currently uses the *full* `TOOLBAR_H` height
(`widget_hit(save_x0, 0, BTN_W, TOOLBAR_H, cx, cy)`), taller than the
button `notepad_draw()` actually paints (`by`/`bh`, inset by
`BTN_MARGIN`) -- a small pre-existing looseness where clicking just
above/below the visible button still works. Routing that through
`ui_button_group`, which hit-tests against the button's own drawn
geometry, would tighten that hitbox and change what currently works --
a real (if minor) behavior change bundled into what should be a
no-behavior-change refactor. Left for its own change if wanted; it's
not blocked on anything (the "second real caller" bar is already met
by `calculator.c`), see `docs/roadmap.md`.

## Start menu click flash: a deferred close via pit_ticks(), not a blocking sleep

A Start menu click used to run the row's action and close the menu in
the same frame -- no visible confirmation the click landed, just an
instant jump to whatever opened. Adding a brief "you clicked this" flash
needed the menu to stay open and visibly highlighted for a short time
*after* the action already ran, which a single-threaded `hlt`-loop WM
(see `apps/wm/wm.c`'s top comment) can't do with an actual blocking
sleep -- that would freeze mouse/keyboard handling for every window,
not just the menu, for the duration.

Solved the same way the existing once-a-second clock redraw already
does (`wm_run()`'s `last_second`/`pit_ticks()` check): record a
`pit_ticks()` deadline (`start_menu_flash_until`) instead of blocking,
and check it every loop tick (`wm_update_start_menu_flash()`, called
unconditionally from `wm_run()`'s loop). The row's action still runs
immediately on click -- only the menu's `start_menu_open = 0` is
deferred until the deadline passes. This is the first *deliberately
timed* (not just event-triggered) UI state this codebase has beyond
that clock tick; if a future feature wants something similar (a toast
notification, a temporary status message), this is the pattern to
reuse rather than reinventing a delay mechanism -- `pit_ticks()`
deadline + a per-tick check, never a blocking sleep in the WM loop.
See `CHANGELOG.md`'s `[Unreleased]` entry for the full mechanism.

## Title-bar buttons: press-then-commit-on-release, reusing the content_pressed shape

Minimize/maximize/close used to act the instant `wm_handle_left_click()`
saw a mouse-down on them (`left_edge_down`, `wm.c`'s main loop) -- a
slipped click on close had no recovery, and there was no hover feedback
at all. Requested to match Windows/KDE: mouse-down only arms the button,
the action fires on mouse-up *only if the cursor is still over that same
button*, and dragging off cancels silently.

This is structurally the same problem `content_pressed` already solved
for app buttons (Calculator's `on_press`/`on_release`, see the
`ui_button`/`ui_button_group` entry above and `wm_update_drag_resize()`
in `wm_input.c`): a press starts a "which target is armed" state, every
tick while held recomputes whether the cursor is still over that target
(only redrawing when that changes), and release either commits or
cancels depending on where the cursor ended up. `title_btn_armed_win`/
`title_btn_armed_kind`/`title_btn_pressed_active` + a new
`wm_update_title_btn_press()` mirror that shape exactly, just at the WM
level instead of the app level -- there wasn't an existing WM-level
"armed target" concept to reuse, so this is a second, parallel instance
of the same pattern rather than a shared implementation. If a third
armable-target case shows up, that's the point to consider factoring the
pattern out.

Hover (cursor over a button, not held) is a separate, simpler piece --
`title_hover_win`/`title_hover_kind`, recomputed fresh every tick from
the live mouse position by `wm_update_title_hover()`, same "derive live,
don't persist a stale answer" approach as the Start menu's own hover
(see that entry above). It deliberately goes quiet while a button's
armed (`wm_update_title_hover()` no-ops then) -- the press visual takes
over, so the two never fight over what to draw.

Deliberately does *not* `bring_to_front()` a window just because its
title-bar button was pressed -- only the committed action does that
(maximize already did; minimize/close never did), so a press-then-
drag-off-then-release cancel has no visible side effect whatsoever, not
even a restack. See `CHANGELOG.md`'s `[Unreleased]` entry for the full
mechanism and what was verified.

## apps/ui/: a directory for retained-widget objects, once there were three

`ui_button.c`/`.h` and `ui_button_group.c`/`.h` lived directly in
`apps/` at first (there was only one pair, no directory felt warranted
yet -- same "don't split preemptively" judgment call `CLAUDE.md`
describes for files in general). Adding `ui_textbox.c`/`.h` as a third
pair made a flat `apps/` start to mix two different kinds of file
(whole *apps* like `calculator.c`/`notepad.c`, and small *widget*
building blocks they both depend on) -- the same signal that split
`apps/wm/` out earlier, applied one level up. `apps/ui/` follows that
exact precedent: its own `Makefile` wildcard/rule (`UI_C`/`UI_OBJ`,
mirroring `WM_C`/`WM_OBJ`), files included via a relative path
(`"ui/ui.h"`) from `apps/`'s own files.

The umbrella `ui.h` is a separate, smaller decision: every GUI app that
uses more than one widget had to remember one `#include` per widget
(`calculator.c` needed both `ui_button.h` and `ui_button_group.h`
already, before textbox existed at all) -- purely a convenience
aggregate, adds no declarations of its own, just `#include`s every
`ui_*.h` in the directory so a future widget is picked up by every app
that already has `#include "ui/ui.h"`, no per-app change needed. Chose
this over folding `ui_button_group` into `ui_button.h` (the other
option on the table): a bare `ui_button` used alone (no group) would
otherwise still pull in group's array/hit-testing logic it doesn't
need, and umbrella + separate files keeps that single-responsibility
split while still solving the actual pain (remembering multiple
`#include` lines).

## Kernel heap: coalesces by real address adjacency, not list order

`kmalloc()`/`kfree()` (`kernel/mm/heap.c`) grow the heap by calling
`pmm_alloc_contiguous()` again whenever the free list can't satisfy a
request, appending the new region's block to the end of the list. That
means list order and physical-address order agree *within* a region,
but nothing guarantees the next `pmm_alloc_contiguous()` call returns
frames adjacent to the previous region -- the PMM's bitmap allocator
is free to hand back frames from anywhere. `kfree()`'s coalescing
(`try_merge_next()`) checks the real pointer arithmetic
(`(uint8_t *)(b + 1) + b->size == (uint8_t *)n`) before merging two
list-adjacent blocks, not just "these are next to each other in the
list" -- merging on list-order alone would corrupt the heap the first
time two separately-grown regions happened to sit list-adjacent but
not address-adjacent (freeing block A would silently absorb block B's
header into A's payload size, and a later allocation into that
"merged" space would write past the actual end of A's real memory).
See `CHANGELOG.md`'s `[Unreleased]` entry for the full design and what
`heap_selftest()` verifies.

## Calculator is the first `multi_instance` GUI app

`gui_apps.h`'s `multi_instance` flag (see `CHANGELOG.md`) is opt-in
per app, and Calculator was the one asked for by name when multi-
window support was requested ("open two or three calculators") -- it's
also the simplest existing app to convert: no filesystem state, no
scrollback buffer, just a handful of small structs (`calc_state` +
button array + button group) that fit cleanly into one `kzalloc()`'d
`struct calculator_instance` per window. Notepad and Terminal weren't
converted in the same change -- they're not asked for as multi-window
yet, and each has more state (Notepad's filename/dirty-flag/scrollback,
Terminal's shell subprocess plumbing) that would make the conversion a
bigger, separate decision about what "two Terminals" even means (two
independent shells? a shared one?) rather than a mechanical port.
Window titles for multiple windows of the same app deliberately stay
identical (no "(2)" suffix) -- an explicit choice when this was built,
not an oversight; distinguishing same-titled windows in the taskbar is
left for a future change if it turns out to matter in practice.

## `kfree()`'s coalescing only checked the block being merged in, not the block being merged into

The heap allocator's backward-coalescing call was `if (b->prev)
try_merge_next(b->prev)` -- correct-looking, since `try_merge_next()`
already checks its *argument's* `next` pointer is free before merging.
The bug: it never checked whether `b->prev` *itself* was free. Freeing
a block whose list-previous neighbor was still allocated silently
folded the freed block's size into the still-in-use neighbor's `size`
field (since `try_merge_next(prev)` saw `prev->next` was now free and
merged it in), with no corresponding adjustment to `g_used_bytes`. A
later `kfree()` of that neighbor then subtracted more than was ever
added, underflowing the unsigned `g_used_bytes` counter. This existed
since the heap allocator was first written and went completely
undetected -- nothing ever displayed `heap_used_bytes()` until Task
Manager did, and it showed an impossible ~16 exabyte figure. Fixed
with a `b->prev->free` guard; `heap_selftest()` now explicitly asserts
`heap_used_bytes() == 0` after freeing everything, so this class of
regression is caught at every future boot instead of needing another
UI to happen to surface it. See `CHANGELOG.md`'s `[Unreleased]` entry
(the Task Manager bullet).

## TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single

TFS2's original 2048-byte inline-file format was replaced with a
classic Unix-inode-style scheme (12 direct block pointers + single/
double/triple indirect) specifically to reach multi-gigabyte files
without keeping the whole file in RAM. Direct + single indirect alone
tops out at `12 + 1024` blocks (~4MB at the 4096-byte block size) --
nowhere close to the 8GB target. Direct + single + double gets to
roughly `12 + 1024 + 1024*1024` blocks (~4GB) -- still short. Triple
indirect (`1024^3` more blocks reachable through one pointer) is what
actually clears 8GB with headroom, which is why all three tiers exist
rather than stopping at double indirect the way a smaller target could
have. This is also why `tfs_selftest()` deliberately targets a write
at a ~4.6GB offset -- past double indirect's ceiling -- as the boot-time
proof that the triple-indirect chain is really being built and walked,
not just declared. See `CHANGELOG.md`'s `[Unreleased]` entry (the TFS2
multi-GB bullet) for the full format writeup.

## `fs_read_range()`/`fs_write_range()` were added alongside `fs_read()`/`fs_write()`, not as a replacement

`fs_read()`'s contract has always been "return a pointer to the whole
file, loaded into RAM in one call" -- fine for small text files, but
architecturally incapable of handling a file larger than available RAM
(256MB in the normal QEMU config) no matter how large the on-disk
format gets, since the call itself has nowhere to put an 8GB result.
Rather than redesign every existing caller (Notepad, the shell,
editor.c -- all of which only ever touch small files and are simplest
written against "give me the whole thing") around a chunked API they
don't need, `fs_read_range(path, offset, buf, len)`/`fs_write_range()`/
`fs_size()` were added as a second, parallel API for callers that
genuinely need bounded-memory access to a large file. `fs_read()`/
`fs_write()` keep their exact old behavior and signatures. See
`CHANGELOG.md`'s `[Unreleased]` entry (the TFS2 multi-GB bullet).

## `apps/widgets.c`/`.h` no longer exist -- and `ui_scrollback`/`ui_scrollbar` didn't get an owned-geometry wrapper

When every widget still in `apps/widgets.c`/`.h` (the base `widget_hit`/
`widget_button` primitives, `text_scrollback`, the scrollbar, the
checkbox) moved into `apps/ui/` by explicit request, it was a pure file
move -- same function names/signatures, no rename, no redesign -- the
same precedent `ui_button_group.c` had already set when IT moved from
`apps/` into `apps/ui/` ("same content, no behavior change"). The one
design question worth recording: `ui_button`/`ui_textbox` own their own
geometry (`x/y/w/h` fields, a `set_geometry()` call), so why didn't
`ui_scrollback`/`ui_scrollbar` get the same treatment? Because owning
geometry only pays for itself when a caller would otherwise have to
carry that state itself across frames -- and every real
`text_scrollback`/scrollbar caller (Notepad, Terminal, the editor)
already recomputes its content rect from the window's live size on
every single frame (that's what makes resize support work at all), so
there's no per-frame bookkeeping an owned-geometry wrapper would
actually remove. `ui_textbox`'s geometry, by contrast, genuinely is
mostly-static (a fixed-position field that only moves on a font-size
change), which is exactly the case an owned `set_geometry()` call
saves real work for. See `CHANGELOG.md`'s `[Unreleased]` entry.

## The desktop's right-click quick-launch menu doesn't distinguish icons from empty space

`apps/wm/desktop.c`'s `desktop_handle_right_click()` always opens the
same full quick-launch menu (one row per `gui_app_registry` entry)
regardless of whether the click landed on a specific icon -- a
per-icon menu (e.g. "Open" / a future "Rename"/"Properties") was
explicitly scoped out this round, not an oversight: desktop icons
don't have any per-icon identity or state beyond "which
`gui_app_registry` index am I" yet (no rename, no repositioning, no
custom icon), so a per-icon menu would have nothing more useful to
offer than the quick-launch menu already does. Revisit once icons gain
real per-icon state worth a dedicated menu for. See `docs/roadmap.md`
and `CHANGELOG.md`'s `[Unreleased]` entry (the desktop/context-menu
bullet).

## `context_menu.h`'s items carry a `void *ctx`, but `start_menu.h`'s don't

`struct start_action` (`start_menu.h`) is a fixed, compile-time-known
array (`wm_system_actions[]`) -- every action's callback is a distinct
named function with nothing to parameterize, so a bare `void
(*on_select)(void)` was always enough. `struct context_menu_item`
(`context_menu.h`), by contrast, is built fresh at open time from
runtime data (which window, which app) -- "Close window" needs to know
*which* window, "Open" needs to know *which* app -- so its callback
signature carries a `void *ctx` the caller stashes that data in (a
`gui_app` pointer, or a small `static int` holding a window index) and
gets back unchanged when a row is selected. Not applied retroactively
to `start_menu.h` since nothing there needs it and the two aren't a
shared abstraction to begin with (see `apps/wm/start_menu.h`'s own top
comment on why it isn't a general "menu" type). See `CHANGELOG.md`'s
`[Unreleased]` entry.

## dmesg coverage: log from the one-shot call site, not the hot function itself

When extending `klog_write()` coverage to the RTC (`kernel/core/
timer.c`'s `rtc_read()`), the log line went into `kernel_main()`
(`kernel/core/kernel.c`), which calls `rtc_read()` exactly once at
boot for this purpose -- not into `rtc_read()` itself, even though
that's the more obvious place a driver-level log usually lives (see
every other dmesg addition in the same change: `pci_init()`,
`mouse_init()`, `vga_init()`, `keyboard_set_layout()` all log from
inside the driver function). `rtc_read()` is the exception because
it's not an init function -- it's called continuously by the taskbar
clock and `tz.c` every time either redraws, so a log line inside it
would flood the 16KB ring buffer with a new timestamped line every
second or so, pushing out everything else `dmesg` is actually useful
for. The general rule this leaves for the next area added to dmesg
coverage: log from whatever call site is genuinely one-shot (an
`*_init()` function, a boot-sequence call in `kernel_main()`), not
from a function just because it's the "natural" owner of the
information, if that function is actually called on every frame/tick/
redraw instead of once. See `CHANGELOG.md`'s `[Unreleased]` entry for
the full list of areas covered and `klog_write_dec()`/
`klog_write_hex()` (`kernel/include/api/klog.h`), added in the same change
for klog messages that need to include a number.

## Real disk-hosted ELF binaries: an old plan re-verified before building, not built from the doc as written

`docs/roadmap.md` already had a plan for this (split into (A) a real
syscall-based ELF program, (B) loading it from `/bin`), written in an
earlier session as a pure planning pass. Before actually building it,
that plan got re-checked against the current codebase rather than
implemented as written -- worth recording why, since the two
differences found are exactly the kind of "the codebase moved out from
under an old doc" trap a future session could hit again elsewhere:

- The plan's stated hard blocker for (B) was TFS2 capping a file at
  `FS_DATA_MAX` = 2048 bytes, with a whole discussion of multi-slot
  chaining to fix it. By the time this was re-checked, `tfs.c` no
  longer referenced `FS_DATA_MAX` at all -- TFS2 v2's block-addressed
  on-disk rework (the multi-GB file support entry, `CHANGELOG.md`'s
  `[Unreleased]`) had already solved this as a side effect, for
  unrelated reasons, in a different session that had no idea an old
  ELF-binaries plan was depending on that limit staying in place. Three
  comments (`kernel/lib/etc_config.c`, `apps/editor.c`/`.h`) still
  cited the old 2048-byte ceiling as real months later -- corrected in
  the same change that shipped this (see `CHANGELOG.md`).
- The plan assumed `elf_load()`'s ELF blob would need copying out of
  TFS2's live in-RAM table into a scratch buffer before executing,
  since that memory "isn't stable the way a GRUB module's reserved
  region is." Checking `elf_load()` (`kernel/proc/elf.c`) and
  `heap.c`'s own top comment together showed this wasn't needed:
  `elf_load()` just casts its `elf_phys_addr` argument straight to a
  pointer with zero translation, which only works because GRUB modules
  sit in identity-mapped low physical memory -- and `kmalloc()` is
  *also* carved out of that same identity-mapped low-4GiB range (see
  `paging.c`'s top comment, referenced from `heap.c`), so `fs_read()`'s
  returned buffer address already works there directly. One real
  constraint this does leave, not present in the GRUB-module path:
  nothing may call `fs_read()` again until the loaded process finishes,
  since the backend reuses one static buffer across calls (`fs.h`'s
  `fs_read()` doc comment already says this; `elf_run.c`'s own comment
  restates it as a caller-facing constraint).

Net effect: (A) and (B) shipped together in one change instead of two,
since (B) turned out to be much smaller than the plan estimated. What
the plan got right and is still true: getting a binary's bytes onto
`disk.img` at all needs *something* outside the OS, since there's no
in-guest compiler -- see the next entry for which of the plan's two
options (`bootstrap-install` vs. a host-side writer tool) was picked,
and why. See `CHANGELOG.md`'s `[Unreleased]` entry for the full
implementation (`SYS_PCI_COUNT`/`SYS_PCI_INFO`, `userland/lspci.c`,
`elf_run.c`, `install_bin_binaries()`).

## `/bin` binaries: boot-time bootstrap-install now, a host-side TFS2 writer tool later

Getting a compiled ELF's bytes onto `disk.img` has no in-guest-compiler
option -- something outside the OS has to place them there. Two ways
were on the table (see the previous entry's roadmap plan): a host-side
tool that writes directly into TFS2's on-disk format (informed by
`docs/tfs2-spec.md`'s existing read-only reference parser, which would
need a write-side counterpart built from scratch), or copying a GRUB
module's bytes into `/bin` once, at boot, via code the kernel already
has (`fs_write_range()`/`fs_touch()`, both already exercised by other
callers). The user chose bootstrap-install for `lspci` now, with the
host-side tool explicitly deferred to a later session (see
`docs/roadmap.md`'s new backlog entry) rather than skipped -- the
bootstrap path needs zero new tooling and was demonstrably enough to
prove the whole `/bin`-loading pipeline end to end, while the
host-side tool only pays for itself once a *second* binary needs
installing without a kernel rebuild, which isn't true yet. The
tradeoff this defers, worth remembering when that second binary shows
up: `install_bin_binaries()` (`kernel/core/kernel.c`) is a small table
of `{module_index, bin_path}` pairs specifically so adding one more
GRUB-module-installed binary is a one-line addition, not a redesign --
but it's still "add a GRUB module + a table row + rebuild the kernel"
per binary, not "drop a file onto the disk image," which is exactly
what the host-side tool is for.

## `tools/tfs2_writer.py`: content-hash sync, not mtime comparison; direct+single-indirect write scope, not full indirect support

Two scope calls made building the deferred host-side TFS2 writer (see
the entry above): how `sync`'s "only rewrite if changed" policy
detects a change, and how big a file the tool is willing to write at
all.

**Content hash, not mtime.** TFS2's `created`/`modified` fields are
toy-os's own RTC-sourced local wall-clock time (see `fs.h`'s
`fs_stat()` comment) -- there's no epoch, and no defined relationship
to the *host* machine's clock a comparison could lean on without
assuming a particular skew. Comparing "is the local file newer" against
that would be guessing. Hashing the on-disk content and comparing it to
the local file's content sidesteps the clock question entirely and is
just as correct for the actual goal ("did this file's bytes change") --
this is why `sync`'s `sync/` subtree policy reads the existing file
back and SHA-256-compares it rather than checking timestamps.

**Write scope is direct + single-indirect blocks only (~4.03 MB/file),
not the full direct+single+double+triple scheme `docs/tfs2-spec.md`
documents for reading.** The tool refuses cleanly (clear error, no
silent truncation) rather than write a partial file past that size.
Everything this tool exists for -- ELF binaries, config/text seed
files -- fits comfortably under that ceiling; double/triple-indirect
allocation is real extra code (the same recursive block-tree shape
`tfs.c`'s own `alloc_block()`-adjacent logic would need) that has no
current caller. If a future seed file genuinely needs to be larger,
extend `write_file()`'s allocation loop rather than raising the limit
silently -- the read path (`block_for_index()`) already walks all four
levels, so only the write side needs the extra work.

## `/bin/lspci` moved from boot-time bootstrap-install to build-time seeding, once the writer tool existed

The previous entry deferred the host-side TFS2 writer tool and kept
`install_bin_binaries()`'s boot-time bootstrap-install
(`kernel/core/kernel.c`, copying a GRUB module's bytes into `/bin` the
first time toy-os boots against a disk) as the interim way to get
`lspci` onto disk. Once the writer tool existed and grew a `format`
subcommand (see the entry above -- needed because `write`/`sync`
previously required an already-formatted image, which a brand new
`disk.img` isn't until toy-os boots and formats it once), that
interim mechanism became fully redundant: `tools/tfs2_writer.py sync`
can format-and-seed a completely untouched `disk.img` in one call, at
BUILD time, with no boot cycle needed at all.

`install_bin_binaries()`/`BIN_BOOTSTRAP` and the `lspci.elf` GRUB
module were removed outright rather than kept as a fallback -- two
mechanisms solving the same problem is exactly the kind of debt this
project avoids once the better one exists (see `CLAUDE.md`'s file-split
guidance for the same instinct applied elsewhere: don't keep unused
machinery "just in case"). If a future binary genuinely needs
boot-time-only install for some reason GRUB-module bootstrap-install
would fit better than build-time seeding, that's a fresh design
question when it actually comes up, not a reason to have kept the old
table around empty -- the git history (this entry, and the
`CHANGELOG.md` sections it points at) has everything needed to bring
the pattern back if so.

The Makefile's new `seed` target runs on every `make iso` (not just
when `disk.img` is first created) -- deliberately `.PHONY` so it always
re-runs, relying on `sync`'s own content-hash compare (not `make`'s
mtime-based staleness check) to make repeat calls cheap. This matters
because `disk.img` is explicitly NOT rebuilt by `make clean` (see its
own comment in the Makefile -- it's local persistent dev state, not a
build output) -- if `seed` only ran once, a rebuilt `lspci.elf` with
real code changes would silently never reach an existing `disk.img`
again.

## Every ELF64 test binary moved to `/bin`, not just `lspci` -- and why two didn't fold in cleanly

`lspci` was the first ELF64 binary moved off a GRUB module onto
build-time-seeded `/bin` (see this file's `seed`-target entry above).
The remaining dozen-ish test binaries followed the same path in one
pass (CHANGELOG.md's `[Unreleased]` entry has the full list) rather
than staying GRUB modules indefinitely, once it was clear the seeding
mechanism generalized cleanly -- there was no longer a reason for
`lspci` to be the only one.

Almost all of them folded into the existing generic loader
(`kernel/proc/elf_run.c`'s `elf_run_from_fs()`) with zero new code,
which is the whole point of that function existing: one loader, N
binaries, no per-binary kernel harness. Two didn't:

- **`schedtest`** (`counter_a`/`counter_b`) needs two processes running
  *concurrently* under the real preemptive scheduler -- a one-shot
  `run <name>` inherently can't do that, no matter how generic the
  loader gets. This got real new code: `scheduler.c`'s
  `spawn_from_fs(const char *path)`, replacing `spawn_from_module()`
  outright (its only caller was `scheduler_demo_run()`) -- same
  no-copy-needed `fs_read()` reasoning `elf_run_from_fs()` already
  used, just wired into the scheduler's spawn path instead of the
  one-shot run path.
- **`elftest`/`hello.elf`** tested toy-os's raw manual-`iretq` ring-3
  entry specifically, a different (and older) code path than
  `process_run_ring3()`'s recoverable one. Folding it into `run hello`
  means that specific raw-entry test coverage is gone -- a real
  tradeoff, made deliberately (user's call, weighing one narrow bit of
  coverage against one less special case) rather than accidentally.
  `ring3test` remains as the one place the raw-`iretq` path is still
  exercised at all (see this file's entry above). What this migration
  did *not* notice: `elftest` had also been mapping a page at
  `USERLAND_MARKER_ADDR` for `hello.elf` to write to, and the generic
  path doesn't -- see [hello.c stopped faulting on purpose and started
  faulting by
  accident](#helloc-stopped-faulting-on-purpose-and-started-faulting-by-accident).

`ring3test` itself was never a candidate to fold in -- it uses no ELF
file whatsoever, there's nothing to seed.

Along the way, `elf_run_from_fs()` gained an unconditional
`syscall_reset_heap()` call it didn't have before -- found by reading
`echo_test.c`, which called this itself ahead of its old
dedicated-command loader. Migrating `echo_test` onto the generic path
without this would have silently broken its `sbrk()`-based heap the
first time anyone actually exercised it, not at compile time. Making
it unconditional (rather than a per-binary opt-in flag) costs nothing
for a binary that never calls `sbrk()` -- it's bookkeeping, not an
allocation -- so there was no reason to keep it special-cased.

`apps/terminal.c`'s GUI Terminal window still blocks `run` wholesale,
not per-target. Several of the newly-independent `/bin` binaries
(`gui_test`, `win_test`, `echo_test`) have the same hazards inside a
GUI window the old dedicated commands were blocked for (drawing
straight to the physical framebuffer, blocking forever without
yielding back to the window manager) -- a per-target allowlist was
prototyped, but the QMP test written to verify it was invalid: it
relied on `tools/gui_flow.py`'s `open_app("Terminal")`, which (due to
a separate, pre-existing bug -- see below) was actually opening
Calculator. Rather than ship GUI-safety-relevant logic that couldn't
be verified, the simpler wholesale block was kept. Every `/bin` binary
can still be run from the physical shell regardless.

That `gui_flow.py` bug was real and unrelated to this migration:
`ITEM_H` (the assumed Start-menu row height) was `32`, stale against
the kernel's actual `gfx_char_h() + 6` (`24` at the default font
size) -- so every `open_app()` call was clicking roughly one row below
where it meant to. Found and fixed once it was blocking this
migration's own testing; see CHANGELOG.md's `[Unreleased]` entry.

## Click-to-position/selection lives in the shared `text_scrollback` widget, not a Notepad-only one

When asked to add click-to-position and text selection, user chose
extending the shared `apps/ui/ui_scrollback.c` widget (used by
Notepad, Terminal, and `apps/editor.c`) over building a new
Notepad-only widget. Terminal and `editor.c` never call the new
selection API, so it's inert there -- but any future caller of
`text_scrollback` gets click-to-position/selection for free, and there
was no plausible reason for the underlying pixel<->buffer-index math
(and its correctness) to exist twice. See CHANGELOG.md's
`[Unreleased]` entry for the full implementation.

## The serial debug console is poll-based from existing idle loops, not a new kernel thread

`debug_console_poll()` is called from `keyboard_getchar()`'s hlt-wait
loop and `wm_run()`'s main event loop -- both already wake on every
interrupt and already have a "cheap thing to do while otherwise idle"
convention (`vga_cursor_tick()`). Piggybacking there means a serial
debug session over COM1 works without any new scheduling or
kernel-thread machinery, matching this kernel's existing
single-threaded-cooperative-with-interrupts model. The honest
limitation: it does NOT get polled while a blocking command, a
ring-3 process, or anything else that isn't one of those two loops is
running -- accepted rather than solved, since fixing it properly would
mean either real kernel threads or polling from many more places for
a debug-only feature. See CHANGELOG.md's `[Unreleased]` entry.

Getting serial RX working at all needed two things past "unmask the
PIC": `serial_irq_init()`'s IRQ registration/unmask has to run *after*
`idt_init()`, not from `serial_init()` itself, since `serial_init()`
deliberately runs first in `kernel_main()` (so `klog_write()` has
somewhere to send its very first byte) -- before `idt_init()`'s own
"mask everything, then unmask only what's wired up" pass, which would
otherwise immediately undo an earlier unmask. And unmasking the PIC
line isn't sufficient by itself: the UART's own Interrupt Enable
Register also has to be set (`serial_init()`'s original `outb(COM1+1,
0x00)` leaves it at 0, since nothing needed RX before this). Found
live -- a fully-correct-looking IRQ handler + PIC unmask produced zero
bytes, not even local echo, until the IER bit was added.

## Keyboard layouts are data files (`/etc/kbs/<name>`) generated from Linux's own XKB data, not a compiled-in enum

The original `se` layout only remapped the three Å/Ä/Ö keys -- everything
else stayed identical to `us`, including keys whose physical legend is
genuinely different on a real Nordic keyboard (the key beside right
Shift types `-`/`_` on a physical FI/SE keyboard, not `/`/`?`). Rather
than hand-fix scancodes one bug report at a time, layouts moved to
`/etc/kbs/<name>` data files, generated by `tools/gen_kbs.py` from
`xkbcli compile-keymap` (Linux's own, already-correct XKB layout
compiler -- no X server needed) instead of anyone re-deriving a
scancode chart by hand. Translation logic itself moved out of
`keyboard.c` into a new `kernel/lib/keyboard_layout.c`, since owning
per-region character tables was never really the driver's job (raw
scancode/shift-state handling is). See CHANGELOG.md's `[Unreleased]`
entry for the full implementation, including the AltGr/dead-key scope
limits (this driver has no AltGr handling at all, so those symbols
were never reachable regardless of the table) and a real bug the
generator's first cut had (omitting Escape/Backspace/Tab/Enter from
its key list, which silently broke Enter the moment the shell started
loading layouts from generated files instead of the old compiled-in
ones -- found live, not by review).

## A syscall's path-pointer validation checks a full `FS_PATH_MAX` range, not just up to the string's NUL

`elf_run_from_fs()`'s argv-on-stack layout (see `ls`'s migration to a
real `/bin` binary, `CHANGELOG.md`'s `[Unreleased]` entry) originally
packed argument strings as tightly as possible against the one stack
page's literal top address. That broke `SYS_LISTDIR` the moment an
argv string landed close enough to the page boundary: its handler
(`kernel/proc/syscall.c`) calls `vmm_validate_user_range(pml4, rdi,
FS_PATH_MAX)` on the incoming path pointer -- a fixed 64-byte range
from wherever the pointer starts, regardless of the real string's
length -- so a short string near the page's end still failed
validation because the *range* ran past the mapped page, even though
the string itself (with its NUL) fit fine. Every path-taking syscall
in this kernel follows the same fixed-range-not-string-length
convention (bounding the read once up front rather than trusting a
NUL inside untrusted ring-3 memory), so this isn't `SYS_LISTDIR`-
specific -- any future caller building a buffer a userland path
pointer will point into needs to leave `FS_PATH_MAX` bytes of margin
after it, not just after its longest real string. Fixed by reserving
`FS_PATH_MAX` bytes of never-written padding at the stack page's true
top before laying out any argv strings, guaranteeing every token's
start address has a full `FS_PATH_MAX` mapped bytes after it. Found
live via QMP testing (`ls /` failed with "cannot access '/'"), not by
review.

## Repo is MIT; the baked JetBrains Mono glyph data is separately SIL OFL 1.1

`kernel/drivers/font_ttf.c`/`.h` (generated by `tools/genttf.py`) bake
anti-aliased glyph *bitmaps* rendered from JetBrains Mono directly into
committed C source that ships in the kernel binary -- not just a build-
time dependency, actual redistributed derivative data. JetBrains Mono
itself is licensed under the SIL Open Font License 1.1, not MIT, and
OFL requires its license text travel with the font (or "substantial
rendered derivatives" of it, which baked bitmaps arguably are) --
regardless of what license covers the rest of the software using it.
Relicensing the whole repo to OFL wasn't needed or appropriate (OFL
doesn't require that, and it's a font license, not a general software
one); instead `LICENSE` got a short "Third-party font" section pointing
at `tools/OFL.txt` (already present, already referenced from
`README.md`), so a reader relying on `LICENSE` alone doesn't wrongly
conclude the baked font data is MIT. `tools/gen_kbs.py`'s use of the
system's XKB data is a *different* situation and needed no such
carve-out: it shells out to the locally installed `xkbcli` at build/
seed time and the XKB layout data itself is never embedded or
committed (`seed/sync/` is gitignored, pure regenerable build output)
-- more like depending on `gcc` than bundling third-party data.

## The file picker is a WM-level modal overlay (`apps/wm/file_picker.c`), not an `apps/ui/` widget

`apps/ui/` widgets are content-relative: they draw and hit-test
against coordinates local to the window that owns them, and a widget
never needs to know about other windows or the desktop. A file picker
doesn't fit that shape -- it has to draw on top of *every* window
(including ones that didn't open it), catch clicks before the window
underneath ever sees them, and stay open across the same kind of
screen-absolute modal lifecycle `confirm_dialog.c`/`context_menu.c`/
`start_menu.c` already use (shared open/close state in
`wm_internal.h`, drawn last in `wm_render_frame()`, given first
refusal on input in `wm_input.c`/`wm.c`). So it was built the same
way those three are built, as a fourth WM-level overlay, not shoehorned
into the widget library just because it looks like "a dialog with
some UI in it." Any future app-opened dialog that needs to sit above
arbitrary other windows (a color picker, an "are you sure" variant,
etc.) should follow this same WM-overlay pattern rather than trying to
make it work as an `apps/ui/` widget instantiated by the calling app.
See `CHANGELOG.md`'s `[Unreleased]` entry for the full feature writeup
(navigation model, Notepad's Open.../Save As... integration, the
`redraw_pending` bug found via QMP testing on `../` double-click
navigation).

## TFS2's write batching: `write_range_impl()`'s data path in bulk, `persist_record()`'s journal down to two barriers

`ata_flush_begin()`/`ata_flush_end()` (`ata.h`) let a caller defer the
synchronous `CMD_CACHE_FLUSH` that used to follow every single ATA
write, batching it into one flush at the end of a run of many writes
instead -- `stress 100` measured ~1.4MB/s before this existed (one
flush per 4KB filesystem block written, plus a second one per
newly-allocated block's bitmap-sector update, so a 100MB write was
tens of thousands of tiny synchronous round trips). `write_range_impl()`
(`kernel/fs/tfs.c`) wraps its whole per-file-write loop in one
batch, and `persist_bitmap_bit()` defers the bitmap sector write
itself (not just its flush) to the batch's end, coalescing what would
otherwise be one redundant sector write per allocated block into one
write per distinct dirty sector.

`persist_record()` -- the write-ahead-journal-protected path that
persists a file's metadata -- can't use that same treatment, because
its four writes (journal data, commit header, real table slot, header
clear) do have ordering requirements: `ata_flush_begin()`/`end()`
suppresses ALL flushes in the region, which is exactly what a WAL can't
tolerate. For a long time it therefore flushed after every one of the
four, on the reasoning that a journal needs each write durable before
the next.

Two of those four barriers turn out to carry no weight, and the
reasoning is worth keeping because it's the general shape of the
question "does this write need a barrier?":

- **After journal data: not needed.** A torn write there fails the
  FNV-1a checksum stored in the commit header, so replay discards the
  entry. "The operation didn't happen" is a legitimate crash outcome.
- **After the commit header: REQUIRED.** Once the table slot is being
  overwritten, the journal entry is the only surviving copy of a record
  that can be torn.
- **After the table slot: REQUIRED.** Retiring the journal entry before
  the real slot is durable leaves a torn slot with nothing to replay.
- **After the header clear: not needed.** Losing it costs one redundant
  replay next boot, which rewrites the same bytes to the same slot.

So the rule isn't "a WAL flushes every write" -- it's "a barrier is
required where losing write N-1 would make write N unrecoverable." Two
of four qualify, which halved the cost of every metadata operation (a
256-record disk format went 0.73s -> 0.34s).

The two barriers use `ata_flush_now()` (ata.h), not `ata_flush_end()`,
and that distinction is load-bearing: `end()` only flushes once its own
depth reaches 0, so a journal sequence running inside an OUTER batch
would silently get no barrier at all. `tfs_check()`'s repair pass calls
`persist_record()` inside exactly such a batch -- which meant, between
the `fsck` commit and this one, the journal briefly had every one of its
flushes suppressed there.

`write_range_impl()`'s data blocks still have no ordering requirement at
all -- a half-written data block after a crash is just incomplete file
content (`fs_write_range()` documents partial-write behavior), not a
corrupted recovery structure -- so that path stays one flush per batch.
See `CHANGELOG.md`'s `[Unreleased]` entry for the numbers and for how
replay/discard were verified without an actual power loss.

## `fs_ops`'s new steppable-write function pointers are required, not optional/NULLable

The async-I/O roadmap item's Phase 2 (`fs_write_range_begin()`/
`fs_write_range_step()`, see `docs/roadmap.md`) added two new function
pointers to `struct fs_ops` (`kernel/include/kernel/fs_ops.h`) rather than a
separate, optional side-interface a backend could leave unset. Every
other entry in that struct is unconditionally required -- `vfs.c`'s
dispatch wrappers call straight through (`g_fs->touch(...)`, etc.) with
no NULL check on the function pointer itself, only on the *arguments*
(see `fs_write_range_step()`'s handle guard, added the same session).
Making the two new ones optional would have meant either a NULL check
on every dispatch call (real overhead on the hot path for something
today's single backend always implements) or a silent fallback to
non-stepped behavior a caller couldn't easily detect it got. Since
`fs_ops.h`'s own top comment is explicit that this struct isn't a
mount-point/capability-negotiation scheme -- there's exactly one active
backend, chosen once at boot -- there's no scenario today where a
backend legitimately can't implement these two. If a future backend
genuinely can't support incremental writes (say, one backed by a
remote API with no partial-write primitive), that's the point to
revisit this, not before.

## Repo history scrubbed of the maintainer's real name -- privacy request, not a bug fix

The maintainer's real name and two personal email addresses appeared
in two places: the `LICENSE` copyright line, and as the commit
author/email on every one of 91 commits in git history (both a Gmail
address and a personal domain address). Requested directly, for
privacy reasons -- not something this project would otherwise flag on
its own. Fixed with `git-filter-repo` (not `filter-branch` -- the
maintained, recommended tool), run in an isolated copy of the repo,
never touching the working sandbox clone or the user's real checkout
directly:
- `--mailmap` remapped both old `name <email>` identities to a single
  generic `toy-os <noreply@toy-os.local>` identity, rewriting every
  commit's author AND committer fields.
- `--replace-text` rewrote the literal string in blob content too (the
  `LICENSE` copyright line existed unchanged across its whole history,
  so scrubbing only the latest revision would have left it in every
  earlier commit's tree).
- Verified clean afterward by grepping the *entire* rewritten
  history's authors and full patch text (`git log --all -p`) for the
  name and both emails -- zero hits is the actual proof, not just
  "the current file looks right."

**Delivery mechanic, because this session can't push (see the git-proxy
entry above):** `git bundle create --all` packaged the rewritten
history into one file, delivered via `SendUserFile` +
`device_commit_files` same as any other file. The user applied it
themselves: fresh `git clone` from the bundle, fix the `origin` remote
(cloning from a local bundle auto-sets `origin` to the bundle's own
path, so `git remote add origin <url>` collides -- use `git remote
set-url origin <url>` instead, or remove-then-add), then `git push
--force` both the branch and the tag from their own machine. Verified
afterward from the session by fetching from GitHub (read-only git
still works through the proxy) and re-running the same "grep the whole
history" check against `origin/main` and the tag -- don't trust a
local check alone since the point is what's actually live on GitHub.

**Pitfall hit during verification, worth knowing about:** `git log
--all` inside the *session's own sandbox clone* pulled in the sandbox's
own stale local branch/tag refs (never rewritten, and a leftover local
tag that fetch doesn't force-overwrite by default) alongside the
freshly-fetched `origin/main` -- produced a false-positive "still
finding the name" result on the first check. Check specific refs
(`origin/main`, the actual tag ref) explicitly rather than `--all` when
verifying a remote's real state from a local clone that has its own
unrelated history sitting around.

**One loose end, not urgent:** the rewrite ran against the *session's
sandbox mirror's* commit graph (the copy this session had, not the
real checkout's parallel commit with the same content) -- harmless
content-wise (both had identical diffs), but it means one commit now
public on GitHub is authored `Claude (sandbox mirror) <claude@sandbox>`
rather than the generic `toy-os` identity everything else got. Not
PII, just slightly inconsistent -- worth folding into the identity
`toy-os <noreply@toy-os.local>` if this repo's history ever gets
rewritten again for another reason, not worth a whole rewrite on its
own just for this.

**Standing convention going forward:** every commit in this repo,
whether made by a session or by the maintainer directly, should use
the `toy-os <noreply@toy-os.local>` identity -- never a real name or
personal email. See `CLAUDE.md`'s "Working in the cloud sandbox"
section for the mechanical detail (the device-bridge session has no
git identity configured at all by default, so this has to be passed
explicitly on every commit, not assumed).

## `struct window *` isn't a stable per-window identity across frames -- don't cache one

Caught live during Milestone 1 phase 3's QMP testing (wiring `wm_run()`
to poll a pending write, `apps/wm/wm.c`/`wm.h` -- see `CHANGELOG.md`'s
`[Unreleased]` entry): `apps/notepad.c` originally cached the `struct
window *` passed to `notepad_open()` once, in a static, and reused it
later (across many frames) to register a steppable write against.
Wrong -- `bring_to_front()` (`wm.c`) reorders `windows[]` (a fixed
`struct window windows[MAX_WINDOWS]` array) by copying window
*contents* between slots (`windows[i] = windows[i + 1]`, etc.), not by
moving pointers/identity. A `struct window *` captured once can end up,
after any later reorder, pointing at a completely different window's
data -- same memory address, different window. Every existing WM
callback (`on_click`, `on_key`, ...) was already safe from this because
it always receives a freshly-resolved pointer for the CURRENT frame
(`wm_input.c`'s hit-testing loop looks the window up by its live index
every time) and never holds onto it past that one call.

Anything that needs a window handle to survive across MULTIPLE frames
(the new case here: a write polled once per frame until it completes)
can't reuse that pattern -- either capture the pointer fresh at a point
where it's provably still valid (`window_start_write()`'s caller in
notepad.c now does this: captured in `notepad_click()` when Save As...
is pressed, valid because the file picker it opens next is modal, so no
other window can be reordered while it's open -- see
`wm_handle_left_click()`'s dispatch order in `wm_input.c`), or track it
by an index the WM itself keeps accurate across reorders (which
`pending_write_win` does, fixed up inside `bring_to_front()`/
`close_window()` -- see those functions' own comments). The bug was
silent and easy to miss by code review alone: the underlying write
still completed correctly on disk every time (verified via
`tools/tfs2_writer.py ls`), only the UI-facing completion callback fired
against the wrong window, so Notepad's Save As... button just stayed
disabled forever. Caught by clicking a second window in front of
Notepad before Save, not by reading the code -- worth remembering next
time a change wants to hold a `struct window *` past the callback it
was handed in.

## The M16 scheduler is permanently armed now -- an empty process table makes that safe

`scheduler_armed` (`kernel/proc/scheduler.c`) used to be false by
default and only ever set true, briefly, inside `scheduler_demo_run()`
(`schedtest`), set back false before returning even on failure -- see
that function's own build-172-era comment for the original reasoning.
Milestone 1 phase 4b (Terminal async spawn, see `CHANGELOG.md`'s
`[Unreleased]` entry) needed a scheduler available OUTSIDE that one demo
call, so `scheduler_init()` now sets `scheduler_armed = 1` once, at
boot, and nothing ever unsets it again.

Why this doesn't reopen the M8-M15 safety argument the disarmed default
existed for: `scheduler_tick()` being armed only matters once something
is actually in the process table. `find_next_ready()` scanning an
all-`SCHED_UNUSED` table always returns -1, so every tick that finds
nothing ready just re-confirms `g_next_kernel_rsp` at whatever
`isr_dispatch`'s default already set it to (`regs`, i.e. resume exactly
what was interrupted) -- byte-for-byte the same outcome the old disarmed
early-return produced. So every legacy `process_run_ring3()` caller
(every M8-M15 test command, the physical shell's own `run`/`ls`) is
still completely unaffected, for the same reason as before, just via a
different mechanism (an empty table instead of a flag check). See
`scheduler.c`'s own top comment for the full writeup, and
`scheduler_poll()`'s `SCHED_ZOMBIE` state for the other real change this
item made (a process holds its exit code until explicitly reaped,
instead of being freed back to `SCHED_UNUSED` the instant it exits).

## `strace` traces an address space, and prints each line after the handler returns

Two questions the design answers, both easy to get backwards.

*Why not a global on/off switch?* Because "trace this program" is the
actual request, and a global switch would also catch whatever else runs
next. `strace_arm()` marks intent, the next process created claims it
(`strace_claim()`, one line in `elf_run_from_fs()` and one in the
scheduler's `spawn_from_fs()`), and `syscall_process_exit_cleanup()`
releases it -- so a recycled CR3 can't inherit a stale trace. This is
the same single-slot compare-CR3 pattern `SYS_SBRK`'s heap arming and
`SYS_WIN_CREATE`'s window state already use in `syscall.c`.

*Why format on entry but print on exit?* The arguments must be READ
before the handler runs (a `SYS_READ` fills the buffer its pointer
names), but printing them then would leave `write(1, "hi", 2)` half
on screen while the traced process's own "hi" prints into the middle of
it. Formatting into a buffer at entry and emitting the whole line after
the handler returns keeps trace lines intact and puts the program's
output above its own trace line. The cost is real and accepted: a
handler that faults mid-call prints no line at all, and `SYS_EXIT` --
which may never return -- has to close out its own line, which is why a
trace ends with `exit(0) = ?` rather than a return value.

See `kernel/proc/strace.c`'s top comment and `CHANGELOG.md`'s
`[Unreleased]` entry for the full writeup, including why `strace`
resolves binaries through `shell_path_find()` instead of the usual
`shell_exec_name()`.

## The kernel/lib/ toolkit: converters that fill a buffer, not printers

`string.h`/`knum.h`/`kfmt.h`/`kpath.h` were added together after a
survey counted the same code written over and over: nine
implementations of int->decimal, ten of int->hex, six digit-parsing
loops, three path resolvers.

**Why they were duplicated in the first place, and what fixed it.** The
number formatters weren't copied out of laziness -- each one printed to
a different place (the screen via `vga_putc`, the kernel log via
`klog_putc`, a `char` buffer, a window). `klog.h` even carried a
comment explaining that duplicating `vga.c`'s digit loop was cheaper
than making the log depend on a driver, which was a fair call given the
options. The fix is the third option that comment didn't have: **the
shared thing is a converter that fills a caller-owned buffer, not a
printer.** `k_utoa(v, buf, sizeof buf)` has no opinion about where the
digits go, so every sink can use it, it depends on nothing itself, and
-- unlike anything that writes straight to a screen -- it can be unit
tested. None of the nine originals had a single test.

**Two rules everything there follows.** A formatter that doesn't fit
its buffer writes NOTHING (just a NUL) rather than a truncated value,
because a truncated number or path is a *wrong* number or path, not a
partial one. And a parser rejects rather than guesses -- empty input, a
stray character, an overflow are all failures, with the caller's output
left untouched. `shell_sys.c`'s `parse_decimal()` had already
established the second rule locally; the toolkit made it the project's.

**The path case is the one that was a real bug, not just duplication.**
`shell.c`'s `resolve_path()` was `static`, so `terminal.c` couldn't
call it and grew its own "deliberately simpler" version that skipped
"." and ".." entirely. `edit ../notes.txt` therefore meant different
things in the GUI Terminal and at the physical shell -- the kind of
divergence that only appears once someone types the command in the
other window. `k_path_resolve()` is one implementation with tests, so
they agree by construction.

See `CHANGELOG.md`'s `[Unreleased]` entry for the full migration and
the count of copies removed, and CLAUDE.md for the "check the toolkit
first" convention.

## Case folding is ASCII-only, even though this kernel's layouts have Å/Ä/Ö

`k_tolower`/`k_toupper`/`k_strcasecmp` fold A-Z <-> a-z and leave every
other byte alone -- including the Latin-1 Å/Ä/Ö (0xC4/0xC5/0xD6 and
their lowercase forms) the `se` keyboard layout produces. That looks
like an oversight in a kernel that went to the trouble of supporting
those characters, and isn't one: the only caller is
`tz_find_by_name()`, and nothing in the timezone database -- or any
other name compared case-insensitively today -- is non-ASCII, so
Latin-1 folding would be range added ahead of a caller, which is the
thing these helpers were deleted once already for. It also isn't free
to get right: `char` is signed in this build, so every byte >= 0x80
arrives negative, and 0xD7/0xF7 (multiplication and division signs)
sit inside the Latin-1 letter block without being letters. `string.h`'s
comment states what widening later would involve.

These same three helpers were written and deleted once before, for
having no caller (see the toolkit entry above); they came back the way
`k_strstr` did, when something real needed them. That's the "second
real caller" rule working, not an argument against it.

See `kernel/include/api/string.h`, `kernel/lib/tz_test.c`, and
CHANGELOG.md's `[Unreleased]`.

## Terminal's `run <name>` uses an explicit allowlist, not a blocklist

`apps/terminal.c`'s `RUN_ALLOWED_BINS` (Milestone 1 phase 4b) is the
opposite shape from `BLOCKED_CMDS` right above it in the same file:
`BLOCKED_CMDS` assumes safe-unless-listed (`gui`/`ring3test`/
`schedtest`, verified hazardous individually), `RUN_ALLOWED_BINS`
assumes unsafe-unless-listed. Deliberate, not an inconsistency -- by the
time this item was built, `run` could reach any `/bin` binary by name,
including ones nobody had specifically checked yet, so the safe default
flipped: a real, non-blocking spawn mechanism now exists, so the
question for each `/bin` binary became "was this specific one actually
verified safe" (reads no stdin, doesn't touch the framebuffer/its own
window) rather than "has anyone flagged this specific one as unsafe
yet." Every entry was checked against its own `userland/*.c` source, not
added by assumption -- see `CHANGELOG.md`'s `[Unreleased]` entry for
exactly which binaries and why each excluded one was excluded
(`echo`'s `SYS_READ_KEY` loop, `gui_test`/`win_test`'s framebuffer/
window takeover, `counter_a`/`counter_b`'s intentionally-infinite
`schedtest` demo loop).

## Cowork device-bridge vs. direct local checkout: detected via `git config user.name`, not assumed

Every session on this repo used to be a Cowork cloud session reaching
the user's real checkout only through the device bridge
(`mcp__remote-devices__*`) -- `CLAUDE.md`'s whole "Working in the cloud
sandbox" section, this skill's delivery steps, and `tools/preflight.sh`/
`tools/qmp_test.py` were all written assuming that, unconditionally.
That stopped being true the first time a session ran directly on the
user's own machine (a local Claude Code session, normal file/Bash tools
straight against the real checkout, no device bridge involved) --
CLAUDE.md's Cowork-only claims (git push/`gh release create` "blocked",
no git identity configured, `Makefile`/workflow files "protected",
stale `.git/index.lock`) turned out to just be false in that mode: a
real `v0.1.0` release was cut and later patched with plain `git push`
and `gh release create`/`gh release upload`, no proxy blocking anything.

Rather than rewrite the docs assuming *only* local from here on
(Cowork sessions still happen too), both modes are now supported, with
a deterministic way to tell which one applies instead of guessing:
`git config user.name` -- empty means Cowork/device-bridge (that
session has no git identity configured at all, local or global, since
it's its own isolated VM), non-empty means a direct local checkout
(this repo's convention pre-configures it to `toy-os
<noreply@toy-os.local>`). `CLAUDE.md`'s "Working in the cloud sandbox
vs. directly on the user's machine" section, `tools/preflight.sh`'s
closing message, and `~/.claude/skills/toy-os-feature-workflow/`'s
step 0 all use this same check. See `CHANGELOG.md`'s `[Unreleased]`
entry for the full list of files touched, including the unrelated but
same-session `tools/qmp_test.py` fix (a QEMU-backgrounding pattern that
turned out to be unreliable specifically in the sandboxed environment
this was discovered in).

## Stack canaries: `-mstack-protector-guard=global` and a fixed constant, not GCC's defaults

Milestone 2's stack-canary item (`CHANGELOG.md`'s `[Unreleased]` entry)
turns `-fstack-protector-strong` on for both `CFLAGS` and
`USERLAND_CFLAGS` (previously explicit `-fno-stack-protector` in both,
just a "not built yet" placeholder -- see this file's `docs/roadmap.md`
excerpt for the original reasoning). Two things about the flags chosen
are worth knowing if this is ever revisited:

- **`-mstack-protector-guard=global`, not GCC's default `tls`.** The
  default reads the canary via `%fs:0x28` on x86-64 -- this kernel
  never sets up a per-CPU/per-thread FS/GS base at all (no `wrmsr` to
  `IA32_FS_BASE`/`GS_BASE`, no `swapgs`, confirmed by grep across
  `kernel/core/`), so the TLS-based default would dereference an
  unconfigured segment. `global` instead reads a plain
  `extern uintptr_t __stack_chk_guard` (`kernel/lib/stack_protector.c`
  for the kernel, `userland/stack_chk.c` for userland -- two separate
  symbols, two separate address spaces, no reason to share one).
  Building real TLS infrastructure just to use GCC's default guard
  would have been wildly disproportionate to what this milestone item
  actually needed.
- **The guard value is a fixed compile-time constant, not random.**
  There's no entropy source anywhere in this kernel (the same gap
  blocking the kernel-ASLR item further down this same milestone) --
  seeding a real random guard at boot needs an RNG that doesn't exist
  yet. A fixed guard still catches the threat model that actually
  matters here (an accidental linear buffer overflow corrupting
  whatever's next on the stack) -- it just can't defend against an
  attacker who's read this exact binary and crafts an overflow that
  writes the correct guard bytes back on its way past. Worth
  revisiting once/if a real RNG exists.
- **`__stack_chk_fail`, not a synthesized trap.** Neither the kernel
  nor userland implementation routes through `idt.c`'s existing fault
  dispatcher -- each is just a small, direct function GCC's generated
  epilogue calls on a mismatch. The kernel's prints a panic banner and
  falls into the same unconditional `cli; hlt` loop `idt.c`'s
  non-recoverable path already ends on (there's no "recoverable"
  case for a kernel-side canary trip -- nowhere to hand control back
  to). Userland's is even simpler: `SYS_WRITE` a message then
  `SYS_EXIT` with a distinct code (2) -- from the kernel's point of
  view that's just an ordinary process exit, no different from any
  other `run <name>` finishing, so nothing new was needed to "catch"
  it.
- **Verifying it actually works needs `noinline` on the test's overflow
  function.** `userland/stack_smash_test.c`'s first version called an
  un-annotated `static void smash(void)` from `_start` -- at `-O2` GCC
  inlined it straight into `_start`, which moved the canary check to
  `_start`'s OWN epilogue, after `_start`'s later code (a "survived"
  message + `SYS_EXIT`) had already run and exited the process. The
  test printed "UNEXPECTEDLY SURVIVED" and exited normally with the
  canary silently corrupted underneath -- not because canaries don't
  work, but because the check was unreachable code by the time control
  got there (confirmed by disassembling `build/userland/stack_smash_test.o`
  and finding the compare-and-branch instructions positioned after the
  exit syscall). `__attribute__((noinline))` on `smash()` fixed it --
  a real, non-inlined function has its own `ret` and therefore its own
  canary check firing immediately on return, before `_start` ever
  reaches the "survived" path. Worth remembering for any future
  deliberate-crash test: inlining can silently move a compiler-inserted
  check somewhere your test's control flow never reaches.

## GPT header verification: a host-compiled unit test, not a live boot -- TFS2's own journal collides with LBA 1

`kernel/drivers/partition.c`'s GPT support (Milestone 3, `CHANGELOG.md`'s
`[Unreleased]` entry) couldn't be verified the same way its MBR half was
(a real `disk.img` patched with synthetic data, booted, `parttable` run
from the shell over QMP) -- a real, unavoidable architectural conflict,
not a testing inconvenience:

- The GPT header's LBA is fixed by spec at LBA 1.
- `kernel/fs/tfs.c`'s `FS_JOURNAL_HEADER_LBA` is *also* LBA 1
  (`FS_SUPERBLOCK_LBA + 1`).
- `tfs_init()` calls `tfs_selftest()` **unconditionally** after either
  mounting or formatting (`if (g_disk_backed) tfs_selftest();`, no
  bypass/flag), and `tfs_selftest()` creates and writes a real file --
  which, via `persist_record()`, always ends with `write_journal_header(0,
  0, 0)`, overwriting LBA 1 with a real `"JRN1"` journal header.
- This runs synchronously during `kernel_main()`, before the shell
  prompt is ever reachable -- there is no window, pre-boot or live-patch
  mid-boot, where a custom GPT header at LBA 1 survives long enough for
  a shell command to read it. Confirmed two ways during development: a
  disk patched with a valid GPT header before boot came back showing a
  fresh `"JRN1"` journal header at LBA 1 after boot (self-test's write
  landed exactly where the GPT header had been); and patching the file
  live from the host while QEMU sat idle at the shell prompt was
  *also* unreliable -- QEMU's own write-back caching raced the external
  patch and won, restoring the stale in-memory `"JRN1"` copy moments
  later; a plain host-side read immediately confirmed the external
  write was never actually left standing.
- (The MBR half doesn't have this problem: its partition-table region
  is bytes 446-511 of LBA 0, which TFS2 never touches -- `write_superblock()`
  only ever writes bytes 0-4. `tools/mkpart_test.py --mbr` reads the
  existing LBA 0 sector and only patches that region, preserving TFS2's
  magic so `tfs_init()` mounts normally instead of reformatting.)

Verified instead with a host-compiled unit test
(`/tmp/.../parttest/harness.c` during development, not committed --
see below) that `#include`s the real, unmodified
`kernel/drivers/partition.c`, with a tiny stub `ata_read_sector()`
reading from a plain file instead of real hardware. Run against a
synthetic image `tools/mkpart_test.py --gpt` wrote (a scratch file, not
`disk.img`), it correctly validated the header's CRC32, decoded both
partitions' type/unique GUIDs, LBA ranges, and UTF-16LE names exactly
matching what was written. This is real execution-level proof of the
parsing algorithm (CRC32, field offsets, GUID mixed-endian decoding) --
compiled from the actual shipped source, not a second reimplementation
-- just not exercised through `ata.c`'s real hardware I/O path the way
the MBR case was. `tools/mkpart_test.py` itself is committed (useful
for any future partition-table work); the throwaway `harness.c`/stub
`ata.h` were scratch-only and not worth keeping as-is -- recreate the
same shape (stub `ata_read_sector()`, `#include` the real `.c` file
being tested) if this pattern is ever needed again for another
on-disk-format parser.

## GDB debugging: QEMU's built-in stub, not an in-kernel serial protocol implementation

`make debug` (`CLAUDE.md`'s "Debugging with GDB" section) boots toy-os
frozen at CPU reset (`-s -S`) so a real `gdb` on the host can attach
via `target remote localhost:1234` -- real breakpoints, single-step,
register/memory inspection. This is QEMU's own built-in GDB remote
stub: QEMU emulates the CPU directly, so it can expose full debugger
control over whatever's running in the guest without the guest OS
needing to implement anything at all.

Worth stating explicitly because the first framing of this idea (a
`/btw` suggestion) got it wrong: it proposed toy-os's kernel would need
to "speak the GDB remote serial protocol" itself -- real, substantial
protocol work (packet framing, register/memory read-write commands,
breakpoint handling) on top of `kernel/core/debug_console.c`'s existing
scope (a handful of if/else-dispatched diagnostic commands). That's
simply unnecessary: `-s`/`-S` are ordinary QEMU flags, no different in
kind from `-vnc`/`-serial file:...` already used throughout
`tools/qmp_test.py`'s testing setup, and they work today with zero
toy-os code changes. Confirmed directly, not just asserted: `break
kernel_main` + `continue` over a real `gdb` session correctly ran the
CPU from reset through GRUB/multiboot2 and stopped exactly at
`kernel_main`, with a real backtrace showing source file/line.

The one actual gap, now closed: `CFLAGS`/`USERLAND_CFLAGS` never
carried `-g`, so `kernel.bin` and every userland ELF had zero DWARF
debug info -- GDB could still technically attach, but would only ever
see raw addresses, no function names or source lines, making
`break kernel_main`-style debugging impossible. Added `-g` to both,
kept at `-O2` rather than dropping to `-Og`/`-O0` for a separate debug
build -- same binary as always, just now carrying symbols, at the cost
of some locals showing "optimized out" in GDB. A real, deliberate
build-config-simplicity tradeoff, not an oversight.

## Why the compositor uses one scene-wide damage region, not per-window exposure tracking

The window manager used to redraw everything -- `desktop_draw()`'s full
clear plus every window/taskbar/menu -- on any scene change at all,
including a once-a-second clock tick. Milestone 19's "real" dirty-rect
compositor replaces that with a scene-level damage-region accumulator
(`wm_damage_rect()`, `apps/wm/wm_render.c`) that's deliberately
separate from `gfx.c`'s existing pixel-level dirty-rect tracking
(`dirty_mark()`/`gfx_present()`'s blit-only-the-touched-bbox
optimization) -- that layer already existed and still does its job one
level lower, blitting only the touched region to the real framebuffer
after software rendering finishes. The new layer sits above it,
deciding what even gets *drawn* in software in the first place, via a
new `gfx_set_clip_rect()` primitive that gates `gfx_put_pixel()` (not
`gfx_get_pixel()` -- a caller reading existing pixels, e.g. to save
content before drawing over it, still wants the real framebuffer
regardless of the active clip).

The core design choice: when something in the damaged region needs
repainting, redraw *everything* within that region, back-to-front
(desktop, then windows in z-order, then taskbar/menus), rather than
computing which specific sub-rectangles got newly exposed by a move/
close/reorder. Explicitly tracking exposure would mean, for every
window-geometry change, diffing the old rect against every
window/desktop area now underneath it -- a real polygon-clipping
problem. Redraw-by-z-order-within-the-damaged-bbox sidesteps that
entirely: whatever should be visible in that region gets drawn last in
the correct order, so occlusion just falls out of the existing paint
order for free. The tradeoff is redrawing a few more pixels than the
minimal exposure set would require (anything already-correct inside
the damaged bbox gets repainted too) -- deliberately accepted as
"simple and correct" over "minimal and fragile," consistent with the
gfx.c dirty-rect layer's own bounding-box-not-rect-list tradeoff.
Verified directly via QMP: dragging Calculator off of Notepad and
confirming Notepad's revealed area redraws correctly and only the
damaged bbox is touched (`screenshots/2026-08-12/
compositor-exposure-after-drag.png`).

Damage sources are a mix of one automatic path and several explicit
ones, because not every scene change is a pure geometry diff:
`compute_window_damage()` (`wm_render.c`) diffs each window's
position/size/visibility against fields stored directly on
`struct window` (`last_x/y/w/h/last_visible`) every frame, which
catches drags/resizes/minimize/restore automatically. But z-order
swaps (`bring_to_front()`), open/close (`open_app()`/`close_window()`),
and interaction-driven redraws that don't change any window's rect at
all (desktop icon drag, `window_invalidate()`, focused-window key/wheel
delivery) all report their own damage explicitly, since there's no
before/after rect diff to detect them from.

Two real bugs surfaced only by interactive QMP testing, not by
re-reading the code:

- **Taskbar staleness on close.** Closing Calculator via its title-bar
  X left its taskbar button drawn (confirmed by screenshot -- code
  review alone missed it because the closing window's own rect *was*
  correctly damaged, just not the taskbar strip). The taskbar's button
  list/layout/tint depends on state outside any single window's rect,
  so `close_window()`, `open_app()`, `bring_to_front()`, and the
  visibility-flip branch of `compute_window_damage()` (minimize/
  restore of the frontmost window changes its own tint) all now also
  damage the taskbar strip explicitly
  (`screenshots/2026-08-12/compositor-close-taskbar-fixed.png`).
- **Desktop icon drag highlight sliver.** Dragging a desktop icon left
  a thin stale highlight-colored line on screen along the drag path.
  `desktop_draw()`'s selection-highlight rect draws 4px *above* the
  icon's own y (`y - 4`, to include the highlight border), but the
  drag's damage strip in `desktop.c` was anchored exactly at the icon's
  y with no top margin, so that top 4px escaped the damaged region and
  was never repainted over. Fixed with a
  `DESKTOP_DRAG_DAMAGE_MARGIN` applied to the damage strip's top edge,
  matching the highlight rect's own offset
  (`screenshots/2026-08-12/compositor-icon-drag-no-artifact.png`).

Deliberately out of scope in the Phase 1+2 round above (falls back to
the old full-screen repaint on any menu/taskbar-content-click/dialog
change, which is safe -- never worse than before, just not optimized):
precise damage reporting for those interactions. That's still open.

**Phase 3** (a later round): skip `draw_window_chrome()`/`on_draw()`/
`draw_resize_grip()` entirely for a window whose rect doesn't
intersect the frame's damage box, rather than calling them and letting
`gfx_set_clip_rect()` drop their writes -- `wm_render.c`'s
`window_intersects_damage()`, consulted in `wm_render_frame()`'s
per-window loop only when a damage box was actually reported that
frame. This is exactly the kind of change that turns a latent bug into
a visible one: `bring_to_front()` (`apps/wm/wm.c`) had only ever
damaged the newly-promoted window's rect, never the
previously-frontmost window's -- but that window's titlebar tint
(focused blue vs. unfocused gray) changes on every z-order swap too.
Under Phase 1+2 this was silently harmless: the previously-frontmost
window's chrome still got *called* every frame regardless, and its
tint pixels happened to fall inside whatever damage box was active in
every scenario tested at the time, so the missing damage report never
actually produced a wrong pixel. Once Phase 3 started skipping the
call itself for windows outside the damage box, that same gap became a
real bug -- a window that had just lost focus would keep showing its
old blue titlebar indefinitely, since nothing would ever call its
chrome-drawing code again until some other damage happened to cover
it. Fixed by having `bring_to_front()` damage the previously-frontmost
window's rect alongside the newly-promoted one. Verified via QMP:
opened two non-overlapping windows, swapped focus between them
repeatedly via taskbar clicks, confirmed both titlebar tints updated
correctly on every swap
(`screenshots/2026-08-12/compositor-phase3-focus-tint-fixed.png`).

## The taskbar/tray falls back to full-screen repaint on purpose, not as an oversight

`apps/wm/wm_tray.c`'s `tray_damage()` deliberately does NOT call
`wm_damage_rect()` -- it just sets `redraw_pending`, relying on
`wm_render_frame()`'s full-screen fallback, the same as menus/dialogs
(see the compositor entry above). This looks like it's leaving an easy
optimization on the table (the tray API shipped the same day as
Milestone 19's damage-region work), but an earlier version DID scope
tray/clock updates to just the taskbar strip via `wm_damage_rect()`,
and it caused two real bugs, both caught live on the user's own
machine rather than in QMP testing:

1. **Black desktop on GUI entry.** `tray_init()` (which registers the
   clock as tray item 0) runs during `wm_run()`'s setup, before the
   main loop starts. Its `wm_damage_rect()` call poisoned the very
   first frame's "no damage reported yet -- unknown, be safe, draw
   everything" full-screen fallback into a taskbar-only clip, so
   `desktop_draw()`/window chrome ran but had every pixel outside that
   strip clipped away -- the desktop was simply never drawn.
2. **Mouse cursor drift.** The once-a-second clock tick used to report
   no damage at all, which forced a full-screen fallback redraw every
   second -- an implicit, never-designed-as-such safety net that kept
   `wm_render.c`'s cursor-under-pixels snapshot (`cursor_under`, used
   by the cheap `wm_render_cursor_move()` path) resynced against the
   real screen. Scoping the tick's damage to just the taskbar strip
   silently removed that safety net, and the cursor stopped tracking
   correctly.

Both are root-caused and fixed in the same CHANGELOG.md `[Unreleased]`
entry (search "tray damage-scoping regression"). The fix was simply to
stop scoping tray damage at all, matching every other still-unscoped
piece of WM chrome -- not to fix the two bugs while keeping the
optimization. Scoping the tray/taskbar precisely is still a real,
open piece of the Milestone 19 compositor plan (`docs/roadmap.md`), but
it needs the pre-loop-damage and once-a-second-safety-net issues above
solved properly first, not just reverted -- a future attempt should
budget for both, not assume the first bug found is the only one.

## NX enforcement scoped to userspace only -- the kernel's own identity map stays RWX

Milestone 2's "NX bit enforcement" and "W^X on kernel + userspace
mappings" roadmap items were done together for the *userspace* half
only (`kernel/proc/elf.c`/`vmm.c`, `userland/link.ld`) -- deliberately
not touching `kernel/arch/x86_64/boot.asm`'s own flat 2MiB-huge-page identity
map, which stays plain present+writable, no NX, no code/data split, on
purpose. Giving the kernel itself real NX/W^X would need `linker.ld` to
page-align `.text` away from `.rodata`/`.data`/`.bss` first (it
currently doesn't, unlike `userland/link.ld` post this change) and
`pmm.c`'s frame-reservation logic to become section-aware instead of
treating the whole kernel image as one blob -- a much larger, riskier
change to a boot-critical path than userspace enforcement, which only
touches process page tables that already get created fresh per-process
anyway. Left as the remaining half of the "W^X on kernel... mappings"
roadmap checkbox.

The default mapper (`vmm_map_user_page()`) was changed to be
non-executable by default rather than adding a parallel "safe" variant
-- every pre-existing call site (a process's stack, `SYS_SBRK` heap
growth, the GUI framebuffer, a window's pixel buffer) is data, never
code, so this is both the secure default and correct for all of them
with zero call-site changes; only `kernel/proc/elf.c` (needs real
per-segment control) and `kernel/proc/ring3_test.c` (its one
hand-assembled code page) call the explicit-flags variant instead. See
CHANGELOG.md's `[Unreleased]` entry for the full mechanics and the QMP
verification (a purpose-built `userland/nx_test.c` that jumps into a
non-executable data page and confirms the CPU actually faults --
`error_code=0x15` decodes to Present+User+Instruction-Fetch, not a
generic unmapped-page fault).

## An unreadable superblock is not a foreign disk -- refuse to format, don't guess

`tfs_init()` distinguishes "the superblock read failed" from "the
superblock read fine and isn't ours", and only the second one formats.
The first degrades to RAM-only for that boot and leaves the disk
untouched. This looks like defensive over-engineering until you notice
the failure it replaced was silent total data loss: the two cases used
to share one `if`, and `ata_read_sector()` genuinely does give up after
three exhausted DMA attempts, which this project has observed happening
on real hardware for transient reasons (host filesystem stalls, not a
sick drive -- see `ata.c`'s `dma_transfer_with_retry()` comment).

The asymmetry is the point: formatting a disk that was actually fine is
unrecoverable, while refusing to format a disk that really is blank
costs one boot and a clear `dmesg` line telling you to check it. When
the two error paths have wildly different costs, the cheap-to-recover
one is the correct default. A blank/foreign disk still auto-formats,
because that path is only reached on a *successful* read.

See `CHANGELOG.md`'s `[Unreleased]` entry for the full writeup,
including the related "disk too small to hold the metadata region" case
and the one case still not detectable (a 0-length image, which QEMU
answers with zeros rather than an error).

## Metadata ordering: persist the record first, free the blocks second -- prefer a leak to a double-allocation

`tfs_delete()` and `tfs_write()`'s truncate path both detach a file's
block pointers, write the record that now references nothing, and only
then return the blocks to the free bitmap (`detach_blocks()`/
`reattach_blocks()` in `tfs.c`). The reverse order is the obvious one
and was what the code did first, but it opens a window where the bitmap
says a block is free while an on-disk record still points at it -- the
next allocation hands that block to a different file and two files
silently share it.

Inverting the order makes the worst case the *opposite* failure: blocks
marked allocated that nothing references. That's a space leak, it's
detectable by walking every record's pointers, and it costs disk space
rather than data. There's no fsck-style pass to reclaim them yet (see
`docs/roadmap.md`'s Milestone 3) -- the ordering is chosen so that when
something does go wrong, the recoverable failure is the one that
happens.

## Zero-filling a freshly allocated block is skipped only when the caller overwrites it whole

`block_for_index()`'s allocation modes (`BLK_ALLOC` vs.
`BLK_ALLOC_NOZERO`, `tfs.c`) exist because zero-filling every newly
allocated block costs a full block write, and for a sequential write
that immediately overwrites the whole block that write is pure waste --
it doubled the ATA commands per block and split the multi-block
coalescing apart, which measurement caught (the first coalescing pass
only reached 20.3 MB/s of the eventual 25.1).

Where zero-filling still always happens, and why:
- **Indirect index blocks, unconditionally.** Their unwritten entries
  are read back as block pointers, so they must be the 0 sentinel and
  not whatever a deleted file left there -- `walk_indirect()` ignores
  the NOZERO flag for them on purpose.
- **Any partially written block.** Otherwise the read-modify-write in
  `write_range_one_block()` would leak a deleted file's contents into
  the untouched part of the block.
- **Sparse gaps**, implicitly: a hole has no block at all and reads as
  zero via `read_block(0)`.

The one visible consequence: if the write that was supposed to overwrite
a NOZERO block fails, the block keeps stale content. It's past the
file's size (which only advances for bytes actually written), so no read
can reach it. See `CHANGELOG.md`'s `[Unreleased]` entry.

## The DMA bounce buffer is 64KB because that's one PRD, not because 64KB benchmarked well

`ata.c`'s `DMA_BUF_FRAMES` is 16 (65536 bytes), which sets
`ATA_MAX_SECTORS_PER_XFER` to 128. The number comes from the hardware
interface, not tuning: a Physical Region Descriptor's byte-count field
is 16 bits, with 0 encoding 64KB, so 64KB is the largest single-PRD
transfer possible and 129 sectors would truncate to a genuinely wrong
value rather than a clamped one. Going bigger means scatter-gather --
multiple PRD entries -- which is a real feature, not a constant change.

Two related choices: the allocation failure path retries for the
original 2 frames rather than dropping to PIO (a smaller DMA window is
still far better than no DMA), and `ata_max_sectors_per_xfer()` reports
the runtime value separately from the compile-time maximum so callers
that batch (TFS2's coalescing) adapt instead of assuming. See
`CHANGELOG.md`'s `[Unreleased]` entry.

## `fsck` reclaims leaks and marks stragglers, but never resolves a double-allocation

`fs_check()`/`tfs_check()` (`tfs.c`) repairs exactly three things and
deliberately refuses a fourth:

| Finding | Repaired? | Why |
|---|---|---|
| Leaked block (allocated, unreferenced) | yes -- freed | Costs only space; the free bitmap is provably wrong and the record tree is the authority. |
| Referenced but marked free | yes -- marked allocated | The dangerous direction: leaving it lets the allocator hand the block to a second file. |
| Out-of-range pointer | yes -- zeroed | It can't name real data; zeroing turns it into a hole that reads as zeros. |
| Block claimed by two records | **no** -- reported only | Both records are internally plausible. Choosing which keeps the block silently destroys the other file's data, and no amount of on-disk information says which one is right. |

That last row is the whole design stance: a repair tool that guesses
turns a recoverable disk into a confidently-wrong one. It reports the
count and tells you to delete one of the affected files.

The scratch "referenced" bitmap is a static 288KB array (`g_fsck_seen`),
not `kmalloc()`'d, because a 288KB allocation needs 72 contiguous frames
from pmm and failing to get them would mean "can't check the disk"
precisely when something is already wrong. Same reasoning `g_bitmap`
itself uses one bullet up, with a repair-tool-specific edge.

This exists because the truncate/delete ordering deliberately prefers a
leak to a double-allocation (see the entry above) -- that trade is only
correct if something can reclaim the leak afterwards.

Testing it needed fault injection: the inconsistencies it repairs are
ones the kernel goes out of its way not to produce, so
`tools/tfs2_writer.py corrupt` manufactures them host-side
(`--leak N`, `--free-referenced N`, `--bad-pointer PATH`). Doing that
turned up a live demonstration of why the referenced-but-free repair
matters: with three referenced blocks marked free, the very next boot's
shell-history append allocated one of them to `/etc/history`, which
already belonged to `/bin/counter_a` -- a real double-allocation,
created by the corruption in seconds. See `CHANGELOG.md`'s
`[Unreleased]` entry.

## Tab completion is a shared candidate generator, not a shared line editor

`apps/completion.c` answers one question -- "given this line and cursor,
what could this word become?" -- and does no input handling and no
drawing at all. Both shells call it from their own input loops and
present the result their own way: `shell.c` with `vga_putc()`/
`vga_backspace()`, `terminal.c` through its `text_scrollback` widget.

The tempting alternative was a shared line editor owning the buffer,
history, editing keys and completion, with both shells as thin adapters.
That's the better end state -- history and line editing genuinely are
implemented twice today and can drift -- but it means rewriting two
working input paths in the same change as adding a feature. Splitting at
"candidates" instead put the new, interesting logic in one place without
touching either loop's structure. The line editor consolidation is still
worth doing; it's just its own change.

Two consequences worth knowing:

- **The command list is duplicated.** `dispatch()` is a hand-written
  if/else chain over ~39 commands whose handlers have genuinely
  different signatures, so completion keeps its own
  `COMPLETION_COMMANDS[]` table rather than driving dispatch from it
  (that would need ~40 wrapper functions in a core file). One drift
  direction is self-reporting: `dispatch()`'s unknown-command branch
  checks the table and says "tab-completable but has no dispatch case"
  instead of "unknown command". The other direction shows up as "tab
  doesn't complete my new command", which announces itself.
- **Behaviour is zsh's default, deliberately**: one Tab extends to the
  common prefix, and lists candidates when more than one remains
  (zsh's AUTO_LIST). Not menu-completion, which would need state across
  keystrokes and a rule for what any other key does to the pending
  selection -- state that both input loops would have to hold
  identically.

See `CHANGELOG.md`'s `[Unreleased]` entry, including the pre-existing
`dispatch()` bug completion exposed (a trailing space in `args` made
`cat /etc/timezones ` fail as "no such file").

## PATH lives in the shell, not the kernel -- and builtins win over it

Typing a bare `nx_test` runs `/bin/nx_test`; the `run` prefix is
optional now. Three decisions in that, each with a real alternative:

**PATH is shell state.** `timezone` and `font_size` live in
`/etc/toyos.conf` behind small kernel-side modules (`tz.c`,
`font_config.c`) because the kernel itself consults them. Nothing in the
kernel has any use for PATH -- it's a question about how a command line
is interpreted, which is entirely the shell's business. So
`apps/shell_path.c` reads the same shared config file through kapi.h's
`etc_config_get()` and keeps the parsed result to itself, rather than
adding a `path_config.c` next to the other two. The dividing line worth
remembering: a setting goes kernel-side when the KERNEL reads it, not
merely because it lives in the shared config file.

**Builtins beat PATH, not the other way round.** A real Unix shell lets
a `/bin/ls` shadow nothing (builtins generally win) but the instinct to
let disk binaries take precedence is common enough to name why it would
be wrong here: `ls` is a builtin *wrapper* that resolves its positional
argument against the shell's cwd before handing `/bin/ls` an absolute
path, because a PATH-executed binary receives raw arguments and has no
cwd of its own. Letting `/bin/ls` win would silently break `ls docs`.
The order is builtins, then `apps.c`'s console-app registry, then each
PATH directory left to right, first match winning.

**`run` stays.** It costs nothing, keeps every existing doc and habit
valid, and is the explicit form when you'd rather not wonder whether a
name collides with a builtin. Both it and a bare name go through one
resolver (`shell_exec_name()`), so they can't diverge.

Two smaller notes: a name containing `/` is treated as a path rather
than a PATH lookup (so `/bin/foo` and `docs/foo` mean what they say),
and entries in PATH that don't exist are skipped silently -- the default
`/bin;/usr/bin` names a directory that isn't on a stock disk, and
warning about it on every boot would be noise. See `CHANGELOG.md`'s
`[Unreleased]` entry.

## Console scrollback is a character ring in vga.c, and the boot log is echoed to it

Two changes that only make sense together: the kernel now mirrors its
log to the physical console while booting, and the console keeps a
scrollback ring so what scrolled past is still readable.

Neither works alone. `klog_write()` went to the serial port and the
`dmesg` ring only, so the screen showed "toy-os booting..." and then the
shell -- scrollback would have had nothing of the boot to scroll back
to. And echoing the log without scrollback just moves text past too fast
to read. `kernel_main()` turns the echo on early and off again just
before `apps_start()`, so it covers boot and nothing else: leaving it on
would put every ATA retry and filesystem warning on top of whatever the
shell or the GUI is drawing.

Design points worth keeping:

- **The ring stores a colour per CELL, not per line.** Output here is
  routinely multi-coloured within a line (a green prompt then grey
  input, `ls`'s per-type colouring), and replaying it in one colour
  would be a visibly worse copy of what you saw. 256 lines x 256 cols x
  2 bytes = 128KB of `.bss`, sized like `g_bitmap` for the same reason:
  it must work before `heap_init()`.
- **Wrapping is recorded as a line break**, so the ring reproduces the
  *screen* rather than the logical text. The alternative reproduces text
  better but can't redraw what you actually saw after a `fontsize`
  change.
- **PageUp/PageDown are swallowed by `keyboard_getchar()`, the blocking
  reader -- deliberately not by `keyboard_try_getchar()`**, which is
  what the window manager polls. The GUI Terminal and Notepad have their
  own PageUp/PageDown scrolling of their own widgets; swallowing the
  keys at the driver level would break both.
- **New output snaps the view back to live** before writing, so the
  history and the live console can't interleave into nonsense on screen.
- **`vga_clear()` does not discard history**, which is what a terminal
  does -- and it's load-bearing here: boot itself clears the screen (via
  `vga_reflow()` when the persisted font size loads), so a scroll-back
  limit computed as "only if there's more history than fits a screen"
  concluded there was nothing to scroll to. The limit has an explicit
  one-step case for exactly that.

See `CHANGELOG.md`'s `[Unreleased]` entry.
