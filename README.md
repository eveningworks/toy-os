<h1 align="center">toy-os</h1>

<p align="center">
  A small x86-64 operating system, written from scratch in C and assembly.<br>
  Boots via GRUB into a 64-bit kernel with a shell, a window manager, and a
  journaling disk-backed filesystem.
</p>

<p align="center">
  <a href="https://github.com/eveningworks/toy-os/actions/workflows/build.yml">
    <img alt="CI" src="https://github.com/eveningworks/toy-os/actions/workflows/build.yml/badge.svg">
  </a>
  <img alt="Language" src="https://img.shields.io/badge/language-C%20%2B%20NASM-blue">
  <img alt="Target" src="https://img.shields.io/badge/target-x86__64-lightgrey">
  <img alt="Version" src="https://img.shields.io/badge/version-0.3.0--dev-orange">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

<p align="center">
  <img src="screenshots/readme/desktop.png" alt="toy-os desktop: window manager with the Terminal app open" width="49%">
  <img src="screenshots/readme/shell.png" alt="toy-os shell: about, ls, df and the in-kernel test suite" width="49%">
</p>

---

## Contents

- [What this is](#what-this-is)
- [Project status](#project-status)
- [Quick start](#quick-start)
- [Highlights](#highlights)
- [Architecture](#architecture)
- [Development](#development)
- [Documentation](#documentation)
- [Releases](#releases)
- [License](#license)

## What this is

A hobby OS built one subsystem at a time, with the reasoning for each
decision written down as it happened. It boots on real hardware and under
QEMU, and it does not stop at "hello world from the kernel":

- **Real memory management** — a physical frame allocator, per-process
  page tables, a kernel heap with optional red-zones, NX/W^X enforcement
  on both the kernel and userspace, SMEP/SMAP, stack canaries, and kernel
  ASLR.
- **Real processes** — an ELF64 loader, ring-3 user mode, syscalls, a
  preemptive round-robin scheduler, pipes and `spawn`/`waitpid`, and
  per-process FPU state across context switches.
- **Two real filesystems** — TFS3 (the default: block groups, real
  inodes, hardlinks, journal transactions, superblock backups) and TFS2
  (the original, kept as a second backend). Both journal metadata, so
  files survive a power cut, and both come with an `fsck`. The VFS picks
  by superblock probe; `fsformat` switches live.
- **A real GUI, and it is not in the kernel** — the window manager is
  itself a **ring-3 process**: movable, resizable windows, a taskbar, a
  Start menu built from `.desktop` files (picked up live — drop a file
  in and it appears), and a desktop of draggable icons with rubber-band
  selection. Apps are ordinary ring-3 processes too, owning their
  windows over **TWP**, the Toy Window Protocol, served by **TWS** and
  programmed against with **Toykit**. The kernel keeps the framebuffer
  and the protocol; everything above them is a process, and killing the
  desktop is survivable.
- **Its own test suite** — `make test` boots the OS headless, runs
  in-kernel tests including deliberate fault injection, and exits
  non-zero on failure. A separate GUI suite drives the desktop over a
  serial debug channel and asserts on pixels.

No cross-compiler is needed: host and target are both x86-64, so the
system GCC works with `-ffreestanding` and kernel-appropriate flags.

## Project status

**Version 0.3.0-dev.** A hobby project under active development, not
production software. What that means concretely:

**Works today** — booting on real hardware and QEMU, the shell and its
line editor, both filesystems with `fsck` and live reformatting, the
window manager and its apps, ring-3 processes with pipes and job
control, and the full test suite (270 in-kernel tests, 9 ring-3
diagnostics, ~300 GUI checks across 23 tools).

**[Milestone 41](docs/wm-ring3-design.md) is complete** (2026-08-18):
the window manager is an ordinary ring-3 process. `gui` spawns
`/bin/wm/system/toywm`, which claims the compositor role, is granted the
real framebuffer, composites the desktop and serves every client through
the window protocol. The ring-0 window manager and its widget set are
deleted — about 10,400 lines — so there is one implementation again.
Killing the desktop (`kill 1`) is survivable: the kernel revokes the
grant, asks client windows to close, restores the text console, and
`spawn /bin/wm/system/toywm` starts a new one.

**Known gaps** — there is no USB stack ([Milestone
32](docs/roadmap.md)), so input on real hardware currently depends on
the firmware's BIOS legacy PS/2 emulation. No networking, no SMP, no
`malloc`/`FILE`/`printf` in userland. `docs/roadmap.md` tracks all of it,
including a candid known-issues list.

## Quick start

### Dependencies

A C toolchain, NASM, GRUB's rescue-image tools, and QEMU.

| Distribution | Command |
|---|---|
| **Debian / Ubuntu / Mint** | `sudo apt install build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3` |
| **Arch / CachyOS / Manjaro** | `sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full python` |
| **Fedora / RHEL / Rocky** | `sudo dnf install gcc make binutils nasm grub2-tools grub2-pc-modules xorriso mtools qemu-system-x86 python3` |
| **openSUSE** | `sudo zypper install gcc make binutils nasm grub2 grub2-i386-pc xorriso mtools qemu-x86 python3` |
| **Alpine** | `doas apk add build-base nasm grub grub-bios xorriso mtools qemu-system-x86_64 python3` |
| **Void** | `sudo xbps-install -S base-devel nasm grub xorriso mtools qemu python3` |

<details>
<summary>What each package is for, if your distribution names them differently</summary>

| Need | Why |
|---|---|
| `gcc`, `binutils`, `make` | Compiles and links the kernel. Any GCC that can target x86-64 works; no cross-compiler required. |
| `nasm` | Assembles the boot, interrupt and context-switch stubs. |
| `grub-mkrescue` + `xorriso` + `mtools` | Builds the bootable ISO. `grub-mkrescue` needs all three, and the BIOS modules package (`grub-pc-bin` on Debian, `grub2-pc-modules` on Fedora, `grub2-i386-pc` on openSUSE) is easy to miss. |
| `qemu-system-x86_64` | Runs it. |
| `python3` | Build-time disk seeding and the test/dev tools. Pillow (`pip install pillow`) is needed only for screenshots. |

Verified firsthand on Arch/CachyOS, and the Debian/Ubuntu list is what CI
installs on every push. The rest are package-name translations of the
same requirements — corrections welcome.
</details>

### Build and run

```bash
git clone https://github.com/eveningworks/toy-os.git
cd toy-os
make run
```

That builds the kernel, seeds a disk image, produces `toy-os.iso` and
boots it in QEMU. Type `help` at the `/>` prompt, or `gui` for the window
manager (the Start menu's *Exit to shell* comes back). **PageUp/PageDown**
scrolls the console's history, including the boot log.

```bash
make            # kernel.bin + the userland ELF binaries
make iso        # + toy-os.iso, seeding disk.img
make run        # build + boot in QEMU with a graphical window
make run-kvm    # same, KVM-accelerated instead of emulated (needs /dev/kvm)
make run-menu   # same, with the GRUB boot menu visible
make run-nographic  # serial console only -- use this over SSH
make live-iso   # a Live CD that boots with no disk attached at all
make demo-iso   # boots straight into a scripted tour
make debug      # boot frozen (-s -S) for GDB
make test       # boot headless, run the in-kernel test suite
make verify     # full gate: clean build + iso + boot test + test suite
```

<details>
<summary>Troubleshooting</summary>

| Symptom | Cause and fix |
|---|---|
| `grub-mkrescue not found` | Install GRUB's rescue tools (`grub-common` on Debian, `grub2-tools` on Fedora). The build looks for both `grub-mkrescue` and `grub2-mkrescue`. |
| `grub-mkrescue` fails on *"cannot find `xorriso`"* or mtools | Install `xorriso` **and** `mtools`; it needs both even for a BIOS-only image. |
| ISO builds but QEMU says *"no bootable device"* | The BIOS modules package is missing — `grub-pc-bin` (Debian), `grub2-pc-modules` (Fedora), `grub2-i386-pc` (openSUSE), `grub-bios` (Alpine). |
| No window appears (e.g. over SSH) | `make run-nographic`. |
| The mouse doesn't move in QEMU | Don't add `-device usb-tablet`/`usb-mouse`. This kernel's mouse driver is PS/2 only, and an explicit USB pointer device makes QEMU route motion there instead. |
| Everything is very slow | `make run` emulates the CPU. `make run-kvm` runs it natively — but that only helps compute-bound code; disk I/O measures ~1.9× *slower* under KVM, because each port-I/O instruction becomes a VM exit. |
| Drawing is slow on real hardware but fine in QEMU | Use `make run-kvm` to reproduce it. Plain `make run` emulates the CPU and **ignores guest memory types entirely**, so an uncached or write-combined framebuffer behaves like cached RAM and a whole class of graphics performance bug is invisible. KVM honours them. `gfxbench` reports the numbers and which mechanisms are live. |
| `disk.img` is 9 GB | It's a *sparse* file — it costs only what is actually written. `make clean-disk` wipes it. |

For breakpoint debugging, `make debug` boots frozen against QEMU's own
GDB stub — no kernel-side GDB code involved:

```bash
gdb build/kernel.bin -ex "target remote localhost:1234"
```

Both `CFLAGS` and `USERLAND_CFLAGS` carry `-g`, so the kernel and every
userland ELF have real DWARF symbols.
</details>

### Using it

Type `help` at the prompt. The full command reference lives in
[docs/commands.md](docs/commands.md); [docs/boot-flags.md](docs/boot-flags.md)
covers what you can pass on the GRUB command line.

## Highlights

**Boot and hardware.** Multiboot2 via GRUB2, with the 32→64-bit long-mode
transition done by hand (`kernel/arch/x86_64/boot.asm`). Linear RGB
framebuffer with automatic fallback to 80×25 VGA text. PS/2 keyboard and
mouse sharing the 8042 through one dispatcher, with keyboard layouts as
*data files* generated from Linux's own XKB data rather than a
compiled-in table. PIT, CMOS RTC, PC speaker, PCI enumeration, and
MBR/GPT partition parsing.

**Memory hardening.** NX and W^X from each ELF segment's real `p_flags`
in userspace, and the kernel's own identity map is W^X too — `.text` is
the only executable range and is read-only, with CR0.WP set so ring 0
actually honours it. SMEP and SMAP wherever the CPU reports them, with
kernel code reaching user memory only through copy helpers that go via
the kernel's own map, so **EFLAGS.AC is never set anywhere** and there is
no STAC/CLAC window. Guard pages below user stacks, randomised stack
canaries, heap red-zones behind a runtime switch, and **kernel ASLR** —
the kernel relocates itself to a random base at boot and patches ~7,400
of its own absolute references.

**Filesystems.** [TFS3](docs/tfs3-spec.md) is the default: block groups,
128-byte checksummed inodes, hardlinks, atomic rename and truncation,
32-slot journal transactions, ext-style superblock backups, ~590k files
on a 9 GiB volume. [TFS2](docs/tfs2-spec.md) remains as a second backend
with its format unchanged. Both scale individual files to gigabytes
through direct and single/double/triple-indirect pointers, batch ATA
flushes, TRIM freed blocks back to the host, and refuse to touch a disk
whose superblock could not be read rather than destroying a possibly-good
filesystem.

**Graphics and GUI.** A real anti-aliased font (JetBrains Mono, baked to
bitmaps at build time) in eight switchable sizes — and because the entire
UI is font-*derived*, changing the size reflows everything rather than
clipping it. Double-buffered rendering with damage-region clipping, and a
compositor whose damage invariant is enforced by a verification mode that
renders every frame twice and reports any pixel that changed without
being declared.

**Userland.** Every ring-3 program is just a `main()`: crt0 provides
`_start` over the standard SysV stack layout, and libsys gives one typed
wrapper per syscall. The shared kernel toolkit is compiled a second time
under the C names, so a ring-3 `strlen` and the kernel's `k_strlen`
cannot diverge. Adding a program is a `.c` file with no Makefile edit.
Still deliberately not a libc — no `malloc`, `FILE`, `printf` or `errno`.

## Architecture

Directories are subsystems, not filing cabinets — where a file lives says
what kind of thing it is.

```
kernel/
  arch/x86_64/  everything x86-64 by nature: the Multiboot2 entry and
                long-mode transition, interrupt stubs, the ring switch,
                GDT/TSS, IDT, the 8259 PIC, page tables, self-relocation
  core/         bring-up and whole-machine concerns: kernel_main,
                multiboot parsing, timers, serial + the debug console
  mm/           physical frames, per-process address spaces, kernel heap
  proc/         ELF64 loader, syscalls, scheduler, the window server
  fs/           TFS3 and TFS2 behind the probe-selecting VFS
  lib/          services with no hardware: the shared toolkit (strings,
                numbers, formatter, paths, line editor), JSON, klog,
                /etc config, entropy, the RAM meter
  drivers/      one piece of hardware each: console, framebuffer, font,
                i8042 + keyboard/mouse, ATA, PCI, partitions, speaker,
                and the display-driver registry
  include/      headers split by audience, ENFORCED by include paths:
                api/ (what apps may use), abi/ (the kernel<->userland
                contract), kernel/ (internal, off apps/'s path)

apps/           kernel-space programs: the shell, the editor, the
                scripted demo. NO GUI lives here any more -- the window
                manager and its widgets moved to userland/wm/ and
                userland/ui/. What is left of apps/ui/ is the one text
                widget the kernel's own `edit` command draws with.

userland/       ring-3 programs, split by ROLE:
  rt/           crt0, libsys, linker script
  ui/           Toykit -- the GUI toolkit clients program against
  lib/          non-UI libraries: the tosh shell, C-name string/stdio
  gui/          windowed apps      -> seeded to /bin
  bin/          command-line tools -> seeded to /bin
  tests/        single-mechanism diagnostics -> seeded to /tests

seed/, data/    what gets mirrored onto disk.img at build time
tools/          build, test and delivery tooling (see Development)
docs/           design records, specs, and the decision log
```

Source discovery is recursive, so a new file — or a whole new directory —
under `kernel/` or `apps/` is picked up with no Makefile edit. Header
dependencies are tracked, and `tools/check_deps.py` proves per build
directory that the tracking is actually live.

`kernel_main()` does hardware bring-up and then calls `apps_start()`,
which launches the shell. It never mentions the shell by name — the shell
is simply the first entry in the app registry, and that seam is what
keeps the kernel core ignorant of what apps exist.

## Development

```bash
make verify                     # the full gate: clean build + iso + boot test + ktest
bash tools/preflight.sh         # same, plus a git status summary (~25s)
python3 tools/gui_regress.py    # every GUI test tool as one table (~1.5 min)
```

Writing a test is a `KTEST()` block in a `*_test.c` file next to the code
it exercises. It registers itself through a linker section, so there is
no registry to update and no Makefile edit:

```c
KTEST("mm", "kzalloc returns zeroed memory") {
    uint8_t *p = kzalloc(256);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 256; i++) KTEST_ASSERT_EQ(p[i], 0);
    kfree(p);
}
```

`kernel/include/kernel/fault_inject.h` can fail the next N disk writes,
disk reads or `kmalloc` calls, which is how the error paths are tested at
all.

Selected tools (each documented in its own docstring):

| Tool | What it's for |
|---|---|
| `vm.py` | Start a headless VM and run shell commands against it, getting **text** back: `vm.py run "fsck"`. Usually a better check than a screenshot. |
| `ktest_run.py` | Drives the in-kernel suite over serial and turns it into an exit code. What `make test` and CI run. |
| `boot_smoke_test.py` | Fast "does it still boot cleanly" check, no GUI. |
| `gui_debug.py` | Asks the WM what it is doing — window rects, z-order, hit-testing, damage — instead of measuring a screenshot. |
| `gui_regress.py` | Every GUI test tool, each on its own fresh disk image and VM, as one pass/fail table. |
| `damage_sweep.py`, `damage_hunt.py` | Walk the interactions that break the compositor's damage invariant, over one seed or many. |
| `pixel_probe.py` | Reads exact pixel values out of screenshots and tabulates them across several, so a rendering change is a number rather than an impression. |
| `qmp_test.py`, `gui_flow.py`, `shell_flow.py` | Drive the GUI over QEMU's QMP socket, with the mouse/keyboard gotchas already handled. |
| `tfs3_writer.py`, `tfs2_writer.py` | Read, write, inspect and corrupt-for-testing files inside a `disk.img` from the host, without booting. Each refuses the other's images. |
| `fs_switch_test.py` | Proves probe, wipefs, live `fsformat` both ways, and reboot persistence. |
| `faulttest_run.py` | The `/tests` binaries that fault ON PURPOSE, asserted against the kernel's own crash report rather than an exit code they don't have. |

`CLAUDE.md` documents the conventions and environment quirks in depth.

## Documentation

| Document | Contents |
|---|---|
| [docs/decisions.md](docs/decisions.md) | Topic-indexed answers to "why is this built this way?". Start here when something looks odd. |
| [docs/roadmap.md](docs/roadmap.md) | What's planned, ordered so prerequisites come first — plus the known-issues list. |
| [docs/commands.md](docs/commands.md) | The full shell command reference. |
| [docs/boot-flags.md](docs/boot-flags.md) | Every word the kernel looks for on the GRUB command line. |
| [docs/filesystem-layout.md](docs/filesystem-layout.md) | What lives where on the OS's own disk. Checked against the built image by `tools/check_layout.py`. |
| [docs/gui-guidelines.md](docs/gui-guidelines.md) | How the GUI should look and behave, and how to verify a change to it properly. |
| [docs/uapp-design.md](docs/uapp-design.md) | Toykit's design: how a ring-3 GUI app is written, and the staging that got there. |
| [docs/wm-ring3-design.md](docs/wm-ring3-design.md) | Milestone 41 — how the window manager was moved out of the kernel, stage by stage. Complete. |
| [docs/process-isolation.md](docs/process-isolation.md) | The full ring0/ring3 build-up, told as it was built, bugs included. |
| [docs/tfs3-spec.md](docs/tfs3-spec.md) / [design](docs/tfs3-design.md) | Byte-level format of the default filesystem, and the reasoning behind it. |
| [docs/tfs2-spec.md](docs/tfs2-spec.md) | Byte-level format of the legacy second backend. |
| [docs/live-cd-design.md](docs/live-cd-design.md) | How the Live CD carries a filesystem image as a GRUB module. |
| [docs/arch-portability.md](docs/arch-portability.md) | What is and isn't x86-64-specific, and what a second architecture would take. |
| [kernel/README.md](kernel/README.md), [apps/README.md](apps/README.md) | Where a new file goes, and how to add an app. |
| [CHANGELOG.md](CHANGELOG.md) | History through 2026-08-15, now **frozen** — `git log` is the chronological record, and reasoning lives in `docs/decisions.md`. |

## Releases

Tagged releases live on
[GitHub Releases](https://github.com/eveningworks/toy-os/releases). Each
ships `toy-os.iso`, a gzipped `disk.img.gz` (there is no installer yet, so
the pre-seeded image is what puts `/bin/ls` and friends on the
filesystem — the ISO alone boots into a near-empty one), and
`run_release.sh`, a standalone launcher that needs no repo checkout.

## License

MIT — see [LICENSE](LICENSE). The baked JetBrains Mono glyph data in
`kernel/drivers/font_ttf.c` is separately covered by the SIL Open Font
License 1.1 ([tools/OFL.txt](tools/OFL.txt)), and the bundled `pci.ids`
database in `data/` has its own terms — see LICENSE's "Third-party data"
section.
