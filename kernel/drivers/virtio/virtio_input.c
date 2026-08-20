// virtio-input: a keyboard, mouse or tablet on the virtio transport.
//
// THE DEVICE IS evdev ON A WIRE. Its events are literally Linux's
// `struct input_event` minus the timestamp -- (type, code, value), 8
// bytes -- which is why this driver is mostly plumbing: the input core
// speaks that vocabulary already (kernel/include/kernel/input.h), so
// there is no translation layer here at all. That is the payoff of
// having chosen evdev as the canonical form rather than AT scancodes.
//
// A QUEUE THE DEVICE DRIVES, which is the shape no virtio device here
// had before. Block, entropy and GPU are all request-response: the
// driver asks, the device answers, the driver waits. An event queue is
// the opposite -- the driver hands over a pile of EMPTY buffers and the
// device fills them when the user does something, which may be never.
// So completion is virtqueue_take() (non-blocking, "whatever is there")
// rather than virtqueue_poll(), and every buffer taken is immediately
// handed back, because a queue that runs out of buffers silently stops
// reporting input.
//
// CONFIG SPACE IS A WINDOW, not a struct: write `select` and `subsel`,
// then read what they name. That is how a device says whether it is a
// keyboard, a mouse or a tablet -- it does not say so directly, it says
// which EVENT TYPES it emits, and the answer is a bitmap whose SIZE
// being nonzero is the fact worth having.
#include "virtio.h"
#include "virtio_input.h"
#include "input.h"
#include "irq.h"
#include "pic.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

// --- the protocol (spec 5.8) ------------------------------------------

#define VIRTIO_INPUT_CFG_UNSET     0x00
#define VIRTIO_INPUT_CFG_ID_NAME   0x01
#define VIRTIO_INPUT_CFG_EV_BITS   0x11
#define VIRTIO_INPUT_CFG_ABS_INFO  0x12

// Offsets within the config window.
#define CFG_SELECT 0
#define CFG_SUBSEL 1
#define CFG_SIZE   2
#define CFG_UNION  8

// evdev event types, on the wire.
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03

#define REL_X     0
#define REL_Y     1
#define REL_WHEEL 8

#define ABS_X 0
#define ABS_Y 1

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};
_Static_assert(sizeof(struct virtio_input_event) == 8, "an input event is 8 bytes on the wire");

// How many empty buffers each device keeps posted. A burst of input --
// a fast mouse move is a REL_X, a REL_Y and an EV_SYN every few
// milliseconds -- must not outrun the drain, and the drain runs when
// the kernel is idle. 64 is comfortably more than one poll interval's
// worth and costs 512 bytes per device.
#define EVENT_BUFS 64

#define MAX_INPUT_DEVICES 4

struct input_dev {
    struct virtio_device vdev;
    struct virtqueue eventq;
    char name[24];

    // The buffers, and which one each descriptor is currently holding.
    // Needed because virtqueue_take() reports the DESCRIPTOR that came
    // back, not the buffer -- the descriptor pool hands out whatever is
    // free, so the two orders diverge after the first refill.
    struct virtio_input_event bufs[EVENT_BUFS] __attribute__((aligned(8)));
    uint16_t buf_of_head[VIRTQ_MAX_SIZE];

    uint32_t caps;
    uint8_t irq;              // the line it was routed to, 0 if polled
    uint8_t buttons;          // the mask this device is currently holding
    int abs_x, abs_y;         // last absolute position seen, per axis
    int abs_max_x, abs_max_y;
    int present;
};

static struct input_dev g_devs[MAX_INPUT_DEVICES];
static int g_count;
static uint32_t g_events;
static struct input_source g_sources[MAX_INPUT_DEVICES];

int virtio_input_count(void) { return g_count; }
uint32_t virtio_input_events(void) { return g_events; }

// --- config-space questions -------------------------------------------

// Selects a config window and returns its size. Zero means "this device
// does not have that", which is the whole interrogation protocol: a
// keyboard answers 0 for EV_REL, a mouse answers nonzero.
static uint8_t cfg_select(struct input_dev *d, uint8_t select, uint8_t subsel) {
    virtio_cfg_write8(&d->vdev, CFG_SELECT, select);
    virtio_cfg_write8(&d->vdev, CFG_SUBSEL, subsel);
    return virtio_cfg_read8(&d->vdev, CFG_SIZE);
}

static void read_name(struct input_dev *d) {
    uint8_t size = cfg_select(d, VIRTIO_INPUT_CFG_ID_NAME, 0);
    unsigned n = size;
    if (n >= sizeof d->name) n = sizeof d->name - 1;
    for (unsigned i = 0; i < n; i++) {
        d->name[i] = (char)virtio_cfg_read8(&d->vdev, CFG_UNION + i);
    }
    d->name[n] = 0;
    if (!d->name[0]) k_strlcpy(d->name, "virtio-input", sizeof d->name);
}

// An absolute axis reports its range through ABS_INFO: min, max, fuzz,
// flat, res. Only max is used here -- the pointer state scales against
// it (mouse_feed_abs), and a device reporting a zero range is treated as
// having no absolute axis at all rather than dividing by it.
static int read_abs_max(struct input_dev *d, uint8_t axis) {
    if (cfg_select(d, VIRTIO_INPUT_CFG_ABS_INFO, axis) < 8) return 0;
    uint32_t min = virtio_cfg_read32(&d->vdev, CFG_UNION + 0);
    uint32_t max = virtio_cfg_read32(&d->vdev, CFG_UNION + 4);
    if (max <= min) return 0;
    return (int)(max - min);
}

// --- one event --------------------------------------------------------

static void handle_event(struct input_dev *d, const struct virtio_input_event *ev) {
    switch (ev->type) {
        case EV_KEY:
            if (ev->code >= INPUT_BTN_LEFT && ev->code <= INPUT_BTN_MIDDLE) {
                // Buttons are reported as a MASK by the pointer state,
                // so the driver holds the mask and edits one bit --
                // otherwise a right-click would clear a held left one.
                uint8_t bit = (uint8_t)(1u << (ev->code - INPUT_BTN_LEFT));
                if (ev->value) d->buttons |= bit;
                else d->buttons &= (uint8_t)~bit;
                input_report_buttons(d->buttons);
            } else if (ev->value != 2) {
                // value 2 is AUTOREPEAT, which the device generates and
                // this kernel already does for itself in the console --
                // taking both would double every held key.
                input_report_key(ev->code, ev->value ? 1 : 0);
            }
            break;

        case EV_REL:
            // X and Y arrive as separate events, so each is reported on
            // its own with the other axis zero. That is correct for a
            // relative device (the motions add) and is why REL needs no
            // EV_SYN handling here.
            if (ev->code == REL_X) input_report_rel((int)(int32_t)ev->value, 0);
            else if (ev->code == REL_Y) input_report_rel(0, -(int)(int32_t)ev->value);
            else if (ev->code == REL_WHEEL) input_report_wheel((int)(int32_t)ev->value);
            break;

        case EV_ABS:
            // ABS is different: an axis on its own is half a position,
            // so the last value of each is kept and both are reported
            // together. A tablet that moved only in X still reports the
            // right Y because that is the Y it last had.
            if (ev->code == ABS_X) d->abs_x = (int)ev->value;
            else if (ev->code == ABS_Y) d->abs_y = (int)ev->value;
            else break;
            input_report_abs(d->abs_x, d->abs_y, d->abs_max_x, d->abs_max_y);
            break;

        case EV_SYN:
        default:
            break;
    }
    g_events++;
}

// --- the drain --------------------------------------------------------

static void post_buffer(struct input_dev *d, uint16_t buf_index) {
    struct virtio_sg in = {
        .phys = (uint64_t)(uintptr_t)&d->bufs[buf_index],
        .len = sizeof d->bufs[buf_index],
    };
    int head = virtqueue_submit(&d->eventq, 0, 0, &in, 1);
    if (head < 0) return;          // pool exhausted; the drain refills it
    d->buf_of_head[head] = buf_index;
}

static void drain(struct input_dev *d) {
    if (!d->present) return;

    int head = 0;
    uint32_t len = 0;
    int drained = 0;
    while (virtqueue_take(&d->eventq, &head, &len)) {
        uint16_t buf = d->buf_of_head[head];
        if (buf < EVENT_BUFS && len >= sizeof(struct virtio_input_event)) {
            handle_event(d, &d->bufs[buf]);
        }
        // Straight back to the device. A buffer kept for later is a
        // buffer the device cannot report into, and running out does not
        // fail loudly -- input simply stops arriving.
        post_buffer(d, buf);
        drained++;
    }
    if (drained) virtqueue_kick(&d->eventq);
}

// One poll function per device slot, because struct input_source carries
// no context pointer -- and it carries none deliberately: a void* would
// be the only field in it that could be wrong at runtime.
//
// These are the FALLBACK. A device with a usable interrupt line leaves
// input_source.poll NULL and is drained from the handler below instead;
// this path exists for a device the chipset routed nowhere, which is
// rare but is not something to leave silently dead.
static void poll_0(void) { drain(&g_devs[0]); }
static void poll_1(void) { drain(&g_devs[1]); }
static void poll_2(void) { drain(&g_devs[2]); }
static void poll_3(void) { drain(&g_devs[3]); }
static void (*const POLLS[MAX_INPUT_DEVICES])(void) = { poll_0, poll_1, poll_2, poll_3 };

// --- the interrupt ----------------------------------------------------
//
// ONE handler for every virtio-input device, registered once per line
// they occupy. It walks the claimed devices and asks each whether it
// was the source -- which on a shared, level-triggered INTx line is not
// optional: several of these functions routinely land on one line, and
// a device whose ISR is never read holds that line asserted and the
// interrupt repeats forever.
//
// The ISR read is what deasserts it, and it is DESTRUCTIVE, so it
// happens exactly once per device per interrupt.
//
// Safe here: draining touches only this device's ring and then the same
// keyboard/pointer state the PS/2 IRQ handlers already write from
// interrupt context. There is no lock because there is no sharing --
// an interrupt-driven device has no poll(), so the idle path never
// touches its queue.
static void input_irq_handler(uint64_t *regs) {
    (void)regs;
    for (int i = 0; i < g_count; i++) {
        struct input_dev *d = &g_devs[i];
        if (!d->present || !d->irq) continue;
        if (!(virtio_isr_read(&d->vdev) & VIRTIO_ISR_HAS_QUEUE)) continue;
        drain(d);
    }
}

// --- bring-up ---------------------------------------------------------

static int claim_one(int index, struct input_dev *d) {
    d->vdev.name = "virtio-input";
    if (!virtio_pci_find(VIRTIO_ID_INPUT, index, &d->vdev)) return 0;

    // No device-specific features are defined for virtio-input beyond
    // the transport's own, so nothing is asked for.
    if (!virtio_begin(&d->vdev, 0)) return 0;

    if (!virtqueue_setup(&d->vdev, 0, &d->eventq)) {
        klog_write("virtio-input: could not set up its event queue\n");
        virtio_fail(&d->vdev);
        return 0;
    }

    read_name(d);

    // What the device can emit. The bitmap contents are not inspected --
    // only whether there IS one -- because "does this device produce
    // relative motion?" is the question, and a size of zero answers it
    // without decoding a single bit.
    if (cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_KEY) > 0) d->caps |= INPUT_CAP_KEYS;
    if (cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_REL) > 0) d->caps |= INPUT_CAP_REL | INPUT_CAP_WHEEL;
    if (cfg_select(d, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS) > 0) {
        d->abs_max_x = read_abs_max(d, ABS_X);
        d->abs_max_y = read_abs_max(d, ABS_Y);
        // An absolute device whose axes have no range is not one this
        // can use -- reporting it as ABS would send every position
        // through a division by zero guard and produce nothing.
        if (d->abs_max_x > 0 && d->abs_max_y > 0) d->caps |= INPUT_CAP_ABS;
    }

    // DRIVER_OK before the queue is used, then fill it: the device may
    // start reporting the instant it has a buffer.
    virtio_driver_ok(&d->vdev);
    d->present = 1;

    for (uint16_t i = 0; i < EVENT_BUFS; i++) post_buffer(d, i);
    virtqueue_kick(&d->eventq);

    return 1;
}

void virtio_input_init(void) {
    for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
        struct input_dev *d = &g_devs[g_count];
        k_memset(d, 0, sizeof *d);
        if (!claim_one(i, d)) break;   // no more devices of this type

        // Interrupts if the chipset routed this function anywhere;
        // idle polling if it did not. Deciding per device rather than
        // per driver means one unrouted device does not cost the others
        // their interrupts, and no device is left with neither.
        d->irq = virtio_enable_intx(&d->vdev);
        if (d->irq) {
            irq_register_handler(d->irq, input_irq_handler);
            pic_clear_mask(d->irq);
        }

        g_sources[g_count].name = d->name;
        g_sources[g_count].caps = d->caps;
        g_sources[g_count].poll = d->irq ? 0 : POLLS[g_count];
        g_sources[g_count].irq = d->irq;
        input_register_source(&g_sources[g_count]);

        klog_printf("virtio-input: \"%s\" claimed (%s%s%s), %s\n", d->name,
                    (d->caps & INPUT_CAP_KEYS) ? "keys " : "",
                    (d->caps & INPUT_CAP_REL) ? "rel " : "",
                    (d->caps & INPUT_CAP_ABS) ? "abs" : "",
                    d->irq ? "IRQ-driven" : "polled (no interrupt line)");
        if (d->irq) klog_printf("virtio-input: \"%s\" on IRQ %u\n", d->name, d->irq);
        g_count++;
        if (g_count >= MAX_INPUT_DEVICES) break;
    }
}
