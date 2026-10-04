# Devices toy-os has drivers for

Every driver in the tree, by the class registry it plugs into. **The
live answer on a running machine is `lsdrv -a`**, which also names the
device each driver actually bound; a bare `lsdrv` lists only the ones
that bound something. This page is what exists in the source, not what
is present on your hardware.

`tools/check_docs.py` fails the build when a `DRIVER_DECLARE` in
`kernel/` has no row here, so the list cannot silently fall behind. It
does not check the *right-hand* column: what a driver claims is prose a
person has to keep true.

## Network (`net_device`)

| Driver | Claims | Notes |
|---|---|---|
| `r8169-kdb` | Realtek RTL8111/8168 (`10ec:8168`), RTL8101 (`10ec:8136`) | The kernel DEBUGGER's card on the Lenovo: with `kdebug=net,...` the onboard NIC is claimed before PCI binding and driven by polling, and the OS networks through a USB adapter instead. `r8169.c`'s bring-up order (re(4)'s) without the interrupt or the PHY kick, rings of 8 and 8. No emulator models this chip, so real hardware is its only test. `docs/kdebug-design.md`, stage 3b. |
| `e1000-kdb` | Intel 82540EM (`8086:100E`) | The kernel DEBUGGER's card, not the OS's: with `kdebug=net,...` the last 82540EM is claimed before PCI binding (`pci_device_claim()`, so `lspci` names its driver `kdebug`) and driven by polling alone -- no interrupt, no lock, no allocation after bring-up (`kdebug_nic.h`). Rings of 8 and 8, the minimum: RDLEN/TDLEN must be multiples of 128 bytes, and a 4-descriptor TX ring made the card resend a stale buffer. `docs/kdebug-design.md`, stage 3. |
| `e1000` | Intel 82540EM (`8086:100E`) | QEMU's default NIC. Legacy descriptors, no offload. Built as a MODULE by default (`build.conf`), loaded at boot by PCI match; the one driver with a `remove()`, so it can be unloaded and reloaded. |
| `r8169` | Realtek `10EC:8168` (RTL8111/8168/8211/8411), `10EC:8136` (RTL8101/8102/8106) | The Ethernet built into most x86 laptops. No emulator models it — see `docs/decisions.md`. Built as a MODULE by default (`build.conf`) with a `remove()`, so it reloads on the laptop without a reboot. |
| `virtio-net` | virtio net, modern and transitional | |
| `r8153` | Realtek `0BDA:8152`, `0BDA:8153`, TP-Link UE300 `2357:0601` | USB, Realtek's own vendor protocol rather than a class driver; the RTL8153 "A" steppings (`rtl8153.c` over the `rtl_usb.c` core). |
| `r8156` | Realtek `0BDA:8156` (RTL8156, RTL8156B) | The 2.5G part, same core, its own init/reset table (`rtl8156.c`). Link at 2.5 Gb/s proven by passthrough on 2026-09-09; the RTL8153B is still refused. |
| `cdc-ecm` | USB CDC Ethernet, by class | Receive is untested — `docs/bugs.md`. |

## Block (`block_device`)

| Driver | Claims | Notes |
|---|---|---|
| `nvme` | Any PCI NVMe controller (class `01:08:02`) | Every namespace becomes a disk (`nvme0`, `nvme1`, ...), 512- or 4096-byte blocks; one I/O queue on MSI-X. Beats AHCI for the root; `nonvme` steps down. |
| `ahci` | Any PCI SATA AHCI controller (class `01:06:01`) | The preferred disk on real hardware without NVMe. |
| `ata` | Legacy IDE, PIO and busmaster DMA | The fallback; `noahci` forces it. |
| `virtio-blk` | virtio block, modern and transitional | Preferred in a VM. |
| `ram` | No hardware | A RAM-backed device, for `ramfs` and tests. |

## Display (`display_driver`)

| Driver | Claims | Notes |
|---|---|---|
| `intel-display` | Intel gen8/gen9 display engine | gen8: eDP modeset, EDID over AUX, cursor plane, page flip, backlight, panel fitter. Gen9 (Kaby Lake): cursor plane and page flip at the firmware's mode, EDID over GMBUS. |
| `virtio-gpu` | virtio GPU | 2D modesetting; `-vga virtio`. |
| `bochs` | QEMU stdvga (`1234:1111`) | Modesetting. |
| `vmsvga` | VMware SVGA II (`15AD:0405`) | Modesetting; honours `video=WxH`. |
| `vesafb` | Whatever GRUB set up | No modesetting — the universal fallback. |

## Input (`input_source`)

| Driver | Claims | Notes |
|---|---|---|
| `i8042` | PS/2 keyboard and mouse | The path a default boot uses. |
| `usb-hid` | USB HID, **boot protocol only** | A non-boot-protocol device is skipped and says so; report-descriptor parsing is a roadmap item. Reads FIVE button bits out of the boot report: the spec defines three, and every real 5-button mouse puts the thumb pair in bits 3-4 of the same byte -- an assumption about hardware, stated in `docs/decisions.md`. |
| `virtio-input` | virtio keyboard, mouse, tablet | |

## Sound (`sound_device`)

| Driver | Claims | Notes |
|---|---|---|
| `hda` | Any PCI HD Audio controller (class `04:03`) | Generic codec walk, no vendor quirks; `hdadump` is the diagnostic. |
| `ac97` | Any PCI AC'97 codec (class `04:01`) | |
| `usb-audio` | USB audio class, UAC1 and UAC2 | Isochronous OUT; no feedback endpoint yet. |
| `ring3` | Nothing on a bus — a PROCESS binds it by calling `SYS_SND_REGISTER` | `/bin/hdad` is the one that does; `start()` is asynchronous there. See `docs/umdf-design.md`. |

## Buses, clocks and entropy

| Driver | Claims | Notes |
|---|---|---|
| `xhci` | Any PCI xHCI controller (class `0C:03`) | USB 3. The only USB host controller — there is no EHCI or UHCI driver. `nousb` skips it. |
| `tsc` | Invariant TSC | Calibrated against the boot clock (the PM timer where there is one, else the PIT); deadline-capable. |
| `acpi_pm` | ACPI PM timer, 3.579545 MHz | Named by the FADT; needs no calibration and runs without the tick, so a machine with no invariant TSC can still go tickless. 24 bits wrap in 4.7 s. |
| `pit` | 8253/8254, counted per tick | The last-resort clocksource; `clocksource=pit` forces it. Advances only from the tick, so it keeps the timer periodic. |
| `virtio-rng` | virtio entropy source | Seeds `krandom`, raising it above TSC jitter. |

## What there is no driver for

USB mass storage, Wi-Fi (the laptop's Intel 3160 is unclaimed),
NVIDIA or AMD graphics, EHCI/UHCI, PS/2-over-USB legacy emulation, and
any Ethernet controller not listed above. `lspci` on a machine shows
what was found; a device with no driver is listed with none.
