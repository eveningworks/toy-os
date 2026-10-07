# devctl

**a `/bin` program.**

**Category:** System information

## Synopsis

```
devctl [list] | show ID | events [ID] | report [-a]
devctl disable [-p] ID | enable ID | apply
```

## Options

- `list` (or nothing) -- every device, grouped by type: its stable ID,
  the driver driving it, its state and its name.
- `show ID` -- everything known about one device, in sections: Problem
  (a driver that declined it, and why), Device, Connection (a network
  card), Display (a monitor), Disk and Partitions (a disk), Driver,
  Resources (memory and I/O ranges, the interrupt, a USB device's
  endpoints), Events. The same list the Device Manager's pane shows.
- `events [ID]` -- what happened to the devices this boot, oldest first:
  a driver took one (`Driven`), looked at one and said no (`Declined`),
  let one go (`Released`); a process took one (`Taken`) or gave it back;
  one was plugged in (`Added`) or pulled out (`Removed`). With an ID,
  that device's only.
- `report` -- the hardware report: a line naming the build, then every
  device under its type with its properties, resources and events. For a
  bug report or for writing a driver; **network addresses and MACs are
  left out** unless `-a`/`--addresses`.
- `disable ID` -- unbind the device's driver, so nothing drives it until
  it is enabled or the machine restarts.
- `disable -p ID` (or `--persist`) -- the same, and **keep it disabled
  after a restart**: it is written to `/etc/devices.conf`, which the
  `devices` service re-applies at boot.
- `enable ID` -- bind the driver again, and forget any `-p`.
- `apply` -- what the `devices` service runs at boot. Disables every
  present device `/etc/devices.conf` names.
- `-h`, `--help` -- every command and option in one page.

## Description

The command-line half of the Device Manager; both are
`userland/lib/udevice.c` (the list) and `udevice_info.c` (one device's
properties), so they cannot disagree.

```
/$ devctl
ID                   TYPE         DRIVER     STATE      NAME
pci:00:02.0          Display adap bochs      ok         VGA compatible controller
mon:0                Monitors     bochs      ok         QEMU Monitor
pci:00:05.0          Network adap e1000      ok         82540EM Gigabit Ethernet Controller
pci:00:04.0          Sound        hda        ok         82801I (ICH9 Family) HD Audio Controller
blk:ahci0            Disk drives  ahci       ok         QEMU HARDDISK
...
/$ devctl events pci:00:05.0
TIME       DEVICE               WHAT        DRIVER       DETAIL
0.68       pci:00:05.0          Driven      e1000        Driven by e1000
/$ devctl disable pci:00:04.0
devctl: disabled pci:00:04.0 (82801I (ICH9 Family) HD Audio Controller)
```

An ID is stable across boots: a PCI device by its slot (`pci:00:04.0`),
a USB device by its port and ids (`usb:2:2357:0601`), a disk by its block
name (`blk:ahci0`), the monitor as `mon:0`. A persisted entry also records
the device's vendor:device ids, and **a different card in the same slot is
left alone** -- a re-plugged or reordered machine cannot disable the wrong
thing.

STATE is `ok`, `disabled`, `held` (a process holds it -- a ring-3
driver such as `snddrv`), or `no-driver` (a device that wants a driver
and has none -- every USB device but a hub, and a PCI display, network,
sound, storage, USB or input controller; a PCI bridge or chipset
function with no driver is simply `ok`). A driver that looked at a
device and **declined** it -- not its model, no medium, a resource it
could not get -- leaves it `no-driver`, and `show` says which driver and
why.

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
- **Events are the device lifecycle, not the drivers' chatter.** What a
  driver logs about a device stays in the kernel log (`dmesg`); the
  event list is kept apart so it survives that log wrapping. It holds
  the latest 128.

## The trap

Spawn it; never `run` it. An unbind needs a scheduler slot, and the
legacy loader has none -- the refusal says so.
