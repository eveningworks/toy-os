// Bochs DISPI (QEMU `-vga std` / `bochs-display`, VirtualBox VBoxVGA)
// -- see bochs.h for why this driver exists at all.
//
// The register interface is two I/O ports: write an index to 0x01CE,
// then read or write the 16-bit value at 0x01CF. That is the whole
// device programming model; there is no FIFO, no command ring, and
// nothing to flush, because the adapter scans the framebuffer
// continuously exactly as vesafb's does. What it adds over vesafb is the
// one thing vesafb cannot do: program a mode.
//
// Numbers below are from the Bochs VBE interface (`vbe_display_api`),
// which is what QEMU's hw/display/vga.c implements and what Linux's
// bochs-drm drives.
#include "bochs.h"
#include "display.h"
#include "pci.h"
#include "klog.h"
#include "kfmt.h"
#include "io.h"
#include "driver.h" // DRIVER_REGISTER -- `lsdrv -v` names THIS file

#define DISPI_IOPORT_INDEX 0x01CE
#define DISPI_IOPORT_DATA  0x01CF

#define DISPI_INDEX_ID              0x0
#define DISPI_INDEX_XRES            0x1
#define DISPI_INDEX_YRES            0x2
#define DISPI_INDEX_BPP             0x3
#define DISPI_INDEX_ENABLE          0x4
#define DISPI_INDEX_BANK            0x5
#define DISPI_INDEX_VIRT_WIDTH      0x6
#define DISPI_INDEX_VIRT_HEIGHT     0x7
#define DISPI_INDEX_X_OFFSET        0x8
#define DISPI_INDEX_Y_OFFSET        0x9
// Video memory in 64 KiB units. THE REAL BOUND ON A MODE HERE -- see
// bochs_try_mode(). QEMU's stdvga defaults to 16 MiB (`vgamem_mb`),
// which holds 1920x1080x4 (8.3 MiB) and does NOT hold 3840x2160x4
// (33.2 MiB).
#define DISPI_INDEX_VIDEO_MEMORY_64K 0xa

#define DISPI_ID0 0xB0C0u
#define DISPI_ID5 0xB0C5u

#define DISPI_DISABLED    0x00u
#define DISPI_ENABLED     0x01u
// Turns XRES/YRES/BPP into "report the MAXIMUM you support" for as long
// as it is set, instead of "this is the mode I want".
#define DISPI_GETCAPS     0x02u
#define DISPI_LFB_ENABLED 0x40u
// Without this the adapter blanks all of video memory on every mode set.
// Not merely wasteful: a mode set that clears memory the console has
// already drawn into loses the boot log.
#define DISPI_NOCLEARMEM  0x80u

// QEMU's stdvga and bochs-display both present as this; VirtualBox's
// VBoxVGA is the second pair. Matched by ID rather than by PCI class,
// because a VGA-compatible class code says nothing about whether the
// DISPI window is there.
#define QEMU_VENDOR 0x1234
#define QEMU_DEVICE 0x1111
#define VBOX_VENDOR 0x80ee
#define VBOX_DEVICE 0xbeef

static struct display_surface g_surface;
static uint64_t g_fb;      // BAR0, the linear framebuffer
static int g_active;

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_IOPORT_INDEX, index);
    outw(DISPI_IOPORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_IOPORT_INDEX, index);
    return inw(DISPI_IOPORT_DATA);
}

// Finds the adapter and records its framebuffer. Returns 0 without
// touching a single register when the hardware is not this one -- the
// ordinary `-vga vmware`/`-vga virtio` case, not an error.
static int find_adapter(void) {
    const struct pci_device *dev = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d) continue;
        if ((d->vendor_id == QEMU_VENDOR && d->device_id == QEMU_DEVICE) ||
            (d->vendor_id == VBOX_VENDOR && d->device_id == VBOX_DEVICE)) {
            dev = d;
            break;
        }
    }
    if (!dev) return 0;

    // BAR0 is the linear framebuffer, a 32-bit memory BAR under 4 GiB on
    // every machine this boots on -- which the kernel identity-maps, so
    // the physical address is usable as-is (see paging.h).
    if (pci_bar_is_io(dev->bar[0])) {
        klog_write("bochs: BAR0 is an I/O range -- unexpected, giving up\n");
        return 0;
    }
    g_fb = pci_bar_addr(dev->bar[0]);
    if (!g_fb) return 0;

    // The version handshake. Anything from ID0 up understands the mode
    // registers this driver writes; the later IDs only add features it
    // does not use (bank switching, 64 KiB granularity offsets).
    uint16_t id = dispi_read(DISPI_INDEX_ID);
    if (id < DISPI_ID0 || id > DISPI_ID5) {
        klog_printf("bochs: DISPI id 0x%x is not in 0xB0C0..0xB0C5 -- giving up\n", id);
        return 0;
    }
    return 1;
}

// Programs one mode, or refuses it WITHOUT disturbing the current one.
//
// The refusal-is-clean part is what makes walking a ladder safe, and it
// is why every check happens before the first write of the sequence:
// once XRES goes out, the previous mode is gone whether or not the rest
// succeeds. Same discipline as vmsvga_init().
static int bochs_try_mode(uint32_t w, uint32_t h) {
    // What the adapter says it can do. GETCAPS repurposes the same three
    // registers, so it has to be turned off again before programming
    // anything -- and it is a read of capabilities, not a mode set, so
    // the display is untouched either way.
    dispi_write(DISPI_INDEX_ENABLE, DISPI_GETCAPS);
    uint16_t max_w = dispi_read(DISPI_INDEX_XRES);
    uint16_t max_h = dispi_read(DISPI_INDEX_YRES);
    uint16_t max_bpp = dispi_read(DISPI_INDEX_BPP);
    uint32_t mem_64k = dispi_read(DISPI_INDEX_VIDEO_MEMORY_64K);
    dispi_write(DISPI_INDEX_ENABLE, DISPI_DISABLED);

    if (max_bpp < 32) {
        klog_printf("bochs: adapter tops out at %u bpp -- 32 is required\n", max_bpp);
        return 0;
    }
    if (w > max_w || h > max_h) {
        klog_printf("bochs: %ux%u exceeds the adapter's %ux%u -- trying smaller\n",
                     w, h, max_w, max_h);
        return 0;
    }

    // **THE BOUND THAT ACTUALLY BITES, and the reason it is checked here
    // rather than assumed from the resolution.** QEMU reports a generous
    // max_w/max_h and a video memory size that a 4K mode does not fit
    // in; programming it anyway gives a live display scanning past the
    // end of its own memory, which is a torn or black screen with no
    // error anywhere. `-device VGA,vgamem_mb=64` is the fix on the host
    // side; declining and taking the next ladder rung is the fix here.
    uint64_t need = (uint64_t)w * (uint64_t)h * 4;
    uint64_t have = (uint64_t)mem_64k * 65536ull;
    if (mem_64k && need > have) {
        klog_printf("bochs: %ux%u needs %uKB but the adapter has %uKB -- trying smaller\n",
                     w, h, (uint32_t)(need / 1024), (uint32_t)(have / 1024));
        return 0;
    }

    // The programming sequence: disable, describe the mode, enable. The
    // adapter latches XRES/YRES/BPP when ENABLE goes 1, so the order is
    // not cosmetic.
    //
    // VIRT_WIDTH is set explicitly and read BACK rather than assumed:
    // it is what the pitch is derived from, and an adapter is free to
    // round it up for alignment. Believing our own value instead would
    // shear every row by the difference.
    dispi_write(DISPI_INDEX_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(DISPI_INDEX_BPP, 32);
    dispi_write(DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);
    dispi_write(DISPI_INDEX_X_OFFSET, 0);
    dispi_write(DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(DISPI_INDEX_BANK, 0);
    dispi_write(DISPI_INDEX_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM);

    uint16_t got_w = dispi_read(DISPI_INDEX_XRES);
    uint16_t got_h = dispi_read(DISPI_INDEX_YRES);
    uint16_t virt_w = dispi_read(DISPI_INDEX_VIRT_WIDTH);
    if (got_w != w || got_h != h || !virt_w) {
        klog_printf("bochs: asked for %ux%u, adapter reports %ux%u -- rejecting\n",
                     w, h, got_w, got_h);
        return 0;
    }

    g_surface.addr = g_fb;
    g_surface.pitch = (uint32_t)virt_w * 4;
    g_surface.width = w;
    g_surface.height = h;
    g_surface.bpp = 32;
    g_active = 1;
    return 1;
}

static int bochs_drv_probe(void) {
    if (!find_adapter()) return 0;

    // What GRUB left, so this driver can tell "I improved the mode" from
    // "I re-programmed exactly what was already on screen". vesafb has
    // not been activated yet -- this reads multiboot's own record.
    struct display_surface grub;
    extern void vesafb_get_probe_surface(struct display_surface *out);
    vesafb_get_probe_surface(&grub);

    int w, h;
    for (int i = 0; display_mode_candidate(i, &w, &h); i++) {
        // **DECLINE rather than claim when the ladder has walked down to
        // what GRUB already gave us.** vesafb is a strictly simpler
        // driver for the same pixels, and claiming here would mean this
        // driver's mode-set path ran on every single boot to achieve
        // nothing -- with a blank-and-reprogram of a live console in the
        // middle of it. The ladder is largest-first, so reaching this
        // point means nothing bigger was accepted.
        if ((uint32_t)w <= grub.width && (uint32_t)h <= grub.height) break;
        if (bochs_try_mode((uint32_t)w, (uint32_t)h)) {
            klog_printf("bochs: set %dx%d (GRUB had %ux%u)\n",
                         w, h, grub.width, grub.height);
            return 1;
        }
    }
    return 0;
}

static void bochs_drv_get_surface(struct display_surface *out) { *out = g_surface; }

// No flush (the adapter scans continuously, like vesafb), no cursor, no
// acceleration. DISPLAY_CAP_MODESET is deliberately NOT advertised
// either: this driver picks a mode at probe, and the cap means the
// display layer may change one at RUNTIME -- which nothing above here
// can survive yet, since gfx.c's back buffer and every ring-3
// compositor mapping are sized at their own init. Advertising it would
// be exactly the kind of dishonest capability display_probe() exists to
// refuse.
static const struct display_driver bochs_driver = {
    .name = "bochs",
    .probe = bochs_drv_probe,
    .get_surface = bochs_drv_get_surface,
    .caps = 0,
};

void bochs_register(void) {
    DRIVER_REGISTER("bochs", "display");
    display_register(&bochs_driver);
}
