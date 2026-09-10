# modload

**a `/bin` program.**

**Category:** System administration

## Synopsis

```
modload <name | path>
```

## Options

None. A bare name loads `/lib/modules/<name>.ko`; an argument
containing a `/` is used as the path.

## Description

Loads a kernel module -- a driver built as a `.ko` rather than into
the kernel image (`drivers.conf` decides which). The kernel links it
against its export table, runs its initcalls, and binds any PCI device
its drivers match that no other driver has claimed.

```
/$ modload hello
/$ dmesg | grep hello
hello: loaded (1)
module: hello loaded at 0x1a3c000 (4 KiB text, 4 KiB data)
```

A module that is already loaded is refused (`File exists`). One that
reaches for a kernel function the export table does not carry is
refused with the symbol **named in the kernel log**:

```
/$ modload unexported
modload: /lib/modules/unexported.ko: Invalid argument
modload: the kernel log says why -- `dmesg`
/$ dmesg | tail -1
module: unexported: unknown symbol scheduler_kill (not exported -- see kernel/core/kexports.c)
```

The other refusals: `No such file`, `Exec format error` for anything
that is not a relocatable x86-64 object (a truncated file included),
`No space left` when the module table is full.

Most modules never need this command: at boot the kernel loads every
name in `/etc/modules`, then every module whose `modules.alias` line
matches a device present on the PCI bus. `modload` is for a module that
was unloaded, or one nothing matches.

## See also

[`modunload`](modunload.md), [`lsmod`](lsmod.md), [`lsdrv`](lsdrv.md),
[`dmesg`](dmesg.md).
