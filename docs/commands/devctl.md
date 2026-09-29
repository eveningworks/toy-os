# devctl

**a `/bin` program.**

**Category:** System information

## Synopsis

```
devctl [list] | devctl disable [-p] ID | devctl enable ID | devctl apply
```

## Options

- `list` (or nothing) -- every device, grouped by type: its stable ID,
  the driver driving it, its state and its name.
- `disable ID` -- unbind the device's driver, so nothing drives it until
  it is enabled or the machine restarts.
- `disable -p ID` -- the same, and **keep it disabled after a restart**:
  it is written to `/etc/devices.conf`, which the `devices` service
  re-applies at boot.
- `enable ID` -- bind the driver again, and forget any `-p`.
- `apply` -- what the `devices` service runs at boot. Disables every
  present device `/etc/devices.conf` names.

## Description

The command-line half of the Device Manager; both are
`userland/lib/udevice.c`, so they cannot disagree.

```
/$ devctl
ID                   TYPE         DRIVER     STATE      NAME
pci:00:02.0          Display adap bochs      ok         VGA compatible controller
pci:00:05.0          Network adap e1000      ok         82540EM Gigabit Ethernet Controller
pci:00:04.0          Sound        ac97       ok         82801AA AC'97 Audio Controller
usb:5:0627:0001      Input device usb-hid    ok         QEMU Tablet
...
/$ devctl disable pci:00:04.0
devctl: disabled pci:00:04.0 (82801AA AC'97 Audio Controller)
```

An ID is stable across boots: a PCI device by its slot (`pci:00:04.0`),
a USB device by its root port and ids (`usb:2:2357:0601`). A persisted
entry also records the device's vendor:device ids, and **a different card
in the same slot is left alone** -- a re-plugged or reordered machine
cannot disable the wrong thing.

STATE is `ok`, `disabled`, `held` (a process holds it -- a ring-3
driver such as `hdad`), or `no-driver` (a device that wants a driver
and has none in this build -- every USB device but a hub, and a PCI
display, network, sound, storage, USB or input controller; a PCI bridge
or chipset function with no driver is simply `ok`).

## What it deliberately does not do

- **It cannot disable a device whose driver does not support it.** Disabling
  is an unbind, and the kernel's only gate on it is the driver's
  `remove()`. No storage controller has one, so the disk the root
  filesystem is on cannot be switched off -- the refusal names that.
- **"Persisted" means released right after boot, not never bound.**
  Drivers bind before the root filesystem, where `/etc/devices.conf`
  lives, is mounted; the `devices` service releases them again before
  `netd` and `soundd` start. Windows makes the same call for its
  boot-critical devices.
- **"Disabled" is recorded, not detected.** An unbound device looks the
  same to the kernel whether no driver matched it or somebody switched it
  off, so the state lives in `/etc/devices.conf` and, until the next
  restart, `/tmp/devices.disabled`.

## The trap

Spawn it; never `run` it. An unbind needs a scheduler slot, and the
legacy loader has none -- the refusal says so.
