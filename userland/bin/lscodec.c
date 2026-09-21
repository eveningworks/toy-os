// lscodec -- the HD Audio codec graph, read by a process.
//
// STAGE 3 of docs/umdf-design.md, and the first ring-3 driver in this
// tree. It takes an HD Audio controller off the kernel, maps its
// registers, drives its command ring itself, walks the codec graph, and
// hands the controller back.
//
// THE PARSER IS NOT IN THIS FILE. kernel/lib/hda_codec.c is compiled
// twice -- once into the kernel for hda.c, once into this binary --
// so there is one implementation of the walk, not two that drift. What
// is here is the TRANSPORT: bringing a controller out of reset, the
// CORB and RIRB, and printing.
//
// WHY THIS IS WORTH DOING, in one line: the graph is untrusted input --
// node counts, widget types and connection lists come off the card --
// and in ring 3 a lie about them kills a program instead of the machine.
//
// THREE THINGS THAT BITE, all paid for already:
//
//  - THE CONTROLLER ARRIVES IN RESET and its whole register file reads
//    zero, because hda_remove() writes GCTL.CRST low on its way out. So
//    the first thing here is to bring it up -- and CRST reading back
//    high is the write being ACCEPTED, not the link being ready. The
//    codecs need the spec's 25 frames, which is what a process can just
//    sleep for and hda_probe() cannot.
//  - A REGISTER IS READ AT ITS OWN WIDTH. GCAP is 16 bits, and reading
//    it as two bytes gives 0x0001: a device models a register, not
//    memory.
//  - IT NEEDS `spawn`, never `run`. The legacy loader has no scheduler
//    slot, so every one of these calls comes back EPERM.
//
// AND IT MUST GIVE THE CONTROLLER BACK. A claim dropped by a dying
// process deliberately does NOT rebind, so a crash here leaves the
// machine with no sound until something claims and releases it again.
// Every exit path below goes through finish().
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   // usleep -- the thing a driver in a PROCESS can do
#include <errno.h>
#include "rt/sys.h"
#include "pci.h"
#include "hda_codec.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "sound_abi.h"   // the ring the card plays, and its chunking
#include "fixed.h"       // fx_sin -- there is no floating point here
#include "lib/cmd.h"

#define USAGE "lscodec [-v] [-d INDEX] [--tone [SECONDS]]"

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03

// --- the controller ----------------------------------------------------

// THE WAKEWORD, in a page of its own. The kernel bumps it from the IRQ
// stub, so it has to be somewhere stable and CACHEABLE -- not the DMA
// buffer, which is mapped uncacheable for the device's sake and would
// have the kernel's write and this process's read disagreeing about
// which copy is real. An shm page is what every other wakeword here
// uses (userland/lib/uchan.c).
struct wake_page { volatile uint32_t word; };

struct ctrl {
    volatile uint8_t *mmio;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    uint16_t corb_ents, rirb_ents;
    uint16_t rirb_rp;
    uint8_t  cad;
    int      iss;         // input stream descriptors, from GCAP
    uint32_t sd;          // OUR output descriptor's register base
    int      dev;         // the PCI enumeration index
    int      claimed;
    int      irq;         // 1 once the interrupt is routed here
    uint32_t irqs;        // how many the kernel reported, in total
    uint32_t woken;       // waits the wakeword actually released
    uint32_t timeouts;    // ...and waits that fell back to the deadline
    volatile struct wake_page *wake;
};

static inline uint8_t  mr8(struct ctrl *h, uint32_t o)  { return *(volatile uint8_t *)(h->mmio + o); }
static inline uint16_t mr16(struct ctrl *h, uint32_t o) { return *(volatile uint16_t *)(h->mmio + o); }
static inline uint32_t mr32(struct ctrl *h, uint32_t o) { return *(volatile uint32_t *)(h->mmio + o); }
static inline void mw8(struct ctrl *h, uint32_t o, uint8_t v)   { *(volatile uint8_t *)(h->mmio + o) = v; }
static inline void mw16(struct ctrl *h, uint32_t o, uint16_t v) { *(volatile uint16_t *)(h->mmio + o) = v; }
static inline void mw32(struct ctrl *h, uint32_t o, uint32_t v) { *(volatile uint32_t *)(h->mmio + o) = v; }

// A bounded spin, then a sleep -- this is a process, so waiting costs
// the scheduler nothing and there is no tick deadline to miss.
static int wait_bits32(struct ctrl *h, uint32_t off, uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if ((mr32(h, off) & mask) == want) return 0;
        usleep(100);
    }
    return -1;
}

// One verb, one response, polled. The kernel's hda_cmd() has to
// serialise against its own interrupt handler; nothing here does,
// because this process is the only thing touching the RIRB.
static int corb_cmd(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out) {
    struct ctrl *h = ctx;
    if (out) *out = 0;

    uint16_t wp = (uint16_t)((mr16(h, HDA_CORBWP) + 1) % h->corb_ents);
    h->corb[wp] = ((uint32_t)h->cad << 28) | ((uint32_t)nid << 20) | (verb20 & 0xFFFFF);
    mw16(h, HDA_CORBWP, wp);

    // RIRBSTS IS ACKNOWLEDGED EVERY LAP, not just at the end. QEMU's
    // controller stops fetching from the CORB while RINTCNT responses
    // are pending and RINTFL is still set, so a path that never wrote
    // it gets exactly one answer and then silence. It is also what
    // quiesces the device before the interrupt is acked -- the ack
    // unmasks the line, and a level line whose cause is still asserted
    // re-fires at once.
    for (int lap = 0; lap < 2000; lap++) {
        mw8(h, HDA_RIRBSTS, RIRBSTS_ACK);
        uint16_t rwp = mr16(h, HDA_RIRBWP) & 0xFF;
        while (h->rirb_rp != rwp) {
            h->rirb_rp = (uint16_t)((h->rirb_rp + 1) % h->rirb_ents);
            uint64_t e = h->rirb[h->rirb_rp];
            if ((uint32_t)(e >> 32) & 0x10) continue;  // unsolicited: a jack event
            if (out) *out = (uint32_t)e;
            if (h->irq) {
                int n = sys_dev_irq_ack(h->dev);
                if (n > 0) h->irqs += (uint32_t)n;
            }
            return 0;
        }
        if (h->irq) {
            // PARKED, not spinning: the controller raises an interrupt
            // per response (RIRBCTL_RINTCTL with RINTCNT 1), the kernel
            // stub bumps this word, and SYS_FUTEX_WAIT returns. The
            // value is re-read each lap so a bump that landed between
            // the drain and the park does not park on a stale one --
            // which is the race the word's value argument closes.
            uint32_t seen = h->wake->word;
            int n = sys_dev_irq_ack(h->dev);   // unmask before waiting
            if (n > 0) h->irqs += (uint32_t)n;
            sys_futex_wait(&h->wake->word, seen, 50);
            // WOKEN, OR MERELY OUT OF TIME. The interrupt COUNT cannot
            // tell these apart -- it is bumped by the ring-0 stub,
            // which runs whether or not the wake reaches anybody, and
            // the deadline means the walk completes either way with
            // the same graph. This is the only number that does.
            if (h->wake->word != seen) h->woken++; else h->timeouts++;
        } else {
            usleep(100);
        }
    }
    // A TIMEOUT RESYNCS THE READ POINTER, or the answer that arrives
    // late is handed to the NEXT verb -- every reading after one slow
    // codec shifted by one, which is a wrong graph rather than an
    // error. Skipping to the write pointer discards it instead.
    // Reduced mod the ring size, which the write pointer is NOT: it
    // is 8 bits, and a controller reporting fewer than 256 entries
    // would otherwise leave the read pointer outside its own ring.
    h->rirb_rp = (uint16_t)((mr16(h, HDA_RIRBWP) & 0xFF) % h->rirb_ents);
    return -1;
}

static int corb_rirb_start(struct ctrl *h, uint64_t dma_phys) {
    uint8_t csz = mr8(h, HDA_CORBSIZE) >> 4;
    uint8_t rsz = mr8(h, HDA_RIRBSIZE) >> 4;
    uint8_t csel = (csz & 4) ? 2 : (csz & 2) ? 1 : 0;
    uint8_t rsel = (rsz & 4) ? 2 : (rsz & 2) ? 1 : 0;
    h->corb_ents = csel == 2 ? 256 : csel == 1 ? 16 : 2;
    h->rirb_ents = rsel == 2 ? 256 : rsel == 1 ? 16 : 2;

    mw8(h, HDA_CORBCTL, 0);
    mw8(h, HDA_RIRBCTL, 0);

    mw8(h, HDA_CORBSIZE, csel);
    mw32(h, HDA_CORBLBASE, (uint32_t)(dma_phys + HDA_CORB_OFF));
    mw32(h, HDA_CORBUBASE, (uint32_t)((dma_phys + HDA_CORB_OFF) >> 32));
    mw16(h, HDA_CORBWP, 0);
    // The read-pointer reset handshake: set, see it set, clear, see it
    // clear. Some controllers never show the set half, so both waits
    // are bounded and neither is fatal -- Linux tolerates the same.
    mw16(h, HDA_CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(mr16(h, HDA_CORBRP) & 0x8000); i++) usleep(100);
    mw16(h, HDA_CORBRP, 0);
    for (int i = 0; i < 100 && (mr16(h, HDA_CORBRP) & 0x8000); i++) usleep(100);

    mw8(h, HDA_RIRBSIZE, rsel);
    mw32(h, HDA_RIRBLBASE, (uint32_t)(dma_phys + HDA_RIRB_OFF));
    mw32(h, HDA_RIRBUBASE, (uint32_t)((dma_phys + HDA_RIRB_OFF) >> 32));
    mw16(h, HDA_RIRBWP, 0x8000);
    mw16(h, HDA_RINTCNT, 1);
    mw8(h, HDA_RIRBSTS, RIRBSTS_ACK);
    h->rirb_rp = 0;

    mw8(h, HDA_CORBCTL, CORBCTL_RUN);
    mw8(h, HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    return 0;
}

// --- the stream: stage 5's write half ----------------------------------
//
// THE CARD READS THE SAMPLES ITSELF. This process writes a sine into a
// pinned buffer, hands the card that buffer's PHYSICAL address through
// a descriptor list, and the controller fetches it without the CPU
// touching it again. That is the whole of what DMA buys and the whole
// of what it costs: the address is unchecked by any hardware, so a
// wrong one is the card writing wherever it was told.
//
// The buffer list is CYCLIC by construction -- CBL is the whole ring
// and LVI the last entry -- so the engine wraps forever rather than
// halting, which is what hda.c does and what makes an underrun play
// silence rather than stop.
struct bdl_entry { uint64_t addr; uint32_t len; uint32_t ioc; };

#define TONE_HZ 440

static void tone_fill(volatile int16_t *ring) {
    // 440 Hz at SND_RATE, in fixed point: fx_sin takes TURNS, so one
    // period is FX_ONE and the step is that over the samples per cycle.
    uint32_t per_cycle = SND_RATE / TONE_HZ;
    for (uint32_t i = 0; i < SND_RING_BYTES / 4; i++) {
        int32_t v = (fx_sin((fx_t)((i % per_cycle) * (FX_ONE / per_cycle))) * 8000) >> 16;
        ring[2 * i] = ring[2 * i + 1] = (int16_t)v;
    }
}

// Point the engine at the list and run it. `phys` is the DMA buffer's
// physical base; the list sits at +3072 and the ring at +4096, the same
// layout hda.c uses so the two are reading the same map.
static int stream_start(struct ctrl *h, uint64_t phys) {
    uint32_t sd = h->sd;
    mw32(h, sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(h, sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    // The reset handshake: assert, see it assert, release, see it go.
    mw32(h, sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 100 && !(mr32(h, sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    mw32(h, sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(h, sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    if (mr32(h, sd + SD_CTL) & SD_CTL_SRST) return -1;

    uint64_t bdl = phys + 3072;
    mw32(h, sd + SD_BDPL, (uint32_t)bdl);
    mw32(h, sd + SD_BDPU, (uint32_t)(bdl >> 32));
    mw32(h, sd + SD_CBL, SND_RING_BYTES);
    mw16(h, sd + SD_LVI, SND_CHUNKS - 1);
    mw16(h, sd + SD_FMT, HDA_FMT_48K_S16_STEREO);
    mw8(h, sd + SD_STS, SD_STS_ACK);
    mw32(h, sd + SD_CTL,
         ((uint32_t)HDA_STREAM_TAG << SD_CTL_STREAM_SHIFT) | SD_CTL_RUN);
    return 0;
}

static void stream_stop(struct ctrl *h) {
    uint32_t sd = h->sd;
    mw32(h, sd + SD_CTL, mr32(h, sd + SD_CTL) & ~(uint32_t)SD_CTL_RUN & 0x00FFFFFF);
    for (int i = 0; i < 100 && (mr32(h, sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    mw8(h, sd + SD_STS, SD_STS_ACK);
}

// --- printing ----------------------------------------------------------

static const char *widget_type(uint8_t t) {
    switch (t) {
    case WT_AUD_OUT:  return "dac";
    case WT_AUD_IN:   return "adc";
    case WT_MIXER:    return "mixer";
    case WT_SELECTOR: return "select";
    case WT_PIN:      return "pin";
    default:          return "other";
    }
}

static const char *pin_device(uint32_t defcfg) {
    switch (DEFCFG_DEV(defcfg)) {
    case DEV_LINE_OUT: return "line-out";
    case DEV_SPEAKER:  return "speaker";
    case DEV_HP_OUT:   return "headphone";
    default:           return "other";
    }
}

static void print_route(const char *what, struct hda_codec *c, struct hda_out *o) {
    printf("  %s route:", what);
    for (int i = 0; i < o->len; i++)
        printf(" %s%02x", i ? "-> " : "", o->path[i]);
    printf("  (pin %02x dac %02x)\n", o->pin, o->dac);
    hda_codec_pick_volume(c, o);
    if (!o->vol_nid) { printf("  %s volume: none on this route\n", what); return; }
    printf("  %s volume: nid %02x, %u steps of %u.%u dB, 0 dB at %u%s\n",
           what, o->vol_nid, o->vol_steps,
           o->vol_step_qdb / 4, (o->vol_step_qdb % 4) * 25,
           o->vol_offset, o->vol_mute ? ", mute-capable" : "");
}

static void print_widgets(struct hda_codec *c) {
    for (int i = 0; i < c->nwidgets; i++) {
        struct hda_widget *w = &c->widgets[i];
        printf("  nid %02x %-6s caps %08x", w->nid, widget_type(w->type), w->caps);
        if (w->type == WT_PIN)
            printf(" pincap %08x defcfg %08x %s%s",
                   w->pincap, w->defcfg, pin_device(w->defcfg),
                   (w->caps & WCAP_DIGITAL) ? " digital" : "");
        uint8_t conns[HDA_CONN_MAX];
        int n = hda_codec_conn_list(c, w->nid, conns, HDA_CONN_MAX);
        if (n) {
            printf(" conn");
            for (int k = 0; k < n; k++) printf(" %02x", conns[k]);
        }
        printf("\n");
    }
}

// --- the device --------------------------------------------------------

static struct ctrl g_h;
static struct hda_codec g_codec;

// THE ONLY WAY OUT. Without DEV_RELEASE_REBIND the kernel leaves the
// controller unbound, and on a machine with one sound card that means
// no sound until somebody claims and releases it again.
static int finish(int code) {
    // The wakeword goes before the claim does: the kernel drops the
    // routing with the claim, but a registered word pointing into a
    // page this process is about to unmap is not something to leave
    // lying about.
    if (g_h.wake) { sys_wakeword(0); sys_shm_unlink("snd.lscodec.wake"); }
    if (g_h.claimed) sys_dev_release(g_h.dev, DEV_RELEASE_REBIND);
    return code;
}

// Is this device claimable at all? QUERY_PCIDEV is how ring 3 asks,
// and the alternative is to find out BY CLAIMING -- which unbinds a
// live device to learn something the kernel already knows.
static int claimable(int index) {
    struct query_pcidev q;
    QUERY_FOREACH(QUERY_PCIDEV, q, i) {
        if ((int)q.index == index) return q.claimable ? 1 : 0;
    }
    return 0;
}

// The FIRST class-04:03 controller. On a machine with two that is the
// display-audio one (00:03.0 on the test laptop), which has no analog
// output and drives no speakers -- so the default target is the one
// that cannot silence the machine. `-d` overrides it.
static int find_controller(void) {
    int count = sys_pci_count();
    for (int i = 0; i < count; i++) {
        struct pci_device d;
        if (sys_pci_info(i, &d) != 0) continue;
        if (d.class_code == HDA_CLASS && d.subclass == HDA_SUBCLASS) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    int verbose = 0, want = -1, tone_secs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { want = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--tone")) {
            tone_secs = 2;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                tone_secs = atoi(argv[++i]);
            if (tone_secs < 1) tone_secs = 1;
            if (tone_secs > 10) tone_secs = 10;
            continue;
        }
        cmd_usage(USAGE);
        return 1;
    }

    int index = want >= 0 ? want : find_controller();
    if (index < 0) {
        fprintf(stderr, "lscodec: no HD Audio controller on this machine\n");
        return 1;
    }
    struct pci_device d;
    if (sys_pci_info(index, &d) != 0) {
        fprintf(stderr, "lscodec: no PCI device at index %d\n", index);
        return 1;
    }
    if (!claimable(index)) {
        fprintf(stderr, "lscodec: pci %d cannot be claimed -- a ring-0 driver "
                        "holds it and has no remove() (lspci -k says which "
                        "can)\n", index);
        return 1;
    }

    g_h.dev = index;
    if (sys_dev_claim(index) != 0) {
        fprintf(stderr, "lscodec: cannot claim pci %d: %s\n", index, strerror(errno));
        return 1;
    }
    g_h.claimed = 1;
    printf("lscodec: pci %d %02x:%02x.%u %04x:%04x claimed\n",
           index, d.bus, d.device, d.function, d.vendor_id, d.device_id);

    int64_t bar0 = sys_dev_map_bar(index, 0);
    if (bar0 <= 0) {
        fprintf(stderr, "lscodec: bar0 did not map: %s\n", strerror(errno));
        return finish(1);
    }
    g_h.mmio = (volatile uint8_t *)(uintptr_t)bar0;

    // THE RING ONLY WHEN PLAYING. A read-only walk needs the command
    // rings and nothing else, and every byte of this is memory the card
    // can be pointed at -- so the size is what the run actually uses.
    uint64_t want_dma = tone_secs ? HDA_RING_BYTES + SND_RING_BYTES : HDA_RING_BYTES;
    uint64_t dma_phys = 0;
    int64_t dma = sys_dev_dma_alloc(index, want_dma, &dma_phys);
    if (dma <= 0) {
        fprintf(stderr, "lscodec: no DMA buffer: %s\n", strerror(errno));
        return finish(1);
    }
    g_h.corb = (volatile uint32_t *)(uintptr_t)(dma + HDA_CORB_OFF);
    g_h.rirb = (volatile uint64_t *)(uintptr_t)(dma + HDA_RIRB_OFF);
    printf("lscodec: bar0 at %llx, command rings at %llx (phys %llx)\n",
           (unsigned long long)bar0, (unsigned long long)dma,
           (unsigned long long)dma_phys);

    // Out of reset. CRST high is the write landing; the codecs answering
    // is a separate wait, and GCAP reads 0 until then.
    mw32(&g_h, HDA_INTCTL, 0);
    mw32(&g_h, HDA_GCTL, mr32(&g_h, HDA_GCTL) & ~(uint32_t)GCTL_CRST);
    wait_bits32(&g_h, HDA_GCTL, GCTL_CRST, 0, 100);
    mw32(&g_h, HDA_GCTL, GCTL_CRST);
    if (wait_bits32(&g_h, HDA_GCTL, GCTL_CRST, GCTL_CRST, 100) != 0) {
        fprintf(stderr, "lscodec: the controller never left reset\n");
        return finish(1);
    }
    usleep(50000);

    uint16_t gcap = mr16(&g_h, HDA_GCAP);
    uint16_t statests = mr16(&g_h, HDA_STATESTS) & 0x7FFF;
    mw16(&g_h, HDA_STATESTS, statests);
    mw32(&g_h, HDA_GCTL, GCTL_CRST | GCTL_UNSOL);
    g_h.iss = (gcap >> 8) & 0xF;
    // The first OUTPUT descriptor follows the inputs -- the same
    // derivation hda.c makes, and the reason GCAP is read rather than
    // assumed (QEMU says 4 in, the ASUS's display audio says 0).
    g_h.sd = HDA_SD_BASE + (uint32_t)g_h.iss * 0x20;
    printf("lscodec: hd audio %u.%u gcap %#x (%u in, %u out) codecs %#x\n",
           mr8(&g_h, HDA_VMAJ), mr8(&g_h, HDA_VMIN), gcap,
           (gcap >> 8) & 0xF, (gcap >> 12) & 0xF, statests);
    if (!statests) {
        fprintf(stderr, "lscodec: no codec answered the reset\n");
        return finish(1);
    }

    corb_rirb_start(&g_h, dma_phys);

    // --- the interrupt, if we can have one ---------------------------
    //
    // A WAKEWORD FIRST: the kernel refuses to route an interrupt to a
    // process that has nowhere to be woken, rather than letting it park
    // forever. Its page is shm because a wakeword has to stay mapped
    // and this one is written by the kernel from interrupt context.
    int wfd = sys_shm_open("snd.lscodec.wake", 4096, SHM_CREATE | SHM_EXCL);
    if (wfd >= 0) {
        void *wp = sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
                            SYS_MAP_SHARED, wfd, 0);
        if (wp != (void *)-1) {
            g_h.wake = wp;
            g_h.wake->word = 0;
            if (sys_wakeword(&g_h.wake->word) == 0 &&
                sys_dev_irq_enable(index) == 0) {
                g_h.irq = 1;
                // The controller's own enable. Without it the RIRB
                // raises nothing and the wait below would only ever
                // time out -- slower than the poll it replaced.
                mw32(&g_h, HDA_INTCTL, INTCTL_GIE | INTCTL_CIE);
            }
        }
    }
    printf("lscodec: responses are %s\n",
           g_h.irq ? "interrupt-driven" : "polled (no interrupt available)");

    g_codec.cmd = corb_cmd;
    g_codec.ctx = &g_h;

    int found = 0, played = 0;
    for (uint8_t cad = 0; cad < 15; cad++) {
        if (!(statests & (1u << cad))) continue;
        g_h.cad = cad;
        uint32_t vendor = 0;
        if (corb_cmd(&g_h, 0, V12(VERB_GET_PARAM, PARAM_VENDOR_ID), &vendor) != 0) {
            printf("codec %u: did not answer\n", cad);
            continue;
        }
        found++;
        g_codec.vendor = vendor;
        printf("codec %u: vendor %04x:%04x\n", cad, vendor >> 16, vendor & 0xFFFF);
        if (hda_codec_enumerate(&g_codec) != 0) {
            printf("  no audio function group\n");
            continue;
        }
        printf("  afg nid %02x, %d widget(s)\n", g_codec.afg, g_codec.nwidgets);
        if (verbose) print_widgets(&g_codec);

        if (hda_codec_pick_outputs(&g_codec) != 0) {
            // The honest answer on a display-audio codec, and the same
            // one the kernel's hda.c gives before declining to register
            // it: every pin is digital, so there is nothing to route.
            printf("  no analog output -- nothing to route\n");
            continue;
        }
        print_route("speaker", &g_codec, &g_codec.spk);
        if (g_codec.have_hp) print_route("headphone", &g_codec, &g_codec.hp);

        if (tone_secs && !played) {
            // ROUTE FIRST, THEN RUN. The codec has to be told which DAC
            // listens to our stream tag and to unmute the path; without
            // that the engine runs happily and nothing comes out.
            hda_codec_route_output(&g_codec, &g_codec.spk,
                                   HDA_FMT_48K_S16_STEREO, HDA_STREAM_TAG);
            volatile int16_t *ring = (volatile int16_t *)(uintptr_t)(dma + 4096);
            tone_fill(ring);
            volatile struct bdl_entry *bdl =
                (volatile struct bdl_entry *)(uintptr_t)(dma + 3072);
            for (int k = 0; k < SND_CHUNKS; k++) {
                bdl[k].addr = dma_phys + 4096 + (uint64_t)k * SND_CHUNK_BYTES;
                bdl[k].len = SND_CHUNK_BYTES;
                bdl[k].ioc = 1;
            }
            if (stream_start(&g_h, dma_phys) != 0) {
                fprintf(stderr, "lscodec: the stream never left reset\n");
            } else {
                printf("lscodec: playing %d Hz for %d s from ring 3\n",
                       TONE_HZ, tone_secs);
                for (int k = 0; k < tone_secs * 10; k++) usleep(100000);
                stream_stop(&g_h);
                printf("lscodec: stream stopped\n");
            }
            played = 1;
        }
    }

    // THE COUNT IS THE EVIDENCE. A build whose interrupts never arrived
    // walks the same graph and prints the same routes -- it just waits
    // out a 50 ms timeout per verb instead. Only this number tells the
    // two apart.
    if (g_h.irq)
        printf("lscodec: %u interrupt(s), %u wakeup(s), %u timeout(s)\n",
               g_h.irqs, g_h.woken, g_h.timeouts);
    printf("lscodec: %d codec(s), releasing pci %d back to the kernel\n",
           found, index);
    return finish(found ? 0 : 1);
}
