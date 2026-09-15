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
#include "pci_internal.h" // pci_bar_mem_addr/_size -- the EDID BAR
#include "paging.h"       // paging_map_device
#include "klog.h"
#include "kfmt.h"
#include "io.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv -v` names THIS file

DRIVER_DECLARE("bochs", "display", "Bochs/QEMU stdvga, modesetting");

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
static const struct pci_device *g_pci;
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
    g_pci = dev;

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
        klog_printf(KLOG_ERR "bochs: asked for %ux%u, adapter reports %ux%u -- rejecting\n",
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

// THE ADAPTER'S LIMITS, READ ONCE. Asking for them means putting the
// adapter into GETCAPS and taking it out again, which is a WRITE to a
// live ENABLE register -- so this is emphatically not the "read of
// capabilities only; the display is untouched" its comment used to
// claim. The limits cannot change while the machine runs, so they are
// read at probe and answered from RAM afterwards.
static uint16_t g_caps_max_w, g_caps_max_h;
static uint32_t g_caps_mem_64k;

// RESTORES WHAT WAS THERE, read back rather than derived. The version
// this replaced rebuilt the ENABLE value from `g_active` -- which is 0
// during probe, so calling it there DISABLED an adapter that GRUB had
// left enabled, and the mode-adoption path then claimed a display that
// was switched off. Saving and restoring the register makes the call
// safe wherever it happens.
static void bochs_read_caps(void) {
    uint16_t saved = dispi_read(DISPI_INDEX_ENABLE);
    dispi_write(DISPI_INDEX_ENABLE, DISPI_GETCAPS);
    g_caps_max_w = dispi_read(DISPI_INDEX_XRES);
    g_caps_max_h = dispi_read(DISPI_INDEX_YRES);
    g_caps_mem_64k = dispi_read(DISPI_INDEX_VIDEO_MEMORY_64K);
    dispi_write(DISPI_INDEX_ENABLE, saved);
}

// The modes this adapter accepts: the ladder, filtered by the cached
// limits and by video memory -- the same two checks bochs_try_mode()
// makes, so a listed mode is one it will set. Touches no register.
static int bochs_accepts(uint32_t w, uint32_t h) {
    if (w > g_caps_max_w || h > g_caps_max_h) return 0;
    if (g_caps_mem_64k && (uint64_t)w * h * 4 > (uint64_t)g_caps_mem_64k * 65536ull) return 0;
    return 1;
}

static int bochs_drv_mode_count(void) {
    int n = 0, w, h;
    for (int i = 0; display_ladder_mode(i, &w, &h); i++)
        if (bochs_accepts((uint32_t)w, (uint32_t)h)) n++;
    return n;
}

static void bochs_drv_mode_at(int index, struct display_mode *out) {
    int n = 0, w, h;
    out->width = g_surface.width; out->height = g_surface.height; out->bpp = 32;
    for (int i = 0; display_ladder_mode(i, &w, &h); i++) {
        if (!bochs_accepts((uint32_t)w, (uint32_t)h)) continue;
        if (n++ == index) { out->width = (uint32_t)w; out->height = (uint32_t)h; return; }
    }
}

static int bochs_drv_set_mode(const struct display_mode *m) {
    if (!m || m->bpp != 32) return 0;
    return bochs_try_mode(m->width, m->height);
}

static int bochs_drv_probe(void) {
    if (!find_adapter()) return 0;

    // What GRUB left, so this driver can tell "I improved the mode" from
    // "I re-programmed exactly what was already on screen". vesafb has
    // not been activated yet -- this reads multiboot's own record.
    struct display_surface grub;
    extern void vesafb_get_probe_surface(struct display_surface *out);
    vesafb_get_probe_surface(&grub);

    // THE LIMITS, ONCE. Reading them toggles ENABLE on a live adapter,
    // so it must not happen while anything is on screen -- see
    // bochs_read_caps(). Here it is free: nothing has been drawn yet.
    bochs_read_caps();

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
    // Nothing better than GRUB's mode: ADOPT it rather than re-program
    // it, and claim anyway -- the mode on screen is this adapter's, and
    // claiming is what makes a RUNTIME change possible later. vesafb
    // would show the same pixels and could never change them.
    if (grub.width && grub.addr == g_fb && grub.bpp == 32) {
        g_surface = grub;
        g_active = 1;
        klog_printf("bochs: adopting GRUB's %ux%u -- modeset available\n",
                     grub.width, grub.height);
        return 1;
    }
    return 0;
}

static void bochs_drv_get_surface(struct display_surface *out) { *out = g_surface; }

// QEMU's stdvga keeps the monitor's EDID at offset 0 of its MMIO BAR
// (BAR2, `edid=on`, the default); an adapter without that BAR, or one
// whose block starts with anything but the EDID header, reports none.
static int bochs_drv_read_edid(uint8_t *out, int cap) {
    if (!g_pci || !out || cap <= 0) return 0;
    uint64_t base = pci_bar_mem_addr(g_pci, 2);
    uint64_t size = pci_bar_mem_size(g_pci, 2);
    if (!base || size < 0x400) return 0;
    volatile uint8_t *m = paging_map_device(base, 0x1000);
    if (!m) return 0;
    if (m[0] != 0x00 || m[1] != 0xFF) return 0;
    int n = cap < 128 ? cap : 128;
    for (int i = 0; i < n; i++) out[i] = m[i];
    return n;
}

// No flush (the adapter scans continuously, like vesafb), no cursor, no
// acceleration. MODESET: a mode can change after boot now that
// screen_set_mode() (kernel/core/screen.c) re-plumbs gfx, the console
// and the compositor's grant around the driver's set_mode.
static const struct display_driver bochs_driver = {
    .name = "bochs",
    .probe = bochs_drv_probe,
    .get_surface = bochs_drv_get_surface,
    .caps = DISPLAY_CAP_MODESET,
    .mode_count = bochs_drv_mode_count,
    .mode_at = bochs_drv_mode_at,
    .set_mode = bochs_drv_set_mode,
    .read_edid = bochs_drv_read_edid,
};

void bochs_register(void) {
    display_register(&bochs_driver);
}
