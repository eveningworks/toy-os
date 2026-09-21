// THE SOUND PLUGIN ABI -- what /lib/snd/<name>.so exports and what
// /bin/snddrv gives it.
//
// WHY A PLUGIN AT ALL: adding a sound card should not rebuild the host.
// A driver is a file dropped into /lib/snd/, which snddrv scans at
// startup -- Windows UMDF's and DriverKit's split between a
// system-provided host process and a per-device driver, done with
// dlopen (docs/dynlink-design.md stage 4).
//
// WHAT THE HOST DOES AND THE DRIVER NEVER HAS TO: enumerate PCI, take
// the claim, map the BARs, get the DMA grant, register with the kernel's
// sound core, route the interrupt, run the request loop, report periods,
// and hand the card back on the way out. A driver programs its chip and
// nothing else.
//
// WHAT THE DRIVER NEVER TOUCHES: the samples. The kernel owns the ring
// and `soundd` writes it; a driver is told the ring's PHYSICAL address
// and points its engine at it. That is what keeps a ring-3 driver's
// exposure to one buffer on a machine with no IOMMU.
#ifndef _SND_DRIVER_H
#define _SND_DRIVER_H

#include <stdint.h>
#include "pci.h"

// Bumped when anything below changes shape. The host refuses a plugin
// that does not match rather than calling through a moved slot -- the
// abi/toyabi.h rule, for the same reason.
#define SND_DRIVER_ABI 1u

// The symbol every plugin exports, by this exact name.
#define SND_DRIVER_SYMBOL "snd_driver"

#define SND_BARS 6

// One card, as the host hands it over. Everything here is already done
// by the time open() is called.
struct snd_dev {
    int      pci;                     // PCI enumeration index (lspci's)
    struct pci_device info;           // what the bus says about it

    // Memory BARs the host mapped, or NULL. An I/O BAR is NOT mapped --
    // ring 3 cannot run in/out -- and is reached with snd_io_* below.
    volatile uint8_t *bar[SND_BARS];

    // The driver's own DMA buffer: command rings, descriptor lists,
    // whatever the chip needs. NOT the audio ring.
    void    *dma;
    uint64_t dma_phys;
    uint64_t dma_bytes;

    // Filled in by open(): what the hardware says it supports, as
    // SND_RATE_* / SND_DEPTH_* masks (abi/sound_abi.h). 0 means "the
    // driver does not say", which lssound prints differently from
    // "nothing".
    uint32_t rates, depths;

    void    *priv;                    // the driver's own state
};

// A driver answers this after an interrupt.
#define SND_IRQ_NOT_MINE (-1)         // the device did not raise it

struct snd_driver {
    uint32_t    abi;                  // SND_DRIVER_ABI
    const char *name;                 // "hda" -- also the device's name
    const char *label;                // what a person sees in lssound

    // How much DMA the host should get before open(). 0 means none.
    uint64_t    dma_bytes;

    // Would this driver drive that device? Reads the PCI config the
    // host already has; the device is NOT claimed yet, so this must
    // not touch the hardware.
    int  (*match)(const struct pci_device *d);

    // Bring the chip up and find its output. The card is claimed, its
    // memory BARs are mapped and its DMA buffer granted. Fill in
    // dev->rates/depths. 0 to take it, -1 to decline -- declining is a
    // normal answer (a display-audio codec has no analog output), and
    // the host hands the card back and tries the next one.
    int  (*open)(struct snd_dev *dev);
    void (*close)(struct snd_dev *dev);

    // Play the kernel's ring, which lives at `ring_phys` and is
    // SND_CHUNKS chunks of SND_CHUNK_BYTES. The engine must raise an
    // interrupt per chunk, because that is the host's only way to
    // learn where the card has reached.
    int  (*start)(struct snd_dev *dev, uint64_t ring_phys);
    void (*stop)(struct snd_dev *dev);

    // An interrupt arrived. Acknowledge it at the chip and answer the
    // BYTE OFFSET into the ring that the card has FINISHED, rounded
    // down to a chunk -- or SND_IRQ_NOT_MINE if this device did not
    // raise it. Reporting a position the card has not reached has the
    // kernel zero a chunk still being played; every driver here counts
    // completions rather than trusting a position register, because a
    // process reads that register milliseconds after the interrupt.
    int  (*period)(struct snd_dev *dev);

    void (*set_volume)(struct snd_dev *dev, int pct);
};

#endif // _SND_DRIVER_H
