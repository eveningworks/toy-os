// A SOUND DEVICE IMPLEMENTED BY A PROCESS -- docs/umdf-design.md's end
// state, and abi/sound_abi.h carries the contract.
//
// WHAT MOVES AND WHAT DOES NOT. The core keeps the ring, the exclusive
// stream and the consumed-chunk zeroing; the driver programs the card.
// `soundd` is not changed and does not know -- it opens the stream and
// mixes exactly as it did against `hda.c`, which is the whole point:
// the driver layer moved and the mixer above it did not notice.
//
// THE CORE CANNOT CALL INTO A PROCESS. `start`/`stop`/`set_volume` are
// function pointers it invokes synchronously, so they become a REQUEST
// written into a page the driver shares, plus a bump of its wakeword.
// The driver answers in `running`, which is what the core publishes.
//
// **THAT MAKES start() ASYNCHRONOUS, and it is the one real semantic
// change.** A ring-0 driver's start() has programmed the engine by the
// time it returns; this one has only asked. The core already publishes
// `running` for an app to watch rather than promising the engine is
// live, so nothing above had to change -- but a caller that assumed
// otherwise would be wrong here first.
#include "sound.h"
#include "sound_abi.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "scheduler.h"
#include "vmm.h"
#include "futex.h"
#include "dev_claim.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "driver.h"

// IT DECLARES ITSELF LIKE ANY OTHER, because the registry's question is
// "what is bound to this device", and the honest answer is a process.
// It has no `matches`/`probe`: nothing on a bus makes this bind -- a
// PROGRAM does, by calling SYS_SND_REGISTER.
DRIVER_DECLARE("ring3", "sound", "A sound device driven by a ring-3 process");

// One registered driver process. A second one is refused rather than
// queued: the machine has one PCM stream, so a second driver would be
// a device nothing can reach.
static struct {
    int      pid;
    uint64_t pml4;
    uint64_t page_phys;   // the driver's snd_driver_page, by FRAME
    char     name[SND_DRV_NAME_MAX];
    char     label[SND_DRV_LABEL_MAX];
    struct sound_device dev;
    int      live;
} g_drv;

// The page is reached by PHYSICAL address, the way the wakeword is, so
// a request can be posted from any context without caring which
// address space is loaded.
static volatile struct snd_driver_page *page(void) {
    if (!g_drv.live || !g_drv.page_phys) return 0;
    return (volatile struct snd_driver_page *)(uintptr_t)g_drv.page_phys;
}

static void post(uint32_t op, uint32_t volume) {
    volatile struct snd_driver_page *p = page();
    if (!p) return;
    p->op = op;
    p->volume = volume;
    // SEQ LAST, and bumped rather than set: it is what the driver
    // compares against, so the op must be in place before it moves,
    // and two identical requests must still read as two.
    p->seq = p->seq + 1;
    futex_note_ready(g_drv.pid);
}

static int proc_start(void) {
    volatile struct snd_driver_page *p = page();
    if (!p) return -1;
    post(SND_REQ_START, 0);
    // ASKED, NOT DONE. Returning 0 says the request is with the driver;
    // `running` is what says the engine is live, and the core publishes
    // that from the driver's own answer via sound_proc_poll().
    return 0;
}

static void proc_stop(void) {
    // PUBLISHED AT ONCE, unlike the start. "Stopped" is true the moment
    // the core decides it -- an app must not see `running` while the
    // engine is being torn down -- whereas "started" is only true once
    // the driver has actually programmed the card, which is what the
    // period reports below say.
    sound_publish_running(&g_drv.dev, 0);
    post(SND_REQ_STOP, 0);
}

static void proc_volume(int pct) {
    post(SND_REQ_VOLUME, (uint32_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct));
}

int sound_proc_registered(void) { return g_drv.live; }

// --- registration ------------------------------------------------------

static void unregister_now(const char *why) {
    if (!g_drv.live) return;
    klog_printf("sound: ring-3 driver %s gone -- %s\n", g_drv.name, why);
    // LIVE FIRST, AND IT IS LOAD-BEARING. sound_unregister() calls
    // dev->stop(), which would post a request into a page whose frame
    // has already been freed -- this runs after shm_process_gone(). The
    // clear makes page() answer 0 and post() a no-op.
    g_drv.live = 0;
    sound_unregister(&g_drv.dev);
    k_memset(&g_drv, 0, sizeof g_drv);
}

// THE REGISTRATION DIES WITH THE ADDRESS SPACE, like the claim and the
// stream. Called from release_process_state().
void sound_proc_space_gone(uint64_t pml4) {
    if (g_drv.live && g_drv.pml4 == pml4) unregister_now("its process exited");
}

int sys_snd_register(struct syscall_ctx *c) {
    struct snd_register_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if (g_drv.live) { ret = -EBUSY; goto out; }
    m.name[SND_DRV_NAME_MAX - 1] = '\0';
    m.label[SND_DRV_LABEL_MAX - 1] = '\0';
    if (!m.name[0] || (m.page & 3)) { ret = -EINVAL; goto out; }

    // A WAKEWORD FIRST, for the reason SYS_DEV_IRQ_ENABLE refuses
    // without one: a request posted to a driver that cannot be woken is
    // a device that never starts, diagnosed as silence.
    int pid = scheduler_current_pid();
    if (!futex_wakeword_phys(pid)) { ret = -ENODEV; goto out; }

    uint64_t phys = vmm_user_phys(c->pml4, m.page);
    if (!phys) { ret = -EFAULT; goto out; }

    uint64_t ring_phys = 0;
    void *ring = sound_ring_alloc(&ring_phys);
    if (!ring) { ret = -ENOMEM; goto out; }

    k_memset(&g_drv, 0, sizeof g_drv);
    g_drv.pid = pid;
    g_drv.pml4 = c->pml4;
    g_drv.page_phys = phys;
    k_strlcpy(g_drv.name, m.name, sizeof g_drv.name);
    k_strlcpy(g_drv.label, m.label, sizeof g_drv.label);
    g_drv.dev.name = g_drv.name;
    g_drv.dev.label = g_drv.label;
    g_drv.dev.driver = "ring3";
    g_drv.dev.start = proc_start;
    g_drv.dev.stop = proc_stop;
    g_drv.dev.set_volume = proc_volume;
    g_drv.dev.rates = m.rates;
    g_drv.dev.depths = m.depths;
    g_drv.live = 1;

    if (!sound_register(&g_drv.dev, ring, ring_phys)) {
        g_drv.live = 0;
        ret = -EBUSY;
        goto out;
    }
    m.ring_phys = ring_phys;
    if (!vmm_copy_to_user(c->pml4, c->a0, &m, sizeof m)) {
        unregister_now("its caller handed back a bad pointer");
        ret = -EFAULT;
        goto out;
    }
    klog_printf("sound: ring-3 driver %s registered, ring at %llx\n",
                g_drv.name, (unsigned long long)ring_phys);
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_snd_period(struct syscall_ctx *c) {
    int64_t ret = -EPERM;
    if (g_drv.live && g_drv.pml4 == c->pml4) {
        uint32_t pos = (uint32_t)c->a0;
        // ON A CHUNK BOUNDARY AND INSIDE THE RING, checked rather than
        // trusted: this walks the zeroing loop from the last position
        // to this one, and a position it can never reach would zero the
        // whole ring one chunk at a time and never stop.
        if (pos < SND_RING_BYTES && (pos % SND_CHUNK_BYTES) == 0) {
            // REPORTING A PERIOD IS THE PROOF IT IS RUNNING, and the
            // only one the core can have: start() could merely ask. So
            // this is where `running` becomes true for an app watching
            // it -- one chunk late, which at 48 kHz is 21 ms.
            sound_publish_running(&g_drv.dev, 1);
            sound_period_done(pos);
            ret = 0;
        } else {
            ret = -EINVAL;
        }
    }
    c->regs[14] = (uint64_t)ret;
    return 0;
}
