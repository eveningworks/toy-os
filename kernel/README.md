# kernel/

Directories here are **subsystems, not filing cabinets**. Where a file
lives is a statement about what kind of thing it is, and that's the
question to answer when adding one.

| Directory | Holds | The test for "does it belong here?" |
|---|---|---|
| `arch/x86_64/` | Multiboot entry, GDT/TSS, IDT, PIC, IRQ dispatch, page tables, the ring switch | Would this be rewritten wholesale on a different CPU architecture? |
| `core/` | `kernel_main`, multiboot parsing, timer, serial + the debug console, power | Does it own the machine as a whole, rather than one resource? |
| `mm/` | Physical frames, address spaces, the kernel heap | Is it about memory? |
| `proc/` | ELF loading, syscalls, processes, the scheduler | Is it about *running* something? |
| `fs/` | VFS dispatch + the TFS2 backend | Is it about files? |
| `drivers/` | Console, graphics, PS/2, ATA, PCI, partitions, speaker | Does it talk to a specific piece of hardware? |
| `lib/` | strings, JSON, klog, debug flags, `/etc` config, timezone/font/keyboard settings | Is it a service with no hardware and no policy of its own? |
| `include/` | Headers, split by audience | See `include/README.md` |

## Why this shape

It was two directories -- `core/` (33 files) and `drivers/` -- until the
2026-08-13 restructure. `core/` had accumulated five unrelated concerns,
and "where does this go?" had exactly one answer, so it kept growing.
Three specific things the split buys:

- **`arch/x86_64/` makes the portability question real.**
  `docs/arch-portability.md` discusses what's x86-specific, and a
  RISC-V port is in the roadmap's backlog; before this, that meant
  reading 19 files to find out which ones. Now it's a directory. The
  rule to hold the line: nothing outside `arch/` should contain `inb`/
  `outb`, inline assembly, or a control-register access.
- **A filesystem is not a device driver.** `tfs.c`/`vfs.c` sat in
  `drivers/` next to `ata.c`. The block device is a driver; the
  filesystem on top of it is a subsystem. Mount points (roadmap
  Milestone 16) will add backends here, not there.
- **`lib/` names the leftovers honestly.** `string.c`, `json.c`,
  `klog.c` and the `/etc` config readers aren't hardware bring-up and
  never were -- they were in `core/` because there was nowhere else.

## Adding a file

Answer the table's question. If two directories both seem right the
file is probably doing two things; if none fits, `core/` is the honest
default, but reach for it knowing that's how the old `core/` got to 33
files.

The build picks up any `.c` under `kernel/` automatically (recursive
`find` in the Makefile), so a new subsystem directory needs no build
change -- which also means **a `.c` file here is in the kernel image**,
whether or not you meant it to be.
