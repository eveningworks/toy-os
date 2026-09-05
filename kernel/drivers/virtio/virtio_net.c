// virtio-net: two queues, a 12-byte header, and nothing else.
//
// Queue 0 is receive and queue 1 is transmit -- the driver hands the
// device empty buffers on one and full ones on the other, which is
// virtio_input.c's event-queue shape in both directions rather than
// virtio_blk.c's request-response.
//
// THE HEADER IS ALWAYS 12 BYTES HERE. In legacy virtio it is 10 unless
// VIRTIO_NET_F_MRG_RXBUF was negotiated; under VIRTIO_F_VERSION_1 --
// which virtio_begin() requires, so this driver has it by construction
// -- num_buffers is always present. That is why no feature bit is
// consulted before skipping it, and why getting it wrong would show up
// as every frame being received two bytes shifted.
//
// NO OFFLOADS, DELIBERATELY. Checksum and segmentation offload are
// exactly what virtio-net is good at, and negotiating any of them
// means the stack must handle a frame whose checksum is not filled in
// yet (VIRTIO_NET_F_CSUM) or one larger than the MTU (GSO). Neither is
// a saving worth having before there is a TCP to be fast at.
#include "virtio.h"
#include "virtio_net.h"
#include "irq.h"
#include "pic.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "string.h"
#include "errno.h"

// driver-none: the virtio transport half; net_virtio.c declares the driver

#define VIRTIO_ID_NET      1
#define VIRTIO_NET_F_MAC   (1ull << 5)

#define NET_CFG_MAC        0     // config-space offset of the 6 MAC bytes

#define RX_BUFS   16
#define TX_BUFS   8
#define BUF_SIZE  2048           // header + a 1518-byte frame, rounded up

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

_Static_assert(sizeof(struct virtio_net_hdr) == 12, "virtio-net header is 12 bytes under VERSION_1");

static struct virtio_device g_vdev;
static struct virtqueue g_rxq, g_txq;
static int g_present;
static uint8_t g_mac[6];
static void (*g_sink)(const void *, uint32_t);

// DMA buffers as statics: the kernel image is identity-mapped below
// 4 GiB, so a static's address is its physical address (the same
// assumption virtio_input.c's event buffers make, and the reason a
// kernel STACK address would not do).
static uint8_t g_rx_buf[RX_BUFS][BUF_SIZE];
static uint8_t g_tx_buf[TX_BUFS][BUF_SIZE];
static uint16_t g_rx_of_head[VIRTQ_MAX_SIZE];
static uint16_t g_tx_of_head[VIRTQ_MAX_SIZE];
static uint8_t g_tx_busy[TX_BUFS];

int virtio_net_present(void) { return g_present; }
const uint8_t *virtio_net_mac(void) { return g_mac; }
void virtio_net_set_rx(void (*sink)(const void *, uint32_t)) { g_sink = sink; }

static void post_rx(uint16_t buf) {
    struct virtio_sg in = { .phys = (uint64_t)(uintptr_t)g_rx_buf[buf], .len = BUF_SIZE };
    int head = virtqueue_submit(&g_rxq, 0, 0, &in, 1);
    if (head < 0) return;   // the pool refills as chains retire
    g_rx_of_head[head] = buf;
}

// Retire finished transmits. Called before every send rather than from
// an interrupt: the transmit queue's completions carry no information
// the stack wants, only buffers to reuse.
static void reap_tx(void) {
    int head = 0;
    uint32_t len = 0;
    while (virtqueue_take(&g_txq, &head, &len)) {
        uint16_t buf = g_tx_of_head[head];
        if (buf < TX_BUFS) g_tx_busy[buf] = 0;
    }
}

void virtio_net_drain(void) {
    if (!g_present) return;

    int head = 0;
    uint32_t len = 0;
    int drained = 0;
    while (virtqueue_take(&g_rxq, &head, &len)) {
        uint16_t buf = g_rx_of_head[head];
        if (buf < RX_BUFS && len > sizeof(struct virtio_net_hdr) && g_sink)
            g_sink(g_rx_buf[buf] + sizeof(struct virtio_net_hdr),
                   len - (uint32_t)sizeof(struct virtio_net_hdr));
        post_rx(buf);
        drained++;
    }
    if (drained) virtqueue_kick(&g_rxq);
}

int virtio_net_transmit(const void *frame, uint32_t len) {
    if (!g_present) return -ENODEV;
    if (len + sizeof(struct virtio_net_hdr) > BUF_SIZE) return -EINVAL;

    reap_tx();

    int buf = -1;
    for (int i = 0; i < TX_BUFS; i++) if (!g_tx_busy[i]) { buf = i; break; }
    if (buf < 0) return -ENOSPC;

    k_memset(g_tx_buf[buf], 0, sizeof(struct virtio_net_hdr));
    k_memcpy(g_tx_buf[buf] + sizeof(struct virtio_net_hdr), frame, len);

    struct virtio_sg out = {
        .phys = (uint64_t)(uintptr_t)g_tx_buf[buf],
        .len = (uint32_t)sizeof(struct virtio_net_hdr) + len,
    };
    int head = virtqueue_submit(&g_txq, &out, 1, 0, 0);
    if (head < 0) return -ENOSPC;

    g_tx_of_head[head] = (uint16_t)buf;
    g_tx_busy[buf] = 1;
    virtqueue_kick(&g_txq);
    return 0;
}

static void net_irq(uint64_t *regs) {
    (void)regs;
    if (!g_present) return;
    if (!virtio_irq_is_ours(&g_vdev)) return;
    virtio_net_drain();
}

void virtio_net_attach(const struct pci_device *pci) {
    if (g_vdev.pci) {
        klog_printf("virtio-net: a second device at %02x:%02x.%u -- one is driven\n",
                    pci->bus, pci->device, pci->function);
        return;
    }
    if (!virtio_pci_attach(pci, VIRTIO_ID_NET, &g_vdev)) return;
    if (!virtio_begin(&g_vdev, VIRTIO_NET_F_MAC)) return;

    // No MAC offered means the device expects the driver to invent one.
    // Refuse instead: an address nobody assigned is one the host's
    // filtering has never heard of, and the failure would be silent.
    if (!virtio_has_feature(&g_vdev, VIRTIO_NET_F_MAC)) {
        klog_write("virtio-net: device offers no MAC address -- refusing\n");
        virtio_fail(&g_vdev);
        return;
    }
    for (int i = 0; i < 6; i++)
        g_mac[i] = virtio_cfg_read8(&g_vdev, NET_CFG_MAC + (uint32_t)i);

    // MSI-X BEFORE THE QUEUES: virtqueue_setup() is what writes each
    // queue's table entry, so a later call would leave both queues
    // notifying nothing. Nothing can be delivered yet -- no queue is
    // enabled and DRIVER_OK is not set.
    int msix = virtio_msix_enable(&g_vdev, net_irq);

    if (!virtqueue_setup(&g_vdev, 0, &g_rxq) || !virtqueue_setup(&g_vdev, 1, &g_txq)) {
        klog_write("virtio-net: could not set up the receive/transmit queues\n");
        virtio_fail(&g_vdev);
        return;
    }

    for (uint16_t i = 0; i < RX_BUFS; i++) post_rx(i);
    virtqueue_kick(&g_rxq);

    // Publish first, arm last: a device that can interrupt before its
    // handler can see a built driver hangs the machine, and only under
    // KVM (see CLAUDE.md's virtio interrupt rule). With MSI-X the arming
    // write is DRIVER_OK, not the INTx enable, so g_present has to be
    // set before it rather than after.
    g_present = 1;
    virtio_driver_ok(&g_vdev);

    if (!msix) {
        uint8_t line = virtio_intx_line(&g_vdev);
        if (line) {
            irq_register_handler(line, net_irq);
            pic_clear_mask(line);
            virtio_intx_enable(&g_vdev);
        }
    }
}
