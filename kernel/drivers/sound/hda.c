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
#include <stdint.h>

DRIVER_DECLARE("hda", "sound", "Intel High Definition Audio");

// --- controller registers ---------------------------------------------
#define HDA_GCAP      0x00 // 16: OSS 15:12, ISS 11:8, BSS 7:4, 64OK bit 0
#define HDA_GCTL      0x08 // 32
#define HDA_WAKEEN    0x0C // 16
#define HDA_STATESTS  0x0E // 16, RW1C: a bit per codec that answered reset
#define HDA_INTCTL    0x20 // 32
#define HDA_INTSTS    0x24 // 32
#define HDA_CORBLBASE 0x40
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48 // 16
#define HDA_CORBRP    0x4A // 16, bit 15 = reset
#define HDA_CORBCTL   0x4C // 8
#define HDA_CORBSIZE  0x4E // 8: 1:0 selected, 7:4 supported
#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58 // 16, bit 15 = reset (write-only)
#define HDA_RINTCNT   0x5A // 16
#define HDA_RIRBCTL   0x5C // 8
#define HDA_RIRBSTS   0x5D // 8, RW1C
#define HDA_RIRBSIZE  0x5E // 8
#define HDA_SD_BASE   0x80 // stream descriptors, 0x20 apart, inputs first

#define GCTL_CRST   0x001
#define GCTL_UNSOL  0x100
#define INTCTL_GIE  0x80000000u
#define INTCTL_CIE  0x40000000u
#define INTSTS_GIS  0x80000000u
#define INTSTS_CIS  0x40000000u
#define CORBCTL_RUN 0x02
#define RIRBCTL_DMAEN 0x02
#define RIRBCTL_RINTCTL 0x01
#define RIRBSTS_ACK 0x05 // RINTFL | OIS

// Stream descriptor, relative to its base.
#define SD_CTL   0x00 // 24-bit; STS is the fourth byte of the same dword
#define SD_STS   0x03 // 8, RW1C
#define SD_LPIB  0x04 // 32: bytes played of the current lap
#define SD_CBL   0x08 // 32: cyclic buffer length
#define SD_LVI   0x0C // 16
#define SD_FMT   0x12 // 16
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_CTL_SRST 0x01
#define SD_CTL_RUN  0x02
#define SD_CTL_IOCE 0x04
#define SD_CTL_FEIE 0x08
#define SD_CTL_DEIE 0x10
#define SD_CTL_STREAM_SHIFT 20
#define SD_STS_ACK  0x1C // BCIS | FIFOE | DESE
#define SD_STS_BCIS 0x04

// 48 kHz base, 16-bit, 2 channels -- SND_RATE/SND_CHANNELS as the codec
// spells them.
#define HDA_FMT_48K_S16_STEREO 0x0011
#define HDA_STREAM_TAG 1

// --- codec verbs -------------------------------------------------------
// A 20-bit verb+payload: 12-bit verb with an 8-bit payload, or 4-bit
// verb with 16 bits.
#define V12(verb, pl) (((uint32_t)(verb) << 8) | ((pl) & 0xFF))
#define V4(verb, pl)  (((uint32_t)(verb) << 16) | ((pl) & 0xFFFF))

#define VERB_GET_PARAM        0xF00
#define VERB_GET_CONN_LIST    0xF02
#define VERB_SET_CONN_SEL     0x701
#define VERB_GET_CONV         0xF06
#define VERB_SET_CONV         0x706
#define VERB_SET_POWER        0x705
#define VERB_SET_PIN_CTL      0x707
#define VERB_GET_PIN_CTL      0xF07
#define VERB_SET_UNSOL        0x708
#define VERB_GET_PIN_SENSE    0xF09
#define VERB_SET_EAPD         0x70C
#define VERB_GET_CONFIG_DEF   0xF1C
#define VERB_SET_AMP          0x3 // 4-bit
#define VERB_GET_AMP          0xB // 4-bit
#define VERB_SET_FORMAT       0x2 // 4-bit

#define PARAM_VENDOR_ID     0x00
#define PARAM_NODE_COUNT    0x04
#define PARAM_FUNC_TYPE     0x05
#define PARAM_WIDGET_CAPS   0x09
#define PARAM_PIN_CAPS      0x0C
#define PARAM_AMP_IN_CAPS   0x0D
#define PARAM_CONN_LIST_LEN 0x0E
#define PARAM_AMP_OUT_CAPS  0x12

#define FUNC_AUDIO 0x01

// Widget capabilities (PARAM_WIDGET_CAPS).
#define WCAP_TYPE(c)     (((c) >> 20) & 0xF)
#define WCAP_IN_AMP      0x002
#define WCAP_OUT_AMP     0x004
#define WCAP_AMP_OVRD    0x008
#define WCAP_UNSOL       0x080
#define WCAP_CONN_LIST   0x100
#define WCAP_DIGITAL     0x200
#define WCAP_POWER       0x400
#define WT_AUD_OUT  0x0
#define WT_AUD_IN   0x1
#define WT_MIXER    0x2
#define WT_SELECTOR 0x3
#define WT_PIN      0x4

// Pin capabilities and the default configuration.
#define PINCAP_PRESENCE 0x00004
#define PINCAP_HP_DRIVE 0x00008
#define PINCAP_OUTPUT   0x00010
#define PINCAP_EAPD     0x10000
#define DEFCFG_CONN(c)  (((c) >> 30) & 0x3) // 0 jack, 1 none, 2 fixed, 3 both
#define DEFCFG_DEV(c)   (((c) >> 20) & 0xF)
#define DEV_LINE_OUT 0x0
#define DEV_SPEAKER  0x1
#define DEV_HP_OUT   0x2

#define PINCTL_HP_EN  0x80
#define PINCTL_OUT_EN 0x40

// SET_AMP payload bits.
#define AMP_OUT   0x8000
#define AMP_IN    0x4000
#define AMP_LEFT  0x2000
#define AMP_RIGHT 0x1000
#define AMP_MUTE  0x0080
#define AMP_IDX(i) (((i) & 0xF) << 8)

// --- state ---------------------------------------------------------------
#define HDA_MAX_CTRL   2   // a PCH controller and a display-audio one
#define HDA_MAX_WIDGET 96
#define HDA_PATH_MAX   6
#define HDA_CONN_MAX   32
#define HDA_UNSOL_TAG  1

struct hda_bdl_entry {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc; // bit 0
};

struct hda_widget {
    uint8_t  nid;
    uint8_t  type;
    uint32_t caps;
    uint32_t pincap; // pins only
    uint32_t defcfg; // pins only
};

// One analog output: a pin, and the route from it back to a DAC.
struct hda_out {
    uint8_t pin, dac;
    uint8_t path[HDA_PATH_MAX]; // pin first, DAC last
    int     len;
    // The volume knob on this route: the first widget from the DAC end
    // with a stepped output amplifier. `offset` is the 0 dB step.
    uint8_t vol_nid;
    uint8_t vol_steps, vol_offset;
    uint8_t vol_step_qdb; // dB per step, in quarter-dB (caps field + 1)
    int     vol_mute;     // the amp can mute
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

    uint8_t  cad, afg;
    uint32_t vendor;
    struct hda_widget widgets[HDA_MAX_WIDGET];
    int nwidgets;

    struct hda_out spk, hp;
    int have_spk, have_hp;
    int hp_plugged;
    int jack_pending, in_jack;

    uint8_t msi_vector, irq;
    uint32_t irqs, jack_events;

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

// Spin until (reg & mask) == want, or the tick deadline. pit_ticks()
// only advances with interrupts on, which every caller here has.
static int wait_reg32(struct hda_ctrl *h, uint32_t off, uint32_t mask, uint32_t want, int ticks) {
    uint64_t deadline = pit_ticks() + (uint64_t)ticks;
    while ((mr32(h, off) & mask) != want) {
        if (pit_ticks() >= deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static int wait_reg8(struct hda_ctrl *h, uint32_t off, uint8_t mask, uint8_t want, int ticks) {
    uint64_t deadline = pit_ticks() + (uint64_t)ticks;
    while ((mr8(h, off) & mask) != want) {
        if (pit_ticks() >= deadline) return -1;
        cpu_relax();
    }
    return 0;
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

static uint32_t param(struct hda_ctrl *h, uint8_t nid, uint8_t p) {
    uint32_t v = 0;
    hda_cmd(h, nid, V12(VERB_GET_PARAM, p), &v);
    return v;
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
    wait_reg8(h, HDA_CORBCTL, CORBCTL_RUN, 0, 5);

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

static struct hda_widget *widget(struct hda_ctrl *h, uint8_t nid) {
    for (int i = 0; i < h->nwidgets; i++)
        if (h->widgets[i].nid == nid) return &h->widgets[i];
    return 0;
}

// The connection list of `nid`, expanded (a range entry stands for
// every node between the previous entry and itself).
static int conn_list(struct hda_ctrl *h, uint8_t nid, uint8_t *out, int cap) {
    struct hda_widget *w = widget(h, nid);
    if (!w || !(w->caps & WCAP_CONN_LIST)) return 0;
    uint32_t len = param(h, nid, PARAM_CONN_LIST_LEN);
    int n = (int)(len & 0x7F);
    int longf = (len & 0x80) != 0;
    int per = longf ? 2 : 4;
    int count = 0;
    for (int i = 0; i < n && count < cap; i += per) {
        uint32_t r = 0;
        if (hda_cmd(h, nid, V12(VERB_GET_CONN_LIST, i), &r) != 0) break;
        for (int j = 0; j < per && i + j < n && count < cap; j++) {
            uint32_t entry = longf ? (r >> (16 * j)) & 0xFFFF : (r >> (8 * j)) & 0xFF;
            uint32_t range = longf ? 0x8000 : 0x80;
            uint32_t mask  = longf ? 0x7FFF : 0x7F;
            uint8_t  nid2  = (uint8_t)(entry & mask);
            if ((entry & range) && count > 0) {
                for (uint8_t k = (uint8_t)(out[count - 1] + 1); k <= nid2 && count < cap; k++)
                    out[count++] = k;
            } else {
                out[count++] = nid2;
            }
        }
    }
    return count;
}

// `hdadump` on the GRUB line: every widget, its capabilities and its
// connection list, one klog line each -- what a silent route on a new
// machine is diagnosed from (docs/boot-flags.md).
static int want_dump(void) {
    const char *c = multiboot_cmdline();
    return c && k_strstr(c, "hdadump");
}

static uint32_t amp_caps(struct hda_ctrl *h, struct hda_widget *w, int out);

static void dump_widgets(struct hda_ctrl *h) {
    for (int i = 0; i < h->nwidgets; i++) {
        struct hda_widget *w = &h->widgets[i];
        uint8_t conns[HDA_CONN_MAX];
        int n = conn_list(h, w->nid, conns, HDA_CONN_MAX);
        char cl[3 * 12 + 4] = "";
        int used = 0;
        for (int k = 0; k < n && k < 12; k++)
            used += k_snprintf(cl + used, sizeof cl - (uint32_t)used, " %02x", conns[k]);
        uint32_t oc = (w->caps & WCAP_OUT_AMP) ? amp_caps(h, w, 1) : 0;
        uint32_t ic = (w->caps & WCAP_IN_AMP) ? amp_caps(h, w, 0) : 0;
        klog_printf("%s: nid %02x type %u caps %08x pincap %08x defcfg %08x oamp %08x iamp %08x conn%s%s\n",
                    h->name, w->nid, w->type, w->caps, w->pincap, w->defcfg, oc, ic,
                    cl, n > 12 ? " ..." : "");
    }
}

static int enumerate(struct hda_ctrl *h) {
    uint32_t nodes = param(h, 0, PARAM_NODE_COUNT);
    uint8_t fg_start = (uint8_t)(nodes >> 16), fg_count = (uint8_t)nodes;
    h->afg = 0;
    for (uint8_t i = 0; i < fg_count; i++) {
        uint8_t nid = (uint8_t)(fg_start + i);
        if ((param(h, nid, PARAM_FUNC_TYPE) & 0xFF) == FUNC_AUDIO) { h->afg = nid; break; }
    }
    if (!h->afg) return -1;
    hda_cmd(h, h->afg, V12(VERB_SET_POWER, 0), 0); // D0

    nodes = param(h, h->afg, PARAM_NODE_COUNT);
    uint8_t start = (uint8_t)(nodes >> 16), count = (uint8_t)nodes;
    h->nwidgets = 0;
    for (uint8_t i = 0; i < count && h->nwidgets < HDA_MAX_WIDGET; i++) {
        struct hda_widget *w = &h->widgets[h->nwidgets];
        w->nid = (uint8_t)(start + i);
        w->caps = param(h, w->nid, PARAM_WIDGET_CAPS);
        w->type = (uint8_t)WCAP_TYPE(w->caps);
        w->pincap = w->defcfg = 0;
        if (w->type == WT_PIN) {
            w->pincap = param(h, w->nid, PARAM_PIN_CAPS);
            hda_cmd(h, w->nid, V12(VERB_GET_CONFIG_DEF, 0), &w->defcfg);
        }
        h->nwidgets++;
    }
    if (h->nwidgets && want_dump()) dump_widgets(h);
    return h->nwidgets ? 0 : -1;
}

// Depth-first from a pin toward an analog DAC, through mixers and
// selectors only. Fills `path` pin-first; returns its length or 0.
static int find_path(struct hda_ctrl *h, uint8_t nid, uint8_t *path, int depth, uint32_t *visited) {
    if (depth >= HDA_PATH_MAX) return 0;
    struct hda_widget *w = widget(h, nid);
    if (!w || (w->caps & WCAP_DIGITAL)) return 0;
    if (visited[nid >> 5] & (1u << (nid & 31))) return 0;
    visited[nid >> 5] |= 1u << (nid & 31);
    path[depth] = nid;
    if (w->type == WT_AUD_OUT) return depth + 1;
    if (w->type != WT_PIN && w->type != WT_MIXER && w->type != WT_SELECTOR) return 0;
    uint8_t conns[HDA_CONN_MAX];
    int n = conn_list(h, nid, conns, HDA_CONN_MAX);
    for (int i = 0; i < n; i++) {
        int len = find_path(h, conns[i], path, depth + 1, visited);
        if (len) return len;
    }
    return 0;
}

static uint32_t amp_caps(struct hda_ctrl *h, struct hda_widget *w, int out) {
    uint8_t p = out ? PARAM_AMP_OUT_CAPS : PARAM_AMP_IN_CAPS;
    return param(h, (w->caps & WCAP_AMP_OVRD) ? w->nid : h->afg, p);
}

// Route one output: power, the DAC's stream and format, every
// amplifier on the way unmuted at 0 dB, the pin enabled for output.
static void route_output(struct hda_ctrl *h, struct hda_out *o) {
    o->vol_nid = 0;
    for (int i = 0; i < o->len; i++) {
        struct hda_widget *w = widget(h, o->path[i]);
        if (!w) continue;
        if (w->caps & WCAP_POWER) hda_cmd(h, w->nid, V12(VERB_SET_POWER, 0), 0);
    }
    for (int i = o->len - 1; i >= 0; i--) {
        struct hda_widget *w = widget(h, o->path[i]);
        if (!w) continue;
        uint8_t next = (i + 1 < o->len) ? o->path[i + 1] : 0;

        if (w->type == WT_AUD_OUT) {
            hda_cmd(h, w->nid, V4(VERB_SET_FORMAT, HDA_FMT_48K_S16_STEREO), 0);
            hda_cmd(h, w->nid, V12(VERB_SET_CONV, HDA_STREAM_TAG << 4), 0);
        }

        // The input side: select `next`, unmute it, and on a mixer mute
        // the rest so a microphone loop does not ride along.
        if (next && (w->caps & WCAP_CONN_LIST)) {
            uint8_t conns[HDA_CONN_MAX];
            int n = conn_list(h, w->nid, conns, HDA_CONN_MAX);
            int idx = 0;
            for (int k = 0; k < n; k++) if (conns[k] == next) { idx = k; break; }
            if (n > 1 && w->type != WT_MIXER)
                hda_cmd(h, w->nid, V12(VERB_SET_CONN_SEL, idx), 0);
            if (w->caps & WCAP_IN_AMP) {
                uint32_t ic = amp_caps(h, w, 0);
                uint16_t gain = (uint16_t)(ic & 0x7F); // the 0 dB step
                for (int k = 0; k < n; k++) {
                    if (w->type != WT_MIXER && k != idx) continue;
                    uint16_t pl = AMP_IN | AMP_LEFT | AMP_RIGHT | AMP_IDX(k) | gain;
                    if (k != idx) pl |= AMP_MUTE;
                    hda_cmd(h, w->nid, V4(VERB_SET_AMP, pl), 0);
                }
            }
        }

        if (w->caps & WCAP_OUT_AMP) {
            // Amp caps: 6:0 the 0 dB step (offset), 14:8 the step count,
            // 22:16 the step size, 31 mute-capable.
            uint32_t oc = amp_caps(h, w, 1);
            uint8_t offset = (uint8_t)(oc & 0x7F), steps = (uint8_t)((oc >> 8) & 0x7F);
            hda_cmd(h, w->nid, V4(VERB_SET_AMP, AMP_OUT | AMP_LEFT | AMP_RIGHT | offset), 0);
            if (!o->vol_nid && (steps || (oc & 0x80000000u))) {
                o->vol_nid = w->nid;
                o->vol_steps = steps;
                o->vol_offset = offset;
                o->vol_step_qdb = (uint8_t)(((oc >> 16) & 0x7F) + 1);
                o->vol_mute = (oc & 0x80000000u) != 0;
            }
        }

        if (w->type == WT_PIN) {
            uint8_t ctl = PINCTL_OUT_EN;
            if (w->pincap & PINCAP_HP_DRIVE) ctl |= PINCTL_HP_EN;
            hda_cmd(h, w->nid, V12(VERB_SET_PIN_CTL, ctl), 0);
            // EAPD: the external amplifier most laptops put between the
            // codec and the speaker. Set wherever the pin says it can be.
            if (w->pincap & PINCAP_EAPD) hda_cmd(h, w->nid, V12(VERB_SET_EAPD, 0x02), 0);
        }
    }
}

static int out_rank(struct hda_widget *w) {
    if (w->type != WT_PIN || !(w->pincap & PINCAP_OUTPUT) || (w->caps & WCAP_DIGITAL)) return 0;
    if (DEFCFG_CONN(w->defcfg) == 1) return 0; // nothing wired
    switch (DEFCFG_DEV(w->defcfg)) {
    case DEV_SPEAKER:  return DEFCFG_CONN(w->defcfg) == 0 ? 3 : 4;
    case DEV_LINE_OUT: return 2;
    case DEV_HP_OUT:   return 1;
    default:           return 0;
    }
}

// Choose the speaker (or line-out) pin and, separately, a headphone
// jack; route each back to a DAC.
static int pick_outputs(struct hda_ctrl *h) {
    h->have_spk = h->have_hp = 0;
    int best = 0;
    struct hda_widget *spk = 0, *hp = 0;
    for (int i = 0; i < h->nwidgets; i++) {
        struct hda_widget *w = &h->widgets[i];
        int r = out_rank(w);
        if (r > best) { best = r; spk = w; }
        if (r == 1 && !hp) hp = w;
    }
    if (!spk) return -1;
    if (hp == spk) hp = 0; // headphones are the only output: always on

    uint32_t visited[8] = {0};
    h->spk.len = find_path(h, spk->nid, h->spk.path, 0, visited);
    if (!h->spk.len) return -1;
    h->spk.pin = spk->nid;
    h->spk.dac = h->spk.path[h->spk.len - 1];
    h->have_spk = 1;

    if (hp) {
        uint32_t v2[8] = {0};
        h->hp.len = find_path(h, hp->nid, h->hp.path, 0, v2);
        if (h->hp.len) {
            h->hp.pin = hp->nid;
            h->hp.dac = h->hp.path[h->hp.len - 1];
            h->have_hp = 1;
        }
    }
    return 0;
}

// Headphones in: the jack drives and the speaker pin is switched off,
// as every laptop does it. Out: the reverse.
static void apply_jack_state(struct hda_ctrl *h) {
    if (!h->have_hp) return;
    struct hda_widget *hpw = widget(h, h->hp.pin);
    uint8_t hp_ctl = PINCTL_OUT_EN | ((hpw && (hpw->pincap & PINCAP_HP_DRIVE)) ? PINCTL_HP_EN : 0);
    hda_cmd(h, h->hp.pin,  V12(VERB_SET_PIN_CTL, h->hp_plugged ? hp_ctl : 0), 0);
    hda_cmd(h, h->spk.pin, V12(VERB_SET_PIN_CTL, h->hp_plugged ? 0 : PINCTL_OUT_EN), 0);
}

static void jack_update(struct hda_ctrl *h) {
    if (!h->have_hp) { h->jack_pending = 0; return; }
    h->in_jack = 1;
    h->jack_pending = 0;
    uint32_t sense = 0;
    if (hda_cmd(h, h->hp.pin, V12(VERB_GET_PIN_SENSE, 0), &sense) == 0) {
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
    wait_reg32(h, sd + SD_CTL, SD_CTL_RUN, 0, 5);
    mw32(h, sd + SD_CTL, SD_CTL_SRST);
    wait_reg32(h, sd + SD_CTL, SD_CTL_SRST, SD_CTL_SRST, 5);
    mw32(h, sd + SD_CTL, 0);
    if (wait_reg32(h, sd + SD_CTL, SD_CTL_SRST, 0, 5) != 0) return -1;

    mw32(h, sd + SD_BDPL, (uint32_t)(h->dma_phys + 3072));
    mw32(h, sd + SD_BDPU, (uint32_t)((h->dma_phys + 3072) >> 32));
    mw32(h, sd + SD_CBL, SND_RING_BYTES);
    mw16(h, sd + SD_LVI, SND_CHUNKS - 1);
    mw16(h, sd + SD_FMT, HDA_FMT_48K_S16_STEREO);
    mw8(h, sd + SD_STS, SD_STS_ACK);

    // The codec side again: a stream reset leaves the converter as it
    // was, but a codec that was power-cycled would not be.
    for (int p = 0; p < 2; p++) {
        struct hda_out *o = p ? &h->hp : &h->spk;
        if (!(p ? h->have_hp : h->have_spk)) continue;
        hda_cmd(h, o->dac, V4(VERB_SET_FORMAT, HDA_FMT_48K_S16_STEREO), 0);
        hda_cmd(h, o->dac, V12(VERB_SET_CONV, HDA_STREAM_TAG << 4), 0);
    }

    mw32(h, HDA_INTCTL, mr32(h, HDA_INTCTL) | INTCTL_GIE | (1u << h->sd_index));
    mw32(h, sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SD_CTL_STREAM_SHIFT) |
                         SD_CTL_RUN | SD_CTL_IOCE | SD_CTL_FEIE | SD_CTL_DEIE);
    return 0;
}

static void stream_stop(struct hda_ctrl *h) {
    uint32_t sd = h->sd;
    mw32(h, sd + SD_CTL, mr32(h, sd + SD_CTL) & ~(uint32_t)(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFF);
    wait_reg32(h, sd + SD_CTL, SD_CTL_RUN, 0, 5);
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
        struct hda_out *o = p ? &h->hp : &h->spk;
        if (!(p ? h->have_hp : h->have_spk) || !o->vol_nid || o->vol_nid == done) continue;
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
        if (s & SD_STS_BCIS) {
            // LPIB is the byte position within the lap; it lands at a
            // chunk boundary give or take the FIFO, so round to nearest.
            uint32_t lpib = mr32(h, h->sd + SD_LPIB);
            uint32_t chunk = ((lpib + SND_CHUNK_BYTES / 2) / SND_CHUNK_BYTES) % SND_CHUNKS;
            sound_period_done(chunk * SND_CHUNK_BYTES);
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

#define HDA_DEV_OPS(n)                                                            \
    static int  hda##n##_start(void)      { return stream_start(&g_hc[n]); }      \
    static void hda##n##_stop(void)       { stream_stop(&g_hc[n]); }              \
    static void hda##n##_volume(int pct)  { set_volume(&g_hc[n], pct); }
HDA_DEV_OPS(0)
HDA_DEV_OPS(1)

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

static void ctrl_teardown(struct hda_ctrl *h, const char *why) {
    klog_printf("%s: %s -- not registered\n", h->name, why);
    mw32(h, HDA_INTCTL, 0);
    mw8(h, HDA_CORBCTL, 0);
    mw8(h, HDA_RIRBCTL, 0);
    mw32(h, HDA_GCTL, 0); // back into reset
    if (h->dma_phys) pmm_free_contiguous(h->dma_phys, 1);
    h->dma_phys = 0;
    h->mmio = 0;
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
        klog_printf("%s: BAR0 at 0x%llx could not be mapped\n", h->name, (unsigned long long)bar0);
        return;
    }
    h->mmio = (volatile uint8_t *)win;
    pci_command_update(d, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER, 0);
    pci_command_update(d, PCI_CMD_INTX_DISABLE, 0);

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
    wait_reg32(h, HDA_GCTL, GCTL_CRST, 0, 10);
    mw32(h, HDA_GCTL, GCTL_CRST);
    if (wait_reg32(h, HDA_GCTL, GCTL_CRST, GCTL_CRST, 10) != 0) {
        ctrl_teardown(h, "controller never left reset");
        return;
    }
    uint64_t until = pit_ticks() + 3;
    while (pit_ticks() < until) cpu_relax();
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
        h->vendor = vendor;
        if (enumerate(h) != 0) {
            klog_printf("%s: codec %04x:%04x at cad %u has no audio function group\n",
                        h->name, vendor >> 16, vendor & 0xFFFF, cad);
            continue;
        }
        if (pick_outputs(h) != 0) {
            klog_printf("%s: codec %04x:%04x at cad %u has no analog output\n",
                        h->name, vendor >> 16, vendor & 0xFFFF, cad);
            continue;
        }
        found = 1;
    }
    if (!found) { ctrl_teardown(h, "no codec with an analog output"); return; }

    route_output(h, &h->spk);
    if (want_dump())
        klog_printf("%s: spk route %02x %02x %02x %02x %02x %02x (%d) vol nid %02x offset %u steps %u\n",
                    h->name, h->spk.path[0], h->spk.path[1], h->spk.path[2], h->spk.path[3],
                    h->spk.path[4], h->spk.path[5], h->spk.len, h->spk.vol_nid,
                    h->spk.vol_offset, h->spk.vol_steps);
    if (h->have_hp) {
        route_output(h, &h->hp);
        if (want_dump())
            klog_printf("%s: hp route %02x %02x %02x %02x %02x %02x (%d) vol nid %02x\n",
                        h->name, h->hp.path[0], h->hp.path[1], h->hp.path[2], h->hp.path[3],
                        h->hp.path[4], h->hp.path[5], h->hp.len, h->hp.vol_nid);
        struct hda_widget *hpw = widget(h, h->hp.pin);
        if (hpw && (hpw->pincap & PINCAP_PRESENCE) && (hpw->caps & WCAP_UNSOL)) {
            hda_cmd(h, h->hp.pin, V12(VERB_SET_UNSOL, 0x80 | HDA_UNSOL_TAG), 0);
            uint32_t sense = 0;
            if (hda_cmd(h, h->hp.pin, V12(VERB_GET_PIN_SENSE, 0), &sense) == 0)
                h->hp_plugged = (sense >> 31) & 1;
        } else {
            h->have_hp = 0; // no jack sense: the speaker route alone
        }
        apply_jack_state(h);
    }

    uint64_t ring_phys = 0;
    void *ring = sound_ring_alloc(&ring_phys);
    if (!ring) { ctrl_teardown(h, "no contiguous frames for the ring"); return; }
    bdl_fill(h, ring_phys);

    // Interrupts: a vector if the machine has one, else the pin. The
    // handler exists before either is armed; registration is last.
    uint8_t line = d->interrupt_line;
    h->msi_vector = lapic_alloc_vector(hda_irq);
    if (h->msi_vector) {
        if (!pci_msix_enable(d, h->msi_vector) && !pci_msi_enable(d, h->msi_vector))
            h->msi_vector = 0;
    }
    if (!h->msi_vector) {
        if (line == 0xFF || line == 0 || line >= 16) {
            ctrl_teardown(h, "no MSI and no usable INTx line");
            return;
        }
        h->irq = line;
        irq_register_handler(line, hda_irq);
        pci_command_update(d, 0, PCI_CMD_INTX_DISABLE);
        pic_clear_mask(line);
    }
    mw32(h, HDA_INTCTL, INTCTL_GIE | INTCTL_CIE); // jack events from here on

    k_snprintf(h->label, sizeof h->label, "%s HD Audio", vendor_name(h->vendor));
    h->dev.name = h->name;
    h->dev.label = h->label;
    h->dev.driver = "hda";
    h->dev.start = index ? hda1_start : hda0_start;
    h->dev.stop = index ? hda1_stop : hda0_stop;
    h->dev.set_volume = index ? hda1_volume : hda0_volume;
    if (!sound_register(&h->dev, ring, ring_phys)) return;
    h->registered = 1;
    klog_printf("%s: %02x:%02x.%u codec %04x:%04x spk pin %#x dac %#x%s%#x %s\n",
                h->name, d->bus, d->device, d->function,
                h->vendor >> 16, h->vendor & 0xFFFF, h->spk.pin, h->spk.dac,
                h->have_hp ? " hp pin " : " hp ", h->have_hp ? h->hp.pin : 0,
                h->msi_vector ? "msi" : "intx");
}

void hda_init(void) {
    for (int i = 0; i < pci_device_count() && g_nctrl < HDA_MAX_CTRL; i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d->class_code != 0x04 || d->subclass != 0x03) continue;
        ctrl_init(&g_hc[g_nctrl], d, g_nctrl);
        g_nctrl++;
    }
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
    KTEST_ASSERT(h->vendor != 0 && h->vendor != 0xFFFFFFFFu);
    KTEST_ASSERT(sound_present());
}

KTEST("hda", "the speaker route ends at a DAC bound to stream 1") {
    struct hda_ctrl *h = test_ctrl();
    if (!h) { KTEST_SKIP("no HDA on this machine"); return; }
    uint32_t conv = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->spk.dac, V12(VERB_GET_CONV, 0), &conv), 0);
    KTEST_ASSERT_EQ((conv >> 4) & 0xF, HDA_STREAM_TAG);
    uint32_t ctl = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->spk.pin, V12(VERB_GET_PIN_CTL, 0), &ctl), 0);
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
    if (!h || !h->spk.vol_nid || !h->spk.vol_offset) { KTEST_SKIP("no stepped amplifier"); return; }
    set_volume(h, 50);
    uint32_t amp = 0;
    KTEST_ASSERT_EQ(hda_cmd(h, h->spk.vol_nid, V4(VERB_GET_AMP, AMP_OUT | AMP_LEFT), &amp), 0);
    KTEST_ASSERT_EQ(amp & 0x7F, taper_gain(&h->spk, 50));
    KTEST_ASSERT((amp & 0x7F) < h->spk.vol_offset); // 50% really is quieter
    set_volume(h, 100);
    KTEST_ASSERT_EQ(hda_cmd(h, h->spk.vol_nid, V4(VERB_GET_AMP, AMP_OUT | AMP_LEFT), &amp), 0);
    KTEST_ASSERT_EQ(amp & 0x7F, h->spk.vol_offset);
}
