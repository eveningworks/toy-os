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
//
// THE RING IS s32 (abi/sound_abi.h). A card that reads 20/24/32-bit
// samples in 32-bit containers plays it as it is; a 16-bit one says so
// in `bits` and hands the host a `bounce` buffer inside its own DMA
// grant, which the HOST keeps filled with the narrowed ring -- the
// kernel core's struct snd_bounce, in this process.
#ifndef _SND_DRIVER_H
#define _SND_DRIVER_H

#include <stdint.h>
#include "pci.h"
#include "sound_abi.h"

// Bumped when anything below changes shape. The host refuses a plugin
// that does not match rather than calling through a moved slot -- the
// abi/toyabi.h rule, for the same reason.
#define SND_DRIVER_ABI 4u

// The symbol every plugin exports, by this exact name.
#define SND_DRIVER_SYMBOL "snd_driver"

#define SND_BARS 6

// One card, as the host hands it over. Everything here is already done
// by the time open() is called.
struct snd_dev {
    int      pci;                     // PCI enumeration index (lspci's),
                                      // or -1 for a driver that found
                                      // its own device on another bus
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

    // Also filled in by open(): the width the card plays at by default,
    // and -- for an engine that can DMA 16-bit samples, which cannot read
    // the s32 ring -- `bounce`: SND_CHUNKS * SND_CHUNK_BYTES_S16 bytes of
    // the driver's own DMA memory, where its descriptors point while 16
    // is the width. A driver that copies the ring itself (USB, per
    // packet) leaves it NULL.
    //
    // THE FORMAT IS THE HOST'S BEFORE EVERY start(): `rate` and `bits`
    // are overwritten from the kernel's request (a rate from `rates`, a
    // width from `depths`), and start() programs the card for them --
    // the host keeps `bounce` filled only while `bits` is 16.
    uint32_t bits;
    uint32_t rate;
    int16_t *bounce;
    uint64_t bounce_phys;

    void    *priv;                    // the driver's own state

    // WHICH DEVICE, for a driver that finds its own. Set by the host
    // from --usb-id before open(); NULL or empty means "any". A USB
    // driver cannot just take the first audio device it sees: the
    // kernel's device table hands out the first FREE entry, so
    // unplugging something reorders it and which DAC a host grabs
    // depends on unplug history.
    const char *select;

    // Filled in by open() when one plugin can serve several devices,
    // so each registers as its own row. Left empty, the host falls
    // back to the driver's own name and label -- which is right for a
    // plugin that drives exactly one chip.
    char     name[SND_DRV_NAME_MAX];
    char     label[SND_DRV_LABEL_MAX];
    // The bus device, as userland/lib/udevice.c spells it. The host fills
    // it for a PCI card; a driver that found its own device sets it.
    char     device_id[24];
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
    //
    // **NULL MEANS THE DRIVER FINDS ITS OWN DEVICE**, because not every
    // sound card is on the PCI bus -- a USB one is named by an xHCI
    // slot, and the host has no way to enumerate a bus it does not know
    // about. Such a driver is offered open() with `pci` = -1 and NO
    // claim taken, and takes whatever claim its own bus needs.
    int  (*match)(const struct pci_device *d);

    // Bring the chip up and find its output. The card is claimed, its
    // memory BARs are mapped and its DMA buffer granted. Fill in
    // dev->rates/depths. 0 to take it, -1 to decline -- declining is a
    // normal answer (a display-audio codec has no analog output), and
    // the host hands the card back and tries the next one.
    int  (*open)(struct snd_dev *dev);
    void (*close)(struct snd_dev *dev);

    // Play the kernel's ring, which lives at `ring_phys` and is
    // SND_CHUNKS chunks of SND_CHUNK_BYTES -- or, at 16 bits, `bounce`,
    // which the host has filled before calling this. The engine must
    // raise an interrupt per chunk, because that is the host's only way
    // to learn where the card has reached.
    int  (*start)(struct snd_dev *dev, uint64_t ring_phys);
    void (*stop)(struct snd_dev *dev);

    // An interrupt arrived. Acknowledge it at the chip and answer the
    // BYTE OFFSET into the RING (not the bounce buffer: chunk index times
    // SND_CHUNK_BYTES) that the card has FINISHED, rounded down to a chunk -- or SND_IRQ_NOT_MINE if this device did not
    // raise it. Reporting a position the card has not reached has the
    // kernel zero a chunk still being played; every driver here counts
    // completions rather than trusting a position register, because a
    // process reads that register milliseconds after the interrupt.
    int  (*period)(struct snd_dev *dev);

    void (*set_volume)(struct snd_dev *dev, int pct);
};

#endif // _SND_DRIVER_H
