# lspci

**a `/bin` program.**

**Category:** System information

## Synopsis

    lspci

## Description

`/bin/lspci`, with no builtin and **no ring-0 fallback** — the kernel-side device lister was deleted with the wrapper, so a damaged `/bin` has no way to list PCI devices. `dmesg` still logs what was found at boot.

Each nonzero BAR is printed as `barN=0x<addr>(mem)` or `(io)`; a memory
BAR carries its size after a slash (`/16K`, `/4M`), as probed by the
kernel at enumeration. An I/O BAR is not sized.

Two things are said about interrupts, and they answer different
questions. **What the device is on** comes first: `msix vector 48`,
`msi vector 48`, or `irq 11` for one still on its pin. A device that
took a vector had its INTx pin disabled with it, so its routed line is
deliberately not printed -- naming a line the device can no longer
assert would be worse than saying nothing. **What it could offer**
follows in brackets: `[msix/4]` (MSI-X, with the table size in entries)
or `[msi]`. A device with a capability and no vector is one whose driver
did not ask, or is not present at all:

    00:07.0  1af4:1041  ethernet controller  msix vector 51  [msix/4]  ...
    00:06.0  8086:2415  multimedia controller  irq 10  ...

QEMU's AC97, e1000 and ich9-ahci models advertise neither capability, so
those three show a bare `irq N` on any emulated boot however their
drivers are written.
