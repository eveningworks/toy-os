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
- [The kernel context is a rotation participant, not a kernel thread](#the-kernel-context-is-a-rotation-participant-not-a-kernel-thread)
- [Blocking syscalls deschedule; they never wait in place](#blocking-syscalls-deschedule-they-never-wait-in-place)
- [`SYS_WAIT_EVENT` makes clients loop instead of restarting the syscall](#sys_wait_event-makes-clients-loop-instead-of-restarting-the-syscall)
- [The windowing protocol is one syscall carrying typed messages, not a syscall per operation](#the-windowing-protocol-is-one-syscall-carrying-typed-messages-not-a-syscall-per-operation)
- [The TWP transport seam has exactly one implementation, so it is UNVALIDATED](#the-twp-transport-seam-has-exactly-one-implementation-so-it-is-unvalidated)
- [`gui` output goes to a caller-supplied sink, not through a klog redirect](#gui-output-goes-to-a-caller-supplied-sink-not-through-a-klog-redirect)
- [The diagnostic channel carries its own payload struct, so presents stay cheap](#the-diagnostic-channel-carries-its-own-payload-struct-so-presents-stay-cheap)
- [A client window's close button is a handshake, not a seizure](#a-client-windows-close-button-is-a-handshake-not-a-seizure)
- [Single-instance is the app's decision, and the launcher always launches](#single-instance-is-the-apps-decision-and-the-launcher-always-launches)
- [A yield is not a tick: SYS_YIELD reschedules without billing](#a-yield-is-not-a-tick-sys_yield-reschedules-without-billing)
- [A client blocks between frames -- WIN_EV_TIMER, not a polling loop](#a-client-blocks-between-frames----win_ev_timer-not-a-polling-loop)
- [A hover test parks the REAL cursor, and must un-park it afterwards](#a-hover-test-parks-the-real-cursor-and-must-un-park-it-afterwards)
- [Rubber-band selection is shared source compiled twice, and it owns the behaviour](#rubber-band-selection-is-shared-source-compiled-twice-and-it-owns-the-behaviour)
- [One desktop-entry directory with a `ShowIn` key, not a second directory per surface](#one-desktop-entry-directory-with-a-showin-key-not-a-second-directory-per-surface)
- [Desktop entries reload live off a filesystem generation counter, not a directory poll](#desktop-entries-reload-live-off-a-filesystem-generation-counter-not-a-directory-poll)
- [`append` zeroed the block it appended into, and `write`/`append` now write LINES](#append-zeroed-the-block-it-appended-into-and-writeappend-now-write-lines)
- [Claiming the compositor role is a message, and it is the one request that works with no window server](#claiming-the-compositor-role-is-a-message-and-it-is-the-one-request-that-works-with-no-window-server)
- [Raw input to the compositor is level state, tapped inside the WM loop rather than at the driver](#raw-input-to-the-compositor-is-level-state-tapped-inside-the-wm-loop-rather-than-at-the-driver)
- [Ring-3 clients draw for themselves, and the font is shared read-only](#ring-3-clients-draw-for-themselves-and-the-font-is-shared-read-only)
- [Calculator's engine is shared source compiled twice, not copied](#calculators-engine-is-shared-source-compiled-twice-not-copied)
- [Ring 3 gets the C names; the kernel keeps `k_`](#ring-3-gets-the-c-names-the-kernel-keeps-k_)
- [The shell is called `tosh`, and the name covers the language, not a binary](#the-shell-is-called-tosh-and-the-name-covers-the-language-not-a-binary)
- [The process entry ABI is SysV, and crt0 owns the stack alignment](#the-process-entry-abi-is-sysv-and-crt0-owns-the-stack-alignment)
- [A legacy ring-3 process needs its own RSP0 and must not be descheduled](#a-legacy-ring-3-process-needs-its-own-rsp0-and-must-not-be-descheduled)
- [The retry sentinel is -2 because 0 is a real answer](#the-retry-sentinel-is--2-because-0-is-a-real-answer)
- [The ring-3 terminal runs its own shell, not the kernel's](#the-ring-3-terminal-runs-its-own-shell-not-the-kernels)
- [The geometry module is shared source compiled twice, like the calculator engine](#the-geometry-module-is-shared-source-compiled-twice-like-the-calculator-engine)
- [Ring-3 GUI apps live in /bin, not /tests](#ring-3-gui-apps-live-in-bin-not-tests)
- [stderr goes to the kernel log, and is never redirected into a pipe](#stderr-goes-to-the-kernel-log-and-is-never-redirected-into-a-pipe)
- [Stack canaries: `-mstack-protector-guard=global` and a fixed constant, not GCC's defaults](#stack-canaries--mstack-protector-guardglobal-and-a-fixed-constant-not-gccs-defaults)
- [NX landed in userspace first, and the default mapper is the non-executable one](#nx-landed-in-userspace-first-and-the-default-mapper-is-the-non-executable-one)
- [Kernel W^X: NX on every huge PDE, one 4KiB split for `.text`, and CR0.WP](#kernel-wx-nx-on-every-huge-pde-one-4kib-split-for-text-and-cr0wp)
- [The framebuffer is write-combined via PAT, and `nopat` exists to make the MTRR fallback reachable](#the-framebuffer-is-write-combined-via-pat-and-nopat-exists-to-make-the-mtrr-fallback-reachable)
- [The RAM meter is an uncomposited overlay, which is why it is debug-only](#the-ram-meter-is-an-uncomposited-overlay-which-is-why-it-is-debug-only)
- [The console is double-buffered because write-combining made its scroll 357x slower](#the-console-is-double-buffered-because-write-combining-made-its-scroll-357x-slower)
- [The legacy text console cannot be selected from GRUB, and `gfxpayload=text` does not do it](#the-legacy-text-console-cannot-be-selected-from-grub-and-gfxpayloadtext-does-not-do-it)
- [The user stack's guard is an unmapped hole plus two rules, and the sbrk rule is the one that mattered](#the-user-stacks-guard-is-an-unmapped-hole-plus-two-rules-and-the-sbrk-rule-is-the-one-that-mattered)
- [SMAP is absolute here because the kernel copies through its own identity map, not with STAC/CLAC](#smap-is-absolute-here-because-the-kernel-copies-through-its-own-identity-map-not-with-stacclac)
- [Heap debug mode is a runtime toggle, and `kfree()` tells the two block shapes apart by a magic that cannot be a pointer](#heap-debug-mode-is-a-runtime-toggle-and-kfree-tells-the-two-block-shapes-apart-by-a-magic-that-cannot-be-a-pointer)
- [The kernel's relocation table is placed after `.data`, and the image that is VERIFIED is not the image that ships](#the-kernels-relocation-table-is-placed-after-data-and-the-image-that-is-verified-is-not-the-image-that-ships)
- [A blank console cell gets the console's colour, and a coloured line pads to its own edge](#a-blank-console-cell-gets-the-consoles-colour-and-a-coloured-line-pads-to-its-own-edge)
- [The serial debug console is poll-based from existing idle loops, not a new kernel thread](#the-serial-debug-console-is-poll-based-from-existing-idle-loops-not-a-new-kernel-thread)

**Filesystem & storage**

- [A filesystem talks to a BLOCK DEVICE, and persistence is the device's answer](#a-filesystem-talks-to-a-block-device-and-persistence-is-the-devices-answer)
- [TFS3's last block group may be partial, like ext2/3/4's](#tfs3s-last-block-group-may-be-partial-like-ext234s)
- [Filesystem is one active backend, not mount points](#filesystem-is-one-active-backend-not-mount-points)
- [`/etc` and `/tmp` are created by the MOUNT, not by `kernel_main()`](#etc-and-tmp-are-created-by-the-mount-not-by-kernel_main)
- [A setting reports whether it PERSISTED, separately from whether it applied](#a-setting-reports-whether-it-persisted-separately-from-whether-it-applied)
- [No recursive delete](#no-recursive-delete)
- [Persistent filesystem is write-through with a single-slot journal](#persistent-filesystem-is-write-through-with-a-single-slot-journal)
- [File timestamps: broken-down local time on disk (TFS2), epoch seconds at the API since M15](#file-timestamps-broken-down-local-time-on-disk-tfs2-epoch-seconds-at-the-api-since-m15)
- [TFS2 v2's block pointers go direct + single + double + triple indirect, not just direct + single](#tfs2-v2s-block-pointers-go-direct--single--double--triple-indirect-not-just-direct--single)
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
- [Thin provisioning: the image is sparse at birth, and TRIM is what keeps it that way](#thin-provisioning-the-image-is-sparse-at-birth-and-trim-is-what-keeps-it-that-way)
- [TFS2 stays in the kernel as a second filesystem -- the VFS probes by superblock magic](#tfs2-stays-in-the-kernel-as-a-second-filesystem----the-vfs-probes-by-superblock-magic)
- [TFS3's journal covers dirent + inode blocks; bitmaps stay leak-safe write-through](#tfs3s-journal-covers-dirent--inode-blocks-bitmaps-stay-leak-safe-write-through)
- [TFS3 v2 grew the journal by moving the layout, not by making it a log like ext4's](#tfs3-v2-grew-the-journal-by-moving-the-layout-not-by-making-it-a-log-like-ext4s)
- [`fs_rename()` refuses an existing destination -- there is no atomic replace](#fs_rename-refuses-an-existing-destination----there-is-no-atomic-replace)
- [Truncation is two phases with a commit between them, and keeps the boundary tables in memory](#truncation-is-two-phases-with-a-commit-between-them-and-keeps-the-boundary-tables-in-memory)
- [ATA DATA SET MANAGEMENT must be issued over DMA, not PIO](#ata-data-set-management-must-be-issued-over-dma-not-pio)
- [Zero-filling a freshly allocated block is skipped only when the caller overwrites it whole](#zero-filling-a-freshly-allocated-block-is-skipped-only-when-the-caller-overwrites-it-whole)

**Drivers & hardware**

- [DMA needs PCI Bus Master Enable, not just a programmed descriptor](#dma-needs-pci-bus-master-enable-not-just-a-programmed-descriptor)
- [The DMA bounce buffer is 64KB because that's one PRD, not because 64KB benchmarked well](#the-dma-bounce-buffer-is-64kb-because-thats-one-prd-not-because-64kb-benchmarked-well)
- [PCI enumeration is a brute-force flat scan, not bridge-aware recursion](#pci-enumeration-is-a-brute-force-flat-scan-not-bridge-aware-recursion)
- [Nordic keyboard/character support: Latin-1, not UTF-8; 3 remapped keys, not a full layout](#nordic-keyboardcharacter-support-latin-1-not-utf-8-3-remapped-keys-not-a-full-layout)
- [Keyboard layouts are data files (`/etc/kbs/<name>`) generated from Linux's own XKB data, not a compiled-in enum](#keyboard-layouts-are-data-files-etckbsname-generated-from-linuxs-own-xkb-data-not-a-compiled-in-enum)
- [GDB debugging: QEMU's built-in stub, not an in-kernel serial protocol implementation](#gdb-debugging-qemus-built-in-stub-not-an-in-kernel-serial-protocol-implementation)
- [ATA's waits are bounded by wall-clock in one context and a spin count in the other](#atas-waits-are-bounded-by-wall-clock-in-one-context-and-a-spin-count-in-the-other)
- [The PIO fallback is reachable on purpose (`ata nodma`), because unreachable fallback code is a guess](#the-pio-fallback-is-reachable-on-purpose-ata-nodma-because-unreachable-fallback-code-is-a-guess)

**GUI: window manager, compositor & widgets**

- [The desktop's app list is a directory of files, not a table in the kernel](#the-desktops-app-list-is-a-directory-of-files-not-a-table-in-the-kernel)
- [Widgets are added once a second real caller needs them -- except the checkbox](#widgets-are-added-once-a-second-real-caller-needs-them----except-the-checkbox)
- [A table PULLS its rows, and stores none of them](#a-table-pulls-its-rows-and-stores-none-of-them)
- [cpu_ticks is a total, not a percentage](#cpu_ticks-is-a-total-not-a-percentage)
- [SYS_KILL is unprivileged, and killing the WM is the point](#sys_kill-is-unprivileged-and-killing-the-wm-is-the-point)
- [A lone button routes its own clicks; the group is for grids](#a-lone-button-routes-its-own-clicks-the-group-is-for-grids)
- [A natural size must not depend on where the widget currently sits](#a-natural-size-must-not-depend-on-where-the-widget-currently-sits)
- [A widget's ops->hit is a boolean, and a row index is not one](#a-widgets-ops-hit-is-a-boolean-and-a-row-index-is-not-one)
- [The ring-3 UI Demo selects on contact where the kernel one committed on release](#the-ring-3-ui-demo-selects-on-contact-where-the-kernel-one-committed-on-release)
- [A compositor's view of a window is at a DERIVED address, and revocation is the feature](#a-compositors-view-of-a-window-is-at-a-derived-address-and-revocation-is-the-feature)
- [The third inert scrollbar: drawing one and handling it are separate jobs](#the-third-inert-scrollbar-drawing-one-and-handling-it-are-separate-jobs)
- [The toolkit routes pointer input; an app configures and is told what changed](#the-toolkit-routes-pointer-input-an-app-configures-and-is-told-what-changed)
- [What editing text MEANS lives in one place, and the storage does not](#what-editing-text-means-lives-in-one-place-and-the-storage-does-not)
- [A GUI test spawns its client directly, instead of typing at a Terminal](#a-gui-test-spawns-its-client-directly-instead-of-typing-at-a-terminal)
- [A client's menus are clamped to its own window, and that is one rectangle away from not being](#a-clients-menus-are-clamped-to-its-own-window-and-that-is-one-rectangle-away-from-not-being)
- [A menu bar opens on press, which is the one place the commit-on-release rule bends](#a-menu-bar-opens-on-press-which-is-the-one-place-the-commit-on-release-rule-bends)
- [Esc doesn't close a window; Alt+F4 does, and it is a WM shortcut rather than an app key](#esc-doesnt-close-a-window-altf4-does-and-it-is-a-wm-shortcut-rather-than-an-app-key)
- [Not-responding is a PING, not a close timeout -- because "refused" and "wedged" look identical to a timer](#not-responding-is-a-ping-not-a-close-timeout----because-refused-and-wedged-look-identical-to-a-timer)
- [Angles are measured in turns, not radians](#angles-are-measured-in-turns-not-radians)
- [The geometry rasteriser draws through a callback, not into a framebuffer](#the-geometry-rasteriser-draws-through-a-callback-not-into-a-framebuffer)
- [The canvas widget clips in the plot callback, not by trimming geometry](#the-canvas-widget-clips-in-the-plot-callback-not-by-trimming-geometry)
- [A Start-menu entry can be a launcher for a ring-3 program](#a-start-menu-entry-can-be-a-launcher-for-a-ring-3-program)
- [The default font size is a one-line change, and that is the point](#the-default-font-size-is-a-one-line-change-and-that-is-the-point)
- [The desktop icon grid wraps, and clips its labels](#the-desktop-icon-grid-wraps-and-clips-its-labels)
- [The Control Panel's applets are a registry table, not gui_apps](#the-control-panels-applets-are-a-registry-table-not-gui_apps)
- [`gfx_draw_string()` doesn't clip to a width -- callers that need that do their own](#gfx_draw_string-doesnt-clip-to-a-width----callers-that-need-that-do-their-own)
- [`widgets.h`/`theme.h` stay minimal on purpose](#widgetshthemeh-stay-minimal-on-purpose)
- [The window manager is one event loop, not decoupled components](#the-window-manager-is-one-event-loop-not-decoupled-components)
- [Esc no longer exits the GUI desktop -- it's unclaimed at the WM level now](#esc-no-longer-exits-the-gui-desktop----its-unclaimed-at-the-wm-level-now)
- [Don't put a `text_scrollback` on the stack](#dont-put-a-text_scrollback-on-the-stack)
- [Console scrollback is a character ring in vga.c, and the boot log is echoed to it](#console-scrollback-is-a-character-ring-in-vgac-and-the-boot-log-is-echoed-to-it)
- [Button press/release feedback: a general `on_press`/`on_release` WM mechanism, not a Calculator-only hack](#button-pressrelease-feedback-a-general-on_presson_release-wm-mechanism-not-a-calculator-only-hack)
- [ui_button/ui_button_group: Brutal-OS-inspired, but not a full retained view system](#ui_buttonui_button_group-brutal-os-inspired-but-not-a-full-retained-view-system)
- [The display layer: cards are drivers, and capabilities must not lie](#the-display-layer-cards-are-drivers-and-capabilities-must-not-lie)
- [The entropy source stops short of a CSPRNG, on purpose](#the-entropy-source-stops-short-of-a-csprng-on-purpose)
- [Damage verification: the invariant nothing enforced](#damage-verification-the-invariant-nothing-enforced)
- [Both cursor paths record where they drew the sprite](#both-cursor-paths-record-where-they-drew-the-sprite)
- [A damage-verify failure renders the frame a THIRD time before believing itself](#a-damage-verify-failure-renders-the-frame-a-third-time-before-believing-itself)
- [GUI testing asks the kernel, rather than measuring a screenshot](#gui-testing-asks-the-kernel-rather-than-measuring-a-screenshot)
- [The WM clips each app's on_draw() to its window -- containment, not optimisation](#the-wm-clips-each-apps-on_draw-to-its-window----containment-not-optimisation)
- [CPU info: one syscall, because "supported" and "enabled" sit on opposite sides of a privilege boundary](#cpu-info-one-syscall-because-supported-and-enabled-sit-on-opposite-sides-of-a-privilege-boundary)
- [Floating point is ring-3 only, and eager -- the same call Linux and Windows made](#floating-point-is-ring-3-only-and-eager----the-same-call-linux-and-windows-made)
- [Button groups commit on RELEASE, and there is no ui_button_group_click()](#button-groups-commit-on-release-and-there-is-no-ui_button_group_click)
- [Start menu click flash: a deferred close via pit_ticks(), not a blocking sleep](#start-menu-click-flash-a-deferred-close-via-pit_ticks-not-a-blocking-sleep)
- [Title-bar buttons: press-then-commit-on-release, reusing the content_pressed shape](#title-bar-buttons-press-then-commit-on-release-reusing-the-content_pressed-shape)
- [apps/ui/: a directory for retained-widget objects, once there were three](#appsui-a-directory-for-retained-widget-objects-once-there-were-three)
- [Calculator is the first `multi_instance` GUI app](#calculator-is-the-first-multi_instance-gui-app)
- [`apps/widgets.c`/`.h` no longer exist -- and `ui_scrollback`/`ui_scrollbar` didn't get an owned-geometry wrapper](#appswidgetsch-no-longer-exist----and-ui_scrollbackui_scrollbar-didnt-get-an-owned-geometry-wrapper)
- [The desktop's right-click quick-launch menu doesn't distinguish icons from empty space](#the-desktops-right-click-quick-launch-menu-doesnt-distinguish-icons-from-empty-space)
- [`context_menu.h`'s items carry a `void *ctx`, but `start_menu.h`'s don't](#context_menuhs-items-carry-a-void-ctx-but-start_menuhs-dont)
- [Click-to-position/selection lives in the shared `text_scrollback` widget, not a Notepad-only one](#click-to-positionselection-lives-in-the-shared-text_scrollback-widget-not-a-notepad-only-one)
- [The file picker is a WM-level modal overlay (`apps/wm/file_picker.c`), not an `apps/ui/` widget](#the-file-picker-is-a-wm-level-modal-overlay-appswmfile_pickerc-not-an-appsui-widget)
- [`struct window *` isn't a stable per-window identity across frames -- don't cache one](#struct-window--isnt-a-stable-per-window-identity-across-frames----dont-cache-one)
- [Why the compositor uses one scene-wide damage region, not per-window exposure tracking](#why-the-compositor-uses-one-scene-wide-damage-region-not-per-window-exposure-tracking)
- [The taskbar/tray falls back to full-screen repaint on purpose, not as an oversight](#the-taskbartray-falls-back-to-full-screen-repaint-on-purpose-not-as-an-oversight)
- [An overlay forces a full repaint, because "declares no damage" is not the same as "is drawn unrestricted"](#an-overlay-forces-a-full-repaint-because-declares-no-damage-is-not-the-same-as-is-drawn-unrestricted)
- [A dropdown's popup is a second draw call the app makes last, not a WM overlay](#a-dropdowns-popup-is-a-second-draw-call-the-app-makes-last-not-a-wm-overlay)
- [ui_listbox counts scroll from the top; ui_scrollbar counts from the bottom](#ui_listbox-counts-scroll-from-the-top-ui_scrollbar-counts-from-the-bottom)
- [GUI tests wait on the WM's queue depth, not on a sleep derived from frame rate](#gui-tests-wait-on-the-wms-queue-depth-not-on-a-sleep-derived-from-frame-rate)
- [Modifier keys ride alongside the key, they don't re-encode it](#modifier-keys-ride-alongside-the-key-they-dont-re-encode-it)
- [Keyboard focus is an app-level ring with a per-widget ops table, not a WM concept](#keyboard-focus-is-an-app-level-ring-with-a-per-widget-ops-table-not-a-wm-concept)
- [An empty clip rect draws nothing -- it is not gfx_clear_clip_rect()](#an-empty-clip-rect-draws-nothing----it-is-not-gfx_clear_clip_rect)

**Shell, apps & console**

- [Terminal wraps the real shell, it doesn't reimplement it](#terminal-wraps-the-real-shell-it-doesnt-reimplement-it)
- [Tab completion is a shared candidate generator, not a shared line editor](#tab-completion-is-a-shared-candidate-generator-not-a-shared-line-editor)
- [Ctrl/Alt are encoded as control codes and an ESC prefix, not as new key codes](#ctrlalt-are-encoded-as-control-codes-and-an-esc-prefix-not-as-new-key-codes)
- [PATH lives in the shell, not the kernel -- and builtins win over it](#path-lives-in-the-shell-not-the-kernel----and-builtins-win-over-it)
- [Shell session state is initialised by the DISPATCHER, not by the REPL](#shell-session-state-is-initialised-by-the-dispatcher-not-by-the-repl)
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

- [The demo ISO is a separate image, and its tour is a file on it](#the-demo-iso-is-a-separate-image-and-its-tour-is-a-file-on-it)
- [Build-number scheme: fix/feature/major tiers, not dates or semver](#build-number-scheme-fixfeaturemajor-tiers-not-dates-or-semver)
- [Versioning: semver + `-dev` suffix, not a per-change build number](#versioning-semver---dev-suffix-not-a-per-change-build-number)
- [A dev build shows its commit; a release shows only its version](#a-dev-build-shows-its-commit-a-release-shows-only-its-version)
- [Repo is MIT; the baked JetBrains Mono glyph data is separately SIL OFL 1.1](#repo-is-mit-the-baked-jetbrains-mono-glyph-data-is-separately-sil-ofl-11)
- [CI is kept for the environment, not the checks -- they duplicate `make verify` exactly](#ci-is-kept-for-the-environment-not-the-checks----they-duplicate-make-verify-exactly)
- [Repo history scrubbed of the maintainer's real name -- privacy request, not a bug fix](#repo-history-scrubbed-of-the-maintainers-real-name----privacy-request-not-a-bug-fix)
- [The repo lives in an organization because a personal repo has no read-only collaborator](#the-repo-lives-in-an-organization-because-a-personal-repo-has-no-read-only-collaborator)
- [The account rename, and the second history scrub -- scoped by measuring, not by instinct](#the-account-rename-and-the-second-history-scrub----scoped-by-measuring-not-by-instinct)
- [A backup of this repo is not a `git clone`](#a-backup-of-this-repo-is-not-a-git-clone)
- [Socket fds: scaffolding ahead of the driver, not a working transport](#socket-fds-scaffolding-ahead-of-the-driver-not-a-working-transport)
- [Header dependency tracking is a `find`, and a test proves it works](#header-dependency-tracking-is-a-find-and-a-test-proves-it-works)
- [Userland programs link one archive, and name nothing](#userland-programs-link-one-archive-and-name-nothing)
- [The GUI stack has names: TWP, TWS and Toykit](#the-gui-stack-has-names-twp-tws-and-toykit)
- [Ring-3 GUI apps are callbacks and a layout, not a loop](#ring-3-gui-apps-are-callbacks-and-a-layout-not-a-loop)
- [Parallel test VMs lease a slot, they don't derive one from their position](#parallel-test-vms-lease-a-slot-they-dont-derive-one-from-their-position)

**Session workflow & environment**

- [Protected files: `device_commit_files` blocks writes, `device_bash` doesn't](#protected-files-device_commit_files-blocks-writes-device_bash-doesnt)
- [The device bridge can't delete files, and `git` leaves stale locks behind on it](#the-device-bridge-cant-delete-files-and-git-leaves-stale-locks-behind-on-it)
- [Cowork device-bridge vs. direct local checkout: detected via `git config user.name`, not assumed](#cowork-device-bridge-vs-direct-local-checkout-detected-via-git-config-username-not-assumed)

## The TWP transport seam has exactly one implementation, so it is UNVALIDATED

Milestone 41's stage 3 added `struct win_transport`
(`kernel/include/kernel/win_transport.h`) -- the same
one-struct-of-function-pointers registry as `display_driver` and the VFS
backend probe -- and there is exactly ONE implementation behind it, the
`direct` one that calls `win_server_request()`/`win_server_debug()` in
process. So nothing proves the interface is not simply syscall-shaped.

That matters because this repo's standing rule is the opposite: an
unreachable path is a guess, which is why `ata nodma`, `nopat` and TFS3
v1 exist to keep fallbacks producible. An abstraction with one
implementation is the same problem wearing a different hat, and saying
so is cheaper than pretending otherwise.

**What is most likely wrong with it, named in advance so stage 4 checks
these first:**

- **Batching.** `request()` is one message in, one answer out,
  synchronously. A ring transport wants to submit many and collect
  later, and the WM's ops are called inline from inside that call.
- **Who owns the copy.** The reply buffer lives in `win_server.c`
  between the WM and the transport, which suits a carriage that must
  chunk. A transport that could hand over the whole reply at once (a
  shared ring) would want to skip that copy entirely, and the current
  shape gives it no way to say so.

A cheap throwaway second implementation was considered and rejected: it
would demonstrate the seam is not *syscall*-shaped without demonstrating
it fits anything real, which is the only question worth answering. The
real second implementation is stage 4's, and the honest position until
then is that this seam is untested design, not proven design.

Deliberately NOT built in this stage: the shared-memory ring. It is a
performance item, not a prerequisite -- stage 4 needs the WM to talk
over *something*, and the syscall path already does. See
`docs/wm-ring3-design.md`'s "Scope DECIDED" note.

## `gui` output goes to a caller-supplied sink, not through a klog redirect

`apps/wm/wm_debug.c` wrote its answers with 143 `klog_write()` /
`klog_printf()` calls, straight to whatever the serial console was
connected to. Stage 3 needs that output to become a PAYLOAD, since the
console now reaches the WM over the transport and a message carries
bytes rather than side effects on a serial port.

The cheap way was a global capture: `klog_set_capture(buf, cap)` for the
duration of a command, leaving all 143 sites untouched. It was rejected,
and the reason is the interesting part -- a redirect is GLOBAL, so
kernel log lines emitted *during* a command get swallowed too. That is
not hypothetical: `gui spawn` provokes ELF-loader logging, `gui open`
provokes the WM's, and `gui damage verify on` provokes the damage
reports. Every one of those is something a test reads back through
`DebugConsole.logs()` or `damage_bugs()`, so the capture would have
quietly moved them out of the serial log and into a reply nobody parses
for them. A green suite would have stayed green while the diagnostics it
depends on went missing.

With an explicit `struct dbg_out` (`apps/wm/wm_debug.h`) only this
file's own output is captured and the kernel log is untouched. The cost
is a mechanical 143-site diff and a sink parameter threaded through
~14 functions, which is a one-time price for a property that holds by
construction afterwards.

Note the sink deliberately breaks `kernel/lib`'s formatter rule (a value
that does not fit writes NOTHING rather than a truncated one): it
truncates and sets `overflow`. The difference is what the value IS -- a
half-written number is wrong, whereas a transcript that stops early is
merely shorter, and the flag is what keeps that visible instead of
silent.

## The diagnostic channel carries its own payload struct, so presents stay cheap

`WIN_REQ_DEBUG_CMD`'s command and its reply ride `struct win_debug_msg`
rather than the two structs every other message uses. This looks like a
second mechanism and is worth explaining, because the staging note for
stage 3 explicitly rejected a side channel.

It is not one: these are ordinary `WIN_REQ_*`/`WIN_EV_*` types going
through the one transport and the one entry point, so a ring-3 window
server inherits the diagnostic path with nothing to re-plumb. What is
separate is only the PAYLOAD, and it has to be. A reply is text and runs
to kilobytes -- `gui help` measured 1699 bytes and `gui windows --json`
grows with the window count -- while `struct win_event` is a fixed 24
bytes and `struct win_request_msg` carries `text[32]`. Neither can hold
one.

The alternative was widening those, and that is the trap: `WIN_REQ_PRESENT`
is sent once per client frame, and the syscall path copies the whole
message in and back out on every request. Widening the shared struct to
hold a diagnostic reply would put a kilobyte-sized copy on the hot path
to serve a channel used only by test tooling. So the hot path keeps its
56-byte message and the diagnostic pays for its own size.

## A table PULLS its rows, and stores none of them

`uui_table` (`userland/ui/uui_table.h`) holds no data. It asks the app
for one cell at a time through a `cell(ctx, row, col, out, cap)`
callback, and the app supplies `row_count`.

The obvious alternative -- hand the widget an array of rows -- needs
somewhere to put a copy, and Toykit has no allocator (which is also why
a menu is a const tree). Task Manager re-reads the whole process table
several times a second; copying it into the widget each time would mean
either a fixed maximum baked into the widget or an allocator built to
serve one caller.

Pulling has a second property that matters more than the memory: there
is no cached state to invalidate. Processes appear and exit under a live
view, and the table simply asks again on the next paint. A widget that
owned rows would need the app to remember to push updates into it, and
the failure mode of forgetting is a table showing plausible, stale
numbers -- which is worse than an empty one, because nothing looks
wrong.

What the app still owes: `uui_table_set_rows()` on a refresh, which
clamps the scroll position and the selection. That is a function rather
than an assignment precisely because both need clamping when the row
count shrinks.

## cpu_ticks is a total, not a percentage

`struct proc_info.cpu_ticks` (`kernel/include/abi/proc_info.h`) reports
the cumulative timer ticks a process has been the running one for. It
does not report a percentage, and the kernel deliberately does not
compute one.

A percentage is a difference between two samples divided by the time
between them, and only the consumer knows how far apart its samples
are. Reporting a percentage would bake the kernel's chosen interval into
the ABI, and any reader refreshing at a different rate would then be
reading a number that means something other than what it says. Linux
makes the same call with `/proc/stat`, which reports totals and leaves
the arithmetic to `top`.

The consequence is that a consumer needs a common clock for the
denominator, which is what `SYS_TICKS` is for -- the SAME counter
`scheduler_tick()` bills against. Dividing a `cpu_ticks` delta by a
delta of anything else (the RTC, say) gives a ratio of two unrelated
clocks that happens to look like a percentage.

## SYS_KILL is unprivileged, and killing the WM is the point

Any process may kill any other, including the window manager once it is
a ring-3 process. There is no permission check, and that is a decision
rather than an omission.

There is nothing to hang a check on. This kernel has no user model, no
capability system and no process groups, so a permission test would have
to invent the very concept it claims to enforce -- and a check that
always passes is worse than no check, because it reads as protection.
The header says so plainly and names `SYS_KILL` as the first syscall a
future privilege model has to gate.

Protecting the window manager specifically was considered and rejected
for a sharper reason: Milestone 41 stage 4's stated exit criterion is
that **killing the WM process must not panic the kernel**. Being able to
do it from Task Manager is a way to exercise the property the milestone
exists to establish, not a hole in it.

The polite counterpart is `WIN_REQ_CLOSE_PID`, which asks the target's
window to close and can be refused -- it runs the same
`wm_request_close()` the X button and Alt+F4 use, so there is no fourth
close path with its own rules. Windows draws the same line between End
Task and End Process.

## Single-instance is the app's decision, and the launcher always launches

Opening Task Manager twice raises the window that already exists instead
of opening a second one. The mechanism is `WIN_REQ_ACTIVATE` plus
Toykit's `UAPP_SINGLE_INSTANCE`, and the decision it encodes is **which
side gets to say whether a second copy may run**.

The obvious implementation is the wrong one here: have the Start menu
notice that it already launched this entry and focus that window instead
of spawning. It needs no protocol change and it was rejected, because
the desktop does not know enough to make the call. Two Notepads editing
two files are useful; two Task Managers are not. That is a fact about
the program, so `apps/gui_apps.h` has always said a launcher **spawns**
and never focuses -- and a launcher-side rule would also only cover the
desktop, leaving `run taskmgr` in a Terminal and `gui spawn` to open
duplicates that the menu refused.

So the program asks. A client that declares an `app_id` and the
single-instance flag sends `WIN_REQ_ACTIVATE` **before it creates
anything**: TWS raises the matching window and answers 1, and the second
copy exits 0 without ever appearing on screen. A client that declares
nothing behaves exactly as every client did before. This is the pattern
real desktops converge on -- Windows' named mutex plus
`SetForegroundWindow`, GApplication's uniqueness, `QtSingleApplication`
-- and the **raise** is the half that makes it feel correct rather than
broken; an app that merely declines to start looks like a failed launch.

Three smaller calls inside it:

**The id rides `WIN_REQ_CREATE`'s `text` field**, which was unused by
that request, rather than becoming a message of its own. A separate
"register my id" message would leave a window briefly existing without
one -- and that gap is exactly long enough for a second copy to ask "is
anyone there?" and be told no.

**The id is an opaque token, not a path or a title.** `"taskmgr"`, not
`/bin/wm/system/taskmgr` (which breaks the moment a binary moves --
which has already happened once to every windowed binary in this repo)
and not `"Task Manager"` (which collides with the title the user sees
and that Notepad rewrites per file).

**It is not a lock, and the header says so.** Nothing serialises the ask
against the create, so two copies launched in the same instant can both
be told "nobody there" and both open. Claiming the id in the ask would
fix it and needs state to release on a crash; every launch path here is
a human clicking a menu, so the gap is recorded rather than closed. Do
not build mutual exclusion on this.

`tools/single_instance_test.py` covers it, and its own positive control
is worth knowing about: with the raise disabled, the check "the relaunch
brought Task Manager to the front" **stayed green**, because a brand-new
window is frontmost too. It compares the window's `client_pid` against
the original's now. "It is on top" and "it is the same window" are
different claims, and only the second one tests anything.

## A yield is not a tick: SYS_YIELD reschedules without billing

Task Manager and Shapes both showed **100% CPU at the same time**,
which on a single CPU is impossible -- and that impossibility, visible
in a screenshot, was the whole diagnosis. It was an accounting bug, not
a scheduling one.

`SYS_YIELD` rescheduled by calling `scheduler_tick()`, the timer's own
entry point. The reasoning written at the time was that a yield is
"indistinguishable from the timer happening to fire right now", and for
the *rescheduling* that is exactly right -- the trapframe is laid out
identically and reusing one rotation beats maintaining two. It is wrong
for the *billing*, because `scheduler_tick()` also does
`cpu_ticks++`, and a tick is a unit of ELAPSED TIME while a yield
elapses microseconds.

The magnitude came from a detail worth knowing: a yield returns only
when the process is next scheduled, about a tick later. So a polling
app yields roughly 100 times a second and charged itself 100 ticks a
second against a 100Hz clock -- a clean, stable 100%. Every polling app
did, simultaneously. Anything that BLOCKED (`SYS_WAIT_EVENT`) never
yielded and read an honest 0%, which is what made it look like a
polling problem rather than an accounting one.

So the rotation is now `scheduler_rotate(regs, bill)`, with
`scheduler_tick()` passing 1 and `scheduler_yield()` passing 0. That
makes accounting **sampled**: whoever is current when the timer lands
pays for the whole tick. The known bias is that a process yielding
constantly is undercharged, and it is the honest direction to be wrong
in -- the sum can no longer exceed 100%, where before every polling
process independently claimed all of it. Measured after: a CPU-bound
`spin_test` reads ~50% (the WM's kernel context takes the rest) while
timer-paced clients read 0%, so the column discriminates. The upgrade,
if precision is ever wanted, is a TSC delta per switch; it is in
`docs/roadmap.md` rather than built.

**The test lesson is the reusable part.** The obvious assertion --
billed must not EXCEED elapsed -- does not catch this, and the positive
control is what proved it: the bug produces `billed == elapsed`
exactly, so a `>` comparison stayed green against a kernel that was
actively wrong. The real assertion is that a process doing nothing but
yielding must be billed SUBSTANTIALLY LESS than the whole window
(`userland/tests/cputime_test.c`). Ask what value the bug actually
produces, not merely which direction it errs in.

It also cannot be driven by `usertest_run.py`: `run` is the legacy
`process_run_ring3()` path with no `procs[]` slot, so the test can find
neither itself nor any billing. `kernel/proc/cputime_test.c` spawns it
properly, the same arrangement `pipe_test` already needed.

## A client blocks between frames -- WIN_EV_TIMER, not a polling loop

A Toykit app with an `on_tick` used to run its loop flat out: tick,
pump without blocking, `sys_yield()`, repeat. That wakes a process 100
times a second whichever cadence it actually wanted -- Task Manager
counted 40 passes to refresh about twice a second, so 98% of its
wake-ups existed only to decide it had nothing to do.

`WIN_REQ_TIMER` arms a repeating timer on a window and `WIN_EV_TIMER`
delivers it, so the app blocks in `SYS_WAIT_EVENT` in between and
`on_tick` arrives as an ordinary event. An app names `tick_ms` and
nothing else changes: Task Manager asks for 500ms, Shapes for 10ms
(one frame per tick, the cadence its yield loop already happened to
run at, so its rotation speed is unchanged).

Four decisions inside it:

**Milliseconds, not ticks.** The tick rate is the kernel's business,
and a client asking to be woken every 500ms should not have to know it
is 100Hz today. The server rounds to whole ticks and floors at one --
an interval faster than the resolution becomes "every tick" rather than
an error, and crucially rather than zero, which would fire every frame
and turn a request to slow down into the busiest possible loop.

**A deadline, not a queue.** The next firing is computed from NOW, not
by adding the interval to the previous deadline. Those differ only when
a client is slower than its own timer, and the second form silently
accumulates overdue firings that all arrive at once when it catches up
-- the opposite of what a client asking for less frequent wake-ups
wanted. Same reasoning as the event queue dropping the oldest.

**One timer per window.** A client wanting several derives them from
one short interval, exactly as an app does on top of a frame clock. A
general timer service is a bigger feature than anything here needs.

**Polling stays as the fallback.** `tick_ms` of 0, or a server that
declines the request, leaves the old loop in place. That is what keeps
this additive: no existing app changed behaviour by not opting in, and
an older server does not produce an app that simply never ticks.

This also fixed `PIT_HZ` being a bare literal at the `pit_init()` call
and a "100 Hz" remark in two comments -- fine until something had to
convert milliseconds to ticks and would have hardcoded it a fourth
time, where being wrong makes every interval silently the wrong length.

## A lone button routes its own clicks; the group is for grids

`uui_button_ops` used to have `draw` and `hit` and nothing else, so a
single declared button was painted, hit-tested, and ignored every click
-- routing lived only in `uui_button_group`. Even one button therefore
needed a group wrapped around it.

That is not how the toolkits this is modelled on behave. A `QPushButton`
and a Win32 `BUTTON` both handle their own click; Qt's `QButtonGroup`
exists for EXCLUSIVITY (radio behaviour), not for delivering the press.
It is also a silent trap of exactly the kind this repo keeps paying for:
the button draws correctly, hit-tests correctly, and does nothing, with
no error anywhere to point at the cause.

So `uui_button_ops` gained `press`/`motion`/`release`, implementing the
same arm-on-press, commit-on-release, don't-commit-if-dragged-off rule
the group implements. The change is ADDITIVE -- the group still routes
its own buttons, so Calculator's keypad is untouched.

What the group is still good for is a GRID of many buttons handled as
one widget, which is genuinely less code than twenty layout items. It is
on `docs/roadmap.md`'s list to retire once that is no longer worth a
separate widget.

## A natural size must not depend on where the widget currently sits

`uui_button_group_natural_size()` computed its buttons' far edge FROM
THE ORIGIN -- `x1`/`y1` started at 0 and took the max of `b->x + b->w`.
That is the union's extent only while the group sits at (0,0), which
held for exactly as long as nothing ever moved a group.

The moment one took part in a layout it broke: placed at y=284, the
group reported a natural HEIGHT of 312 -- its offset plus its size. In a
column that inflated the space the layout believed its children needed,
so the growth allowance for the widget above it was eaten by a number
that was really a coordinate. The visible symptom was Task Manager's
table growing 16 px against a 300 px window resize, which reads as a
broken resize path rather than as a broken measurement.

The rule: a natural size is the size a widget WANTS, asked before anyone
has decided where it goes. One that varies with the widget's current
position is a feedback loop between layout and measurement, and it
converges on a wrong answer rather than failing outright.

It reports `max - min` per axis now. Callers whose widgets start at the
origin are unaffected, which is why nothing caught it earlier.

## A widget's ops->hit is a boolean, and a row index is not one

`uui_route.c` tests the slot as `!it->ops->hit(...)`. A widget whose own
`_hit()` returns a ROW INDEX therefore has to convert -- because row 0
is the one row whose index is falsey, so it reports "not hit" and cannot
be clicked, while every other row works.

`uui_listbox` shipped that way. The failure is close to invisible: the
widget draws, scrolls, highlights on hover and selects rows 1..n
perfectly, and only the first row is dead. It survived a 42-check test
tool. It was found by building `uui_table`, which copied the line and
reproduced the bug, and only then failed loudly enough to trace --
Task Manager's first row is the one a test naturally clicks.

`uui_radio_list` had `>= 0` all along; every other widget's `_hit()`
returns `uui_hit()`, which is already a boolean. So the trap only
applies to widgets that report WHICH item was hit, and there are exactly
two of those.

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

`ui_radio_list`, and later `ui_listbox`/`ui_dropdown`, were added the
same way -- ahead of a second caller, by explicit user request. Worth
being honest that this is now the majority of the recent additions
rather than a one-off exception, so the rule is doing less work than
its wording suggests. What it still buys is the *shape*: each of these
arrived as a real widget with its behaviour inside it rather than as
a helper an app calls, which is the part that actually prevents the
half-implemented second copy (see `docs/gui-guidelines.md`'s
"Behaviour belongs to the component"). `ui_dropdown` is a data point
for that: it needed a scrolling, keyboard-navigable, hover-tracking
list, and composing `ui_listbox` meant writing none of it twice.

## The Control Panel's applets are a registry table, not gui_apps

`apps/control_panel.c` holds a static `struct applet` table -- name,
`draw(x,y,w,h)`, `click(...)` -- deliberately mirroring
`gui_apps.h`'s `gui_app_registry[]` rather than inventing a second
plug-in convention. Adding an applet is adding a row, the same property
the app registry has, and a reader who knows one knows the other.

An applet is explicitly *not* a `gui_app`: no window of its own, it
draws into a rectangle the Control Panel gives it, and it can't be
opened from the Start menu. That was the alternative considered
(applets as ordinary apps, Control Panel as a launcher) and it was
rejected for making "Control Panel" a menu rather than a panel, and for
putting one Start-menu entry per setting.

Two things shipped with it worth keeping:

**Two applets from the start, not one.** Date & Time is the real one;
System Info is read-only and trivial. A plug-in mechanism with exactly
one plug-in demonstrates nothing about being pluggable -- the second
entry is what makes the icon grid, the drill-in and the Back button
meaningful instead of an elaborate way to show a single page.

**The applet doesn't cache its setting.** The timezone applet reads
`tz_current_index()` at draw time and writes `tz_set_index()`, which
persists to `/etc/toyos.conf` itself. A local copy would be a second
source of truth, and the `timezone` shell command can change the same
setting behind the window's back.

Two bugs caught by QMP testing rather than review, both of the "draws
perfectly, does nothing" kind: `on_click`'s coordinates are
**content-relative** while everything drawn is absolute, so the first
version hit-tested in the wrong space and clicks silently did nothing;
and the icon labels were laid out in a hardcoded 104px cell that "Date
& Time" overflows, which `gfx_draw_string()` cheerfully drew straight
over the neighbouring label (see the entry below on that function not
clipping -- this is that lesson recurring, in a new file, four days
after it was written down).

See `apps/control_panel.c`'s top comment and CHANGELOG.md's
`[Unreleased]`.

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
view while the field is active. The lesson recorded at the time was:
`gfx_draw_string()` will not save you, budget the width yourself.

**That lesson did not hold, and the fix is now a function.** The very
next caller to draw text into a fixed box -- the Control Panel's applet
labels -- hit the identical bug, rendering `Date & TSystem Info`, in a
file written days after this entry existed. A rule that has to be
remembered at every call site is one that will be forgotten at some call
site, so the budgeting now has somewhere to live:
`gfx_draw_string_clipped(x, y, max_w, ...)` draws bounded and returns
whether the string fitted, and `gfx_text_width()`/`gfx_text_fit_chars()`
are the measurement half for callers doing their own windowing.
`gfx_draw_string()` itself is unchanged -- clipping it would alter every
existing caller -- so the guidance is now "use the clipped variant for a
fixed box" rather than "remember to do this by hand".

`gfx_text_width()` also pays a later debt: Milestone 21 (proportional
font metrics) lists exactly that function as something it needs, and
every `k_strlen(s) * gfx_char_w()` open-coded at a call site is a place
that silently breaks when a glyph stops being one cell wide. See
CHANGELOG.md's `[Unreleased]` entries.

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

## Thin provisioning: the image is sparse at birth, and TRIM is what keeps it that way

`disk.img` is created with `truncate -s 9G`, so it costs nothing up
front. But sparseness is only ever LOST: a block written once stays
allocated on the host forever, even after toy-os deletes the file that
owned it. The bitmap bit clears, the host is never told, and the file
only grows.

Measured on the development image before any of this existed: **8.1 GiB
actually allocated against 581 blocks (2.3 MiB) that TFS2 considered in
use** -- 99.97% of it the leftovers of past `stress` runs. `fsck`
reported the filesystem completely clean, because it was: nothing had
leaked *inside* the filesystem, the space simply never went back to the
host. That is the whole problem in one sentence, and it is why "the
image is sparse" was true and useless at the same time.

Both halves of the fix exist, deliberately:

- **`tools/tfs2_writer.py trim`** reads the allocation bitmap and
  punches holes (`FALLOC_FL_PUNCH_HOLE`) through every run of free
  blocks. It reclaims images that are already in that state, and covers
  the host-side seeding path, which never goes through the kernel at
  all. Non-destructive: only blocks the filesystem already considers
  free are touched.
- **`ata_trim()`**, issued from `free_block()` (`kernel/fs/tfs.c`) as
  blocks are freed, with `discard=unmap` on every QEMU `-drive` line.
  QEMU turns the guest's TRIM into a hole punch, so an `rm` inside
  toy-os gives the space back with no host tool involved.

The result is measurable: `stress 150` writes 150 MB, verifies it,
deletes it, and the image is unchanged at 2.3 MiB. Before, that run cost
150 MB of host disk permanently.

The kernel deliberately ignores `ata_trim()`'s result. TRIM is an
optimisation -- the block is free either way, and a drive that refuses
it (or doesn't support it, which `ata_trim_supported()` answers from
IDENTIFY word 169) must not turn a successful delete into a failed one.

## ATA DATA SET MANAGEMENT must be issued over DMA, not PIO

DSM (the TRIM command) reads like an ordinary PIO data-out command in
the spec: set the TRIM bit in Features, put the descriptor-block count
in Sector Count, write 512 bytes of LBA ranges. The first implementation
here did exactly that, and it **silently did nothing** -- the drive
accepted the command, raised no error, returned success, and not one
byte was discarded.

QEMU dispatches DSM through `ide_sector_start_dma()` with
`IDE_DMA_TRIM` (`hw/ide/core.c`), so the range list has to arrive by
bus-master transfer. Over PIO it never arrives; the "success" is the
drive acknowledging a command whose payload it is still waiting for.

`ata nodma` does NOT stop it, which is worth knowing because it reads
like it should: that switch forces DATA transfers down the PIO path,
and TRIM keeps going out over the bus master regardless, because there
is nowhere else for it to go. Measured rather than assumed -- with PIO
forced, `stress 30` still leaves the image at its pre-run size. What
DOES disable TRIM is Bus-Master DMA never coming up at all, and
`ata_trim_supported()` accounts for that so the `ata` command can't
report "supported" on a machine where every TRIM would fail silently.

Worse, the half-issued command leaves the channel desynced, and the
next few ATA commands return garbage. That is what produced a burst of
`ata: refusing transfer past end of drive` complaints with absurd LBAs
and a `stress` run that leaked all 10,237 of its blocks -- neither of
which was a filesystem bug at all. Worth knowing before trusting any
DSM return value, and a good reminder that "the command succeeded" and
"the command did something" are different claims.

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

## The PIO fallback is reachable on purpose (`ata nodma`), because unreachable fallback code is a guess

`kernel/drivers/ata.c` has two transfer paths: Bus-Master DMA, and a PIO
fallback for machines where DMA can't be brought up. `ata_init_dma()`
succeeds under QEMU and on ordinary PC hardware -- so the fallback had
never executed on any machine this OS boots, and there was no way to
make it. Roughly a hundred lines of driver that only run in an
emergency, and had never been observed running at all.

`ata_set_dma_forced_off()` (the `ata nodma on|off` command) exists to
close that. It is not a debugging convenience bolted on: it's what makes
the path testable, and `kernel/drivers/ata_test.c` drives the same
switch so PIO executes on every `make test`. The first time it ran, it
worked -- which is the outcome that was *hoped for* before and merely
assumed.

It has a second use that isn't hypothetical. Comparing a known-good PIO
transfer against a suspect DMA one is how an earlier session root-caused
a DMA failure to a host-side filesystem stall rather than a driver bug;
at the time that comparison required hand-editing the driver.

**The implementation detail worth keeping:** every place that asks "DMA
or PIO?" goes through a single `dma_in_use()` helper rather than testing
the flags itself. `ata_max_sectors_per_xfer()` reports a smaller cap on
PIO (8 sectors vs 128), and `tfs.c` batches block writes against that
number -- so a dispatch site that disagreed with the cap site by even
one condition would hand the PIO path a transfer it cannot carry. One
helper makes that disagreement unexpressible.

Related, same file, same session: `wait_drq()` now records *why* it
failed in `g_pio_fail_reason`, mirroring `g_dma_fail_reason` -- a
pass/fail return for control flow, a reason string for whoever reads
`dmesg`. See `ata.c` and CHANGELOG.md's `[Unreleased]`.

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
`struct fs_ops` backend. **Updated at Milestone 15:** there are two
backends now (`tfs3_ops` and `tfs_ops`), and selection is a
boot-time superblock probe rather than a compile-time constant --
see the entry below on TFS2 staying as a second filesystem. What has
NOT changed is this entry's actual decision: exactly one backend is
ACTIVE at a time, and adding a filesystem means registering it in
vfs.c's priority list with `probe()`/`wipe()`/`format()`/`init()` +
a caps bitmask -- not routing different path prefixes to different
backends simultaneously. Mount points remain meaningfully more code
(cross-mount path resolution, boundary conflicts) for a capability
nothing needs yet. See `fs_ops.h`'s top comment and
CHANGELOG-archive-2.md's **Build 304**
for the original reasoning, including what it would take to add mount
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
**Build 480** for the full writeup. **This is TFS2's rule.** The
"would need more than this" prediction came true at Milestone 15:
TFS3's mutations touch several metadata blocks, and it ships the
4-slot transaction journal this entry anticipated -- see the entry
below on TFS3's journal scope.

## File timestamps: broken-down local time on disk (TFS2), epoch seconds at the API since M15

`fs_stat()`'s `created`/`modified` fields (build 480) are `struct
rtc_time` -- the same hour/minute/second/day/month/year struct
`SYS_GETTIME` and the shell's `time` already return -- not a Unix
epoch integer. This kernel has never needed a civil-date<->epoch
conversion for anything else (no code anywhere computes "days since
1970" or similar), so storing the same struct everything else already
uses avoided adding one just for this feature; a host-side tool
reading a TFS2 image converts to epoch seconds itself if it wants
that instead (`docs/tfs2-spec.md`'s reference reader shows the
equivalent conversion via Python's `datetime`). **Updated at
Milestone 15:** the "never needed a conversion" premise expired --
`tz_rtc_to_epoch()`/`tz_epoch_to_rtc()` exist now (tz.c), the
`fs_stat()` API reports epoch seconds on every backend, and TFS3
stores epochs natively; TFS2's 7-byte on-disk civil fields are
unchanged and converted at stat time. The no-zone-recorded caveat
below still applies to both formats -- these are LOCAL-derived
epochs. The tradeoff: no
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
the record budget -- which Milestone 15 has since split in two: on a
TFS2 image, `FS_MAX_FILES` is 256 records with directories counting
against it; on TFS3 (the default for fresh images) the on-disk budget
is effectively gone (~590k inodes), and only the caller-side
`FS_PATH_MAX` = 64 path buffers still bind. See
`docs/filesystem-layout.md`'s budget section for the current rules.

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

`struct text_scrollback` (`apps/ui/ui_scrollback.h` today; `widgets.h`
when this was written) embeds an 8192-cell buffer
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
  mostly zeros. **Run `tools/tfs2_writer.py trim disk.img` first**:
  sparseness is only ever lost, so an image that has been used at all
  is carrying stale blocks it will happily compress. The dev image had
  reached 8.1 GiB of real data before TRIM existed -- see this file's
  thin-provisioning entry. `tools/run_release.sh` ships as a release asset (not
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
textfield, editor) and `userland/tests/echo.c` all
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
public `window_*` API) and `apps/ui/ui.h` (was `widgets.h`), never `wm_internal.h` (that's
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

**Two things above have since changed and are worth reading in the
past tense.** Notepad's toolbar did get migrated to `ui_button_group`
(so the tightened hitbox happened, deliberately). And "no
mouse-enter/leave dispatch" stopped being true when `gui_apps.h`'s
`on_hover` landed with the GUI guidelines: `ui_button` carries a
`hovered` flag beside `pressed` now, driven by
`ui_button_group_hover()`. See the entry below.

## The display layer: cards are drivers, and capabilities must not lie

`gfx.c` used to be a rasteriser AND the framebuffer's owner, and the
moment a second card existed it grew `#include "vmsvga.h"` plus seven
hardcoded calls to that device. `kernel/include/kernel/display.h` is
the interface that replaced it: required `probe`/`get_surface`, optional
`flush`/cursor/accel/modeset behind capability bits, with the registry
in `kernel/drivers/display/`.

Two decisions worth keeping. **GRUB's framebuffer is a driver**
(`vesafb`), registering last as the fallback that always claims -- which
removes the old default-path-vs-driver-path asymmetry AND is the second
implementation that makes the interface a design rather than a guess.
It's deliberately the opposite kind of device from `vmsvga`: passive,
scanned, no cursor, no accel, no modeset, so every optional part of the
interface is exercised by exactly one of the two.

And **`display_probe()` refuses a driver whose caps and function
pointers disagree.** That looks like paranoia and isn't: a card that
needs a flush and doesn't get one renders perfectly into memory and
shows a frozen screen. This project paid for that bug twice in one
session before the check existed.

## Damage verification: the invariant nothing enforced

The WM repaints only the declared damage region, which is correct only
if everything that changes is declared -- from eight sites across three
files, by hand. A miss is stale pixels with no crash and no failing
assertion, and every rendering bug here has been that shape.

`gui damage verify on` renders each frame twice, damage-limited then
unrestricted, and reports any differing pixel. It found four real bugs
in its first minute: a window losing focus repainting its title bar
undeclared, the taskbar clock relying on a full-repaint fallback that
other damage cancels, the alpha-blended cursor compositing over its own
previous frame, and the first frame of a session being narrowed by an
event that arrived before it.

The general lesson, which is why this is written down rather than just
built: when a subsystem's correctness rests on a convention every caller
must remember, the fix is not more care -- it's making the convention
checkable. See `CHANGELOG.md`.

## Both cursor paths record where they drew the sprite

`wm_render_frame()` (the full path) and `wm_render_cursor_move()` (the
cheap "mouse moved, nothing else changed" path) both draw the cursor, so
both set `prev_cursor_*` -- "where the sprite actually is", which is what
the next damage-limited frame uses to decide where to erase it from.

The cheap path used not to, and it looked safe: consecutive cheap moves
restore what the previous one saved, so nothing is left behind. What it
missed is a FULL frame landing while the cursor has already moved on --
that frame damages the position before the cheap move and the position
after it, never the one in between, and a cursor stays on screen. It was
filed as a harmless inconsistency in `docs/roadmap.md` for weeks while
its own symptom sat two entries above it, filed as an unexplained
"resize" damage violation. See `CHANGELOG.md`'s `[Unreleased]` entry --
including why a test driving this with `gui click` cannot catch it.

## A damage-verify failure renders the frame a THIRD time before believing itself

The two-render comparison above can only tell you the renders DISAGREED,
and there are exactly two ways that happens: a genuinely missed damage
declaration, or a `render_scene()` that isn't a pure function of the
frame's state -- in which case the comparison measured nothing. The
second isn't hypothetical, because the two passes don't do the same
work: the unrestricted one calls every window's `on_draw()`, while the
damage-limited one skips windows outside the damage box entirely.

So a report re-renders the same unrestricted frame once more, in the
same frame and under the same load, and states which case it is
("scene stable (real missed damage)" / "SCENE UNSTABLE -- verdict
void") alongside the diff's bounding box. This was built to settle a
recorded known issue whose two hypotheses needed opposite fixes and
which then failed to reproduce at all -- the probe stayed because the
next such report should not have to re-open the same question. See
`CHANGELOG.md`'s `[Unreleased]` entry, and note the harness half of it:
`damage_hunt.py` was scoring a crashed sweep as a PASS.

## GUI testing asks the kernel, rather than measuring a screenshot

The serial debug console gained a `gui` command family
(`apps/wm/wm_debug.c`) that reports window rects, Start-menu geometry,
hit-test results and the WM's own state, and can open windows and inject
clicks/drags/keys. It works while the desktop is up because
`debug_console_poll()` is already called from `wm_run()`'s idle loop.

The reason it exists: every GUI test before it derived its coordinates
from a screenshot by hand and then hardcoded them. `tools/gui_flow.py`
still carries `MENU_TOP_Y = 475` and `ITEM_H = 27` with a comment
recording that they had drifted once and been re-measured off a live
screenshot. The kernel computes those numbers; asking it removes the
whole category. (They were right, as it happens -- `gui menu` reports
475 and 27 -- but now that's checkable instead of assumed.)

Three properties worth knowing before using or extending it.

**Injected input enters below the PS/2 driver.** It goes into
`wm_run()`'s loop as a synthetic (x, y, buttons) triple, so it exercises
WM and app logic and proves nothing about the mouse driver or keyboard
layout. QMP remains the tool for "does input arrive at all", and for
anything whose answer is genuinely a picture.

**It has to be asynchronous.** The commands are dispatched from inside
`wm_run()` (that's where `debug_console_poll()` runs), so a `gui click`
that waited for its own events to drain would be blocking the very loop
that drains them. Enqueue-and-return is the only safe shape -- the same
trap that made a lazy CPU-clock calibration hang inside a syscall.

**A click is four events, not one.** Move, press, a held tick, release,
consumed one per frame. Every control in this GUI arms on press and
commits on release (`docs/gui-guidelines.md`), which only behaves
normally if press and release land on different frames.

It lives in `apps/wm/` because it reads the window table;
`kernel/core/debug_console.c` only recognises the word `gui` and routes
it, the same direction that file already reaches `apps/` for `sh`.

## The WM clips each app's on_draw() to its window -- containment, not optimisation

`wm_render_frame()` narrows the clip rect to a window's content area
around its `on_draw()` call, then restores the scene clip. That looks
like a compositor optimisation and isn't: the damage-region clip already
handles that. It's there because nothing else stopped an app from
drawing outside its own window, and one of them did -- shrinking the
Control Panel sent System Info's lower rows down the desktop, perfectly
legible, outside any frame.

Two things had to be true at once for that. `gfx_draw_string_clipped()`
bounds **width only** -- it takes a `max_w` and has no row budget, so
the name promises more than it delivers, and the horizontal edge clipped
correctly while the bottom didn't exist as a concept. And the only clip
active while apps drew was the damage box, which by construction covers
the desktop under a window.

The intersection is computed by hand in `wm_render.c` because
`gfx_set_clip_rect()` **replaces** the active rect rather than
intersecting it. Setting the content rect naively would have widened the
damage clip back out and quietly undone Phase 1+2's whole point -- worth
knowing before adding a second nested clip anywhere.

Apps should still budget their own height (`docs/gui-guidelines.md` says
so): clipped-away drawing still costs the CPU that produced it, and a
page that stops at the last row that fits looks better than one sliced
through its glyphs. The WM clip is the backstop, not the plan.

## CPU info: one syscall, because "supported" and "enabled" sit on opposite sides of a privilege boundary

`lscpu` could have needed no kernel help at all -- `CPUID` is an
unprivileged instruction, so a ring-3 program can read the vendor,
brand string, family/model/stepping, feature bits and cache topology
entirely by itself. That's the opposite of `lspci`, which needs
`SYS_PCI_COUNT`/`SYS_PCI_INFO` because PCI config space is port I/O.

What ring 3 *cannot* do is read `CR0`/`CR4`/`EFER`. So the question
"does this CPU support SSE2" and the question "did the OS turn SSE2 on"
have different answers, from different places, with different privilege
requirements. The second one is the interesting half here -- SSE2 is
supported by every x86-64 CPU ever built, and this kernel didn't enable
it until `CR4.OSFXSR` was set (see the FP entry below). A `cpuinfo`
that collapsed the two would be strictly less informative than one that
keeps them apart.

Given the `enabled` half needs a syscall regardless, `SYS_CPU_INFO`
returns the whole `struct cpu_info` rather than only the privileged
part. The alternative -- ring 3 doing its own `CPUID` and asking the
kernel only for the control-register bits -- is architecturally tidier
but means two mechanisms and, worse, a second copy of the decoding
(the extended family/model combining rules, leaf 4's `(value - 1)`
encodings). This codebase already has that mistake on display:
`userland/bin/lspci.c` carries "its own class/subclass -> name table"
because `pci_class_name()` is kernel code, and that copy can drift.
The feature *name* table is shared instead, via `api/cpu_features.h` --
a header both sides include, deliberately kept out of `kapi.h` so the
~40 files that don't print CPU flags don't each carry a 90-entry array.

Two implementation notes worth having written down. **Cache topology
needs both vendors' leaves**: leaf 4 is the modern path, but the
default `qemu64` model reports as AuthenticAMD and populates neither
leaf 4 nor AMD's `8000001DH`, so AMD's older `80000005H`/`80000006H`
are a real fallback rather than legacy completeness -- without them the
default VM shows no caches at all. And **the clock calibration has to
happen at boot, not on first use**: it spins waiting for `pit_ticks()`
to advance, and every interrupt gate here (including `int 0x80`) clears
IF, so a lazy calibration reached through the syscall waits forever for
a tick that cannot arrive. That was found as `/bin/lscpu` hanging with
no output and no fault. `tools/vm.py --cpu MODEL` exists so the
model-dependent paths can actually be tested.

## Floating point is ring-3 only, and eager -- the same call Linux and Windows made

Asked whether SSE/SSE2/FPU could be enabled, and whether to do it
kernel-wide "if Linux and Windows have it kernel-wide." They don't.
Linux compiles its own kernel with `-mno-sse -mno-sse2 -mno-mmx
-mno-80387` (the same flags this project's `CFLAGS` already carried) and
makes kernel-side SIMD an explicitly bracketed
`kernel_fpu_begin()`/`kernel_fpu_end()` region used by a handful of
subsystems (AES-NI, RAID6); Windows requires
`KeSaveExtendedProcessorState()`/`KeRestoreExtendedProcessorState()`
around any kernel-mode FP. So here: `userland/`'s ring-3 ELFs are built
without those flags and get real hardware `double`/`float`, while
`kernel/` and `apps/` keep them.

The reason it's the right split rather than merely the conservative one:
with SSE enabled, GCC emits XMM registers in ordinary code -- struct
copies and inlined `memcpy` included, not just code that mentions a
float. An interrupt can land on any instruction, so an FP-enabled kernel
needs an FXSAVE on the interrupt path itself, on every vector. Keeping
the kernel FP-free confines state movement to where the scheduler
actually swaps ring-3 processes, which already exists
(`scheduler.c`'s `switch_to()`).

Note the toy-os-specific wrinkle, because it's a real difference from
the systems being copied: `apps/` here is ring 0, compiled into the
kernel image. So "userland only" is narrower than it sounds -- Calculator
and the WM don't get float, only `userland/`'s ELFs do. If a GUI app
ever genuinely needs it, the answer is the `kernel_fpu_begin()` bracket,
not flipping the whole kernel.

Save/restore is **eager**, not the classic lazy `CR0.TS` + `#NM` scheme.
Lazy is what CVE-2018-3665 (Lazy FP State Restore) exploited to read
another task's registers, and Linux removed its lazy path entirely in
4.14; `FXRSTOR` costs on the order of 100 cycles against a 100 Hz tick,
so the trade isn't close. A KTEST asserts `CR0.TS` stays clear so this
can't be quietly undone.

Two things found by testing rather than reasoning, both worth knowing
before touching this code. **FXSAVE does not write all 512 bytes** --
the tail from offset 464 is "available for software" and left as-is,
which is why `fpu_init_state()` copies a full template instead of
FXSAVE-ing into each new area, and why a round-trip test has to zero its
buffers first. And **a freestanding `_start` wants `RSP % 16 == 8` at
entry, not 0**: the SysV process-entry convention describes what a real
crt0 sees, and a real crt0 realigns before calling `main`, but these
`_start`s are plain C functions GCC compiles as if a return address were
pushed. Handing one a 16-aligned RSP puts every aligned local off by
eight; it took a `#GP` in ring 3 to find that. See `CHANGELOG.md`.

## Button groups commit on RELEASE, and there is no ui_button_group_click()

Both apps built on `ui_button_group` used to act from `gui_apps.h`'s
`on_click`, via a `ui_button_group_click()` that hit-tested a point and
returned that button's code. `on_click` fires on button-**DOWN** (see
`apps/wm/wm_input.c`'s dispatch, and `gui_apps.h`'s own warning), so
both Calculator's keys and Notepad's `Open...`/`Save As...` committed
the instant the mouse went down: press, drag away, release, and the
digit was still entered and the file picker still opened. Confirmed by
running that exact gesture under QMP, not by reading the code -- see
`CHANGELOG.md`.

`ui_button_group_release()` returns the released button's code now
(`-1` if none). That works because `ui_button_group_press()`
re-hit-tests every tick, so a button the cursor has left is already
unpressed and releasing there returns `-1` -- the cancel falls out of
state the group was maintaining anyway, with no "armed control" field
in either app. `ui_button_group_click()` was deleted rather than kept
alongside: it has no notion of a press to cancel, so any control
acting on it is uncancellable by construction, and it had no callers
left. The narrow act-on-contact cases `on_click` is genuinely for
(placing a text cursor, focusing a field) can bring one back if one
ever actually needs it.

Worth noting how this was found, because it generalises: it surfaced
while testing an unrelated change (hover wiring) *because the cancel
path was tested at all*. `docs/gui-guidelines.md` asks for that
explicitly -- press-drag-off-release is a separate test from
press-release, and only the second one had ever been run on these two.

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
implementation (`SYS_PCI_COUNT`/`SYS_PCI_INFO`, `userland/bin/lspci.c`,
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
all. Both calls carried unchanged into `tools/tfs3_writer.py` when
TFS3 landed -- same content-hash sync, same ~4.03 MB
direct+single-indirect cap.

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
interim mechanism became fully redundant: the writer tools'
`sync`
can format-and-seed a completely untouched `disk.img` in one call, at
BUILD time, with no boot cycle needed at all. (Since Milestone 15 the
Makefile's entry point is `tools/seed_disk.py`, which probes the
image's magic and delegates to `tfs2_writer.py` or `tfs3_writer.py`;
a blank image gets the default format, TFS3.)

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

## CI is kept for the environment, not the checks -- they duplicate `make verify` exactly

Asked directly, and worth answering once: on a solo hobby project where
`make verify` runs before every push, is a GitHub Actions workflow
earning anything?

Every step `.github/workflows/build.yml` runs -- clean build, iso,
`check_layout.py`, `boot_smoke_test.py`, `ktest_run.py` -- is a step
`make verify` already runs locally. If the value were "run the checks",
it would be pure duplication and should go.

**The value is the machine, not the checks.** CI builds from a fresh
clone on a host that isn't the maintainer's, and that difference catches
a class of bug local verification structurally cannot:

- `tools/gen_kbs.py` needs `xkbcli`, and the `seed` target skips it with
  a message when absent. CI didn't install it, so CI had been building
  images with **no keyboard layouts at all**, silently, for as long as
  that step existed. No amount of care running `make verify` on a
  machine that has `xkbcli` can find that.
- A file that exists locally but was never committed builds fine
  forever locally. `seed/sync/pci.ids` was exactly that (see the
  filesystem-layout entry). Fresh-clone builds catch that class by
  construction -- though not universally: in that specific case nothing
  in the build referenced the file, so CI would have gone green with a
  degraded image anyway. A partial net, not a complete one.
- Ubuntu vs the maintainer's Arch-family host is what the Makefile's
  `grub2-mkrescue` fallback exists for.

**What CI explicitly does NOT cover:** it has no display and no QMP, so
every GUI regression is invisible to it -- a mis-clipped label, a click
handler testing the wrong coordinate space. Those need local screenshot
testing regardless, and a green CI badge must not be read as "the
desktop is fine".

## Header dependency tracking is a `find`, and a test proves it works

The Makefile's `-include` for the `-MMD -MP` dependency files used to
name six directories by hand. When source discovery went recursive,
kernel objects moved from `build/core/` to `build/kernel/core/`, the
glob stopped matching, and **88 of 161 `.d` files silently stopped
being read** -- `touch kernel/include/kernel/process.h && make all`
rebuilt nothing at all. It is a `$(shell find $(BUILD) -name '*.d')`
now, which cannot drift when a directory moves.

Two things are worth remembering beyond the fix. First, the failure was
invisible in every way this project normally looks: nothing warns, a
clean build is unaffected, and the symptom appears later as a stale
`.o` compiled against an old struct layout -- which is not a compile
error but an array indexed with the wrong stride at runtime. This repo
has paid for that twice already (the Start menu drawing function
prologues as text; a filesystem honesty check reading garbage and
refusing a good backend). Second, that invisibility is why the fix
comes with `tools/check_deps.py` rather than standing alone: it touches
one header per build directory, asks `make -n` what it would rebuild,
and fails if the answer is "nothing". It runs in `preflight.sh` and CI,
and was validated by putting the old glob back and watching it report
exactly the ten directories that had been uncovered. A one-line fix
with no guard would have left the next directory move free to do this
again. See `CHANGELOG.md`'s "the build's header dependency tracking had
silently stopped working" entry.

## The GUI stack has names: TWP, TWS and Toykit

Three things had no names, which made every sentence about them a
description: "the windowing protocol", "the window manager acting as a
server", "the ring-3 widgets". They are now:

| Name | What | Where | Analogy |
|---|---|---|---|
| **TWP** -- Toy Window Protocol | the client<->server message contract | `kernel/include/abi/win_proto.h` | Wayland, the X11 protocol |
| **TWS** -- Toy Window Server | the compositor implementing it | `kernel/proc/win_server.c` + `apps/wm/wm_client.c` | Mutter, Weston, Xorg |
| **Toykit** | the client toolkit an app programs against | `userland/ui/` | GTK, Qt, Win32 |

**Why three and not one.** The protocol is deliberately meant to outlive
this particular server -- M41 moves TWS to ring 3, and the whole bet in
`win_proto.h` is that this is a transport swap rather than a rewrite.
Naming the protocol separately is what makes that sentence sayable, and
what makes "TWP v2" a thing you could version. One name for all three
would blur exactly the line the design leans on.

**TWP and TWS follow TFS2/TFS3's style** -- short, plain, project-initial
-- because they are the same kind of thing: a format or service with a
contract worth versioning. **Toykit deliberately breaks that pattern**,
because the plain version would be "TUI", which universally means *text*
user interface and would mislead every reader arriving without context.
It is also the name said out loud most often, which is worth a real word.

**The symbol prefixes do NOT change.** Toykit's are `uui_`, `ugfx_`,
`uapp_`; TWP's are `WIN_REQ_*`/`WIN_EV_*`. A toolkit's name and its
prefix need not match -- GNOME's toolkit is GTK -- and renaming several
hundred symbols to spell a name out would be churn with no reader
benefit. The names are for docs, comments and conversation, which is
where the ambiguity actually was.

## Ring-3 GUI apps are callbacks and a layout, not a loop

Every TWP client used to hand-write the same three things: the
create/title/present/destroy handshake, a `for(;;)` around a
`switch (ev.type)`, and a `draw(); present();` pair at every state
change -- about 55 of `winclient.c`'s 141 lines before it did anything
of its own. Then it computed every rectangle by hand as well.

Toykit's `uapp` (`userland/ui/uapp.h`) owns the loop; `uui_layout`
(`userland/ui/uui_layout.h`) owns the geometry. An app is a
`struct uapp_desc` and some callbacks.

**The property that justifies it, and the one to preserve:** every
callback is optional and the library has a defined default for every
event, so TWS can gain a feature without any app being edited. That was
tested rather than asserted -- shipping resize (stage 3) touched ZERO
lines in the clients that did not opt in, and they all kept passing. An
unknown event type is ignored on purpose.

Four pieces of API were written and deleted before landing, each for
having no caller: `gfx_text_index_at_x()`, `uapp_text()`, the
`uapp_open()`/`uapp_pump()` escape hatch, and `WIN_REQ_MOVE`. The hatch
is the instructive one -- the design predicted for three stages that
Terminal would need it because Terminal BLOCKS inside a command, and
porting it showed the requirement was to PAINT at a chosen moment
(`uapp_flush()`, one line), not to own the loop. "This app blocks" and
"this app needs the loop" are not the same requirement.

Full design and staging: `docs/uapp-design.md`.

## Userland programs link one archive, and name nothing

Each ring-3 GUI client used to need a hand-written `FOO_OBJS = ...`
block plus its own link rule in the Makefile -- four of them, ~50 lines,
identical apart from the object list. That became one
`EXTRA_OBJS_<name>` line per binary, and then, once the toolkit was
about to be split one-file-per-widget, an archive: `libuapp.a` holds
`userland/ui/`, `userland/lib/` and the sources shared with the kernel,
every program links it, and **a program names nothing at all** -- the
linker pulls only the members it references. A new GUI app is a `.c`
file in `userland/gui/` and no Makefile edit.

The archive was deliberately deferred at design time and then brought
forward, which is the rule working rather than a reversal: the bar is a
second real caller, and splitting `uwidgets.c` into ten per-widget
objects was what created one. Without an archive that split would have
turned each app's object list from three entries into eight.

Three mechanics that are load-bearing together, and useless apart:

- `-ffunction-sections -fdata-sections` plus `--gc-sections`. Archive
  granularity alone still links the whole of a member: one checkbox
  would pull in the listbox, dropdown and text field sharing its `.c`.
- **`userland/rt/link.ld` must match `.text.*`, not just `.text`.** With
  function-sections on, a script matching only `*(.text)` places none of
  the code. The link SUCCEEDS and the ELF is nearly empty; the symptom
  is a fault at the entry point, not a linker error.
- **The archive goes last on the link line.** A linker resolves archive
  members against the undefined symbols accumulated so far, so an
  archive ahead of its callers contributes nothing.

Measured on the 29 userland binaries: 171 KB smaller in total, Terminal
-32%, Calculator -27%, `uiclient` -41%, and `hello` gained nothing (it
references no toolkit symbol, so it pulls no member). See
`CHANGELOG.md`'s "userland programs link one archive" entry.

**A second trap, found when the first source file was deleted:** `ar
rcs` UPDATES an archive rather than rebuilding it, so a member whose
source has gone stays inside indefinitely. Splitting `uwidgets.c` into
per-widget files left `uwidgets.o` in `libuapp.a` for three commits, and
nothing failed -- a linker pulls the first member that satisfies a
symbol and only errors when two ALREADY-pulled members collide, so the
link kept working while being free to use the deleted file's code. It
surfaced only when `ui_checkbox` became an object and the two versions
of `uui_checkbox_draw` finally differed. The rule `rm -f $@` first, so
the archive is a function of its declared objects rather than of every
object that has ever existed. Note `make clean` hides this class of bug
rather than revealing it: a full `preflight.sh` would have built a
correct archive and said nothing, which is why `--skip-clean` runs are
worth keeping in the loop.

**The trap this exposed, which is general:** the Makefile tracks HEADER
dependencies (`-MMD`/`-MP`), not compiler FLAGS. Adding
`-ffunction-sections` to `USERLAND_CFLAGS` invalidated nothing, so the
first build linked stale objects compiled without it -- and the result
looked like `--gc-sections` half-working (binaries shrank, because
unreferenced archive members were still skipped, but unused functions
inside a pulled member survived). `make clean` after a CFLAGS change,
and be suspicious of a flag that appears to work partially.

## Parallel test VMs lease a slot, they don't derive one from their position

`tools/gui_regress.py` runs the GUI test tools concurrently, each
against its own VM. Everything that could collide -- pidfile, serial
socket, QMP port, VNC display -- is derived from one slot number
(`vm.py --instance N`), and slot 0 keeps the original unsuffixed names
so every existing caller is unaffected.

The subtle part is how a tool GETS its slot. Deriving it from the
tool's index in the list (`idx % jobs`) looks equivalent to leasing one
and isn't: with `-j4` and seven tools, task 4 also maps to slot 0, but
it starts as soon as any worker frees up, which is routinely while task
0 is still running there. Written that way first, and the symptom was
actively misleading -- the fifth tool's slot-0 `vm.py stop` killed the
FIRST tool's VM, so the first tool died on a broken pipe and the fifth
died on a screenshot that was never written, with neither traceback
pointing anywhere near the scheduling. Slots come from a
`queue.Queue` lease held for exactly as long as the VM exists.

Ports are derived, not probed for. A free-port probe has a bind/close
race and gives a different port every run, which makes re-driving a
failed tool by hand harder than it needs to be; `-j4` always means
slots 0-3.

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
every backend implements) or a silent fallback to
non-stepped behavior a caller couldn't easily detect it got.
**Updated at Milestone 15:** `fs_ops` DOES carry a capability
bitmask and one optional op now (`link()`, gated by
FS_CAP_HARDLINKS, with the honesty check refusing a backend whose
bit and pointer disagree) -- but the required-not-optional call made
here still stands for the range/steppable ops: both backends (tfs2
and tfs3) implement them, and there's still no scenario where a
backend legitimately can't. If a future backend
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

## The kernel context is a rotation participant, not a kernel thread

Making `wm_run()` keep drawing while a ring-3 process runs sounds like
it needs a kernel thread -- a stack, a context, a slot in the process
table. It doesn't, and deliberately didn't get one.

The kernel context already had everything a scheduler entity needs
except a turn. It needs no address space of its own (kernel code is
correct under any process's CR3, since every PML4 shares kernel entry
0), no FP state (kernel and `apps/` are built `-mno-sse`), and no
kernel stack of its own (ring 0 interrupting ring 0 doesn't switch
stacks, so its trapframe lands on whatever stack it was already using).
`scheduler.c` was already saving its trapframe pointer in
`kernel_saved_rsp`. So it takes a POSITION in the round-robin cycle
(`ROT_KERNEL`, `rotation_pos`) rather than a `struct sched_process`,
and `current_index` keeps meaning exactly what it always did -- -1 when
the kernel is running -- because `syscall.c` depends on that through
`scheduler_current_pid()`.

The one deliberate exception is worth knowing before touching this: the
legacy blocking path (`process_run_ring3()`) runs a ring-3 process
WITHOUT a scheduler slot, so from `scheduler.c`'s point of view that
process's trapframe *is* the kernel context. Rotating away from it and
back would resume it under whatever CR3 and RSP0 the scheduler process
left behind. `kernel_slot_runnable()` therefore drops the kernel
position out of the rotation entirely while one is in flight, keyed on
the pre-existing `process_context_is_armed()` rather than a second flag
that could drift out of agreement with it.

See `CHANGELOG.md`'s `[Unreleased]` entry for the full writeup and how
both directions were verified, `scheduler.c`'s `ROT_KERNEL` comment for
the design, and `docs/roadmap.md`'s Milestone 41 for what this unblocks.

## Blocking syscalls deschedule; they never wait in place

The obvious way to write a blocking syscall here -- `sti`, then spin or
`hlt` inside the handler until the awaited thing arrives -- is not
merely slow in this kernel, it is broken, and it was tried before being
ruled out. It worked for exactly one keystroke and then hung.
`g_next_kernel_rsp` (`idt.c`) is a single global "where to resume"
pointer: correct for the scheduler's own use, never meant to be
reentrant. A nested IRQ handler overwrites it while the outer
`int 0x80` handler is still on the stack, so that outer handler's
epilogue resumes into a stale frame. `syscall.c`'s `SYS_READ_KEY`
comment is the original autopsy, and is why that syscall is
non-blocking by hard requirement rather than by preference.

`scheduler_block_current()` (`kernel/proc/scheduler.c`) sidesteps the
problem instead of trying to make the global reentrant. The handler
does not wait -- it RETURNS, through the ordinary `isr_common`
epilogue, into a different entity, which is exactly the switch
`scheduler_tick()` and `scheduler_on_exit()` already perform with
already-proven machinery. Nothing nests, and interrupts stay off for
the whole handler as they always were.

`scheduler_wake()` is deliberately limited so it is safe to call from
an interrupt handler: it only flips scheduler state and writes an
already-saved trapframe, and never touches `g_next_kernel_rsp`. A woken
process becomes eligible and runs at the next ordinary tick. An IRQ
handler that tried to switch directly to the woken process would be
re-creating exactly the reentrancy this design exists to avoid.

Full writeup in `CHANGELOG.md`'s `[Unreleased]` entry.

## `SYS_WAIT_EVENT` makes clients loop instead of restarting the syscall

`SYS_WAIT_EVENT` returns 0 meaning "you were woken, ask again", so every
client wraps it in `while (sys_wait_event(&ev) != 1) { }`. That looks
like a missing feature and isn't.

The wake happens inside an interrupt handler, running under whatever
address space happened to be current -- which is not necessarily the
waiting process's. So the kernel physically cannot copy the event into
that process's buffer at wake time; the copy has to happen back inside
the client's own syscall, which means the client has to re-enter it.
This is the same spurious-wakeup contract a condition variable has, and
the loop does not spin the CPU: each pass that finds nothing parks the
process again, using no timeslices.

The alternative, which Linux uses, is to rewind RIP over the trapping
instruction so the syscall restarts itself (`ERESTARTSYS`). Rejected
here: it buries a hard assumption about the syscall instruction's
length inside the scheduler, and would have to be revisited if the
syscall entry ever moved off `int 0x80`. The explicit loop costs a
client three lines and hides nothing.

## The windowing protocol is one syscall carrying typed messages, not a syscall per operation

Every windowing operation a ring-3 client can perform -- create,
present, destroy, retitle -- goes through the single `SYS_WIN_REQUEST`,
which dispatches on the `type` field of a `struct win_request_msg`
(`kernel/include/abi/win_proto.h`). The obvious alternative, a syscall
each, was rejected deliberately.

The reason is Milestone 41's whole architectural bet. toy-os is
building toward a GUI where apps are ring-3 processes, and the open
question was whether the window manager itself should move to ring 3
too (the Linux answer) or stay in the kernel (the Windows NT answer).
The chosen path is "kernel compositor now, movable later", and what
makes "later" cheap is that clients and the window server only ever
talk in messages -- never by calling into each other. With a message
protocol, moving the server out is a transport swap and the message
handling is untouched. With one syscall per operation, the syscall
signature IS the protocol, and moving the server means rewriting every
call site.

The same reasoning shapes two smaller choices. Neither
`struct win_request_msg` nor `struct win_event` contains a pointer, and
both have fixed layouts, so the identical bytes work whether copied by
a syscall today or read out of a shared-memory ring later. And a
client's buffer address is DERIVED from its window id
(`win_buffer_vaddr()`) rather than returned by the server, so there is
no address to re-negotiate when the transport changes.

The kernel/WM split follows the same line: `kernel/proc/win_server.c`
owns the memory (ids, buffers, mappings, teardown -- things `apps/`
cannot reach, since `kernel/include/kernel` is off its include path),
`apps/wm/wm_client.c` owns presentation (window list, chrome, z-order,
input routing), and they meet at a registered `struct win_server_ops`
-- the same registry pattern as `display.h`'s `display_driver`.

## A hover test parks the REAL cursor, and must un-park it afterwards

`menubar_test.py` failed one check intermittently for three sessions --
roughly one full-suite run in three, always
`Recent files is greyed until a save, then opens a real submenu`, and
always passing on re-run. Diagnosed 2026-08-16. Two halves, and the
second is the part that was not previously written down.

**The cause was the documented `gui move` trap, in the last tool that
had not adopted the fix.** An injected move overrides the mouse for ONE
WM iteration; the submenu opened on that iteration and closed again when
the real pointer took over, so reading the layout afterwards was a race.
`dialog_test.py` and `uidemo_test.py` already used
`DebugConsole.warp_cursor()` for exactly this and said so in their
docstrings; `menubar_test.py` was the holdout.

Two steps made it a diagnosis rather than a guess, and they generalise:

- **Get a rate under both conditions.** Five runs of the tool ALONE
  passed; two of four full parallel runs failed. That alone ruled out a
  widget bug and pointed at timing.
- **Design a probe whose outcomes differ under each hypothesis.** The
  first two theories -- the disk write is slow, the recent-list update
  lags -- were both wrong, and a probe settled it: the saved file was on
  disk in 0.00s, and a SECOND identical hover opened the submenu. Item
  enabled, hover lost. That is the direct evidence the whole diagnosis
  rests on, and it came from instrumenting a real failure rather than
  from reasoning about the code.

**The evidence for the fix, stated as numbers.** Before: two of four
full-suite runs failed. After: **eight of eight passed**, plus three of
three with the tool alone. At the observed pre-fix rate those eight
consecutive passes are about a 0.4% coincidence, which is the point at
which this stops being "it seems better".

**What does NOT work, recorded because it would otherwise be
rediscovered.** Shortening the check's post-save wait to 0.05s looked
like an on-demand reproducer (it failed 1 of 2, then 1 of 4) and is not:
with the fix reverted AND that amplifier in place, four more runs
passed. Those early failures were luck, not a trigger. So the amplifier
is worthless as a control, and the mechanism evidence above -- an
instrumented real failure -- is what the diagnosis actually rests on.

**The new lesson is the un-park.** A parked real cursor is the point of
`warp_cursor()` and a hazard everywhere after it: it STAYS there, so a
menu opened later finds the pointer already inside it and can close or
expand on its own. Applying the fix to both submenu hovers turned a
different check red 5/5 -- and its partner, "a click outside dismisses
the menu", stayed GREEN, because the menu really was closed; it had
never opened. That is the vacuous-pass shape this repo keeps meeting: an
absence check satisfied for the wrong reason. So a tool that parks the
cursor moves it back to neutral ground when the measurement is done
(`unpark()`), and the pair reads as one idiom rather than two unrelated
calls.

## Rubber-band selection is shared source compiled twice, and it owns the behaviour

Drag a rectangle on the desktop and it selects the icons it touches,
highlighting them live as it sweeps and un-highlighting them when pulled
back -- Windows' and KDE's behaviour. Two decisions made it worth its
own entry.

**Where it lives: `kernel/lib/rubberband.c`, compiled TWICE.** The
desktop is kernel-side today and a file manager will be ring-3, so the
obvious options were "build it in `apps/ui/` and port it later" or
"build it in `userland/ui/` and the desktop waits". Both produce two
implementations that drift, which this project has already paid for
three times (the `kpath` copies that disagreed about `../x`, the three
hand-copied scrolling implementations whose third shipped a dead
scrollbar, the ring-3 Notepad's scrollbar grab). So it takes the path
`kernel/lib/geom.c` and `apps/calc_engine.c` already take: one source
file, built once into the kernel and once into `libuapp.a`. The Makefile
rule already existed; the only cost is that the file must stay
freestanding (`<stdint.h>` only, no allocator, no drawing).

**What it owns: the behaviour, not the items.** The caller answers "how
many?" and "where is item i?" through a `struct rb_ops` and does its own
drawing; the module owns the band, the selection set, the modifier
semantics and the click-versus-drag threshold. That is
`docs/gui-guidelines.md`'s "behaviour belongs to the component" rule,
and the payoff is concrete: every rule is a KTEST against a grid of
made-up rectangles, with no compositor, no cursor and no pixels.

Three rules in there that a from-scratch implementation tends to get
wrong, each with its own test:

- **A shrinking band deselects.** The selection is recomputed from the
  selection-at-drag-start on every motion, never accumulated. An
  accumulating version is indistinguishable while the band grows and
  wrong the moment it shrinks.
- **A band dragged up-and-left is an ordinary band.** The rect is
  normalised; a version that forgets selects nothing in that direction
  and passes every other test.
- **A click is not a zero-size band.** Below a small threshold a press is
  a click, which in replace mode clears the selection ("click empty
  space to deselect") and falls out of the same rule rather than being a
  special case. Without the threshold every click is a degenerate drag.

Two integration notes. Modifiers come from `keyboard_mods_now()`, added
for this: a click carries no modifier state of its own, and it is
deliberately a LIVE sample rather than a latched one -- which is exactly
why keyboard input must NOT use it (a key event carries the modifiers
held when the key was pressed, so a modifier released a moment later
cannot retroactively change an already-typed character). And the band
counts as a drag for the entry-reload guard, because its selection is a
set of registry indices that a reload would renumber.

Group DRAGGING -- moving every selected item together -- is deliberately
not built yet. This is the layer it would sit on.

## One desktop-entry directory with a `ShowIn` key, not a second directory per surface

`/usr/wm/desktop/` feeds BOTH the desktop icons and the Start menu. Asked
for a separate `/usr/wm/startmenu/` so an app could appear in one place
and not the other, the answer is a KEY on the existing entry instead:
`ShowIn=desktop startmenu`, defaulting to both.

The reason is duplication. Two directories means any app wanted in both
places has its file copied into both, and the copies drift -- rename the
app or change its `Exec=` and only one surface updates, silently. That is
the same failure this repo has already paid for with the `kpath` copies
that disagreed about `../x` and the three hand-copied scrolling
implementations whose third copy shipped a dead scrollbar.
freedesktop.org hit the identical question and answered it with
`OnlyShowIn`/`NotShowIn` rather than a second directory.

`NoDisplay=1` is kept and still means NEITHER -- a different statement
("this is not a launchable thing") from `ShowIn` ("it is, but only over
there").

**The parser deliberately breaks this project's usual rule.** Everywhere
else here a parser REJECTS rather than guesses; a `ShowIn` naming nothing
recognisable falls back to showing on both surfaces, with a log line. The
usual rule assumes the outcomes are "a value" or "an error", and here
they are not symmetric: hiding an app because its key was misspelled
makes it vanish with no visible cause, and an unreachable app reads as a
broken system (the desktop has already shipped that bug once, when icons
wrapped off the bottom of the screen). Showing it in one place too many,
loudly, is recoverable.

**The implementation trap, which is where the real bug would have been.**
The Start menu's rows are POSITIONAL: it draws row i from a list and
hit-tests by dividing the click's y by the row height. Filtering the draw
while leaving the hit-test on the unfiltered registry lands every click
on the wrong app and looks perfectly correct in a screenshot. So both go
through one accessor pair -- `gui_app_visible_count()` /
`gui_app_visible_at()` -- which makes the disagreement unrepresentable
rather than merely avoided. The desktop keeps registry indexing instead
(its icon positions are persisted by NAME in `/etc/desktop.conf`, so a
reload must not renumber them) and skips hidden entries in place.

## Desktop entries reload live off a filesystem generation counter, not a directory poll

Dropping a `.desktop` file in now updates the desktop and Start menu
without a restart, the way KDE and Explorer watch their desktop folders.
There is no inotify here, so the question was what "watch" means.

The obvious answer -- re-list the directory every few seconds -- was
rejected on cost: it means a real disk read every few seconds forever on
a completely idle machine. That is exactly the class of always-on
background cost this project keeps out.

Instead `fs_generation()` (`api/fs.h`): one counter the VFS bumps on
every successful mutation. The WM compares it each frame, which is an
integer compare and no I/O, and re-reads the directory only when it has
moved. Idle cost is nothing; latency when something does change is one
frame plus a ~500ms debounce.

Three things about it worth keeping:

- **It is global, not per-path, on purpose.** A watcher wakes for changes
  it does not care about and pays one small directory read for the false
  positive. Per-path watches would need a registry, a lifetime and an
  eviction policy to save a read that only happens when something already
  changed.
- **The streamed write path bumps once, at `FS_STEP_DONE`.** A save is
  one change however many slices it took, and this is the path Notepad
  saves through -- without it, editing a file in the editor would be
  invisible to anything watching.
- **The reload DEFERS while anything holds a reference into the entry
  list** -- an open Start menu (positional rows), an icon mid-drag (an
  index), or an open KERNEL-SPACE app window. The last is the one worth
  knowing about: `struct window::app` is a pointer straight into
  `gui_app_registry[]`, and `gui_apps_load()` rewrites and re-sorts that
  array in place, so reloading underneath an open window would silently
  rebind it to whatever entry landed in its slot -- its callbacks would
  belong to a different app. A ring-3 client's window holds no such
  pointer (`wm_client.c` sets it to 0), so it does not defer, and the
  case disappears with the last builtin in stage 4. Deferring costs
  nothing: the generation stays changed, so it fires the moment the
  condition clears, and the test asserts both halves -- deferred while
  open, delivered on close -- because "it did not appear" alone is
  equally satisfied by a reload that stopped working.

## `append` zeroed the block it appended into, and `write`/`append` now write LINES

Two bugs found by trying to create a `.desktop` file from the shell, one
of them serious.

**TFS3 destroyed data on every append.** `do_write_inner()`'s
partial-block path decides whether to read a block before modifying it,
and asked whether the WRITE OFFSET was at or past end-of-file. An append
starts exactly at `node->size` by definition, so that test was true every
single time and the whole block was zeroed -- wiping the bytes already in
it. `write f AAAA` then `append f BBBB` left four NULs followed by BBBB
on disk. The right question is whether the BLOCK begins past EOF, not
where this particular write starts; a partial block is a read-modify-
write, and the read is skippable only when the block is freshly allocated
or lies wholly beyond the file.

The regression tests have to use a fixture SMALLER than a block. An
append that happens to land on a block boundary takes the fresh-block
path and is correct either way, so a test written with block-aligned data
passes against the bug -- this repo's recurring "the data never crossed
the branch" trap, and the reason all three new KTESTs go red against the
old line while a size-only assertion would not (the file was the right
length; it was full of NULs).

**And neither `write` nor `append` terminated its line**, so `write f a`
followed by `append f b` produced `ab`. That made a multi-line file
impossible to author from the shell at all -- which meant every
line-based format this system has (`/etc/toyos.conf`, `.desktop` entries)
could be READ by the shell and never WRITTEN by it. Both commands write
one terminated line now, and refuse rather than truncate a line that does
not fit, matching kfmt's rule that a value which does not fit is written
not at all rather than wrongly.

## Claiming the compositor role is a message, and it is the one request that works with no window server

Milestone 41's stage 1 landed the compositor registration --
`win_server_set_compositor()`, the access-control idiom every mapping
call copies, and revocation of every mapping when the holder changes --
and nothing outside a KTEST could reach any of it. There was no syscall
and no `WIN_REQ_*` that got there. Stage 2's first job was filling that
gap, and the fork was whether to fill it with a message
(`WIN_REQ_SET_COMPOSITOR = 9`) or a syscall (`SYS_*  = 30`).

The message, for the reason the whole protocol exists: TWP's bet is that
an operation is a typed message on one transport, so moving the server
to ring 3 is a transport swap rather than a rewrite of every call site.
A syscall for the one operation a ring-3 window server needs most would
have been the exact shape the protocol was designed to avoid.

The argument against it was real, though, and worth recording because it
looked fatal at first: every other request is refused outright when no
presentation layer is registered, in TWO places -- `syscall.c`'s
`win_server_active()` gate and `win_server_request()`'s own `!g_ops`
guard. A compositor could therefore never register before the WM did.
That is harmless in stages 2 and 3, where the ring-0 WM is always
registered, and fatal in stage 4, where the ring-3 WM *is* the
compositor and there is no kernel-side presentation layer left to
register first. The registration would have become unreachable at
precisely the point of the milestone.

The fix is small and is the reason the objection did not decide
anything: handle `SET_COMPOSITOR` ABOVE the `!g_ops` guard, and make the
syscall's gate typed (copy the request in first, then apply the gate to
every type except this one). Six lines, and the exception is documented
at all three sites because it is the kind of thing a later edit
re-tightens without noticing.

Two rules came with it. Claiming REPLACES the previous holder -- last
claimant wins, the same non-arbitration `display_register()` already
uses -- and revokes every mapping the old one held. But RELEASING is
only the holder's to do: without that check any process could evict the
compositor and take the raw input stream and every buffer mapping down
with it, a denial of service needing no privilege at all. Both are
KTESTs in `kernel/proc/win_server_test.c`'s `winshare` suite, and the
third one there asserts the no-presentation-layer case directly, with
`WIN_REQ_PRESENT` returning -1 in the same breath as its control.

## Raw input to the compositor is level state, tapped inside the WM loop rather than at the driver

Stage 2 delivers the input stream a compositor needs -- the one
`wm_input.c` consumes, before focus and hit-testing. Two things about
how were decided rather than defaulted.

**It is LEVEL STATE, not synthesised edges.** There is no unified input
event anywhere in this kernel to reuse. `mouse_get_state()` returns an
absolute clamped position plus a button bitmask, and today's WM derives
presses and releases by diffing against its own previous sample; the
wheel is a read-and-reset accumulator; only the keyboard is a real
queue. The only event struct in the tree, `struct win_event`, is
*post*-routing and per-client. So the choice was to synthesise edges in
the kernel or hand the compositor the same level state and let it diff.
The second, because it is what the WM already does -- one differ instead
of two, and an event that stays honest about what the hardware actually
reports. `WIN_EV_RAW_MOUSE` carries screen coordinates and the button
bitmask; `WIN_EV_RAW_KEY` and `WIN_EV_RAW_WHEEL` are separate types
because the keyboard and the wheel are separate mechanisms, not fields
of the pointer's state.

**The tap goes inside `wm.c`'s loop, not at the driver.** This is the
finding that would have cost a session otherwise. `gui click` and
`gui key` inject through `wm_debug.c` and are applied AFTER the real
driver read -- the mouse is overridden for one iteration, and an
injected key is used only when the real keyboard returned -1 so a human
is never pre-empted. A tap on `mouse_get_state()` would therefore be
invisible to every synthetic event, i.e. invisible to all 13 GUI test
tools, which are the only proof any of this works. Tapping after the
override is what makes stage 2 testable at all.

**And it is gated on CHANGE.** The WM loop runs on every timer tick and
the event queue is 32 deep dropping the oldest, so pushing level state
unconditionally floods it while the user sits still. The positive
control for this is worth repeating rather than re-deriving: removing
the gate reddens exactly one check in `tools/compositor_test.py` ("idle
produces no mouse events", 12 lines in one idle second against 0). The
neighbouring `dropped == 0` check stayed GREEN under that control,
because a client blocked in `sys_wait_event()` drains 12 events/second
without effort -- so that check catches a compositor falling BEHIND, not
a flood, and should not be read as covering this.

Both paths run at once, which is the stage's whole shape: stage 4
deletes the WM's own routing and keeps this, so the flip is a deletion
rather than a cutover. `compositor_test.py` asserts every injected input
TWICE -- once in the compositor's log and once in UI Demo's -- because
"the compositor received the click" is equally satisfied by an
implementation that stole the stream outright, which would be a
regression wearing a feature's clothes.

## A client window's close button is a handshake, not a seizure

Clicking the X on a ring-3 client's window does not close it. The
window manager sends `WIN_EV_CLOSE` and waits for the client to answer
with `WIN_REQ_DESTROY`.

Two reasons. The client may have unsaved state and is the only thing
that knows it. And the WM tearing the window down behind the process's
back would leave that process drawing into a buffer that is no longer
on screen -- with the frames behind it freed and possibly reissued to
something else.

The honest consequence is that a client which ignores the request keeps
its window. Force-closing an unresponsive one needs a "not responding"
timeout and a way to kill a process, neither of which exists yet -- see
`docs/roadmap.md`'s Milestone 41. Every real windowing system has the
same handshake and the same escape hatch; toy-os has the handshake so
far.

## Ring-3 clients draw for themselves, and the font is shared read-only

Two decisions that go together, both in `userland/ui/ugfx.c`.

**No drawing syscalls.** A client renders into its own window buffer
with plain arithmetic -- there is no "draw text" or "fill rect" syscall,
and the drawing path crosses into the kernel exactly zero times. The
client only calls `WIN_REQ_PRESENT` when it has finished a frame. The
alternative would have put every client's rendering back inside the
kernel, which is what Milestone 41 is moving away from; drawing is not
a privileged operation, only the framebuffer is.

**The font is mapped, not copied.** `WIN_REQ_FONT` maps the kernel's
baked glyph tables (`kernel/drivers/font_ttf.c`) read-only into the
client. Those tables are ~11,800 lines and are ordinary kernel
`.rodata`, which this kernel already identity-maps, so sharing them is
just pointing more PTEs at the same frames -- no copy, one instance in
memory however many clients ask.

The size saving is the lesser reason. The real one is DRIFT: link a
copy of the font into each binary and a client keeps rendering at the
old size after the desktop's `font_size` setting changes, so client
text and desktop text quietly disagree. Sharing the kernel's own data
makes them identical by construction.

Read-only is load-bearing rather than tidiness -- these are pages of
the kernel image, and a writable mapping would let any client scribble
on kernel `.rodata`. `vmm_map_user_page_flags(..., writable=0,
executable=0)` is what enforces it.

Two details in the ABI exist because getting them wrong renders
convincing-looking garbage rather than failing: the glyph data does not
start on a page boundary, so the request returns glyph 0's offset
within the mapping; and the tables are coverage maps, not bitmasks, so
`ugfx` alpha-blends per pixel (a `> 128` threshold would render the
same letters visibly jagged).

The boundary this stops at: `ugfx` is a drawing runtime, not a widget
toolkit. The widgets Calculator needs were ported separately into
`userland/ui/uui.c` -- see the next entry.

## Calculator's engine is shared source compiled twice, not copied

`userland/gui/calculator.c` is a port of `apps/calculator.c`, but
`apps/calc_engine.c` is NOT ported. The same file is compiled a second
time with `USERLAND_CFLAGS` (Makefile, `build/userland/shared/`) and
linked into the ring-3 binary. `kernel/lib/string.c` and `knum.c` ride
the same path.

Why a second compile rather than reusing the object: the kernel builds
with `-mcmodel=kernel` and a ring-3 ELF with `-mcmodel=large`, linking
at `VMM_USER_BASE`. The objects are not interchangeable, so rebuilding
is the only way to share the SOURCE — and sharing the source is the
whole point. A bug fixed in the engine fixes both copies of the app,
because there is only one engine. Two hand-synced copies of arithmetic
would have been the worst possible outcome of this migration.

The rule for putting a file on that path: it must be freestanding.
`calc_engine.c` needs only `string.h` and `knum.h`, which need only
`<stddef.h>`/`<stdint.h>`. A file that reaches for kernel state does
not qualify, and the `-Iapps` that lets `calculator.c` see
`calc_engine.h` is scoped to that one object with a target-specific
variable so no other userland program gains the ability to include
`apps/` headers.

What was deliberately NOT shared: the presentation layer.
`ui_button_group` became `uui_button_group` (`userland/ui/uui.c`), because
the kernel version draws through `gfx_*` straight to the framebuffer
and takes its events as WM callbacks — neither of which exists in ring
3. That is a genuine port, and its behaviour (commit-on-release,
luminance-derived hover direction) was carried over deliberately rather
than reinvented; see `uui.h`.

The kernel-space Calculator is still there on purpose. Keeping both is
what made the migration verifiable — the two were compared side by
side, and the shared engine means they cannot disagree about
arithmetic. Retiring the old one is a separate decision
(`docs/roadmap.md`, Milestone 41).

## Ring 3 gets the C names; the kernel keeps `k_`

`userland/lib/string.h` and `userland/lib/stdio.h` declare `strlen`,
`memcpy`, `snprintf` and friends — but there is no second
implementation. Every one of them is the toolkit's `k_*` function, and
`kernel/lib/string.c`/`knum.c`/`kfmt.c` are compiled a second time into
`build/userland/shared/` for `libuapp.a`, the same shared-source rule
[Calculator's engine](#calculators-engine-is-shared-source-compiled-twice-not-copied)
uses.

Why two vocabularies for one set of functions. The kernel's reason for
avoiding the C names is in `api/string.h`: GCC knows what `strlen` means
and recognising a hand-written one can produce surprising code in a
freestanding build. Ring 3 has the opposite need — GCC may EMIT calls to
`memcpy`/`memset`/`memmove`/`memcmp` on its own, for a large struct
assignment or an array initialiser, and those calls need real symbols
under exactly those names. Nothing in the tree provided them, which was
a latent link failure rather than a bug anyone had hit. So those four
are real functions (`userland/lib/cmem.c`) and everything else is a
`static inline` wrapper — that split is the rule, not a per-function
judgment.

Three things that bit while building it, each now recorded where it
bites rather than only here. `userland/lib/string.h` cannot include
`"string.h"`, because a quoted include searches the including file's own
directory first and that resolves to itself; the guard makes it a silent
no-op and every `k_*` is then undeclared. It uses `<string.h>`, which
skips the current directory. The implementation file cannot be called
`string.c`, because `ar` stores members by BASENAME and `libuapp.a`
already contains `shared/string.o` — two same-named members in one
archive, which linked silently only because they happened to define
disjoint symbols. And `USERLAND_CFLAGS` carries
`-fno-tree-loop-distribute-patterns`: without it GCC may rewrite
`k_memcpy`'s own copy loop into a `memcpy` call, making `memcpy()` call
`k_memcpy()` call `memcpy()` forever. That LINKS, and fails at runtime
as a stack overflow with no obvious cause. Before a `memcpy` symbol
existed the same rewrite was a loud undefined reference, which is why
the kernel needs no such flag — the asymmetry is deliberate.

Getting `snprintf` there also forced `kfmt.c` apart:
`vga_printf()`/`klog_printf()` needed `vga.h`/`klog.h` and so
disqualified the whole file from the shared path. They live in
`kernel/lib/kfmt_print.c` now. One header still declares all four; the
split is about what each half may INCLUDE. A new conversion goes in
`kfmt.c`, a new sink in `kfmt_print.c`, and a single kernel include in
the former takes `snprintf` away from userland with no other symptom.

What this deliberately is not: a libc. No `malloc`, no `FILE`, no
`printf`, no `errno`, no TLS — those are Milestone 24 and each has real
design in it. See `CHANGELOG.md`'s `[Unreleased]` entry, including the
honest size cost (`lscpu` +610 bytes of text, `lspci` +1042, because the
shared converters are more general than the hand-rolled loops they
replaced) and why a full libc turned out NOT to be a prerequisite for
moving the display server to ring 3.

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

## The entropy source stops short of a CSPRNG, on purpose

`kernel/lib/krandom.c` is RDSEED/RDRAND when the CPU has them, TSC
jitter when it doesn't, and a mixing function over whichever it got. It
is explicitly NOT a CSPRNG -- no entropy accounting, no reseed
schedule, no backtracking resistance -- and `krandom_quality()` exists
so a caller can find that out rather than assume otherwise.

The alternative considered was the Linux shape: an entropy pool fed by
interrupt/keyboard/mouse timing, seeding a ChaCha20 DRBG. Rejected for
this kernel because it would mean shipping and maintaining a stream
cipher, plus event hooks across several drivers, to dress up an input
whose actual quality is set by the machine underneath -- and under TCG
that input is timing measured against a software timestamp counter.
Entropy accounting on top of that would be a number that looks like a
guarantee and isn't, which is the failure mode this repo's testing
notes complain about most. Two honest labels beat one dishonest pool.

What was measured rather than assumed: three separate boots on the
jitter path produced three different values, so the fallback is not
deterministic under emulation. That is the claim it needed to survive;
it is not a claim about cryptographic strength. See `CHANGELOG.md`'s
`[Unreleased]` entry.

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
  for the kernel, `userland/rt/stack_chk.c` for userland -- two separate
  symbols, two separate address spaces, no reason to share one).
  Building real TLS infrastructure just to use GCC's default guard
  would have been wildly disproportionate to what this milestone item
  actually needed.
- **The guard starts as a fixed compile-time constant and is REPLACED
  with a random one at boot.** It was constant-only at first, because
  this kernel had no entropy source at all; `krandom.h` exists now
  (see the entry below), and `stack_guard_randomize()` installs a
  random guard from it early in `kernel_main()`. The constant still
  protects everything before that point, so it is a fallback rather
  than a placeholder -- and it is KEPT, with a log line, if krandom
  reports no entropy, since a guard "randomized" from nothing is no
  stronger and only looks handled.

  Two details that are load-bearing rather than incidental: the install
  must happen directly in `kernel_main()` (changing the guard while an
  instrumented frame is live panics that frame on return -- the
  defence firing on innocent code), and the guard's low byte is forced
  to ZERO deliberately, as glibc does, so that the guard terminates a
  string-copy overflow instead of surviving one.
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
  function.** `userland/tests/stack_smash_test.c`'s first version called an
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

## An overlay forces a full repaint, because "declares no damage" is not the same as "is drawn unrestricted"

The Start menu, context menu, file picker and confirm dialog draw
outside any window's rect and declare no damage of their own. The
compositor's design note called that "falls back to a full-screen
repaint" -- and for a long time it was true, but only by accident: an
overlay frame usually had nothing *else* reporting damage either, so
the frame went unrestricted for that reason rather than because anyone
arranged it.

The moment something else declares damage in the same frame, the
fallback inverts. The frame becomes damage-limited, the overlay is
clipped away, and whatever was on screen before it stays there.
`wm_render_frame()` (`apps/wm/wm_render.c`) now discards the damage box
outright while any overlay is open, which makes the documented
behaviour actually hold instead of depending on a coincidence.

Blunt on purpose: giving each overlay a real damage rect is the better
end state and needs geometry that only `start_menu` exposes today (see
`docs/roadmap.md`'s Milestone 12 entry). Correct-by-construction first,
precise later -- the same order the compositor's other phases took. See
`CHANGELOG.md`'s `[Unreleased]` damage-sweep entry for the reproducer
and the two further bugs the change uncovered underneath it.

## A dropdown's popup is a second draw call the app makes last, not a WM overlay

`ui_dropdown`'s popup list is drawn by `ui_dropdown_draw_popup()`, which
the app calls **after every other widget** -- not by `ui_dropdown_draw()`
itself, and not through the WM's overlay machinery that the file picker,
context menu and confirm dialog use.

Drawing here is immediate-mode: z-order is call order, and there is no
retained view tree to sort. A popup drawn from `ui_dropdown_draw()`
would sit at whatever position the dropdown occupies in the app's draw
sequence, and anything drawn after it would paint over the list. Hiding
that inside one call would need a deferred-draw list this GUI doesn't
have and doesn't otherwise want.

Making it a WM overlay was the other candidate, and is what a real combo
box does -- Windows' popup escapes its window entirely. It was rejected
because those overlays are `apps/wm/` internals with WM-level modality,
and a widget in `apps/ui/` reaching into them would invert the layering
this directory is built on (see `apps/ui/ui_button.h`). The cost is
real and is stated in the header: `wm_render_frame()` clips each app's
`on_draw()` to its content rect, so the popup **cannot leave the
window**. It flips above the box when there is no room below and shrinks
to what is available otherwise; the scrollbar it inherits from
`ui_listbox` is what keeps that acceptable rather than a truncation.

Input is the mirror rule -- the popup is on top, so it gets first
refusal, and an app forwards to the dropdown *before* the widgets
underneath it. See `CHANGELOG.md`'s `[Unreleased]` entry for the
worked example in UI Demo.

## ui_listbox counts scroll from the top; ui_scrollbar counts from the bottom

`ui_scrollbar.h`'s `scroll_offset` is 0 at the **bottom** (pinned to the
newest line), increasing toward the oldest. That convention is right for
the terminal scrollback it grew up alongside, and three callers depend
on it. A list is the other way round: 0 is the first item.

`ui_listbox` therefore works entirely in list coordinates and converts
at the boundary, in two one-line helpers (`listbox_bar_offset()` /
`listbox_top_from_bar()`) that are the only place the two conventions
meet. Changing `ui_scrollbar` to a neutral orientation was the
alternative; it was rejected because it would have touched Terminal,
Notepad and `ui_textview` for the benefit of one new caller, and a
silently-inverted scrollbar is a bug that looks like a rendering
glitch rather than a logic error.

## GUI tests wait on the WM's queue depth, not on a sleep derived from frame rate

`tools/gui_debug.py`'s `settle()` used to sleep a fixed 250ms after
injecting synthetic input, reasoning that events drain one per WM frame
at 100Hz, so a click's four need ~40ms and a drag's eleven ~110ms.

The premise is false: the WM loop is not a metronome. A drag measured
at ~800ms with `gui damage verify on`, which renders every frame twice
and diffs the whole screen -- roughly 70ms per event, seven times the
assumed rate -- and it moves again with the font size, the window count
or the display driver. The fixed sleep therefore raced. Windows moved
between a test's `gui windows` and the command using those coordinates,
so a drag grabbed the wrong thing, and `tools/damage_sweep.py`'s
ancestor reported a *different* bug on each run of the same script.

`gui state` reports `pending` (undelivered injected events,
`wm_debug_input_pending()`) and `settle()` polls it to zero. Anything
derived from frame rate is a guess; the queue depth is a fact. The
commands themselves stay asynchronous and non-blocking -- they are
dispatched from inside the very loop that drains them, so waiting has
to happen on the host side (see `apps/wm/wm_debug.h`).

## Modifier keys ride alongside the key, they don't re-encode it

The input ring carries `(mods << 16) | key`. The KEY half is unchanged
and still terminal-encoded -- Ctrl-A is 0x01, Alt-B is ESC then 'b', as
the "Ctrl and Alt" section of `api/keyboard.h` has always described --
so `keyboard_getchar()` returns exactly what it always did and every CLI
consumer is untouched. The mods half is *additional*, read through
`keyboard_getchar_mods()` / `keyboard_try_getchar_mods()`.

**The motivating case is Shift-Tab.** Shift only swaps the layout's
character table, and Tab has no shifted variant, so Shift-Tab and Tab
are both 0x09 and a focus ring cannot cycle backwards. There is no way
to express it in the terminal encoding at all.

The alternative considered was another discrete `KEY_*` code, as the
`KEY_SHIFT_ARROW_*` and `KEY_CTRL_ARROW_*` families got. That was right
once and doesn't scale: each new GUI combination needs another constant
and another line in `keyboard.c`, and there are only ~32 free codes
before the Nordic block at 0xC4. A live "what is held now?" query was
rejected for the reason those families exist in the first place -- state
read after the fact can disagree with the keypress it describes. The
mods are sampled inside `ring_push()`, at scancode-processing time, the
same instant the layout table picks between 'a' and 'A'.

Consequence worth knowing: `KEY_MOD_CTRL` is reported but a GUI should
rarely match on it, because Ctrl has *already* folded the letter away.
`key == 'a' && (mods & KEY_MOD_CTRL)` is never true; match 0x01. Shift
is the useful bit precisely because it doesn't fold the key.

`gui key <c> [shift|ctrl|alt|altgr]` sends them, and
`gui_apps.h`'s `on_key` grew a `mods` parameter -- four apps implement
it, so extending the signature beat a hidden accessor valid only during
the callback.

## Keyboard focus is an app-level ring with a per-widget ops table, not a WM concept

`apps/ui/ui_focus.h` owns "which widget gets keys", Tab/Shift-Tab
cycling, and the focus ring. It exists because routing keys by trying
each widget in turn breaks as soon as two of them take the keyboard:
whichever is tried first swallows everything it recognises. UI Demo hit
this the day `ui_dropdown` landed -- a dropdown handles arrows even
while CLOSED, so the listbox below it could never be arrowed at all.

**Per-widget ops table, not a switch.** A widget joins by exporting one
`const struct ui_focus_ops` (key/hit/draw_ring/accepts_focus/
set_focused). The alternative -- a `ui_widget_kind` enum and a switch
inside `ui_focus.c` -- would put every widget's name in that file, and a
widget that forgot its case would fail silently at runtime rather than
at the call site. Exporting a table is the same "adding one is adding a
row" property `gui_app_registry[]` and the Control Panel's applet table
already have.

**App-level, not WM-level.** The WM already decides which WINDOW has the
keyboard; a second global focus would have to be kept in agreement with
it. Tab order is array order -- the caller writing the array already
controls it, and every alternative (positional sorting, explicit
indices) is more machinery for a list nobody has found too long to
reorder by hand.

Two smaller calls inside it: a `ui_button_group` is ONE focus stop with
arrows moving between its buttons, because a row of related controls is
one stop in every real toolkit; and the ring WRAPS while `ui_listbox`
CLAMPS, which is not an inconsistency -- a tab ring is a cycle with no
ends, a list has a first and last item whose boundaries mean something.

## NX landed in userspace first, and the default mapper is the non-executable one

Milestone 2's "NX bit enforcement" and "W^X on kernel + userspace
mappings" roadmap items were done for the *userspace* half first
(`kernel/proc/elf.c`/`vmm.c`, `userland/rt/link.ld`), deliberately not
touching `kernel/arch/x86_64/boot.asm`'s own flat 2MiB-huge-page
identity map, on the reasoning that process page tables get created
fresh per process anyway while the boot map is a boot-critical path.
The kernel half landed a milestone later and is its own entry below --
**the identity map is no longer RWX**, so don't take the ordering here
as a statement about today's state.

The default mapper (`vmm_map_user_page()`) was changed to be
non-executable by default rather than adding a parallel "safe" variant
-- every pre-existing call site (a process's stack, `SYS_SBRK` heap
growth, the GUI framebuffer, a window's pixel buffer) is data, never
code, so this is both the secure default and correct for all of them
with zero call-site changes; only `kernel/proc/elf.c` (needs real
per-segment control) and `kernel/proc/ring3_test.c` (its one
hand-assembled code page) call the explicit-flags variant instead. See
CHANGELOG.md's `[Unreleased]` entry for the full mechanics and the QMP
verification (a purpose-built `userland/tests/nx_test.c` that jumps into a
non-executable data page and confirms the CPU actually faults --
`error_code=0x15` decodes to Present+User+Instruction-Fetch, not a
generic unmapped-page fault).

## Kernel W^X: NX on every huge PDE, one 4KiB split for `.text`, and CR0.WP

`paging_enforce_wx()` (`kernel/arch/x86_64/paging.c`, called from the
top of `kernel_main()`) rewrites the identity map boot.asm hands over.
It is deliberately NOT a general "make the map fine-grained" pass: all
2048 2MiB PDEs keep being huge pages and just get their NX bit set,
and only the slots holding something that must not be writable get
split down to 4KiB. That is one slot -- `.boot`/`.text`/`.rodata`/
`.eh_frame`/`.ktests` all fit inside the first 2MiB page -- so the
whole thing costs one 4KiB table out of `.bss` and needs no allocator,
which is why it can run before `pmm_init()` rather than after.

Two claims the previous entry made turned out not to hold, and both
are worth knowing before someone re-derives them:

- **`pmm.c` needed no changes.** The entry above predicted its
  frame reservation would have to become section-aware. It reserves
  `0.._kernel_end` as one blob and nothing here frees any of it, so
  section-awareness would only matter to a change that wants to hand
  parts of the image back, which this isn't.
- **The 2MiB granularity was never the obstacle.** The obstacle was
  believing the split had to happen in `boot.asm`'s 32-bit
  pre-long-mode code. Doing it in C afterwards is the same result with
  none of that risk, and it can read the linker symbols directly.

**Ring 3 is unaffected because user mappings never enter this map at
all**: `userland/rt/link.ld` links at `0x8000000000`, i.e. PML4 index
1, while the identity map is everything under index 0. Every process's
PML4 shares entry 0 (`vmm_create_address_space()`), so the blanket NX
reaches every address space -- and touches no user page.

**CR0.WP is the half that is easy to omit and impossible to notice.**
With WP clear -- the state the CPU resets into, and what GRUB hands
over -- a supervisor write ignores the read/write bit entirely, so ring
0 can scribble over a `.text` mapping that reads as read-only in every
page table. NX needs no equivalent switch (EFER.NXE covers it), so the
failure mode is half-working protection whose page tables look
completely correct in a dump. The `paging` KTEST asserts the bit
separately for exactly this reason, and its positive control is the
demonstration: clearing that one line turns the CR0 check red and
leaves every page-table check green.

Verified live, not only by reading bits back: a one-byte write to
`__ktext_start` from `kernel_main()` produces `PANIC: Page fault`. That
probe is not committed -- a ring-0 fault ends the boot, so it cannot
live in a suite -- see CHANGELOG.md's `[Unreleased]` entry for how to
reproduce it in two lines.

## The framebuffer is write-combined via PAT, and `nopat` exists to make the MTRR fallback reachable

Reported as "drawing is really slow" on a real machine (an ASUS Zenbook
UX305FA) while being perfectly fast under QEMU. The cause was that
nothing in this kernel had ever set a memory type: `pat` and `mtrr`
existed only as CPUID feature-name strings in `cpu_features.h`. GRUB's
linear framebuffer is therefore whatever the firmware left it as, which
on real hardware is **uncached MMIO** — every store is a bus transaction
the CPU stalls on. `gfx_present()` compounded that by writing pixels a
BYTE at a time (three stores per pixel), so a full 1920x1080 frame was
6.2 million individually-stalled writes.

**QEMU cannot show any of this**, because its framebuffer is ordinary
cached host RAM. That is the important part for a future session: no
test in this repo can observe the bug, a clean `gui_regress` says
nothing about it, and the only instrument is `gfxbench` run on real
hardware.

**PAT is preferred over MTRRs** because it is per-page: it needs no
power-of-two size, no natural alignment and no free range register,
all three of which a variable-range MTRR demands and a framebuffer does
not reliably offer. Slot 4 of `IA32_PAT` is repointed at WC and slots
0–3 are left at their architectural defaults, so every mapping that does
not opt in keeps exactly the meaning it had; a page opts in by setting
the PAT bit and clearing PCD/PWT, which selects slot 4.

An MTRR marking a region UC does not defeat this — SDM Table 11-7 gives
UC(MTRR) + WC(PAT) = WC, which is why Linux write-combines framebuffers
through PAT without touching MTRRs either.

**The trap, and it fails silently in the dangerous direction:** bit 12
is PAT on a 2 MiB page, but on a 4 KiB page bit 12 is part of the
PHYSICAL ADDRESS and PAT is bit 7. Writing the huge-page bit into a 4 KiB
PTE does not fault; it silently repoints the mapping somewhere else. The
framebuffer is far above the kernel image so it is never in a range
`paging_enforce_wx()` split, but the code checks `PAGE_HUGE` rather than
relying on that.

**`nopat` exists because the fallback would otherwise be unreachable.**
PAT has been present since the Pentium III, so every machine this OS can
run on — QEMU's default model included — takes the PAT path, and an MTRR
path nobody can execute is a guess, not a fallback. This is the same
reasoning as `ata nodma` keeping the PIO disk path reachable. With the
flag, both were verified to boot and to report the mechanism they
actually used.

What is NOT proven by anything committed: that write-combining is
*faster*. It cannot be, in this environment. The speed claim can only be
settled by `gfxbench` on the real machine.

## The RAM meter is an uncomposited overlay, which is why it is debug-only

`rammeter` (a GRUB flag, see `docs/boot-flags.md`) draws a live
frame-allocator and heap readout in the top-right corner. It writes
STRAIGHT to the visible framebuffer through `gfx_overlay_*`, bypassing
the back buffer, the clip rect and the dirty-rect box alike.

That combination is normally a bug — it is precisely what leaves stale
pixels behind, and it is the family `gui damage verify on` exists to
catch. It is correct here only because the overlay is never composited:
the WM knows nothing about it, paints over it whenever it repaints that
corner, and the meter reappears on its next tick. Nothing it draws is
interactive, so nothing is lost when a repaint eats it.

**A control the user touches must not be built this way.** It belongs in
the back buffer with its damage declared, or the verifier will correctly
call it a violation.

Two consequences worth knowing. The damage verifier compares BACK BUFFER
contents, so the overlay is invisible to it and reports no violations —
which is a property of where it draws, not an exemption anyone coded.
And it ticks only from `wm_render_frame()`, so it appears on the desktop
and NOT at the physical console, which has no repaint loop to hang it
off.

**The heap row is deliberately not warn-coloured.** `heap_total_bytes()`
is what the allocator has claimed from pmm so far, and it claims more on
demand, so heap-used-against-claimed sits near full as a matter of
course — it read 89% on a freshly booted desktop. Colouring that yellow
would cry wolf every boot and teach the reader to ignore the one row
where the colour means something. Only the physical-frame row has a real
ceiling, so only it gets the green/amber/red bands.

## The user stack's guard is an unmapped hole plus two rules, and the sbrk rule is the one that mattered

`kernel/include/kernel/uaddr.h` states the ring-3 address-space map
once -- heap base, heap limit, guard region, stack bottom and top --
and `scheduler.c`'s spawn path, `elf_run.c`'s legacy loader,
`syscall.c`'s `SYS_SBRK` and `idt.c`'s fault report all read it. It used
to be two identical copies (`PROC_USTACK_*` and `ELF_RUN_STACK_*`) with
nothing keeping them equal, and the fault classifier would have been a
third.

**The guard region has no page-table representation, and does not need
one.** It is defined by being unmapped, which is what a page that was
never mapped already does. So a stack overflow ALREADY faulted before
any of this; nothing was added to make it fault. What the constants buy
is the two things a hole cannot do for itself:

- **`SYS_SBRK` is bounded against it.** This is the real defect the
  work found. The heap grows up from `0x8000100000` and the stack down
  from `0x8000200000`, about 1 MiB apart, and sbrk had no ceiling of
  any kind -- a large enough request mapped fresh pages straight over
  the live stack, one page at a time. Nothing faulted and nothing was
  logged; the process simply found its own locals changing underneath
  it. The check is written `inc > LIMIT - brk` rather than
  `brk + inc > LIMIT` because the sum overflows for a large enough
  increment and the comparison then passes.
- **The fault gets a NAME.** `uaddr_is_stack_guard(cr2)` in the ring-3
  branch of `isr_dispatch()` turns `Page fault / CR2=0x80001fc...` into
  `Stack overflow` plus the stack's range. Ring 0 is excluded
  deliberately: the kernel's own stacks are elsewhere, so a supervisor
  fault at that address is a wild pointer and mislabelling it would be
  worse than not labelling it.

One guard page does not catch a single frame LARGER than the guard
jumping clean over it -- the classic guard-page hole, which real
kernels close with a stack-probe ABI. `UADDR_GUARD_PAGES` is there to
be widened rather than have a second mechanism grow beside it.

**Both positive controls changed the design, and neither confirmed
what it was expected to.** Removing the sbrk bound left
`userland/tests/guard_test.c` entirely GREEN, because the test asked
for 1 GiB and the guest ran out of physical memory long before it ran
out of address space -- sbrk refused for the wrong reason and every
check passed. Asking for 2 MiB instead (just past the gap, trivially
allocatable) reddened the refusal checks, and writing through the
returned pointer was needed on top of that before the corruption became
visible at all: an alias costs nothing until somebody writes. And
`userland/tests/stackovf_test.c` first hung forever without faulting,
because GCC's accumulator form of tail-recursion elimination had turned
`return frame[0] + burn(depth + 1)` into a LOOP with one reused frame
at -O2. `volatile` on the frame does not prevent that; the call goes
through a `volatile` function pointer now, and the frame is read after
the call returns. `objdump -d` is what settled it, not reading the C.

## SMAP is absolute here because the kernel copies through its own identity map, not with STAC/CLAC

CR4.SMAP faults a supervisor access to a page whose mapping has U=1.
The standard answer is to bracket every deliberate kernel access to user
memory in `STAC`/`CLAC` -- which switches the protection OFF for exactly
the window a bug would use it in, and which requires getting every
window's extent right forever.

This kernel does not do that, and **sets EFLAGS.AC nowhere at all.**
`vmm_copy_from_user()`/`vmm_copy_to_user()`/`vmm_copy_string_from_user()`
walk the process's page tables to the physical frame and copy through
the kernel's OWN identity map -- a supervisor access to a supervisor
page, which SMAP does not police. That option exists because boot.asm
identity-maps the whole low 4 GiB; a kernel without a full physmap could
not choose it.

Three things follow, in descending order of how easy they are to
forget:

- **A raw `*(T *)user_ptr` in kernel code is now a page fault**, not a
  subtle bug. That is the point: the rule is enforced by the CPU rather
  than by review. All 23 sites that used to do it -- struct copy-outs,
  path strings, the `SYS_READ`/`SYS_WRITE` bulk buffers, `SYS_LISTDIR`'s
  per-entry writes, strace's argument strings -- go through the helpers.
- **The helpers subsume `vmm_validate_user_range()` where they replaced a
  validate-then-copy pair**, and close a TOCTOU gap in doing so: the walk
  and the copy are one operation per page, so there is no interval in
  which a checked mapping can change before it is used. The validator
  still stands alone where nothing is copied.
- **`paging_make_user_page()` is the live trap.** It adds U=1 to the
  KERNEL's own identity mapping of a page, which makes that page
  SMAP-protected against the kernel's ordinary access to it, at the
  address the kernel normally uses. Nothing calls it outside `paging.c`
  today; a future caller must go through the helpers or fault in code
  that looks innocent.

SMEP (ring 0 cannot execute a user page) needed no audit -- the kernel
never executes user pages -- and is the cheaper half by far.

**Both bits are absent on QEMU's default `qemu64` model**, so the
hardware path only runs under `--cpu max`. The KTESTs are written to
assert CR4 against CPUID rather than asserting the bits are on, so they
are meaningful under both models and can fail under either. What they
CANNOT show is enforcement: the helpers never touch a user mapping, so
they behave identically with SMAP on or off. That was proved separately
by putting one raw dereference back into `SYS_WIN_CREATE` -- ring-0
`#PF`, `CR2` pointing at the client's stack, `error_code=0x1`, under
`--cpu max`, while the same build ran clean on `qemu64`.

## A blank console cell gets the console's colour, and a coloured line pads to its own edge

Two rules in `kernel/drivers/vga.c`, from one visible bug: the panic
banner painted ragged red stripes across lines that had nothing to do
with it.

**A scroll fills the incoming row with the console's DEFAULT background,
not the live `cur_bg`.** The row scrolling in is blank -- nobody has
written to it -- so it belongs to the console rather than to whatever
colour a caller happens to have set. Filling it with `cur_bg` painted a
full-width band no text had asked for, and text drawn on that row later
only repainted its own cells, leaving the rest of the band behind. This
is the same reasoning `cursor_hide()` already spells out for the cursor
cell, applied to the other place that invents blank space.

**A newline with a non-default background pads to the end of the line.**
Otherwise a coloured run's right edge is wherever its text happened to
stop, which reads as a highlight rather than as a banner -- and made the
banner's appearance depend on whether a scroll had happened to fill the
row first. Padding makes it deliberate.

Two things about where that padding lives:

- It is in `vga_putc()`, the single funnel, **so the spaces go through
  `sb_record()` as well.** Done inside `fb_putc()` alone it would look
  right until PageUp redrew the line from scrollback without it.
- The padding **replaces** the newline on screen (writing the last column
  wraps, which is the same move) but **must still record one** --
  `sb_record('\n')` is what calls `sb_start_line()`. Skipping it
  accumulated all five banner lines into a single scrollback line, and a
  PageUp/PageDown round trip redrew the banner as one stripe with four
  lines missing. Found by testing the round trip, not by reading it.

And the loop bound is computed BEFORE the first space: looping on
`col < width` does not terminate, because writing the last column wraps
`col` back to 0. That fills the screen solid red, which is at least an
obvious failure.

## `/etc` and `/tmp` are created by the MOUNT, not by `kernel_main()`

`ensure_layout()` in `kernel/fs/vfs.c` makes both, and it is called
from `fs_init()` and from `fs_format_backend()`. They used to be two
`fs_mkdir()` calls on the line after `fs_init()` in `kernel_main()`,
which is correct exactly once per boot and wrong the moment anything
else mounts a filesystem -- `fsformat` reformats and remounts a live
disk and never goes near that line, so it left a volume with neither
directory until the next reboot.

The general shape is worth keeping: **if a step belongs to "having a
filesystem" rather than to "booting", it belongs beside the mount.**
What makes it cheap is that `fs_mkdir()` is a no-op on an existing
directory, so the rule can be "after every mount" with no conditions
to get wrong. See CHANGELOG.md's `[Unreleased]` entry.

## A setting reports whether it PERSISTED, separately from whether it applied

`tz_set_index()`, `font_config_save()`, `cursor_config_save()` and
`keyboard_config_save()` return `enum setting_result`
(`kernel/include/api/etc_config.h`): `SETTING_INVALID`,
`SETTING_SAVED`, `SETTING_UNSAVED`. Three values rather than a bool
because a caller has three different things to say -- and the four
shell commands do say them, through one shared `print_save_result()`.

This replaced three `void` returns and one that answered a different
question (`tz_set_index()` returned 1 for a valid index whether or not
the write landed). The symptom was `timezone Helsinki` printing
`Timezone set to helsinki.` on a filesystem with no `/etc` and writing
nothing -- a lie the user only discovers after a reboot. Note the
writer was never at fault: `etc_config_set()` correctly returned 0 to
callers that did not look, which is the reusable lesson. **A function
that can fail and whose caller returns `void` is a silent failure
waiting for a reason to happen.**

`SETTING_UNSAVED` is deliberately non-zero so the existing
`if (!tz_set_index(i))` idiom still reads as "did it apply?" -- adding
a distinction should not force every caller to care about it.

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

## Shell session state is initialised by the DISPATCHER, not by the REPL

`shell_session_init()` (`apps/shell.c`) loads the history file and
parses PATH, is idempotent, and is called from both `shell_main()` and
`shell_dispatch()`. Those two lines used to sit at the top of
`shell_main()` alone, which is correct exactly as long as the
interactive REPL is the only way into the dispatcher -- and it isn't.
Two other callers reach `shell_dispatch()` directly: `apps/demo.c`'s
`sh` verb, and the serial debug console's `sh` command
(`kernel/core/debug_console.c`, which is what `tools/vm.py exec` drives).

On a demo boot, `demo_run_cli()` runs from `kernel_main()` *before*
`apps_start()`, so `shell_main()` is never reached and PATH was left
empty for the whole tour. The failure was almost invisible: every other
command in `data/wm/demo.script` -- `about`, `df`, `fsck`, `ls`,
`lspci` -- has its own builtin dispatch entry and worked perfectly, so
the single casualty was `lscpu`, the one command in the script with no
builtin, printing "Unknown command" in a screen that scrolls past. (`ls`
is a builtin *wrapper* that hands `/bin/ls` an absolute path, per the
entry above, which is why even it was unaffected.)

The general shape is the same one `ensure_layout()` records above: **if
a step belongs to "having a shell" rather than to "running the
interactive loop", it belongs beside every entry into the shell, not in
one of them.** What makes the rule cheap here is the same thing that
made it cheap there -- a `static int done` guard means it can be called
unconditionally with no ordering to get wrong.

`tools/demo_test.py` is the regression test, and its load-bearing check
is that a PATH-resolved command really reached `elf_run`. Its positive
control is worth repeating before trusting it: reverting the
`shell_dispatch()` call reddens exactly that one check and leaves the
other five green -- so "the demo booted, reached the desktop and opened
windows" is, on its own, no evidence at all that the tour worked.

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

## An empty clip rect draws nothing -- it is not gfx_clear_clip_rect()

`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY clip: every
pixel write is rejected until the clip is changed or cleared. It used to
do the opposite -- treat non-positive as "clear the clip", full screen
drawable -- while `gfx.h` described that same case as "(nothing draws)".
The one caller that can produce an empty rect
(`apps/wm/wm_render.c`'s `clip_to_window_content()`, intersecting a
window's content with the frame's damage box) was written against the
words, not the behaviour, so a frame whose damage grazed a window's
border without reaching its content handed that app's `on_draw()` an
UNCLIPPED screen. That was the damage sweep's long-standing "20 px"
violation (the resize grip buried by a clock-tick frame) and the
intermittent 76k-px resize one; the recorded known-issue's own probe
detail turned out to be wrong, a fresh reminder to measure before
fixing. The rule worth keeping: the two states are different operations
on purpose -- `gfx_clear_clip_rect()` is the only way to remove the
clip, and a computed rectangle with nothing in it must clip everything
out, for the same reason a formatter that can't fit writes nothing.
See `CHANGELOG.md`'s `[Unreleased]` entry for the full diagnosis.

## TFS2 stays in the kernel as a second filesystem -- the VFS probes by superblock magic

Milestone 15 (TFS3) did not replace TFS2: both backends are compiled
in, `vfs.c`'s `fs_init()` walks them in priority order (tfs3 first)
asking each one's side-effect-free `probe()`, and the first valid
superblock wins -- so an existing TFS2 disk keeps mounting untouched
while fresh/blank disks get the default (TFS3). Kept deliberately, at
the user's request, to make filesystem switching a testable, living
path: `fsformat <tfs2|tfs3> confirm` reformats and remounts live
(wiping the OTHER format's signatures first -- the wipefs rule, see
`fs_ops.h`'s `wipe()` contract for the mounted-a-corpse story), and
`tools/fs_switch_test.py` proves the whole cycle including reboot
persistence. Capabilities differences are declared, not discovered:
`fs_ops.caps` mirrors `display_driver`'s honesty rule (bit and
optional op are one fact stated twice, refused when they disagree),
`fs_stat()` is one canonical shape (epoch times + an ino that TFS2
synthesizes from its table slot, Linux's FAT trick), and the ring-3
ABI never changed (epochs convert back to `rtc_time` at the syscall
boundary). See CHANGELOG.md's `[Unreleased]` Stage A/B entries.

## TFS3's journal covers dirent + inode blocks; bitmaps stay leak-safe write-through

The design doc sketched journaling "dirent + inode + bitmaps"; the
shipped journal (Stage C) deliberately narrowed to dirent blocks and
inode-table blocks only -- the structures whose torn write is
namespace corruption. Allocation bitmaps and group descriptors are
write-through and unjournaled under set-before-use /
clear-after-persist ordering, so a crash costs a leaked block that
`fsck` reclaims and never a double allocation -- the exact rule TFS2
established ("prefer a leak to a double-allocation") applied to the
new format. Every operation fits <= 3 of the journal's 4 slots, and
directory growth runs as its own empty-block-first transaction
(inserting the child's name into the grow block would have made the
name visible one transaction before the child's inode existed). See
`docs/tfs3-spec.md`'s journal section and CHANGELOG.md's Stage C
entry.

## TFS3 v2 grew the journal by moving the layout, not by making it a log like ext4's

Four slots turned out to be a design constraint on OPERATIONS, not a
tuning number: a rename that moves a directory between parents touches
five metadata blocks (both dirent blocks, the child's `..`, both
parents' link counts), so it could not be expressed at all. The
journal sits between the superblock and the group descriptors, and
everything before group 0 was spoken for, so making room meant moving
`group0_start` -- i.e. a format version.

**Why not ext4's journal.** jbd2 makes the journal a regular inode
(inode 8, ~128 MB by default) holding a circular log: a descriptor
block naming each following image's real target, the images, then a
commit block. Three separable ideas live in that, and only one was
worth taking now:

- **Credits, taken.** `jbd2_journal_start(journal, nblocks)` reserves
  the worst case up front and refuses an operation that cannot fit
  before it has changed anything. TFS3 used to discover "full" halfway
  through, when `txn_stage()` returned 0 and each caller unwound by
  hand. `txn_begin(credits)` is that discipline in miniature, and it
  is what lets a v1 image behave CORRECTLY rather than half-completing:
  the one operation it cannot hold is refused with a message, and
  everything else is unaffected.
- **A large circular log, deferred.** Its real payoff is batching many
  operations into one commit, which would cut the two `ata_flush_now()`
  barriers TFS3 pays per metadata operation. That is a throughput
  project with its own crash-recovery surface (sequence numbers, log
  wrap, checkpointing), not a side effect of needing five slots.
- **Revoke blocks, not needed.** They exist because a freed metadata
  block can be reused as file data, where replay would clobber it.
  TFS3 journals only dirent and inode-table blocks, and frees blocks
  unjournaled under the leak-safe rule, so the hazard never arises.

**Both versions stay mountable, and that is not politeness.** A probe
that returned "not mine" for a v1 image would hand it to the
blank-disk policy, which formats -- so refusing to READ an old format
is a way of destroying it. v1 mounts read/write with its own geometry;
only `format` (and `fsformat tfs3 confirm`) writes v2. Each version's
geometry is a set of CONSTANTS rather than superblock parameters,
which preserves the property the fixed-size descriptor table exists
for: a reader whose primary superblock is unreadable has two candidate
values of `group0_start` to try, not an unknown one. The superblock
does carry the offsets, but a mount validates them against the
version's constants and rejects a disagreement.

The reformat also has to erase the OTHER version's backup superblock
sectors -- the wipefs rule one format version apart instead of one
filesystem apart, and the same seance it was written for. See
`docs/tfs3-spec.md`'s layout section.

## `fs_rename()` refuses an existing destination -- there is no atomic replace

POSIX `rename(2)` silently replaces the destination. `fs_rename()`
returns 0 instead, and the shell's `mv` says "remove it first".

Two reasons. The API's whole style is "a parser rejects rather than
guesses" applied to destructive operations -- and this is the one
mistake `mv` can make that a user cannot undo, because the replaced
file's blocks are gone. And the atomic version is a bigger operation
than it looks: it has to free the old target's inode inside the same
transaction, which adds a slot and a rollback path for something no
caller has asked for. Adding it later is additive; having shipped a
silent overwrite and then restricting it would not be.

Two other refusals are not policy but necessity: a directory moved
into its own subtree would detach that subtree into a cycle nothing
references, and the root has no parent to be renamed in. Renaming
something to its own path succeeds and changes nothing.

## Truncation is two phases with a commit between them, and keeps the boundary tables in memory

Shrinking obeys the same ordering as every other metadata change here
-- the inode that stops referencing a block must be durable BEFORE the
block's bit is freed, or a crash in between leaves a live file pointing
at space the allocator can hand to a second file. The obvious
implementation (free the tail, then write the inode) inverts exactly
that, and it is the double-allocation the whole discipline exists to
prevent.

That forces a commit into the middle of the operation, which creates a
second problem: phase one rewrites the pointer tables, so phase two can
no longer read from disk what it is supposed to free. The way out is
the shape of the cut. A truncation is a clean split -- at every level
each entry is wholly kept or wholly dropped -- EXCEPT for at most one
straddling entry per level. So there are at most three partially
rewritten tables, and keeping their original images in memory (12 KiB)
is enough for phase two to walk everything phase one detached; every
other table it reads is one phase one deliberately did not touch.

Growing needs none of this: both backends read an unallocated range as
zeros, so a grow moves the size field and nothing else. `truncate f
1000000000` is one inode write and no blocks.

Both backends implement this the same way and separately
(`trunc_begin`/`trunc_free` in tfs3.c, `trunc_detach_tail`/
`trunc_free_tail` in tfs.c), consistent with their already-parallel
block-map walks -- they persist through completely different mechanisms
(a journal transaction vs. a record write), which is most of what the
code around the walk is.

## The shell is called `tosh`, and the name covers the language, not a binary

`tosh` = t + OS + h, contracting "toy-os shell" the way `bash` contracts
"Bourne-again shell". It was unnamed until 2026-08-15, which was fine
while there was one command line and no reason to refer to it.

**What the name covers.** The shell LANGUAGE and behaviour -- the
builtins, the command-line editing, the way a line is dispatched --
which today has two front ends: `apps/shell.c` in the kernel and
`userland/lib/tosh.c` in ring 3. `sh` names a language rather than one
binary, and this follows that. It is deliberately NOT the terminal:
`uterm` is the terminal emulator, and a terminal that is not a shell is
a distinction every real system keeps.

**Four names were rejected for collisions**, which is most of the
reasoning worth recording, because each looks obviously right until you
search for it:

- `toysh` -- toybox's actual shell.
- `hush` -- busybox's actual shell.
- `tsh` -- the CS:APP shell lab, assigned to enormous numbers of
  students, so the name is unsearchable.
- `tush` -- reads as crude Finnish slang, which the maintainer
  (Finnish) would have to explain forever.

`wish` (Tcl), `posh` and `ion` (Redox) were ruled out the same way.
`tosh` is British slang for "nonsense", which is the one live objection
and was accepted deliberately: for a hobby OS's shell it reads as
self-aware rather than rude, and no software owns the name.

**There is no `/bin/tosh` yet.** The ring-3 shell is a LIBRARY, because
its first caller is a GUI terminal that owns its own event loop and
cannot sit blocked in a `read()` (see `tosh.h`). A standalone binary
would be a thin `main()` over the same `tosh_init()`/`tosh_run_line()`
calls -- the header has said so since it was written -- but it needs an
interactive stdin story first: a process started by the physical
shell's `run` has nowhere useful to read a line from, which is the same
limitation that makes `echotest` hang under headless testing.

## The process entry ABI is SysV, and crt0 owns the stack alignment

A new process starts with the standard SysV layout on its stack --
`argc` at `(%rsp)`, then `argv[]`, a NULL, then `envp` (empty; there is
no environment yet, and no auxv, because nothing consumes one and
inventing entries nobody reads is how an ABI accumulates fiction).
`userland/rt/crt0.asm` reads it and calls `main()`.

It used to arrive in RDI/RSI instead. That was toy-os's own convention,
fine while every `_start` was a C function taking two parameters, and a
wall for Milestone 40's "run stock musl binaries". The register path was
deleted rather than kept alongside the stack one: two live conventions
for the same thing is exactly how an ABI rots.

**The alignment inverted with that change, and the direction is
counter-intuitive.** The kernel used to hand over `RSP % 16 == 8` on
purpose. That looked wrong and wasn't: GCC compiles a plain C `_start`
like any other function -- assuming a `call` has just pushed a return
address -- and sizes its prologue from there, so a "correctly"
16-aligned RSP put every aligned stack slot off by 8.

With a hand-written entry point, the standard applies. SysV states the
rule at the CALLEE's entry (`%rsp + 8` is a multiple of 16 there), which
means `%rsp` must be **16-aligned immediately before `call main`**. The
tempting `sub rsp, 8` in crt0 -- reasoning "the old convention was 8, so
restore it" -- produces the opposite and is a real bug: `main()` is then
entered 16-aligned, GCC emits `movaps` against stack slots it believes
are aligned, and those FAULT rather than mis-store.

The failure mode is worth remembering because it disguises itself: every
GUI client took a #GP a few instructions into `main()`, while every
plain non-SSE program worked perfectly. Nothing about that symptom
points at stack alignment until you notice which binaries are affected.

See `CHANGELOG.md`'s `[Unreleased]` entry, `userland/rt/crt0.asm`, and
`kernel/proc/elf_run.c`'s `elf_build_argv_on_stack()`.

## A legacy ring-3 process needs its own RSP0 and must not be descheduled

`process_run_ring3()` (the M8-M15 blocking path, still used by the
physical shell's `run`) runs a ring-3 process with NO `procs[]` entry.
From the scheduler's point of view that process simply IS "the kernel
context": its trapframe lands in `kernel_saved_rsp`, and there is
nowhere to record its CR3 or its RSP0.

Two consequences, both of which were live bugs the moment ring-3 GUI
clients could stay alive across a shell command, and both of which
presented as a bare `RING-3 CRASH: Page fault` **in the client** at a
syscall unrelated to the cause:

1. **It must not be switched away from.** `kernel_slot_runnable()`
   refuses to select the kernel position while one is armed -- but
   `find_next_runnable()`'s fallback returned `ROT_KERNEL` anyway when
   nothing else was runnable, which reintroduced exactly the case the
   guard existed to prevent. `scheduler_tick()` now returns early while
   `process_context_is_armed()`, so the rotation never starts.

2. **It needs its own ring-0 stack.** This path never set RSP0, and got
   away with it while a legacy process could never coexist with a
   scheduled one: RSP0 was still the boot stack. But `switch_to()`
   points RSP0 at the running process's kstack and leaves it there, so
   a later `run` took its traps onto a CLIENT's kernel stack and
   overwrote the trapframe that client was suspended on. It now uses a
   dedicated `g_legacy_kstack` (one is enough -- these calls cannot
   nest, which is what `process_context_is_armed()` guarantees).

The general lesson is worth more than either fix: an execution context
the scheduler does not own an entry for cannot be treated as
schedulable, and "the kernel context" was quietly serving as a
dumping ground for two very different things. Both are covered by a
KTEST in `kernel/proc/sched_test.c` that runs a legacy process
alongside a scheduled one; see `CHANGELOG.md` for the full writeup.

## The retry sentinel is -2 because 0 is a real answer

Every blocking syscall here shares one contract: the kernel cannot hand
over a result at wake time (the wake runs in an interrupt, under an
address space where the caller's buffer may not be mapped), so it wakes
the caller with "ask again" and the real work happens back inside the
caller's own syscall. See `SYS_WAIT_EVENT` in `abi/syscall_abi.h`.

That sentinel is `SYS_RETRY`, defined as **-2**. It started as 0, which
was fine while the only blocking call was `SYS_WAIT_EVENT` -- but 0 is
a LEGITIMATE result for `SYS_READ`: end of file. The moment pipes made
`read` blocking, a reader woken by a write treated the wake as EOF and
reported that the program had finished producing output at the exact
instant it produced some.

The general rule, which is worth more than the specific fix: a sentinel
must be a value the call can never otherwise return. "0 means nothing
happened" is only safe when nothing can legitimately be zero.

## The ring-3 terminal runs its own shell, not the kernel's

`userland/gui/terminal.c` links `userland/lib/tosh.c` -- a shell implemented over
syscalls -- rather than calling the kernel's `shell_dispatch()` through
some new "run this command line" syscall.

The syscall version would have been much less work and is a defensible
thing to build. It was rejected because it moves the WINDOW to ring 3
while leaving the shell in the kernel, which is the part that actually
matters: the kernel shell's builtins (`fsck`, `ktest`, `fsformat`,
`timezone`) reach directly into the filesystem, the test harness and
driver state. Exposing them through one syscall would re-export the
kernel's internals under a new name and call it a migration.

So `tosh` has the builtins a shell can honestly implement over the file
API, and spawns everything else through `SYS_SPAWN` with its output
piped back. The kernel shell is still reachable -- `Esc` leaves the
desktop for the physical one -- which is the honest division: the
ring-3 terminal does what a terminal does, and kernel-only operations
stay in a kernel-only shell.

The cost is real and worth stating: `tosh` is less capable than the
kernel Terminal today. That is a consequence of the boundary being
drawn honestly rather than a defect to paper over.

## Angles are measured in turns, not radians

`kernel/lib/fixed.c`'s `fx_sin()`/`fx_cos()` take an angle where
`FX_ONE` is one FULL rotation, not `2*pi` radians and not 360 degrees.

Three reasons, in order of how much they matter here:

- **Wrapping is free.** A rotation counter that just increments forever
  wraps correctly on its own, because the units and the fixed-point
  representation agree. In radians, wrapping means a modulo against an
  irrational constant, done in fixed point, every frame.
- **The quarter angles are EXACT.** `fx_sin(FX_ONE/4)` is exactly
  `FX_ONE`. A radian API cannot promise that -- `pi/2` is not
  representable, so the answer is 0.9999-something, and every test of
  the trig has to carry a tolerance instead of an equality.
- **Pi never enters the code.** In a fixed-point path its only possible
  contribution is rounding error.

The cost is that a caller thinking in degrees converts (`deg * FX_ONE /
360`), which is one multiply and reads fine. Callers here think in
rotations anyway -- "a quarter turn per second" is the natural way to
say what an animation does.

See `kernel/lib/geom_test.c`'s first three tests, which are equalities
rather than tolerances precisely because of this choice.

## The geometry rasteriser draws through a callback, not into a framebuffer

`kernel/lib/geom.c` never touches memory that looks like a screen. Its
entire output interface is:

    struct geom_target { void (*plot)(void*, int, int, uint32_t, uint8_t); void *ctx; };

The obvious alternative -- take a surface pointer, a stride and a format
-- would be faster (no indirect call per pixel) and is what a real
graphics stack does at the bottom. It was rejected because it forces the
module to know about pixel formats, clipping and address spaces, and
each of those has more than one answer here:

- `gfx.c` plots into the kernel's framebuffer, routing opaque pixels to
  `gfx_put_pixel()` and partial ones to `gfx_blend_pixel()`.
- A ring-3 app plots into its own window surface, in a different address
  space, through `userland/ui/ugfx.c`.
- `uui_canvas` wraps that again to CLIP -- which is the case that
  justifies the design on its own, see the next entry.
- `kernel/lib/geom_test.c` plots into an array and asserts on
  coordinates, with no framebuffer, no window and no display driver
  involved at all. That test file could not exist against a
  surface-pointer API without faking a surface.

The per-pixel indirect call is a real cost. At the sizes this OS draws
-- a few thousand pixels per shape per frame -- it is not a measurable
one, and it buys four callers that would otherwise be four copies.

## The canvas widget clips in the plot callback, not by trimming geometry

`uui_canvas_line()`/`_ellipse()`/`_polyline()` do not compute the
intersection of the shape with the canvas rect. They install a plot
callback that drops any pixel outside it.

Trimming the geometry is the textbook approach and is faster: a line
clipped by Cohen-Sutherland rasterises only the pixels it will keep,
where this rasterises every pixel and discards some. It was rejected
because it needs a DIFFERENT correct implementation per shape -- line,
polyline, ellipse, filled ellipse -- and the curved cases are exactly
where clipping maths goes subtly wrong. Four implementations that must
agree with each other is the shape of bug this project has paid for
before (see the `kpath` entry: three path resolvers that disagreed).

One bounds test, at the bottom, cannot disagree with itself across
shapes. The wasted work is bounded by how far outside the box the caller
drew, which for a widget whose whole job is to hold a drawing is small.

## The geometry module is shared source compiled twice, like the calculator engine

`build/userland/shared/geom.o` and `fixed.o` are `kernel/lib/geom.c` and
`fixed.c` built a second time with the userland flags. Same reasoning as
[Calculator's engine](#calculators-engine-is-shared-source-compiled-twice-not-copied),
and worth restating because this is now the established pattern rather
than a one-off: the objects genuinely cannot be shared (`-mcmodel=kernel`
vs `-mcmodel=large`), but the SOURCE can, and a second hand-written copy
of a rasteriser would drift from the first. When it drifted, the symptom
would be a ring-3 app drawing a slightly different circle from the
kernel -- a rendering difference with no obvious cause and no failing
test.

## stderr goes to the kernel log, and is never redirected into a pipe

`SYS_WRITE` treats fd 1 and fd 2 differently: fd 1 honours
`SYS_SPAWN`'s stdout redirection (into a pipe, so a parent can read a
child's output), fd 2 always goes to `klog_putc()` -- the serial console
and `dmesg`.

They used to be identical, which was a bug with two faces. Redirecting
stdout is a request to capture a program's OUTPUT; folding its
diagnostics into the same stream corrupts whatever the parent was
parsing, which is the precise problem Unix has two descriptors to
avoid. And a GUI client has no terminal at all, so its `sys_print()`
went to whatever sink the console happened to have -- which is how this
was found: `tools/gfxdemo_test.py` could not see a single line the demo
logged, because there was nowhere for a windowed ring-3 process to say
anything.

The practical rule for app code: `sys_print()` for output, `sys_eprint()`
for anything diagnostic. The second is readable regardless of who
spawned the process or where its stdout went, which also makes it the
channel a test tool asserts on -- the same path `strace` output takes.

## A Start-menu entry can be a launcher for a ring-3 program

`struct gui_app` (apps/gui_apps.h) grew one field, `exec_path`. When
it is non-NULL the entry is not an app at all -- it names a binary in
`/bin`, and `open_app()` spawns it instead of creating a window.

The registry was the only way into the Start menu and the desktop, and
it could only describe apps implemented as kernel callbacks. That was
fine until Calculator, Notepad and Terminal moved to ring 3, at which
point the desktop could not launch any of them: they were reachable
only by typing `run calculator` in a Terminal, while the Start menu
went on opening the kernel-space versions. Nothing was broken and the
Task Manager was correctly reporting `[r0]` -- it just looked exactly
like a bug, which is its own kind of defect.

Two things about the design worth keeping:

**A launcher always spawns; it never focuses an existing window.**
Single-instance behaviour (`multi_instance == 0`) works by finding a
window whose `app` pointer matches, and a ring-3 client window has no
`gui_app` at all (wm_client.c sets it to 0) -- so the WM *cannot*
enforce it here without matching on titles, which is guesswork. More
importantly it shouldn't: whether a second copy of a program may run
is the program's own decision on any real system, and a launcher
launches.

**It deliberately does not use `window_start_process()`.** That slot
exists to deliver a process's exit to a specific window's
`on_process_exit` callback, and it is WM-global -- one tracked process
at a time. A launched client has no such callback, and the window
server already tears its window down when it dies, so routing launches
through it would cap the desktop at one ring-3 app for no benefit.

## Ring-3 GUI apps live in /bin, not /tests

Calculator, Notepad, Terminal (`uterm`) and Shapes are seeded to
`/bin`. They were in `/tests` for most of the ring-3 migration, purely
because that is where the first client landed and nobody moved them
once they stopped being experiments.

`docs/filesystem-layout.md` draws the line clearly -- `/tests` holds
"test/demo binaries, one kernel mechanism each... not things a user of
the OS wants offered to them" -- and a program offered in the Start
menu is user-facing by definition. The mechanism tests that remain in
`/tests` (`winclient`, `uiclient`, `pipe_test`, `spin_test`, ...) are
still exactly what that directory describes.

Moving them needed the cleanup that doc mandates: `sync` is additive
and never deletes, so the old `/tests` copies had to be removed from
the existing image explicitly (`tools/tfs3_writer.py delete`). Skipping
that leaves stale binaries frozen at their last-synced content forever.

## The default font size is a one-line change, and that is the point

`kernel/drivers/gfx.c`'s `cur_font_size` has moved three times now
(16x32 -> 11x22 -> FONT_SIZE_18 -> FONT_SIZE_14), each time because
there was more real UI on screen than the previous default had been
chosen against.

It stays a one-liner because every layout in the system is
font-DERIVED, not font-assuming: window sizes come from each app's
`default_size()` (called at open time with the active font), chrome
heights from `gfx_char_h()`, the desktop's column pitch from
`gfx_char_w()`, the Start menu's row height and origin from both. The
14pt change was verified by running the full GUI regression suite --
82 checks across 7 tools -- unchanged, and all of it passed.

The exception is worth knowing, because it has now cost three
re-measurements: HARDCODED PIXEL CONSTANTS IN TEST TOOLS do not
reflow. `tools/gui_flow.py`'s `TASKBAR_H`/`ITEM_H` are calibrated
numbers, and a stale one doesn't fail loudly -- it clicks the wrong
row. That is the standing argument for
`DebugConsole.menu_row(label)`/`gui menu --json`, which ask the kernel
where things actually are, over any constant a tool writes down.

## The desktop icon grid wraps, and clips its labels

Two related decisions, both forced by the registry growing from seven
entries to eleven.

**The default layout wraps into a new column at the bottom edge**
rather than being one unbounded column (`icon_row[i] = i`). An icon
past the bottom of the screen is not just invisible, it is
unclickable -- an app can be in the registry and unreachable from the
desktop, with nothing on screen to indicate why.

**Labels are clipped to the column pitch, with a ".." marker.** The
pitch is fixed (not sized to the longest label present -- one long
name would otherwise widen every column on the desktop, a tradeoff
already rejected once), and it is sized for a 13-character label:
"Control Panel" and "Task Manager", the longest that should never be
cut. Longer ones are truncated rather than allowed to overflow.

Overflow used to be an accepted cosmetic quirk, and honestly was one:
with a single column, a long label ran off to the right over empty
background and stayed perfectly readable. The moment a second column
existed it landed on that column's labels instead and made both
unreadable. Note that clipping ALONE would not have fixed it -- at the
old 76px square-cell pitch only about seven characters fit, so
"Calculator" and "Calculator (ring 3)" both became "Calcu..". The
pitch had to widen too. `gfx_draw_string_clipped()` is what does the
cutting, per `docs/gui-guidelines.md`'s first rule.

## A client's menus are clamped to its own window, and that is one rectangle away from not being

`userland/ui/uui_menubar.c`; landed with the menu bar, see
`CHANGELOG.md`.

On Windows, a popped-up menu is a real `HWND` of the built-in `#32768`
class, positioned in SCREEN coordinates and constrained against the
monitor work area -- it can sit anywhere on the desktop, far outside the
window that owns it, and a menu taller than the screen grows scroll
arrows. On KDE it is a `Qt::Popup` toplevel, which under Wayland is
literally an `xdg_popup`: a child surface with a positioner (anchor
rect, gravity, and `constraint_adjustment` flags -- flip_x, flip_y,
slide_x, slide_y, resize) that the COMPOSITOR resolves, plus an implicit
grab. Every submenu is another such surface.

A TWP client can do none of that. It draws into its own window buffer
and has no mapping of the framebuffer or of any other window, which is
enforced rather than asked for (`docs/gui-guidelines.md`). So the menu
resolves the same vocabulary -- flip to the other side, slide along the
other axis, clamp last -- against a BOUNDS RECTANGLE the caller passes
in (`uui_menubar_set_bounds`), and Notepad passes its content rect.

The point of writing it that way rather than hardcoding the window: that
rectangle is the entire difference. When TWP grows a popup surface
(`docs/roadmap.md`, M41's `WIN_REQ_POPUP`) the widget is handed the
screen rect instead and the placement code is already correct -- one
rect, not a rewrite. The divergence is visible only on a window small
enough that a menu would have overflowed it, which is why shipping the
widget first and the protocol second was the phasing chosen rather than
building both at once.

## A menu bar opens on press, which is the one place the commit-on-release rule bends

`docs/gui-guidelines.md` says a control must not act until the button is
released over it, and `uui_menubar` opens a menu on button-DOWN anyway.
That is deliberate and it is what Windows, KDE, GTK and macOS all do:
pressing a title shows its menu at once, and you may then either release
and click an item or keep the button held, slide down, and release over
the item you want. Requiring a full click to open would break the second
gesture entirely.

The half of the rule that matters is kept -- the ITEM still commits on
release, so pressing "Exit" and dragging off it does nothing. Opening a
menu is not an action; it is showing the actions.

Worth knowing when testing it: that cancel path is the ONLY check in
`tools/menubar_test.py` that can tell a correct menu from one that
commits on press. Every other check presses and releases in the same
place, so a commit-on-press build passes all of them. Confirmed by
building exactly that and watching one check go red.

## Esc doesn't close a window; Alt+F4 does, and it is a WM shortcut rather than an app key

`apps/wm/wm.c`'s key loop and `wm_request_close()`; landed with the
menu bar's follow-up, see `CHANGELOG.md`.

Six ring-3 apps used to quit on Esc, which was always a papercut and
became a real hazard once Esc was also the key that closes a menu: one
stray press in Notepad with no menu open discarded unsaved text. Neither
Windows nor KDE closes a window on Esc. It is now app-local everywhere --
cancel a dialog, close a menu, clear a selection -- and closes nothing.

**Alt+F4 is handled by the window manager, not delivered to the focused
app.** That is what both desktops actually do: Windows routes it through
`DefWindowProc` to `WM_SYSCOMMAND`/`SC_CLOSE`, and KDE's is a KWin
global shortcut, so in neither case does the application see the
keystroke. The alternative -- deliver the key and let each app call
`uapp_quit()` -- was rejected for three reasons: every app would have to
implement it or the shortcut would silently do nothing, each
kernel-space app would need its own copy, and an app stuck in a bad
state could never be closed from the keyboard, which is precisely the
case the shortcut exists for.

**The app still decides what happens**, because the WM ASKS rather than
tears down: `wm_request_close()` sends `WIN_EV_CLOSE` to a client, and
`uapp_desc.on_close` returning 0 refuses. Alt+F4 and the title bar's X
are indistinguishable to an app -- exactly as they are on Windows, where
both arrive as `WM_CLOSE` -- so no client needed editing to gain the
shortcut, and none can tell the two apart in order to behave
inconsistently between them. A `reason` field on the close event was
considered and dropped: neither model desktop distinguishes these at the
app level, and nothing in the tree wanted it.

**Why all three closes share one function.** They did not, and the odd
one out was broken: the context menu's Close called `close_window()`
directly, destroying a ring-3 window without ever sending
`WIN_EV_CLOSE`. It survived because no test could open a context menu.
`gui rclick` and `gui ctxmenu` exist now for that reason, and
`winclient` refuses its first two close requests so each route is
measured rather than assumed.

**Encoding**: `KEY_F4` + `KEY_MOD_ALT`, not a combined `KEY_ALT_F4`
code. The Shift+arrow family got discrete codes because the terminal
encoding genuinely cannot express them; a function key has no such
problem -- nothing is folded, the modifier bits ride alongside it
already, and matching on them generalises to any future Alt+F<n>. The
driver returns on the function-key check before the Alt-prefixes-with-
ESC path, so Alt+F4 arrives as one key with the modifier set rather than
as ESC followed by something.

**Still not solved: an app that ignores the request keeps its window.**
That is the honest consequence of a polite handshake, and force-quitting
needs a not-responding timeout plus a way to kill the process. See
`docs/roadmap.md`, Milestone 41.

## Not-responding is a PING, not a close timeout -- because "refused" and "wedged" look identical to a timer

`abi/win_proto.h`'s `WIN_EV_PING`/`WIN_REQ_PONG`, `apps/wm/wm_client.c`'s
liveness section, `scheduler_kill()`. See `CHANGELOG.md`.

The obvious build is a timer: send `WIN_EV_CLOSE`, and if no
`WIN_REQ_DESTROY` arrives within N seconds, offer to force-quit. It is
smaller, needs no protocol change, and is wrong.

A client is entitled to refuse a close -- `uapp_desc.on_close` returning
0 is a documented, tested part of the toolkit, and `winclient` does
exactly that. To a timer, a client that declined and a client that is
wedged are the same observation: the window is still there N seconds
later. Acting on that means either offering to kill apps that
deliberately said no, or not offering it for apps that are genuinely
hung. There is no threshold that separates them, because the thing that
separates them was never measured.

So the server asks a question only a running event loop can answer.
`WIN_EV_PING` carries a serial; `WIN_REQ_PONG` echoes it. That is
xdg_shell's ping/pong and ICCCM's `_NET_WM_PING`, adopted for the same
reason both exist: from outside, a wedged client and an idle one are
indistinguishable -- neither draws, neither sends -- so the only honest
thing a compositor can otherwise say about an unresponsive window is
nothing.

Three details worth keeping:

**The toolkit answers, not the app.** `uapp_run()` handles the ping with
no callback and no app involvement. A check an app could forget to
answer would report every un-updated app as hung; and answering from the
event loop is precisely the right test, because an app stuck inside its
own `on_draw` never reaches that line.

**The serial is load-bearing.** Without it, a late pong from a previous
ping clears the current one -- so an app answering one round behind, the
exact behaviour of a badly overloaded app, always looks healthy.

**A hung window nobody is touching gets the title-bar mark and nothing
more.** The dialog is modal and appears over whatever the user is doing;
raising one unprompted, for a window they never interacted with, is
worse than the hang. It is offered only when they have actually asked
the window to close.

Force Quit kills the PROCESS (`scheduler_kill()`), not just the window.
Dropping the window alone would leave a process drawing into a buffer
that is no longer mapped, which is the exact hazard the close handshake
exists to avoid -- and would leave the scheduler slot held, which on a
4-slot table is four rescues before the desktop stops launching
anything.

Still not built: a general "this app is hung" indication outside a close
attempt (the ping is only sent when the WM asks a window to close, so
that is the only time the mark can appear), and any way to recover a
client that is hung but has NOT been asked to close.

## Heap debug mode is a runtime toggle, and `kfree()` tells the two block shapes apart by a magic that cannot be a pointer

Red-zones and use-after-free poisoning (`heap debug on`) could have been
a build flag -- `-DHEAP_DEBUG`, zero cost when off, no per-block
bookkeeping. They are a RUNTIME switch instead, for two reasons: the
mechanism is then reachable in a booted OS without producing a second
image, and one build exercises both states, so `make test` and CI cannot
silently cover only the half the Makefile happened to pick. This repo's
standing rule that a fallback nothing can reach is a guess applies to a
debug facility as much as to a driver path.

The cost is that blocks allocated before and after a toggle coexist, and
`kfree()` gets only a pointer. Its layouts are:

    off: [header][............ payload ............]
    on:  [header][span][MAGIC][ payload ][MAGIC][MAGIC]
                              ^-- what the caller holds

so it decides by reading the eight bytes immediately before the payload:
`HEAP_RZ_MAGIC` in a red-zoned block, the header's `prev` pointer in a
plain one. **That is sound rather than a heuristic, and the reason is
worth keeping**: every heap pointer is an address in the identity-mapped
low 4 GiB and therefore fits in 32 bits, while the magic's top half is
nonzero, so no `prev` can ever collide with it. Break either fact -- a
heap above 4 GiB, or a magic that fits in 32 bits -- and the two cases
become indistinguishable on the freeing path, silently.

`prev` being the header's LAST field is load-bearing for the same
reason, which is why it carries a comment saying so.

Two smaller decisions inside it:

**A detected violation quarantines the block; it does not panic.** A
real kernel panics on a corrupt heap, and that is defensible -- but a
KTEST cannot then assert that detection works, so the mechanism would
only ever be proven by a manual crash. Reporting to the log and leaking
the block keeps it testable, and leaking is the right disposal anyway:
the block's metadata is exactly what proved untrustworthy, so returning
it to the free list hands the damage to the next allocation. Quarantined
bytes are counted in neither the used nor the free total, so those two
stop summing to `heap_total_bytes()` once any violation has happened --
stated in `heap.h` rather than left to be discovered.

**`kfree()`'s plain path checks the header, always, debug mode or not.**
An underflow of 1..8 bytes lands on the magic, which makes a red-zoned
block look plain -- and `kfree()` would then take its header from 16
bytes inside the real one and unlink whatever it found there. The guard
is a `HEAP_HDR_MAGIC` field sitting in four bytes of padding the
compiler was already inserting after `free`, so it costs nothing.

`heap_check()` exists because a use-after-free is otherwise only caught
by whatever allocation happens to reuse the block -- possibly a thousand
allocations later, in an unrelated subsystem, or never.

## The kernel's relocation table is placed after `.data`, and the image that is VERIFIED is not the image that ships

Kernel ASLR (`docs/roadmap.md` M2) needs the kernel to know every
ABSOLUTE reference in its own image, so it can adjust them if the image
moves. `tools/genrelocs.py` extracts those from a `ld --emit-relocs`
link and emits a table the kernel carries; `kernel/arch/x86_64/reloc.c`
applies it. That is Linux's `CONFIG_RELOCATABLE` shape -- a build-time
relocs tool, not a PIE link -- and it works here because the low 4 GiB
is identity-mapped, so VA == PA and only the ~7,300 absolute references
need help while the ~11,900 PC-relative ones survive a move untouched.

The obvious problem is circular: generating the table needs a linked
image, and linking the table in changes the image. **The fix is
placement, not a fixed point.** `linker.ld` puts `.krelocs` after
`.data` and before `.bss`, below every section that can contain a fixup
location -- so adding the table cannot move a single address the table
records, and pass 1's entries stay correct in the pass 2 image that
contains them. Moving it above `.data` instead makes exactly 8 entries
describe the image it displaced; `genrelocs.py --verify` reports that as
a same-size, different-content mismatch and names the cause.

It must also stay before `.bss`: `.bss` is NOBITS, so an allocated
section after it would force the file to materialise gfx's 13 MB back
buffer as real bytes on disk.

**The second decision cost a non-booting kernel to find.** The final
link also uses `--emit-relocs`, because `--verify` has to re-derive the
table from the FINAL image -- checking it against pass 1 would only
compare pass 1 to itself. But an image carrying its `.rela` sections is
2 MB larger and GRUB will not boot it: the symptom is a completely empty
serial log, with no kernel output at all, which reads like a code bug
and is a link one. So the build links `kernel.pass2.elf` with `-q`,
verifies THAT, and ships `objcopy --remove-section='.rela.*'` of it.
The verified artifact and the shipped artifact are deliberately
different files, differing only in sections that are never loaded.

A third, smaller trap in the same rule: `build/krelocs.c` is named as a
prerequisite of the kernel and marked `.PRECIOUS`, because make
otherwise classifies it as an intermediate file and DELETES it once the
`.o` is built -- after which the next build's `--verify` fails with a
FileNotFoundError on a path that looks obviously correct.

### What the table's tests can and cannot prove

`kernel_relocate(0)` runs on every boot, before `paging_enforce_wx()`
(the fixups write into `.text`, which that call makes read-only with
CR0.WP, after which every one of them is a ring-0 page fault). A delta
of zero patches nothing, so what it actually proves is that the table
describes ~7,300 real, mapped, writable words in this image.

`kernel_reloc_implausible()` checks that each entry points at a word
holding a reference into the image -- but only for the READ-ONLY part,
and that limit is the interesting half. A fixup in `.data` is a pointer
the kernel initialised and is then free to reassign, to a `kmalloc`'d
block or the framebuffer, so by the time a KTEST runs a healthy `.data`
entry routinely points outside the image. Checking it anyway reports a
working kernel as corrupt, which is how the function was first written.

The upper bound also carries a page of slack, because a relocation's
value is symbol + ADDEND: `pmm.c`'s `reserve_range(0, _kernel_end)`
constant-folds its page round-up into the relocation, so the image
genuinely contains one legitimate absolute reference to
`_kernel_end + 0xfff`. Exactly one entry needs it, which is why the
bound is a measured constant rather than a guess.

Since stage 3 landed, all of this runs against an image that HAS moved,
so the checks now offset every location by the delta -- reading the bare
link-time address reaches into the abandoned image, which is still
mapped and still holds pre-relocation values, so it does not fault. It
just answers questions about a kernel that is no longer running.

What no test inside a running kernel can do is prove the relocation
itself: it cannot move the image out from under itself. That claim is
carried by the whole suite passing on a randomized base instead --
every headless run picks a different one, so 175 KTESTs, the ring-3
diagnostics, the fault tests and the 13 GUI tools are collectively an
assertion that a relocated kernel works, across many bases rather than
one.

### CR3 *does* have to be repointed, for a reason that has nothing to do with mapping

Stage 2 concluded this step was unnecessary and was **wrong**, so the
reasoning is worth stating carefully rather than merely corrected.

The original argument was sound as far as it went: `boot.asm`'s
`p4_table`/`p3_table`/`p2_tables` identity-map the entire low 4 GiB, so
they already map wherever the image lands, and the old tables stay
reserved. Nothing about MAPPING requires a change.

What it missed is that `paging.c` reaches those tables by **linker
symbol** -- `extern uint64_t p2_tables[2048]`. After relocation that
symbol names the COPIED table, so `paging_enforce_wx()` and
`vmm_map_user_page()` write into a table the CPU is not walking. There
is no fault and no error: W^X simply never takes effect, and user
mappings land somewhere nobody reads.

So stage 3 repoints CR3 at the copied tables and rewrites the two
levels of internal pointers to match. The p2 entries need nothing --
they are pure identity mappings, whose values do not depend on where
the table itself lives. Every address in that code is computed as
`symbol + delta`, because it runs from the OLD image where the bare
symbols still name the old tables; writing through them would rewrite
the tables being abandoned and reload CR3 with the value it already
held, which looks exactly like a working call.

**The general lesson is about the test, not the code.** All six W^X
KTESTs stay GREEN with this step disabled, because they read
`p2_tables` through the same symbol `enforce_wx()` wrote -- test and
code agree with each other while the hardware walks something else
entirely. The check that catches it compares CR3 against the symbol,
i.e. asks the CPU rather than the program. When a subsystem is reached
through an indirection, at least one test has to bypass that
indirection, or the whole suite can be self-consistently wrong.

### Why a lower base is refused, and why delta zero cannot exercise the copy

The relocated base is always ABOVE the link base. `pmm.c` reserves the
old image and the new one as two SEPARATE ranges -- separate rather
than one span because on a randomized base the gap between them is most
of RAM -- and the abandoned image has to stay reserved because it is
still live: the CPU uses the GDT inside it until `gdt_init()` replaces
it. A base below the link address would put the old image above the new
one, outside anything the reservation covers, and hand those frames to
the allocator. Being above also makes the copy non-overlapping, so a
forward `k_memcpy` is correct.

`.bss` is COPIED rather than zeroed, which is what lets the caller
carry on: the relocated stack already holds the live frame byte for
byte, so adding the delta to `rsp` lands on the same position within it.

Delta zero is degenerate for the copy, which is why stage 2 could not
exercise it and did not pretend to: at zero the destination is the
source, so copying or zeroing `.bss` would wipe the live stack and the
page tables the CPU is currently walking.

### The base's entropy is bounded by RAM, and cannot come from the normal RNG

`krandom_init()` cannot be called this early. It harvests jitter by
spinning until `pit_ticks()` changes, and the PIT is not initialised
yet -- so on a machine without RDSEED/RDRAND, which is QEMU's default
`qemu64` and therefore most test runs, it would spin forever. The base
gets its own minimal source instead: RDSEED, then RDRAND, then the
timestamp counter, and it REPORTS which one it got rather than letting
a reader assume the base is unpredictable.

The TSC fallback was measured rather than assumed, per the same rule
the entropy source itself was held to: five consecutive boots under TCG
produced five different bases.

The honest entropy figure is the number of candidate bases, not the
width of the random draw: 114 on a 256 MB guest, about 6.8 bits, which
matches the ~6.5 bits predicted when this was scoped. It is bounded by
RAM and by the image being ~14.5 MB in memory (gfx's 13 MB back buffer
in `.bss`), not by the random source. Linux's x86 physical KASLR gets
about 9 bits.

The relocation also cannot log -- `klog_write()` goes straight out the
serial port and `serial_init()` has not run -- so every decision it
makes is recorded in a global and printed by `kernel_main()` once it
can. And every one of those globals must be assigned BEFORE the copy,
because the copy is what carries them into the image that will actually
run; assigning after it writes only to the abandoned image, and the
running kernel would report a delta of zero while sitting at a
relocated address.

`nokaslr` on the GRUB command line turns it off -- the same spelling
Linux uses, and the recovery path if a machine turns out not to survive
being relocated.

## The repo lives in an organization because a personal repo has no read-only collaborator

Wanting to show the code to one person without giving them write access
turns out to be impossible on a personal-account repository. **Every
collaborator on a personal repo gets write.** The Read/Triage/Write/
Maintain/Admin roles exist only for repos owned by an ORGANIZATION, and
paying for GitHub Pro does not change it -- it is a property of personal
repos, not of the plan.

The usual workaround is closed off too: branch protection and rulesets
both return `Upgrade to GitHub Pro or make this repository public` on a
free private repo. Verified by calling the API, before and after the
move -- **a free ORG does not unlock it either**, which contradicts a
guess made mid-session and is why it is written down here.

So on 2026-08-16 the repo moved from a personal account to the
`eveningworks` organization, where an **outside collaborator** can be
added to one repository with the `Read` role. Outside collaborator
rather than org member on purpose: an org member can see the
organization's other repositories and its member list, which defeats the
point once the org holds more than this project.

Two consequences worth knowing:

- **The remote changed** to `git@github.com:eveningworks/toy-os.git`.
  GitHub redirects the old path, but the local remote was repointed
  rather than left depending on a redirect that dies the moment someone
  claims the old name.
- **Release assets survive a transfer**, as do tags, Actions history and
  pending invitations. Only repository *settings* that are plan-gated
  change meaning.

## The account rename, and the second history scrub -- scoped by measuring, not by instinct

The maintainer's GitHub handle changed after the transfer. Two facts
made that safe, and both were checked rather than assumed: the numeric
account ID is unchanged, so GitHub re-resolves every release author and
commit attribution to the new name; and no commit is *authored* by the
GitHub account at all (authors are all `toy-os <noreply@toy-os.local>`,
per the earlier scrub -- see the entry above).

What did carry the old handle was **committer** metadata on nine
web-UI merge commits (`<id>+<handle>@users.noreply.github.com`), one
line in a frozen changelog archive, and one commit message.

**The scope of the fix was decided by measuring, and the first estimate
was wrong by 4x.** Rewriting only the committer fields touches 60
commits and one tag, because the earliest affected commit postdates
`v0.1.0`. Scrubbing the string from historical FILE CONTENTS as well
reaches back to a commit that predates `v0.0.9` -- 264 commits and all
three tags. That difference was found by asking `git log -S` where the
string was introduced, *after* the smaller number had already been
quoted and a decision made on it. The decision was re-put with the real
number, and the narrower scope chosen.

So: committer metadata and the current tree are clean; old revisions of
`CHANGELOG-archive.md` still contain the handle if someone checks out a
months-old commit. That was judged acceptable because a GitHub handle is
public by nature -- unlike the real name the first scrub removed, where
the wider blast radius was worth paying.

The mechanics, if this comes up again: `git filter-branch` in a FRESH
CLONE (never the working checkout, and never a repo with worktrees
attached), `--tag-name-filter cat` so tags follow, then verify with
`git diff <old-tip> <new-tip> --stat` -- which must show only the
intended content change and nothing else. `git filter-repo` is the
modern tool but is not installed here; filter-branch is built in and was
sufficient. Take a backup first (`tools/backup_repo.sh`), because a
force-push over a rewritten history is the one operation where the old
objects stop being reachable.

## A backup of this repo is not a `git clone`

`tools/backup_repo.sh` exists because the obvious backup is incomplete
in a way that only shows up when you need it. A mirror clone captures
every commit, branch and tag -- and none of the **release assets**,
which are ~130 MB of ISOs and pre-seeded disk images that live only on
GitHub. Rebuilding a historical release's assets by hand means checking
out that tag and reproducing the exact build, which is precisely the
situation a backup is supposed to avoid.

It also captures what a clone cannot: the repo's own settings, the pull
request bodies, and a bundle of LOCAL refs, since a mirror of the remote
cannot know about a branch that was never pushed.

Two things it deliberately does not do. It does not capture
collaborators, webhooks, secrets or Actions history -- those are
GitHub-side state with no export, and pretending otherwise would be
worse than saying so. And it does not run the restore test, because that
costs a full build: clone the mirror and run `make all` by hand before
relying on a backup for anything irreversible. That test is worth the
minutes -- matching hashes prove the bytes survived, but only a build
proves it restores to a working project.


## The ring-3 UI Demo selects on contact where the kernel one committed on release

Milestone 41's stage 0 moved UI Demo to ring 3, and with it the 28
checks that are most of what proves the widget set works. Three of those
checks changed meaning, and the reason is worth stating once so a future
session does not "fix" the app back:

**A listbox row and a dropdown item act on CONTACT** in `userland/ui/`,
where the kernel widgets armed on press and committed on release. That
is not a regression in the port -- it is the ring-3 widgets' existing
contract, already shared by four shipping apps, and it is what Windows
and GTK do with a list. `docs/gui-guidelines.md`'s commit-on-release
rule is about controls whose action is NOT already visible: a button
fires something invisible, so a press dragged away from it must do
nothing, and UI Demo still asserts exactly that for its buttons. A
selection is visible as it happens, and a user who presses the wrong row
sees the wrong row highlighted rather than triggering something.

**A closed dropdown takes only the keys that OPEN it** (Down, Enter,
Space), where the kernel one cycled its value with the popup shut. The
ring-3 behaviour is the better one and was kept deliberately: an arrow
key cannot change a setting the user cannot see.

**The button group is not a focus stop.** Ring 3's group has no
keyboard activation, so a tab stop there would be a stop that does
nothing. The tab ring is textbox, dropdown, listbox.

What the port did NOT change is the layout, the widget list, or the log
grammar -- so the other 25 checks are the same assertions against
different code, which is the point of having moved the target.

## A GUI test spawns its client directly, instead of typing at a Terminal

Eight of the thirteen GUI test tools used to open the kernel-space
Terminal and type `run <name>` at it to get a ring-3 client on screen.
Stage 0 deleted that Terminal, and the failure was not subtle: `gui open
Terminal` now spawns the RING-3 terminal, whose window does not exist
when the injected keys arrive, so ten tools failed at their first check
at once.

They use `gui spawn` now, via `DebugConsole.spawn(path, title)`, which
waits for the window and gives the client a moment to present its first
frame. That was already the documented advice -- `wm_debug.c` added `gui
spawn` precisely so a test would stop dragging a Terminal's allowlist,
its single pending-process slot and its shell into a test about
something else -- and stage 0 is simply what forced it.

Two traps the conversion exposed, both of which read as widget bugs:

- **A window in the WM's list is not a window with pixels.** The
  Terminal-typing path had enough incidental latency to hide this;
  spawning directly does not. A capture taken too early reads DESKTOP
  through the window's rect, which scored as ~230k "ink" and made every
  later comparison meaningless. Wait for the app's own layout line where
  it has one.
- **A klog line can land in the MIDDLE of a `--json` reply.** The
  console has no per-writer buffering, so a process exiting while `gui
  windows --json` is printing splices its message through the object.
  `DebugConsole.json()` re-asks rather than trying to unpick the
  fragment: the splice is a collision, not a property of the answer, and
  a repair heuristic would silently accept genuinely malformed output.


## A compositor's view of a window is at a DERIVED address, and revocation is the feature

Milestone 41's stage 1 lets one process's window buffer be mapped into
another's address space, so a ring-3 window manager can composite
windows it does not own. Two things about its shape are worth stating
because both look like details and neither is.

**The address is derived, not returned.**
`win_compositor_vaddr(owner_pid, window)` is a formula, exactly as
`win_buffer_vaddr(window)` already was for a client's own buffer. The
map call reports the address anyway, so the value has one definition at
the call site rather than two -- but the compositor could compute it.

Three things fall out of that, and the third is the reason:

  1. A resize reallocates a window's frames and re-maps them at the
     SAME address, so a compositor's pointer survives a resize it did
     not initiate and never has to be told the pixels moved. This is
     the trick that made client-side resize simple, reused.
  2. There is no per-mapping table to keep in sync, so the two sides
     cannot disagree about where a window is.
  3. **The kernel can revoke a mapping without being told where it is.**
     That is what makes "the mapping is gone after the client died" a
     checkable property rather than a claim resting on bookkeeping.

**Mapping is three lines; revocation is the whole design.** Frames stop
belonging to a window on four separate paths -- an explicit destroy,
the client dying (a different code path: process teardown calls
`win_server_client_gone()`), a resize that reallocates, and the
compositor itself unregistering or exiting -- and every one of them has
to unmap BEFORE the frames go back to the allocator. Miss one and the
compositor reads memory that now belongs to something else, which
appears as flickering garbage inside one window and gets diagnosed as a
drawing bug for as long as that takes.

That is why the KTESTs assert against `vmm_validate_user_range()` --
the page tables -- rather than against the server's own `comp_mapped`
flag. The flag is precisely the thing that would be wrong. The recorded
positive control (disable revocation on destroy; exactly two checks go
red, on the page-table assertion) is in
`kernel/proc/win_server_test.c`.

**Why the compositor's address space is captured at registration**
rather than looked up per call: a mapping must not depend on which
process happens to be current when the request arrives, since a batched
or shared-ring transport breaks that assumption -- the same argument
`win_server_ops` makes about taking `pid` explicitly. It also makes the
path reachable from a KTEST, which has no processes to look up, and
that is the only reason these properties are tested at all.


## A dev build shows its commit; a release shows only its version

`VERSION` changes about twice a milestone, so for the hundreds of builds
in between, "0.3.0-dev" identified nothing: two ISOs weeks apart carried
the same string. `tools/gen_version.sh` now also embeds
`TOYOS_BUILD_ID` -- `git rev-parse --short HEAD`, plus `-dirty` when the
working tree did not match it -- and `TOYOS_VERSION_FULL`, which is what
the `about` command, the Control Panel and the About window display.

The display rule, and why each half:

- **`0.3.0-dev` shows the commit** (`0.3.0-dev (2034bb1)`). A dev build
  is pinned by nothing else, which is the entire problem being solved.
- **A release shows the bare number** (`0.3.0`), dirty tree or not. It
  is already pinned by its git tag, so the hash is noise on the one
  build where it is not needed.
- **`dirty` appears on a DEV build only.** A build from a tree with
  uncommitted changes came from source that exists nowhere in history,
  so the hash it prints is a lie without the marker -- but that matters
  to whoever is BUILDING, not to whoever is running. "Dirty" is jargon
  about a repository the user does not have.

  The dirty-RELEASE case is therefore a **build-time warning** rather
  than a display string: `gen_version.sh` shouts on stderr when
  `VERSION` has no `-dev` and the tree is dirty, where the person who
  can still act on it will see it, and the shipped string stays clean.
  The first version of this printed `0.3.0 (dirty)` to the user; that
  was simply the wrong audience for the message.

**The trap this had to avoid**, and it is the reason a build TIMESTAMP
is not in there: `gen_version.sh` only rewrites `version.h` when the
content actually changed, because `kapi.h` includes it and an
unconditional rewrite makes nearly every object in the tree look stale
on every build. A commit id changes once per commit and the dirty
marker at most twice per session, so the property holds. A timestamp
would differ every single build and quietly turn every build into a
full rebuild.

`unknown` rather than an empty string when git is unavailable (a release
tarball, a stripped checkout): an empty marker reads as a bug in the
script, and a build that cannot say where it came from should say so.


## The third inert scrollbar: drawing one and handling it are separate jobs

`uui_listbox` drew a scrollbar and handled none of its input. Not
"handled it badly" -- a press on the strip did not reach the widget at
all, because `uui_listbox_hit()` deliberately excludes the bar column so
that clicking it cannot select a row. Dragging the thumb did nothing,
clicking the track did nothing, and inside a dropdown popup a click on
the bar fell through to the dismiss branch and CLOSED the popup, which
is the most annoying possible answer to "I tried to scroll".

Found by the maintainer dragging it on the desktop, after a 28-check
suite passed clean. That is the third time this project has shipped a
bar that draws and does nothing (`apps/ui/ui_textview.h` records the
first two), and the recurrence is the interesting part, so:

**Why it keeps happening.** `uui_scrollbar` is deliberately a stateless
drawing-and-hit-testing primitive -- correct design, and the reason
`ui_textview` and `uui_textview` can compose it. But it means DRAWING a
bar is one call and MAKING IT WORK is a separate set of them, and a
control that does the first and not the second looks finished. Every
screenshot is right. Every "does it respond" check passes, because the
rest of the control responds.

**What actually stops it.** `docs/gui-guidelines.md`'s scrollbar section
gained a point 9: a control that draws a bar must handle one, and the
check has to assert the view MOVED. An absence check cannot catch this
-- "the drag changed no selection" passes perfectly against a dead bar,
and it stayed green under the positive control while the four real
checks went red. That is the general lesson worth keeping: when a
control does nothing, every assertion of the form "X did not happen"
still passes.

The fix put the behaviour in the widget (`uui_listbox_press()` /
`_drag()` / `_drag_end()`), not in UI Demo -- behaviour belongs to the
component, so the four shipping apps that use a listbox and every
dropdown popup get it too, rather than each app growing its own copy
and one of them getting the grab offset wrong again.


## The toolkit routes pointer input; an app configures and is told what changed

Every ring-3 app used to dispatch mouse input by hand: try the dropdown,
then the listbox, then each button; remember whether a button is down so
motion means "drag"; remember to call drag_end on release. The rule this
violated is this project's own -- behaviour belongs to the component
(`docs/gui-guidelines.md`) -- and input handling is behaviour.

The measurement that settled it, taken before any code changed:

| app | widget-input forwarding calls |
|---|---|
| Calculator | 0 |
| Terminal, winclient, uiclient | 0 |
| Shapes | 1 |
| Notepad | 3 |
| UI Demo | **21** |

Calculator wrote NONE because `uapp_desc.buttons` already routed one
widget type for it. So the model was proven and simply stopped at button
groups; UI Demo paid twenty-one calls for using anything else.

**The cost was never verbosity, it was silence.** A widget whose input
an app forgot to forward is not a compile error and not a visible
defect: it draws correctly and does nothing. That is exactly how
`uui_listbox` shipped a scrollbar that could not be dragged.

**What replaced it.** `uui_widget_ops` gained `press`/`motion`/
`release`/`wheel`, `uui_route.h` walks the same `struct uui_item` array
a layout already holds, and `uapp` calls it before the app's own
callbacks. An app declares widgets with ids and gets
`on_widget(app, id, reason)`; it reads the new value from the widget
(`uui_dropdown_selected()`, `cb.checked`, `list.selected`). UI Demo went
from 21 forwarding calls to none -- what is left there is hover
REPORTING for the tests, which is the app's own business.

Three decisions inside it worth keeping:

- **The pointer GRAB.** Whoever consumes a press gets every motion and
  the release, wherever the cursor goes. One rule, and it removes all
  the per-app drag bookkeeping: a thumb drag that leaves the scrollbar
  keeps scrolling, and a button dragged off still receives its release
  and so can decline to commit.
- **`overlay_active`.** An open dropdown popup is drawn outside its own
  rect, so `hit` cannot route it. A widget declaring an overlay is
  offered every press first -- input order being the reverse of draw
  order, stated once in the router instead of re-derived by each app.
- **An id plus a REASON, not a per-widget callback.** The id is the
  app's (no allocator here, and one switch reads better than a callback
  pointer on every widget struct). The reason names the input that
  arrived -- press, motion, release, wheel -- which is the difference
  between "scrolled with the wheel" and "dragged the thumb", and cannot
  be recovered from widget state afterwards.

One deliberate behaviour change fell out: **the wheel goes to the widget
under the cursor**, not to whichever widget the app tried first. That is
what a user expects, and it is why `uapp` tracks the last pointer
position -- `WIN_EV_WHEEL` carries notches and no coordinates, exactly
as Wayland's axis event does, because the client already knows where its
pointer is.


## What editing text MEANS lives in one place, and the storage does not

Editable text existed three times over and behaved differently in each:
`uui_textbox` (single-line) had a caret and NO selection at all, `utext`
(multi-line) had selection primitives but no keymap, and Notepad
hand-wrote the keymap on top of them -- about sixty lines deciding what
Ctrl+A means, what Shift+Left does, and what typing with a selection
active should do. A second app wanting a text field would have written
that a fourth time.

`uui_edit` (`userland/ui/uui_edit.h`) owns the CURSOR, the SELECTION and
the KEYMAP, and delegates every read or write of characters through four
accessors. That split is the whole design: a 48-byte line and an 8 KB
ring cannot share storage, but they absolutely should share what
Backspace with a selection active does. It is the same shape
`kernel/lib/klineedit.c` already has kernel-side, where the physical
shell and the GUI Terminal share one line editor and each only paints
the result.

What a text field does now, which it could not before: click to place
the caret, drag to select, Ctrl+A, Shift+arrows, typing replaces the
selection, Backspace and Delete remove it. All of that arrived in
`uui_textbox` by deleting its keymap rather than by writing one.

**Two things the core deliberately does not decide.** Enter is never
consumed -- a field commits, a document inserts a newline, and that is
the widget's call, so Notepad still handles it and the field still
leaves it to the app. And Up/Down are offered only when the caller
supplies line accessors: a single-line field has no line above, so the
core declines the key rather than swallowing it, and the app can use it
for something else.

The multi-line side kept its public API (`utext_sel_*`, `utext_cursor_*`)
because Notepad calls those directly; they delegate now instead of
reimplementing. The one visible change is `struct utext`'s three fields
becoming one `struct uui_edit ed`, so there is a single owner of the
caret rather than two structures that could disagree.


## A filesystem talks to a BLOCK DEVICE, and persistence is the device's answer

TFS3 called `ata_*` directly, which was fine while a disk was the only
thing a filesystem could live on. A Live CD mounts an image the
bootloader handed over as a GRUB module, with no ATA controller
involved, so the choice was a block-device abstraction or an
`if (live) ... else ...` at every call site -- and the second shape rots
predictably: one path gets tested and the other is found broken later.

`struct block_device` (`kernel/include/kernel/block.h`) is the same
registry pattern `display_driver` and `fs_ops` already use here. Five
required operations, two optional ones behind capability bits, one
active device. TFS3 moved in 15 call-site substitutions because it
already funnelled everything through two functions for partition
support; **TFS2 deliberately kept its 24 direct `ata_*` calls**, since a
live image is always TFS3 and rewiring a legacy backend to serve a
feature it will never carry is cost with no return.

**Capabilities are declared and refused at registration**, per the
display_driver rule: a device claiming `BLK_CAP_FLUSH` with no `flush()`
is rejected, and so is a `flush()` with no bit. A device that needs a
flush and silently never gets one turns the journal's two barriers into
no-ops, which is a corruption bug that surfaces long after the mistake.
On a RAM device both optional operations are genuinely absent and the
block layer turns them into no-ops -- correct, not a degradation, since
nothing there can be lost independently of everything else.

**The part worth remembering: persistence belongs to the DEVICE.** The
VFS computes `g_persistent = fs->init() && blk_persistent()`, because a
backend cannot tell -- TFS3 mounts a RAM image exactly as it mounts a
disk, and asking it would have reported a live session as persistent.
`df`, `fsck` and the About window all repeat that answer to the user, so
the single most misleading thing this feature could have done was let a
live volume claim your files were safe. It says
`tfs3 (RAM-only -- won't survive reboot)` instead.


## TFS3's last block group may be partial, like ext2/3/4's

TFS3 divided a volume into whole 128 MiB block groups by floor
division, so the smallest filesystem it could make was 128 MiB and every
volume threw away the remainder. That was fine while the only volume was
a 9 GiB disk image, and became the single biggest cost of the Live CD:
the smallest live image was 129 MiB, which made the ISO 162 MB and put
7 seconds of GRUB module-reading in front of every boot.

ext2/3/4 have always allowed the LAST group to be short. The
32768-blocks-per-group figure is not a size choice, it is the number of
blocks one block of bitmap can describe; nothing about it requires the
volume to divide evenly. So the group count is a ceiling now, and
`group_span(g)` -- "how many blocks group g actually has" -- is the one
place that answers it. **`T3_BPG` still means the STRIDE between
groups**, and every block<->group calculation still uses it; only the
sites that meant "the size of this group" changed. The live image is
24 MiB and the ISO 57 MB.

The trick that kept the change small: at format time, blocks past the
end of the volume are marked USED in the last group's bitmap. The
allocator, the free-block search and the bitmap arithmetic then need no
knowledge of partial groups at all -- they simply never find those
blocks free.

**It also recovered space on the disk that was never a live image.** A
9 GiB `disk.img` now formats to 9362512 KB rather than 9232704 KB:
~127 MB that had existed and been unaddressable the whole time.

Two of the three sites that needed `group_span()` were found by failure
rather than by reading the code, which is the part worth carrying:
`df` reported a 16 MiB volume as 127 MB (free-space accounting still
assumed full groups), and the BACKUP SUPERBLOCK write used the nominal
group end, which lands past the volume -- so `fsformat` on a real disk
failed with nothing printed anywhere. That one was found by putting
`klog_printf(__LINE__)` on every `return 0` in the format path, which
took one run after three wrong guesses. A third followed from the
second: `group_span()` reads `g_sb`, and `format()` had not published
the new geometry yet, so during a format every span was computed from
the PREVIOUS volume's numbers.

**The positive control is the interesting part, because it did not
fire.** Re-introducing the `df` bug leaves the new geometry KTEST green:
on the 9 GB dev disk one group over-reported is 1.4% of the total, and
no honest bound is that tight. What catches it is `live_boot_test.py`'s
size check against the ~24 MB live volume -- the only small volume
anything here mounts. The KTEST's comment now records what it cannot
catch. Generalising: when a control fires nothing, ask whether the
test's DATA can express the bug at all before suspecting the harness.


## The desktop's app list is a directory of files, not a table in the kernel

`apps/gui_apps.c` held a C array of every app on the desktop, so adding
one meant editing the kernel and rebuilding it -- for a ring-3 program
that the kernel otherwise knows nothing about beyond a path to spawn.
With M41 moving the apps out of ring 0 one at a time, that table was
also the thing that would need editing on every single stage.

The list is `/usr/wm/desktop/*.desktop` now, one entry per app, scanned
at desktop startup: freedesktop's idea, and near enough its file format
that the entries are readable to anyone who has seen a Linux one
(`Name=`, `Exec=`, `Icon=`, `Categories=`). The parser is
`etc_config`'s, already in the tree for `/etc/toyos.conf` -- key=value
lines with `#` comments is exactly the format, so no second parser
exists. **Adding an app to the desktop is dropping a file in
`data/wm/desktop/`.**

`Exec=builtin:taskmgr` is the deliberate bridge: it names a
kernel-space app's callbacks rather than a binary, so the two apps that
cannot move to ring 3 yet (Task Manager and Control Panel need syscalls
that do not exist until M41 stage 4) live in the same list as everything
else. That form disappears when the last one moves, and nothing else
needs to change when it does.

Windowed binaries went to `/bin/wm/{system,apps,demos}/` at the same
time, the class taken from the SOURCE directory (`userland/gui/<class>/`)
the way `userland/gui` already meant `/bin`. The category a user sees is
therefore a property of where the code lives, not a string repeated in
two places that can disagree.


## The demo ISO is a separate image, and its tour is a file on it

Asked for a way to show the system on a laptop with nobody typing.
Three shapes were possible: a boot flag on the normal ISO, a recorded
input trace, or a scripted tour. The tour won on the same grounds the
`gui` debug commands did -- it drives the real system through the real
paths, so it cannot drift out of sync with the software it is
demonstrating, and when it breaks it breaks visibly.

**It is its own ISO (`make demo-iso`) rather than a runtime toggle**, for
one reason: a demo that can start itself by accident is a demo that
starts during something else. The `demo` keyword is on the kernel
command line in a grub.cfg that only that image carries.

**The script is a FILE on the image** (`/usr/wm/demo.script`, source
`data/wm/demo.script`), not compiled in, so the tour can be edited on a
live USB stick with no toolchain present. Its CLI half runs at the
console; its GUI half is performed **one step per WM iteration** from
inside `wm_run()`'s loop, through the same `wm_debug_dispatch()` path
the 175 GUI checks already use. That is not an implementation detail --
driving a desktop from outside its own event loop is precisely what
makes a scripted demo hang, and the debug console already had the
answer.

**The live ISO stays a separate artifact from the normal one**, which
cost a red CI to learn: a 129 MiB GRUB module took the boot smoke test
from 1.6s to 7.0s locally and blew CI's timeout outright, because GRUB
reads the whole module off the emulated CD-ROM before the kernel starts.
Partial block groups have since brought the image to 24 MiB, so folding
it into the default ISO is now arguable -- measure the boot before doing
it.

## The console is double-buffered because write-combining made its scroll 357x slower

Write-combining the framebuffer (see the PAT entry above) made every
drawing path in the system faster except one, and made that one
dramatically worse. WC is a **write** optimisation: stores are gathered
into burst transfers instead of going out one at a time. It does nothing
for loads, and it removes the caching that used to hide them -- a read
from a WC page is an uncached bus round trip with no cache fill and no
prefetch.

The framebuffer console was the one surface that read the framebuffer
back. It scrolled by shifting the visible pixels up in place, which is a
whole screen of reads, and its cursor saved the cell underneath itself
before painting over it, which is another read per blink. So the GUI got
faster (double-buffered, writes only) while the CLI got slower, and the
symptom was a scanline you could watch travel down a real display.

Measured with `gfxbench 20` under `make run-kvm`, same build, the only
difference being whether the console had a back buffer:

| | ms per scrolled text line |
|---|---|
| direct (reads the framebuffer) | 178.5 |
| double-buffered | 0.5 |

Full-screen *fill* throughput was identical either way (17.3 GB/s), which
is what confirms the change touches only the read path.

The fix is the invariant, not the number: **the console never reads the
framebuffer.** It draws into gfx.c's back buffer -- which already
existed, is already a static array, and already had dirty-rect tracking
for the WM -- and publishes with `gfx_present()`. Scrolling becomes a RAM
memmove and the cursor's save becomes a RAM read.

Two things worth knowing before editing it. **Drawing and showing are
now separate steps**, so a path that prints and then halts without
reaching a flush point leaves its text in RAM only; the flush points are
`vga_present()` from `keyboard_getchar_mods()`'s idle loop, a throttled
present at the end of each `vga_putc()`, and an explicit call on the
panic path in `idt.c` (which is the one that would otherwise lose the
panic banner itself). And **there is deliberately no "dirty" flag in
vga.c** -- gfx.c already tracks the dirty box and `gfx_present()` no-ops
when it is empty, so a second copy of that fact could only ever disagree
with it, in the silent direction.

Why this was invisible for so long: plain QEMU's TCG ignores guest
memory types entirely, so both paths are equally fast under `make run`
and every test in this repo. `make run-kvm` honours them, which is what
made it reproducible at all -- and is the reason `gfxbench` reports
which mode is live rather than just a number. See `kernel/drivers/vga.c`'s
double-buffering comment and `kernel/drivers/gfx_test.c`'s scroll KTESTs,
which pin the shift but explicitly cannot pin the cost.

## The legacy text console cannot be selected from GRUB, and `gfxpayload=text` does not do it

`kernel/drivers/vga.c` has a complete legacy 80x25 `0xB8000` backend, and
it is dead code on every normal boot: `vga_init()` only reaches it when
`gfx_init()` finds no usable linear framebuffer. The obvious way to make
it reachable -- a second GRUB menu entry with `set gfxpayload=text` --
was tried and **does not work**, and the measurements are worth recording
so nobody spends the time again.

`boot.asm`'s multiboot2 header carries a framebuffer request tag (type 5)
asking for 1280x720x32. GRUB acts on that *before* the kernel runs, so by
the time any kernel command-line word could be read the adapter is
already in a graphics mode and writes to `0xB8000` land nowhere visible.
That rules out a cmdline flag outright.

`gfxpayload` does not rescue it either. Two experiments, both booted and
read back from `dmesg`:

- With the header requesting 1280x720x32, `set gfxpayload=800x600x32` in
  the menu entry changed nothing -- still 1280x720. So for multiboot2 the
  header's request wins and `gfxpayload` is ignored.
- With the header's width/height/depth set to 0/0/0 ("no preference"),
  the resolution *did* change (GRUB chose 1280x800), proving the header
  edit took effect -- and `set gfxpayload=text` **still** produced a
  linear framebuffer. GRUB's multiboot2 loader always sets a graphics
  mode when the kernel carries a framebuffer request tag.

So making text mode selectable needs one of: a second kernel image built
without the framebuffer tag (a `make text-iso` variant, the way
`live-iso` and `demo-iso` are already separate artifacts), or a runtime
VGA mode-3 switch by banging registers directly, since there is no BIOS
`int 10h` in long mode. Neither is built. This is recorded rather than
attempted because the first is a whole second build of the kernel for a
fallback nobody has needed yet, and the second is a few hundred lines of
fragile register tables.

Note the fallback is not *entirely* unreachable in the meantime: it is
what runs on a machine where GRUB provides no framebuffer at all, which
is the case it exists for.
