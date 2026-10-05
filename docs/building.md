# Building and running toy-os

Everything between "I have a checkout" and "it is running, and I can test
a change": the tools to install, every way to boot it, the build
options, a release download on real hardware, and what to try when
something goes wrong. The [README](../README.md) has the three-line
version.

- [Dependencies](#dependencies)
- [Build and run](#build-and-run)
- [Every way to boot it](#every-way-to-boot-it)
- [Configuring the build](#configuring-the-build)
- [Using it](#using-it)
- [Running a release](#running-a-release)
- [Testing a change](#testing-a-change)
- [Troubleshooting](#troubleshooting)

No cross-compiler is needed: host and target are both x86-64, so the
system GCC works with `-ffreestanding` and kernel-appropriate flags.

## Dependencies

A C toolchain, NASM, GRUB's rescue-image tools, and QEMU.

| Distribution | Command |
|---|---|
| **Debian / Ubuntu / Mint** | `sudo apt install build-essential nasm grub-pc-bin grub-common xorriso mtools qemu-system-x86 python3` |
| **Arch / CachyOS / Manjaro** | `sudo pacman -S --needed base-devel nasm grub xorriso mtools qemu-full python` |
| **Fedora / RHEL / Rocky** | `sudo dnf install gcc make binutils nasm grub2-tools grub2-pc-modules xorriso mtools qemu-system-x86 python3` |
| **openSUSE** | `sudo zypper install gcc make binutils nasm grub2 grub2-i386-pc xorriso mtools qemu-x86 python3` |
| **Alpine** | `doas apk add build-base nasm grub grub-bios xorriso mtools qemu-system-x86_64 python3` |
| **Void** | `sudo xbps-install -S base-devel nasm grub xorriso mtools qemu python3` |

If your distribution names them differently, this is what each is for:

| Need | Why |
|---|---|
| `gcc`, `binutils`, `make` | Compiles and links the kernel. Any GCC that can target x86-64 works; no cross-compiler required. |
| `nasm` | Assembles the boot, interrupt and context-switch stubs. |
| `grub-mkrescue` + `xorriso` + `mtools` | Builds the bootable ISO. `grub-mkrescue` needs all three, and the BIOS modules package (`grub-pc-bin` on Debian, `grub2-pc-modules` on Fedora, `grub2-i386-pc` on openSUSE) is easy to miss. The same three also make `disk.img` bootable: `grub-mkimage` builds the disk's `core.img` out of that modules package, and `mtools` writes the FAT32 `/boot` with no root and no loop device. |
| `qemu-system-x86_64` | Runs it. |
| `python3` + Pillow | Build-time disk seeding and the test/dev tools. Pillow (`pip install pillow`) is needed for screenshots and by `preflight.sh`, whose `genttf.py --check` imports it. |

Verified firsthand on Arch/CachyOS, and on Ubuntu 24.04 in a Claude Code
cloud session ([development-setup.md](development-setup.md)); the
Debian/Ubuntu list is what CI installs when it runs — on a release tag or
on demand, not on every push. The rest are package-name translations of
the same requirements — corrections welcome.

## Build and run

```bash
git clone https://github.com/eveningworks/toy-os.git
cd toy-os
make run
```

That builds the kernel, seeds a disk image, installs GRUB and the kernel
onto it, and boots **the disk** in QEMU — no CD involved (`make run
BOOT=cd` boots the ISO instead). **init brings the desktop up on its
own**; *Exit to shell* in the Start menu drops to the `/>` prompt, and
`gui` goes back. For a text-only boot, `make iso KCMDLINE="target=text"`
(or `option cmdline` in `build.conf`, below). PageUp/PageDown scrolls the
console history, including the boot log.

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

## Every way to boot it

There is **one run target**; everything else is a variable on it, so any
combination works without a target per combination. Each device class
picks its implementation **by name** — `VIRTIO=1` just sets all three at
once, and a per-class value overrides it, so `VIRTIO=1 VGA=std` is legal.
A name rather than a boolean because a boolean cannot express a third
one — and NVMe was the fourth. `make help` lists every axis.

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

## Configuring the build

**`build.conf`**, at the top of the tree, is how this checkout is built:
which drivers are loadable modules, then the build options. Edit it and
run `make iso`; only what a change affects is rebuilt. Some of its lines
(the last one is an example — the shipped file leaves `cmdline` unset):

```ini
e1000 = module            # a driver built as /lib/modules/e1000.ko
option hz       = 1000    # the kernel's tick rate: 100, 250, 300, 500 or 1000
option tick     = idle    # stop the tick when nothing runs (or periodic)
option highres  = yes     # one-shot timer deadlines (no = check them per tick)
option cmdline  = target=text   # boot words baked into the media
```

A value on the command line wins for one build (`make iso HZ=250`), and a
misspelt name or value stops the build rather than being ignored. The
comments in the file list every option. Some also have a **boot flag**
that overrides them for one boot without rebuilding, such as `nohz=off`
and `highres=off` — see [boot-flags.md](boot-flags.md).

## Using it

Type `help` at the prompt, then `doc <command>` for the page — those
pages ship ON the machine, so `doc ls` reads the same file as
[commands/ls.md](commands/ls.md) here. `doc -k <word>` searches names and
summaries, `doc -K <word>` searches every page's text.
[commands.md](commands.md) is the same reference on the host;
[boot-flags.md](boot-flags.md) covers what you can pass on the GRUB
command line.

## Running a release

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

**On real hardware.** `dd` overwrites the device you name, completely and
without asking — check it with `lsblk` first, and check the size. Expect
the rough edges of a hobby kernel: it programs the display, USB and ACPI
directly, so a machine it has not met before may hang partway through
boot, and [boot-flags.md](boot-flags.md) lists the escape hatches
(`nousb`, `noahci`, `nomsi`, `nogpe`, `nokaslr`). It does not flash
firmware, write EFI variables or touch anything outside the disk you
point it at, so the realistic worst case is a disk you told it to erase.
**It is used at your own risk:** toy-os comes with no warranty (the MIT
licence's disclaimer applies, as to everything here), and nobody behind
this project takes responsibility for data lost or hardware affected by
running or installing it.

Releases before v0.3.0 shipped `toy-os.iso` plus a gzipped `disk.img`
instead; `run_release.sh` still understands that pair.

## Testing a change

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

[tools.md](tools.md) is the full reference — every script, why it
exists, and the traps it encodes — and [testing.md](testing.md) covers
driving the OS headlessly. [CLAUDE.md](../CLAUDE.md) documents the
conventions a change is written under.

## Troubleshooting

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
one that also works on real hardware, over a serial port
(`kdebug=ttySN`):

```bash
gdb build/kernel.bin -ex "target remote localhost:1234"   # make debug
gdb build/kernel.bin -ex "target remote localhost:1235"   # make run KDEBUG=1
```

Both `CFLAGS` and `USERLAND_CFLAGS` carry `-g`, so the kernel and every
userland ELF have real DWARF symbols. [testing.md](testing.md),
"Debugging with GDB", has the rest.
