# toy-os

A small x86-64 hobby operating system. Boots via GRUB (Multiboot2) into a
64-bit kernel written in C + a bit of Assembly, with a text shell and a
basic graphical mode.

This file covers what toy-os can do today and how to build/run it.
For everything else:

- [CHANGELOG.md](CHANGELOG.md) plus
  [CHANGELOG-archive-2.md](CHANGELOG-archive-2.md) and
  [CHANGELOG-archive.md](CHANGELOG-archive.md) -- the full history in
  order, with bugs found and fixed along the way, split into three
  eras (newest first: the semver era, the Build-number era, the
  earliest milestones)
- [docs/decisions.md](docs/decisions.md) -- short, topic-indexed
  answers to "why does toy-os work this way?"
- [docs/process-isolation.md](docs/process-isolation.md) -- the full
  implementation walkthrough of ring0/ring3 privilege separation
  (GDT/TSS, paging, per-process address spaces, the ELF loader, the
  scheduler), told as it was built, bugs included
- [docs/tfs2-spec.md](docs/tfs2-spec.md) -- the on-disk filesystem
  format (superblock, bitmap, records, journal), spec-style
- [docs/arch-portability.md](docs/arch-portability.md) -- what is and
  isn't x86-64-specific, if this were ever ported
- [docs/roadmap.md](docs/roadmap.md) -- what's planned but not built yet

## Current features

- Boots via GRUB2 as a Multiboot2 kernel, transitions 32-bit -> 64-bit
  long mode itself (`kernel/core/boot.asm`)
- Linear RGB framebuffer (1280x720 default), falling back to 80x25 VGA
  text mode automatically if none is available -- one console (`vga.c`)
  renders through whichever backend is active
- Serial (COM1) debug logging (`-serial stdio` in QEMU) plus a small
  read-only serial *console* (`kernel/core/debug_console.c` --
  `meminfo`/`lsfs`/`lsdev` over a second connection, usable while the
  screen is showing the GUI or a ring-3 process is running); IDT +
  remapped 8259 PIC + exception handler (prints and halts instead of
  triple-faulting)
- PS/2 keyboard and mouse (IRQ12), sharing the 8042 controller through
  one dispatcher (`i8042.c`) so the two IRQs don't steal each other's
  bytes. Keyboard layouts are *data files* (`/etc/kbs/<name>`, `us` and
  `se` shipped) generated from Linux's own XKB data by
  `tools/gen_kbs.py`, not a compiled-in table -- base + Shift + AltGr
  (e.g. `@ # $ { } [ ] \ |` on `se`), arrows, and command history that
  persists across reboot via `/etc/history`
- PIT timer (100 Hz), CMOS RTC (`time`, the GUI clock, selectable
  timezone via `timezone`), and the PC speaker (`beep`)
- PCI bus enumeration (`lspci`, also a real `/bin/lspci` binary) and
  MBR/GPT partition-table parsing (`parttable`)
- Baseline memory hardening (Milestone 2): NX enforced for userspace
  pages with W^X from each ELF segment's real `p_flags`, and
  `-fstack-protector-strong` canaries on both the kernel and userland.
  The kernel's own identity map is still RWX -- see
  `docs/decisions.md`. `run nx_test` / `run stack_smash_test` prove
  both for real, not by assertion.
- A persistent, disk-backed filesystem ("TFS2" -- `kernel/drivers/tfs.c`,
  behind a small VFS dispatch layer so a second backend could be added
  later) with a write-ahead journal and timestamps -- files survive a
  full power-off, not just `reboot`. Up to 256 files/directories;
  individual files scale to gigabytes via direct + single/double/triple
  indirect block pointers. Sequential I/O runs ~25 MB/s write / ~30 MB/s
  read on the emulated ATA path: large writes batch the ATA cache flush
  and free-block bitmap persistence rather than doing one of each per
  4KB block (`ata_flush_begin()`/`ata_flush_end()`), and contiguous
  blocks are coalesced into single 64KB ATA commands. The filesystem
  sizes itself to the drive's real capacity (IDENTIFY words 60-61)
  rather than a compile-time guess, and refuses to format a disk whose
  superblock it couldn't read -- degrading to RAM-only instead of
  destroying a possibly-good filesystem. `about` shows whether the
  current boot found a disk. See `docs/tfs2-spec.md` for the on-disk
  format and `docs/decisions.md` for the durability tradeoffs.
- A real anti-aliased font (JetBrains Mono, baked to bitmaps at build
  time -- `tools/genttf.py`), 8 switchable point sizes (`fontsize <n>`),
  plus 6 Nordic letters (Å/Ä/Ö/å/ä/ö) alongside ASCII. See
  `docs/decisions.md` for why Latin-1 over UTF-8.
- A basic GUI mode: a small window manager (movable/resizable windows,
  taskbar with a notification area/tray (`apps/wm/wm_tray.c`, the clock
  is its first item), Start menu, a desktop background with a draggable
  icon grid (positions persist across reboot), and a
  reusable right-click context menu wired into the desktop, window
  chrome, taskbar, and Start menu) with five apps -- Notepad, About,
  Calculator, Terminal (runs the real shell inside a window; `ls` and an
  allowlist of `/bin` binaries via `run` execute asynchronously through
  the scheduler instead of freezing the window, see `docs/roadmap.md`'s
  async I/O item), and Task
  Manager (lists windows, shows memory usage). The Start menu's "Exit
  to shell" and "Shutdown" both ask for confirmation first
  (`apps/wm/confirm_dialog.c`, a reusable screen-absolute Yes/No
  modal); Shutdown itself uses the QEMU/Bochs ACPI I/O-port poweroff
  trick (`kernel/core/power.c`), with a halt-and-message fallback. A
  reusable Open/Save file-picker dialog (`apps/wm/file_picker.c`, full
  directory navigation) backs Notepad's Open.../Save As... toolbar
  buttons and is meant for any future app that needs one. See
  `apps/README.md` for how to add more apps, and the app registry
  (`apps/apps.c`) that makes that a one-line addition.
- A kernel heap allocator (`kmalloc`/`kzalloc`/`kfree`) and a small JSON
  library (`kernel/core/json.c`, parse/serialize/read-file/write-file --
  coexists with the flat `etc_config.h` name=value format, see
  `docs/decisions.md`).
- The beginnings of real process isolation and a preemptive scheduler
  (up to four ring-3 processes genuinely concurrent) -- see
  [docs/process-isolation.md](docs/process-isolation.md) for the full
  build-up, bugs included.
- Syscalls beyond `write`/`exit`: `SYS_READ_KEY`/`SYS_SBRK` (a per-process
  heap), a real per-window protocol (`SYS_WIN_CREATE`/`SYS_WIN_PRESENT`,
  still modal -- see `docs/roadmap.md`), and file I/O
  (`SYS_OPEN`/`SYS_READ`/`SYS_CLOSE`, `SYS_WRITE` extended to take an
  fd). See the `echo_test`/`win_test`/`file_test` `/bin` binaries
  (`run echo_test`, etc).
- A single version string (`VERSION` at the repo root, semver + a
  `-dev` suffix during development -- see `docs/decisions.md`) shown by
  both `about` and the GUI About window.

### Shell commands

Grouped the same way `help` itself groups them (`help tests` for the
developer/diagnostic set):

Tab completes commands, paths, and known argument sets (`run`, `color`,
`debug`, `keyboard`, `timezone`, `fontsize`, `fsck`, `help`) -- one Tab
extends as far as the candidates agree, and lists them in columns if
more than one remains, zsh-style. Works identically in the physical
shell and the GUI Terminal.

- **General:** `help`, `clear`, `about`, `beep`, `apps`, `run <app>`,
  `gui`, `history` (persists across reboot via `/etc/history`),
  `echo <text>`, `reboot`
- **Files & filesystem:** `ls [-al] [dir]` (colored by default, `-l`
  shows type/size/mtime, `-a` a no-op -- a real disk-hosted `/bin/ls`
  binary, not a shell built-in, see `docs/decisions.md`), `cd [dir]`,
  `pwd`, `mkdir <dir>`, `cat <f>`, `touch <f>`, `write <f> <text>`,
  `append <f> <text>`, `rm <f>`, `stat <f>`, `edit <f>`/`nano <f>`
  (full-screen nano/pico-style editor -- arrows/Home/End/Delete to
  navigate and edit, F2 to save, F3 to exit; works from both the
  physical shell and the GUI Terminal, see `apps/editor.c`). Paths may
  be relative to the cwd or absolute.
- **System info:** `time`, `timezone [city]`, `uptime`, `meminfo`,
  `df` (disk space: total/used/free, KB-scale), `dmesg`, `lspci`,
  `parttable`
- **Appearance:** `color <name>`, `fontsize <8|10|12|14|16|18|20|24>`,
  `keyboard <us|se>` (base + Shift + AltGr)
- **Developer/diagnostic (`help tests`):** `ring3test`, `schedtest`,
  `stress <mb>` (real non-sparse write/read/verify pass over `<mb>`
  megabytes with a live progress bar, exercising direct/single/double/
  triple-indirect blocks with genuine data, verified on real hardware
  at 400MB with no failures -- see `docs/roadmap.md` for the still-open
  full-8GB-scale run), `dmatest [lba]` and `steptest <mb>` (read-only/
  small-write-and-read proofs of the async-I/O work's non-blocking DMA
  primitive and steppable write/read APIs respectively -- see
  `docs/roadmap.md`'s async I/O item; Notepad's Save As.../Open... are
  the real callers), `debug [<subsystem> on|off]` (per-subsystem
  runtime debug-log switches -- `fs`/`wm`/`ata`, off by default, no
  rebuild needed), `fsck [repair]` (filesystem consistency check:
  walks every file's block tree and compares it against the free-block
  bitmap; read-only unless `repair` is passed)

Plus roughly a dozen real disk-hosted test binaries under `/bin`, run
via `run <name>` (e.g. `run write_test`, `run nx_test`,
`run crash_test`) -- see `ls /bin` for the full list and
`docs/decisions.md` for why these moved off dedicated shell commands.

## Releases

Tagged releases live on [GitHub Releases](https://github.com/Drenos/toy-os/releases) --
each one ships `toy-os.iso`, a gzipped `disk.img.gz` (there's no
installer yet, so the pre-seeded disk image is what puts `/bin/ls`,
`/bin/lspci`, etc. on the filesystem -- the ISO alone boots into a
near-empty one), and `run_release.sh`, a standalone launcher that
gunzips the disk image and starts QEMU with the exact device/display
flags this OS expects (no repo checkout needed). See
`docs/decisions.md`'s versioning entry for how a release gets cut.

## Building on CachyOS

No cross-compiler is needed -- since the target and host are both x86-64,
the system GCC works fine with `-ffreestanding` and kernel-appropriate
flags.

```bash
sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full
```

## Build & run

```bash
make            # build kernel.bin + the userland ELFs
make iso        # build toy-os.iso (bootable GRUB image), seeding disk.img
make run        # build + boot in QEMU with a graphical window
make run-audio  # same, with a PulseAudio backend so `beep` is audible
make debug      # boot frozen (-s -S) for GDB: see below
make clean      # remove build artifacts
```

If `make run` doesn't show a window (e.g. over SSH), use:

```bash
make run-nographic   # serial console only, no VGA window
```

For real breakpoint/single-step debugging, `make debug` boots frozen at
CPU reset against QEMU's own GDB stub -- no kernel-side GDB code
involved. In another terminal:

```bash
gdb build/kernel.bin -ex "target remote localhost:1234"
```

`CFLAGS`/`USERLAND_CFLAGS` both carry `-g`, so the kernel and every
userland ELF have real DWARF symbols (function names, source lines,
locals). See `CLAUDE.md` and `docs/decisions.md` for why there's no
in-kernel serial GDB stub.

Type `help` at the `>` prompt once it boots. Type `gui` for the window
manager -- click Start (bottom-left) to launch Notepad or About, drag
windows by their title bar, and use the minimize/maximize/close buttons
in the top-right of each window. Press Esc to come back to the shell.
Type `apps` at the shell to see everything registered at the console
level, `run <name>` to launch any of them.

## Project layout

toy-os is split into four layers, from the hardware up:

```
kernel/core/     -- boot, interrupts (IDT/PIC/IRQ dispatch), timer,
                     serial + the read-only serial debug console
                     (debug_console.c), power, multiboot parsing,
                     kernel_main, GDT/TSS (gdt.c), the kernel heap
                     (heap.c), the physical frame allocator (pmm.c),
                     kernel-space paging (paging.c), per-process address
                     spaces incl. user-pointer validation (vmm.c), an
                     ELF64 loader (elf.c/elf_run.c), the syscall entry
                     point (syscall.c), the process-run/return mechanism
                     it uses (process.c, context_switch.asm) and the
                     preemptive scheduler (scheduler.c). Plus the small
                     cross-cutting services: the kernel log ring buffer
                     behind dmesg (klog.c), runtime debug switches
                     (debugflags.c), /etc config reading (etc_config.c)
                     and its users (tz.c, font_config.c,
                     keyboard_config.c), keyboard layout data-file
                     parsing (keyboard_layout.c), a small JSON library
                     (json.c), stack-canary support (stack_protector.c),
                     and one remaining in-kernel ring-3 demo
                     (ring3_test.c -- the rest became real /bin ELF
                     binaries, see userland/ below). Hardware bring-up
                     only; knows nothing about apps.
kernel/drivers/  -- device drivers: console (vga.c), framebuffer graphics
                     with double buffering + damage-region clipping
                     (gfx.c), the baked TTF font (font_ttf.c), the
                     shared PS/2 controller dispatcher (i8042.c) plus
                     the keyboard and mouse behind it, ATA/DMA disk
                     access (ata.c), PCI enumeration (pci.c),
                     MBR/GPT partition-table parsing (partition.c), the
                     PC speaker (speaker.c), and the persistent,
                     disk-backed filesystem (tfs.c) behind a VFS
                     dispatch layer (vfs.c).
kernel/include/  -- all headers, including kapi.h -- the ONE header apps
                     are supposed to include. It aggregates the driver
                     APIs apps are allowed to use, so drivers can be
                     reshuffled internally without every app needing an
                     edit.
apps/            -- programs. Two kinds:
                     * console apps (shell.c -- itself split into
                       shell.c/shell_fs.c/shell_sys.c, plus the
                       full-screen editor in editor.c -- and gui.c)
                       registered in apps.c: they own the whole screen
                       and run their own loop
                     * GUI apps (notepad.c, about.c, calculator.c +
                       calc_engine.c, terminal.c, taskmgr.c) registered
                       in gui_apps.c -- event-driven, launched from the
                       Start menu or a desktop icon, drawn into a window
                       by the window manager (apps/wm/ -- split across
                       wm.c/wm_input.c/wm_render.c/desktop.c/
                       context_menu.c/start_menu.c/confirm_dialog.c/
                       file_picker.c/wm_tray.c for readability, see
                       apps/README.md)
                     Adding either is "write the file, add one line to
                     the matching registry" -- see apps/README.md.
                     apps/ui/ holds the shared widget primitives
                     (primitives, button, button group, scrollback,
                     scrollbar, checkbox, textbox, icon grid) used by
                     the window manager, Calculator, Notepad, and
                     Terminal -- one file per widget, see apps/README.md.
                     apps/theme.h holds the THEME_* named colors.
userland/        -- freestanding ring-3 test programs (no libc, no
                     crt0), compiled and linked as real ELF64
                     executables via userland/link.ld (-mcmodel=large,
                     and separate page-aligned segments per permission
                     class so W^X means something -- see
                     docs/process-isolation.md and the Makefile's own
                     USERLAND_CFLAGS comment),
                     seeded onto disk.img's /bin at build time (see the
                     Makefile's `seed` target, tools/tfs2_writer.py, and
                     docs/decisions.md) and run via the shell's
                     `run <name>` -- not loaded as GRUB modules anymore.
                     hello.c deliberately executes a privileged
                     instruction from ring 3 (a fault the kernel
                     recovers from and reports, same mechanism
                     crash_test.c also exercises on purpose); exit_test.c
                     and write_test.c call real syscalls and return
                     cleanly instead -- write_test.c is the only one
                     that produces its own console output. write_bad_test.c
                     deliberately passes an invalid pointer to write, to
                     prove the kernel's pointer validation rejects it.
                     gui_test.c draws directly to the real screen and
                     reads real keyboard input -- see the "GUI in user
                     space" note in apps/README.md for its honest scope.
                     nx_test.c and stack_smash_test.c each trip one of
                     the Milestone 2 hardening mechanisms on purpose
                     (jump into a data page; overflow a stack buffer)
                     and are how both are actually verified.
seed/            -- the files mirrored onto disk.img at build time by
                     the Makefile's `seed` target via
                     tools/tfs2_writer.py: seed/sync/bin/ (every
                     userland ELF) and seed/sync/etc/kbs/ (the generated
                     keyboard layout data files). `sync/` is
                     content-hash-synced on every build; a `once/`
                     subtree would be copy-once. See docs/decisions.md.
```

`kernel_main()` (in `kernel/core/kernel.c`) does hardware bring-up --
serial, console, interrupts, filesystem -- then calls `apps_start()`,
which launches the shell. It never mentions the shell by name; the shell
is just the first thing in the app registry. That's the seam that makes
this modular: the kernel core doesn't know or care what apps exist.

Other files:
```
linker.ld    -- links kernel at 1 MiB, matching Multiboot2 conventions
grub.cfg     -- GRUB menu entry pointing at kernel.bin
Makefile     -- build / iso / run / debug / seed targets. Globs
                kernel/core, kernel/drivers, apps, apps/wm and apps/ui,
                so new files in those directories are picked up with no
                Makefile edit; a *new* subdirectory under apps/ needs its
                own wildcard + pattern rule (see CLAUDE.md). Header
                dependencies are tracked (-MMD/-MP), so editing a shared
                header rebuilds everything that includes it.
VERSION      -- the single version string (semver + a -dev suffix);
                kernel/include/version.h is GENERATED from it by
                tools/gen_version.sh, never hand-edited
tools/genttf.py  -- font source of truth; regenerate kernel/drivers/font_ttf.c
                     from the .ttf here, don't edit that file by hand. Only
                     needed to change the font -- the baked output is
                     already committed, so building/running toy-os itself
                     doesn't need this. Regenerating needs Python 3,
                     Pillow (`pip install pillow`), and the JetBrains Mono
                     font installed (`sudo pacman -S ttf-jetbrains-mono`
                     on CachyOS, or download from jetbrains.com/lp/mono).
                     tools/genfont.py is the retired hand-drawn-8x8-font
                     generator, kept for history/reference only -- nothing
                     includes its output anymore.
tools/gen_kbs.py -- generates the seed/sync/etc/kbs/<layout> keyboard
                     layout data files from Linux's own XKB data, so
                     adding a layout is one command, not an afternoon
                     with a scancode chart
tools/OFL.txt    -- SIL Open Font License 1.1 text for JetBrains Mono,
                     the baked font's source face
tools/run_release.sh -- standalone QEMU launcher shipped as a GitHub
                     Release asset (not run by the build itself) --
                     gunzips disk.img.gz if needed and boots with the
                     same device/display flags `make run` uses, for
                     anyone running from just a release download.
```

## Process isolation

By default, the kernel, drivers, shell, and GUI apps all run in one
shared address space at ring 0 -- but toy-os also has the beginnings of
real privilege separation: a GDT/TSS, a physical frame allocator, paging
with per-process address spaces, an ELF64 loader, and a preemptive
scheduler that runs up to four ring-3 processes genuinely concurrently.
See [docs/process-isolation.md](docs/process-isolation.md) for the full
build-up, told as it was built, bugs included. The scheduler is now
continuously armed (not just during the `schedtest` demo) with a public
non-blocking spawn API (`scheduler_spawn()`/`scheduler_poll()`) --
`apps/terminal.c`'s async `ls`/`run` is the real caller; see
`docs/decisions.md` for why permanently-armed is still safe.

