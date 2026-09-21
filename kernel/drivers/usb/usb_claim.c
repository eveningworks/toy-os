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

#define USB_CLASS_HUB 9   // usb_enum.c's, and the same value the spec gives

static struct {
    uint8_t  slot;      // 0 = free
    uint64_t pml4;
    int      pid;
} g_claims[USB_MAX_DEVICES];

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

// --- the syscalls ------------------------------------------------------

int sys_usb_claim(struct syscall_ctx *c) {
    int64_t ret = scheduler_current_mm()
        ? usb_claim_take((uint8_t)c->a0, c->pml4, scheduler_current_pid())
        : -EPERM;
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
