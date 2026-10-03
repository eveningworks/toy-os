# What toy-os does

The long version of [README.md](../README.md)'s summary: what is built,
how it works, and where the interesting decisions were. Ordered by
layer, from the machine upwards.

**Why this is not in the README.** It grew to a point where a first-time
reader met three screens of feature bullets before reaching `make run`.
The README keeps the pitch and the quick start; this file keeps the
depth. `docs/decisions.md` answers "why this way and not the obvious
way" for anything here, and `docs/roadmap.md` says what is not built.

---

**Boot and hardware.** Multiboot2 via GRUB2, with the 32→64-bit
long-mode transition done by hand. Linear RGB framebuffer falling back
to 80×25 VGA text. PS/2 keyboard and mouse sharing the 8042 through one
dispatcher, with keyboard layouts as *data files* generated from Linux's
own XKB data. PIT, CMOS RTC, PC speaker, MBR/GPT partition parsing.
The installed `grub.cfg` is edited from inside the OS by `bootcfg`
(grubby's shape, plus a visudo-style `edit`), checked before it is saved
-- unknown boot words, unclosed braces, a missing kernel -- with the old
file kept as `grub.cfg.bak`; `bootcfg try` boots a change ONCE through
GRUB's own `next_entry`, so a bad word costs one power-cycle. The same
model is in a window twice: the Boot Manager (each entry's words as a
checklist, the file as checked text) and System Settings > System > Boot
menu for the default, the timeout and the next restart.

Four block drivers behind one `block_device` registry the filesystems
never look through: legacy IDE with Bus-Master DMA and a PIO fallback,
**AHCI** off a mapped BAR5 with a per-page PRDT and interrupt-driven
completion, virtio-blk, and a RAM device — which is what a live boot
mounts, with no special case anywhere above it. The **virtio** stack
underneath is PCI
capability walking and 64-bit BAR decoding under a shared modern
transport — `virtio-blk` as the preferred disk (~**10× ATA's write
throughput under KVM**, because a virtqueue is shared memory with one
doorbell where ATA is dense with port I/O and every one of those is a VM
exit), `virtio-rng` feeding the entropy pool, **`virtio-gpu` as a real
display driver** (resource, scanout, transfer-and-flush, its own cursor
queue, and it programs the mode itself so `video=1920x1080` is honoured
rather than left to GRUB), and **`virtio-input`**. One transport, so the
next device is a driver rather than a bring-up project. On real
hardware, **an Intel display driver** for a laptop's Broadwell GPU. It
reads the panel's **EDID** over the DisplayPort AUX channel and
**re-programs the native mode itself** — the pipe cycle, the port clock
and DP link training — rather than inheriting whatever the firmware lit;
smaller modes are placed by the panel fitter under a `system.scaling`
setting, and the resolution can be changed at runtime. It puts the
pointer on the cursor plane, **page-flips across three scanouts** so a
present never waits, and drives the backlight — `system.brightness`,
with a slider in the tray.

Keyboards, mice and tablets from PS/2, virtio and USB all feed one
**input core** whose canonical event is evdev-shaped. Its diagnostic
(`kernel.kbdtap`, off by default) is what `kbd` prints as all four
encodings of one keypress — scancode, keycode, character, modifiers —
which is how a key that works on one keyboard and not another stops
being a mystery.

**USB**, because nothing built since roughly Skylake has a PS/2 port and
QEMU is the only reason that has not bitten yet. An **xHCI** driver —
command ring, event ring, doorbells, per-device contexts — with
enumeration on top and a **HID boot-protocol keyboard and mouse** that
register with the same input core, so they need no layout table of their
own. A **composite device binds every boot interface** (a wireless
receiver is a keyboard and a mouse on one plug), **USB2 hubs** work
(route strings, per-port power and reset, TT fields for a low-speed
mouse behind a high-speed hub), and **hot-plug** does too. Interrupt-
driven on an **MSI-X vector** where the controller offers one and on
legacy INTx where it does not, with an always-on polled backup either
way — a BIOS-reported INTx line can be plausible and dead. Two more
device classes ride on it: **USB Ethernet** (CDC ECM by class, plus an
RTL8153 vendor driver, because that adapter's ECM configuration does not
receive) and **USB Audio** (UAC1 and UAC2, on an isochronous endpoint).
`/bin/lsusb` names what is attached, from the ID database and the
device's own string descriptors under `-v`.
xHCI only, deliberately: UHCI and OHCI are a quarter of the code and run
on nothing made this decade. `make run USB=xhci+mouse` reaches it —
attaching a USB keyboard takes the keyboard *away* from PS/2, which is
what makes its test suite self-controlling.

**Networking, and the stack is in the kernel.** A `net_device` class
with five drivers behind it — Intel `e1000`, Realtek RTL8111/8168 and
RTL8101/8102, virtio-net, and two USB adapters — and above them ARP,
IPv4, ICMP, UDP and client-side TCP. A socket is an ordinary file
descriptor: a process opens it, reads and writes it, and a spawn can
hand one to a child, which is what makes `inetd`'s handler an ordinary
filter reading fd 0 and writing fd 1. The receive path is split across
an interrupt, and a blocking receive parks on a channel with a deadline
on the socket rather than a timer per wait.

**Nothing invents an address.** A card comes up unconfigured and
`/bin/netd` runs at boot to give it one: DHCP with real lease *renewal*
rather than a fresh request each time, falling back to link-local when
no server answers. Naming is a file, and an interface is named by what
the card is rather than by where it is plugged in. `ntpd` sets the
clock from a server; `SYS_SETTIME` takes nanoseconds so a sync does not
land a second late.

The programs on top: `ping`, `host`, `wget` (HTTP only — there is no
TLS, and a URL naming `https` is refused by name rather than attempted),
`netctl`, `httpd` serving this machine's own filesystem to a browser,
and `netlog`, which says what this machine has connected to and which
program opened it. **`telnetd` and `tftpd` ship disabled**, because
neither authenticates and there is no privilege model here to
authenticate against; `service enable telnetd` turns one on, and
`tools/remote.py` uses the pair to drive a bare-metal machine — commands,
file transfer, and replacing the kernel on its own boot partition.

**ACPI, as far as the tables and no further.** The RSDP comes from the
multiboot2 tag (with a BIOS-area scan behind it), the RSDT or XSDT is
walked and every table checksummed, and the FADT and MADT are decoded —
so **the machine powers off through its own firmware's numbers**: the
sleep type from the DSDT's `_S5_` object, the port from its own FADT,
which is what makes shutdown work on VirtualBox and real hardware
instead of only under QEMU, where the old hardcoded `outw(0x604,
0x2000)` was accidentally correct. `reboot` tries the FADT's reset
register before the 8042 pulse. Both are ladders with a halt at the
bottom, and each rung logs which one ran — the machine stops either way,
so the log is the only thing that can tell you. There is **no AML
interpreter**, and `_S5_` is the one deliberate exception: a `Name`
holding a `Package` of constants is data with a fixed grammar, and the
scan refuses any shape it does not recognise rather than guessing at a
sleep type. `/bin/acpi` prints the lot; the MADT half is SMP's first
stage, so the processor list exists and every core reports `online: no`.

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

**Filesystems.** [TFS3](tfs3-spec.md) is the root: block groups,
128-byte checksummed inodes, hardlinks, atomic rename and truncation,
32-slot journal transactions, ext-style superblock backups, ~590k files
on a 9 GiB volume. It scales files to gigabytes through direct and
single/double/triple-indirect pointers, batches ATA flushes, TRIMs freed
blocks back to the host, and refuses to touch a disk whose superblock
could not be read rather than destroying a possibly-good filesystem.
FAT32 is the second on-disk backend — read-write, with VFAT long names —
and exists because `/boot` is a FAT volume the machine boots from and
could not read. It is a generic driver: nothing in it knows it holds a
bootloader, and the read-only-by-default policy lives in the mount code,
the same split Linux keeps between `fs/fat/` and an ESP in `fstab`.
ramfs is the third, in the kernel heap, and is what a diskless boot gets
for a root. A mount table resolves a path to a backend by longest prefix
at a component boundary, so all three can be mounted at once.

**Graphics and GUI.** Real fonts, two ways: eight sizes of JetBrains
Mono baked to bitmaps at build time as the guaranteed fallback, and a
fixed-point TrueType rasterizer that loads a `.ttf` from
`/usr/share/fonts` at runtime — so any size works, and a proportional
face gets genuine per-glyph advances and real kerning. A face is a
*family*: bold is a second file, or synthesized by thickening the
regular outlines where a family has none, which is what GDI does.
Because the whole UI is font-*derived*, changing the face or size
reflows everything, live, without restarting anything.

Fonts come in **two tiers**. The session font is rasterized once in the
kernel and mapped read-only into every window, so all text matches the
desktop's setting by construction rather than by each app being
careful. An app needing something that font cannot express rasterizes
it *itself*, in ring 3, with the same rasterizer — what every Wayland
client does. The honest limit: a loaded face is rasterized into the
same 101-glyph set the baked one carries, so its other few thousand
glyphs are parsed and unreachable until UTF-8 lands, and kerning comes
from the legacy `kern` table only.

**Transparency.** The taskbar, Start, every menu (the desktop's and
each app's) and, by choice, windows can be glass: clear, frosted (a live
blur of whatever is behind, Windows 11's Acrylic) or the blurred
wallpaper alone (Mica), each surface at its own opacity, with windows
see-through by title bar, when inactive, always, or while dragged --
each surface choosing its own kind. A client only marks where its glass is; the compositor owns the effect,
as KDE's blur protocol has it. Off by default; Settings > Appearance >
Transparency.

**The tray's flyouts are one card**, drawn by one kit: network (the
adapter's state, a switch that takes it down, live traffic, its
address, Copy / Renew / Details), volume (each application, the output
device as a choice, Mute all) and brightness (the panel, its mode,
Scaling). The **on-screen keyboard** floats over the desktop, dragged by
its bar, and docks full width with one button.

**Screenshots are an overlay**, GNOME's shape: PrtSc freezes and dims
the screen under a pill -- Region (drag, then resize by its handles),
Screen, Window (the one under the pointer), the pointer, copy to the
clipboard, a delay -- and the shutter leaves a card in the corner with
the picture and Open / Copy / Folder. `/bin/screenshot` does the same
from a shell.

**Notepad** is a tabbed editor in Kate's and Windows 11 Notepad's shape:
a colour-coded command bar, a line-number gutter with the caret's line
tinted, find with every hit highlighted, a live Markdown preview beside
the source that scrolls with it, and undo and redo -- which every text
field on the desktop has, from one edit history in the toolkit. Undoing
back to what was saved makes a document clean again. **Disk Mark**
draws the run as it happens -- four meter cards and GNOME Disks' graph
of throughput over the run, its phases marked -- benchmarks any
writable disk volume, can be stopped, and keeps every run in a history
compared with the one before.

**Images are decoded in ring 3, and the kernel never sees one.** A
baseline JPEG decoder sits behind a codec table keyed on magic bytes,
so a second format is a row and a file rather than a branch. All fixed
point, since there is no floating point in either ring. Files it cannot
handle — progressive, arithmetic-coded, 12-bit, CMYK — are refused *by
name*, which is a different answer from "corrupt" and reads as one.
That is the opposite of the call made for fonts, which are parsed in
ring 0 because the console needs glyphs before any process exists;
nothing in ring 0 needs a picture. What proves it works is libjpeg
itself: the same source file is compiled on the host and compared
against libjpeg over a couple of hundred images, and a GUI tool checks
the framebuffer against libjpeg's decode of the wallpaper pixel for
pixel.

Double-buffered rendering with damage-region clipping, and a compositor
whose damage invariant is enforced by a verification mode that
re-renders each frame unrestricted and reports any pixel that changed
without being declared. A *client's* content is another process's
memory with no buffer-release handshake to hold it still, so those
pixels are masked out rather than judged — the difference between a
check that finds real bugs and one that reports twenty-two imaginary
ones. Apps declare a layout rather than coordinates, and a page too big
for its window scrolls.

**Sound.** An AC'97, an Intel HD Audio and a USB Audio Class driver
behind a `sound_device` registry, and a PCM
stream that is a **mapped ring** rather than a `write()` call — a control
page plus 64 KiB of samples at a fixed address, the app writing ahead of
the hardware and the kernel publishing the play position on each
completion interrupt. Steady state costs **zero syscalls**, and the
buffer is physically contiguous so the card's descriptors point straight
into it. A consumed chunk is **zeroed before the position passes it**,
which is the one rule that makes underruns free: zero is silence in
signed PCM, so a stalled or killed app degrades to quiet instead of
looping its last third of a second. The kernel stops there — it never
mixes, exactly as ALSA's dmix, PulseAudio and Windows' audio engine
never do it in kernel space. **A USB card is the second implementer of
that registry**, on an isochronous OUT endpoint the xHCI driver grew
for it — the format is refused rather than resampled (48 kHz stereo
s16, which is what the ABI fixes), the samples are copied into the
driver's own packet frame because 192 bytes per USB frame divides
neither the ring nor its chunks, and unplugging it mid-playback
publishes `device_gone` so an app can tell that from being stopped.
Which card plays is the first one discovered until somebody chooses in
the tray's volume flyout, and that choice persists. Above the line,
`userland/lib/usnd.h` is a
codec table — WAV and **MP3**, with the rate, channel and width
conversion happening once in the library and never in a codec — a
sixteen-voice mixer with stereo gains, and a sink interface whose second
row is **`soundd`**: a supervised service that owns the card and mixes
every program into it, so two can be audible at once. Its callers: an **Audio Player**, `/bin/aplay`, Minesweeper's
effects, and **DOOM** — whose sound effects are WAD lumps decoded into
voices, and whose **music is OPL synthesis**, Chocolate Doom's own
emulated Yamaha chip driven by the WAD's GENMIDI instrument bank. All of
it is judged on the host: QEMU records what the *device* emitted and the
tests measure that, so a 44.1 kHz file played at the wrong pitch, a dead
DMA engine and a broken zeroing each fail a different check.

**Userland.** Every ring-3 program is just a `main()`: crt0 provides
`_start` over the standard SysV stack layout, and libsys gives one typed
wrapper per syscall. The shared kernel toolkit is compiled a second time
under the C names, so a ring-3 `strlen` and the kernel's `k_strlen`
cannot diverge — and the same rule gives ring 3 the kernel's own
allocator as `malloc`/`free`, its line editor, and its ANSI parser — so
both shells agree about what Ctrl-A does, and the console and a Terminal
window agree about what `ESC[4;12H` means. Adding a program is a
`.c` file with no Makefile edit. Beside that sits **tolibc**
([docs/libc-design.md](libc-design.md)) — stdio, math, time,
dirent, setjmp and scanf — which is the one part of this tree that aims
to be COMPLETE rather than minimal, because its audience is code that
has not been written yet. It is also a **shared library**: the same
sources build `/lib/libc.so` (a second `-fpic` compile, the
compiled-twice pattern one axis over), the toolkit builds
`/lib/libuapp.so` beside it, `/lib/ld-toy.so` loads both into every
`/bin` and GUI program, and it stays *ours* on purpose — porting
musl was sized and declined, because musl is Linux-only by
construction and the port is really a Linux-syscall-compat project
(see `docs/decisions.md`).

**Syscalls.** One table maps each number to its handler, and the handlers
live with the subsystem that owns them — the shape Linux and NT both
settled on. The same row carries what `strace` prints, so tracing and
dispatch cannot disagree about which syscalls exist.

**Introspection.** Kernel state reaches ring 3 through one self-describing
registry rather than a `/proc` filesystem: a subsystem registers a
provider for a fact, and a command formats it — `ps`, `df`, `lsblk`,
`lspci`, `lsusb`, `lscpu`, `lsdrv`, `lsdisplay`, `lsshm`, `acpi`,
`meminfo`, `pmap`, `kstack`, `tty`, `kbd`, `netctl`, `netlog`,
`crashlog`, `service`. Answering *"what did the
machine actually do?"* is treated as a first-class job, distinct from a
test asserting it did the right thing: `strace` decodes a syscall per
line, `meminfo audit` compares every live address space against the
allocator, `tty` names who is holding the keyboard, and `kbd` prints a
keypress at every stage at once — PS/2 scancode, evdev keycode, the
character the layout produced, and the modifiers held — which is how a
key that works on one keyboard and not another stops being a mystery.
The log behind that last one is **off by default** and wipes when
switched off, because there is no privilege model here and a buffer of
recent keystrokes is not something to keep without being asked.

