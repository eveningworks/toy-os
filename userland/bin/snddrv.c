// snddrv -- the sound driver host. One process per card.
//
// docs/umdf-design.md's end state. It claims a sound controller, loads
// the driver for it out of /lib/snd/, and registers with the kernel's
// sound core as a `sound_device` -- so `soundd` mixes into it and
// `aplay`, the Audio Player and the per-application volume all work
// against a driver that is not in the kernel and do not know.
//
// ADDING A SOUND CARD DOES NOT REBUILD THIS PROGRAM. The drivers are
// dlopen'd plugins (userland/include/snd_driver.h), scanned out of a
// directory -- Windows UMDF's and DriverKit's split between a
// system-provided host and a per-device driver.
//
// WHAT NEVER CROSSES INTO THIS PROCESS: the samples. The kernel owns
// the ring, `soundd` writes into it, and a driver here is told the
// ring's PHYSICAL address so it can point its engine at it. The
// exposure is one buffer's address and a device that obeys it.
//
// THE LOOP IS ONE WAIT. The kernel's start/stop/volume request and the
// controller's interrupt both arrive on the wakeword (SYS_WAKEWORD,
// this kernel's eventfd), which is what that word exists for.
//
// IT MUST BE `spawn`ed, never `run` -- every call below needs a
// scheduler slot, and the legacy loader has none.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <dlfcn.h>
#include "rt/sys.h"
#include "pci.h"
#include "snd_driver.h"
#include "sound_abi.h"
#include "syscall_abi.h"
#include "query_abi.h"
#include "lib/cmd.h"
#include <sys/resource.h>

#define USAGE "snddrv [-d INDEX] [--driver NAME] [-v]"

#define PLUGIN_DIR "/lib/snd"
#define MAX_PLUGINS 8

struct shared {
    struct snd_driver_page drv;
    volatile uint32_t wake;
};

static volatile struct shared *g_sh;
static char g_shm_name[32];
static int  g_dev = -1;                 // the PCI index we hold
static int  g_running;
static int  g_verbose;
static struct snd_dev g_card;
static const struct snd_driver *g_drv;

// A POLITE KILL HANDS THE CARD BACK. A claim dropped by a DYING
// process deliberately does not rebind, so without this a kill leaves
// the machine with no sound device at all. SIGHUP is here because
// `spawn` keeps the launching shell's process group, and closing that
// shell silenced a laptop once.
static volatile sig_atomic_t g_quit;
static void on_term(int sig) { (void)sig; g_quit = 1; }

static void give_back(void) {
    if (g_dev >= 0) sys_dev_release(g_dev, DEV_RELEASE_REBIND);
    g_dev = -1;
}

// A driver that found its own device releases it in close(); the host
// only knows about the PCI one it took.

static void release_all(void) {
    if (g_running && g_drv) { g_drv->stop(&g_card); g_running = 0; }
    if (g_drv && g_drv->close) g_drv->close(&g_card);
    if (g_sh) { sys_wakeword(0); sys_shm_unlink(g_shm_name); }
    give_back();
}

// --- the plugins -------------------------------------------------------

static const struct snd_driver *g_plugins[MAX_PLUGINS];
static int g_nplugins;

// EVERY .so IN THE DIRECTORY, so a new card is a file rather than an
// edit here. A plugin that will not load is REPORTED and skipped: one
// bad file must not cost the machine every other card.
static void load_plugins(const char *want) {
    DIR *d = opendir(PLUGIN_DIR);
    if (!d) {
        fprintf(stderr, "snddrv: no %s -- no drivers to load\n", PLUGIN_DIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && g_nplugins < MAX_PLUGINS) {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 3, ".so") != 0) continue;

        char path[128];
        snprintf(path, sizeof path, "%s/%s", PLUGIN_DIR, e->d_name);
        void *h = dlopen(path, RTLD_NOW);
        if (!h) {
            fprintf(stderr, "snddrv: %s: %s\n", path, dlerror());
            continue;
        }
        const struct snd_driver *drv = dlsym(h, SND_DRIVER_SYMBOL);
        if (!drv) {
            fprintf(stderr, "snddrv: %s exports no `%s`\n", path, SND_DRIVER_SYMBOL);
            continue;
        }
        // REFUSED BY NAME rather than called through a moved slot --
        // abi/toyabi.h's rule, for the same reason.
        if (drv->abi != SND_DRIVER_ABI) {
            fprintf(stderr, "snddrv: %s is driver ABI %u, this host speaks %u"
                            " -- rebuild it\n",
                    path, (unsigned)drv->abi, (unsigned)SND_DRIVER_ABI);
            continue;
        }
        // `match` may be NULL -- that is a driver whose device is not
        // on the PCI bus and which finds its own (snd_driver.h).
        if (!drv->name || !drv->open || !drv->start ||
            !drv->stop || !drv->period) {
            fprintf(stderr, "snddrv: %s has an incomplete driver table\n", path);
            continue;
        }
        if (want && strcmp(want, drv->name) != 0) continue;
        g_plugins[g_nplugins++] = drv;
        if (g_verbose) fprintf(stderr, "snddrv: loaded %s (%s)\n", path, drv->name);
    }
    closedir(d);
}

// --- the hardware ------------------------------------------------------

static int claimable(int index) {
    struct query_pcidev q;
    QUERY_FOREACH(QUERY_PCIDEV, q, i)
        if ((int)q.index == index) return q.claimable ? 1 : 0;
    return 0;
}

// Claim this device and let the driver bring it up. 0 on success, and
// on failure the card is handed BACK, because the caller tries another.
static int bring_up(int index, const struct snd_driver *drv,
                    const struct pci_device *info) {
    if (sys_dev_claim(index) != 0) {
        if (g_verbose)
            fprintf(stderr, "snddrv: cannot claim pci %d: %s\n", index, strerror(errno));
        return -1;
    }
    g_dev = index;

    memset(&g_card, 0, sizeof g_card);
    g_card.pci = index;
    g_card.info = *info;

    // Only MEMORY bars are mapped: ring 3 cannot run in/out, so an I/O
    // BAR is reached through the kernel instead (snd_driver.h).
    for (int b = 0; b < SND_BARS; b++) {
        if (!info->bar[b] || (info->bar[b] & 1)) continue; // absent or I/O
        int64_t va = sys_dev_map_bar(index, b);
        if (va > 0) g_card.bar[b] = (volatile uint8_t *)(uintptr_t)va;
    }

    if (drv->dma_bytes) {
        int64_t dma = sys_dev_dma_alloc(index, drv->dma_bytes, &g_card.dma_phys);
        if (dma <= 0) {
            fprintf(stderr, "snddrv: dma: %s\n", strerror(errno));
            give_back();
            return -1;
        }
        g_card.dma = (void *)(uintptr_t)dma;
        g_card.dma_bytes = drv->dma_bytes;
    }

    if (drv->open(&g_card) != 0) {
        if (g_verbose)
            fprintf(stderr, "snddrv: %s declined pci %d\n", drv->name, index);
        give_back();
        return -1;
    }
    g_drv = drv;
    return 0;
}

int main(int argc, char **argv) {
    int want_pci = -1;
    const char *want_drv = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { want_pci = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--driver") && i + 1 < argc) { want_drv = argv[++i]; continue; }
        if (!strcmp(argv[i], "-v")) { g_verbose = 1; continue; }
        cmd_usage(USAGE);
        return 1;
    }

    // A DAEMON LEAVES THE SESSION THAT STARTED IT. `spawn` reparents to
    // init but keeps the PROCESS GROUP, so the launching shell's SIGHUP
    // arrives here -- and its default action would kill this without
    // the release path, leaving the card unbound and the machine mute.
    // A DRIVER RUNS WHEN ITS DEVICE ASKS, NOT WHEN ITS TURN COMES.
    // Woken by an interrupt and then queued behind the compositor and
    // the mixer, this missed its refill deadline ~5.6 times a second on
    // a USB audio endpoint -- 17 ms of dead air against a 12 ms buffer.
    // An in-kernel driver never sees it, because it refills inside the
    // handler. Every OS gives an audio thread the same treatment.
    //
    // SAFE HERE BECAUSE THIS PROCESS BLOCKS: the loop below parks on a
    // wakeword within microseconds of being run, so a better level
    // cannot starve anything. A CPU-bound process must never ask.
    // **NOT RAISED YET, AND THE REASON IS A LOCKUP.** Asking for -10
    // here wedged the machine: the loop below does not truly rest for a
    // driver whose device is not on PCI -- `fired` is unconditional
    // there, so every wakeword bump costs a full pass whether or not
    // anything completed. At the default level that merely wastes a
    // slice; at a better one it starves the console, because priority
    // here is STRICT and has no ageing. Fix the loop first, then raise
    // this -- docs/bugs.md has the measurement either way (the rate DID
    // reach 99.5% with it, from 90%).

    setsid();
    signal(SIGHUP, on_term);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    load_plugins(want_drv);
    if (!g_nplugins) {
        fprintf(stderr, "snddrv: no usable driver in %s\n", PLUGIN_DIR);
        return 1;
    }

    // The shared page before the claim: the kernel refuses to register
    // a driver it cannot wake, and refuses to route an interrupt to one.
    snprintf(g_shm_name, sizeof g_shm_name, "snd.drv.%d", sys_getpid());
    int fd = sys_shm_open(g_shm_name, 4096, SHM_CREATE | SHM_EXCL);
    if (fd < 0) { fprintf(stderr, "snddrv: cannot make its page\n"); return 1; }
    void *p = sys_mmap(0, 4096, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    if (p == (void *)-1) { fprintf(stderr, "snddrv: cannot map its page\n"); return 1; }
    g_sh = p;
    if (sys_wakeword(&g_sh->wake) != 0) {
        fprintf(stderr, "snddrv: cannot register a wakeword\n");
        return 1;
    }

    // EACH CANDIDATE UNTIL ONE PLAYS. A laptop with display audio has
    // two HD Audio controllers and the FIRST is usually the digital one
    // -- no analog output, no speakers -- so a host that took the first
    // match would be the system's sound device and play nothing.
    int count = sys_pci_count();
    int index = -1;
    for (int i = 0; i < count && index < 0; i++) {
        if (want_pci >= 0 && i != want_pci) continue;
        struct pci_device info;
        if (sys_pci_info(i, &info) != 0) continue;
        for (int k = 0; k < g_nplugins && index < 0; k++) {
            if (!g_plugins[k]->match) continue;   // finds its own; below
            if (!g_plugins[k]->match(&info)) continue;
            if (!claimable(i)) {
                fprintf(stderr, "snddrv: pci %d cannot be claimed\n", i);
                continue;
            }
            if (bring_up(i, g_plugins[k], &info) == 0) index = i;
        }
    }
    // THEN THE DRIVERS THAT FIND THEIR OWN. A USB card is named by an
    // xHCI slot, not a PCI index, so the host cannot enumerate it --
    // the plugin does, and takes its own claim.
    for (int k = 0; k < g_nplugins && index < 0 && want_pci < 0; k++) {
        if (g_plugins[k]->match) continue;
        memset(&g_card, 0, sizeof g_card);
        g_card.pci = -1;
        if (g_plugins[k]->open(&g_card) == 0) {
            g_drv = g_plugins[k];
            index = -1;          // nothing of ours to release on the PCI side
            break;
        }
        if (g_verbose)
            fprintf(stderr, "snddrv: %s found no device of its own\n",
                    g_plugins[k]->name);
    }

    if (!g_drv) {
        fprintf(stderr, "snddrv: no sound card a loaded driver can play\n");
        release_all();
        return 1;
    }

    struct snd_register_msg m;
    memset(&m, 0, sizeof m);
    g_sh->drv.magic = SND_DRV_MAGIC;
    snprintf(m.name, sizeof m.name, "%s-ring3", g_drv->name);
    strlcpy(m.label, g_drv->label ? g_drv->label : g_drv->name, sizeof m.label);
    m.rates = g_card.rates;
    m.depths = g_card.depths;
    m.page = (uint64_t)(uintptr_t)&g_sh->drv;
    // INTERRUPTS BEFORE REGISTRATION, and the sequence read before it:
    // from the moment the core knows about this driver it may post a
    // start, and a request that arrives during setup must be SEEN.
    // ONLY A PCI DEVICE HAS AN INTERRUPT TO ROUTE. A driver that found
    // its own device on another bus is woken by whatever that bus's
    // completion path bumps -- for USB, the kernel's isochronous
    // callback bumps this same wakeword.
    if (index >= 0) sys_dev_irq_enable(index);
    uint32_t seen_seq = g_sh->drv.seq;
    if (sys_snd_register(&m) != 0) {
        fprintf(stderr, "snddrv: cannot register: %s\n", strerror(errno));
        release_all();
        return 1;
    }
    uint64_t ring_phys = m.ring_phys;
    fprintf(stderr, "snddrv: %s serving pci %d as %s\n",
            g_drv->name, index, m.name);

    while (!g_quit) {
        uint32_t w = g_sh->wake;

        uint32_t seq = g_sh->drv.seq;
        if (seq != seen_seq) {
            seen_seq = seq;
            switch (g_sh->drv.op) {
            case SND_REQ_START:
                // A FAILED START IS SAID OUT LOUD. `running` stays 0,
                // which is what an app watches, but silence with
                // nothing in the log reads as a dead card rather than
                // a driver that refused.
                if (!g_running) {
                    if (g_drv->start(&g_card, ring_phys) == 0) g_running = 1;
                    else fprintf(stderr, "snddrv: %s failed to start the engine\n",
                                 g_drv->name);
                }
                break;
            case SND_REQ_STOP:
                if (g_running) { g_drv->stop(&g_card); g_running = 0; }
                g_sh->drv.running = 0;
                break;
            case SND_REQ_VOLUME:
                if (g_drv->set_volume) g_drv->set_volume(&g_card, (int)g_sh->drv.volume);
                break;
            default:
                break;
            }
        }

        // Acknowledge at the chip BEFORE the kernel unmasks, or a level
        // line re-fires the instant it is let through. A driver with no
        // PCI device has nothing to unmask, so it is simply ASKED on
        // every wake -- its own period() answers SND_IRQ_NOT_MINE when
        // nothing has completed.
        int fired = (index >= 0) ? (sys_dev_irq_ack(index) > 0) : 1;
        if (fired && g_running) {
            int pos = g_drv->period(&g_card);
            if (pos != SND_IRQ_NOT_MINE) {
                sys_snd_period((uint32_t)pos);
                g_sh->drv.running = 1;
            }
        }

        // One word, both sources. The timeout is a BACKSTOP, not the
        // clock: tight while the engine runs, because that is what a
        // missed period report sounds like; long while idle, so a
        // daemon nothing is using does not wake 50 times a second.
        sys_futex_wait(&g_sh->wake, w, g_running ? 20 : 1000);
    }

    fprintf(stderr, "snddrv: releasing pci %d back to the kernel\n", index);
    release_all();
    return 0;
}
