// AC'97 -- the first sound_device (kernel/sound.h). QEMU's `-device
// AC97` (8086:2415), matched by CLASS so a real ICH-era codec claims
// too.
//
// The controller is two I/O BARs: NAM (the mixer -- volumes, sample
// rates) and NABM (the bus master -- three DMA "boxes"; only PCM OUT
// is driven here). A box plays a 32-entry BUFFER DESCRIPTOR LIST; each
// entry is one chunk of the core's shared ring, flagged to interrupt
// on completion, and the engine is kept running FOREVER by moving LVI
// (the last-valid index) one behind CIV (the index being played) on
// every interrupt -- the standard AC'97 ring trick, so the hardware
// never reaches "last valid" and never halts. What it replays if the
// app stalls is whatever the core zeroed, which is silence
// (abi/sound_abi.h's one rule).
//
// DMA addresses are PHYSICAL and the kernel relocates itself, so the
// BDL lives in a pmm frame like every other DMA object here (the
// virtio rule), never in a static.
#include "sound.h"
#include "sound_abi.h"
#include "pci.h"
#include "pci_internal.h" // pci_command_update() -- the INTx enable
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "timer.h"
#include "string.h"
#include "kfmt.h"
#include "klog.h"
#include "ktest.h"
#include <stdint.h>
#include "driver.h" // DRIVER_DECLARE -- `lsdrv`
#include "pci_driver.h"

DRIVER_DECLARE("ac97", "sound", "Intel AC'97 audio codec");

// NAM (mixer) registers.
#define NAM_RESET      0x00
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL    0x18

// NABM: the PCM OUT box, and the two globals.
#define PO_BDBAR 0x10 // dword: BDL physical address
#define PO_CIV   0x14 // byte, RO: descriptor being played
#define PO_LVI   0x15 // byte: last valid descriptor
#define PO_SR    0x16 // word, RW1C
#define PO_CR    0x1B // byte
#define GLOB_CNT 0x2C // dword
#define GLOB_STA 0x30 // dword

#define SR_LVBCI 0x04
#define SR_BCIS  0x08
#define SR_FIFOE 0x10
#define SR_ACK   (SR_LVBCI | SR_BCIS | SR_FIFOE)

#define CR_RPBM  0x01 // run
#define CR_RR    0x02 // reset the box's registers
#define CR_IOCE  0x10 // interrupt on completion

#define GC_COLD_RESET 0x02
#define GS_CODEC_READY 0x100

struct bdl_entry {
    uint32_t addr;
    uint16_t samples; // 16-bit units
    uint16_t flags;   // bit15 = IOC
};

static const struct pci_device *g_pci;
static uint8_t g_msi_vector;   // LAPIC vector, 0 when on the INTx pin
static uint16_t g_nam, g_nabm;
static struct bdl_entry *g_bdl; // one pmm frame; identity-mapped
static uint64_t g_bdl_phys;

static void ac97_irq(uint64_t *regs) {
    (void)regs;
    uint16_t sr = inw(g_nabm + PO_SR);
    if (!(sr & SR_ACK)) return; // not ours (a shared line is legal)
    uint8_t civ = inb(g_nabm + PO_CIV) & 31;
    sound_period_done((uint32_t)civ * SND_CHUNK_BYTES);
    // One behind the player: the engine never sees "last valid".
    outb(g_nabm + PO_LVI, (uint8_t)((civ + 31) & 31));
    outw(g_nabm + PO_SR, sr & SR_ACK); // RW1C: only the bits seen
}

static int ac97_start(const struct sound_device *dev) {
    (void)dev;   // one AC'97 per machine
    // Reset the box, then arm it. RR self-clears when the reset is done.
    // A SPIN COUNT, not a tick deadline: this runs from SND_CTL_START,
    // a syscall, where interrupts are off and pit_ticks() stands still.
    outb(g_nabm + PO_CR, CR_RR);
    uint32_t spins = 0;
    while ((inb(g_nabm + PO_CR) & CR_RR) && ++spins < 1000000u) { }
    if (inb(g_nabm + PO_CR) & CR_RR) return -1;

    outl(g_nabm + PO_BDBAR, (uint32_t)g_bdl_phys);
    outb(g_nabm + PO_LVI, 31);
    outw(g_nabm + PO_SR, SR_ACK);
    outb(g_nabm + PO_CR, CR_RPBM | CR_IOCE);
    return 0;
}

static void ac97_stop(const struct sound_device *dev) {
    (void)dev;
    outb(g_nabm + PO_CR, 0);
    outw(g_nabm + PO_SR, SR_ACK);
}

// 0..100 onto the codec: 40 dB of attenuation across the slider, linear
// in dB like hda.c and sound_usb.c, mute at 0 -- all of it on the MASTER
// (1.5 dB steps, so at most 26, which a 5-bit codec also has). PCM out
// is a GAIN, not an attenuation: 0x08 is 0 dB and 0 is +12 dB, and
// attenuating both linearly put 25% at about -105 dB, i.e. silence.
//
// QEMU'S AC97 IS NOT dB-ACCURATE: it scales each register linearly in
// steps and multiplies the two, so under it this slider spans about
// 4 dB and 0x08 costs 2.6 dB (measured). A codec follows the spec.
#define AC97_TAPER_DB 40
#define AC97_PCM_0DB  0x0808
static void ac97_set_volume(const struct sound_device *dev, int pct) {
    (void)dev;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    outw(g_nam + NAM_PCM_VOL, AC97_PCM_0DB);
    if (pct == 0) {
        outw(g_nam + NAM_MASTER_VOL, 0x8000);
        return;
    }
    uint16_t att = (uint16_t)(AC97_TAPER_DB * 2 * (100 - pct) / 300);
    outw(g_nam + NAM_MASTER_VOL, (uint16_t)((att << 8) | att));
}

static const struct sound_device ac97_dev = {
    .name = "ac97",
    .driver = "ac97",
    // THE DRIVER'S SET, NOT THE CODEC'S CEILING. AC97 baseline is 48
    // kHz 16-bit, and a codec with Variable Rate Audio can do more --
    // but VRA is not programmed here, so reporting what the hardware
    // might manage would describe something nothing can ask for.
    .rates = SND_RATE_48000,
    .depths = SND_DEPTH_16,
    .start = ac97_start,
    .stop = ac97_stop,
    .set_volume = ac97_set_volume,
};

static const struct pci_match ac97_matches[] = { PCI_MATCH_CLASS(0x04, 0x01, PCI_ANY) };

static void ac97_probe(const struct pci_device *dev) {
    if (g_pci) {
        klog_printf("ac97: a second codec at %02x:%02x.%u -- one is driven\n",
                    dev->bus, dev->device, dev->function);
        return;
    }
    g_pci = dev;

    if (!pci_bar_is_io(g_pci->bar[0]) || !pci_bar_is_io(g_pci->bar[1])) {
        klog_write("ac97: unexpected memory BARs -- not driving it\n");
        return;
    }
    g_nam  = (uint16_t)pci_bar_addr(g_pci->bar[0]);
    g_nabm = (uint16_t)pci_bar_addr(g_pci->bar[1]);

    pci_enable_bus_master(g_pci);

    // Cold reset released, then wait for the codec to report ready.
    outl(g_nabm + GLOB_CNT, GC_COLD_RESET);
    uint64_t deadline = pit_ticks() + 100; // a real codec takes ~1ms
    while (!(inl(g_nabm + GLOB_STA) & GS_CODEC_READY) && pit_ticks() < deadline) { }
    if (!(inl(g_nabm + GLOB_STA) & GS_CODEC_READY)) {
        klog_write("ac97: codec never came ready -- not driving it\n");
        return;
    }
    outw(g_nam + NAM_RESET, 1); // any write resets the mixer to defaults
    ac97_set_volume(&ac97_dev, 100);

    uint64_t ring_phys = 0;
    void *ring = sound_ring_alloc(&ring_phys);
    if (!ring) {
        klog_write("ac97: no contiguous frames for the ring\n");
        return;
    }
    g_bdl_phys = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    if (!g_bdl_phys) return;
    g_bdl = (struct bdl_entry *)(uintptr_t)g_bdl_phys;
    for (int i = 0; i < SND_CHUNKS; i++) {
        g_bdl[i].addr = (uint32_t)(ring_phys + (uint64_t)i * SND_CHUNK_BYTES);
        g_bdl[i].samples = SND_CHUNK_BYTES / 2;
        g_bdl[i].flags = 0x8000; // IOC
    }

    // The commit point, AHCI's ordering: the handler exists before the
    // line is unmasked, and registration is last. A vector if the
    // device offers one, else the pin -- QEMU's AC97 advertises
    // neither capability, so this is the pin on every emulated boot.
    uint8_t line = pci_irq_line(g_pci);
    g_msi_vector = pci_msi_request(g_pci, ac97_irq);
    if (!g_msi_vector) {
        if (line == 0xFF || line == 0 || line >= 16) {
            klog_write("ac97: no usable INTx line -- not driving it\n");
            return;
        }
        irq_register_handler(line, ac97_irq);
        // INTx may arrive DISABLED in the command register (firmware's
        // choice); clearing it is the half AHCI's bring-up calls the
        // commit point. Without it the engine plays but never reports a
        // completion, hw_pos freezes, and the ring loops its primed lap
        // forever -- measured as a 2.8s recording whose tail never went
        // quiet.
        pci_command_update(g_pci, 0, PCI_CMD_INTX_DISABLE);
        irq_unmask(line);
    }

    if (!sound_register(&ac97_dev, ring, ring_phys)) return;
    if (g_msi_vector)
        klog_printf("ac97: %02x:%02x.%u nam %#x nabm %#x %s vector %u\n",
                    g_pci->bus, g_pci->device, g_pci->function,
                    g_nam, g_nabm, g_pci->irq_msix ? "MSI-X" : "MSI",
                    g_msi_vector);
    else
        klog_printf("ac97: %02x:%02x.%u nam %#x nabm %#x irq %u\n",
                    g_pci->bus, g_pci->device, g_pci->function,
                    g_nam, g_nabm, line);
}

// LETTING GO, which is the gate on being CLAIMED: a device whose
// driver has no remove() can never be taken by a ring-3 driver
// (kernel/drivers/dev_claim.c). /lib/snd/ac97.so is what wants it.
//
// THE ENGINE IS HALTED BEFORE THE INTERRUPT GOES, not after: a
// completion arriving with no handler registered is a spurious vector,
// and one arriving after the BDL frame is freed points a live DMA
// engine at memory the allocator has handed to somebody else.
static void ac97_remove(const struct pci_device *d) {
    if (!g_pci || g_pci != d) return;

    ac97_stop(&ac97_dev);
    sound_unregister(&ac97_dev);

    if (g_msi_vector) pci_msi_release(d, g_msi_vector);
    else {
        uint8_t line = pci_irq_line(d);
        if (line != 0xFF && line && line < 16) {
            irq_mask(line);
            irq_unregister_handler(line, ac97_irq);
        }
    }
    g_msi_vector = 0;

    // BUS MASTERING OFF before the frame goes back: the claim path
    // raises it again for whoever takes the device next.
    pci_command_update(d, PCI_CMD_BUS_MASTER, 0);
    if (g_bdl_phys) pmm_free_contiguous(g_bdl_phys, 1);
    g_bdl = 0;
    g_bdl_phys = 0;
    g_nam = g_nabm = 0;
    g_pci = 0;
    klog_printf("ac97: released %02x:%02x.%u\n", d->bus, d->device, d->function);
}

// --- KTESTs -- skip without the device, like ahci's --------------------

KTEST("ac97", "the controller registered and the codec is ready") {
    if (!g_pci) { KTEST_SKIP("no AC97 on this machine"); return; }
    KTEST_ASSERT(inl(g_nabm + GLOB_STA) & GS_CODEC_READY);
    KTEST_ASSERT(sound_present());
}

KTEST("ac97", "start runs the engine and stop halts it") {
    if (!g_pci || !g_bdl) { KTEST_SKIP("no AC97 on this machine"); return; }
    KTEST_ASSERT_EQ(ac97_start(&ac97_dev), 0);
    KTEST_ASSERT(inb(g_nabm + PO_CR) & CR_RPBM);
    ac97_stop(&ac97_dev);
    KTEST_ASSERT_EQ(inb(g_nabm + PO_CR) & CR_RPBM, 0);
}
KTEST("ac97", "volume is a 40 dB taper on the master, and PCM stays at 0 dB") {
    if (!g_pci) { KTEST_SKIP("no AC97 on this machine"); return; }
    uint16_t master = inw(g_nam + NAM_MASTER_VOL), pcm = inw(g_nam + NAM_PCM_VOL);
    ac97_set_volume(&ac97_dev, 25);
    uint16_t at25 = inw(g_nam + NAM_MASTER_VOL), pcm25 = inw(g_nam + NAM_PCM_VOL);
    ac97_set_volume(&ac97_dev, 1);
    uint16_t at1 = inw(g_nam + NAM_MASTER_VOL);
    ac97_set_volume(&ac97_dev, 0);
    uint16_t at0 = inw(g_nam + NAM_MASTER_VOL);
    outw(g_nam + NAM_PCM_VOL, pcm);
    outw(g_nam + NAM_MASTER_VOL, master);

    KTEST_ASSERT_EQ(at25, 0x1414);       // 20 steps of 1.5 dB: -30 dB, audible
    KTEST_ASSERT_EQ(pcm25, AC97_PCM_0DB);
    KTEST_ASSERT_EQ(at1, 0x1a1a);        // -39 dB: inside a 5-bit codec's range
    KTEST_ASSERT(at0 & 0x8000);          // 0 is mute, not a number
}

PCI_DRIVER_REMOVABLE("ac97", ac97_matches, ac97_probe, ac97_remove);
