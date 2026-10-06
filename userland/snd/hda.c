// /lib/snd/hda.so -- Intel High Definition Audio, driven from ring 3.
//
// THE CHIP HALF ONLY. /bin/snddrv claims the controller, maps BAR0,
// grants the DMA buffer, registers with the kernel's sound core and
// runs the request loop; this file brings the controller out of reset,
// drives the CORB and RIRB, walks the codec, routes an analog output
// and programs the stream descriptor. userland/include/snd_driver.h is
// the contract.
//
// THE CODEC PARSER IS NOT IN THIS FILE. kernel/lib/hda_codec.c is
// compiled for ring 3 as well, so the graph walk here is the same
// implementation the tree has always had -- the geom.c/klineedit.c
// rule. What is here is the TRANSPORT.
//
// THREE THINGS THAT BITE, all paid for already:
//
//  - THE CONTROLLER ARRIVES IN RESET and its whole register file reads
//    zero, because the previous owner wrote GCTL.CRST low on its way
//    out. CRST reading back high is the write being ACCEPTED, not the
//    link being ready; the codecs need the spec's 25 frames, which a
//    process can simply sleep for.
//  - A REGISTER IS READ AT ITS OWN WIDTH. GCAP is 16 bits, and reading
//    it as two bytes gives 0x0001: a device models a register, not
//    memory.
//  - COUNT COMPLETIONS, DO NOT TRUST LPIB. It is a FETCH position and
//    runs ahead of what has been played, and a process reads it
//    milliseconds after the interrupt rather than inside it.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "snd_driver.h"
#include "hda_codec.h"
#include "sound_abi.h"
#include "rt/sys.h"

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03

struct bdl_entry { uint64_t addr; uint32_t len; uint32_t ioc; };

struct hda_state {
    volatile uint8_t *mmio;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    volatile struct bdl_entry *bdl;
    uint64_t dma_phys;
    uint16_t corb_ents, rirb_ents, rirb_rp;
    uint8_t  cad;
    uint32_t sd;            // our output stream descriptor's base
    uint32_t chunk;         // the ring chunk the card has finished
    int      running;
    uint16_t fmt;           // HDA_FMT_48K_STEREO(dev->bits)
    uint32_t chunk_bytes;   // one BDL entry at that width
    struct hda_codec codec;
};

static struct hda_state g_st;

static inline uint8_t  mr8(uint32_t o)  { return *(volatile uint8_t *)(g_st.mmio + o); }
static inline uint16_t mr16(uint32_t o) { return *(volatile uint16_t *)(g_st.mmio + o); }
static inline uint32_t mr32(uint32_t o) { return *(volatile uint32_t *)(g_st.mmio + o); }
static inline void mw8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(g_st.mmio + o) = v; }
static inline void mw16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_st.mmio + o) = v; }
static inline void mw32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_st.mmio + o) = v; }

// --- the codec's transport -------------------------------------------

static int corb_cmd(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out) {
    (void)ctx;
    if (out) *out = 0;
    uint16_t wp = (uint16_t)((mr16(HDA_CORBWP) + 1) % g_st.corb_ents);
    g_st.corb[wp] = ((uint32_t)g_st.cad << 28) | ((uint32_t)nid << 20) | (verb20 & 0xFFFFF);
    mw16(HDA_CORBWP, wp);
    for (int lap = 0; lap < 2000; lap++) {
        mw8(HDA_RIRBSTS, RIRBSTS_ACK);
        uint16_t rwp = mr16(HDA_RIRBWP) & 0xFF;
        while (g_st.rirb_rp != rwp) {
            g_st.rirb_rp = (uint16_t)((g_st.rirb_rp + 1) % g_st.rirb_ents);
            uint64_t e = g_st.rirb[g_st.rirb_rp];
            if ((uint32_t)(e >> 32) & 0x10) continue;   // unsolicited
            if (out) *out = (uint32_t)e;
            return 0;
        }
        usleep(200);
    }
    g_st.rirb_rp = (uint16_t)((mr16(HDA_RIRBWP) & 0xFF) % g_st.rirb_ents);
    return -1;
}

// --- the driver ops ---------------------------------------------------

static int hda_match(const struct pci_device *d) {
    return d->class_code == HDA_CLASS && d->subclass == HDA_SUBCLASS;
}

static int hda_open(struct snd_dev *dev) {
    memset(&g_st, 0, sizeof g_st);
    g_st.mmio = dev->bar[0];
    if (!g_st.mmio || !dev->dma) return -1;
    g_st.dma_phys = dev->dma_phys;
    g_st.corb = (volatile uint32_t *)((uint8_t *)dev->dma + HDA_CORB_OFF);
    g_st.rirb = (volatile uint64_t *)((uint8_t *)dev->dma + HDA_RIRB_OFF);
    g_st.bdl  = (volatile struct bdl_entry *)((uint8_t *)dev->dma + 3072);
    g_st.corb_ents = 256;
    g_st.rirb_ents = 256;

    // Out of reset, then wait for the codecs -- the thing a process can
    // do and a ring-0 probe cannot.
    mw32(HDA_INTCTL, 0);
    mw32(HDA_GCTL, mr32(HDA_GCTL) & ~(uint32_t)GCTL_CRST);
    for (int i = 0; i < 100 && (mr32(HDA_GCTL) & GCTL_CRST); i++) usleep(1000);
    mw32(HDA_GCTL, GCTL_CRST);
    for (int i = 0; i < 100 && !(mr32(HDA_GCTL) & GCTL_CRST); i++) usleep(1000);
    usleep(50000);

    uint16_t gcap = mr16(HDA_GCAP);
    uint16_t statests = mr16(HDA_STATESTS) & 0x7FFF;
    mw16(HDA_STATESTS, statests);
    mw32(HDA_GCTL, GCTL_CRST | GCTL_UNSOL);
    g_st.sd = HDA_SD_BASE + (uint32_t)((gcap >> 8) & 0xF) * 0x20;
    if (!statests) return -1;

    mw8(HDA_CORBCTL, 0);
    mw8(HDA_RIRBCTL, 0);
    mw8(HDA_CORBSIZE, 2);
    mw32(HDA_CORBLBASE, (uint32_t)(g_st.dma_phys + HDA_CORB_OFF));
    mw32(HDA_CORBUBASE, (uint32_t)((g_st.dma_phys + HDA_CORB_OFF) >> 32));
    mw16(HDA_CORBWP, 0);
    mw16(HDA_CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(mr16(HDA_CORBRP) & 0x8000); i++) usleep(100);
    mw16(HDA_CORBRP, 0);
    for (int i = 0; i < 100 && (mr16(HDA_CORBRP) & 0x8000); i++) usleep(100);
    mw8(HDA_RIRBSIZE, 2);
    mw32(HDA_RIRBLBASE, (uint32_t)(g_st.dma_phys + HDA_RIRB_OFF));
    mw32(HDA_RIRBUBASE, (uint32_t)((g_st.dma_phys + HDA_RIRB_OFF) >> 32));
    mw16(HDA_RIRBWP, 0x8000);
    mw16(HDA_RINTCNT, 1);
    mw8(HDA_RIRBSTS, RIRBSTS_ACK);
    g_st.rirb_rp = 0;
    mw8(HDA_CORBCTL, CORBCTL_RUN);
    mw8(HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);

    g_st.codec.cmd = corb_cmd;
    g_st.codec.ctx = 0;
    for (uint8_t cad = 0; cad < 15; cad++) {
        if (!(statests & (1u << cad))) continue;
        g_st.cad = cad;
        uint32_t vendor = 0;
        if (corb_cmd(0, 0, V12(VERB_GET_PARAM, PARAM_VENDOR_ID), &vendor) != 0) continue;
        g_st.codec.vendor = vendor;
        if (hda_codec_enumerate(&g_st.codec) != 0) continue;
        // NOT A FAILURE, AN ANSWER: a display-audio codec's pins are
        // all digital, so there is no analog route to find.
        if (hda_codec_pick_outputs(&g_st.codec) != 0) continue;
        // The deepest width the DAC reports, as kernel/drivers/sound/
        // hda.c picks it: 20/24/32 read the s32 ring directly, 16 plays
        // the host's narrowed copy in the rest of this DMA grant.
        hda_codec_pcm_support(&g_st.codec, g_st.codec.spk.dac,
                              &dev->rates, &dev->depths);
        dev->bits = (dev->depths & SND_DEPTH_32) ? 32 : (dev->depths & SND_DEPTH_24) ? 24 :
                    (dev->depths & SND_DEPTH_20) ? 20 : 16;
        g_st.fmt = HDA_FMT_48K_STEREO(dev->bits);
        g_st.chunk_bytes = dev->bits == 16 ? SND_CHUNK_BYTES_S16 : SND_CHUNK_BYTES;
        if (dev->bits == 16) {
            dev->bounce = (int16_t *)((uint8_t *)dev->dma + HDA_RING_BYTES);
            dev->bounce_phys = dev->dma_phys + HDA_RING_BYTES;
            memset(dev->bounce, 0, SND_CHUNKS * SND_CHUNK_BYTES_S16);
        }
        hda_codec_route_output(&g_st.codec, &g_st.codec.spk, g_st.fmt, HDA_STREAM_TAG);
        dev->priv = &g_st;
        return 0;
    }
    return -1;
}

static void hda_close(struct snd_dev *dev) {
    (void)dev;
    if (g_st.mmio) mw32(HDA_INTCTL, 0);
}

static int hda_start(struct snd_dev *dev, uint64_t ring_phys) {
    uint64_t base = dev->bounce ? dev->bounce_phys : ring_phys;
    mw32(g_st.sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(g_st.sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    mw32(g_st.sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 100 && !(mr32(g_st.sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    mw32(g_st.sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(g_st.sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    if (mr32(g_st.sd + SD_CTL) & SD_CTL_SRST) return -1;

    // The descriptor list points at the KERNEL's ring, not at anything
    // this process owns -- and is built here rather than once at open,
    // because it is start() that is handed the address.
    for (int k = 0; k < SND_CHUNKS; k++) {
        g_st.bdl[k].addr = base + (uint64_t)k * g_st.chunk_bytes;
        g_st.bdl[k].len = g_st.chunk_bytes;
        g_st.bdl[k].ioc = 1;
    }
    uint64_t bdl = g_st.dma_phys + 3072;
    mw32(g_st.sd + SD_BDPL, (uint32_t)bdl);
    mw32(g_st.sd + SD_BDPU, (uint32_t)(bdl >> 32));
    mw32(g_st.sd + SD_CBL, SND_CHUNKS * g_st.chunk_bytes);
    mw16(g_st.sd + SD_LVI, SND_CHUNKS - 1);
    mw16(g_st.sd + SD_FMT, g_st.fmt);
    mw8(g_st.sd + SD_STS, SD_STS_ACK);
    mw32(HDA_INTCTL, INTCTL_GIE | INTCTL_CIE |
                     (1u << ((mr16(HDA_GCAP) >> 8) & 0xF)));
    mw32(g_st.sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SD_CTL_STREAM_SHIFT) |
                           SD_CTL_RUN | SD_CTL_IOCE);
    g_st.chunk = 0;
    g_st.running = 1;
    return 0;
}

static void hda_stop(struct snd_dev *dev) {
    (void)dev;
    mw32(g_st.sd + SD_CTL, mr32(g_st.sd + SD_CTL) &
         ~(uint32_t)(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFF);
    for (int i = 0; i < 100 && (mr32(g_st.sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    mw8(g_st.sd + SD_STS, SD_STS_ACK);
    g_st.running = 0;
}

static int hda_period(struct snd_dev *dev) {
    (void)dev;
    if (!g_st.running) return SND_IRQ_NOT_MINE;
    uint8_t st = mr8(g_st.sd + SD_STS);
    if (!(st & SD_STS_BCIS)) return SND_IRQ_NOT_MINE;
    mw8(g_st.sd + SD_STS, st & SD_STS_ACK);

    // ONE COMPLETION PER CHUNK, SO COUNT THEM -- see this file's header.
    // LPIB is still the correction, in BOTH directions: the interrupt
    // count includes RIRB's, so it can run ahead as well as behind.
    uint32_t next = (g_st.chunk + 1) % SND_CHUNKS;
    uint32_t lc = (mr32(g_st.sd + SD_LPIB) / g_st.chunk_bytes) % SND_CHUNKS;
    uint32_t behind = (lc + SND_CHUNKS - next) % SND_CHUNKS;
    if (behind > SND_CHUNKS / 2) next = lc;
    else if (behind >= 2) next = (lc + SND_CHUNKS - 1) % SND_CHUNKS;
    g_st.chunk = next;
    return (int)(next * SND_CHUNK_BYTES);
}

// 0..100 onto the route's amplifier with the stack's 40 dB taper, so
// one percentage is one loudness wherever it is applied.
#define TAPER_DB 40

static void hda_set_volume(struct snd_dev *dev, int pct) {
    (void)dev;
    struct hda_out *o = &g_st.codec.spk;
    if (!g_st.codec.have_spk || !o->vol_nid) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint16_t pl = AMP_OUT | AMP_LEFT | AMP_RIGHT;
    if (pct == 0 && o->vol_mute) {
        pl |= AMP_MUTE;
    } else {
        uint32_t atten_qdb = (uint32_t)TAPER_DB * 4u * (uint32_t)(100 - pct) / 100u;
        uint32_t down = atten_qdb / (o->vol_step_qdb ? o->vol_step_qdb : 1);
        pl |= (down < o->vol_offset) ? (uint16_t)(o->vol_offset - down) : 0;
    }
    corb_cmd(0, o->vol_nid, V4(VERB_SET_AMP, pl), 0);
}

const struct snd_driver snd_driver = {
    .abi = SND_DRIVER_ABI,
    .name = "hda",
    .label = "HD Audio (ring 3)",
    .dma_bytes = HDA_RING_BYTES + SND_CHUNKS * SND_CHUNK_BYTES_S16,  // + a 16-bit copy
    .match = hda_match,
    .open = hda_open,
    .close = hda_close,
    .start = hda_start,
    .stop = hda_stop,
    .period = hda_period,
    .set_volume = hda_set_volume,
};
