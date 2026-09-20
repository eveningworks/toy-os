# A driver in ring 3

A staged plan, in the shape `docs/winserver-ring3-design.md` used. It
answers "what would it take to run a device driver as a process here,
what does that actually buy, and which driver goes first?"

**STAGES 1 AND 2 ARE BUILT (2026-09-20); stages 3-5 are not.** The stage
markers are the authority, and they are on the headings -- if a stage
ever splits, put its marker on each half (the window-server plan's
stage 6 split and its heading kept saying "outstanding" for eleven
days).

## The finding that shapes the whole plan

**Without an IOMMU, a ring-3 driver that can DMA is not a security
boundary.** It programs the device with a physical address; the device
writes there; no page table is consulted. A driver that can reach a
card's registers can point that card at any physical page on the
machine. Moving it out of ring 0 does not change that, and a design
claiming otherwise would be lying about its own threat model.

This machine has no IOMMU. `grep -rl iommu kernel/` finds one mention,
in a virtio header.

So the honest statement of what this buys is **crash isolation, not
containment**: a malformed codec response, a bad descriptor or a null
deref kills a restartable service instead of the machine, and the bug
lands in a log with a core-shaped story rather than a panic. That is
the same argument `fontd` and `clipboardd` already won here -- a font is
attacker-shaped input and a clipboard is untrusted data, and neither
moved for containment reasons either.

It is also why the stages below put the PARSING first and the DMA last:
the parsing is where the bugs are, and it is the half that needs no
trust at all.

## What the three real answers are

| system | how it resolves DMA in user mode |
|---|---|
| **Windows UMDF** | **forbids it.** UMDF drivers are for devices that do not master the bus -- USB, HID, portable devices. Anything with DMA is KMDF, in the kernel. |
| **macOS DriverKit** | **IOMMU.** A DriverKit driver maps buffers through `IODMACommand`, and the device is behind the DART/VT-d translation, so a wild address faults instead of landing. Apple moved audio there (AudioDriverKit) once that held. |
| **Linux VFIO** | **IOMMU, and it refuses without one.** `vfio-pci` will not give a group to userspace unless it is IOMMU-isolated, except under an explicit `enable_unsafe_noiommu_mode` that taints the kernel. |
| **Genode, seL4** | everything is user-mode, and both say in their own documentation that a DMA-capable driver without an IOMMU is inside the TCB. |

**toy-os takes UMDF's answer first and leaves DriverKit's as the exit.**
Stages 1-3 move only what needs no bus mastering. The DMA half is
stage 5, and it is explicitly gated: either an IOMMU exists by then, or
the driver is written down as trusted-with-physical-memory in the same
sentence that ships it. `enable_unsafe_noiommu_mode` is the precedent
for saying that out loud rather than quietly.

## What already exists, measured (2026-09-20, after stage 1)

More than expected when this was written: two of the three primitives
were half-built already, because the compositor needed the same things.

| what | where | state |
|---|---|---|
| map a device's BAR into a process | `SYS_DEV_MAP_BAR` | **BUILT, stage 1** |
| map physical pages into a process | `vmm_map_user_borrowed()` | **built** -- the framebuffer grant uses it (`kernel/proc/win_surface.c`) |
| a memory type for a register file | `VMM_MT_UC` | **BUILT, stage 1**; `vmm_user_memtype()` reads one back |
| find a device from ring 3 | `SYS_PCI_COUNT` / `SYS_PCI_INFO` | **built** -- `/bin/lspci` already walks every device and its BARs |
| a BAR's address and size | `pci_bar_addr()`, `pci_bar_mem_size()` | **built**, size-probed once at enumeration |
| is a ring-0 driver bound to it? | `pci_device_driver()`, `g_bound[]` | **built** -- stage 1 refuses on it |
| single-holder access to a device | the compositor ROLE (`win_role.c`) | **the pattern exists**, for one device |
| a CLAIM, and a way to make ring 0 let go | `SYS_DEV_CLAIM`, `pci_device_release()` | **BUILT, stage 2** |
| deliver an interrupt to a process | -- | **nothing** -- stage 4 |
| pinned, physically contiguous memory | `pmm_alloc_contiguous()` | built for kernel callers; no ring-3 path -- stage 5 |

The sound stack, which is the worked example:

| file | lines | what it is |
|---|---|---|
| `kernel/drivers/sound/hda.c` | 1068 | controller + CORB/RIRB (~340), **the codec graph (~250)**, the stream/BDL (~80), interrupts (~50) |
| `kernel/drivers/sound/sound_usb.c` | 1154 | USB audio, on top of the whole USB stack |
| `kernel/drivers/sound/sound.c` | 472 | the `sound_device` class registry and the one exclusive PCM stream |
| `kernel/drivers/sound/ac97.c` | 225 | port I/O and bus-master DMA |
| `userland/bin/soundd.c` | ~200 | **already ring 3** -- the mixer every client talks to |

The mixing policy left ring 0 long ago; what is left is the hardware.

## The stages

### Stage 1 -- a process can map a device's registers -- DONE 2026-09-20

`VMM_MT_UC`, and a grant that hands a process ONE PCI device's memory
BAR at its probed size.

**What shipped** (`SYS_DEV_MAP_BAR`, 118):

| piece | where |
|---|---|
| the validation, on its own so a KTEST can drive it | `dev_bar_check()`, `kernel/mm/mmap.c` |
| the grant | `sys_dev_map_bar()`, beside it -- it is an mmap whose backing is a BAR |
| the memory type | `VMM_MT_UC` and `vmm_user_memtype()`, `kernel/mm/vmm.c` |
| the region kind | `MMAP_KIND_MMIO` / `QUERY_PROCMAP_MMIO`, so `pmap` prints `mmio` |
| the wrapper | `sys_dev_map_bar()`, `userland/rt/sys.h` -- returns -1 and sets errno |
| the tests | `kernel/mm/devbar_test.c` (4 KTESTs), `userland/tests/devbar_test.c` |

**Four things it cost, worth not paying twice:**

- **A ring-3 driver cannot be started with `run`.** The grant resolves
  the caller's address space the way `mmap` does, and the legacy loader
  has no scheduler slot -- every call comes back `EPERM`. `spawn`, and
  in `tools/usertest_run.py` that means registering it SPAWNED.
- **The wrapper returns -1 and sets `errno`**, not a negative errno.
  `rt/sys.c`'s `err()` does that to every syscall, and a caller
  comparing against `-EBUSY` sees a success.
- **A census that stops at its first success is ordering-dependent.**
  The probe checked "is some device EBUSY?" in the same loop it used to
  find a grantable one, so whether it saw a refusal depended on which
  device sorted first. It failed on a machine where the rule works.
- **`check_syscalls.py` catches a number collision and nothing else
  does** -- designated initializers keep the last row, so two syscalls
  on one number is a silent dispatch to the wrong handler.

**What it did NOT do:** nothing about sound moved, and no device changed
hands. Any BAR a ring-0 driver holds is still refused, which is every
device that matters -- so what a process can map today is whatever the
kernel does not want.

**THE VALIDATION IS THE WHOLE SAFETY OF IT.** The caller names a device
and a BAR index, never an address: the kernel answers from its own
enumeration (`pci_bar_addr()` / `pci_bar_mem_size()`), so a process
cannot ask for "physical 0x100000" and be handed the kernel's image. A
grant of an arbitrary physical range would be `/dev/mem`, which is the
thing every system on the list above stopped shipping.

`VMM_MT_UC` is not optional and is not the framebuffer's WC. Write
combining lets stores merge and arrive out of order, which is correct
for pixels and wrong for a register file -- a doorbell written before
the descriptor it announces is a hang that reproduces once a week.

### Stage 2 -- a claim, so two drivers cannot hold one device -- DONE 2026-09-20

A process claims a device by index. The kernel unbinds its ring-0
driver, records the holder, and the BAR grant then REQUIRES that claim
-- so an unbound device stopped being a free-for-all too. The claim
drops when the process dies. That is `vfio-pci`'s unbind-then-bind,
minus the sysfs.

**What shipped** (`SYS_DEV_CLAIM` 119, `SYS_DEV_RELEASE` 120):

| piece | where |
|---|---|
| the claim table, the syscalls and `QUERY_PCIDEV` | `kernel/drivers/dev_claim.c` |
| the unbind, and the re-probe that undoes it | `pci_device_release()` / `pci_device_rebind()`, `kernel/drivers/pci_bind.c` |
| is this device releasable at all? | `pci_device_removable()`, beside them |
| the grant now requires the claim | `dev_bar_check()`, which grew a `pml4` argument |
| the claim dies with its address space | `release_process_state()`, `kernel/proc/syscall.c` |
| the first BUILT-IN driver that can let go | `hda_remove()`, `kernel/drivers/sound/hda.c` |
| who has each device, from ring 3 | `lspci -k` |
| the tests | 5 KTESTs in `dev_claim.c`, `userland/tests/devclaim_test.c`, `tools/devclaim_test.py` |

**The three questions, answered:**

1. **Can a bound driver be asked to let go?** Yes, and that is the
   gate. A driver with no `remove()` can never be claimed -- a driver
   CAPABILITY standing in for a privilege check this kernel has no uid
   for. `hda` is the first BUILT-IN driver to gain one; `e1000` and
   `r8169` already had one, because a module cannot be unloaded
   without it. So the claimable set today is the sound card and the
   NICs, and NO STORAGE CONTROLLER -- which is what keeps the root
   filesystem out of reach, and is asserted by a KTEST rather than
   assumed.
2. **What revokes a grant?** Nothing needs to. The mapping is
   BORROWED and dies with the address space, and the claim is dropped
   in the same teardown -- so there is no window where a revoked
   mapping is a hole, and `win_surface.c`'s poison page is not needed
   here. A device is left UNBOUND when its holder dies, because a
   supervised driver has to find it free when it restarts.
3. **Who may claim?** Whoever asks, once the device is releasable.
   Said plainly as NOT a permission model, the way `MKPART_CONFIRM` is.
   `dev_claim_take()` is the one place a uid check would go.

**Four things it cost, worth not paying twice:**

- **A `probe()` MAY NOT BE CALLED WITH INTERRUPTS OFF, and two routes
  now do.** `hda_probe()` waits 30 ms for the link; on a machine whose
  clocksource is the PIT that is a `pit_ticks()` loop, and a syscall
  runs with IF clear -- the machine stopped dead at one instruction,
  no panic, no log. `driver_ctx_enter()` in `pci_bind.c` now brackets
  every probe AND every remove with interrupts on and preemption off.
  `sys_modload` had the same latent bug through `pci_rebind()` and is
  fixed by the same bracket.
- **A released controller is handed over IN RESET, and its registers
  then read ZERO.** `hda_remove()` writes GCTL.CRST low on its way
  out, so a ring-3 driver's first job is to bring the controller up --
  and CRST reading back high is the write being accepted, not the link
  being ready: GCAP stays 0 until the codecs are out of reset. The
  ring-3 driver SLEEPS for that, which is the thing the kernel's probe
  cannot do.
- **AN MMIO REGISTER IS READ AT ITS OWN WIDTH.** GCAP is 16 bits, and
  reading it as two bytes gave `0x0001` against the kernel's `0x4401`
  -- the byte at +1 does not decode, because a device models a
  REGISTER, not memory. It passed a "not all-ones" check either way.
- **A claim is not containment, and the kernel can still reach the
  registers.** `paging_map_device()` returns the identity map for a BAR
  below 4 GiB, so ring 0 never loses its view. What a claim removes is
  the DRIVER, not the access.

### Stage 3 -- the codec graph moves, and nothing else

**NEXT, and stage 2 unblocked it.** HDA's widget enumeration -- ~250 lines that walk what the card reports
about itself -- becomes a ring-3 program reading the same registers
through the stage-1 grant. **It needs no DMA and no interrupt:** CORB
and RIRB are MMIO rings, polled.

This is the stage with the real payoff, and it is deliberately first:
the codec graph is UNTRUSTED INPUT. Node counts, widget types and
connection lists come off the card, they are attacker-shaped on any
machine where the card is not what it claims, and every one of them is
parsed in ring 0 today. It is the same argument that moved `ttf.c`'s
callers and the image decoders.

### Stage 4 -- an interrupt becomes a wakeup

The handler stays in ring 0 -- it must, it runs in interrupt context --
and does exactly two things: mask the line and wake a futex the driver
is parked on. The driver services the device and unmasks by asking.
That is VFIO's eventfd, spelled in the primitives this kernel already
has (`SYS_FUTEX_WAIT`/`_WAKE`, built 2026-09-08 for the window channel).

The trap to write down before building it: a LEVEL-triggered line that
is unmasked before the device is quiesced re-fires immediately and the
machine livelocks in the handler. Mask-until-acked is not a nicety.

### Stage 5 -- DMA, and the decision that gates it

Streaming audio needs the card writing into RAM. Two ways, and the
choice is the user's to make when it is reached:

- **An IOMMU first.** VT-d on this hardware, a domain per claimed
  device, and the driver's buffers mapped into it. Then the claim is a
  real boundary and the plan matches DriverKit's.
- **Trusted, and said so.** The kernel pins a contiguous buffer, hands
  over its physical address, and the driver programs the card. This
  works today and contains nothing: the write-up ships with the
  sentence, the way VFIO's no-IOMMU mode taints the kernel.

Until one is chosen, HDA's stream stays in ring 0 and the ring-3 half
is the control plane. **A split driver is a legitimate end state**, not
a half-finished one -- it is what DriverKit's audio drivers are, with
the DMA engine behind the framework and the policy in the driver.

## What does NOT move, and why that is not a compromise

- **The PCM stream's exclusivity and the `sound_device` registry.** One
  stream, one owner, refusing the second asker is a kernel invariant;
  `soundd` already mixes above it.
- **USB audio.** It rides the USB stack; moving it means moving xHCI,
  which is a different project with a much worse blast radius.
- **AC97.** Port I/O needs `ioperm`-shaped permission, a fourth
  primitive, for a card that is 225 lines and works. It is the wrong
  first target.
- **The interrupt handler itself.** Interrupt context is ring 0's by
  definition.

## The case against

Worth reading before starting, because two of these are good.

- **The sound path works.** Nothing here fixes a bug a user has. The
  argument is preventive, and the repo's own bar is a second real
  caller, not a plausible one -- so the honest justification is the
  PARSING in stage 3, not "drivers should be in user space".
- **It adds a permission model to a kernel that has none.** There is no
  user model here (`SETTING_OP_RELOAD`'s comment says so in as many
  words). A device claim is the first thing in the tree that says "not
  you", and that concept will want an owner eventually.
- **Without an IOMMU the end state is a driver that is trusted anyway.**
  If stage 5 takes the trusted route, the machine ends with the same
  exposure and one more process. The reason to do it regardless is
  stage 3, which stands on its own and needs none of stage 5.
- **In favour, and it is the strongest point:** stages 1 and 2 are
  reusable. A BAR grant and a device claim are what any ring-3 driver
  needs, and the next one -- a NIC, a second display -- starts at stage
  3 rather than at zero.
