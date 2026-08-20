#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include "pci.h"

// virtio: one transport, many devices.
//
// A virtio device is not real hardware -- it is an interface the
// hypervisor implements, designed so a guest can move bulk data without
// the register-poke-per-byte cost of emulating a real chip. Every
// virtio device (block, net, GPU, entropy, input) speaks the SAME
// transport: the same PCI discovery, the same feature negotiation, the
// same ring structure. Only the payload differs.
//
// That is why this file exists rather than the mechanics being written
// out inside a disk driver. Linux splits it the same way
// (drivers/virtio/ under the device drivers that use it) and so does
// Windows' virtio-win (one VirtIOLib linked into viostor and netkvm).
//
// WHAT THIS DELIBERATELY IS NOT
// -----------------------------
// It follows the SHAPE of those, not their size. There is no bus type,
// no driver-match table, no probe/remove callbacks, no vtable over
// several transports, no packed-ring support, no indirect descriptors,
// no event-index suppression, no MSI-X, no DMA/IOMMU layer and no
// hotplug. A driver here scans PCI in its own init and calls these
// functions directly -- vmsvga.c's precedent, and the right size for a
// kernel with one virtio device.
//
// MODERN ONLY (virtio 1.x)
// ------------------------
// The transport is the modern one: registers are found by walking the
// PCI capability list into MMIO BARs. The legacy 0.9.5 I/O-port layout
// is not implemented, and a device that does not offer
// VIRTIO_F_VERSION_1 is refused in virtio_begin() rather than half
// supported. A second, unexercised code path is an unvalidated one.
//
// Note this does NOT mean only modern DEVICES work: QEMU's default
// (disable-legacy=auto on pc-i440fx) produces a TRANSITIONAL device,
// which speaks both. It is claimed here through its modern half.

#define VIRTIO_PCI_VENDOR 0x1AF4

// Device types, from the spec's device-id assignment.
#define VIRTIO_ID_NET   1
#define VIRTIO_ID_BLK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_RNG   4
#define VIRTIO_ID_GPU  16
#define VIRTIO_ID_INPUT 18

// --- device status (spec 2.1) ----------------------------------------
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED      0x80

// --- transport feature bits ------------------------------------------
//
// The feature space is 64 bits wide and VIRTIO_F_VERSION_1 lives at bit
// 32, which is why every feature word in this file is uint64_t. A
// uint32_t here silently loses the one bit that decides whether the
// modern transport is available at all.
#define VIRTIO_F_INDIRECT_DESC (1ull << 28)
#define VIRTIO_F_EVENT_IDX     (1ull << 29)
#define VIRTIO_F_VERSION_1     (1ull << 32)

// --- PCI capability layout (spec 4.1.4) ------------------------------
//
// virtio publishes its register windows as several vendor-specific
// (0x09) PCI capabilities. They are told apart by cfg_type, NOT by the
// capability id -- which is why pci_capability_find() takes a resume
// offset, so one id can be walked repeatedly.
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define VIRTIO_PCI_CAP_PCI_CFG    5

// Offsets within a virtio_pci_cap, from the start of the capability.
#define VIRTIO_CAP_CFG_TYPE 3
#define VIRTIO_CAP_BAR      4
#define VIRTIO_CAP_OFFSET   8
#define VIRTIO_CAP_LENGTH   12
#define VIRTIO_CAP_NOTIFY_MULT 16   // NOTIFY_CFG only

// --- common configuration register map (spec 4.1.4.3) ----------------
// Offsets within the COMMON_CFG window.
#define VIRTIO_COMMON_DFSELECT   0x00  // u32 device_feature_select
#define VIRTIO_COMMON_DF         0x04  // u32 device_feature   (ro)
#define VIRTIO_COMMON_GFSELECT   0x08  // u32 driver_feature_select
#define VIRTIO_COMMON_GF         0x0C  // u32 driver_feature
#define VIRTIO_COMMON_MSIX_CFG   0x10  // u16
#define VIRTIO_COMMON_NUMQ       0x12  // u16 num_queues       (ro)
#define VIRTIO_COMMON_STATUS     0x14  // u8  device_status
#define VIRTIO_COMMON_CFGGEN     0x15  // u8  config_generation (ro)
#define VIRTIO_COMMON_Q_SELECT   0x16  // u16 queue_select
#define VIRTIO_COMMON_Q_SIZE     0x18  // u16 queue_size
#define VIRTIO_COMMON_Q_MSIX     0x1A  // u16
#define VIRTIO_COMMON_Q_ENABLE   0x1C  // u16 queue_enable
#define VIRTIO_COMMON_Q_NOFF     0x1E  // u16 queue_notify_off  (ro)
#define VIRTIO_COMMON_Q_DESC     0x20  // u64 queue_desc
#define VIRTIO_COMMON_Q_DRIVER   0x28  // u64 queue_driver (avail)
#define VIRTIO_COMMON_Q_DEVICE   0x30  // u64 queue_device (used)

// ISR status bits (the ISR_CFG window is one byte, read-to-clear).
#define VIRTIO_ISR_QUEUE  0x1
#define VIRTIO_ISR_CONFIG 0x2

// --- the split virtqueue, on the wire (spec 2.7) ---------------------
//
// These three structures are SHARED MEMORY with the device: their
// layout is the protocol, not an implementation choice, and every field
// is little-endian (which x86-64 already is, so no swapping here).

#define VRING_DESC_F_NEXT     1  // this descriptor chains to .next
#define VRING_DESC_F_WRITE    2  // the DEVICE writes it (we read it)
#define VRING_DESC_F_INDIRECT 4  // not negotiated here

// One buffer. Descriptors are a free POOL, not a queue -- a request is
// a CHAIN of them linked by .next, and the chain's head index is what
// gets published in the available ring.
struct vring_desc {
    uint64_t addr;    // physical address of the buffer
    uint32_t len;
    uint16_t flags;   // VRING_DESC_F_*
    uint16_t next;    // only meaningful with VRING_DESC_F_NEXT
};

// Driver -> device. `idx` is a free-running counter that is never
// reset: it is taken modulo the queue size to index ring[], so
// wraparound costs nothing and needs no separate "empty vs full" flag.
struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];  // ring[idx % size] = head descriptor index
    // a uint16_t used_event follows when VIRTIO_F_EVENT_IDX is
    // negotiated; the space is allocated regardless (spec 2.7.6).
};

struct vring_used_elem {
    uint32_t id;   // head descriptor index of the completed chain
    uint32_t len;  // bytes the DEVICE wrote into that chain
};

// Device -> driver. Same free-running `idx` rule as avail.
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
    // a uint16_t avail_event follows -- see vring_avail.
};

// The wire layout is fixed by the spec, so its sizes are a COMPILE-TIME
// fact and belong in a static assert rather than a KTEST: this fires at
// build time, costs nothing to run, and cannot be skipped on a machine
// that happens to lack a virtio device.
_Static_assert(sizeof(struct vring_desc) == 16, "vring_desc is 16 bytes on the wire");
_Static_assert(sizeof(struct vring_used_elem) == 8, "vring_used_elem is 8 bytes on the wire");


// --- a claimed device -------------------------------------------------

struct virtio_device {
    const struct pci_device *pci;
    const char *name;           // "virtio-blk" -- for log lines only
    uint16_t type;              // VIRTIO_ID_*
    volatile uint8_t *common;   // COMMON_CFG window
    volatile uint8_t *notify;   // NOTIFY_CFG window base
    volatile uint8_t *isr;      // ISR_CFG window
    volatile uint8_t *cfg;      // DEVICE_CFG window, may be NULL
    uint32_t cfg_len;
    uint32_t notify_len;        // for bounds-checking a doorbell
    uint32_t notify_off_multiplier;
    uint64_t features;          // NEGOTIATED; valid only after virtio_begin()
};

// Finds the `index`-th virtio device of type `type` and maps its modern
// capability windows.
//
// Returns 0 when there is none -- the ORDINARY case on every
// configuration this OS boots by default, not an error, and it logs
// NOTHING (vmsvga_init()'s "the ordinary -vga std case" rule). It logs
// and returns 0 when a device of that type IS present but its modern
// capabilities are missing or unusable, because that is a real refusal
// and a silent one would be unreadable in dmesg.
int virtio_pci_find(uint16_t type, int index, struct virtio_device *out);

// Reset -> ACKNOWLEDGE -> DRIVER -> negotiate -> FEATURES_OK, including
// the spec-mandated FEATURES_OK re-read. `wanted` is the driver's mask
// of DEVICE-specific bits it understands; VIRTIO_F_VERSION_1 is added
// here, and its absence from the device's offer is a hard refusal --
// the one place the modern-only decision is enforced.
//
// 1 on success. 0 after setting FAILED and logging why.
int virtio_begin(struct virtio_device *d, uint64_t wanted);

// status |= DRIVER_OK. Call after every queue is set up: the spec
// forbids using a queue before this.
void virtio_driver_ok(struct virtio_device *d);

// status |= FAILED, for an abort after virtio_begin() succeeded.
void virtio_fail(struct virtio_device *d);

int virtio_has_feature(const struct virtio_device *d, uint64_t bit);

// --- interrupts (legacy INTx) -----------------------------------------
//
// OPT-IN, and the default is off. virtio_pci_find() sets
// PCI_CMD_INTX_DISABLE for every device it claims, because a device
// left free to assert a LEVEL-triggered line with no handler to service
// it holds that line down and the PIC re-delivers it forever -- a storm
// that presents as a hang. Block, entropy and GPU poll and stay that
// way; virtio-input asks for interrupts, because an event queue has
// nothing to poll FOR.
//
// NOT MSI-X, and the reason is not virtio's: MSI is delivered as a
// memory write to the Local APIC, and this kernel has none (the PIC is
// all there is -- kernel/arch/x86_64/irq.c). See docs/roadmap.md.
//
// Returns the IRQ line the chipset routed this function to, or 0 when
// there is none usable. The caller installs the handler itself, and
// that handler MUST read the ISR -- see virtio_isr_read().
uint8_t virtio_enable_intx(struct virtio_device *d);

#define VIRTIO_ISR_HAS_QUEUE  0x1
#define VIRTIO_ISR_HAS_CONFIG 0x2

// Reads and CLEARS the ISR status byte -- destructive, so exactly once
// per interrupt per device. On a shared line, a zero result means this
// device was not the source and the handler should do nothing.
uint8_t virtio_isr_read(const struct virtio_device *d);

// Device-configuration space (the DEVICE_CFG window). A 64-bit read is
// TWO 32-bit reads, low half first -- see virtio_pci.c.
// Writes one byte of device-configuration space. Needed only by a
// device whose config space is a selectable WINDOW rather than a plain
// struct -- virtio-input, whose `select`/`subsel` bytes choose what the
// rest of the window then reports. See virtio_pci.c.
void virtio_cfg_write8(const struct virtio_device *d, uint32_t off, uint8_t v);

uint8_t  virtio_cfg_read8 (const struct virtio_device *d, uint32_t off);
uint16_t virtio_cfg_read16(const struct virtio_device *d, uint32_t off);
uint32_t virtio_cfg_read32(const struct virtio_device *d, uint32_t off);
uint64_t virtio_cfg_read64(const struct virtio_device *d, uint32_t off);


// --- virtqueues -------------------------------------------------------

// The largest queue this driver will use. Modern virtio explicitly lets
// a driver SHRINK a queue by writing a smaller size back, so a device
// offering more is clamped rather than refused. QEMU's default is 256,
// so this never fires today -- what it buys is a bounded allocation
// (3 frames) instead of "whatever the device asked for".
#define VIRTQ_MAX_SIZE 256

struct virtqueue {
    struct virtio_device *vdev;
    uint16_t index;
    uint16_t size;

    volatile struct vring_desc  *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used  *used;
    volatile uint16_t *notify;   // precomputed doorbell

    uint64_t ring_phys;          // pmm_alloc_contiguous() base
    uint32_t ring_frames;

    uint16_t free_head;          // head of the free-descriptor list
    uint16_t num_free;
    uint16_t avail_shadow;       // our copy of avail->idx
    uint16_t last_used;          // our copy of used->idx
};

// One buffer in a request. `phys` must be a physical address the device
// can reach; below 4 GiB that is the same as the kernel virtual address,
// since this kernel identity-maps that range.
struct virtio_sg {
    uint64_t phys;
    uint32_t len;
};

// Allocates queue `index`'s rings and tells the device where they are.
// Returns 0 if the device reports queue size 0 (no such queue) or the
// contiguous allocation fails.
int virtqueue_setup(struct virtio_device *d, uint16_t index, struct virtqueue *vq);
void virtqueue_teardown(struct virtqueue *vq);

// Chains n_out device-READABLE buffers then n_in device-WRITABLE ones
// into one descriptor chain and publishes it. Returns the chain's head
// descriptor index, or -1 when there are not enough free descriptors.
// Does NOT notify the device -- call virtqueue_kick().
int  virtqueue_submit(struct virtqueue *vq,
                      const struct virtio_sg *out, int n_out,
                      const struct virtio_sg *in,  int n_in);

// Rings the doorbell for whatever has been submitted.
void virtqueue_kick(struct virtqueue *vq);

// Spins until the chain headed by `head` is retired. 1 on completion
// (*used_len, when non-NULL, gets the byte count the DEVICE reported),
// 0 on timeout -- after which the chain's descriptors are deliberately
// NOT reclaimed; see virtqueue.c.
int  virtqueue_poll(struct virtqueue *vq, int head, uint32_t *used_len);

// NON-BLOCKING completion, for a queue the DEVICE drives.
//
// virtqueue_poll() answers "has MY chain come back?", which is the
// request-response shape every virtio device here has had so far: the
// driver asks, the device answers, the driver waits. An input event
// queue is the other shape -- the driver hands the device a pile of
// empty buffers and the device fills them WHENEVER SOMETHING HAPPENS,
// which may be never. There is no chain to wait for, and waiting would
// be waiting for the user to press a key.
//
// So this takes whatever is there. Returns 1 and fills *out_head (the
// retired chain, whose descriptors are returned to the pool) and
// *out_len (bytes the device wrote), or 0 when the used ring is empty.
// Call it in a loop until it returns 0.
int virtqueue_take(struct virtqueue *vq, int *out_head, uint32_t *out_len);

// Free descriptors currently available. Exported so a test can assert
// the pool BALANCES across many transfers -- a chain that is never
// returned leaks silently and only kills the driver hours later.
uint16_t virtqueue_free_count(const struct virtqueue *vq);

// Chains abandoned by a timeout, cumulative. Nonzero means the device
// stopped answering at some point and the queue is running down.
uint32_t virtqueue_lost_chains(void);

#endif
