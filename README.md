<h1 align="center">toy-os</h1>

<p align="center">
  A hobby x86-64 operating system, written from scratch in C and assembly.<br>
  Boots via GRUB into a 64-bit kernel with a ring-3 desktop, networking,
  USB, sound and a journaling disk-backed filesystem.
</p>

<p align="center">
  <em>Written with Claude Code: a human makes the design calls, Claude does
  the implementation and the testing.</em>
</p>

<p align="center">
  <a href="https://github.com/eveningworks/toy-os/actions/workflows/build.yml">
    <img alt="release build" src="https://github.com/eveningworks/toy-os/actions/workflows/build.yml/badge.svg">
  </a>
  <img alt="Language" src="https://img.shields.io/badge/language-C%20%2B%20NASM-blue">
  <img alt="Target" src="https://img.shields.io/badge/target-x86__64-lightgrey">
  <img alt="Version" src="https://img.shields.io/badge/version-0.4.0--dev-orange">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

<p align="center">
  <img src="screenshots/readme/desktop.png" alt="toy-os desktop: the Image Viewer, the File Manager, Notepad, DOOM and a Terminal open at once" width="49%">
  <img src="screenshots/readme/shell.png" alt="toy-os Terminal: `doc ls` rendering the ls manual page, headings and code spans styled, paged" width="49%">
</p>

---

## Contents

- [What this is](#what-this-is)
- [Project status](#project-status)
- [Quick start](#quick-start)
- [How this was built](#how-this-was-built)
- [Development](#development)
- [Documentation](#documentation)
- [Releases](#releases)
- [License](#license)

## What this is

A hobby operating system with a paper trail: every subsystem built one at
a time, and every design decision written down with the reasoning behind
it -- including the ones that turned out wrong. It boots on real hardware
and under QEMU, and it does not stop at "hello world from the kernel":

- **The machine** — Multiboot2 and a long-mode transition done by hand, a
  physical frame allocator and per-process page tables, NX/W^X, SMEP/SMAP,
  stack canaries and kernel ASLR. ACPI tables are parsed, and the machine
  powers off through its own firmware methods rather than a fixed port.
- **Processes** — an ELF64 loader, a preemptive scheduler, an `init` as
  pid 1 that supervises services, pipes and `spawn`/`waitpid`, signals and
  job control, threads with real thread-local storage, and **dynamic
  linking** against `/lib/libc.so` and `/lib/libuapp.so`.
- **Storage** — TFS3, a journaling filesystem with `fsck`, beside FAT32
  and a RAM filesystem, on MBR/GPT partitions the kernel reads and writes.
  It **installs itself** onto another disk and that disk boots.
- **Networking** — ARP, IPv4, ICMP, UDP and client-side TCP over five NIC
  drivers, with DHCP, DNS, `ping`, `wget` and an `httpd` that serves this
  machine's own filesystem.
- **A desktop, and it is not in the kernel** — the window manager is a
  ring-3 process and so is every app: a file manager, a terminal with tabs,
  an image viewer, an audio player, Minesweeper and DOOM. They share one
  toolkit, down to the file chooser, which opens as a modal window of its
  own the way Windows' and KDE's do.
- **Sound** — AC'97, Intel HD Audio and USB Audio behind one device class,
  mixed by a ring-3 daemon; WAV and an MP3 decoder written here rather than
  vendored.
- **Its own manual** — `doc ls` on the machine renders the same page this
  repository holds, wrapped to whatever the terminal actually is.
- **Its own test suite** — in-kernel tests with deliberate fault injection,
  ring-3 diagnostics, and a GUI suite that drives the desktop over a serial
  channel and asserts on pixels.

**[docs/features.md](docs/features.md) is the long version** — what each of
those actually is, and where the interesting decisions were.
**[docs/architecture.md](docs/architecture.md)** is the map of the tree.

No cross-compiler is needed: host and target are both x86-64, so the system
GCC works with `-ffreestanding` and kernel-appropriate flags.

## Project status

**Version 0.4.0-dev**; the latest release is
[v0.3.0](https://github.com/eveningworks/toy-os/releases/tag/v0.3.0). A
hobby project under active development, not production software.

**Works today** — booting on real hardware and under QEMU; the shell and
its line editor; three filesystems behind one mount table, with `fsck`
and live reformatting; partitions read and written, and an installer that
puts this system onto another disk; the ring-3 window manager and its
apps; fonts loaded and rasterized from disk at runtime; ring-3 processes
with pipes, `spawn`/`waitpid`, signals, threads and job control; `mmap`
with file-backed demand paging; dynamic linking, with tolibc shipped as
`/lib/libc.so`; loadable kernel modules, with `drivers.conf` saying
which drivers are built as `.ko` files and the e1000 loaded by PCI match
at boot; a TTY layer with pseudo-terminals, so `Ctrl-C` interrupts
a job and a full-screen editor runs in a Terminal window; USB — xHCI with
hubs, hot-plug, HID, Ethernet and audio; sound on three device classes,
mixed by `soundd`, including DOOM with music; an Intel display driver
that reads the panel's EDID and programs the mode itself, with runtime
resolution changes and backlight control; ACPI tables and firmware-driven
shutdown; networking on five NIC drivers — UDP, TCP, DHCP and DNS, so
`wget` fetches a real page off the internet and `httpd` serves this
machine's filesystem to a browser — one connection per child process
under `inetd`, which makes a handler an ordinary filter, and `netlog`
saying what this machine has connected to and which program did it; and
`doc`, the manual, on the machine. And the test suite: the in-kernel
tests, the ring-3 diagnostics, and the GUI tools `gui_regress.py` runs as
one table.

**Known gaps** — **BIOS/CSM boot only; UEFI does not work.** GRUB's EFI
build faults before the kernel runs, so a machine with CSM disabled will
not boot this. No SMP: other cores are discovered through the MADT and
every one of them reports offline. USB has no mass storage and no HID report-descriptor parsing. Dynamic linking is
eager-binding with no `dlopen`; `mmap`'s `MAP_SHARED` works only over a
named shared-memory object, and there is no `mprotect`; `fork` is
copy-on-write but `spawn` is still the door every program uses.
Swap has its area and its page-table encoding and nothing that pages out
yet. **No privilege model**: no user accounts and no permission checks,
so anything ring 3 can ask for, any process can ask for.
[docs/roadmap.md](docs/roadmap.md) tracks all of it, with a candid
known-issues list; `git log` and [docs/decisions.md](docs/decisions.md)
carry how each piece arrived and why.

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
| `grub-mkrescue` + `xorriso` + `mtools` | Builds the bootable ISO. `grub-mkrescue` needs all three, and the BIOS modules package (`grub-pc-bin` on Debian, `grub2-pc-modules` on Fedora, `grub2-i386-pc` on openSUSE) is easy to miss. The same three also make `disk.img` bootable: `grub-mkimage` builds the disk's `core.img` out of that modules package, and `mtools` writes the FAT32 `/boot` with no root and no loop device. |
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

That builds the kernel, seeds a disk image, installs GRUB and the kernel
onto it, and boots **the disk** in QEMU — no CD involved (`make run
BOOT=cd` boots the ISO instead). **init brings the desktop up on its own**; *Exit to
shell* in the Start menu drops to the `/>` prompt, and `gui` goes back.
For a text-only boot, `make iso KCMDLINE="target=text"`. PageUp/PageDown
scrolls the console history, including the boot log.

```bash
make            # kernel.bin + the userland ELF binaries
make iso        # + toy-os.iso; also seeds disk.img and installs GRUB on it
make run        # build + boot in QEMU with a graphical window
make live-iso   # a Live CD that boots with no disk attached at all
make usb-image  # a compact self-booting image to dd to a USB stick
make debug      # boot frozen (-s -S) for GDB
make test       # boot headless, run the in-kernel test suite
make verify     # full gate: clean build + iso + boot test + test suite
```

There is **one run target**; everything else is a variable on it, so
any combination works without a target per combination. Each device
class picks its implementation **by name** — `VIRTIO=1` just sets all
three at once, and a per-class value overrides it, so `VIRTIO=1
VGA=std` is legal. A name rather than a boolean because a boolean
cannot express a third one, and NVMe is on the roadmap. `make help`
lists every axis.

```bash
make run KVM=1          # KVM instead of emulation (needs /dev/kvm)
make run VIRTIO=1       # virtio for every device class: disk, GPU, input
make run DISK=virtio    # ...or one class at a time: virtio-blk, no IDE
make run DISK=ahci      #    a SATA drive behind an ICH9 HBA
make run VGA=virtio     #    the virtio-gpu driver
make run VGA=vmware     #    the adapter with a hardware cursor
make run INPUT=virtio   #    virtio keyboard, mouse and tablet
make run AUDIO=1        # an AC97 -- needed for any sound at all
make run AUDIO=hda      # ...or an Intel HD Audio controller with an output codec
make run AUDIO=usb      # ...or a USB audio card, on an xHCI controller
make run AUDIO=both     #    both, so the device picker has something to pick
make run WINDOW=full    # full-screen, pixel-exact, no decorations
make run WINDOW=fit     # a resizable window the guest is SCALED into
make run NOGRAPHIC=1    # serial console only -- use this over SSH
make run MENU=1         # show GRUB's menu instead of booting through
make run MEM=512        # a smaller machine
make run NET=virtio     # virtio-net instead of the e1000 (NET=none for no card)
make run NODISK=1       # no disk attached at all
make run LIVE=1         # the Live CD, no disk attached
```

<details>
<summary>Troubleshooting</summary>

| Symptom | Cause and fix |
|---|---|
| `grub-mkrescue not found` | Install GRUB's rescue tools (`grub-common` on Debian, `grub2-tools` on Fedora). The build looks for both `grub-mkrescue` and `grub2-mkrescue`. |
| `grub-mkrescue` fails on *"cannot find `xorriso`"* or mtools | Install `xorriso` **and** `mtools`; it needs both even for a BIOS-only image. |
| ISO builds but QEMU says *"no bootable device"* | The BIOS modules package is missing — `grub-pc-bin` (Debian), `grub2-pc-modules` (Fedora), `grub2-i386-pc` (openSUSE), `grub-bios` (Alpine). |
| No window appears (e.g. over SSH) | `make run NOGRAPHIC=1`. |
| The QEMU window runs off the screen | The window is the guest mode plus a title bar, sharing the screen with your panel, so `video=1920x1080` cannot fit *as a window* on a 1080px screen. Three trades: `WINDOW=full` (pixel-exact, no decorations), a shorter guest mode like `video=1920x1000` (windowed **and** pixel-exact — it need not be a standard mode), or `WINDOW=fit` (scaled into a resizable window, blurred font). Placement is your WM's; `-display sdl` has no position option. |
| The mouse doesn't move in QEMU | Don't add `-device usb-tablet`/`usb-mouse` by hand: QEMU routes pointer motion to a USB device once one is attached, and a PS/2 run then gets none. For the USB HID driver use `make run USB=xhci+mouse`. |
| Everything is very slow | `make run KVM=1` runs the CPU natively. That helps compute-bound code only — *ATA* disk I/O is ~1.9× **slower** under KVM, since every port-I/O instruction becomes a VM exit. That is ATA's penalty, not KVM's: `KVM=1 DISK=virtio` measures ~10× ATA's write throughput. |
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

Type `help` at the prompt, then `doc <command>` for the page --
those pages ship ON the machine, so `doc ls` reads the same file as
[docs/commands/ls.md](docs/commands/ls.md) here.
`doc -k <word>` searches names and summaries, `doc -K <word>` searches
every page's text. [docs/commands.md](docs/commands.md) is the same
reference on the host; [docs/boot-flags.md](docs/boot-flags.md) covers
what you can pass on the GRUB command line.

## How this was built

toy-os is written with [Claude Code](https://claude.com/claude-code),
Anthropic's agentic coding tool. A human makes the design calls; Claude does
the implementation and the testing.

The conventions it works under are in [CLAUDE.md](CLAUDE.md), the reasoning
behind the design is in [docs/decisions.md](docs/decisions.md), and
`tools/preflight.sh` is the gate every change passes before it lands.

## Development

```bash
make verify                     # the full gate: clean build + iso + boot test + ktest
bash tools/preflight.sh         # same, plus a git status summary
python3 tools/gui_regress.py    # every GUI test tool as one table
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
| `check_deps.py`, `check_layout.py`, `check_docs.py`, `check_dispatch.py`, `check_widget_ops.py`, `check_tool_commands.py` | The build's own invariants: header tracking is live, the disk matches its documented layout, the docs have no dead pointers, no dispatch chain has quietly grown big enough to want a table, no widget's ops table is missing a slot it needs, and no tool drives a guest command that has been renamed away. |
| `ondemand_sweep.py` | Runs the test tools that neither the gate nor `gui_regress.py` covers, and reports which have rotted — two were found red by accident after failing for an unknown period. A skip is counted apart from a pass. Never a gate. |
| `predates.py` | Answers "was this already broken?" by measuring: stashes the tree, rebuilds at HEAD, runs the command, restores, compares. |
| `qmp_test.py`, `gui_flow.py`, `shell_flow.py` | Drive the GUI over QEMU's QMP socket, with the mouse/keyboard gotchas already handled. |
| `tfs3_writer.py` | Read, write, inspect and corrupt-for-testing files inside a `disk.img` from the host, without booting. `--at-lba`/`--sectors` reach a filesystem inside a partition. |
| `fs_switch_test.py` | Proves probe, wipefs, live `fsformat`, and reboot persistence. |
| `fat32_test.py` | FAT32 and the mount table against an **independent implementation**: `mtools` reads back what the guest wrote and `fsck.fat` audits the volume. A self-test cannot catch an expectation being wrong; the strongest check is a 185 KiB binary extracted on the host and compared byte for byte. |
| `partition_test.py` | Boots with the filesystem inside an MBR or GPT partition. Its real check is `df`: a kernel ignoring partitions still boots, so "it booted" proves nothing. Its last phase proves `fsformat` cannot be aimed at the bootloader. |
| `install_grub.py` | Puts GRUB and the kernel onto `disk.img` -- the boot sector, `core.img` in the BIOS boot partition, `/boot` in the FAT32 one -- and answers which medium a launch should boot. |
| `regex_hostcheck.py` | tolibc's `<regex.h>` against **glibc's**, over one shared case table — an oracle sharing no code is the only thing that catches a wrong expectation. |
| `usb_test.py` | xHCI: a HID boot keyboard and mouse, hot-plug (QMP `device_add` on the running guest) and a `usb-hub` with both devices behind it. Self-controlling: QEMU routes keystrokes to `usb-kbd` once attached, so a broken driver receives nothing. Its real check is the ring wrap — past 256 TRBs, asserting the *last* file. |
| `kbd_test.py`, `keyboard_paths_test.py` | The input path, asserted on both drivers: that the same keys produce the same keycode and character over PS/2 and virtio-input, and that `kbd`'s four columns say what each stage really did. |

`CLAUDE.md` documents the conventions and environment quirks in depth.

## Documentation

| Document | Contents |
|---|---|
| [docs/features.md](docs/features.md) | What is built, layer by layer, and where the interesting decisions were. The long version of "What this is". |
| [docs/architecture.md](docs/architecture.md) | Which directory holds what, and why the boundaries are where they are. |
| [docs/decisions.md](docs/decisions.md) | Topic-indexed answers to "why is this built this way?", over [docs/decisions/](docs/decisions/) — split by area. Start here when something looks odd. |
| [docs/roadmap.md](docs/roadmap.md) | What's planned, grouped into layers from the kernel up, with a "ready now" list and the known issues. |
| [docs/roadmap-details.md](docs/roadmap-details.md) | The per-item reasoning and test plans behind that list. |
| [docs/bugs.md](docs/bugs.md) | What is currently BROKEN, one line each, with the reproduction in roadmap-details. Separate from the roadmap because "not built yet" and "misbehaving" are different questions. |
| [docs/conventions/](docs/conventions/) | The conventions `CLAUDE.md` indexes by headline, written up in full and split by area: kernel, GUI, storage, shell, build. |
| [docs/development-setup.md](docs/development-setup.md) | Setting up another machine or a fork: the packages, KVM and Docker groups, the SSH key, and the commit identity that fails silently. |
| [docs/testing.md](docs/testing.md) | How to run and drive this OS headlessly, the QMP mechanics, and what the emulator does not model. |
| [docs/tools.md](docs/tools.md) | Every script in `tools/`: what it does, why it exists, and the traps it encodes. |
| [docs/settings-and-queries.md](docs/settings-and-queries.md) | Facts vs settings vs tunables, and how an app reads or changes either. |
| [docs/query-design.md](docs/query-design.md) | How kernel state reaches ring 3, and why it is not `/proc`: `SYS_QUERY`, a self-describing registry, and a provider per fact. |
| [docs/dynlink-design.md](docs/dynlink-design.md) | Shared libraries: what they took, staged — and the honest case against them at this scale. |
| [docs/driver-guide.md](docs/driver-guide.md) | How to write a driver, ordered by the task rather than by topic. |
| [docs/devices.md](docs/devices.md) | Every driver in the tree, by class registry, and what each one claims. |
| [docs/libc-design.md](docs/libc-design.md) | `tolibc`, the C library — what it covers, and why its bar for adding a function is the opposite of the rest of the project. |
| [docs/commands.md](docs/commands.md) | The command index; [docs/commands/](docs/commands/) has one page each. |
| [docs/smp-design.md](docs/smp-design.md) | More than one core, staged — ACPI/MADT, the Local APIC, application processors, one kernel lock first and then splitting it. Stage 1 (the tables and the processor list) is built; the rest is designed, with the case against. |
| [docs/signals-design.md](docs/signals-design.md) | Signals, a foreground process, and what `Ctrl-C` needs. Every stage is built — delivery, dispositions, job control, and ring-3 handlers with a `SA_RESTORER` from userland. |
| [docs/tty-design.md](docs/tty-design.md) | The TTY layer: a terminal as an object, pseudo-terminals, and one implementation of `Ctrl-C` and `Ctrl-Z` for the console and a window alike. Stages 1–3 built; virtual terminals are what remain. |
| [docs/boot-flags.md](docs/boot-flags.md) | Every word the kernel looks for on the GRUB command line. |
| [docs/filesystem-layout.md](docs/filesystem-layout.md) | What lives where on the OS's own disk. Checked against the built image by `tools/check_layout.py`. |
| [docs/gui-guidelines.md](docs/gui-guidelines.md) | How the GUI should look and behave, and how to verify a change to it properly. |
| [docs/uapp-design.md](docs/uapp-design.md) | Toykit's design: how a ring-3 GUI app is written, and the staging that got there. |
| [docs/wm-ring3-design.md](docs/wm-ring3-design.md) | Milestone 41 — how the window manager was moved out of the kernel, stage by stage. Complete. |
| [docs/init-design.md](docs/init-design.md) | The staged plan for an init as pid 1, the process tree under it, and the shell moving to ring 3. |
| [docs/process-isolation.md](docs/process-isolation.md) | The full ring0/ring3 build-up, told as it was built, bugs included. |
| [docs/tfs3-spec.md](docs/tfs3-spec.md) / [design](docs/tfs3-design.md) | Byte-level format of the default filesystem, and the reasoning behind it. |
| [docs/tfs2-spec.md](docs/tfs2-spec.md) | Byte-level format of the removed TFS2 backend, kept for the record. |
| [docs/live-cd-design.md](docs/live-cd-design.md) | How the Live CD carries a filesystem image as a GRUB module. |
| [docs/arch-portability.md](docs/arch-portability.md) | What is and isn't x86-64-specific, and what a second architecture would take. |
| [kernel/README.md](kernel/README.md), [apps/README.md](apps/README.md) | Where a new file goes, and how to add an app. |

## Releases

Tagged releases live on
[GitHub Releases](https://github.com/eveningworks/toy-os/releases). Each
ships two media and a launcher:

| Asset | What it is |
|---|---|
| `toy-os-live.iso` | Boots with **no disk at all** — the filesystem rides in RAM as a GRUB module, and what you write to it is gone at power off. The one to try first. |
| `toyos-usb.img.gz` | A real 512 MB disk image. `gunzip`, `dd` it to a stick, and a machine boots it — and **keeps** what you write, which the live ISO does not. |
| `run_release.sh` | Boots a download in QEMU with the right flags — nothing to clone or build. It picks whichever medium it finds beside it, so it runs an older release's assets as well as a current one. |
| `SHA256SUMS` | Verify before you `dd`. |

From either medium, `install --disk <name> confirm` writes toy-os to an
internal drive and makes it boot. **Both media are BIOS/CSM only.**

**On real hardware.** `dd` overwrites the device you name, completely
and without asking — check it with `lsblk` first, and check the size.
Expect the rough edges of a hobby kernel: it programs the display, USB
and ACPI directly, so a machine it has not met before may hang partway
through boot, and [docs/boot-flags.md](docs/boot-flags.md) lists the
escape hatches (`nousb`, `noahci`, `nomsi`, `nogpe`, `nokaslr`). It does
not flash firmware, write EFI variables or touch anything outside the
disk you point it at, so the realistic worst case is a disk you told it
to erase. The MIT licence's warranty disclaimer applies, as to
everything here.

Releases before v0.3.0 shipped `toy-os.iso` plus a gzipped `disk.img`
instead; `run_release.sh` still understands that pair.

## License

MIT — see [LICENSE](LICENSE), which carries the full inventory of
third-party material. In short:

- **`userland/ports/doom/` is GPL-2-or-later** (doomgeneric). It builds
  into one binary that nothing else links, so it is an aggregation and
  the rest of the repository stays MIT. The Doom IWAD is **not** in this
  repository — `tools/fetch_wad.py` obtains one, under id Software's own
  terms.
- `userland/ports/cjson/` is MIT, under its own copyright.
- The five runtime-loadable fonts in `data/fonts/` are under the
  Bitstream Vera and SIL Open Font licenses, with each notice shipped
  beside the font.
- The baked JetBrains Mono glyph data in `kernel/drivers/font_ttf.c` is
  under the SIL Open Font License 1.1 ([tools/OFL.txt](tools/OFL.txt)).
- The bundled `pci.ids` and `usb.ids` databases in `data/` have their
  own terms.

`tools/check_licenses.py` fails the build if a vendored port or a
shipped font is missing from that inventory — the font list had already
drifted from two to five before it existed.
