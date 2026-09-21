// A USB DEVICE HELD BY A RING-3 PROCESS -- dev_claim.c's shape, for a
// bus the kernel keeps owning. usb_claim.h says why the two differ.
// driver-none: it drives nothing; it records who holds what
#include "usb_claim.h"
#include "usb.h"
#include "usb_audio.h"
#include "usb_hid.h"
#include "scheduler.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "syscalls.h"
#include "syscall_abi.h"
#include "xhci.h"
#include "pmm.h"
#include "vmm.h"
#include "mmap.h"
#include "futex.h"

#define USB_CLASS_HUB 9   // usb_enum.c's, and the same value the spec gives

static struct {
    uint8_t  slot;      // 0 = free
    uint64_t pml4;
    int      pid;
} g_claims[USB_MAX_DEVICES];

// ONE ISOCHRONOUS OUT ENDPOINT PER CLAIM. A UAC device streams on one;
// a holder that needs two can ask when there is one.
static struct {
    uint8_t  slot, ep, open;
    uint64_t dma_phys, dma_base, dma_pages;
    // Written by the completion callback, which runs in INTERRUPT
    // CONTEXT -- volatile, and read-and-cleared by the status call.
    volatile uint32_t completions;
    // NEVER CLEARED, unlike `completions` -- it is what the close line
    // reports, and a total nobody resets is the one number that says
    // whether the interrupt half ever ran at all.
    volatile uint32_t total;
    volatile uint32_t posted;
    int pid;
} g_isoch;

static void isoch_close(void);

static const struct usb_device_info *dev_by_slot(uint8_t slot) {
    for (int i = 0; i < usb_device_count(); i++) {
        const struct usb_device_info *d = usb_device_at(i);
        if (d && d->in_use && d->slot == slot) return d;
    }
    return 0;
}

static int slot_index(uint8_t slot) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_claims[i].slot == slot && g_claims[i].pml4) return i;
    return -1;
}

int usb_claim_holder_pid(uint8_t slot) {
    int i = slot_index(slot);
    return i < 0 ? 0 : g_claims[i].pid;
}

int usb_claim_check(uint8_t slot, uint64_t pml4) {
    int i = slot_index(slot);
    if (i < 0) return -EACCES;
    return g_claims[i].pml4 == pml4 ? 0 : -EBUSY;
}

// EVERY CLASS DRIVER HERE HAS AN UNBIND, so every enumerated device is
// claimable -- unlike PCI, where a driver without a remove() is the
// thing that makes a device un-claimable. Kept as a function anyway
// because it is the gate a caller asks about, and the day a class
// driver arrives that cannot let go this is the one place to say so.
int usb_claim_claimable(uint8_t slot) {
    const struct usb_device_info *d = dev_by_slot(slot);
    if (!d) return 0;
    // A HUB IS NOT OFFERED. Taking one away would strand every device
    // behind it, which is a claim over things the holder never asked
    // for and cannot give back.
    if (d->dev_class == USB_CLASS_HUB ||
        (d->if_count && d->ifs[0].if_class == USB_CLASS_HUB)) return 0;
    return 1;
}

int usb_claim_take(uint8_t slot, uint64_t pml4, int pid) {
    if (!pml4) return -EPERM;
    const struct usb_device_info *d = dev_by_slot(slot);
    if (!d) return -EINVAL;
    int i = slot_index(slot);
    if (i >= 0) return g_claims[i].pml4 == pml4 ? 0 : -EBUSY;
    if (!usb_claim_claimable(slot)) return -EPERM;

    int free_slot = -1;
    for (int k = 0; k < USB_MAX_DEVICES; k++)
        if (!g_claims[k].pml4) { free_slot = k; break; }
    if (free_slot < 0) return -EBUSY;

    // THE UNBIND COMES FIRST, and it is what the claim is FOR: the
    // class driver has the device's endpoints configured, and two
    // owners posting to one transfer ring is not a thing to recover
    // from. Every unbind here is a no-op on a device that driver does
    // not hold, which is why they are simply all called.
    usb_hid_unbind(slot);
    usb_audio_unbind(slot);
    usb_net_unbind(slot);
    usb_r8153_unbind(slot);
    // `bound` IS SET BY EVERY BIND AND WAS CLEARED BY NOTHING -- it
    // only ever went true, so `lsusb` and QUERY_USB reported a device
    // no driver holds as bound. Cleared here because this is the one
    // place that unbinds a device which then stays present; a DETACH
    // takes the whole entry away.
    ((struct usb_device_info *)d)->bound = 0;

    g_claims[free_slot].slot = slot;
    g_claims[free_slot].pml4 = pml4;
    g_claims[free_slot].pid  = pid;
    klog_printf("usb: pid %d claimed slot %u (%04x:%04x)\n",
                pid, slot, d->vendor_id, d->product_id);
    return 0;
}

int usb_claim_drop(uint8_t slot, uint64_t pml4, int rebind) {
    int i = slot_index(slot);
    if (i < 0) return -EACCES;
    if (pml4 && g_claims[i].pml4 != pml4) return -EACCES;

    int pid = g_claims[i].pid;
    // THE ENDPOINT GOES WITH THE CLAIM. A holder that drops the device
    // while TDs are posted would otherwise leave the controller
    // fetching from frames the allocator has handed to somebody else.
    if (g_isoch.open && g_isoch.slot == slot) isoch_close();
    k_memset(&g_claims[i], 0, sizeof g_claims[i]);
    klog_printf("usb: pid %d released slot %u%s\n", pid, slot,
                rebind ? " (rebinding)" : "");

    // THE DEVICE IS LEFT AS THE HOLDER LEFT IT unless a rebind is
    // asked for -- its alternate setting, its clock, its endpoint
    // state. A class driver binding on top of that is entitled to
    // configure what it needs, which is what bind already does.
    if (rebind) {
        struct usb_device_info *d = (struct usb_device_info *)dev_by_slot(slot);
        if (d && d->cfg && d->cfg_len) usb_bind_drivers(d, d->cfg, d->cfg_len);
    }
    return 0;
}

// A TD GROUP FINISHED. From the event drain, so it does the two cheap
// things and nothing else -- the holder does the refilling, which is
// the whole point of it being in ring 3.
static void isoch_done(void *ctx, uint32_t bytes) {
    (void)ctx; (void)bytes;
    if (!g_isoch.open) return;
    g_isoch.completions++;
    g_isoch.total++;
    futex_note_ready(g_isoch.pid);
}

static void isoch_close(void) {
    if (!g_isoch.open) return;
    // ONE LINE, AT CLOSE. A probe per completion would outrun the klog
    // ring at an endpoint's service rate; this is the whole question --
    // did the controller ever come back -- asked once.
    klog_printf("usb: isoch ep 0x%x closing -- %u posted, %u completed\n",
                g_isoch.ep, (unsigned)g_isoch.posted, (unsigned)g_isoch.total);
    // THE MAPPING GOES BEFORE THE FRAMES. They are borrowed, so no
    // teardown disposes of them -- freeing first would hand the
    // allocator pages the holder still has a live writable PTE for.
    if (g_isoch.dma_base)
        mmap_drop_dma_region(g_isoch.dma_base, g_isoch.dma_pages);
    if (g_isoch.dma_phys)
        pmm_free_contiguous(g_isoch.dma_phys, g_isoch.dma_pages);
    k_memset(&g_isoch, 0, sizeof g_isoch);
}

int sys_usb_isoch_open(struct syscall_ctx *c) {
    struct usb_isoch_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    if (g_isoch.open) { ret = -EBUSY; goto out; }
    if (!m.dma_bytes || m.dma_bytes > 64 * 1024 || !m.mps) { ret = -EINVAL; goto out; }

    uint64_t npages = (m.dma_bytes + 4095) / 4096;
    uint64_t base = mmap_dma_reserve(npages);
    if (!base) { ret = -ENOMEM; goto out; }
    uint64_t phys = pmm_alloc_contiguous(npages, PMM_ZONE_DMA32);
    if (!phys) { ret = -ENOMEM; goto out; }
    if (!mmap_map_dma(c->pml4, base, phys, npages, 1, VMM_MT_UC)) {
        pmm_free_contiguous(phys, npages);
        ret = -ENOMEM;
        goto out;
    }

    g_isoch.slot = (uint8_t)m.slot;
    g_isoch.ep   = (uint8_t)m.ep;
    g_isoch.dma_phys = phys;
    g_isoch.dma_base = base;
    g_isoch.dma_pages = npages;
    g_isoch.pid = scheduler_current_pid();
    g_isoch.open = 1;

    if (xhci_add_isoch_out((uint8_t)m.slot, (uint8_t)m.ep, (uint16_t)m.mps,
                           (uint8_t)m.interval, isoch_done, 0) < 0) {
        isoch_close();
        ret = -EIO;
        goto out;
    }

    m.addr = base;
    m.phys = phys;
    if (!vmm_copy_to_user(c->pml4, c->a0, &m, sizeof m)) {
        isoch_close();
        ret = -EFAULT;
        goto out;
    }
    klog_printf("usb: pid %d opened isoch ep 0x%x on slot %u, %llu page(s) at %llx\n",
                g_isoch.pid, m.ep, (unsigned)m.slot,
                (unsigned long long)npages, (unsigned long long)phys);
    ret = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_isoch_post(struct syscall_ctx *c) {
    struct usb_isoch_post_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    if (!g_isoch.open || g_isoch.slot != m.slot || g_isoch.ep != m.ep) {
        ret = -EINVAL;
        goto out;
    }
    uint32_t count = m.count ? m.count : 1;
    uint32_t stride = m.stride ? m.stride : m.len;
    // THE RANGE IS CHECKED FOR THE WHOLE GROUP, not trusted: each of
    // these becomes a PHYSICAL address in a descriptor the controller
    // will read, so one past the grant would point the hardware at
    // somebody else's page. Checked on the LAST, which bounds them all.
    uint64_t bytes = g_isoch.dma_pages * 4096;
    uint64_t last = (uint64_t)m.offset + (uint64_t)(count - 1) * stride;
    if (!m.len || !count || count > 256 || last + m.len > bytes) {
        ret = -EINVAL;
        goto out;
    }

    // A COMPLETION ON THE LAST ONLY, which is what `IOC every N` means:
    // an isochronous endpoint completes in order and an error reports
    // itself regardless, so one event per group is all the driver needs.
    uint32_t done = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (xhci_isoch_post((uint8_t)m.slot, (uint8_t)m.ep,
                            g_isoch.dma_phys + m.offset + (uint64_t)i * stride,
                            m.len, (m.ioc && i + 1 == count) ? 1 : 0) != 0)
            break;
        done++;
    }
    g_isoch.posted += done;
    ret = done ? (int64_t)done : -EINVAL;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_isoch_status(struct syscall_ctx *c) {
    int64_t ret;
    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if ((ret = usb_claim_check((uint8_t)c->a0, c->pml4)) != 0) goto out;
    if (!g_isoch.open || g_isoch.slot != (uint8_t)c->a0 ||
        g_isoch.ep != (uint8_t)c->a1) { ret = -EINVAL; goto out; }
    // READ AND CLEAR, so a caller that misses a wakeup still learns how
    // many landed rather than one.
    ret = (int64_t)g_isoch.completions;
    g_isoch.completions = 0;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// --- the syscalls ------------------------------------------------------

int sys_usb_claim(struct syscall_ctx *c) {
    int64_t ret = scheduler_current_mm()
        ? usb_claim_take((uint8_t)c->a0, c->pml4, scheduler_current_pid())
        : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// ONE BOUNCE PAGE, and it is shared.
//
// xhci_control() takes a KERNEL buffer and uses its virtual address as
// the physical one, so neither a user pointer nor a kernel stack (which
// has a guard page and is not identity-mapped) can be handed to it.
//
// A MODULE-LEVEL BUFFER REACHED FROM A SYSCALL is what
// docs/smp-design.md says to read before adding, and the reason is
// here: a ring-3 process is preemptible inside a syscall, so two
// holders of two different devices would otherwise interleave through
// this one page. The transfer runs under the same preemption guard
// vfs.c's FS_OP() uses, for the same reason.
static uint64_t g_bounce;

int sys_usb_control(struct syscall_ctx *c) {
    struct usb_control_msg m;
    int64_t ret;

    if (!scheduler_current_mm()) { ret = -EPERM; goto out; }
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) { ret = -EFAULT; goto out; }
    if ((ret = usb_claim_check((uint8_t)m.slot, c->pml4)) != 0) goto out;
    if (m.len > USB_CONTROL_MAX || (m.len && !m.buf)) { ret = -EINVAL; goto out; }

    // The two that would desync the kernel from the controller.
    // bmRequestType 0 = host-to-device, standard, device.
    if (m.setup[0] == 0x00 && (m.setup[1] == 5 || m.setup[1] == 9)) {
        klog_printf("usb: pid %d asked for %s on slot %u -- refused\n",
                    scheduler_current_pid(),
                    m.setup[1] == 5 ? "SET_ADDRESS" : "SET_CONFIGURATION",
                    (unsigned)m.slot);
        ret = -EINVAL;
        goto out;
    }

    if (!g_bounce) {
        g_bounce = pmm_alloc_contiguous(1, PMM_ZONE_DMA32);
        if (!g_bounce) { ret = -ENOMEM; goto out; }
    }
    uint8_t *buf = (uint8_t *)(uintptr_t)g_bounce;  // identity-mapped

    if (m.len && !m.in &&
        !vmm_copy_from_user(c->pml4, buf, m.buf, m.len)) { ret = -EFAULT; goto out; }

    scheduler_preempt_disable();
    int r = xhci_control((uint8_t)m.slot, m.setup, m.len ? buf : 0,
                         (uint16_t)m.len, m.in ? 1 : 0);
    scheduler_preempt_enable();

    if (r < 0) { ret = -EIO; goto out; }
    if (m.len && m.in &&
        !vmm_copy_to_user(c->pml4, m.buf, buf, m.len)) { ret = -EFAULT; goto out; }
    ret = (int64_t)m.len;
out:
    c->regs[14] = (uint64_t)ret;
    return 0;
}

int sys_usb_release(struct syscall_ctx *c) {
    int64_t ret = scheduler_current_mm()
        ? usb_claim_drop((uint8_t)c->a0, c->pml4,
                         (c->a1 & USB_RELEASE_REBIND) ? 1 : 0)
        : -EPERM;
    c->regs[14] = (uint64_t)ret;
    return 0;
}

// THE CLAIM DIES WITH THE ADDRESS SPACE, and does NOT rebind -- the
// PCI path's rule, for its reason: a driver under supervision is
// restarted and has to find its device free.
void usb_claim_space_gone(uint64_t pml4) {
    if (!pml4) return;
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_claims[i].pml4 == pml4)
            usb_claim_drop(g_claims[i].slot, 0, 0);
}
