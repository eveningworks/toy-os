# Architecture portability

An assessment of what it would take to make toy-os buildable for a
second CPU architecture (RISC-V 64 is used as the running example
throughout, since it's the most common "second arch" hobby-OS choice
and has an active QEMU `virt` machine target), and a phased plan for
getting there -- written up because it was asked for directly, not
because a second arch is being built now.

**Status: Phase 1 below is DONE** (2026-08-13). `kernel/arch/x86_64/`
exists and holds the unambiguously architecture-specific files; it
happened as part of a general directory restructure rather than as
portability work, but it's the same move this document proposed. The
remaining phases are still unscheduled. See `docs/roadmap.md` for where
that work would be tracked and `kernel/README.md` for what each
directory means today.

## tl;dr

Roughly **2,200-2,500 lines of C plus 3 assembly files** (`boot.asm`,
`context_switch.asm`, `isr.asm`) are architecture-specific, out of
about 30,000 lines of C in the repo (~24,700 in `kernel/`, ~5,300 in
`apps/`) -- under 10%. The rest (the
filesystem, the window manager, every GUI app, the scheduler's actual
round-robin *policy*, the font renderer, `apps/calc_engine.c`, etc.)
is already ordinary freestanding C with no CPU-specific content. This
is a smaller lift than it might look from the outside, but the parts
that ARE arch-specific are foundational -- boot, interrupts, paging,
port I/O -- so a second arch is still a real project (weeks, not
days), not a mechanical file-shuffle.

## What's already architecture-neutral

No asm, no inline asm, no port I/O -- these need zero changes to
support a second arch, once they're moved (a pure `git mv` plus header
path fixes, not a rewrite):

- `kernel/fs/` -- the whole filesystem stack: `tfs3.c` + `tfs.c` (the
  two backends) + `vfs.c` (the probe/dispatch layer) + `fs_test.c`.
  (Line counts rot -- run `wc -l` for today's numbers.)
- `userland/wm/*` (1,592 lines, split across `wm.c`/`wm_input.c`/
  `wm_render.c`/`desktop.c`/`context_menu.c`/`start_menu.c`) -- the
  window manager, aside from one bare `hlt` in `wm.c`'s idle wait
  (trivially wrapped, see below).
- `apps/calc_engine.c` (287 lines), `apps/ui/*` (the widget primitives
  -- see `apps/README.md`), `theme.h`.
- `kernel/drivers/font_ttf.c` (16,700+ generated lines) + `gfx.c` (337
  lines) -- font rendering and the framebuffer blit/blend primitives.
- The scheduler's round-robin *policy* in `scheduler.c` (289 lines) --
  only one `hlt` in the idle path.
- `kernel/lib/heap_core.c`/`heap.h` (the allocator, shared with ring 3's
  malloc since 2026-08-18) and
  `kernel/lib/json.c`/`json.h` (the JSON parser/serializer) -- plain
  freestanding C, no CPU-specific content.
- Most of `apps/*.c`, `etc_config.c`, `klog.c`, `string.c`.
- `kernel/proc/elf.c` -- aside from one machine-type check
  (`EM_X86_64` at line 90), which just needs an `#ifdef`/table entry
  per arch, not a rewrite.

## What's architecture-specific, and how hard each piece is

Ordered roughly by how much work a RISC-V port of each would be, easiest
first.

**Trivial (config/constant swap, hours not days):**
- `kernel/proc/elf.c`'s `EM_X86_64` check -- one more machine-type
  constant.
- The one bare `hlt` in `userland/wm/wm.c` and `scheduler.c` -- both just
  need a `cpu_idle()` wrapper (`hlt` on x86, `wfi` on RISC-V) behind a
  header, same insulation `kapi.h` already does for everything else.

**Moderate (logic ports directly, encoding/mechanism differs):**
- `kernel/arch/x86_64/paging.c` (68 lines) + `vmm.c` (168 lines) -- the actual
  page-table-walking *logic* (allocate, map, unmap, walk) is portable;
  the page table *entry format* isn't (x86 PML4, 4 levels, vs RISC-V
  Sv39/Sv48, 3-4 levels, different flag bit layout). Needs an
  arch-specific entry-encoding layer under a shared page-table-walker
  API, not a full rewrite of the calling code in `pmm.c`/`process.c`.
- `context_switch.asm` (47 lines) -- register save/restore is
  conceptually identical across archs (save callee-saved regs + stack
  pointer, swap, restore); it's already isolated behind
  `kernel/include/kernel/context_switch.h`'s C-callable interface, so this is
  "write a RISC-V version with the same signature," not "redesign the
  calling code."
- `kernel/drivers/pci.c` (166 lines) -- x86 uses port I/O
  (`CONFIG_ADDRESS`/`CONFIG_DATA`, ports 0xCF8/0xCFC); RISC-V PCI is
  MMIO/ECAM-based. Same protocol semantics, different access mechanism
  -- the PCI *logic* (config space layout, capability walking) carries
  over, the low-level read/write doesn't.
- `kernel/core/serial.c` (COM1, ports 0x3F8+) -- the UART chip
  itself (16550-compatible) is the same one RISC-V's QEMU `virt`
  machine exposes, just via MMIO instead of port I/O. Register-level
  protocol is portable; the access mechanism isn't.

**Substantial (no direct analog, needs its own subsystem):**
- `kernel/arch/x86_64/gdt.c` (111 lines) -- segmentation and the TSS are pure
  x86 concepts. RISC-V has neither; a RISC-V port doesn't port this
  file at all, it just doesn't exist on that arch (privilege level
  switching is handled entirely differently, via `mstatus`/`sstatus`).
- `kernel/arch/x86_64/idt.c` (278 lines) + `isr.asm` (126 lines, 48 stub
  macros + the `int 0x80` syscall vector) -- x86's IDT gate-table
  format and per-vector stub-per-interrupt model has no RISC-V analog;
  RISC-V uses a single trap vector (`mtvec`/`stvec`) with software
  dispatch on `mcause`. The *dispatch contract* the rest of the kernel
  sees (`isr_dispatch(regs)` gets called with a normalized register
  frame) generalizes fine -- it's the mechanism that gets to that call
  that's a full rewrite per arch.
- `kernel/arch/x86_64/pic.c` (8259 PIC, port I/O) -- no RISC-V equivalent at
  all; needs a PLIC (Platform-Level Interrupt Controller, MMIO-based)
  driver instead, not a port of this file.
- `kernel/core/timer.c` (PIT + CMOS/RTC, port I/O) -- RISC-V has
  neither; needs a CLINT (timer) driver plus a separate RTC story
  (QEMU `virt` has no built-in RTC the way PC BIOS does).
- `kernel/drivers/i8042.c`/`keyboard.c`/`mouse.c` (PS/2, port I/O) --
  no RISC-V PS/2 controller exists on `virt`; would need a different
  input path entirely (e.g. virtio-input), which is a real driver
  project on its own, independent of everything else in this list.
- `kernel/drivers/ata.c` (440 lines, port-I/O PIO) -- no ATA/IDE on
  RISC-V `virt`; disk access would go through virtio-blk instead, a
  different driver from the ground up (though it would sit behind the
  same `vfs.c` this repo already has, so nothing above it changes).
- `kernel/core/power.c` -- the QEMU-reset-via-port-0x64 trick is
  x86-only; RISC-V shutdown/reset goes through an SBI call instead.
- `boot.asm` (208 lines) -- Multiboot2 + GRUB, CPUID checks, hand-built
  identity-mapped page tables, the 32-to-64-bit long-mode transition:
  all specifically about how a PC BIOS/GRUB hands control to an x86-64
  kernel. RISC-V boots completely differently (OpenSBI/U-Boot hands off
  via a flattened device tree, not Multiboot2) -- this file doesn't
  port, it gets replaced outright by an arch-specific equivalent.

**A second, lower boundary worth calling out:** every `userland/*.c`
test binary independently hand-rolls its own `int $0x80` syscall
trampoline -- there's no shared userland syscall wrapper today. A
RISC-V port would need an `ecall`-based twin of
`kernel/include/abi/syscall_abi.h`'s contract, and this is also a good
moment to stop the duplication and give userland one shared trampoline
function instead of ~11 copies of it, independent of the portability
work itself.

## `kapi.h` as the existing insulation boundary

`kernel/include/api/kapi.h` (44 lines) is already a clean boundary: it's a
pure aggregating header with zero asm or port I/O of its own, and
`apps/` code never includes a driver header directly or calls
`inb`/`outb`/inline asm -- with exactly one exception (the bare `hlt`
in `wm.c`'s idle wait, noted above). That means the ~27,500 lines of
`apps/` code and everything reachable only through `kapi.h` needs *no
changes* to support a second arch -- the boundary this project already
enforces for unrelated reasons (see `CLAUDE.md`) turns out to double as
the portability boundary almost for free.

## Directory layout

This section described a proposal; most of it is now simply the layout.
What actually exists (see `kernel/README.md`):

```
kernel/
  arch/x86_64/   boot.asm context_switch.asm isr.asm       <- exists
                 gdt.c idt.c pic.c irq.c paging.c
    riscv64/     (mirror image, once/if that port starts)  <- doesn't
  mm/            pmm.c vmm.c heap_os.c                     <- exists
  proc/          process.c scheduler.c elf.c elf_run.c
                 syscall.c ring3_test.c
  fs/            vfs.c tfs3.c tfs.c fs_test.c
  lib/           string.c json.c klog.c etc_config.c ...
  core/          kernel.c multiboot.c timer.c serial.c
                 power.c debug_console.c
  drivers/       vga.c gfx.c ata.c pci.c mouse.c ...
  test/          ktest.c fault_inject.c
apps/            (unchanged -- already insulated via kapi.h/wm.h)
```

Three differences from what this document originally proposed, worth
knowing before planning the next phase:

- The split went further than `arch/` + `core/`: `mm/`, `proc/`, `fs/`,
  `lib/` and `test/` exist too, so "whatever's left" is now itself
  organised by concern rather than being a bucket.
- **`timer.c`, `power.c`, `serial.c` and `pci.c` did NOT move to
  `arch/`**, though this document listed them. Each is a mix of port-I/O
  mechanism and portable logic, and moving them wholesale would put
  portable code in an arch directory -- extracting the split is Phase 2
  below, and it hasn't happened.
- `paging.c` moved wholesale despite the same mixed character, because
  its portable part (the walk) is small relative to the x86 page-table
  entry encoding that dominates it. `vmm.c` stayed in `mm/` for the
  mirror-image reason.

The line to hold, now that the directory exists: **nothing outside
`arch/` should contain `inb`/`outb`, inline assembly, or a
control-register access.** That's checkable with a grep, and it's what
keeps the boundary from eroding between now and whenever a second arch
starts.

## Phased plan

This is a plan for *if/when* a second arch gets picked up -- nothing
here is scheduled. Phases are ordered so each one leaves the tree in a
working, still-boots-on-x86_64 state; none of them require the second
arch to actually exist yet.

1. ~~**Mechanical move, zero behavior change.**~~ **DONE** (2026-08-13,
   as part of a general restructure -- see the git history).
   `kernel/arch/x86_64/` holds `boot.asm`, `context_switch.asm`,
   `isr.asm`, `gdt.c`, `idt.c`, `pic.c`, `irq.c` and `paging.c`.
   `timer.c`/`power.c` did not move, for the reason given above.
   Two notes for whoever does Phase 2: the Makefile no longer needs a
   wildcard per directory (source discovery is recursive now, so a new
   `arch/riscv64/` would be picked up automatically), and headers are
   split by audience under `kernel/include/{api,abi,kernel}/` with the
   boundary enforced by include paths -- an arch header belongs in
   `kernel/`.
2. **Split the mixed files.** Pull the port-I/O access functions out of
   `pci.c` and `serial.c` into `kernel/arch/x86_64/`, leaving the
   config-space/protocol logic in `kernel/drivers/`/`kernel/core/`
   behind a small function-pointer or weak-symbol interface. Same for
   `paging.c`'s entry-encoding vs. its walk logic. This is the phase
   most likely to surface a design question worth an `AskUserQuestion`
   round when it actually happens (function pointers vs. link-time
   selection vs. `#ifdef` -- real tradeoffs, not a one-line call).
3. **Introduce a syscall-arch boundary for userland.** Give the ~11
   `userland/*.c` test binaries one shared syscall-trampoline function
   instead of each hand-rolling `int $0x80`, behind a header that a
   RISC-V `ecall` version could later implement identically. Pure
   cleanup, no new arch involved yet, but a prerequisite for one.
4. **Add the second arch's boot path and trap/interrupt mechanism.**
   The genuinely new work -- `kernel/arch/riscv64/boot.S` (device-tree
   entry instead of Multiboot2), the `mtvec`-based trap dispatcher, a
   PLIC driver, a CLINT timer driver. This is where "weeks not days"
   applies; everything before this phase is preparation, this phase is
   the actual port.
5. **Bring up drivers with no direct analog.** Console/serial (mostly
   reusable, per above), then virtio-blk (replacing ATA) behind the
   existing `vfs.c`, then virtio-input (replacing PS/2) behind the
   existing keyboard/mouse consumer code in `apps/`. Each of these can
   land independently and be smoke-tested on its own.
6. **GUI/WM bring-up on the second arch.** By this point `userland/wm/*`
   and everything above `kapi.h` should need zero changes -- this phase
   is mostly "does it actually work," i.e. QMP-style GUI testing
   against the new arch's QEMU machine type, not new code.

## Recommendation

Given the size of the arch-specific surface (roughly a tenth of the
codebase) and that it's already well-insulated behind `kapi.h`, a
second arch is realistic *if wanted* -- but phases 1-3 above are worth
doing on their own merits regardless of whether RISC-V (or anything
else) ever actually gets built, since they're cleanup with no
downside: they make the x86-64-specific surface explicit and
contained instead of scattered by directory convention only, and they
remove the userland syscall-trampoline duplication noted in
`references`-style project files as a pre-existing smell. Phase 4
onward is real, scoped work that should only start once there's an
actual reason to run toy-os on a second machine type -- building it
speculatively risks carrying an unused arch tree that silently rots
(untested code is often wrong code, the same lesson this project's own
`CLAUDE.md` states about testing over re-reading).
