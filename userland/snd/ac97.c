// /lib/snd/ac97.so -- Intel AC'97, driven from ring 3.
//
// THE CHIP HALF ONLY; /bin/snddrv does the claim, the DMA grant, the
// registration and the request loop (userland/include/snd_driver.h).
//
// **IT IS TWO I/O BARs, WHICH IS WHY SYS_DEV_IO EXISTS.** NAM is the
// mixer (volume) and NABM the bus-master box (the descriptor list and
// the engine). Neither is memory: `in`/`out` are ring-0 instructions,
// so every register access below is a syscall the kernel performs and
// validates against this device's own BARs. That is VFIO's answer
// rather than ioperm()'s, and at a few accesses per interrupt the cost
// does not show up.
//
// THE DESCRIPTOR LIST IS 32 ENTRIES OF 16-BIT SAMPLES, not bytes --
// the one place this chip's units differ from HD Audio's, and reading
// it as bytes plays everything an octave out.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "snd_driver.h"
#include "sound_abi.h"
#include "rt/sys.h"

#define AC97_CLASS 0x04
#define AC97_SUBCLASS 0x01

#define BAR_NAM  0   // the mixer
#define BAR_NABM 1   // the bus-master box

// NAM (mixer) registers.
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL    0x18
#define NAM_EXT_ID     0x28 // bit 0: the codec has Variable Rate Audio
#define NAM_EXT_CTL    0x2A // bit 0: VRA on
#define NAM_DAC_RATE   0x2C // the front DAC's rate in Hz, VRA only
#define EXT_VRA        0x0001

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

#define GC_COLD_RESET  0x02
#define GS_CODEC_READY 0x100

struct bdl_entry {
    uint32_t addr;
    uint16_t samples; // 16-bit units, NOT bytes
    uint16_t flags;   // bit15 = IOC
};

static struct {
    int pci;
    struct bdl_entry *bdl;
    uint64_t bdl_phys;
    int running;
    int vra;
} g_st;

// Every register access is a syscall.
//
// A REFUSED ACCESS IS NOT A REGISTER FULL OF ONES. Reading the error
// back as data is what made this driver believe a card it could not
// touch had reported its codec ready, so a failure is counted and
// said once -- a probe that outruns the log destroys its own evidence.
static int g_io_errors;

static uint32_t rd(int bar, uint32_t o, int width) {
    uint32_t v = 0;
    if (sys_dev_io_read(g_st.pci, bar, o, width, &v) != 0) {
        if (!g_io_errors++)
            fprintf(stderr, "ac97: port access refused (bar %d offset %#x) -- "
                            "is this device claimed?\n", bar, (unsigned)o);
        return 0;
    }
    return v;
}

static inline uint8_t  nabm8(uint32_t o)  { return (uint8_t)rd(BAR_NABM, o, 1); }
static inline uint16_t nabm16(uint32_t o) { return (uint16_t)rd(BAR_NABM, o, 2); }
static inline uint32_t nabm32(uint32_t o) { return rd(BAR_NABM, o, 4); }
static inline void wnabm8(uint32_t o, uint8_t v)   { sys_dev_io_write(g_st.pci, BAR_NABM, o, 1, v); }
static inline void wnabm16(uint32_t o, uint16_t v) { sys_dev_io_write(g_st.pci, BAR_NABM, o, 2, v); }
static inline void wnabm32(uint32_t o, uint32_t v) { sys_dev_io_write(g_st.pci, BAR_NABM, o, 4, v); }
static inline void wnam16(uint32_t o, uint16_t v)  { sys_dev_io_write(g_st.pci, BAR_NAM, o, 2, v); }
static inline uint16_t nam16(uint32_t o) { return (uint16_t)rd(BAR_NAM, o, 2); }

// VRA, as kernel/drivers/sound/ac97.c probes it: on, then each standard
// rate written and READ BACK -- a codec rounds a rate it cannot do, and
// a rounded one is not on offer. Back at 48 kHz afterwards.
static uint32_t probe_vra(void) {
    uint32_t rates = SND_RATE_48000;
    if (!(nam16(NAM_EXT_ID) & EXT_VRA)) return rates;
    wnam16(NAM_EXT_CTL, nam16(NAM_EXT_CTL) | EXT_VRA);
    if (!(nam16(NAM_EXT_CTL) & EXT_VRA)) return rates;
    static const struct { uint16_t hz; uint32_t bit; } T[] = {
        { 8000, SND_RATE_8000 }, { 11025, SND_RATE_11025 }, { 16000, SND_RATE_16000 },
        { 22050, SND_RATE_22050 }, { 32000, SND_RATE_32000 }, { 44100, SND_RATE_44100 },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        wnam16(NAM_DAC_RATE, T[i].hz);
        if (nam16(NAM_DAC_RATE) == T[i].hz) rates |= T[i].bit;
    }
    wnam16(NAM_DAC_RATE, SND_RATE);
    g_st.vra = 1;
    return rates;
}

static int ac97_match(const struct pci_device *d) {
    return d->class_code == AC97_CLASS && d->subclass == AC97_SUBCLASS;
}

static int ac97_open(struct snd_dev *dev) {
    memset(&g_st, 0, sizeof g_st);
    g_st.pci = dev->pci;
    if (!dev->dma) return -1;
    g_st.bdl = dev->dma;
    g_st.bdl_phys = dev->dma_phys;

    // BOTH BARs MUST BE I/O. A part that presents memory BARs is not
    // this chip however its class reads, and SYS_DEV_IO would refuse
    // every access anyway -- said here rather than discovered as a
    // silent wall of -EINVAL.
    if ((dev->info.bar[BAR_NAM] & 1) == 0 || (dev->info.bar[BAR_NABM] & 1) == 0) {
        fprintf(stderr, "ac97: unexpected memory BARs -- not driving it\n");
        return -1;
    }

    // Cold reset released, then wait for the codec. A process can just
    // sleep for this, where the kernel driver counted PIT ticks.
    wnabm32(GLOB_CNT, GC_COLD_RESET);
    int ready = 0;
    for (int i = 0; i < 100 && !ready; i++) {
        if (nabm32(GLOB_STA) & GS_CODEC_READY) ready = 1;
        else usleep(1000);
    }
    if (!ready) {
        fprintf(stderr, "ac97: the codec never reported ready\n");
        return -1;
    }
    if (g_io_errors) return -1;   // "ready" would have been a refused read

    // 48 kHz 16-bit is every AC97's baseline; a Variable Rate Audio
    // codec adds the rates it reads back exactly.
    dev->rates = probe_vra();
    dev->depths = SND_DEPTH_16;
    dev->rate = SND_RATE;
    // 16-BIT, so the engine plays the host's narrowed copy of the s32
    // ring, in the DMA grant after the descriptor list's page.
    dev->bits = 16;
    dev->bounce = (int16_t *)((uint8_t *)dev->dma + 4096);
    dev->bounce_phys = dev->dma_phys + 4096;
    memset(dev->bounce, 0, SND_CHUNKS * SND_CHUNK_BYTES_S16);
    dev->priv = &g_st;
    return 0;
}

static void ac97_close(struct snd_dev *dev) {
    (void)dev;
    wnabm8(PO_CR, 0);
}

static int ac97_start(struct snd_dev *dev, uint64_t ring_phys) {
    (void)ring_phys;   // the engine reads dev->bounce, never the ring
    if (dev->bits != 16 || (dev->rate != SND_RATE && !g_st.vra)) return -1;
    if (g_st.vra) wnam16(NAM_DAC_RATE, (uint16_t)dev->rate);
    // Reset the box, then arm it. RR self-clears when the reset is done.
    wnabm8(PO_CR, CR_RR);
    int done = 0;
    for (int i = 0; i < 100 && !done; i++) {
        if (!(nabm8(PO_CR) & CR_RR)) done = 1;
        else usleep(100);
    }
    if (!done) return -1;

    // SAMPLES, NOT BYTES -- this chip counts 16-bit units.
    for (int k = 0; k < SND_CHUNKS; k++) {
        g_st.bdl[k].addr = (uint32_t)(dev->bounce_phys + (uint64_t)k * SND_CHUNK_BYTES_S16);
        g_st.bdl[k].samples = SND_CHUNK_BYTES_S16 / 2;
        g_st.bdl[k].flags = 0x8000; // IOC: one interrupt per chunk
    }

    wnabm32(PO_BDBAR, (uint32_t)g_st.bdl_phys);
    wnabm8(PO_LVI, SND_CHUNKS - 1);
    wnabm16(PO_SR, SR_ACK);
    wnabm8(PO_CR, CR_RPBM | CR_IOCE);
    g_st.running = 1;
    return 0;
}

static void ac97_stop(struct snd_dev *dev) {
    (void)dev;
    wnabm8(PO_CR, 0);
    wnabm16(PO_SR, SR_ACK);
    g_st.running = 0;
}

static int ac97_period(struct snd_dev *dev) {
    (void)dev;
    if (!g_st.running) return SND_IRQ_NOT_MINE;
    uint16_t sr = nabm16(PO_SR);
    if (!(sr & SR_ACK)) return SND_IRQ_NOT_MINE; // a shared line is legal

    // CIV IS THE DESCRIPTOR BEING PLAYED, so the chunks BEFORE it are
    // the consumed ones -- this chip reports its position directly and
    // exactly, which HD Audio's fetch-ahead LPIB does not.
    uint8_t civ = (uint8_t)(nabm8(PO_CIV) & (SND_CHUNKS - 1));
    // One behind the player, or the engine sees "last valid" and stops.
    wnabm8(PO_LVI, (uint8_t)((civ + SND_CHUNKS - 1) & (SND_CHUNKS - 1)));
    wnabm16(PO_SR, sr & SR_ACK); // RW1C: only the bits seen
    return (int)((uint32_t)civ * SND_CHUNK_BYTES);
}

// The same taper as kernel/drivers/sound/ac97.c, which says why.
#define AC97_TAPER_DB 40
#define AC97_PCM_0DB  0x0808

static void ac97_set_volume(struct snd_dev *dev, int pct) {
    (void)dev;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    wnam16(NAM_PCM_VOL, AC97_PCM_0DB);
    if (pct == 0) { wnam16(NAM_MASTER_VOL, 0x8000); return; }
    uint16_t att = (uint16_t)(AC97_TAPER_DB * 2 * (100 - pct) / 300);
    wnam16(NAM_MASTER_VOL, (uint16_t)((att << 8) | att));
}

const struct snd_driver snd_driver = {
    .abi = SND_DRIVER_ABI,
    .name = "ac97",
    .label = "Intel AC'97 (ring 3)",
    .dma_bytes = 4096 + SND_CHUNKS * SND_CHUNK_BYTES_S16,  // the list's page + a 16-bit copy
    .match = ac97_match,
    .open = ac97_open,
    .close = ac97_close,
    .start = ac97_start,
    .stop = ac97_stop,
    .period = ac97_period,
    .set_volume = ac97_set_volume,
};
