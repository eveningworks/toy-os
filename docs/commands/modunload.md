# modunload

**a `/bin` program.**

**Category:** System administration

## Synopsis

```
modunload <name>
```

## Options

None.

## Description

Unloads a kernel module by the name `lsmod` shows. The module's drivers
release the devices they hold, its exit function runs, its tables leave
every registry, and its memory is freed.

```
/$ modunload e1000
/$ lsmod
lsmod: no module is loaded
/$ modload e1000
/$ ifconfig
```

**Refused with `Device or resource busy` when a driver in the module
holds a device and has no `remove()`** -- such a driver keeps its
device until reboot, and the module with it. `lsmod` shows those as
`(cannot unload)`. Nothing is unloaded partially: every held device is
checked before any is released.

Unloading a network driver takes its interface out of the stack;
`/bin/netd` notices within a few seconds and leases an address again
once the driver is back.

## See also

[`modload`](modload.md), [`lsmod`](lsmod.md).
