# The shape of the machine

**Category:** Getting started

## Description

toy-os is a hobby x86-64 operating system, written from scratch. It
boots through Multiboot2 and GRUB, and is written in freestanding C and
NASM. It has ring0/ring3 separation, per-process paging, an ELF64
loader, a preemptive scheduler, real syscalls, disk-backed filesystems,
and a windowing system whose compositor is an ordinary ring-3 process
with no special privileges beyond a framebuffer grant.

There is no cross-compiler. The host and the target are both x86-64, so
plain system gcc, ld and nasm with freestanding flags produce the
kernel. That removes an entire class of setup problem from anyone who
wants to build it.

## Read on

- `kernel` -- what runs in ring 0, and how it protects itself
- `processes` -- init, services, signals and jobs
- `filesystem` -- TFS3, FAT32, and the lock that keeps them safe
- `desktop` -- the window manager, applications and text
- `shell` -- the two shells, and what a builtin is for

Every command has its own page in this manual: `doc <name>` in a
shell, or the Help app, where a page's name in `this style` is a link.

## Why any of this

It is a hobby operating system, and nothing here needs to exist. What
makes it worth building is that every layer is small enough to hold in
your head at once, and that the decisions are written down where the
next person to look will find them.

The source tree's README.md has the architecture; docs/decisions.md has
why things are the way they are rather than the obvious way.
