# A driver in ring 3

A staged plan, in the shape `docs/winserver-ring3-design.md` used. It
answers "what would it take to run a device driver as a process here,
what does that actually buy, and which driver goes first?"

**Nothing below is built.** The stage markers are the authority.

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

## What already exists, measured (2026-09-20)

More than expected. Two of the three primitives are half-built, because
the compositor needed the same things.

| what | where | state |
|---|---|---|
| map physical pages into a process | `vmm_map_user_borrowed()` | **built** -- the framebuffer grant uses it (`kernel/proc/win_surface.c`) |
| a memory type for the mapping | `enum vmm_memtype` | **NORMAL and WC only.** MMIO registers need **UC**, which does not exist yet |
| find a device from ring 3 | `SYS_PCI_COUNT` / `SYS_PCI_INFO` | **built** -- `/bin/lspci` already walks every device and its BARs |
| a BAR's address and size | `pci_bar_addr()`, `pci_bar_mem_size()` | **built**, size-probed once at enumeration |
| single-holder access to a device | the compositor ROLE (`win_role.c`) | **the pattern exists**, for one device |
| deliver an interrupt to a process | -- | **nothing** |
| pinned, physically contiguous memory | `pmm_alloc_contiguous()` | built for kernel callers; no ring-3 path |

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

### Stage 1 -- a process can map a device's registers

`VMM_MT_UC`, and a grant that hands a process ONE PCI device's memory
BAR at its probed size.

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

### Stage 2 -- a claim, so two drivers cannot hold one device

A process claims a device by (bus, device, function). The kernel refuses
if a ring-0 driver is bound to it (`DRIVER_DECLARE`) or another process
holds the claim; the claim drops when the process exits, and the grant
goes with it. That is `vfio-pci`'s unbind-then-bind, minus the sysfs.

The compositor role is the in-tree precedent for a single-holder,
kernel-checked claim, and its lesson comes too: `win_surface.c` keeps a
revoked mapping pointing at a scratch page rather than unmapping it,
because a process preempted mid-store does not stop when the kernel
decides it should. A revoked BAR needs the same treatment or the answer
is a #PF in a driver that did nothing wrong.

### Stage 3 -- the codec graph moves, and nothing else

HDA's widget enumeration -- ~250 lines that walk what the card reports
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
