<h1 align="center">toy-os</h1>

<p align="center">
  A small x86-64 operating system, written from scratch in C and assembly.<br>
  Boots via GRUB into a 64-bit kernel with a shell, a window manager, and a
  journaling disk-backed filesystem.
</p>

<p align="center">
  <a href="https://github.com/eveningworks/toy-os/actions/workflows/build.yml">
    <img alt="release build" src="https://github.com/eveningworks/toy-os/actions/workflows/build.yml/badge.svg">
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
  page tables, **demand-paged heaps** (`sbrk` reserves; the page arrives
  on first touch), one allocator serving both `kmalloc` and ring-3
  `malloc`, NX/W^X on both the kernel and userspace, SMEP/SMAP, stack
  canaries and kernel ASLR.
- **Real processes** — an ELF64 loader, ring-3 user mode, a table-driven
  syscall layer, a preemptive scheduler, **an `init` as pid 1** that
  adopts orphans and **supervises services** described by files in
  `/etc/services.d` (the desktop is one, restarted if it dies), a process
  tree with pipes and `spawn`/`waitpid`, and per-process FPU state across
  context switches.
- **Two real filesystems** — TFS3 (the default: block groups, real
  inodes, hardlinks, journal transactions, superblock backups) and TFS2
  (the original, kept as a second backend). Both journal metadata, so
  files survive a power cut, and both come with an `fsck`. The VFS picks
  by superblock probe; `fsformat` switches live.
- **A real GUI, and it is not in the kernel** — the window manager is
  itself a **ring-3 process**: movable, resizable windows, a taskbar, a
  Start menu built from `.desktop` files (picked up live), and a desktop
  of draggable icons with rubber-band selection. Apps are ordinary ring-3
  processes too, owning their windows over **TWP**, the Toy Window
  Protocol, served by **TWS** and programmed against with **Toykit**. The
  kernel keeps the framebuffer and the protocol; everything above them is
  a process, and killing the desktop is survivable.
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
window manager and its apps, fonts loaded and rasterized from disk at
runtime, ring-3 processes with pipes and `spawn`/`waitpid`, and the full
test suite: a few hundred in-kernel
tests, the ring-3 diagnostics, and the GUI tools `gui_regress.py` runs
as one table.

**[Milestone 41](docs/wm-ring3-design.md) is complete** (2026-08-18):
the window manager is an ordinary ring-3 process. `gui` spawns
`/bin/wm/system/toywm`, which claims the compositor role, is granted the
real framebuffer, composites the desktop and serves every client through
the window protocol. The ring-0 window manager and its widget set are
deleted — about 10,400 lines — so there is one implementation again.
Killing the desktop is survivable: the kernel revokes the grant, asks
client windows to close and restores the text console.

**[init](docs/init-design.md) holds pid 1 and starts the desktop**
(2026-08-18): `system.default_target` (`text`/`graphical`) says what the
machine is for, `/etc/services.d` says what to start, and init restarts a
service that dies — with a backoff, a give-up so a crash loop cannot spin
the machine, and `Restart=on-failure` semantics so a clean exit (the Start
menu's *Exit to shell*) means what it says. `target=text` on the GRUB line overrides the target for
one boot without rewriting the file. **In progress** — a console device,
then the shell moving to ring 3.

**Known gaps** — no USB stack, so input on real hardware depends on the
firmware's legacy PS/2 emulation. No networking, no SMP, no demand
paging, and no `malloc`/`FILE`/`printf` in userland.
[docs/roadmap.md](docs/roadmap.md) tracks all of it, including a candid
known-issues list.

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

Verified firsthand on Arch/CachyOS; the Debian/Ubuntu list is what CI
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
boots it in QEMU. **init brings the desktop up on its own**; *Exit to
shell* in the Start menu drops to the `/>` prompt, and `gui` goes back.
For a text-only boot, `make iso KCMDLINE="target=text"`. PageUp/PageDown
scrolls the console history, including the boot log.

```bash
make            # kernel.bin + the userland ELF binaries
make iso        # + toy-os.iso, seeding disk.img
make run        # build + boot in QEMU with a graphical window
make live-iso   # a Live CD that boots with no disk attached at all
make demo-iso   # boots straight into a scripted tour
make debug      # boot frozen (-s -S) for GDB
make test       # boot headless, run the in-kernel test suite
make verify     # full gate: clean build + iso + boot test + test suite
```

There is **one run target**, and everything that would otherwise be its
own is a variable on it — so any combination works without a target per
combination:

```bash
make run KVM=1        # KVM-accelerated instead of emulated (needs /dev/kvm)
make run VIRTIO=1     # virtio for EVERY device class: disk, GPU and input
make run NOGRAPHIC=1  # serial console only -- use this over SSH
make run MENU=1       # show GRUB's boot menu instead of booting straight through
make run AUDIO=1      # PC speaker wired to sound, so `beep` is audible
make run MEM=512      # a smaller machine
make run LIVE=1       # the Live CD, with no disk attached
make run DEMO=1       # the scripted tour
make run KVM=1 VIRTIO=1   # ...or any mix
```

**Each device class picks its implementation by name**, and `VIRTIO=1`
is simply the switch that sets all three at once. A per-class value
overrides it, so `VIRTIO=1 VGA=std` is a legal thing to ask for:

```bash
make run DISK=virtio        # virtio-blk, and NO IDE controller at all
make run VGA=virtio         # the virtio-gpu driver
make run VGA=vmware         # the adapter with a hardware cursor
make run INPUT=virtio       # virtio keyboard, mouse and tablet
```

A *name* rather than a boolean because a boolean cannot express a third
one, and this machine is going to grow them — NVMe is on the roadmap,
and an `NVME=1` beside a `VIRTIO=1` would immediately raise "what does
setting both mean?". `make help` lists every axis.

<details>
<summary>Troubleshooting</summary>

| Symptom | Cause and fix |
|---|---|
| `grub-mkrescue not found` | Install GRUB's rescue tools (`grub-common` on Debian, `grub2-tools` on Fedora). The build looks for both `grub-mkrescue` and `grub2-mkrescue`. |
| `grub-mkrescue` fails on *"cannot find `xorriso`"* or mtools | Install `xorriso` **and** `mtools`; it needs both even for a BIOS-only image. |
| ISO builds but QEMU says *"no bootable device"* | The BIOS modules package is missing — `grub-pc-bin` (Debian), `grub2-pc-modules` (Fedora), `grub2-i386-pc` (openSUSE), `grub-bios` (Alpine). |
| No window appears (e.g. over SSH) | `make run NOGRAPHIC=1`. |
| The mouse doesn't move in QEMU | Don't add `-device usb-tablet`/`usb-mouse`. This kernel's mouse driver is PS/2 only, and an explicit USB pointer device makes QEMU route motion there instead. |
| Everything is very slow | `make run` emulates the CPU; `make run KVM=1` runs it natively. That only helps compute-bound code — *ATA* disk I/O measures ~1.9× **slower** under KVM, since each port-I/O instruction becomes a VM exit. That penalty is ATA's, not KVM's: `make run KVM=1 DISK=virtio` puts the disk on virtio-blk and measures ~10× ATA's write throughput, because a virtqueue barely touches port I/O at all. |
| Drawing is slow on real hardware but fine in QEMU | Reproduce it with `make run KVM=1`. Plain `make run` **ignores guest memory types entirely**, so a write-combined framebuffer behaves like cached RAM and a whole class of graphics bug is invisible. `gfxbench` reports which mechanisms are live. |
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

Type `help` at the prompt. [docs/commands.md](docs/commands.md) indexes
one page per command under [docs/commands/](docs/commands/), and is the
full command reference; [docs/boot-flags.md](docs/boot-flags.md) covers
what you can pass on the GRUB command line.

## Highlights

**Boot and hardware.** Multiboot2 via GRUB2, with the 32→64-bit long-mode
transition done by hand. Linear RGB framebuffer falling back to 80×25 VGA
text. PS/2 keyboard and mouse sharing the 8042 through one dispatcher,
with keyboard layouts as *data files* generated from Linux's own XKB data
rather than a compiled-in table. PIT, CMOS RTC, PC speaker, MBR/GPT
partition parsing, and a **virtio** stack: PCI capability walking and
64-bit BAR decoding underneath a shared modern-virtio transport, with
`virtio-blk` on top of it as the preferred disk, `virtio-rng` feeding
the kernel's entropy pool (a QEMU guest usually has no RDSEED/RDRAND,
and the jitter fallback is weakest under emulation), and **`virtio-gpu`
as a real display driver** — resource, scanout, transfer-and-flush and
its own cursor queue, programming the mode itself so `video=1920x1080`
is honoured rather than left to whatever GRUB negotiated, and
**`virtio-input`** (keyboard, mouse and tablet) feeding an **input core**
whose canonical event is evdev-shaped, so PS/2, virtio and a future USB
HID driver are all sources in one registry — and the first virtio
devices here to complete on a real interrupt rather than a poll. One
transport, so the next device (net) is a driver rather than a
bring-up project — and it is about **10× ATA's write throughput under
KVM**, because a virtqueue is shared memory with one doorbell where ATA
is dense with port I/O and every one of those is a VM exit.

**Memory hardening.** NX and W^X from each ELF segment's real `p_flags`,
and the kernel's own identity map is W^X too — `.text` is the only
executable range and is read-only, with CR0.WP set so ring 0 honours it.
SMEP and SMAP wherever the CPU reports them, with kernel code reaching
user memory only through copy helpers that go via the kernel's own map,
so **EFLAGS.AC is never set anywhere** and there is no STAC/CLAC window.
Guard pages below user stacks and kernel stacks, randomised stack
canaries, heap red-zones behind a runtime switch, and **kernel ASLR** —
the kernel relocates itself to a random base at boot and patches ~7,400
of its own absolute references. A mapping records whether it OWNS its
frame, so teardown cannot hand back memory somebody else is still using,
and `meminfo audit` checks every live address space against the
allocator.

**Filesystems.** [TFS3](docs/tfs3-spec.md) is the default: block groups,
128-byte checksummed inodes, hardlinks, atomic rename and truncation,
32-slot journal transactions, ext-style superblock backups, ~590k files
on a 9 GiB volume. [TFS2](docs/tfs2-spec.md) remains as a second backend,
format unchanged. Both scale files to gigabytes through direct and
single/double/triple-indirect pointers, batch ATA flushes, TRIM freed
blocks back to the host, and refuse to touch a disk whose superblock
could not be read rather than destroying a possibly-good filesystem.

**Graphics and GUI.** Real fonts, two ways: eight sizes of JetBrains Mono
baked to bitmaps at build time as the guaranteed fallback, and a
fixed-point TrueType rasterizer that loads a `.ttf` from
`/usr/share/fonts` at runtime — so any size works, not just a baked one,
and a proportional face gets genuine per-glyph advance widths and real
kerning from the font's own tables. A face is a *family*: bold is a
second file loaded alongside the regular one, or — where a family has no
bold — synthesized by thickening the regular outlines, which is what GDI
does. Because the whole UI is font-*derived*, changing the face or the
size reflows everything rather than clipping it, live, without
restarting anything.

Fonts come in **two tiers**. The session font is the desktop's face, in
both weights, rasterized once in the kernel and mapped read-only into
every window — so all text on screen matches the desktop's setting by
construction rather than by each app being careful. An app that needs
something that font cannot express — a different face, a heading at
twice the body size — rasterizes it *itself*, in ring 3, using the same
rasterizer, into its own memory; that is what every Wayland client does.
The honest limit: a loaded face is rasterized into the same 101-glyph
set the baked one carries — ASCII plus six Nordic letters — so its other
few thousand glyphs are parsed and unreachable until UTF-8 lands, and
kerning is read from the legacy `kern` table only, so a face that keeps
its kerning in GPOS renders unkerned.

Double-buffered rendering with damage-region clipping, and a
compositor whose damage invariant is enforced by a verification mode
that re-renders each frame unrestricted and reports any pixel that
changed without being declared — over the chrome, desktop, taskbar,
menus and cursor it draws itself. A *client's* content is another
process's memory with no buffer-release handshake to hold it still, so
those pixels are masked out rather than judged, which is the difference
between a check that finds real bugs and one that reports twenty-two
imaginary ones. Apps declare a layout rather than coordinates, and a page
too big for its window scrolls.

**Userland.** Every ring-3 program is just a `main()`: crt0 provides
`_start` over the standard SysV stack layout, and libsys gives one typed
wrapper per syscall. The shared kernel toolkit is compiled a second time
under the C names, so a ring-3 `strlen` and the kernel's `k_strlen`
cannot diverge — and the same rule gives ring 3 the kernel's own
allocator as `malloc`/`free`, and its line editor, so the two shells and
the physical one agree about what Ctrl-A does. Adding a program is a
`.c` file with no Makefile edit. Still deliberately not a libc — no
`realloc`, `FILE`, `printf` or `errno`.

**Syscalls.** One table maps each number to its handler, and the handlers
live with the subsystem that owns them — the shape Linux and NT both
settled on. The same row carries what `strace` prints, so tracing and
dispatch cannot disagree about which syscalls exist.

## Architecture

Directories are subsystems, not filing cabinets — where a file lives says
what kind of thing it is.

```
kernel/
  arch/x86_64/  anything a different CPU would need rewritten: boot and
                long mode, interrupts, GDT/TSS/IDT, PIC, paging, ASLR
  core/         bring-up and whole-machine concerns: kernel_main,
                multiboot, timers, serial + the debug console
  mm/           physical frames, address spaces, the kernel heap
  proc/         ELF64 loader, the syscall table, scheduler, window server
  fs/           TFS3 and TFS2 behind the probe-selecting VFS
  lib/          services with no hardware of their own: the shared
                toolkit (strings, numbers, formatting, paths, line
                editing), JSON, klog, /etc config, entropy
  drivers/      one piece of hardware each, plus the display registry
  include/      split by audience and ENFORCED by include paths: api/
                (what apps may use), abi/ (the kernel<->userland
                contract), kernel/ (internal, off apps/'s path)

apps/           kernel-space programs: the shell, the editor, the demo.
                No GUI lives here any more.

userland/       ring-3 programs, split by ROLE:
  rt/           crt0, libsys, linker script
  ui/           Toykit -- the toolkit clients program against
  lib/          non-UI libraries: the tosh shell, C-name string/stdio
  wm/           the window manager, itself a ring-3 program
  gui/          windowed apps      -> seeded to /bin
  bin/          command-line tools -> seeded to /bin
  tests/        single-mechanism diagnostics -> seeded to /tests

seed/, data/    what gets mirrored onto disk.img at build time
tools/          build, test and delivery tooling (see Development)
docs/           design records, specs, and the decision log
```

Source discovery is recursive, so a new file — or a whole directory —
under `kernel/` or `apps/` needs no Makefile edit. Header dependencies
are tracked, and `tools/check_deps.py` proves per build directory that
the tracking is actually live.

`kernel_main()` brings up the hardware and calls `apps_start()`, which
launches the shell without naming it: the shell is simply the first
entry in the app registry, and that seam keeps the kernel core ignorant
of what apps exist.

## Development

```bash
make verify                     # the full gate: clean build + iso + boot test + ktest
bash tools/preflight.sh         # same, plus a git status summary (~25s)
python3 tools/gui_regress.py    # every GUI test tool as one table (~1.5 min)
```

Writing a test is a `KTEST()` block in a `*_test.c` next to the code it
exercises. It registers itself through a linker section — no registry,
no Makefile edit:

```c
KTEST("mm", "kzalloc returns zeroed memory") {
    uint8_t *p = kzalloc(256);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 256; i++) KTEST_ASSERT_EQ(p[i], 0);
    kfree(p);
}
```

`kernel/include/kernel/fault_inject.h` can fail the next N disk writes,
disk reads or `kmalloc` calls, which is how the error paths are tested
at all.

Selected tools, each documented in its own docstring:

| Tool | What it's for |
|---|---|
| `vm.py` | Start a headless VM and run shell commands against it, getting **text** back: `vm.py run "fsck"`. Usually a better check than a screenshot. |
| `ktest_run.py`, `usertest_run.py`, `faulttest_run.py` | The in-kernel suite, the ring-3 `/tests` diagnostics, and the ones that fault ON PURPOSE — asserted against the kernel's crash report rather than an exit code they don't have. |
| `boot_smoke_test.py` | Fast "does it still boot cleanly", no GUI. |
| `gui_debug.py` | Asks the WM what it is doing — window rects, z-order, hit-testing, damage — instead of measuring a screenshot. |
| `gui_regress.py` | Every GUI test tool, each on its own fresh disk image and VM, as one pass/fail table. |
| `damage_sweep.py`, `damage_hunt.py` | Walk the interactions that break the compositor's damage invariant, over one seed or many. |
| `flake_hunt.py` | Runs one tool N times and reports which CHECKS failed and how often — a rate, not a verdict. |
| `pixel_probe.py` | Reads exact pixel values out of screenshots, so a rendering change is a number rather than an impression. |
| `frame_balance.py`, `mem_stress.py` | Physical memory: does a process's teardown return exactly what it took, and does the machine survive running out? The patterns written are address-derived, so two mappings sharing one frame is detectable. |
| `check_deps.py`, `check_layout.py`, `check_docs.py`, `check_dispatch.py`, `check_widget_ops.py` | The build's own invariants: header tracking is live, the disk matches its documented layout, the docs have no dead pointers, no dispatch chain has quietly grown big enough to want a table, and no widget's ops table is missing a slot it needs. |
| `qmp_test.py`, `gui_flow.py`, `shell_flow.py` | Drive the GUI over QEMU's QMP socket, with the mouse/keyboard gotchas already handled. |
| `tfs3_writer.py`, `tfs2_writer.py` | Read, write, inspect and corrupt-for-testing files inside a `disk.img` from the host, without booting. Each refuses the other's images. |
| `fs_switch_test.py` | Proves probe, wipefs, live `fsformat` both ways, and reboot persistence. |

`CLAUDE.md` documents the conventions and environment quirks in depth.

## Documentation

| Document | Contents |
|---|---|
| [docs/decisions.md](docs/decisions.md) | Topic-indexed answers to "why is this built this way?", over [docs/decisions/](docs/decisions/) — split by area. Start here when something looks odd. |
| [docs/roadmap.md](docs/roadmap.md) | What's planned, grouped into layers from the kernel up, with a "ready now" list and the known issues. |
| [docs/roadmap-details.md](docs/roadmap-details.md) | The per-item reasoning and test plans behind that list. |
| [docs/bugs.md](docs/bugs.md) | What is currently BROKEN, one line each, with the reproduction in roadmap-details. Separate from the roadmap because "not built yet" and "misbehaving" are different questions. |
| [docs/conventions/](docs/conventions/) | The conventions `CLAUDE.md` indexes by headline, written up in full and split by area: kernel, GUI, storage, shell, build. |
| [docs/testing.md](docs/testing.md) | How to run and drive this OS headlessly, the QMP mechanics, and what the emulator does not model. |
| [docs/tools.md](docs/tools.md) | Every script in `tools/`: what it does, why it exists, and the traps it encodes. |
| [docs/settings-and-queries.md](docs/settings-and-queries.md) | Facts vs settings vs tunables, and how an app reads or changes either. |
| [docs/commands.md](docs/commands.md) | The command index; [docs/commands/](docs/commands/) has one page each. |
| [docs/signals-design.md](docs/signals-design.md) | Signals, a foreground process, and what `Ctrl-C` needs. Designed, not built. |
| [docs/boot-flags.md](docs/boot-flags.md) | Every word the kernel looks for on the GRUB command line. |
| [docs/filesystem-layout.md](docs/filesystem-layout.md) | What lives where on the OS's own disk. Checked against the built image by `tools/check_layout.py`. |
| [docs/gui-guidelines.md](docs/gui-guidelines.md) | How the GUI should look and behave, and how to verify a change to it properly. |
| [docs/uapp-design.md](docs/uapp-design.md) | Toykit's design: how a ring-3 GUI app is written, and the staging that got there. |
| [docs/wm-ring3-design.md](docs/wm-ring3-design.md) | Milestone 41 — how the window manager was moved out of the kernel, stage by stage. Complete. |
| [docs/init-design.md](docs/init-design.md) | The staged plan for an init as pid 1, the process tree under it, and the shell moving to ring 3. |
| [docs/process-isolation.md](docs/process-isolation.md) | The full ring0/ring3 build-up, told as it was built, bugs included. |
| [docs/tfs3-spec.md](docs/tfs3-spec.md) / [design](docs/tfs3-design.md) | Byte-level format of the default filesystem, and the reasoning behind it. |
| [docs/tfs2-spec.md](docs/tfs2-spec.md) | Byte-level format of the legacy second backend. |
| [docs/live-cd-design.md](docs/live-cd-design.md) | How the Live CD carries a filesystem image as a GRUB module. |
| [docs/arch-portability.md](docs/arch-portability.md) | What is and isn't x86-64-specific, and what a second architecture would take. |
| [kernel/README.md](kernel/README.md), [apps/README.md](apps/README.md) | Where a new file goes, and how to add an app. |

## Releases

Tagged releases live on
[GitHub Releases](https://github.com/eveningworks/toy-os/releases). Each
ships `toy-os.iso`, a gzipped `disk.img.gz` and `run_release.sh`, a
standalone launcher needing no checkout. The disk image matters: there is
no installer yet, so the pre-seeded image is what puts `/bin/ls` and
friends on the filesystem — the ISO alone boots into a near-empty one.

## License

MIT — see [LICENSE](LICENSE). The baked JetBrains Mono glyph data in
`kernel/drivers/font_ttf.c` is separately covered by the SIL Open Font
License 1.1 ([tools/OFL.txt](tools/OFL.txt)); the two runtime-loadable
fonts in `data/fonts/` carry their own licenses beside them; and the
bundled `pci.ids` database in `data/` has its own terms — see LICENSE's
"Third-party font" and "Third-party data" sections.
