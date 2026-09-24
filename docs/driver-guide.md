# Writing a driver

What to do when you have a piece of hardware and no driver for it. The
rules are scattered across `docs/conventions/kernel.md` by topic and
`docs/decisions/drivers.md` by rationale; this is the same material
ordered by the task, and it stops where those begin.

**The two worked examples are `kernel/drivers/net/e1000.c`** — PCI,
emulated by QEMU, about 320 lines — **and `kernel/drivers/net/r8169.c`**
— PCI, on real hardware only, MSI-X, no emulator anywhere. Read the
first to see the shape and the second to see what changes when nothing
can test it. `docs/devices.md` lists what already exists.

## 1. Find out what you actually have

    lspci            on the machine: vendor:device, class, BARs, IRQ
    lsusb            the same for USB
    lsdrv            which driver claimed what
    lsdrv -a         ...and what claimed nothing

A device in `lspci` and not in `lsdrv` is the gap. Note the **class and
prog-if** as well as the ID: matching a class drives every card of a
kind, matching an ID drives one.

## 2. Pick the class registry, not a new mechanism

A driver does not invent a way to be reached. It registers with the
class that already exists:

| Class | Header | Registers with |
|---|---|---|
| Network | `kernel/include/kernel/netdev.h` | `net_register()` |
| Block | `kernel/include/kernel/block.h` | `blk_register()` |
| Display | `kernel/include/kernel/display.h` | `display_register()` |
| Input | `kernel/include/kernel/input.h` | `input_register_source()` |
| Sound | `kernel/include/kernel/sound.h` | `sound_register()` |
| Clock | `kernel/include/kernel/clocksource.h` | `clocksource_register()` |

If your device fits none of them you are adding a class, which is a
bigger decision than a driver — see `docs/decisions/drivers.md` on why
a class exists at all.

## 3. Declare yourself, and let the bus call you

Two declarations, both **file-scope data**, never a call inside
`init()`:

```c
DRIVER_DECLARE("r8169", "net", "Realtek RTL8169/8168 PCIe Ethernet");

static const struct pci_match r8169_matches[] = {
    PCI_MATCH_ID(0x10EC, 0x8168),
};
PCI_DRIVER("r8169", r8169_matches, r8169_probe);
```

`tools/check_drivers.py` fails the build on a `.c` under
`kernel/drivers/` with neither a `DRIVER_DECLARE` nor a
`driver-none: <reason>` comment. The reason it is data and not a call:
`e1000`'s declaration once sat after its "no card here" return, so a
build containing the driver listed no driver and no static check could
see it.

`pci_bind()` then calls your `probe()` once per matching device, at
`INIT_BUS`. **A second controller is a second `probe()` call**, not a
silent skip — say so if you only drive one.

**The same file can be built as a MODULE** -- `<name> = module` in
`build.conf`, where the name is your file's basename -- with no
change to the declarations: the loader runs and registers the same
three tables. Two things then matter. Every kernel function you call
must be an `EXPORT_SYMBOL` in `kernel/core/kexports.c` (the build
names any that is not), and a driver that should be UNLOADABLE
declares a `remove()` through `PCI_DRIVER_REMOVABLE` -- mask
interrupts first, then unregister from the class, then free what
`probe()` allocated, then reset your statics so a re-probe starts from
nothing. Without one, the module is pinned while the device is bound.

## 4. Reach the hardware

**Which BAR.** `pci_bar_mem_addr(dev, n)` — *not* `pci_bar_addr()`,
which takes a raw dword, cannot see the neighbouring slot and therefore
cannot decode a 64-bit BAR. Sizes come from `pci_bar_mem_size()`,
probed once at enumeration; never size a BAR yourself, because probing
turns the device's decode off underneath whoever is using it.

**Turn decode on.** `pci_command_update(dev, PCI_CMD_MEMORY |
PCI_CMD_BUS_MASTER, 0)`. Bus mastering is required before any DMA moves
real data, and a device will happily report success without it.

**MMIO below 4 GiB is identity-mapped**, so the BAR address is a
pointer. Reads and writes go through `volatile` accessors.

## 5. DMA

- **Rings and buffers come from `pmm_alloc_contiguous(n, PMM_ZONE_DMA32)`.**
  A device that takes a 32-bit address cannot reach `PMM_ZONE_ANY`.
- **A DMA TARGET MUST NEVER BE ON THE STACK.** The failure is not a
  crash at the DMA — it is a corrupted saved register somewhere else
  entirely. This cost a general protection fault in an unrelated
  function once (`usb_enum.c`, 275cc88); the buffer is a static now.
- **Publish the buffer address before the ownership bit**, with
  `kbarrier()` between. x86-64 will not reorder those stores, but GCC
  will.

## 6. Interrupts, in this order

```c
uint8_t vector = pci_msi_request(dev, my_irq);   /* MSI-X, then MSI */
if (vector)                 { enable interrupts in the device; }
else if (line != 0xFF && line < 16) { irq_register_handler(line, my_irq);
                                      pic_clear_mask(line); }
else                        { dev->poll = my_poll; }   /* the class polls */
```

**A shared INTx handler must return early when the device's status
register reads zero** — the line is level-triggered and shared, and a
handler that acknowledges somebody else's interrupt breaks them.

**The commit point matters.** Every field your handler reads must be
set before the flag that lets it run, because the first interrupt can
arrive during `probe()`:

```c
    /* ... everything the handler touches, set up ... */
    g_present = 1;                  /* the commit point */
    reg_write16(REG_IMR, mask);     /* only now may it fire */
```

**Only ask for interrupts you will act on.** A transmit-complete
interrupt whose handler does nothing is an interrupt per packet.

## 7. Bounded waits

**Never spin without a bound, and prefer a deadline where one can be
trusted.** `clocksource_deadline_capable()` is the question — the TSC
is free-running, the PIT's counter is incremented by the timer
interrupt and stands still with interrupts off. In a `probe()` at
`INIT_BUS`, a **poll count with `cpu_relax()`** is the honest choice:

```c
for (int i = 0; i < POLL_LIMIT; i++) {
    if (!(reg_read8(REG_CR) & CR_RESET)) return 1;
    cpu_relax();                    /* `pause`: on KVM this is required */
}
```

A poll count is not a timeout. `XHCI_POLL_BACKSTOP`'s ceiling expired
before real devices answered on a slow laptop, and every failure
reported the ceiling rather than a device saying no — which reads as a
device fault and is not one.

## 8. Make the untestable half testable

**QEMU emulates almost nothing you will want to drive.** The split that
works: put the pure rules — descriptor field layouts, length
arithmetic, protocol framing — in functions that touch no register, in
a header of their own, and KTEST them. Everything else needs the card.

`kernel/include/kernel/r8169.h` is three functions and
`r8169_test.c` is ten KTESTs over them. That is where the silent bugs
were: a received length used as reported delivers four bytes of Ethernet
FCS as payload, and a transmit ring with no end marker sends the engine
off the end of it. Both look like a working driver until they don't.

**A green test proves nothing until you have seen it red.** Break the
thing it covers, confirm it fails on the *right* assertion, restore.
And ask what a broken version would still pass — one of those ten
KTESTs passed against deliberately broken code until the fixture was
reordered.

## 9. Bringing it up on real hardware

**Give yourself a way back.** Two things, and they are a pair:

- **A rescue boot word.** `nor8169`, `nousb`, `noahci` — matched as a
  whole word, checked at the top of `probe()`. This keeps the *new*
  kernel while skipping your driver, which is the more useful half when
  the question is whether your driver is the problem. Add a row to
  `docs/boot-flags.md`.
- **A reachable fallback kernel.** `grub.cfg` has a "previous kernel"
  entry, and an installed machine has `set timeout=0`, which draws no
  menu and makes it unreachable. Fix that *before* the first flash.

**Flash with `tools/remote.py flash`**, which syncs the userland with
the kernel. A kernel alone is half a build: an ABI struct that changes
size moves fields under every binary compiled against the old one, and
the machine then boots perfectly and cannot be given an address.

**Print what the chip says it is.** `r8169` logs its XID and which
interrupt it took; that one line is most of the diagnosis when a
different revision turns up.

## 10. Before you commit

- `tools/check_drivers.py`, `check_dispatch.py`, `check_docs.py` —
  all run by `tools/preflight.sh`.
- A row in `docs/devices.md`; the build fails without one.
- A `docs/boot-flags.md` row if you added a boot word.
- `docs/decisions/drivers.md` if you did something the next person
  would otherwise re-litigate.

## What this does not cover

USB devices bind through the enumerator rather than a PCI match table —
see `usb_r8153_bind()` and `docs/conventions/kernel.md`'s USB entries.
virtio devices sit on a shared transport and register with their class
separately; `net_virtio.c` is forty lines because `virtio_net.c` does
the work. Neither changes anything above about DMA, interrupts or
testing.
