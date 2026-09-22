// Intel High Definition Audio -- the third sound_device (kernel/sound.h),
// and the one a laptop's own speakers are behind. Matched by PCI class
// (0x0403), so QEMU's `-device ich9-intel-hda` and a PCH's controller
// claim alike.
//
// Two halves, as in Linux's snd-hda-intel + the generic codec parser:
//
//  - THE CONTROLLER: one MMIO BAR. Commands to the codec go through the
//    CORB (a ring of 32-bit verbs the controller DMAs out) and answers
//    come back on the RIRB (a ring of 64-bit responses it DMAs in).
//    Playback is a STREAM DESCRIPTOR playing a buffer descriptor list:
//    one entry per chunk of the core's shared ring, each flagged IOC,
//    and the list is CYCLIC by construction (CBL wraps to entry 0), so
//    the engine never halts and a stalled app plays whatever the core
//    zeroed -- silence (abi/sound_abi.h's one rule).
//  - THE CODEC: a graph of WIDGETS -- DACs, mixers, selectors, pin
//    complexes -- that the codec describes through parameters and the
//    driver has to ROUTE: pick an output pin by what its default
//    configuration says it is wired to (a speaker, a headphone jack),
//    walk its connection list back to a DAC, and unmute every amplifier
//    on the way. Linux's `snd-hda-codec-realtek` is 12k lines of quirks
//    on top of that walk; this is the walk alone.
//
// THE COMMAND PATH RUNS WITH INTERRUPTS OFF, on purpose. The RIRB has one
// consumer at a time, and both a thread (init, a volume change, a KTEST)
// and the interrupt handler (a jack event) need to send a verb and read
// its answer. Serialising in hda_cmd() -- save flags, cli, send, drain
// until the answer, restore -- is what lets one mailbox serve both; a
// codec answers within a frame (20.8 us), so the window is short.
//
// DMA addresses are PHYSICAL and the kernel relocates itself, so the
// CORB, RIRB and BDL share one pmm frame (the virtio rule), never a
// static.
#include "sound.h"
#include "hda_codec.h" // the registers, the verbs and the graph parser
#include "clocksource.h" // clocksource_delay_ms -- a delay that needs no interrupt
#include "sound_abi.h"
#include "pci.h"
#include "pci_internal.h"
#include "paging.h"
#include "lapic.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "timer.h"
#include "barrier.h"
#include "string.h"
#include "kfmt.h"
#include "klog.h"
#include "ktest.h"
#include "driver.h"
#include "multiboot.h" // multiboot_cmdline() -- the `hdadump` boot word
#include "fixed.h"     // fx_sin, for the kernel.hda_tone diagnostic
#include <stdint.h>
#include "pci_driver.h"

DRIVER_DECLARE("hda", "sound", "Intel High Definition Audio");

// The controller registers, the codec's verbs and the graph parser are
// all in api/hda_codec.h, shared with /bin/lscodec.
//
// PCI config space, Intel SCH/PCH controllers only.
#define HDA_INTEL_DEVC         0x78
#define HDA_INTEL_DEVC_NOSNOOP (1u << 11)



// --- state ---------------------------------------------------------------
#define HDA_MAX_CTRL   2   // a PCH controller and a display-audio one
#define HDA_UNSOL_TAG  1

struct hda_bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc; // bit 0
};

struct hda_ctrl {
    const struct pci_device *pci;
    volatile uint8_t *mmio;
    int index;
    int iss, oss;
    uint32_t sd;       // our output stream descriptor's register base
    int      sd_index; // its bit in INTCTL/INTSTS

    // One DMA frame: CORB at +0, RIRB at +1024, BDL at +3072.
    uint64_t dma_phys;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    struct hda_bdl_entry *bdl;
    uint16_t corb_ents, rirb_ents;
    uint16_t rirb_rp;
    uint32_t resp;
    int      resp_ready;

    uint8_t  cad;
    // The graph, and the parser that walks it -- api/hda_codec.h, the
    // same object /bin/lscodec builds over its own CORB/RIRB.
    struct hda_codec codec;
    int hp_plugged;
    int jack_pending, in_jack;

    uint8_t msi_vector, irq;
    uint32_t irqs, jack_events;
    uint32_t chunk;    // the chunk the engine is in, counted from BCIS
    uint32_t fifoe, dese; // stream error counts, for the log
    uint32_t diag;     // kernel.hda_tone: completions left; the handler
                       // leaves the ring alone and stops the stream at 0

    struct sound_device dev;
    char name[SOUND_NAME_MAX];
    char label[40];
    int registered;
};

static struct hda_ctrl g_hc[HDA_MAX_CTRL];
static int g_nctrl;

// --- MMIO -----------------------------------------------------------------
static inline uint8_t  mr8(struct hda_ctrl *h, uint32_t off)  { return *(volatile uint8_t *)(h->mmio + off); }
static inline uint16_t mr16(struct hda_ctrl *h, uint32_t off) { return *(volatile uint16_t *)(h->mmio + off); }
static inline uint32_t mr32(struct hda_ctrl *h, uint32_t off) { return *(volatile uint32_t *)(h->mmio + off); }
static inline void mw8(struct hda_ctrl *h, uint32_t off, uint8_t v)   { *(volatile uint8_t *)(h->mmio + off) = v; }
static inline void mw16(struct hda_ctrl *h, uint32_t off, uint16_t v) { *(volatile uint16_t *)(h->mmio + off) = v; }
static inline void mw32(struct hda_ctrl *h, uint32_t off, uint32_t v) { *(volatile uint32_t *)(h->mmio + off) = v; }

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

// Spin until (reg & mask) == want, bounded by a SPIN COUNT rather than
// pit_ticks(): start()/stop() run from a syscall, where interrupts are
// off (context_switch.asm) and a tick deadline never arrives. ~1M
// MMIO reads is milliseconds on anything real, tens under TCG.
#define HDA_SPIN_MAX 1000000u

static int wait_reg32(struct hda_ctrl *h, uint32_t off, uint32_t mask, uint32_t want) {
    for (uint32_t n = 0; n < HDA_SPIN_MAX; n++) {
        if ((mr32(h, off) & mask) == want) return 0;
        cpu_relax();
    }
    return -1;
}

static int wait_reg8(struct hda_ctrl *h, uint32_t off, uint8_t mask, uint8_t want) {
    for (uint32_t n = 0; n < HDA_SPIN_MAX; n++) {
        if ((mr8(h, off) & mask) == want) return 0;
        cpu_relax();
    }
    return -1;
}

// --- CORB / RIRB ---------------------------------------------------------

static void jack_update(struct hda_ctrl *h);

// Consume every response the controller has written. Runs with
// interrupts off -- from the handler, or inside hda_cmd()'s window.
// ACKNOWLEDGES RIRBSTS HERE, not only in the handler: QEMU's controller
// stops fetching from the CORB once RINTCNT responses are pending
// until RINTFL is cleared, so a polled command path that never wrote
// it got exactly one answer and then silence.
static void rirb_drain(struct hda_ctrl *h) {
    mw8(h, HDA_RIRBSTS, RIRBSTS_ACK);
    uint16_t wp = mr16(h, HDA_RIRBWP) & 0xFF;
    while (h->rirb_rp != wp) {
        h->rirb_rp = (uint16_t)((h->rirb_rp + 1) % h->rirb_ents);
        uint64_t e = h->rirb[h->rirb_rp];
        uint32_t ex = (uint32_t)(e >> 32);
        if (ex & 0x10) {
            // Unsolicited: a jack event. Handled after the drain, never
            // inside it -- handling sends verbs, which drain.
            if ((ex & 0xF) == h->cad) h->jack_pending = 1;
        } else {
            h->resp = (uint32_t)e;
            h->resp_ready = 1;
        }
    }
}

// Send one verb to `nid` on the codec and wait for its answer. Returns
// 0 with *out filled (NULL allowed), -1 on a timeout. See the top of
// the file for why the whole exchange runs with interrupts off.
static int hda_cmd(struct hda_ctrl *h, uint8_t nid, uint32_t verb20, uint32_t *out) {
    uint64_t f = irq_save();
    h->resp_ready = 0;
    uint16_t wp = (uint16_t)((mr16(h, HDA_CORBWP) + 1) % h->corb_ents);
    h->corb[wp] = ((uint32_t)h->cad << 28) | ((uint32_t)nid << 20) | (verb20 & 0xFFFFF);
    kmb();
    mw16(h, HDA_CORBWP, wp);
    // A codec answers within a frame; a few ms of spinning is the
    // generous bound, and pit_ticks() cannot be used with IF clear.
    for (uint32_t spins = 0; !h->resp_ready && spins < 4000000u; spins++) {
        rirb_drain(h);
        cpu_relax();
    }
    int ok = h->resp_ready;
    if (out) *out = ok ? h->resp : 0;
    irq_restore(f);
    if (h->jack_pending && !h->in_jack) jack_update(h);
    return ok ? 0 : -1;
}

// The parser's transport: one verb in, one response out. This is the
// whole of what api/hda_codec.h needs from a controller, and what
// /bin/lscodec supplies from its own CORB/RIRB.
static int hda_codec_cmd(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out) {
    return hda_cmd((struct hda_ctrl *)ctx, nid, verb20, out);
}

// Set the CORB and RIRB up and start them. Sizes come from what the
// controller supports (256 entries on everything real; the register
// says so rather than the driver assuming).
static int corb_rirb_init(struct hda_ctrl *h) {
    uint8_t csz = mr8(h, HDA_CORBSIZE) >> 4;
    uint8_t rsz = mr8(h, HDA_RIRBSIZE) >> 4;
    uint8_t csel = (csz & 4) ? 2 : (csz & 2) ? 1 : 0;
    uint8_t rsel = (rsz & 4) ? 2 : (rsz & 2) ? 1 : 0;
    h->corb_ents = csel == 2 ? 256 : csel == 1 ? 16 : 2;
    h->rirb_ents = rsel == 2 ? 256 : rsel == 1 ? 16 : 2;

    mw8(h, HDA_CORBCTL, 0);
    mw8(h, HDA_RIRBCTL, 0);
    wait_reg8(h, HDA_CORBCTL, CORBCTL_RUN, 0);

    mw8(h, HDA_CORBSIZE, csel);
    mw32(h, HDA_CORBLBASE, (uint32_t)h->dma_phys);
    mw32(h, HDA_CORBUBASE, (uint32_t)(h->dma_phys >> 32));
    mw16(h, HDA_CORBWP, 0);
    // The read-pointer reset handshake: set, see it set, clear, see it
    // clear. Some controllers never show the set half (Linux tolerates
    // the same), so the waits are bounded and not fatal.
    mw16(h, HDA_CORBRP, 0x8000);
    for (int i = 0; i < 1000 && !(mr16(h, HDA_CORBRP) & 0x8000); i++) cpu_relax();
    mw16(h, HDA_CORBRP, 0);
    for (int i = 0; i < 1000 && (mr16(h, HDA_CORBRP) & 0x8000); i++) cpu_relax();

    mw8(h, HDA_RIRBSIZE, rsel);
    mw32(h, HDA_RIRBLBASE, (uint32_t)(h->dma_phys + 1024));
    mw32(h, HDA_RIRBUBASE, (uint32_t)((h->dma_phys + 1024) >> 32));
    mw16(h, HDA_RIRBWP, 0x8000);
    mw16(h, HDA_RINTCNT, 1);
    mw8(h, HDA_RIRBSTS, RIRBSTS_ACK);
    h->rirb_rp = 0;

    mw8(h, HDA_CORBCTL, CORBCTL_RUN);
    mw8(h, HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    return 0;
}

// --- the codec graph ---------------------------------------------------
//
// THE WALK ITSELF IS IN kernel/lib/hda_codec.c, compiled into ring 3 as
// well (docs/umdf-design.md stage 3). What stays here is the half that
// WRITES -- routing, amplifiers, pin control -- and the klog dump.

static int want_dump(void) {
    const char *c = multiboot_cmdline();
    return c && k_strstr(c, "hdadump");
}

static void dump_widgets(struct hda_ctrl *h) {
    for (int i = 0; i < h->codec.nwidgets; i++) {
        struct hda_widget *w = &h->codec.widgets[i];
        uint8_t conns[HDA_CONN_MAX];
        int n = hda_codec_conn_list(&h->codec, w->nid, conns, HDA_CONN_MAX);
        char cl[3 * 12 + 4] = "";
        int used = 0;
        for (int k = 0; k < n && k < 12; k++)
            used += k_snprintf(cl + used, sizeof cl - (uint32_t)used, " %02x", conns[k]);
        uint32_t oc = (w->caps & WCAP_OUT_AMP) ? hda_codec_amp_caps(&h->codec, w, 1) : 0;
        uint32_t ic = (w->caps & WCAP_IN_AMP) ? hda_codec_amp_caps(&h->codec, w, 0) : 0;
        klog_printf("%s: nid %02x type %u caps %08x pincap %08x defcfg %08x oamp %08x iamp %08x conn%s%s\n",
                    h->name, w->nid, w->type, w->caps, w->pincap, w->defcfg, oc, ic,
                    cl, n > 12 ? " ..." : "");
    }
}


// Headphones in: the jack drives and the speaker pin is switched off,
// as every laptop does it. Out: the reverse.
static void apply_jack_state(struct hda_ctrl *h) {
    if (!h->codec.have_hp) return;
    struct hda_widget *hpw = hda_codec_widget(&h->codec, h->codec.hp.pin);
    uint8_t hp_ctl = PINCTL_OUT_EN | ((hpw && (hpw->pincap & PINCAP_HP_DRIVE)) ? PINCTL_HP_EN : 0);
    hda_cmd(h, h->codec.hp.pin,  V12(VERB_SET_PIN_CTL, h->hp_plugged ? hp_ctl : 0), 0);
    hda_cmd(h, h->codec.spk.pin, V12(VERB_SET_PIN_CTL, h->hp_plugged ? 0 : PINCTL_OUT_EN), 0);
}

static void jack_update(struct hda_ctrl *h) {
    if (!h->codec.have_hp) { h->jack_pending = 0; return; }
    h->in_jack = 1;
    h->jack_pending = 0;
    uint32_t sense = 0;
    if (hda_cmd(h, h->codec.hp.pin, V12(VERB_GET_PIN_SENSE, 0), &sense) == 0) {
        int plugged = (sense >> 31) & 1;
        if (plugged != h->hp_plugged) {
            h->hp_plugged = plugged;
            h->jack_events++;
            apply_jack_state(h);
            klog_printf("%s: headphones %s\n", h->name, plugged ? "plugged in" : "unplugged");
        }
    }
    h->in_jack = 0;
}

// --- the stream ------------------------------------------------------

static void bdl_fill(struct hda_ctrl *h, uint64_t ring_phys) {
    for (int i = 0; i < SND_CHUNKS; i++) {
        h->bdl[i].addr = ring_phys + (uint64_t)i * SND_CHUNK_BYTES;
        h->bdl[i].len = SND_CHUNK_BYTES;
        h->bdl[i].ioc = 1;
    }
}

static int stream_start(struct hda_ctrl *h) {
    uint32_t sd = h->sd;
    mw32(h, sd + SD_CTL, 0);
    wait_reg32(h, sd + SD_CTL, SD_CTL_RUN, 0);
    mw32(h, sd + SD_CTL, SD_CTL_SRST);
    wait_reg32(h, sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST);
    mw32(h, sd + SD_CTL, 0);
    if (wait_reg32(h, sd + SD_CTL, SD_CTL_SRST, 0) != 0) return -1;

    mw32(h, sd + SD_BDPL, (uint32_t)(h->dma_phys + 3072));
    mw32(h, sd + SD_BDPU, (uint32_t)((h->dma_phys + 3072) >> 32));
    mw32(h, sd + SD_CBL, SND_RING_BYTES);
    mw16(h, sd + SD_LVI, SND_CHUNKS - 1);
    mw16(h, sd + SD_FMT, HDA_FMT_48K_S16_STEREO);
    mw8(h, sd + SD_STS, SD_STS_ACK);

    // The codec side again: a stream reset leaves the converter as it
    // was, but a codec that was power-cycled would not be.
    for (int p = 0; p < 2; p++) {
        struct hda_out *o = p ? &h->codec.hp : &h->codec.spk;
        if (!(p ? h->codec.have_hp : h->codec.have_spk)) continue;
        hda_cmd(h, o->dac, V4(VERB_SET_FORMAT, HDA_FMT_48K_S16_STEREO), 0);
        hda_cmd(h, o->dac, V12(VERB_SET_CONV, HDA_STREAM_TAG << 4), 0);
    }

    h->chunk = 0;
    mw32(h, HDA_INTCTL, mr32(h, HDA_INTCTL) | INTCTL_GIE | (1u << h->sd_index));
    mw32(h, sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SD_CTL_STREAM_SHIFT) |
                         SD_CTL_RUN | SD_CTL_IOCE | SD_CTL_FEIE | SD_CTL_DEIE);
    return 0;
}

static void stream_stop(struct hda_ctrl *h) {
    uint32_t sd = h->sd;
    mw32(h, sd + SD_CTL, mr32(h, sd + SD_CTL) & ~(uint32_t)(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFF);
    wait_reg32(h, sd + SD_CTL, SD_CTL_RUN, 0);
    mw32(h, HDA_INTCTL, mr32(h, HDA_INTCTL) & ~(1u << h->sd_index));
    mw8(h, sd + SD_STS, SD_STS_ACK);
}

// 0..100 onto the route's amplifier with the taper sound_usb.c uses:
// linear in dB across 40 dB of attenuation, 0 dB at 100, mute at 0.
// Linear in STEPS was the first version, and on a 74-step, 0.75 dB
// amplifier that put 40% at -33 dB: barely audible from a laptop.
#define HDA_TAPER_DB 40

static uint8_t taper_gain(const struct hda_out *o, int pct) {
    uint32_t atten_qdb = (uint32_t)HDA_TAPER_DB * 4u * (uint32_t)(100 - pct) / 100u;
    uint32_t down = atten_qdb / o->vol_step_qdb;
    return down < o->vol_offset ? (uint8_t)(o->vol_offset - down) : 0;
}

// Applied to the speaker and headphone routes alike so a jack switch
// does not jump.
static void set_volume(struct hda_ctrl *h, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint8_t done = 0;
    for (int p = 0; p < 2; p++) {
        struct hda_out *o = p ? &h->codec.hp : &h->codec.spk;
        if (!(p ? h->codec.have_hp : h->codec.have_spk) || !o->vol_nid || o->vol_nid == done) continue;
        done = o->vol_nid;
        uint16_t pl = AMP_OUT | AMP_LEFT | AMP_RIGHT;
        if (pct == 0 && o->vol_mute) pl |= AMP_MUTE;
        else pl |= taper_gain(o, pct);
        hda_cmd(h, o->vol_nid, V4(VERB_SET_AMP, pl), 0);
    }
}

// --- interrupts ------------------------------------------------------

static void hda_irq_one(struct hda_ctrl *h) {
    uint32_t sts = mr32(h, HDA_INTSTS);
    if (!(sts & INTSTS_GIS)) return;
    h->irqs++;
    if (sts & (1u << h->sd_index)) {
        uint8_t s = mr8(h, h->sd + SD_STS);
        if (s & 0x08) h->fifoe++;
        if (s & 0x10) h->dese++;
        if ((s & 0x18) && h->fifoe + h->dese <= 3)
            klog_printf(KLOG_ERR "%s: stream error sts %#x (fifoe %u dese %u)\n", h->name, s, h->fifoe, h->dese);
        if ((s & SD_STS_BCIS) && h->diag) {
            // The diagnostic tone: count it down and stop the engine
            // from here, with no waiting -- pit_ticks() does not advance
            // inside a handler, so stream_stop()'s deadline cannot.
            if (--h->diag == 0) {
                mw32(h, h->sd + SD_CTL, mr32(h, h->sd + SD_CTL) & ~(uint32_t)(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFF);
                klog_printf("%s: hda_tone done -- %u irqs, fifoe %u, dese %u\n",
                            h->name, h->irqs, h->fifoe, h->dese);
            }
        } else if (s & SD_STS_BCIS) {
            // One BCIS per chunk, so COUNT them rather than trust LPIB,
            // which a PCH reports ~100 bytes short of the boundary at the
            // interrupt (measured) and QEMU reports exactly. LPIB only
            // pulls the count forward when interrupts were coalesced.
            uint32_t lpib = mr32(h, h->sd + SD_LPIB);
            uint32_t lc = (lpib / SND_CHUNK_BYTES) % SND_CHUNKS;
            uint32_t next = (h->chunk + 1) % SND_CHUNKS;
            uint32_t ahead = (lc + SND_CHUNKS - next) % SND_CHUNKS;
            if (ahead >= 2 && ahead < SND_CHUNKS / 2) next = (lc + SND_CHUNKS - 1) % SND_CHUNKS;
            h->chunk = next;
            sound_period_done(next * SND_CHUNK_BYTES);
        }
        mw8(h, h->sd + SD_STS, s & SD_STS_ACK);
    }
    if (sts & INTSTS_CIS) {
        rirb_drain(h);
        mw8(h, HDA_RIRBSTS, RIRBSTS_ACK);
        if (h->jack_pending && !h->in_jack) jack_update(h);
    }
}

static void hda_irq(uint64_t *regs) {
    (void)regs;
    for (int i = 0; i < g_nctrl; i++)
        if (g_hc[i].mmio) hda_irq_one(&g_hc[i]);
}

// --- the sound_device, one trampoline set per controller --------------

// ONE SET, because the op carries its device now. This was a
// trampoline PAIR generated per controller -- the only way to tell two
// cards apart when the ops took no argument.
static int  hda_dev_start(const struct sound_device *d)  { return stream_start(d->priv); }
static void hda_dev_stop(const struct sound_device *d)   { stream_stop(d->priv); }
static void hda_dev_volume(const struct sound_device *d, int pct) { set_volume(d->priv, pct); }

static const char *vendor_name(uint32_t vendor) {
    switch (vendor >> 16) {
    case 0x10EC: return "Realtek";
    case 0x8086: return "Intel";
    case 0x1AF4: return "QEMU";
    case 0x14F1: return "Conexant";
    case 0x1013: return "Cirrus Logic";
    case 0x111D: return "IDT";
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x11D4: return "Analog Devices";
    case 0x8384: return "SigmaTel";
    case 0x1106: return "VIA";
    default:     return "HD Audio";
    }
}

static void diag_tone(struct hda_ctrl *h, int16_t *ring);
static struct hda_ctrl *test_ctrl(void);

// The hardware half: every engine off and the controller back in
// reset. Shared with hda_remove(), which has nothing to apologise for
// and so does not want ctrl_teardown()'s line.
static void ctrl_quiesce(struct hda_ctrl *h) {
    mw32(h, HDA_INTCTL, 0);
    mw8(h, HDA_CORBCTL, 0);
    mw8(h, HDA_RIRBCTL, 0);
    mw32(h, HDA_GCTL, 0); // back into reset
    if (h->dma_phys) pmm_free_contiguous(h->dma_phys, 1);
    h->dma_phys = 0;
    h->mmio = 0;
}

static void ctrl_teardown(struct hda_ctrl *h, const char *why) {
    klog_printf("%s: %s -- not registered\n", h->name, why);
    ctrl_quiesce(h);
}

static void ctrl_init(struct hda_ctrl *h, const struct pci_device *d, int index) {
    h->pci = d;
    h->index = index;
    k_snprintf(h->name, sizeof h->name, "hda%d", index);

    uint64_t bar0 = pci_bar_mem_addr(d, 0);
    if (!bar0) {
        klog_printf("%s: no memory BAR0 -- not driving it\n", h->name);
        return;
    }
    uint64_t len = pci_bar_mem_size(d, 0);
    volatile void *win = paging_map_device(bar0, len ? len : 0x4000);
    if (!win) {
        klog_printf(KLOG_ERR "%s: BAR0 at 0x%llx could not be mapped\n", h->name, (unsigned long long)bar0);
        return;
    }
    h->mmio = (volatile uint8_t *)win;
    pci_command_update(d, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);
    pci_command_update(d, PCI_CMD_INTX_DISABLE, 0);

    // SNOOP. An Intel PCH controller has a NOSNOOP bit in its DEVC config
    // register; set (firmware's default on the test laptop), its DMA
    // reads RAM without snooping the CPU caches and plays whatever has
    // been evicted so far -- a clean sine came out as "clapping", an app's
    // refills as crackle. Linux's azx_init_pci() clears the same bit.
    if (d->vendor_id == 0x8086) {
        uint16_t devc = pci_config_read16(d, HDA_INTEL_DEVC);
        if (devc & HDA_INTEL_DEVC_NOSNOOP) {
            pci_config_write16(d, HDA_INTEL_DEVC, devc & (uint16_t)~HDA_INTEL_DEVC_NOSNOOP);
            klog_printf("%s: DEVC %#x -- NOSNOOP was set, cleared\n", h->name, devc);
        }
    }

    h->dma_phys = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
    if (!h->dma_phys) { h->mmio = 0; return; }
    h->corb = (volatile uint32_t *)(uintptr_t)h->dma_phys;
    h->rirb = (volatile uint64_t *)(uintptr_t)(h->dma_phys + 1024);
    h->bdl  = (struct hda_bdl_entry *)(uintptr_t)(h->dma_phys + 3072);
    k_memset((void *)(uintptr_t)h->dma_phys, 0, 4096);

    // Reset the link. STATESTS fills in as codecs come out of reset;
    // the spec's 25 frames is half a millisecond, so a few ticks is
    // plenty and a real codec has been seen to need most of them.
    mw32(h, HDA_INTCTL, 0);
    mw32(h, HDA_GCTL, mr32(h, HDA_GCTL) & ~(uint32_t)GCTL_CRST);
    wait_reg32(h, HDA_GCTL, GCTL_CRST, 0);
    mw32(h, HDA_GCTL, GCTL_CRST);
    if (wait_reg32(h, HDA_GCTL, GCTL_CRST, GCTL_CRST) != 0) {
        ctrl_teardown(h, "controller never left reset");
        return;
    }
    clocksource_delay_ms(30);   // was 3 ticks; real ms on a TSC
    mw16(h, HDA_WAKEEN, 0);
    uint16_t statests = mr16(h, HDA_STATESTS) & 0x7FFF;
    mw16(h, HDA_STATESTS, statests);
    mw32(h, HDA_GCTL, GCTL_CRST | GCTL_UNSOL);

    uint16_t gcap = mr16(h, HDA_GCAP);
    h->iss = (gcap >> 8) & 0xF;
    h->oss = (gcap >> 12) & 0xF;
    if (!h->oss) { ctrl_teardown(h, "no output streams"); return; }
    h->sd_index = h->iss; // the first OUTPUT descriptor follows the inputs
    h->sd = HDA_SD_BASE + (uint32_t)h->sd_index * 0x20;
    klog_printf("%s: %02x:%02x.%u gcap %#x (%d in, %d out) codecs %#x\n",
                h->name, d->bus, d->device, d->function, gcap, h->iss, h->oss, statests);

    corb_rirb_init(h);
    h->codec.cmd = hda_codec_cmd;
    h->codec.ctx = h;

    // The first codec with an audio function group and an analog
    // output. A display-audio controller's codec has only digital pins
    // and is left alone -- HDMI output is a roadmap item.
    int found = 0;
    for (uint8_t cad = 0; cad < 15 && !found; cad++) {
        if (!(statests & (1u << cad))) continue;
        h->cad = cad;
        uint32_t vendor = 0;
        if (hda_cmd(h, 0, V12(VERB_GET_PARAM, PARAM_VENDOR_ID), &vendor) != 0) {
            klog_printf("%s: codec at cad %u did not answer\n", h->name, cad);
            continue;
        }
        h->codec.vendor = vendor;
        if (hda_codec_enumerate(&h->codec) != 0) {
            klog_printf("%s: codec %04x:%04x at cad %u has no audio function group\n",
                        h->name, vendor >> 16, vendor & 0xFFFF, cad);
            continue;
        }
        if (want_dump()) dump_widgets(h);
        if (hda_codec_pick_outputs(&h->codec) != 0) {
            klog_printf("%s: codec %04x:%04x at cad %u has no analog output\n",
                        h->name, vendor >> 16, vendor & 0xFFFF, cad);
            continue;
        }
        found = 1;
    }
    if (!found) { ctrl_teardown(h, "no codec with an analog output"); return; }

    hda_codec_route_output(&h->codec, &h->codec.spk,
                           HDA_FMT_48K_S16_STEREO, HDA_STREAM_TAG);
    if (want_dump())
        klog_printf("%s: spk route %02x %02x %02x %02x %02x %02x (%d) vol nid %02x offset %u steps %u\n",
                    h->name, h->codec.spk.path[0], h->codec.spk.path[1], h->codec.spk.path[2], h->codec.spk.path[3],
                    h->codec.spk.path[4], h->codec.spk.path[5], h->codec.spk.len, h->codec.spk.vol_nid,
                    h->codec.spk.vol_offset, h->codec.spk.vol_steps);
    if (h->codec.have_hp) {
        hda_codec_route_output(&h->codec, &h->codec.hp,
                               HDA_FMT_48K_S16_STEREO, HDA_STREAM_TAG);
        if (want_dump())
            klog_printf("%s: hp route %02x %02x %02x %02x %02x %02x (%d) vol nid %02x\n",
                        h->name, h->codec.hp.path[0], h->codec.hp.path[1], h->codec.hp.path[2], h->codec.hp.path[3],
                        h->codec.hp.path[4], h->codec.hp.path[5], h->codec.hp.len, h->codec.hp.vol_nid);
        struct hda_widget *hpw = hda_codec_widget(&h->codec, h->codec.hp.pin);
        if (hpw && (hpw->pincap & PINCAP_PRESENCE) && (hpw->caps & WCAP_UNSOL)) {
            hda_cmd(h, h->codec.hp.pin, V12(VERB_SET_UNSOL, 0x80 | HDA_UNSOL_TAG), 0);
            uint32_t sense = 0;
            if (hda_cmd(h, h->codec.hp.pin, V12(VERB_GET_PIN_SENSE, 0), &sense) == 0)
                h->hp_plugged = (sense >> 31) & 1;
        } else {
            h->codec.have_hp = 0; // no jack sense: the speaker route alone
        }
        apply_jack_state(h);
    }

    uint64_t ring_phys = 0;
    void *ring = sound_ring_alloc(&ring_phys);
    if (!ring) { ctrl_teardown(h, "no contiguous frames for the ring"); return; }
    bdl_fill(h, ring_phys);

    // Interrupts: a vector if the machine has one, else the pin. The
    // handler exists before either is armed; registration is last.
    uint8_t line = pci_irq_line(d);
    h->msi_vector = pci_msi_request(d, hda_irq);
    if (!h->msi_vector) {
        if (line == 0xFF || line == 0 || line >= 16) {
            ctrl_teardown(h, "no MSI and no usable INTx line");
            return;
        }
        h->irq = line;
        irq_register_handler(line, hda_irq);
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);
        irq_unmask(line);
    }
    mw32(h, HDA_INTCTL, INTCTL_GIE | INTCTL_CIE); // jack events from here on

    hda_codec_pcm_support(&h->codec, h->codec.spk.dac,
                          &h->dev.rates, &h->dev.depths);
    k_snprintf(h->label, sizeof h->label, "%s HD Audio", vendor_name(h->codec.vendor));
    h->dev.name = h->name;
    h->dev.label = h->label;
    h->dev.driver = "hda";
    h->dev.priv = h;
    h->dev.start = hda_dev_start;
    h->dev.stop = hda_dev_stop;
    h->dev.set_volume = hda_dev_volume;
    if (!sound_register(&h->dev, ring, ring_phys)) return;
    h->registered = 1;
    klog_printf("%s: %02x:%02x.%u codec %04x:%04x spk pin %#x dac %#x%s%#x %s\n",
                h->name, d->bus, d->device, d->function,
                h->codec.vendor >> 16, h->codec.vendor & 0xFFFF, h->codec.spk.pin, h->codec.spk.dac,
                h->codec.have_hp ? " hp pin " : " hp ", h->codec.have_hp ? h->codec.hp.pin : 0,
                h->msi_vector ? "msi" : "intx");
}

// `config set kernel.hda_tone on`: three seconds of 375 Hz written into
// the ring ONCE by the kernel and played with nothing refilling or
// zeroing it. Splits a bad sound in half: this path is DMA, stream and
// codec only, so what crackles here is not the app or the core. It
// plays over whatever stream is open, which is fine for a diagnostic.
// NON-BLOCKING: the handler counts the completions and stops the
// engine -- a wait on pit_ticks() from a syscall hung the test laptop.
static void diag_tone(struct hda_ctrl *h, int16_t *ring) {
    for (uint32_t i = 0; i < SND_RING_BYTES / 4; i++) {
        int32_t v = (fx_sin((fx_t)((i % 128) * (FX_ONE / 128))) * 8000) >> 16;
        ring[2 * i] = ring[2 * i + 1] = (int16_t)v;
    }
    set_volume(h, 50);
    h->diag = 3 * SND_RATE * SND_FRAME_BYTES / SND_CHUNK_BYTES; // ~3 s of chunks
    if (stream_start(h) != 0) h->diag = 0;
}

void hda_diag_tone(void) {
    struct hda_ctrl *h = test_ctrl();
    if (!h) { klog_write("hda: no registered controller for the tone\n"); return; }
    uint64_t phys = 0;
    void *ring = sound_ring_alloc(&phys);
    if (ring) diag_tone(h, (int16_t *)ring);
}

static const struct pci_match hda_matches[] = { PCI_MATCH_CLASS(0x04, 0x03, PCI_ANY) };

// Once per controller: a laptop has the PCH's and the GPU's.
static void hda_probe(const struct pci_device *d) {
    // A FREE SLOT, not the next one: hda_remove() frees one in the
    // middle, and a bump counter would leak it and rename the
    // controller on every re-probe. g_nctrl stays the high-water mark
    // every walk over the array bounds itself with.
    int slot = -1;
    for (int i = 0; i < HDA_MAX_CTRL; i++) if (!g_hc[i].pci) { slot = i; break; }
    if (slot < 0) {
        klog_printf("hda: a %dth controller at %02x:%02x.%u -- not driven\n",
                    HDA_MAX_CTRL + 1, d->bus, d->device, d->function);
        return;
    }
    ctrl_init(&g_hc[slot], d, slot);
    if (slot >= g_nctrl) g_nctrl = slot + 1;
}

// What makes HDA claimable from ring 3 (docs/umdf-design.md stage 2).
// The first remove() on a BUILT-IN driver; the two NIC modules already
// had one, since a module cannot be unloaded without it. Order matters twice: the core
// is told before the registers go, because its stop() writes to them;
// and the interrupt is silenced before the controller is reset, or a
// line still unmasked fires into a handler whose controller is gone.
static void hda_remove(const struct pci_device *d) {
    struct hda_ctrl *h = 0;
    for (int i = 0; i < g_nctrl; i++) if (g_hc[i].pci == d) { h = &g_hc[i]; break; }
    if (!h) return;

    // `mmio` is 0 for a controller whose init failed: there is nothing
    // to quiesce, but the SLOT still has to come free or the device can
    // never be re-probed.
    if (h->mmio) {
        if (h->registered) sound_unregister(&h->dev);
        h->registered = 0;

        if (h->msi_vector) pci_msi_release(d, h->msi_vector);
        else if (h->irq) { irq_mask(h->irq); irq_unregister_handler(h->irq, hda_irq); }
        h->msi_vector = 0;
        h->irq = 0;

        ctrl_quiesce(h);
    }
    h->pci = 0;
    klog_printf("%s: released %02x:%02x.%u\n", h->name, d->bus, d->device, d->function);
}

// --- KTESTs -- skip without the device, like ac97's -------------------

static struct hda_ctrl *test_ctrl(void) {
    for (int i = 0; i < g_nctrl; i++) if (g_hc[i].registered) return &g_hc[i];
    return 0;
}

KTEST("hda", "the controller is out of reset and a codec answered") {
    struct hda_ctrl *h = test_ctrl();
    if (!h) { KTEST_SKIP("no HDA on this machine"); return; }
    KTEST_ASSERT(mr32(h, HDA_GCTL) & GCTL_CRST);
    KTEST_ASSERT(h->codec.vendor != 0 && h->codec.vendor != 0xFFFFFFFFu);
    KTEST_ASSERT(sound_present());
}

KTEST("hda", "the speaker route ends at a DAC bound to stream 1") {
    struct hda_ctrl *h = test_ctrl();
    if (!h) { KTEST_SKIP("no HDA on this machine"); return; }
    uint32_t conv = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->codec.spk.dac, V12(VERB_GET_CONV, 0), &conv), 0);
    KTEST_ASSERT_EQ((conv >> 4) & 0xF, HDA_STREAM_TAG);
    uint32_t ctl = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->codec.spk.pin, V12(VERB_GET_PIN_CTL, 0), &ctl), 0);
    KTEST_ASSERT(ctl & PINCTL_OUT_EN);
}

KTEST("hda", "start runs the stream and stop halts it") {
    struct hda_ctrl *h = test_ctrl();
    if (!h) { KTEST_SKIP("no HDA on this machine"); return; }
    KTEST_ASSERT_EQ(stream_start(h), 0);
    KTEST_ASSERT(mr32(h, h->sd + SD_CTL) & SD_CTL_RUN);
    stream_stop(h);
    KTEST_ASSERT_EQ(mr32(h, h->sd + SD_CTL) & SD_CTL_RUN, 0);
}

KTEST("hda", "a volume change lands in the route's amplifier") {
    struct hda_ctrl *h = test_ctrl();
    if (!h || !h->codec.spk.vol_nid || !h->codec.spk.vol_offset) { KTEST_SKIP("no stepped amplifier"); return; }
    set_volume(h, 50);
    uint32_t amp = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->codec.spk.vol_nid, V4(VERB_GET_AMP, AMP_OUT | AMP_LEFT), &amp), 0);
    KTEST_ASSERT_EQ(amp & 0x7F, taper_gain(&h->codec.spk, 50));
    KTEST_ASSERT((amp & 0x7F) < h->codec.spk.vol_offset); // 50% really is quieter
    set_volume(h, 100);
    KTEST_ASSERT_EQ(hda_cmd(h, h->codec.spk.vol_nid, V4(VERB_GET_AMP, AMP_OUT | AMP_LEFT), &amp), 0);
    KTEST_ASSERT_EQ(amp & 0x7F, h->codec.spk.vol_offset);
}
PCI_DRIVER_REMOVABLE("hda", hda_matches, hda_probe, hda_remove);
