// The virtio modern PCI transport: find a device, map its register
// windows, negotiate features. Everything above this file (the
// virtqueue, and the device drivers on top of that) is device-agnostic
// because this one hides where the registers are.
//
// See kernel/include/kernel/virtio.h for what this deliberately does
// NOT implement, and why modern-only.
#include "virtio.h"
#include "pci.h"
#include "pci_internal.h"
#include "klog.h"
#include "kfmt.h"
#include "timer.h"

// driver-none: the shared virtio-over-PCI transport

// A window has to live somewhere the kernel can reach. The identity map
// covers the low 4 GiB and there is no paging_map_kernel_range(), so a
// BAR above that is unreachable rather than merely awkward -- refuse it
// with a log line instead of dereferencing a truncated pointer.
//
// Not reachable on QEMU's pc-i440fx (SeaBIOS assigns the 16 KiB virtio
// BAR inside the 32-bit hole), which is exactly why it is a check and a
// message rather than a TODO: the day it fires, the message says what
// to build.
#define VIRTIO_ADDR_LIMIT 0x100000000ull

// Reset is not guaranteed synchronous, so the spec requires reading
// device_status back until it reads 0. Bounded, because a device that
// never clears it must not hang the boot.
#define VIRTIO_RESET_TICKS 100

static inline uint8_t  mmio_r8 (volatile uint8_t *p, uint32_t off) { return *(volatile uint8_t  *)(p + off); }
static inline uint16_t mmio_r16(volatile uint8_t *p, uint32_t off) { return *(volatile uint16_t *)(p + off); }
static inline uint32_t mmio_r32(volatile uint8_t *p, uint32_t off) { return *(volatile uint32_t *)(p + off); }
static inline void mmio_w8 (volatile uint8_t *p, uint32_t off, uint8_t v)  { *(volatile uint8_t  *)(p + off) = v; }
static inline void mmio_w16(volatile uint8_t *p, uint32_t off, uint16_t v) { *(volatile uint16_t *)(p + off) = v; }
static inline void mmio_w32(volatile uint8_t *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p + off) = v; }

// The device type. A MODERN device encodes it in its PCI device id
// (0x1040 + type); a TRANSITIONAL one keeps the old id (0x1001 for
// block) and names its type in the PCI subsystem device id instead.
//
// Handling both is not optional politeness: QEMU's virtio-blk-pci
// defaults to disable-legacy=auto, so the most natural command line a
// user types -- `-drive if=virtio` on the default pc-i440fx machine --
// produces a TRANSITIONAL device. A driver matching only 0x1040+type
// finds nothing there and the feature appears silently broken.
static uint16_t device_type(const struct pci_device *d) {
    if (d->device_id >= 0x1040 && d->device_id <= 0x107F) return (uint16_t)(d->device_id - 0x1040);
    return pci_config_read16(d, 0x2E);  // subsystem device id
}

// Maps one capability's window. Returns NULL (having logged) if the
// capability names something unusable -- every check here is cheap and
// each one catches a real emulator/firmware mistake rather than a
// hypothetical.
static volatile uint8_t *map_window(const struct pci_device *d, uint8_t cap,
                                    const char *what, uint32_t *out_len) {
    uint8_t  bar_index = pci_config_read8(d, (uint8_t)(cap + VIRTIO_CAP_BAR));
    uint32_t offset    = pci_config_read32(d, (uint8_t)(cap + VIRTIO_CAP_OFFSET));
    uint32_t length    = pci_config_read32(d, (uint8_t)(cap + VIRTIO_CAP_LENGTH));

    if (bar_index > 5) {
        klog_printf("virtio: %s capability names BAR %u -- out of range\n", what, bar_index);
        return 0;
    }
    if (pci_bar_is_io(d->bar[bar_index])) {
        klog_printf("virtio: %s capability names BAR %u, which is I/O space\n", what, bar_index);
        return 0;
    }
    uint64_t base = pci_bar_mem_addr(d, bar_index);
    if (!base) {
        klog_printf("virtio: %s capability names BAR %u, which is unimplemented\n", what, bar_index);
        return 0;
    }
    // Overflow before the range check, or the check itself is wrong.
    if ((uint64_t)offset + (uint64_t)length > 0xFFFFFFFFull) {
        klog_printf("virtio: %s window offset+length overflows\n", what);
        return 0;
    }
    uint64_t addr = base + offset;
    if (addr + length > VIRTIO_ADDR_LIMIT) {
        // The one failure that would need real work to fix, so it says so.
        klog_printf("virtio: %s window at 0x%llx is above 4 GiB -- this kernel identity-maps"
                    " only the low 4 GiB and has no kernel-range mapper\n",
                    what, (unsigned long long)addr);
        return 0;
    }
    if (out_len) *out_len = length;
    return (volatile uint8_t *)(uintptr_t)addr;  // identity-mapped below 4 GiB
}

int virtio_pci_find(uint16_t type, int index, struct virtio_device *out) {
    if (!out) return 0;

    const struct pci_device *dev = 0;
    int seen = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d || d->vendor_id != VIRTIO_PCI_VENDOR) continue;
        if (device_type(d) != type) continue;
        if (seen++ == index) { dev = d; break; }
    }
    // No such device is the ordinary case and says nothing. Anything
    // below this point is a device that IS present and is being
    // refused, which must be readable.
    if (!dev) return 0;

    for (int i = 0; i < (int)sizeof *out; i++) ((uint8_t *)out)[i] = 0;
    out->pci = dev;
    out->type = type;

    // Walk the vendor-specific capabilities. They all share id 0x09 and
    // are told apart by cfg_type, which is why the walker takes a
    // resume offset -- asking for "the next 0x09" is the whole point.
    uint8_t cap = pci_capability_find(dev, PCI_CAP_ID_VNDR, 0);
    if (!cap) {
        klog_printf("virtio: device %04x:%04x has no vendor capabilities --"
                    " legacy-only, refusing (this transport is modern-only)\n",
                    dev->vendor_id, dev->device_id);
        return 0;
    }
    for (; cap; cap = pci_capability_find(dev, PCI_CAP_ID_VNDR, cap)) {
        uint8_t kind = pci_config_read8(dev, (uint8_t)(cap + VIRTIO_CAP_CFG_TYPE));
        switch (kind) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                if (!out->common) out->common = map_window(dev, cap, "common", 0);
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                if (!out->notify) {
                    out->notify = map_window(dev, cap, "notify", &out->notify_len);
                    out->notify_off_multiplier =
                        pci_config_read32(dev, (uint8_t)(cap + VIRTIO_CAP_NOTIFY_MULT));
                }
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                if (!out->isr) out->isr = map_window(dev, cap, "isr", 0);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                if (!out->cfg) out->cfg = map_window(dev, cap, "device", &out->cfg_len);
                break;
            default: break;  // PCI_CFG (5) is a config-space backdoor we don't need
        }
    }

    if (!out->common || !out->notify || !out->isr) {
        klog_printf("virtio: device %04x:%04x is missing a required window"
                    " (common=%d notify=%d isr=%d)\n",
                    dev->vendor_id, dev->device_id,
                    out->common ? 1 : 0, out->notify ? 1 : 0, out->isr ? 1 : 0);
        return 0;
    }

    // Memory decode and DMA, and INTx off by DEFAULT because most of
    // these drivers poll. A driver that wants interrupts calls
    // virtio_enable_intx() to clear the bit -- opt-in, because the
    // failure mode of getting it wrong is not a missed interrupt, it is
    // a line asserted forever with nobody to clear it.
    // INTx is level-triggered and shared: a device left free to assert
    // it with no handler installed holds the line down for everything
    // else on it.
    pci_command_update(dev, PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE, 0);
    return 1;
}

// Reports the line the chipset routed this function to, or 0 if there
// is none to use. NO SIDE EFFECTS -- enabling is virtio_intx_enable()
// below, and virtio.h says why the two are separate.
//
// The caller must install an irq handler that READS THE ISR REGISTER --
// the read is what deasserts a level-triggered line, and without it the
// first interrupt never ends. Returning the line rather than
// registering the handler here keeps this file free of any opinion
// about how a driver services its device.
uint8_t virtio_intx_line(const struct virtio_device *d) {
    if (!d || !d->pci) return 0;
    uint8_t line = d->pci->interrupt_line;
    // 0xFF is the PCI convention for "not connected", and 0 is IRQ0 --
    // the timer, which no PCI device is routed to. Either means there
    // is nothing to enable, and enabling anyway would leave the device
    // free to assert a line nobody listens on.
    if (line == 0xFF || line == 0 || line >= 16) return 0;
    return line;
}

// The commit point: after this the device may assert its line, so
// everything its handler reaches for has to be in place already.
void virtio_intx_enable(struct virtio_device *d) {
    if (!d || !d->pci) return;
    pci_command_update(d->pci, 0, PCI_CMD_INTX_DISABLE);
}

// Reads (and thereby CLEARS) the ISR status byte. Bit 0 means "one of
// my queues has activity"; bit 1 means the device configuration
// changed. On a shared line this is how a handler answers "was it me?"
// -- and it must be called exactly once per interrupt per device,
// because the read is destructive.
uint8_t virtio_isr_read(const struct virtio_device *d) {
    if (!d || !d->isr) return 0;
    return *(volatile uint8_t *)d->isr;
}

static void status_set(struct virtio_device *d, uint8_t bits) {
    uint8_t now = mmio_r8(d->common, VIRTIO_COMMON_STATUS);
    mmio_w8(d->common, VIRTIO_COMMON_STATUS, (uint8_t)(now | bits));
}

void virtio_fail(struct virtio_device *d) { if (d && d->common) status_set(d, VIRTIO_STATUS_FAILED); }
void virtio_driver_ok(struct virtio_device *d) { if (d && d->common) status_set(d, VIRTIO_STATUS_DRIVER_OK); }

int virtio_has_feature(const struct virtio_device *d, uint64_t bit) {
    return d && (d->features & bit) ? 1 : 0;
}

static uint64_t read_device_features(struct virtio_device *d) {
    mmio_w32(d->common, VIRTIO_COMMON_DFSELECT, 0);
    uint64_t lo = mmio_r32(d->common, VIRTIO_COMMON_DF);
    mmio_w32(d->common, VIRTIO_COMMON_DFSELECT, 1);
    uint64_t hi = mmio_r32(d->common, VIRTIO_COMMON_DF);
    return (hi << 32) | lo;
}

static void write_driver_features(struct virtio_device *d, uint64_t f) {
    mmio_w32(d->common, VIRTIO_COMMON_GFSELECT, 0);
    mmio_w32(d->common, VIRTIO_COMMON_GF, (uint32_t)(f & 0xFFFFFFFFu));
    mmio_w32(d->common, VIRTIO_COMMON_GFSELECT, 1);
    mmio_w32(d->common, VIRTIO_COMMON_GF, (uint32_t)(f >> 32));
}

int virtio_begin(struct virtio_device *d, uint64_t wanted) {
    if (!d || !d->common) return 0;

    // 1. Reset, and WAIT for it. Writing 0 asks; reading 0 back is the
    //    device agreeing. Skipping the read-back works on QEMU and
    //    hangs on hardware, which is the worst possible split.
    mmio_w8(d->common, VIRTIO_COMMON_STATUS, 0);
    uint32_t start = pit_ticks();
    while (mmio_r8(d->common, VIRTIO_COMMON_STATUS) != 0) {
        if (pit_ticks() - start > VIRTIO_RESET_TICKS) {
            klog_printf("virtio: %s did not acknowledge reset\n", d->name ? d->name : "device");
            return 0;
        }
    }

    // 2. Announce ourselves.
    status_set(d, VIRTIO_STATUS_ACKNOWLEDGE);
    status_set(d, VIRTIO_STATUS_DRIVER);

    // 3. What does it offer?
    uint64_t offered = read_device_features(d);

    // 4. VERSION_1 is the modern transport. Without it the device speaks
    //    only the legacy I/O-port layout, which this file does not
    //    implement -- refuse rather than half-drive it. Single place the
    //    modern-only decision lives.
    if (!(offered & VIRTIO_F_VERSION_1)) {
        klog_printf("virtio: %s offers no VERSION_1 -- legacy-only, refusing\n",
                    d->name ? d->name : "device");
        virtio_fail(d);
        return 0;
    }

    // 5. Take what we asked for and VERSION_1, and nothing else. Not
    //    negotiating EVENT_IDX is what keeps the store-load fence
    //    unreachable (see barrier.h); not negotiating INDIRECT_DESC is
    //    what keeps a chain a plain chain.
    uint64_t negotiated = offered & (wanted | VIRTIO_F_VERSION_1);
    write_driver_features(d, negotiated);

    // 6. Say we are done choosing...
    status_set(d, VIRTIO_STATUS_FEATURES_OK);

    // 7. ...and CHECK IT AGREED. Mandatory in the spec and skipped by
    //    most hobby drivers. What it catches is the device silently
    //    running in a mode the driver did not ask for, which surfaces
    //    much later as wrong data rather than as a failure here.
    //
    //    Honest note: QEMU always accepts our subset, so deleting this
    //    check turns nothing red in this repo's tests. It is spec
    //    conformance for devices we do not have.
    uint8_t back = mmio_r8(d->common, VIRTIO_COMMON_STATUS);
    if (!(back & VIRTIO_STATUS_FEATURES_OK)) {
        klog_printf("virtio: %s rejected the negotiated feature set\n",
                    d->name ? d->name : "device");
        virtio_fail(d);
        return 0;
    }

    d->features = negotiated;
    return 1;
}

// Device-config reads. The 64-bit one is TWO 32-bit reads rather than a
// single 64-bit access: the spec permits a device to declare a maximum
// MMIO access width, and a wider access to such a window silently does
// not work. Low half first, because a device may latch on the high one.
// The device-configuration space is READ-ONLY for most devices, and
// this exists because virtio-input's is not: its config space is a
// WINDOW, selected by writing `select`/`subsel` and then reading back
// whatever those name (the device's name, its supported event types,
// an axis range). Without a write there is no way to ask the second
// question. Spec 5.8.5.
void virtio_cfg_write8(const struct virtio_device *d, uint32_t off, uint8_t v) {
    if (!d || !d->cfg || off >= d->cfg_len) return;
    *(volatile uint8_t *)(d->cfg + off) = v;
}

uint8_t virtio_cfg_read8(const struct virtio_device *d, uint32_t off) {
    if (!d || !d->cfg || off >= d->cfg_len) return 0;
    return mmio_r8(d->cfg, off);
}
uint16_t virtio_cfg_read16(const struct virtio_device *d, uint32_t off) {
    if (!d || !d->cfg || off + 2 > d->cfg_len) return 0;
    return mmio_r16(d->cfg, off);
}
uint32_t virtio_cfg_read32(const struct virtio_device *d, uint32_t off) {
    if (!d || !d->cfg || off + 4 > d->cfg_len) return 0;
    return mmio_r32(d->cfg, off);
}
uint64_t virtio_cfg_read64(const struct virtio_device *d, uint32_t off) {
    if (!d || !d->cfg || off + 8 > d->cfg_len) return 0;
    uint64_t lo = mmio_r32(d->cfg, off);
    uint64_t hi = mmio_r32(d->cfg, off + 4);
    return (hi << 32) | lo;
}
