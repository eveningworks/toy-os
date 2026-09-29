# about

**a `/bin` program.**

**Category:** System information

## Synopsis

    about

## Description

`/bin/about` — the RUNNING KERNEL's version and build time, this program's
own version beside it, the machine the firmware says it is (`QUERY_SMBIOS`,
left out when the firmware names none), the boot method, and the filesystem
line (which it can print because `QUERY_FSINFO` exists).

**It reports two versions on purpose, and warns when they differ.** The kernel's
comes from `QUERY_VERSION` — the kernel's own copy of what it is. This program's
is compiled into it. They are the same on any machine installed from one image,
and they diverge the moment one is updated without the other, which is what
happens when a kernel or a binary is pushed over the network.

    /$ about
    toy-os v0.3.0 -- an x86-64 hobby kernel
      kernel:   0.3.0-dev (426601f)  built 2026-09-01 10:39:12
      userland: 0.3.0-dev (214d29e)  built 2026-08-31
      ** kernel and userland are from different builds **
    Machine: QEMU Standard PC (i440FX + PIIX, 1996)
    Boot: GRUB/Multiboot2 | C + ASM | Tested on QEMU
    Storage: tfs3, disk-backed (files persist across reboots)

The comparison is on the BUILD ID, not the version string: two builds of
`0.3.0-dev` from different commits are exactly the case this is for, and they
share a version. A kernel too old to carry the provider says so rather than
printing nothing — the absence dates it more precisely than silence would.

The same information is in the GUI About window, from the same provider, so the
two cannot disagree.