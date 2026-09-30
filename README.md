<h1 align="center">toy-os</h1>

<p align="center">
  A hobby x86-64 operating system, written from scratch in C and assembly.<br>
  Boots via GRUB into a 64-bit kernel with a ring-3 desktop, networking,
  USB, sound and a journaling disk-backed filesystem.
</p>

<p align="center">
  <a href="https://github.com/eveningworks/toy-os/actions/workflows/build.yml">
    <img alt="release build" src="https://github.com/eveningworks/toy-os/actions/workflows/build.yml/badge.svg">
  </a>
  <img alt="Language" src="https://img.shields.io/badge/language-C%20%2B%20NASM-blue">
  <img alt="Target" src="https://img.shields.io/badge/target-x86__64-lightgrey">
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

A hobby operating system built one subsystem at a time, with the
reasoning behind each design decision written down — including the ones
that turned out wrong. It boots on real hardware and under QEMU.

- **The machine** — Multiboot2 and a long-mode transition done by hand, a
  physical frame allocator and per-process page tables, NX/W^X, SMEP/SMAP,
  stack canaries and kernel ASLR. ACPI tables are parsed, and the machine
  powers off through its own firmware methods rather than a fixed port.
  The timer is **one-shot and tickless when idle**, the way Linux's
  hrtimers and NO_HZ work: a sleep ends within tens of microseconds of
  its deadline, and an idle machine stops interrupting itself.
- **Processes** — an ELF64 loader, a preemptive scheduler, an `init` as
  pid 1 that supervises services, pipes and `spawn`/`waitpid`, signals and
  job control, threads with real thread-local storage, and **dynamic
  linking** against `/lib/libc.so` and `/lib/libuapp.so`.
- **Storage** — TFS3, a journaling filesystem with `fsck`, beside FAT32
  and a RAM filesystem, on MBR/GPT partitions the kernel reads and writes,
  over IDE, AHCI, NVMe or virtio-blk -- including disks with 4096-byte
  sectors.
  It **installs itself** onto another disk and that disk boots, and it
  **updates itself** from a build server -- `update`, or the System
  Update window with its progress bar -- replacing only the files that
  changed, and finishing a library or kernel update at the next boot.
- **Networking** — ARP, IPv4 (with fragmentation), ICMP, UDP and client-side TCP over six NIC
  drivers, with DHCP, DNS, `ping`, `wget`, `speedtest` and an `httpd` that serves this
  machine's own filesystem.
- **A desktop, and it is not in the kernel** — the window manager is a
  ring-3 process and so is every app: a file manager, a terminal with tabs,
  an image viewer, an audio player, Minesweeper and DOOM. They share one
  toolkit, down to the file chooser, which opens as a modal window of its
  own the way Windows' and KDE's do. Minimize and maximize animate —
  four effects including a macOS-style genie, at five speeds, or off.
- **Display and input** — an Intel driver that reads the panel's EDID and
  programs the mode itself, with runtime resolution changes, panel
  fitting and backlight control, beside VESA, virtio-gpu and VMware
  adapters. USB HID devices are driven through their own **report
  descriptor**, parsed at bind time, so a five-button mouse has five
  buttons, 16-bit axes and horizontal scroll rather than the three
  buttons the boot protocol allows.
- **Sound** — AC'97, Intel HD Audio and USB Audio behind one device class,
  mixed by a ring-3 daemon; WAV and an MP3 decoder written here rather than
  vendored, and MIDI through a SoundFont synthesiser with a General MIDI
  bank generated here (or a real one, with `EXTRAS=1`).
- **Its own manual** — `doc ls` on the machine renders the same page this
  repository holds, wrapped to whatever the terminal actually is.
- **A log that outlives the boot** — the kernel ring holds a few hundred
  lines, so `logd` persists it and every service's output to `/var/log`,
  one plain-text file per boot. `log -p 3` reads three boots back, which
  is what an intermittent fault actually needs: the boot that went wrong
  compared against the good ones around it.
- **Its own test suite** — in-kernel tests with deliberate fault injection,
  ring-3 diagnostics, and a GUI suite that drives the desktop over a serial
  channel and asserts on pixels.

**[docs/features.md](docs/features.md) is the long version** — what each of
those actually is, and where the interesting decisions were.
**[docs/architecture.md](docs/architecture.md)** is the map of the tree.

No cross-compiler is needed: host and target are both x86-64, so the system
GCC works with `-ffreestanding` and kernel-appropriate flags.

## Project status

A hobby project under active development, not production software. The
current version is in [VERSION](VERSION); tagged builds are on
[Releases](https://github.com/eveningworks/toy-os/releases).

**Works today** — booting on real hardware and under QEMU; the shell and
its line editor; three filesystems behind one mount table, with `fsck`
and live reformatting; partitions read and written, and an installer that
puts this system onto another disk; the ring-3 window manager and its
apps; fonts loaded and rasterized from disk at runtime; ring-3 processes
with pipes, `spawn`/`waitpid`, signals, threads and job control; `mmap`
with file-backed demand paging; dynamic linking, with tolibc shipped as
`/lib/libc.so`; loadable kernel modules, with `build.conf` saying
which drivers are built as `.ko` files and the e1000 loaded by PCI match
at boot; a TTY layer with pseudo-terminals, so `Ctrl-C` interrupts
a job and a full-screen editor runs in a Terminal window; USB — xHCI with
hubs, hot-plug, report-protocol HID, Ethernet and audio; sound on three
device classes,
mixed by `soundd`, including DOOM with music; an Intel display driver
that reads the panel's EDID and programs the mode itself, with runtime
resolution changes and backlight control; ACPI tables and firmware-driven
shutdown; networking on six NIC drivers — UDP, TCP, DHCP and DNS, so
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
every one of them reports offline. USB has no mass storage. Dynamic
linking is eager-binding with no `dlopen`; `mmap`'s `MAP_SHARED` works only over a
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
| `python3` + Pillow | Build-time disk seeding and the test/dev tools. Pillow (`pip install pillow`) is needed for screenshots and by `preflight.sh`, whose `genttf.py --check` imports it. |

Verified firsthand on Arch/CachyOS, and on Ubuntu 24.04 in a Claude Code
cloud session (`docs/development-setup.md`); the Debian/Ubuntu list is
what CI installs when it runs — on a release tag or on demand, not on every
push, because what a clean-checkout build is uniquely good at is not
worth a gate that cries wolf. The rest are package-name translations of
the same requirements — corrections welcome.
</details>


### Build and run

```bash
git clone https://github.com/eveningworks/toy-os.git
cd toy-os
make run
```

That builds the kernel, seeds a disk image, installs GRUB and the kernel
onto it, and boots **the disk** in QEMU — no CD involved (`make run
BOOT=cd` boots the ISO instead). **init brings the desktop up on its
own**; *Exit to shell* in the Start menu drops to the `/>` prompt, and
`gui` goes back.
For a text-only boot, `make iso KCMDLINE="target=text"` (or `option
cmdline` in `build.conf`, below). PageUp/PageDown
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
cannot express a third one -- and NVMe was the fourth. `make help`
lists every axis.

```bash
make run KVM=1          # KVM instead of emulation (needs /dev/kvm)
make run VIRTIO=1       # virtio for every device class: disk, GPU, input
make run DISK=virtio    # ...or one class at a time: virtio-blk, no IDE
make run DISK=ahci      #    a SATA drive behind an ICH9 HBA
make run DISK=nvme      #    an NVMe SSD
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

### Configuring the build

**`build.conf`**, at the top of the tree, is how this checkout is built:
which drivers are loadable modules, then the build options. Edit it and
run `make iso`; only what a change affects is rebuilt. Some of its lines
(the last one is an example -- the shipped file leaves `cmdline` unset):

```ini
e1000 = module            # a driver built as /lib/modules/e1000.ko
option hz       = 1000    # the kernel's tick rate: 100, 250, 300, 500 or 1000
option tick     = idle    # stop the tick when nothing runs (or periodic)
option highres  = yes     # one-shot timer deadlines (no = check them per tick)
option cmdline  = target=text   # boot words baked into the media
```

A value on the command line wins for one build (`make iso HZ=250`),
and a misspelt name or value stops the build rather than being ignored.
The comments in the file list every option. Some also have a **boot
flag** that overrides them for one boot without rebuilding, such as
`nohz=off` and `highres=off` -- see
[docs/boot-flags.md](docs/boot-flags.md).

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
GDB stub, and `make run KDEBUG=1` boots with the kernel's OWN stub — the
one that also works on real hardware, over a serial port (`kdebug=ttySN`):

```bash
gdb build/kernel.bin -ex "target remote localhost:1234"   # make debug
gdb build/kernel.bin -ex "target remote localhost:1235"   # make run KDEBUG=1
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

The tools in `tools/` drive the system headlessly — booting guests,
running the suites, reading pixels and answering "was this already
broken?". The ones worth knowing first:

| Tool | What it's for |
|---|---|
| `vm.py` | Start a headless VM and run shell commands against it, getting **text** back: `vm.py exec "fsck"`. Usually a better check than a screenshot. |
| `boot_smoke_test.py` | Fast "does it still boot cleanly", no GUI. |
| `ktest_run.py`, `usertest_run.py` | The in-kernel suite and the ring-3 `/tests` diagnostics. |
| `gui_regress.py` | Every GUI test tool, each on its own fresh disk image and VM, as one pass/fail table. |
| `gui_debug.py` | Asks the window manager what it is doing — window rects, z-order, hit-testing, damage — instead of measuring a screenshot. |
| `predates.py` | Answers "was this already broken?" by measuring: stashes the tree, rebuilds at HEAD, runs the command, restores, compares. |

**[docs/tools.md](docs/tools.md) is the full reference** — every script,
why it exists, and the traps it encodes.

`CLAUDE.md` documents the conventions and environment quirks in depth.

## Documentation

Everything under [docs/](docs/) is written as the work happens, including
the decisions that turned out wrong. Start here:

| Document | Contents |
|---|---|
| [docs/features.md](docs/features.md) | What is built, layer by layer, and where the interesting decisions were. The long version of "What this is". |
| [docs/architecture.md](docs/architecture.md) | Which directory holds what, and why the boundaries are where they are. |
| [docs/decisions.md](docs/decisions.md) | "Why is this built this way?", indexed by topic over [docs/decisions/](docs/decisions/). **Start here when something looks odd.** |
| [docs/roadmap.md](docs/roadmap.md) | What is planned, grouped into layers from the kernel up. |
| [docs/bugs.md](docs/bugs.md) | What is currently broken, one line each — kept apart from the roadmap, because "not built yet" and "misbehaving" are different questions. |
| [docs/commands.md](docs/commands.md) | The command index; [docs/commands/](docs/commands/) has a page each, and those same pages ship on the machine. |
| [docs/testing.md](docs/testing.md) | How to drive this OS headlessly, the QMP mechanics, and what the emulator does not model. |
| [docs/tools.md](docs/tools.md) | Every script in `tools/`. |
| [docs/development-setup.md](docs/development-setup.md) | Setting up another machine or a fork. |
| [CLAUDE.md](CLAUDE.md) | The conventions a change is written under. |

Reference material for one subsystem lives beside it —
[driver-guide](docs/driver-guide.md) and [devices](docs/devices.md) for
drivers, [gui-guidelines](docs/gui-guidelines.md) and
[uapp-design](docs/uapp-design.md) for the desktop,
[tfs3-spec](docs/tfs3-spec.md) for the filesystem's byte layout,
[boot-flags](docs/boot-flags.md) for the GRUB command line, and a design
document per staged subsystem (signals, TTY, SMP, dynamic linking, init).
[kernel/README.md](kernel/README.md) and [apps/README.md](apps/README.md)
say where a new file goes.

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
