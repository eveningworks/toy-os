# toy-os

A small x86-64 hobby operating system. Boots via GRUB (Multiboot2) into a
64-bit kernel written in C + a bit of Assembly, with a text shell and a
basic graphical mode.

This file covers what toy-os can do today and how to build/run it.
For everything else:

- [CHANGELOG.md](CHANGELOG.md) / [CHANGELOG-archive.md](CHANGELOG-archive.md) --
  the full build-by-build history, in order, with bugs found and fixed
  along the way
- [docs/decisions.md](docs/decisions.md) -- short, topic-indexed
  answers to "why does toy-os work this way?"
- [docs/process-isolation.md](docs/process-isolation.md) -- the full
  implementation walkthrough of ring0/ring3 privilege separation
  (GDT/TSS, paging, per-process address spaces, the ELF loader, the
  scheduler), told as it was built, bugs included
- [docs/roadmap.md](docs/roadmap.md) -- what's planned but not built yet

## Current features

- Boots via GRUB2 as a Multiboot2 kernel, transitions 32-bit -> 64-bit
  long mode itself (`kernel/core/boot.asm`)
- Linear RGB framebuffer (1280x720 default), falling back to 80x25 VGA
  text mode automatically if none is available -- one console (`vga.c`)
  renders through whichever backend is active
- Serial (COM1) debug logging (`-serial stdio` in QEMU); IDT + remapped
  8259 PIC + exception handler (prints and halts instead of triple-faulting)
- PS/2 keyboard (US QWERTY + Nordic `keyboard <us|se>`, shift + AltGr
  (e.g. `@ # $ { } [ ] \ |` on `se`) + arrows, command history that
  persists across reboot via `/etc/history`) and mouse (IRQ12), sharing
  the 8042 controller through one dispatcher (`i8042.c`) so the two
  IRQs don't steal each other's bytes
- PIT timer (100 Hz) and CMOS RTC (`time`, the GUI clock)
- A persistent, disk-backed filesystem ("TFS2" -- `kernel/drivers/tfs.c`,
  behind a small VFS dispatch layer so a second backend could be added
  later) with a write-ahead journal and timestamps -- files survive a
  full power-off, not just `reboot`. `about` shows whether the current
  boot found a disk. See `docs/tfs2-spec.md` for the on-disk format.
- A real anti-aliased font (JetBrains Mono, baked to bitmaps at build
  time -- `tools/genttf.py`), 8 switchable point sizes (`fontsize <n>`),
  plus 6 Nordic letters (Å/Ä/Ö/å/ä/ö) alongside ASCII. See
  `docs/decisions.md` for why Latin-1 over UTF-8.
- A basic GUI mode: a small window manager (movable/resizable windows,
  taskbar, Start menu, a desktop background with an icon grid, and a
  reusable right-click context menu wired into the desktop, window
  chrome, taskbar, and Start menu) with five apps -- Notepad, About,
  Calculator, Terminal (runs the real shell inside a window), and Task
  Manager (lists windows, shows memory usage). The Start menu's "Exit
  to shell" asks for confirmation first (`apps/wm/confirm_dialog.c`, a
  reusable screen-absolute Yes/No modal, same pattern as the context
  menu). See `apps/README.md` for how to add more apps, and the app
  registry (`apps/apps.c`) that makes that a one-line addition.
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

`help` (categorized; `help tests` for the developer/diagnostic ones
below), `clear`, `time`, `uptime`, `echo <text>`, `about`, `meminfo`,
`df` (disk space: total/used/free, KB-scale), `dmesg`, `color <name>`,
`reboot`, `ls [-al] [dir]` (colored by
default, `-l` shows type/size/mtime, `-a` accepted as a no-op -- a
real disk-hosted `/bin/ls` binary, not a shell built-in, see
`docs/decisions.md`), `cat <f>`, `touch <f>`,
`write <f> <text>`, `append <f> <text>`, `rm <f>`,
`edit <f>`/`nano <f>` (full-screen nano/pico-style editor -- arrows/
Home/End/Delete to navigate and edit, F2 to save, F3 to exit; works
from both the physical shell and the GUI Terminal, see `apps/editor.c`),
`gui`, `apps`, `run <app>`, `history` (persists across reboot via
`/etc/history`),
`fontsize <8|10|12|14|16|18|20|24>`, `keyboard <us|se>` (base + Shift +
AltGr), and the developer/diagnostic set
(`help tests`): `ring3test`, `schedtest`, `stress <mb>` (real
non-sparse write/read/verify pass over `<mb>` megabytes, exercising
direct/single/double/triple-indirect blocks with genuine data -- see
`docs/roadmap.md` for the still-open full-8GB-scale run), plus a dozen
real disk-hosted test binaries under `/bin` run via `run <name>` (e.g.
`run write_test`, `run crash_test`) -- see `ls /bin` for the full list
and `docs/decisions.md` for why these moved off dedicated shell
commands.

## Building on CachyOS

No cross-compiler is needed -- since the target and host are both x86-64,
the system GCC works fine with `-ffreestanding` and kernel-appropriate
flags.

```bash
sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full
```

## Build & run

```bash
make          # build kernel.bin only
make iso      # build toy-os.iso (bootable GRUB image)
make run      # build + boot in QEMU with a graphical window
make clean    # remove build artifacts
```

If `make run` doesn't show a window (e.g. over SSH), use:

```bash
make run-nographic   # serial console only, no VGA window
```

Type `help` at the `>` prompt once it boots. Type `gui` for the window
manager -- click Start (bottom-left) to launch Notepad or About, drag
windows by their title bar, and use the minimize/maximize/close buttons
in the top-right of each window. Press Esc to come back to the shell.
Type `apps` at the shell to see everything registered at the console
level, `run <name>` to launch any of them.

## Project layout

toy-os is split into four layers, from the hardware up:

```
kernel/core/     -- boot, interrupts (IDT/PIC), timer, serial, power,
                     multiboot parsing, kernel_main, GDT/TSS (gdt.c),
                     the physical frame allocator (pmm.c), kernel-space
                     paging (paging.c), per-process address spaces incl.
                     user-pointer validation (vmm.c), an ELF64 loader
                     (elf.c), the syscall entry point (syscall.c) and the
                     process-run/return mechanism it uses (process.c,
                     context_switch.asm), and six ring-3 demos
                     (ring3_test.c, elf_test.c, syscall_test.c,
                     write_test.c, ptr_test.c, gui_test.c -- the last one
                     is the first-step "GUI in user space" experiment,
                     see apps/README.md). Hardware bring-up only; knows
                     nothing about apps.
kernel/drivers/  -- device drivers: console (vga.c), framebuffer graphics
                     with double buffering (gfx.c), the bitmap font, the
                     shared PS/2 controller dispatcher (i8042.c) plus the
                     keyboard and mouse behind it, and the persistent,
                     disk-backed filesystem (tfs.c).
kernel/include/  -- all headers, including kapi.h -- the ONE header apps
                     are supposed to include. It aggregates the driver
                     APIs apps are allowed to use, so drivers can be
                     reshuffled internally without every app needing an
                     edit.
apps/            -- programs. Two kinds:
                     * console apps (shell.c, gui.c) registered in
                       apps.c -- they own the whole screen and run their
                       own loop
                     * GUI apps (notepad.c, about.c, calculator.c,
                       taskmgr.c) registered in gui_apps.c --
                       event-driven, launched from the Start menu or a
                       desktop icon, drawn into a window by the window
                       manager (apps/wm/ -- split across wm.c/
                       wm_input.c/wm_render.c/desktop.c/context_menu.c/
                       start_menu.c for readability, see apps/README.md)
                     Adding either is "write the file, add one line to
                     the matching registry" -- see apps/README.md.
                     apps/ui/ holds the shared widget primitives (button,
                     scrollback, scrollbar, checkbox, textbox) used by
                     the window manager, Calculator, Notepad, and
                     Terminal -- one file per widget, see apps/README.md.
userland/        -- freestanding ring-3 test programs (no libc, no
                     crt0), compiled and linked as real ELF64
                     executables via userland/link.ld (-mcmodel=large --
                     see the write syscall bug entry above for why),
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
Makefile     -- build / iso / run targets (globs kernel/core, kernel/drivers,
                apps automatically -- new files are picked up with no
                Makefile edits needed)
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
tools/OFL.txt    -- SIL Open Font License 1.1 text for JetBrains Mono,
                     the baked font's source face
```

## Process isolation

By default, the kernel, drivers, shell, and GUI apps all run in one
shared address space at ring 0 -- but toy-os also has the beginnings of
real privilege separation: a GDT/TSS, a physical frame allocator, paging
with per-process address spaces, an ELF64 loader, and a preemptive
scheduler that runs up to four ring-3 processes genuinely concurrently.
See [docs/process-isolation.md](docs/process-isolation.md) for the full
build-up, told as it was built, bugs included.

