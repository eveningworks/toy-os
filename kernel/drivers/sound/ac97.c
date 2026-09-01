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
#include "driver.h" // DRIVER_REGISTER -- `lsdrv`

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

static int ac97_start(void) {
    // Reset the box, then arm it. RR self-clears when the reset is done.
    outb(g_nabm + PO_CR, CR_RR);
    uint64_t deadline = pit_ticks() + 10;
    while ((inb(g_nabm + PO_CR) & CR_RR) && pit_ticks() < deadline) { }
    if (inb(g_nabm + PO_CR) & CR_RR) return -1;

    outl(g_nabm + PO_BDBAR, (uint32_t)g_bdl_phys);
    outb(g_nabm + PO_LVI, 31);
    outw(g_nabm + PO_SR, SR_ACK);
    outb(g_nabm + PO_CR, CR_RPBM | CR_IOCE);
    return 0;
}

static void ac97_stop(void) {
    outb(g_nabm + PO_CR, 0);
    outw(g_nabm + PO_SR, SR_ACK);
}

// 0..100 onto the codec's attenuators: 0 dB at 100, mute at 0. Master
// is 6-bit attenuation per channel, PCM out 5-bit.
static void ac97_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == 0) {
        outw(g_nam + NAM_MASTER_VOL, 0x8000);
        return;
    }
    uint16_t att6 = (uint16_t)((100 - pct) * 63 / 100);
    outw(g_nam + NAM_MASTER_VOL, (uint16_t)((att6 << 8) | att6));
    uint16_t att5 = (uint16_t)((100 - pct) * 31 / 100);
    outw(g_nam + NAM_PCM_VOL, (uint16_t)((att5 << 8) | att5));
}

static const struct sound_device ac97_dev = {
    .name = "ac97",
    .start = ac97_start,
    .stop = ac97_stop,
    .set_volume = ac97_set_volume,
};

void ac97_init(void) {
    // DECLARED BEFORE THE HARDWARE IS LOOKED FOR, so a driver
    // that finds nothing still appears in `lsdrv` -- "compiled
    // in but idle" is the answer somebody is looking for.
    DRIVER_REGISTER("ac97", "sound");
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d->class_code == 0x04 && d->subclass == 0x01) { g_pci = d; break; }
    }
    if (!g_pci) return; // no audio controller: not an error

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
    ac97_set_volume(100);

    uint64_t ring_phys = 0;
    void *ring = sound_ring_alloc(&ring_phys);
    if (!ring) {
        klog_write("ac97: no contiguous frames for the ring\n");
        return;
    }
    g_bdl_phys = pmm_alloc_contiguous(1);
    if (!g_bdl_phys) return;
    g_bdl = (struct bdl_entry *)(uintptr_t)g_bdl_phys;
    for (int i = 0; i < SND_CHUNKS; i++) {
        g_bdl[i].addr = (uint32_t)(ring_phys + (uint64_t)i * SND_CHUNK_BYTES);
        g_bdl[i].samples = SND_CHUNK_BYTES / 2;
        g_bdl[i].flags = 0x8000; // IOC
    }

    // The commit point, AHCI's ordering: the handler exists before the
    // line is unmasked, and registration is last.
    uint8_t line = g_pci->interrupt_line;
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
    pic_clear_mask(line);

    driver_bound("ac97", "snd0");
    if (!sound_register(&ac97_dev, ring, ring_phys)) return;
    klog_printf("ac97: %02x:%02x.%u nam %#x nabm %#x irq %u\n",
                g_pci->bus, g_pci->device, g_pci->function,
                g_nam, g_nabm, line);
}

// --- KTESTs -- skip without the device, like ahci's --------------------

KTEST("ac97", "the controller registered and the codec is ready") {
    if (!g_pci) { KTEST_SKIP("no AC97 on this machine"); return; }
    KTEST_ASSERT(inl(g_nabm + GLOB_STA) & GS_CODEC_READY);
    KTEST_ASSERT(sound_present());
}

KTEST("ac97", "start runs the engine and stop halts it") {
    if (!g_pci || !g_bdl) { KTEST_SKIP("no AC97 on this machine"); return; }
    KTEST_ASSERT_EQ(ac97_start(), 0);
    KTEST_ASSERT(inb(g_nabm + PO_CR) & CR_RPBM);
    ac97_stop();
    KTEST_ASSERT_EQ(inb(g_nabm + PO_CR) & CR_RPBM, 0);
}
