# Loadable drivers: kernel modules, staged

**Status: all three stages BUILT 2026-09-10.** `kernel/core/module.c`
is the loader, `kernel/core/kexports.c` the export list, `drivers.conf`
says which drivers are modules (`e1000` by default), and
`modload`/`modunload`/`lsmod` are the commands. The rule is in
`docs/conventions/kernel.md` ("A DRIVER CAN BE A MODULE...") and the
decisions in `docs/decisions/drivers.md`. Two things below were WRONG
when this was designed and are corrected in place, marked **(corrected)**;
the rest is the research that shaped it, kept so it is not re-derived.

## What real systems do

**Linux:** a `.ko` is an ELF *relocatable object*. `insmod` links it
inside the kernel against the exported symbol table (`EXPORT_SYMBOL`,
resolved through kallsyms), gives it its own memory with RX text and RW
data, then runs its `module_init`, which registers into the same driver
registries a built-in driver uses. Boot-time modules come from an
initramfs; PCI hotplug loads by a match-table alias (`modalias`).
**Windows:** `.sys` PE images; boot-start versus demand-start is a
registry value, and PnP loads a driver when a device carrying its
hardware ID is enumerated. **FreeBSD:** `kld`, the same shape as Linux.
**macOS** went the other way -- DriverKit runs drivers as user
processes -- which toy-os has already done for the display server and
declined for USB and the network (no IOMMU, `docs/decisions/drivers.md`).

**The shape to copy:** an object file linked in-kernel against an
export table, registering through the registries that already exist.
**What not to copy:** modversions, module signing, a symbol namespace,
`depmod` -- baggage a one-author kernel has no reason to carry.

## What toy-os already has (measured 2026-09-09)

More than half of a loader, pointed in the wrong direction:

- **Every driver is DATA.** `DRIVER_DECLARE` drops a `struct
  driver_decl` into `.drivers`, `PCI_DRIVER` a `struct pci_driver` into
  `.pci_drivers`, `INITCALL` a `struct initcall` into `.initcalls`; the
  core walks the three sections (`kernel/core/driver.c`, `initcall.c`,
  `pci_bind()`). A module has only to hand the loader the same three
  tables.
- **A relocation tool and a symbol table.** `tools/genrelocs.py` walks
  `ld --emit-relocs` output for KASLR; `tools/gen_syms.py` bakes every
  kernel function's LINK-TIME address into `.ksyms` for panics
  (`ksyms_lookup()`). Both are what `insmod` needs, read the other way.
- **An ELF64 loader** (`kernel/proc/elf.c`) -- for `PT_LOAD` segments
  into a ring-3 address space, not for a relocatable `.o`; the header
  structs and the bounds discipline carry over, the segment walk does
  not.
- **W^X kernel memory**: `paging.c` rewrites the image's permissions
  from `__ktext_start`/`__kdata_start` at boot, and `vmm.h`'s mapping
  helpers take `writable`/`executable`.

**What was missing (all built now):** an in-kernel linker for `.o`
relocations, an EXPORT table (which symbols a module may reference, at
RUNTIME addresses -- with KASLR the baked `.ksyms` values carry the
boot's delta), per-module RX/RW memory, unload with a use count, and
the Makefile split saying which drivers are modules.

**(corrected)** Per-module RX memory is not a `kmalloc` allocation: the
identity map's RAM is NX after `paging_enforce_wx()`, so a module's
text needs its own page-aligned frames and `paging_set_kernel_exec()`
to flip them -- the loader takes one `pmm_alloc_contiguous()` run,
text first.

## Two findings that decide the shape

**Code model.** The kernel is `-mcmodel=kernel`: every reference is a
32-bit PC-relative or sign-extended absolute within 2 GiB of the image.
`kmalloc` memory may sit above 4 GiB (`docs/conventions/kernel.md`), so
a module placed there cannot reach the kernel by `R_X86_64_PC32`. Two
answers: a dedicated module VA range within 2 GiB of the kernel (what
Linux does, `MODULES_VADDR`), or compile modules with `-mcmodel=large
-fno-pic`, which turns every external reference into an `R_X86_64_64`
through `movabs` and lets a module live anywhere. **Stage (a) takes the
large model**: the loader then handles two relocation types
(`R_X86_64_64`, and `R_X86_64_PC32`/`PLT32` for a module's own
intra-section jumps) rather than a placement policy, and the cost -- a
few bytes per call site -- is nothing a driver notices. The VA-range
answer stays available if a module ever wants the kernel model.

**What a driver imports is small.** `e1000.c`, a full PCI NIC driver,
references about a dozen kernel functions: the `pci_*` helpers, `irq_
register_handler()`, `pmm_alloc_contiguous()`, `klog_*`, `k_mem*`,
`net_register()`/`net_rx()`, `driver_bound()`. The export table is
therefore a deliberate LIST, not "every global" -- `EXPORT_SYMBOL`'s
shape, generated into a section the same way `.ksyms` is, so a module
that reaches for something unexported fails to load with the symbol
named rather than linking against an internal.

**What modules will NOT buy:** a smaller `kernel.bin`. It is 5.6 MB and
most of that is `font_ttf.c`'s baked tables. The payoff is reloading a
driver on the laptop without a reboot, and a laptop image that does not
carry QEMU-only drivers.

## Stages

Each ships and is tested on its own; none is started.

- **(a) The loader and a hello module.** `EXPORT_SYMBOL(name)` into a
  `.kexports` section; a `modules/` directory OUTSIDE `kernel/` (every
  `.c` under `kernel/` is in the image), compiled with `-mcmodel=large
  -fno-pic` to `.ko` objects seeded into `/lib/modules/`; `SYS_MODLOAD`
  and `SYS_MODUNLOAD` taking a path and a name, behind `/bin/modload`
  and `/bin/modunload` (everyday commands are `/bin` programs); the
  loader reads the file into `kmalloc` memory (`fs_read_into`, sized by
  `fs_size`), allocates RX text and RW data pages, applies the
  relocations against `.kexports`, walks the module's own
  `.initcalls`/`.drivers`/`.pci_drivers`, and keeps a table of loaded
  modules (`QUERY_MODULES`, `lsmod`). `hello.ko` logs on load and
  unload and registers nothing. KTESTs: a relocation of each type
  against a synthetic object, an unexported symbol refused by name, a
  truncated file refused. Unload refuses a module whose use count is
  non-zero -- a bound driver holds one.
- **(b) One real driver.** `e1000` or `r8169`: a PCI driver whose match
  table and `probe()` are already the boundary `docs/driver-guide.md`
  names. What this stage has to add is the driver registries accepting
  a table that is not in the image's own section -- `driver.c`,
  `initcall.c` and `pci_bind()` walk a LIST of tables rather than one
  section -- and `pci_bind()` re-run against an unbound device when a
  module arrives. Test: `net_test.py` with the driver as a module, and
  `modunload` while the interface is up refused.
- **(c) Boot and on demand.** `/etc/modules`, one name per line, loaded
  by an initcall at the level PCI binding runs; and a load triggered by
  a PCI match -- a `modalias`-shaped table generated at build time from
  every module's match list, so an unbound device names the module that
  would take it. `docs/filesystem-layout.md` gains `/lib/modules`.
  **(corrected)** Not at the level PCI binding runs: `INIT_BUS` precedes
  `INIT_FS`, so nothing on disk is readable there. Boot loading is at
  `INIT_CONFIG`, and the loader re-binds unclaimed devices afterwards
  (`pci_rebind()`). The table is `/lib/modules/modules.alias`, written
  by `tools/gen_modalias.py`.

## Out of scope

- **Isolation.** A module is ring 0. A driver that must not be able to
  take the machine down is a process, and the display server is the
  precedent.
- **Versioning and signing.** One author, one tree, one build.
