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
| `e1000` | Intel 82540EM (`8086:100E`) | QEMU's default NIC. Legacy descriptors, no offload. Built as a MODULE by default (`drivers.conf`), loaded at boot by PCI match; the one driver with a `remove()`, so it can be unloaded and reloaded. |
| `r8169` | Realtek `10EC:8168` (RTL8111/8168/8211/8411), `10EC:8136` (RTL8101/8102/8106) | The Ethernet built into most x86 laptops. No emulator models it — see `docs/decisions.md`. Built as a MODULE by default (`drivers.conf`) with a `remove()`, so it reloads on the laptop without a reboot. |
| `virtio-net` | virtio net, modern and transitional | |
| `r8153` | Realtek `0BDA:8152`, `0BDA:8153`, TP-Link UE300 `2357:0601` | USB, Realtek's own vendor protocol rather than a class driver; the RTL8153 "A" steppings (`rtl8153.c` over the `rtl_usb.c` core). |
| `r8156` | Realtek `0BDA:8156` (RTL8156, RTL8156B) | The 2.5G part, same core, its own init/reset table (`rtl8156.c`). Link at 2.5 Gb/s proven by passthrough on 2026-09-09; the RTL8153B is still refused. |
| `cdc-ecm` | USB CDC Ethernet, by class | Receive is untested — `docs/bugs.md`. |

## Block (`block_device`)

| Driver | Claims | Notes |
|---|---|---|
| `ahci` | Any PCI SATA AHCI controller (class `01:06:01`) | The preferred disk on real hardware. |
| `ata` | Legacy IDE, PIO and busmaster DMA | The fallback; `noahci` forces it. |
| `virtio-blk` | virtio block, modern and transitional | Preferred in a VM. |
| `ram` | No hardware | A RAM-backed device, for `ramfs` and tests. |

## Display (`display_driver`)

| Driver | Claims | Notes |
|---|---|---|
| `intel-display` | Intel gen8 display engine | eDP modeset, EDID over AUX, cursor plane, backlight, panel fitter. |
| `virtio-gpu` | virtio GPU | 2D modesetting; `-vga virtio`. |
| `bochs` | QEMU stdvga (`1234:1111`) | Modesetting. |
| `vmsvga` | VMware SVGA II (`15AD:0405`) | Modesetting; honours `video=WxH`. |
| `vesafb` | Whatever GRUB set up | No modesetting — the universal fallback. |

## Input (`input_source`)

| Driver | Claims | Notes |
|---|---|---|
| `i8042` | PS/2 keyboard and mouse | The path a default boot uses. |
| `usb-hid` | USB HID, **boot protocol only** | A non-boot-protocol device is skipped and says so; report-descriptor parsing is a roadmap item. |
| `virtio-input` | virtio keyboard, mouse, tablet | |

## Sound (`sound_device`)

| Driver | Claims | Notes |
|---|---|---|
| `hda` | Any PCI HD Audio controller (class `04:03`) | Generic codec walk, no vendor quirks; `hdadump` is the diagnostic. |
| `ac97` | Any PCI AC'97 codec (class `04:01`) | |
| `usb-audio` | USB audio class, UAC1 and UAC2 | Isochronous OUT; no feedback endpoint yet. |

## Buses, clocks and entropy

| Driver | Claims | Notes |
|---|---|---|
| `xhci` | Any PCI xHCI controller (class `0C:03`) | USB 3. The only USB host controller — there is no EHCI or UHCI driver. `nousb` skips it. |
| `tsc` | Invariant TSC | Calibrated against the PIT; deadline-capable. |
| `pit` | 8253/8254, 100 Hz | The fallback clocksource; `notsc` forces it. |
| `virtio-rng` | virtio entropy source | Seeds `krandom`, raising it above TSC jitter. |

## What there is no driver for

NVMe, USB mass storage, Wi-Fi (the laptop's Intel 3160 is unclaimed),
NVIDIA or AMD graphics, EHCI/UHCI, PS/2-over-USB legacy emulation, and
any Ethernet controller not listed above. `lspci` on a machine shows
what was found; a device with no driver is listed with none.
