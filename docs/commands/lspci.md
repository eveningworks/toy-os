# lspci

**a `/bin` program.**

**Category:** System information

## Synopsis

    lspci

## Description

`/bin/lspci`, with no builtin and **no ring-0 fallback** — the kernel-side device lister was deleted with the wrapper, so a damaged `/bin` has no way to list PCI devices. `dmesg` still logs what was found at boot.