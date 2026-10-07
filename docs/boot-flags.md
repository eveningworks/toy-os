# GRUB command-line flags

Words appended to the `multiboot2 /boot/kernel.bin` line in `grub.cfg`.
The kernel reads the raw string with `multiboot_cmdline()` and each
consumer looks for its own word with `k_strstr()` — so matching is by
SUBSTRING and there is no parser. A `key=value` word is read with
`multiboot_cmdline_value()`, which additionally requires the key to
start the line or follow a space, so `default_target=` is not
`target=`.

Two consequences worth knowing before adding one: a flag whose name
appears inside another word matches by accident (`pat` would fire on
`nopat`), and the flags are order-independent because nothing parses
position. Pick names that cannot be substrings of each other.

## The flags

| Flag | Effect | Read by |
|---|---|---|
| `video=<W>x<H>` | Asks a MODESETTING display driver for that screen size, between 640x480 and 3840x2160 (`DISPLAY_MAX_W/H` in `display.c`). Falls back down a ladder of standard sizes if the adapter refuses, and to GRUB's own mode if none work. **Only meaningful to a driver that can program the CRTC** -- `bochs`, `vmsvga` and `virtio-gpu` can (on `virtio-gpu` the ladder deliberately beats the host's own preferred size, so a flag you typed is not overruled by the size QEMU happens to be showing), a plain VESA framebuffer cannot, so on an adapter GRUB has already fixed this does nothing and that is not a bug. **`bochs` is why this flag now does something on the DEFAULT `-vga std`** -- before it, the ordinary adapter had no modesetting driver at all and this flag was inert on every default boot and every headless test. Above 1080p, note that a window still cannot exceed `WIN_CLIENT_MAX_W/H` (1920x1080), so a 4K desktop gets 4K of desktop and 1080p of window. Exists because a VESA BIOS with a short mode list (VirtualBox's VBoxVGA) leaves GRUB on 640x480 however big the multiboot header's preference was. **A stored `system.resolution` (System Settings, or `config set resolution`) is applied at the config stage of boot**, so an installed machine keeps its mode without this flag; the flag still decides the mode the console boots in, and a live ISO has nowhere to store one. | `kernel/drivers/display/display.c` |
| `debugcon` | Turns ON the serial debug console -- the `dbg>` prompt that `sh`, `gui`, `diag`, `ktest` and `readfile` answer on. **Off without it**: the console is an unauthenticated root shell on a serial port, so it listens only when asked, as Linux needs `kgdboc=` or a getty and Windows needs `bcdedit /ems on`. `make iso` bakes it in by default (`DEBUGCON=1`) because every test tool drives the guest through it; release media are built with `DEBUGCON=0`. It talks on COM2 when a second UART is present (what `tools/vm.py` gives a guest) and on COM1 otherwise; the kernel log stays on COM1 either way and is not affected by this flag. **`debugcon=ttyS0`** turns it on AND keeps it on COM1 even when COM2 answers the probe -- for a board whose Super I/O decodes a second port it has no connector for. Matched as a whole word. | `kernel/core/debug_console.c` |
| `kdebug=ttyS<N>[,wait]` | Arms the kernel's GDB stub (`kernel/debug/`) on serial port N, 1 to 3 -- a real `gdb` can then stop the machine, set breakpoints and hardware watchpoints, read and write memory and registers, and continue; a kernel fault or panic stops in it instead of going straight to the panic. **Off without it, and nothing from the running system can turn it on**: whoever holds that port owns the machine, the reason Linux needs `kgdboc=` and Windows `bcdedit /debug on`. `,wait` stops the boot at `kernel_main` until a debugger attaches (Linux's `kgdbwait`). `ttyS1` takes COM2 from the debug console, which moves to COM1; `make run KDEBUG=1` sets that up, `tools/vm.py --kdebug` gives a guest a COM3 for `ttyS2`. With it armed the idle tick never stops, because the tick is what hears a break-in. ttyS0 is refused: it is the log. `docs/kdebug-design.md`. | `kernel/debug/kdebug.c` |
| `kdebug=net,ip=<A.B.C.D>,key=<hex>[,port=<N>][,nic=<BB:DD.F>][,wait]` | Arms the same stub on the NETWORK: a NIC the debugger OWNS -- the last 82540EM on the bus, or the one `nic=` names -- claimed before any driver binds it, so the OS never sees it (KDNET's dedicated-card model). `ip=` is the stub's own address and it answers ARP for it; `port=` defaults to 50000. **`key=` is required, at least 16 bytes (32 hex digits), at most 64**: every datagram carries HMAC-SHA256 under it and a sequence number that must rise, so a forged or replayed one gets silence. Not encrypted -- a sniffer on the LAN can read what a session reads. GDB reaches it through `tools/kdebug_bridge.py`; `make run KDEBUG=net` sets up both ends. A word it cannot parse, or a machine with no card it can own, leaves the stub OFF and says so. | `kernel/debug/kdebug_net.c` |
| `novirtio` | Keeps the filesystem on the ATA disk even when a virtio-blk device is attached. virtio-blk is PREFERRED by default now -- about 10x ATA's write throughput under KVM, and the kernel test suite runs 6.4 s on it against 11.9 s on ATA -- so this is the switch that keeps the legacy path REACHABLE, and therefore tested. Same reason `nopat` and `notsc` exist. A machine with no virtio device is unaffected either way. | `kernel/drivers/block/block_virtio.c` |
| `noahci` | Keeps the filesystem off the SATA drive even when an AHCI controller has one, stepping the block layer's precedence down one rung to legacy IDE. The precedence (the tie-break when no `bootpart=` says otherwise) is virtio-blk, then NVMe, then AHCI, then ATA, so `novirtio`, `nonvme` and this are the switches that keep the lower rungs REACHABLE, and therefore tested. **It is not a driver kill switch**: the driver still finds the HBA, brings up the port and reports it through `/bin/ahci`; what changes is who carries the root. On a machine whose only disk is the SATA one, that means ramfs, which is the point -- `tools/ahci_test.py`'s third boot asserts exactly that. | `kernel/drivers/block/block_ahci.c` |
| `nonvme` | Registers no NVMe namespace with the block layer, stepping the precedence down past NVMe -- to AHCI, then ATA -- the way `noahci` steps past AHCI. The precedence is virtio-blk, then NVMe, then AHCI, then ATA. Like `noahci` it is **not a driver kill switch**: the controller is still brought up and its namespaces identified (the boot log says so); only the block layer is kept off them. On a machine whose only disk is NVMe that means ramfs. | `kernel/drivers/block/block_nvme.c` |
| `noncq` | Keeps AHCI to ONE command at a time: the block layer is not offered the driver's NCQ `submit_batch`, so a batch goes out one command after another exactly as before NCQ existed. The driver still reads the drive's queue depth and `/bin/ahci` still reports it. Two reasons it exists: the one-at-a-time path is what every device without NCQ takes, so it has to stay reachable; and it is the same-build A/B for what queuing buys -- Linux's `libata.force=noncq`. | `kernel/drivers/block/block_ahci.c` |
| `notrim` | Stops EVERY block backend discarding -- ATA's DSM, AHCI's, and virtio-blk's `VIRTIO_BLK_F_DISCARD`. Gated in one place (`blkdev_trim_supported()`), because three backends can discard and a word covering one would be worse than none. **It is a diagnostic A/B, not a preference**: a discard punches a hole in the host image, and a flush after one is far slower on some host filesystems than others, so when a machine starts failing journal barriers this answers "is it the trims?" in one boot instead of a kernel rebuild. The cost of leaving it on is that `disk.img` only ever grows. | `kernel/drivers/block/block.c` |
| `nogpe` | Skips the GPE quiesce before the S5 write, leaving a laptop's lid, embedded controller and USB events latched as they are. **A diagnostic A/B, not a preference**: clearing `PM1_STS` and disabling the GPEs landed together for one symptom -- a machine that rebooted instead of powering off -- and a machine that now stops is consistent with either, so this is what tells them apart in ONE boot instead of a bisect. Powers off with the flag = the `PM1_STS` clear was the fix; reboots with it = the GPEs were. **It has been run: an ASUS notebook with a 32-byte GPE0 block reboots with the flag and stops without it, so the GPE work is what that machine needed.** Kept because the next machine is a different machine. Same family as `nopat` and `notsc`. It announces itself on screen, because the failure it probes takes the log with it. | `kernel/acpi/acpi_power.c` |
| `acpidebug` | Prints the poweroff plan ON THE SCREEN and pauses about ten seconds before the S5 write: the sleep type and the PM1a port, whether ACPI mode actually came up, and the PM1 event and GPE blocks that get cleared first. **It exists because a machine that REBOOTS instead of stopping takes the evidence with it** -- the klog lines describing the attempt are gone before anyone can read them, and a live image has no disk to keep them on. Reach for it when `reboot --poweroff` restarts the machine. **Shut down from the TEXT target to read it**: from the desktop the compositor owns the screen, so the text is invisible and only the ten-second pause is observable. Off by default, because a shutdown that pauses is a worse shutdown. | `kernel/acpi/acpi_power.c` |
| `loglevel=<0-7>` | How much of the kernel log still reaches the CONSOLE -- the serial port, and the screen during boot. Linux's numbers: 2 crit, 3 err, 4 warn, 6 info (the default), 7 debug; every level at or below the number gets through. **The RING always keeps everything**, so `dmesg` shows what the console did not -- a threshold that dropped bytes from the log would be discarding exactly the evidence an intermittent fault needs, which this project has already paid for once. Read before the first line is logged, so it covers the boot it was typed for; a stored `system.loglevel` applies later and wins for everything after early boot, which is the order Linux's `loglevel=` and sysctl have. | `kernel/lib/klog.c` |
| `nokaslr` | Disables kernel ASLR — the kernel runs where it was linked instead of relocating itself to a random 2 MiB-aligned base. First thing to try when something breaks in a way that smells address-dependent. | `kernel/arch/x86_64/reloc.c` |
| `nomsi` | Keeps the Local APIC switched off, so every device stays on the 8259 PIC and its shared INTx lines. Without it the LAPIC comes up in virtual wire mode and the xHCI takes an MSI-X vector; with it, the whole machine is exactly what it was before either existed. **Two reasons it is a flag rather than a build option.** It is the fallback a CPU with no APIC takes anyway, so it has to keep working and therefore has to be reachable -- the same argument `nopat`, `notsc`, `novirtio` and `noahci` make. And an interrupt that stops arriving is one of the hardest failures to attribute: this answers "is it the APIC?" in one boot instead of a bisect. `tools/msi_test.py` documents the exact sequence. | `kernel/arch/x86_64/lapic.c` |
| `noioapic` | Keeps every interrupt line on the 8259 PIC even though the Local APIC is up, so the I/O APIC is never programmed and the PIC keeps reaching the CPU through LINT0 (virtual wire). MSI still works. The bisect flag for "a line stopped arriving": `nomsi` turns off both controllers at once, this one only the routing half. | `kernel/arch/x86_64/ioapic.c` |
| `nopat` | Forces the framebuffer's write-combining to go through an MTRR instead of PAT. Exists so the MTRR fallback is reachable — every machine this OS runs on has PAT, so without this switch that path could never be tested. | `kernel/arch/x86_64/paging.c` |
| `root=<device>` | Which device carries the root, by the name `lsblk` prints (`root=ahci0`, `root=ata0p3`). Overrides every other rule -- `bootpart=`, then what each disk holds. Naming a PARTITION also names its disk. A name this boot did not find is REPORTED (with the table it does have) and IGNORED, never fatal: a machine that will not boot because of one mistyped word gives its owner nothing to fix it with. Linux's `root=` and NT's BCD `osdevice`. | `kernel/fs/mount.c` |
| `bootpart=<partuuid>` | The partition GRUB loaded the kernel from, by PARTUUID (a GPT entry's unique GUID, or `<disk signature>-<nn>` on MBR) -- PASSED BY `grub.cfg` (`probe --part-uuid`), not typed. The disk carrying it is the root disk whatever its partitions hold, so a second disk -- blank, or another system's -- cannot take the root by sitting on a faster controller. Empty on the CD, which has no partition. Without it the kernel takes the first disk in driver precedence that has a boot partition, else a partition table, else the first. systemd's `LoaderDevicePartUUID`. | `kernel/fs/mount.c` |
| `usbtrace` | Prints a line per xHCI bring-up STEP. Off by default -- a healthy boot would carry a dozen lines forever. It exists because bring-up is a run of MMIO writes with NO output between them, so a machine that hangs in there shows its last message as whatever came before the whole sequence (the extended-capability walk) and points at the wrong place. On a laptop there is no serial, so the screen is the only channel. Also prints the scratchpad-buffer count the controller asks for, which QEMU reports as ZERO -- so the branch that allocates them has never run in any test here (`docs/bugs.md`). | `kernel/drivers/usb/xhci.c` |
| `hdadump` | Prints every widget of each HD Audio codec -- type, capabilities, pin configuration, amplifier capabilities, connection list -- and the speaker/headphone routes the driver chose, one klog line each. What a silent route on a new machine is diagnosed from: the generic walk in `hda.c` has no vendor quirks, so when a codec needs one, this is the evidence. ~15 lines on a laptop codec, at boot only. | `kernel/drivers/sound/hda.c` |
| `nousb` | Skips xHCI bring-up entirely, so no USB controller is touched. Unlike `noahci` this IS a driver kill switch, and it is here as a RESCUE flag rather than to keep a fallback tested: xHCI bring-up hangs one real laptop (`docs/bugs.md`), and a machine that hangs before a prompt has no way to tell you why. On a machine with no xHCI controller it changes nothing. | `kernel/drivers/usb/xhci.c` |
| `nor8169` | Leaves the built-in Realtek RTL8111/8168 Ethernet card alone, so its registers are never touched. A RESCUE flag, the same kind as `nousb`: no emulator models this chip, so unlike every other driver here its bring-up has only ever run on real hardware, and a machine it hangs cannot say so. Pair it with the "previous kernel" entry in `grub.cfg` -- this flag keeps the NEW kernel, which is what you want when the question is whether the NIC is the problem. Changes nothing on a machine without the card. | `kernel/drivers/net/r8169.c` |
| `notsc` | Keeps the TSC from becoming the clocksource, leaving the ACPI PM timer (or, without one, the PIT). Exists so the non-TSC paths stay reachable on a machine whose TSC is invariant — the mirror of `nopat`. `clocksource=pit` is the way to reach the coarse, tick-driven clock itself. | `kernel/arch/x86_64/clocksource_tsc.c` |
| `clocksource=<tsc\|acpi_pm\|pit>` | That clocksource wins whatever its rating, the moment it registers — Linux's parameter. A name that never registers (no invariant TSC, no PM timer) leaves rating order alone. `clocksource=pit` is the only way to run on the tick-driven clock on a machine with a PM timer, which also forces the periodic tick: one-shot mode needs a clock that runs without it. | `kernel/core/clocksource.c` |
| `highres=off` | Keeps the timer PERIODIC for this boot, as `option highres = no` does at build time: every deadline is checked once a tick instead of armed for. The fallback a machine without a LAPIC or a free-running clock takes anyway, so it has to be reachable. | `kernel/core/clockevent.c` |
| `nohz=off` | Keeps the tick running while idle, with one-shot deadlines otherwise (Linux's `nohz=off`, and `option tick = periodic` at build time). The A/B for anything suspected of depending on an idle tick — a poller that never asked for its wake. | `kernel/core/clockevent.c` |
| `target=<text\|graphical\|rescue>` | Overrides the persisted startup target (`system.default_target`) for this boot ONLY -- what init starts, per the descriptors in `/etc/services.d`. `text` starts the console shell (`/bin/tosh`, and the kernel's own shell stands down for it), so a desktop that faults on boot never costs you the machine; `graphical` forces a desktop on a machine configured for text. **`rescue` starts NO services at all**, so the kernel's own shell owns the console -- the shell that still works when the filesystem is too broken for `/bin/tosh` to load, and the one carrying the kernel introspection commands ring 3 has no equivalent for. systemd's `rescue.target`. The file is NOT rewritten, so the override shows up as a live-vs-stored difference in `config diff` and is discarded by `config reload`. Matching requires the word to start the line or follow a space, so `default_target=` -- the key's real name -- is not mistaken for it. | `kernel/lib/target.c` |
| `kbd=<name>` | Loads that keyboard layout for this boot ONLY, overriding the persisted `system.keyboard_layout` (`/usr/share/kbs/<name>`, generated by `tools/gen_kbs.py`). Exists for TESTING: a QMP qcode names a physical key by its US-layout label, so what the guest actually types depends on the layout the guest has loaded -- under the `se` default `/` arrives as `-`, which turned `spawn /bin/tosh` into `spawn -bin-tosh` and still passed a substring assertion. `kbd=us` makes a harness's punctuation mean what it says. The file is NOT rewritten, same as `target=`. A name with no table is ignored rather than fatal. | `kernel/lib/keyboard_config.c` |
| `live` | Forces the GRUB-module filesystem image to be mounted even when a real ATA disk is present. Without it the live image is used only when there is no disk. Only meaningful on `toy-os-live.iso`. | `kernel/fs/vfs.c` |
| `faultinject` | Arms the kernel's DELIBERATE fault table (`SYS_CRASHTEST`, and `config set kernel.crash <kind>` from a shell, `kernel/core/crashtest.c`), so the Crash Test app's Ring 0 buttons actually panic the machine. Off by default: a crash hole has no business being open on an ordinary boot, and the app shows the buttons either way and reports the refusal. |
| `panic=<seconds>` | What a kernel panic does after it has reported: count down that many seconds and RESTART, through the same reset as `reboot` minus its filesystem flush (`kernel/core/panic.c`). **Default 10** -- Windows restarts automatically, and Linux's `panic=N` is the same word. `panic=0` halts forever, as every panic did before; a negative value restarts at once. A key PRESSED on the PS/2 keyboard restarts early. It matters beyond convenience: the panic's log is kept in RAM (`kernel/panic_store.h`) and only a WARM reset carries it to the next boot, where logd appends it to the dead boot's log -- a machine left halted is power-cycled, and the record goes with it. A value that is not a number is refused and the default stands. | `kernel/core/panic.c` |
| `bootcfg.trial` | Marks a boot as a `bootcfg try` trial entry; nothing in the kernel reads it. `bootcfg` and the Boot Manager find it on the running command line (`QUERY_CMDLINE`) to tell a trial boot from the entry it was copied from, which comparing words cannot do once either has been edited. `try --keep` strips it. | `userland/lib/ubootcfg.c` |

## Setting one

**Once, without rebuilding** — at the GRUB menu press `e`, append the
word to the `multiboot2` line, then `Ctrl-X` to boot.

Note `grub.cfg` sets `timeout=0`, so no menu is drawn by default: hold
**Shift** during boot to force it, or build with `make run MENU=1`, which
sets a non-zero `GRUB_TIMEOUT`.

**On a running machine** — `bootcfg words 0 +nokaslr` (or the Boot
Manager app, or System Settings > System > Boot menu) edits the installed
`/boot/boot/grub/grub.cfg`, checked, keeping the old file as
`grub.cfg.bak`; `bootcfg try 0 +nokaslr` boots the change ONCE first.
Both accept only words in the table above (`--force` overrides).

**Permanently, in the build** — add the word to the `multiboot2` line in `grub.cfg`
and `make iso`. Note the repo-root `grub.cfg` is the SOURCE for two
generated copies -- `iso/boot/grub/grub.cfg` in the ISO and
`/boot/grub/grub.cfg` inside `disk.img`'s FAT32 boot partition, which is
the one an ordinary boot reads. Edit the repo-root file; editing either
copy is overwritten by the next build, and editing only one of them
gives you a machine whose two boot media disagree.

## Not flags, and why

Two things people reasonably expect to find here and will not.

**There is no flag for the legacy 80x25 text console.** `vga.c` has a
complete `0xB8000` backend and `vga_init()` falls back to it whenever
`gfx_init()` finds no usable linear framebuffer -- but you cannot ask for
that from the command line. `boot.asm`'s multiboot2 header carries a
framebuffer request tag, and GRUB acts on it *before* the kernel runs, so
by the time `multiboot_cmdline()` could be read the adapter is already in
a graphics mode and writes to `0xB8000` land nowhere visible. A GRUB menu
entry does not work either: `gfxpayload` is ignored for multiboot2
whenever the header requests a framebuffer, measured both with a specific
mode requested and with a 0/0/0 "no preference" header. Reaching that
backend needs a second kernel image built without the tag, or a runtime
VGA mode-3 switch by hand. See `docs/decisions.md`.

**There is no flag to turn console double buffering off.** The console
draws into `gfx.c`'s back buffer so that it never reads the framebuffer,
which is a correctness-shaped performance property rather than a
preference -- reading a write-combined surface costs ~350x more per
scrolled line on hardware. `gfxbench` reports which mode is live if you
need to confirm it.

## Adding a flag

Read the string with `multiboot_cmdline()` and match with `k_strstr()`,
the way the five above do. Two rules:

- **The flag turns something OFF, or turns a debug aid ON.** Nothing
  here should be load-bearing for a normal boot — a machine that needs a
  flag to work correctly has a bug, not a configuration.
- **Add a row to the table above.** It is the only list of these; the
  code has them spread across five files with no registry, which is
  fine for five and is exactly why they need to be written down
  somewhere.
  `tools/gen_bootwords.py` parses it at build time into the list
  `bootcfg` and the Boot Manager accept -- a word that is not here is
  refused there. Keep the shape: the word in backticks first in its
  cell, and an Effect whose first clause reads on its own, since that
  clause is the description the Boot Manager shows.

A setting that a *user* would want to keep belongs in `/etc/toyos.conf`
via `etc_config_get()`/`etc_config_set()` instead — see
`docs/filesystem-layout.md`. The command line is for decisions that have
to be made before the filesystem is mounted, or that exist only to make
a code path reachable for testing.

## Setting one without editing the menu

Every word above can be baked in at build time, instead of pressing `e`
in the GRUB menu and retyping it on each boot. `make iso` bakes it into
BOTH ordinary media -- the ISO and `disk.img`'s FAT32 `/boot/grub` --
from one repo-root `grub.cfg`, so it takes effect whichever one boots:

```
make iso       KCMDLINE="video=1920x1080"
make live-iso  KCMDLINE="video=1600x900 nokaslr"
```

**Where this list is repeated, and why.** GRUB's `e` editor shows the
selected menuentry's BODY and nothing else -- comments at the top of a
`grub*.cfg` are invisible there, which is precisely where someone about
to add a boot word is looking. So a four-line summary lives INSIDE each
menuentry as well, and it is kept short deliberately: the edit screen is
about twenty lines, so a full table would push `multiboot2` and `boot`
off it and make the screen worse rather than better. Verified by booting
the live ISO with a menu and screenshotting the editor, not by reasoning
about GRUB's parser.

`KCMDLINE` is empty by default, so every automated path -- the boot
smoke test, `ktest`, CI, `gui_regress.py` -- boots exactly as it did
before. The substitution happens in the `grub*.cfg` -> `iso*/` step, and
those files carry a one-screen summary of this table for whoever reads
them on the ISO itself.
