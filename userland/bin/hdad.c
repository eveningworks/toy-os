// hdad -- HD Audio, driven from ring 3, as the machine's sound device.
//
// THE END OF docs/umdf-design.md. `lscodec --tone` proved a process
// could make a card play; this is the version the SYSTEM uses. It
// registers as a `sound_device` (SYS_SND_REGISTER), so `soundd` mixes
// into the core's ring exactly as it did against the in-kernel driver,
// and `aplay`, the Player and the per-application volume all work with
// no idea that the driver is a process.
//
// WHAT THIS PROCESS NEVER TOUCHES: the samples. The kernel owns the
// ring, `soundd` writes into it, and this program only tells the card
// which physical address to fetch from. Its exposure is what stage 5
// already granted -- one buffer's address, and a device that obeys it.
//
// THE LOOP IS ONE WAIT. Everything that can happen arrives on the
// wakeword (SYS_WAKEWORD, this kernel's eventfd): the kernel posting a
// start/stop/volume request into the shared page, and the controller's
// interrupt arriving through the stage-4 routing. One word, both
// sources, which is what that word exists for.
//
// IT MUST BE `spawn`ed, never `run` -- every one of these calls needs a
// scheduler slot, and the legacy loader has none.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include "rt/sys.h"
#include "pci.h"
#include "hda_codec.h"
#include "sound_abi.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "lib/cmd.h"

#define USAGE "hdad [-d INDEX]"

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03
#define SHM_PAGE "snd.hdad"

struct bdl_entry { uint64_t addr; uint32_t len; uint32_t ioc; };

// The shared page carries BOTH the kernel's requests and the wakeword,
// because they are woken together and there is no reason for two.
struct shared {
    struct snd_driver_page drv;
    volatile uint32_t wake;
};

static volatile uint8_t *g_mmio;
static volatile uint32_t *g_corb;
static volatile uint64_t *g_rirb;
static volatile struct bdl_entry *g_bdl;
static volatile struct shared *g_sh;
static uint64_t g_dma_phys;
static uint16_t g_corb_ents = 256, g_rirb_ents = 256, g_rirb_rp;
static uint8_t  g_cad;
static uint32_t g_sd;          // our output stream descriptor's base
static uint32_t g_chunk;       // the ring chunk the card has finished
static uint64_t g_ring_phys;   // the CORE's ring, which the card fetches
static int      g_dev = -1;    // the PCI index we hold
static int      g_running;
static struct hda_codec g_codec;
// A POLITE KILL HANDS THE CARD BACK. A claim dropped by a DYING process
// deliberately does not rebind, so without this `kill hdad` leaves the
// machine with no sound device at all -- recoverable only by running
// hdad or lscodec again. Windows' UMDF host shuts a device down on its
// way out for the same reason; a CRASH still leaves it unbound, which
// is the deliberate part.
static volatile sig_atomic_t g_quit;
static void on_term(int sig) { (void)sig; g_quit = 1; }

static inline uint8_t  mr8(uint32_t o)  { return *(volatile uint8_t *)(g_mmio + o); }
static inline uint16_t mr16(uint32_t o) { return *(volatile uint16_t *)(g_mmio + o); }
static inline uint32_t mr32(uint32_t o) { return *(volatile uint32_t *)(g_mmio + o); }
static inline void mw8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(g_mmio + o) = v; }
static inline void mw16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio + o) = v; }
static inline void mw32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }

// --- the codec's transport, as lscodec's ------------------------------

static int corb_cmd(void *ctx, uint8_t nid, uint32_t verb20, uint32_t *out) {
    (void)ctx;
    if (out) *out = 0;
    uint16_t wp = (uint16_t)((mr16(HDA_CORBWP) + 1) % g_corb_ents);
    g_corb[wp] = ((uint32_t)g_cad << 28) | ((uint32_t)nid << 20) | (verb20 & 0xFFFFF);
    mw16(HDA_CORBWP, wp);
    for (int lap = 0; lap < 2000; lap++) {
        mw8(HDA_RIRBSTS, RIRBSTS_ACK);
        uint16_t rwp = mr16(HDA_RIRBWP) & 0xFF;
        while (g_rirb_rp != rwp) {
            g_rirb_rp = (uint16_t)((g_rirb_rp + 1) % g_rirb_ents);
            uint64_t e = g_rirb[g_rirb_rp];
            if ((uint32_t)(e >> 32) & 0x10) continue;   // unsolicited
            if (out) *out = (uint32_t)e;
            return 0;
        }
        usleep(200);
    }
    g_rirb_rp = (uint16_t)((mr16(HDA_RIRBWP) & 0xFF) % g_rirb_ents);
    return -1;
}

// --- the stream --------------------------------------------------------

static int stream_start(void) {
    mw32(g_sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(g_sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    mw32(g_sd + SD_CTL, SD_CTL_SRST);
    for (int i = 0; i < 100 && !(mr32(g_sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    mw32(g_sd + SD_CTL, 0);
    for (int i = 0; i < 100 && (mr32(g_sd + SD_CTL) & SD_CTL_SRST); i++) usleep(100);
    if (mr32(g_sd + SD_CTL) & SD_CTL_SRST) return -1;

    // THE DESCRIPTOR LIST IS BUILT HERE, not once at startup, because
    // the core may ask for a start the moment SYS_SND_REGISTER returns
    // -- and it is that call which hands back the ring's address. Doing
    // it in the caller left a window where a start could be served
    // against an empty list.
    for (int k = 0; k < SND_CHUNKS; k++) {
        g_bdl[k].addr = g_ring_phys + (uint64_t)k * SND_CHUNK_BYTES;
        g_bdl[k].len = SND_CHUNK_BYTES;
        g_bdl[k].ioc = 1;
    }
    uint64_t bdl = g_dma_phys + 3072;
    mw32(g_sd + SD_BDPL, (uint32_t)bdl);
    mw32(g_sd + SD_BDPU, (uint32_t)(bdl >> 32));
    mw32(g_sd + SD_CBL, SND_RING_BYTES);
    mw16(g_sd + SD_LVI, SND_CHUNKS - 1);
    mw16(g_sd + SD_FMT, HDA_FMT_48K_S16_STEREO);
    mw8(g_sd + SD_STS, SD_STS_ACK);
    // IOCE, because a completion per chunk is how the core learns where
    // the card is -- SYS_SND_PERIOD is this driver's only way to say
    // it, and without the interrupt there is nothing to say it from.
    mw32(HDA_INTCTL, INTCTL_GIE | INTCTL_CIE | (1u << ((mr16(HDA_GCAP) >> 8) & 0xF)));
    mw32(g_sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SD_CTL_STREAM_SHIFT) |
                        SD_CTL_RUN | SD_CTL_IOCE);
    return 0;
}

static void stream_stop(void) {
    mw32(g_sd + SD_CTL, mr32(g_sd + SD_CTL) &
         ~(uint32_t)(SD_CTL_RUN | SD_CTL_IOCE) & 0x00FFFFFF);
    for (int i = 0; i < 100 && (mr32(g_sd + SD_CTL) & SD_CTL_RUN); i++) usleep(100);
    mw8(g_sd + SD_STS, SD_STS_ACK);
}

// 0..100 onto the route's amplifier, with the cards' 40 dB taper -- the
// same curve hda.c and soundd use, so one percentage is one loudness
// wherever it is applied.
#define TAPER_DB 40

static void set_volume(int pct) {
    struct hda_out *o = &g_codec.spk;
    if (!g_codec.have_spk || !o->vol_nid) return;
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

// --- bring-up ----------------------------------------------------------

static int claimable(int index) {
    struct query_pcidev q;
    QUERY_FOREACH(QUERY_PCIDEV, q, i)
        if ((int)q.index == index) return q.claimable ? 1 : 0;
    return 0;
}

static int find_controllers(int *out, int cap) {
    int count = sys_pci_count(), n = 0;
    for (int i = 0; i < count && n < cap; i++) {
        struct pci_device d;
        if (sys_pci_info(i, &d) != 0) continue;
        if (d.class_code == HDA_CLASS && d.subclass == HDA_SUBCLASS) out[n++] = i;
    }
    return n;
}

// REBIND, so the kernel's own driver takes the card back: a claim this
// process merely DROPS leaves the device unbound, and on a one-card
// machine that is silence with nothing to explain it.
static void give_back(void) {
    if (g_dev >= 0) sys_dev_release(g_dev, DEV_RELEASE_REBIND);
    g_dev = -1;
    g_mmio = 0;
}

static void release_all(void) {
    if (g_running) stream_stop();
    if (g_sh) { sys_wakeword(0); sys_shm_unlink(SHM_PAGE); }
    give_back();
}

// Claim one controller, bring it up and route its analog output.
// Returns 0 when this card can play, -1 when it cannot -- and on -1 the
// card is handed BACK, because the caller is going to try another one.
static int bring_up(int index) {
    if (sys_dev_claim(index) != 0) {
        fprintf(stderr, "hdad: cannot claim pci %d: %s\n", index, strerror(errno));
        return -1;
    }
    g_dev = index;

    int64_t bar0 = sys_dev_map_bar(index, 0);
    if (bar0 <= 0) { fprintf(stderr, "hdad: bar0: %s\n", strerror(errno)); give_back(); return -1; }
    g_mmio = (volatile uint8_t *)(uintptr_t)bar0;

    int64_t dma = sys_dev_dma_alloc(index, HDA_RING_BYTES, &g_dma_phys);
    if (dma <= 0) { fprintf(stderr, "hdad: dma: %s\n", strerror(errno)); give_back(); return -1; }
    g_corb = (volatile uint32_t *)(uintptr_t)(dma + HDA_CORB_OFF);
    g_rirb = (volatile uint64_t *)(uintptr_t)(dma + HDA_RIRB_OFF);
    g_bdl  = (volatile struct bdl_entry *)(uintptr_t)(dma + 3072);

    // Out of reset, then wait for the codecs -- the thing a process can
    // do and hda_probe() cannot.
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
    g_sd = HDA_SD_BASE + (uint32_t)((gcap >> 8) & 0xF) * 0x20;
    if (!statests) {
        fprintf(stderr, "hdad: pci %d: no codec answered\n", index);
        give_back();
        return -1;
    }

    // CORB/RIRB, as lscodec sets them up.
    mw8(HDA_CORBCTL, 0);
    mw8(HDA_RIRBCTL, 0);
    mw8(HDA_CORBSIZE, 2);
    mw32(HDA_CORBLBASE, (uint32_t)(g_dma_phys + HDA_CORB_OFF));
    mw32(HDA_CORBUBASE, (uint32_t)((g_dma_phys + HDA_CORB_OFF) >> 32));
    mw16(HDA_CORBWP, 0);
    mw16(HDA_CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(mr16(HDA_CORBRP) & 0x8000); i++) usleep(100);
    mw16(HDA_CORBRP, 0);
    for (int i = 0; i < 100 && (mr16(HDA_CORBRP) & 0x8000); i++) usleep(100);
    mw8(HDA_RIRBSIZE, 2);
    mw32(HDA_RIRBLBASE, (uint32_t)(g_dma_phys + HDA_RIRB_OFF));
    mw32(HDA_RIRBUBASE, (uint32_t)((g_dma_phys + HDA_RIRB_OFF) >> 32));
    mw16(HDA_RIRBWP, 0x8000);
    mw16(HDA_RINTCNT, 1);
    mw8(HDA_RIRBSTS, RIRBSTS_ACK);
    g_rirb_rp = 0;
    mw8(HDA_CORBCTL, CORBCTL_RUN);
    mw8(HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);

    g_codec.cmd = corb_cmd;
    g_codec.ctx = 0;
    int found = 0;
    for (uint8_t cad = 0; cad < 15 && !found; cad++) {
        if (!(statests & (1u << cad))) continue;
        g_cad = cad;
        uint32_t vendor = 0;
        if (corb_cmd(0, 0, V12(VERB_GET_PARAM, PARAM_VENDOR_ID), &vendor) != 0) continue;
        g_codec.vendor = vendor;
        if (hda_codec_enumerate(&g_codec) != 0) continue;
        if (hda_codec_pick_outputs(&g_codec) != 0) continue;
        found = 1;
    }
    if (!found) {
        // NOT A FAILURE, AN ANSWER: a display-audio codec's pins are
        // all digital, so there is no analog route to find and the
        // kernel's own hda driver declines the same card.
        fprintf(stderr, "hdad: pci %d has no analog output\n", index);
        give_back();
        return -1;
    }
    hda_codec_route_output(&g_codec, &g_codec.spk,
                           HDA_FMT_48K_S16_STEREO, HDA_STREAM_TAG);
    return 0;
}

int main(int argc, char **argv) {
    int want = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { want = atoi(argv[++i]); continue; }
        cmd_usage(USAGE);
        return 1;
    }

    int cand[8], ncand = 0;
    if (want >= 0) {
        cand[ncand++] = want;
    } else {
        ncand = find_controllers(cand, 8);
    }
    if (!ncand) { fprintf(stderr, "hdad: no HD Audio controller\n"); return 1; }

    // The shared page first: the kernel refuses to register a driver it
    // cannot wake, and refuses to route an interrupt to one either.
    int fd = sys_shm_open(SHM_PAGE, 4096, SHM_CREATE | SHM_EXCL);
    if (fd < 0) { fprintf(stderr, "hdad: another hdad is running\n"); return 1; }
    void *p = sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    if (p == (void *)-1) { fprintf(stderr, "hdad: cannot map its page\n"); return 1; }
    g_sh = p;
    if (sys_wakeword(&g_sh->wake) != 0) {
        fprintf(stderr, "hdad: cannot register a wakeword\n");
        return 1;
    }

    // EACH CONTROLLER IN TURN, until one has an analog output. The ASUS
    // has two, and the FIRST is display audio -- all-digital pins, no
    // speakers -- so a daemon that took the first one would be the
    // system's sound device and play nothing. `lscodec` defaults to the
    // first on purpose (it is the one that cannot silence the machine);
    // a driver wants the opposite. `-d` pins one and skips the search.
    int index = -1;
    for (int i = 0; i < ncand && index < 0; i++) {
        if (!claimable(cand[i])) {
            fprintf(stderr, "hdad: pci %d cannot be claimed\n", cand[i]);
            continue;
        }
        if (bring_up(cand[i]) == 0) index = cand[i];
    }
    if (index < 0) {
        fprintf(stderr, "hdad: no HD Audio controller with an analog output\n");
        release_all();
        return 1;
    }

    // REGISTER, and everything the core could ask for must already be
    // true: it may post a start the instant this returns.
    struct snd_register_msg m;
    memset(&m, 0, sizeof m);
    g_sh->drv.magic = SND_DRV_MAGIC;   // LAST, as the ABI says
    strlcpy(m.name, "hda-ring3", sizeof m.name);
    strlcpy(m.label, "HD Audio (ring 3)", sizeof m.label);
    hda_codec_pcm_support(&g_codec, g_codec.spk.dac, &m.rates, &m.depths);
    m.page = (uint64_t)(uintptr_t)&g_sh->drv;
    // INTERRUPTS BEFORE REGISTRATION, and the sequence read before it
    // too: from the moment the core knows about this driver it may post
    // a start, and a request that arrives during setup must be SEEN,
    // not skipped past. Reading `seq` afterwards silently swallowed it.
    sys_dev_irq_enable(index);
    uint32_t seen_seq = g_sh->drv.seq;
    if (sys_snd_register(&m) != 0) {
        fprintf(stderr, "hdad: cannot register: %s\n", strerror(errno));
        release_all();
        return 1;
    }

    // The core's ring is what the card must fetch, so the descriptor
    // list points at IT rather than at anything this process owns.
    g_ring_phys = m.ring_phys;
    fprintf(stderr, "hdad: serving pci %d as the machine's sound device\n", index);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    while (!g_quit) {
        uint32_t w = g_sh->wake;

        // The kernel's request, if the sequence moved.
        uint32_t seq = g_sh->drv.seq;
        if (seq != seen_seq) {
            seen_seq = seq;
            switch (g_sh->drv.op) {
            case SND_REQ_START:
                if (!g_running && stream_start() == 0) { g_running = 1; g_chunk = 0; }
                break;
            case SND_REQ_STOP:
                if (g_running) { stream_stop(); g_running = 0; }
                g_sh->drv.running = 0;
                break;
            case SND_REQ_VOLUME:
                set_volume((int)g_sh->drv.volume);
                break;
            default:
                break;
            }
        }

        // The controller's interrupt: acknowledge the stream's status
        // BEFORE the ack unmasks, or a level line re-fires at once.
        int n = sys_dev_irq_ack(index);
        if (n > 0 && g_running) {
            uint8_t st = mr8(g_sd + SD_STS);
            if (st & SD_STS_BCIS) {
                // ONE COMPLETION PER CHUNK, SO COUNT THEM -- hda.c's
                // rule, and a process needs it MORE than the kernel
                // does. LPIB is a FETCH position; hda.c reads it inside
                // the handler where it sits on the boundary, while this
                // reads it milliseconds later, after the card has moved
                // on. Reporting that has the core zero a chunk the card
                // has not played yet: measured as 890 Hz out of a 1 kHz
                // file, with 1.95s of sound for 1.5s of samples.
                uint32_t next = (g_chunk + (uint32_t)n) % SND_CHUNKS;
                // LPIB is still the correction, and in BOTH directions:
                // `n` counts every interrupt this controller raised,
                // RIRB's included, so the count can run ahead as well
                // as fall behind.
                uint32_t lc = (mr32(g_sd + SD_LPIB) / SND_CHUNK_BYTES) % SND_CHUNKS;
                uint32_t behind = (lc + SND_CHUNKS - next) % SND_CHUNKS;
                if (behind > SND_CHUNKS / 2) next = lc;
                else if (behind >= 2) next = (lc + SND_CHUNKS - 1) % SND_CHUNKS;
                g_chunk = next;
                sys_snd_period(next * SND_CHUNK_BYTES);
                g_sh->drv.running = 1;
            }
            mw8(g_sd + SD_STS, st & SD_STS_ACK);
        }

        // One word, both sources. The timeout is a BACKSTOP, not the
        // clock: a lost wakeup costs a stutter rather than a hang.
        // Tight while the engine runs, because that is what a missed
        // period report sounds like; long while it is idle, so a daemon
        // nothing is using does not wake 50 times a second forever.
        sys_futex_wait(&g_sh->wake, w, g_running ? 20 : 1000);
    }

    fprintf(stderr, "hdad: releasing pci %d back to the kernel\n", index);
    release_all();
    return 0;
}
