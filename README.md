<h1 align="center">toy-os</h1>

<p align="center">
  A small x86-64 operating system, written from scratch in C and assembly.<br>
  Boots via GRUB into a 64-bit kernel with a shell, a window manager, and a
  disk-backed filesystem.
</p>

<p align="center">
  <a href="https://github.com/Drenos/toy-os/actions/workflows/build.yml">
    <img alt="CI" src="https://github.com/Drenos/toy-os/actions/workflows/build.yml/badge.svg">
  </a>
  <img alt="Language" src="https://img.shields.io/badge/language-C%20%2B%20NASM-blue">
  <img alt="Target" src="https://img.shields.io/badge/target-x86__64-lightgrey">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

<p align="center">
  <img src="screenshots/readme/desktop.png" alt="toy-os desktop: window manager with the Terminal app open" width="49%">
  <img src="screenshots/readme/shell.png" alt="toy-os shell: about, ls, df and the in-kernel test suite" width="49%">
</p>

---

## Contents

- [What this is](#what-this-is)
- [Quick start](#quick-start)
  - [Install the dependencies](#install-the-dependencies)
  - [Build and run](#build-and-run)
  - [Troubleshooting](#troubleshooting)
- [Using it](#using-it)
- [Development](#development)
- [Features](#features)
- [Project layout](#project-layout)
- [Documentation](#documentation)
- [License](#license)

## What this is

A hobby OS built one subsystem at a time, with the reasoning for each
decision written down as it happened. It boots on real hardware and
under QEMU, and it is not a teaching toy that stops at "hello world from
the kernel":

- **Real memory management** -- a physical frame allocator, per-process
  page tables, a kernel heap, NX/W^X enforcement and stack canaries.
- **Real processes** -- an ELF64 loader, ring-3 user mode, syscalls, and
  a preemptive round-robin scheduler.
- **A real filesystem** -- TFS2, journaled and disk-backed, with
  indirect block pointers, an `fsck`, and files that survive a power
  cut.
- **A real GUI** -- a window manager with movable/resizable windows, a
  taskbar, a Start menu, a draggable desktop, and five apps including a
  terminal emulator that runs the actual shell.
- **Its own test suite** -- `make test` boots the OS headless, runs
  in-kernel tests including deliberate fault injection, and exits
  non-zero on failure.

No cross-compiler is needed: host and target are both x86-64, so the
system GCC works with `-ffreestanding` and kernel-appropriate flags.

## Quick start

### Install the dependencies

You need a C toolchain, NASM, GRUB's rescue-image tools, and QEMU.

<table>
<tr><th>Distribution</th><th>Command</th></tr>
<tr><td><b>Debian / Ubuntu / Mint / Pop!_OS</b></td><td>

```bash
sudo apt install build-essential nasm grub-pc-bin grub-common \
                 xorriso mtools qemu-system-x86 python3
```

</td></tr>
<tr><td><b>Arch / CachyOS / Manjaro / EndeavourOS</b></td><td>

```bash
sudo pacman -S --needed base-devel nasm grub xorriso mtools \
                        qemu-full python
```

</td></tr>
<tr><td><b>Fedora / RHEL / Rocky / Alma</b></td><td>

```bash
sudo dnf install gcc make binutils nasm grub2-tools grub2-pc-modules \
                 xorriso mtools qemu-system-x86 python3
```

</td></tr>
<tr><td><b>openSUSE</b></td><td>

```bash
sudo zypper install gcc make binutils nasm grub2 grub2-i386-pc \
                    xorriso mtools qemu-x86 python3
```

</td></tr>
<tr><td><b>Alpine</b></td><td>

```bash
doas apk add build-base nasm grub grub-bios xorriso mtools \
             qemu-system-x86_64 python3
```

</td></tr>
<tr><td><b>Void</b></td><td>

```bash
sudo xbps-install -S base-devel nasm grub xorriso mtools \
                     qemu python3
```

</td></tr>
</table>

**What each package is for**, if your distribution names them
differently:

| Need | Why |
|---|---|
| `gcc`, `binutils`, `make` | Compiles and links the kernel. Any GCC that can target x86-64 works; no cross-compiler required. |
| `nasm` | Assembles the boot, interrupt and context-switch stubs. |
| `grub-mkrescue` + `xorriso` + `mtools` | Builds the bootable ISO. `grub-mkrescue` needs all three, and the BIOS modules package (`grub-pc-bin` on Debian, `grub2-pc-modules` on Fedora, `grub2-i386-pc` on openSUSE) is easy to miss. |
| `qemu-system-x86_64` | Runs it. |
| `python3` | Build-time disk seeding and the test/dev tools. Pillow (`pip install pillow`) is needed only for screenshots. |

> **Verified firsthand on Arch/CachyOS**, and the Debian/Ubuntu list is
> what this project's CI installs on every push, so both are known-good.
> The Fedora, openSUSE, Alpine and Void lines are package-name
> translations of the same requirements -- if one is wrong on your
> system, the table above says what to look for, and a correction is
> welcome.

### Build and run

```bash
git clone https://github.com/Drenos/toy-os.git
cd toy-os
make run
```

That builds the kernel, seeds a disk image, produces `toy-os.iso`, and
boots it in QEMU. Type `help` at the `/>` prompt; type `gui` for the
window manager and press the Start menu's *Exit to shell* to come back.

The kernel prints its init sequence to the screen while booting, and the
console keeps scrollback -- **PageUp/PageDown** scrolls through anything
that went past, including the boot messages, from any prompt.

Other targets:

```bash
make            # build kernel.bin + the userland ELF binaries
make iso        # + toy-os.iso (bootable GRUB image), seeding disk.img
make run        # build + boot in QEMU with a graphical window
make run-audio  # same, with a PulseAudio backend so `beep` is audible
make run-menu   # same, but with the GRUB boot menu visible (5s timeout)
make run-nographic  # serial console only -- use this over SSH
make debug      # boot frozen (-s -S) for GDB, see below
make test       # boot headless, run the in-kernel test suite
make verify     # full check: clean build + iso + boot test + test suite
make clean      # remove build artifacts (leaves disk.img alone)
```

### Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `make: grub-mkrescue not found` | Install GRUB's rescue tools (`grub-common` on Debian, `grub2-tools` on Fedora, `grub2` on openSUSE). The build looks for both `grub-mkrescue` and `grub2-mkrescue`, so the Fedora/openSUSE naming is handled for you. |
| `grub-mkrescue` fails with *"cannot find `xorriso`"* or an EFI/mtools error | Install `xorriso` **and** `mtools`; `grub-mkrescue` needs both even for a BIOS-only image. |
| ISO builds but QEMU shows *"no bootable device"* | The BIOS modules package is missing -- `grub-pc-bin` (Debian), `grub2-pc-modules` (Fedora), `grub2-i386-pc` (openSUSE), `grub-bios` (Alpine). |
| No window appears (e.g. over SSH) | `make run-nographic` -- serial console only, no display needed. |
| The mouse doesn't move in QEMU | Don't add `-device usb-tablet`/`usb-mouse`. This kernel's mouse driver is PS/2 only, and an explicit USB pointer device makes QEMU route motion there instead. |
| Everything is very slow | Expected without KVM. The build and tests don't need it; `make run` is usable either way. |
| `disk.img` is 9 GB | It's a *sparse* file -- it costs only what's actually written. `make clean-disk` wipes it. |

For real breakpoint/single-step debugging, `make debug` boots frozen at
CPU reset against QEMU's own GDB stub -- no kernel-side GDB code
involved. In another terminal:

```bash
gdb build/kernel.bin -ex "target remote localhost:1234"
```

`CFLAGS`/`USERLAND_CFLAGS` both carry `-g`, so the kernel and every
userland ELF have real DWARF symbols (function names, source lines,
locals). See `docs/decisions.md` for why there's no in-kernel serial GDB
stub.

## Using it

Grouped the same way `help` itself groups them (`help tests` for the
developer/diagnostic set):

Executables run by name, with no prefix: typing `nx_test` searches
`PATH` (set in `/etc/toyos.conf`, default `/bin;/usr/bin`, searched left
to right with the first match winning) and runs what it finds. `run
<name>` still works as the explicit form, going through the same
resolver. `path` shows the search order. Shell builtins win over both,
which is what keeps `ls` able to resolve a cwd-relative argument before
handing `/bin/ls` an absolute path.

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
- **Developer/diagnostic (`help tests`):** `strace <binary> [args]`
  (Linux-style syscall tracing of a `/bin` binary -- one decoded line
  per syscall, e.g. `open("notes.txt", O_WRITE|O_CREAT) = 3`, plus a
  count when it exits; also captured in `dmesg`), `ring3test`,
  `schedtest`,
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
  rebuild needed), `ktest [suite]` (run the in-kernel test suite --
  see `make test`), `fsck [repair]` (filesystem consistency check:
  walks every file's block tree and compares it against the free-block
  bitmap; read-only unless `repair` is passed)

Plus roughly a dozen real disk-hosted test binaries under `/bin`, run
via `run <name>` (e.g. `run write_test`, `run nx_test`,
`run crash_test`) -- see `ls /bin` for the full list and
`docs/decisions.md` for why these moved off dedicated shell commands.

## Development

```bash
make verify     # the full gate: clean build + iso + boot test + ktest
make test       # just the in-kernel test suite
```

Writing a test is a `KTEST()` block in a `*_test.c` file next to the
code it exercises -- it registers itself through a linker section, so
there's no registry to update and no Makefile edit:

```c
KTEST("mm", "kzalloc returns zeroed memory") {
    uint8_t *p = kzalloc(256);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 256; i++) KTEST_ASSERT_EQ(p[i], 0);
    kfree(p);
}
```

`kernel/include/kernel/fault_inject.h` can fail the next N disk writes,
disk reads or `kmalloc` calls, which is how the error paths are tested.

Useful tools in `tools/` (all documented in their own docstrings):

| Tool | What it's for |
|---|---|
| `vm.py` | Start a headless VM and run shell commands against it, getting **text** back: `vm.py run "fsck"`. Usually a better check than a screenshot. |
| `ktest_run.py` | Drives the in-kernel test suite over serial and turns it into an exit code. What `make test` and CI run. |
| `boot_smoke_test.py` | Fast "does it still boot cleanly" check, no GUI. |
| `qmp_test.py`, `gui_flow.py`, `shell_flow.py` | Drive the GUI over QEMU's QMP socket for rendering/input work, with the mouse/keyboard gotchas already handled. |
| `tfs2_writer.py` | Read, write, delete and inspect files inside `disk.img` from the host, without booting. |
| `screenshot_diff.py` | Pixel-diff two screenshots with a pass/fail threshold. |

`CLAUDE.md` documents the conventions and environment quirks in depth.

## Features

- Boots via GRUB2 as a Multiboot2 kernel, transitions 32-bit -> 64-bit
  long mode itself (`kernel/arch/x86_64/boot.asm`)
- Linear RGB framebuffer (1280x720 default), falling back to 80x25 VGA
  text mode automatically if none is available -- one console (`vga.c`)
  renders through whichever backend is active, with scrollback
  (PageUp/PageDown) over the last few hundred lines. The kernel's init
  sequence is printed to it during boot and stays readable afterwards
- Serial (COM1) debug logging (`-serial stdio` in QEMU) plus a serial
  *console* (`kernel/core/debug_console.c`) usable while the screen is
  showing the GUI or a ring-3 process is running: `meminfo`/`lsfs`/
  `lsdev` for inspection, `ktest` to run the test suite, and `sh
  <command>` to run any shell command with its output coming back over
  the wire -- which is what makes headless verification a text
  assertion rather than a screenshot (`tools/vm.py`); IDT +
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
- A persistent, disk-backed filesystem ("TFS2" -- `kernel/fs/tfs.c`,
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
  library (`kernel/lib/json.c`, parse/serialize/read-file/write-file --
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

## Project layout

toy-os is split by concern, from the hardware up. Directories are
subsystems, not filing cabinets -- where a file lives says what kind of
thing it is:

```
kernel/arch/x86_64/ -- everything that is x86-64 by nature and would be
                     rewritten wholesale on another architecture: the
                     Multiboot2 entry and 32->64-bit transition
                     (boot.asm), interrupt stubs (isr.asm), the ring
                     switch (context_switch.asm), GDT/TSS (gdt.c),
                     IDT (idt.c), the 8259 PIC (pic.c), IRQ dispatch
                     (irq.c) and kernel-space page tables (paging.c).
                     See docs/arch-portability.md.
kernel/core/     -- bring-up and the pieces that own the machine as a
                     whole: kernel_main (kernel.c), multiboot parsing,
                     the PIT/RTC timer, serial plus the read-only serial
                     debug console (debug_console.c), and power
                     (power.c). Hardware bring-up only; knows nothing
                     about apps.
kernel/mm/       -- memory: the physical frame allocator (pmm.c),
                     per-process address spaces incl. user-pointer
                     validation (vmm.c), and the kernel heap (heap.c).
kernel/proc/     -- processes: the ELF64 loader (elf.c/elf_run.c), the
                     syscall entry point (syscall.c), the process
                     run/return mechanism (process.c), the preemptive
                     scheduler (scheduler.c), and the one remaining
                     in-kernel ring-3 demo (ring3_test.c -- the rest
                     became real /bin ELF binaries, see userland/).
kernel/fs/       -- the filesystem: TFS2 (tfs.c) behind the VFS dispatch
                     layer (vfs.c). A filesystem isn't a device driver,
                     so it doesn't live in drivers/ -- the block device
                     it sits on (ata.c) does.
kernel/lib/      -- cross-cutting services with no hardware of their
                     own: freestanding string routines (string.c), a
                     JSON library (json.c), the kernel log ring buffer
                     behind dmesg (klog.c), runtime debug switches
                     (debugflags.c), /etc config reading (etc_config.c)
                     and its users (tz.c, font_config.c,
                     keyboard_config.c), keyboard layout data-file
                     parsing (keyboard_layout.c), and stack-canary
                     support (stack_protector.c).
kernel/drivers/  -- device drivers, and only device drivers: console
                     (vga.c), framebuffer graphics with double buffering
                     + damage-region clipping (gfx.c), the baked TTF
                     font (font_ttf.c), the shared PS/2 controller
                     dispatcher (i8042.c) plus the keyboard and mouse
                     behind it, ATA/DMA disk access (ata.c), PCI
                     enumeration (pci.c), MBR/GPT partition-table
                     parsing (partition.c) and the PC speaker
                     (speaker.c).
kernel/include/  -- headers, split by audience and enforced by the
                     build's include paths rather than by convention:
                     api/ is what apps/ may use (kapi.h and everything
                     it aggregates), abi/ is the kernel<->userland
                     contract userland/ shares, kernel/ is internal and
                     is NOT on apps/'s include path. See
                     kernel/include/README.md.
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
                     hello.c is the smallest one -- greet via SYS_WRITE,
                     exit(0) -- and the one to read first; crash_test.c
                     and nx_test.c are the ones that deliberately fault,
                     to show the kernel recovering; exit_test.c
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
                kernel/include/api/version.h is GENERATED from it by
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

## Documentation

| Document | Contents |
|---|---|
| [docs/decisions.md](docs/decisions.md) | Topic-indexed answers to "why is this built this way?" -- ~70 entries. Start here when something looks odd. |
| [docs/roadmap.md](docs/roadmap.md) | What's planned, ordered so prerequisites come before the work that needs them. |
| [docs/process-isolation.md](docs/process-isolation.md) | The full ring0/ring3 build-up: GDT/TSS, paging, per-process address spaces, the ELF loader, the scheduler -- told as it was built, bugs included. |
| [docs/tfs2-spec.md](docs/tfs2-spec.md) | Byte-level on-disk filesystem format, spec-style. |
| [docs/arch-portability.md](docs/arch-portability.md) | What is and isn't x86-64-specific, and what a second architecture would take. |
| [CHANGELOG.md](CHANGELOG.md) | The full history with rationale, split into three eras ([archive-2](CHANGELOG-archive-2.md), [archive](CHANGELOG-archive.md)). |
| [kernel/README.md](kernel/README.md) | What each kernel subsystem holds, and the test for where a new file goes. |
| [apps/README.md](apps/README.md) | How to add a console app or a GUI app. |

## Releases

Tagged releases live on
[GitHub Releases](https://github.com/Drenos/toy-os/releases). Each ships
`toy-os.iso`, a gzipped `disk.img.gz` (there's no installer yet, so the
pre-seeded disk image is what puts `/bin/ls`, `/bin/lspci` and friends
on the filesystem -- the ISO alone boots into a near-empty one), and
`run_release.sh`, a standalone launcher that gunzips the disk image and
starts QEMU with the flags this OS expects, with no repo checkout
needed.

## License

MIT -- see [LICENSE](LICENSE). The baked JetBrains Mono glyph data in
`kernel/drivers/font_ttf.c` is separately covered by the SIL Open Font
License 1.1 ([tools/OFL.txt](tools/OFL.txt)).
