// VMware SVGA II (PCI 15ad:0405) -- see vmsvga.h for why this driver
// exists at all (a hardware mouse cursor, which plain VGA cannot do).
//
// Register/command numbers below are from VMware's published svga_reg.h,
// which is what QEMU's hw/display/vmware_vga.c implements.
#include "vmsvga.h"
#include "display.h"
#include "pci.h"
#include "klog.h"
#include "kfmt.h"
#include "io.h"
#include "string.h"
#include "driver.h" // DRIVER_REGISTER -- `lsdrv -v` names THIS file

#define VMSVGA_VENDOR 0x15ad
#define VMSVGA_DEVICE 0x0405

// BAR0 is an I/O range: an index/value register pair. Every register
// access is "write the index, then read or write the value".
#define PORT_INDEX 0
#define PORT_VALUE 1

#define SVGA_REG_ID              0
#define SVGA_REG_ENABLE          1
#define SVGA_REG_WIDTH           2
#define SVGA_REG_HEIGHT          3
#define SVGA_REG_MAX_WIDTH       4
#define SVGA_REG_MAX_HEIGHT      5
#define SVGA_REG_BITS_PER_PIXEL  7
#define SVGA_REG_BYTES_PER_LINE  12
#define SVGA_REG_FB_START        13
#define SVGA_REG_FB_OFFSET       14
#define SVGA_REG_FB_SIZE         16
#define SVGA_REG_CAPABILITIES    17
#define SVGA_REG_MEM_START       18
#define SVGA_REG_MEM_SIZE        19
#define SVGA_REG_CONFIG_DONE     20
#define SVGA_REG_SYNC            21
#define SVGA_REG_BUSY            22
// "Deprecated" in VMware's header, and exactly what QEMU implements --
// its adapter advertises CURSOR_BYPASS/BYPASS_2 (caps 0xe3) and no
// bypass-3, so cursor POSITION goes through these registers rather than
// the FIFO. Measured, not assumed: the device reported caps=0xe3 and
// fifo_caps=0x0.
#define SVGA_REG_CURSOR_ID       24
#define SVGA_REG_CURSOR_X        25
#define SVGA_REG_CURSOR_Y        26
#define SVGA_REG_CURSOR_ON       27

// Version handshake: write an ID, read it back. The device accepts the
// highest version it supports, so writing ID_2 and reading ID_2 back
// means "we agree on version 2".
#define SVGA_MAGIC 0x900000u
#define SVGA_ID_2  ((SVGA_MAGIC << 8) | 2u)

#define SVGA_CAP_CURSOR        0x00000020u
#define SVGA_CAP_ALPHA_CURSOR  0x00000400u

// The FIFO's first words are registers, not commands.
#define SVGA_FIFO_MIN                 0
#define SVGA_FIFO_MAX                 1
#define SVGA_FIFO_NEXT_CMD            2
#define SVGA_FIFO_STOP                3
#define SVGA_FIFO_CAPABILITIES        4
#define SVGA_FIFO_CURSOR_ON           9
#define SVGA_FIFO_CURSOR_X           10
#define SVGA_FIFO_CURSOR_Y           11
#define SVGA_FIFO_CURSOR_COUNT       12
#define SVGA_FIFO_CURSOR_LAST_UPDATED 13
#define SVGA_FIFO_NUM_REGS           14

#define SVGA_FIFO_CAP_CURSOR_BYPASS_3 (1u << 4)

#define SVGA_CMD_UPDATE               1
#define SVGA_CMD_DEFINE_ALPHA_CURSOR 22

#define CURSOR_MAX_PIXELS (64 * 64)

static uint16_t g_io;              // BAR0 base
static volatile uint32_t *g_fifo;  // BAR2
static uint32_t g_fifo_size;
static int g_active;               // display taken over
static int g_cursor_ok;
static int g_bypass3;              // cursor position via FIFO regs, no command
static int g_alpha_ok;             // adapter advertises a 32-bit alpha cursor
static struct display_surface g_surface;
static int g_cursor_on = 1;
// The hardware cursor is OFF by default, and that is a considered
// default rather than caution.
//
// Positioning it writes SVGA_REG_CURSOR_X/Y/ON, and QEMU responds by
// calling dpy_mouse_set(), which warps the HOST pointer. This kernel's
// mouse is PS/2 -- a RELATIVE device with no absolute pointing
// alternative (see CLAUDE.md on why a usb-tablet can't just be added:
// there's no USB stack, and adding one silently breaks PS/2 input
// entirely). A warp against a relative device feeds motion straight
// back to the guest, and the reported symptom matches: the pointer
// jumps around and a window drag is nearly impossible.
//
// Stated as the hypothesis it is -- it has not been instrumented -- but
// the behaviour is reproducible and the default should not be the
// broken one. The display takeover itself is unaffected and stays on:
// that part works, and it's a real modesetting driver.
//
// The path where a hardware cursor genuinely works here is virtio-gpu
// plus virtio-input (an absolute device), which is Milestone 27a --
// see docs/roadmap.md.
static int g_cursor_enabled = 0;

static void reg_write(uint32_t index, uint32_t value) {
    outl(g_io + PORT_INDEX, index);
    outl(g_io + PORT_VALUE, value);
}

static uint32_t reg_read(uint32_t index) {
    outl(g_io + PORT_INDEX, index);
    return inl(g_io + PORT_VALUE);
}

// Waits for the device to drain what we've written. SYNC then poll
// BUSY, per the spec's documented handshake.
static void fifo_sync(void) {
    reg_write(SVGA_REG_SYNC, 1);
    while (reg_read(SVGA_REG_BUSY)) { /* spin -- QEMU clears this promptly */ }
}

// Appends one word to the command FIFO, wrapping at MAX.
static void fifo_write(uint32_t value) {
    uint32_t min = g_fifo[SVGA_FIFO_MIN];
    uint32_t max = g_fifo[SVGA_FIFO_MAX];
    uint32_t next = g_fifo[SVGA_FIFO_NEXT_CMD];

    // If the ring is full, let the device catch up first. A full FIFO
    // is not expected here (the only command this driver ever sends is
    // a cursor definition), but silently overwriting unconsumed
    // commands would be a memorably confusing bug.
    uint32_t after = next + 4;
    if (after >= max) after = min;
    while (after == g_fifo[SVGA_FIFO_STOP]) fifo_sync();

    g_fifo[next / 4] = value;
    g_fifo[SVGA_FIFO_NEXT_CMD] = after;
}

int vmsvga_init(uint32_t want_w, uint32_t want_h) {
    g_active = 0;
    g_cursor_ok = 0;

    const struct pci_device *dev = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->vendor_id == VMSVGA_VENDOR && d->device_id == VMSVGA_DEVICE) {
            dev = d;
            break;
        }
    }
    if (!dev) return 0; // the ordinary -vga std case, not an error

    if (!pci_bar_is_io(dev->bar[0])) {
        klog_write("vmsvga: BAR0 is not an I/O range -- unexpected, giving up\n");
        return 0;
    }
    g_io = (uint16_t)pci_bar_addr(dev->bar[0]);

    // Version handshake before touching anything else.
    reg_write(SVGA_REG_ID, SVGA_ID_2);
    if (reg_read(SVGA_REG_ID) != SVGA_ID_2) {
        klog_write("vmsvga: device did not accept SVGA_ID_2\n");
        return 0;
    }

    uint32_t caps = reg_read(SVGA_REG_CAPABILITIES);
    uint32_t max_w = reg_read(SVGA_REG_MAX_WIDTH);
    uint32_t max_h = reg_read(SVGA_REG_MAX_HEIGHT);
    if (want_w > max_w || want_h > max_h) {
        // Not a failure: the probe walks a ladder and will try the next
        // size down. Refusing WITHOUT touching the current mode is what
        // makes that safe, so nothing below this point has run yet.
        klog_printf("vmsvga: %ux%u exceeds the adapter's %ux%u -- trying smaller\n",
                     want_w, want_h, max_w, max_h);
        return 0;
    }

    // Take the display over: set the mode, then enable. Order matters --
    // the device latches width/height/bpp when ENABLE goes 1.
    reg_write(SVGA_REG_WIDTH, want_w);
    reg_write(SVGA_REG_HEIGHT, want_h);
    reg_write(SVGA_REG_BITS_PER_PIXEL, 32);
    reg_write(SVGA_REG_ENABLE, 1);

    uint32_t fb_base = reg_read(SVGA_REG_FB_START);
    uint32_t fb_off  = reg_read(SVGA_REG_FB_OFFSET);
    uint32_t pitch   = reg_read(SVGA_REG_BYTES_PER_LINE);
    if (!fb_base || !pitch) {
        klog_write("vmsvga: no framebuffer reported after enable -- reverting\n");
        reg_write(SVGA_REG_ENABLE, 0);
        return 0;
    }

    // Record the surface for get_surface(). No mapping needed: QEMU
    // puts these BARs under 4GiB and this kernel identity-maps that
    // whole range.
    g_surface.addr = (uint64_t)fb_base + fb_off;
    g_surface.pitch = pitch;
    g_surface.width = want_w;
    g_surface.height = want_h;
    g_surface.bpp = 32;
    g_active = 1;

    // The FIFO lives in BAR2. Its first words are registers; commands
    // start at MIN.
    uint32_t fifo_base = reg_read(SVGA_REG_MEM_START);
    g_fifo_size = reg_read(SVGA_REG_MEM_SIZE);
    if (fifo_base && g_fifo_size >= SVGA_FIFO_NUM_REGS * 4) {
        g_fifo = (volatile uint32_t *)(uintptr_t)fifo_base;
        g_fifo[SVGA_FIFO_MIN]      = SVGA_FIFO_NUM_REGS * 4;
        g_fifo[SVGA_FIFO_MAX]      = g_fifo_size;
        g_fifo[SVGA_FIFO_NEXT_CMD] = SVGA_FIFO_NUM_REGS * 4;
        g_fifo[SVGA_FIFO_STOP]     = SVGA_FIFO_NUM_REGS * 4;
        reg_write(SVGA_REG_CONFIG_DONE, 1);

        uint32_t fifo_caps = g_fifo[SVGA_FIFO_CAPABILITIES];
        g_bypass3 = (fifo_caps & SVGA_FIFO_CAP_CURSOR_BYPASS_3) != 0;
        // SVGA_CAP_CURSOR alone is enough: it means the adapter has a
        // hardware cursor and takes the FIFO define-cursor command.
        // Requiring SVGA_CAP_ALPHA_CURSOR as well was too strict --
        // QEMU has the cursor but doesn't advertise the alpha cap, and
        // the first version of this driver therefore declared "cursor
        // unavailable" on the one adapter it was written for.
        g_cursor_ok = (caps & SVGA_CAP_CURSOR) != 0;
        g_alpha_ok = (caps & SVGA_CAP_ALPHA_CURSOR) != 0;
    }

    klog_printf("vmsvga: caps=0x%x fifo_base=0x%x fifo_size=%u fifo_caps=0x%x\n",
                 caps, fifo_base, g_fifo_size,
                 g_fifo ? g_fifo[SVGA_FIFO_CAPABILITIES] : 0);
    klog_printf("vmsvga: SVGA II active, %ux%u x32 pitch %u, cursor %s%s\n",
                 want_w, want_h, pitch,
                 g_cursor_ok ? (g_cursor_enabled ? "hardware" : "hardware (disabled by default -- see vmsvga.c)")
                             : "unavailable",
                 g_cursor_ok && g_bypass3 ? " (bypass3)" : "");
    return 1;
}

// **In SVGA mode the adapter does not scan the framebuffer.** Writing
// pixels changes nothing on screen until the guest names the rectangle
// that changed. Missing this is not a subtle bug: after the takeover
// the console drew normally and the display simply stopped changing,
// frozen on whatever QEMU happened to refresh at mode-set time.
//
// Cheap by design -- one 5-word FIFO command, no sync. QEMU consumes
// the FIFO on its own refresh tick, so there's nothing to wait for.
void vmsvga_update(int x, int y, int w, int h) {
    if (!g_active || !g_fifo || w <= 0 || h <= 0) return;
    fifo_write(SVGA_CMD_UPDATE);
    fifo_write((uint32_t)(x < 0 ? 0 : x));
    fifo_write((uint32_t)(y < 0 ? 0 : y));
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);
}

int vmsvga_active(void) { return g_active; }

int vmsvga_cursor_available(void) { return g_active && g_cursor_ok && g_cursor_enabled; }

void vmsvga_cursor_set_enabled(int on) { g_cursor_enabled = on ? 1 : 0; }
int vmsvga_cursor_supported(void) { return g_active && g_cursor_ok; }

int vmsvga_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!vmsvga_cursor_available() || !argb) return 0;
    if (w <= 0 || h <= 0 || w * h > CURSOR_MAX_PIXELS) return 0;

    fifo_write(SVGA_CMD_DEFINE_ALPHA_CURSOR);
    fifo_write(0);              // cursor id
    fifo_write((uint32_t)hot_x);
    fifo_write((uint32_t)hot_y);
    fifo_write((uint32_t)w);
    fifo_write((uint32_t)h);
    for (int i = 0; i < w * h; i++) fifo_write(argb[i]);
    fifo_sync();
    return 1;
}

void vmsvga_cursor_move(int x, int y) {
    if (!vmsvga_cursor_available()) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (g_bypass3) {
        // The cheapest path: three memory writes, no port I/O. COUNT
        // last and it must change -- that's what tells the device the
        // preceding values form one complete new position.
        g_fifo[SVGA_FIFO_CURSOR_X]  = (uint32_t)x;
        g_fifo[SVGA_FIFO_CURSOR_Y]  = (uint32_t)y;
        g_fifo[SVGA_FIFO_CURSOR_ON] = (uint32_t)(g_cursor_on ? 1 : 0);
        g_fifo[SVGA_FIFO_CURSOR_COUNT] = g_fifo[SVGA_FIFO_CURSOR_COUNT] + 1;
        return;
    }
    // Register path (CURSOR_BYPASS/BYPASS_2) -- what QEMU actually
    // offers. Four port writes per move, still far less work than
    // blitting a sprite and restoring what was under it.
    reg_write(SVGA_REG_CURSOR_ID, 0);
    reg_write(SVGA_REG_CURSOR_X, (uint32_t)x);
    reg_write(SVGA_REG_CURSOR_Y, (uint32_t)y);
    reg_write(SVGA_REG_CURSOR_ON, (uint32_t)(g_cursor_on ? 1 : 0));
}

void vmsvga_cursor_show(int on) {
    if (!vmsvga_cursor_available()) return;
    g_cursor_on = on ? 1 : 0;
    if (g_bypass3) {
        g_fifo[SVGA_FIFO_CURSOR_ON] = (uint32_t)g_cursor_on;
        g_fifo[SVGA_FIFO_CURSOR_COUNT] = g_fifo[SVGA_FIFO_CURSOR_COUNT] + 1;
    } else {
        reg_write(SVGA_REG_CURSOR_ID, 0);
        reg_write(SVGA_REG_CURSOR_ON, (uint32_t)g_cursor_on);
    }
}


// ---------------------------------------------------------------------
// The display_driver interface (see kernel/include/kernel/display.h)
// ---------------------------------------------------------------------
//
// vmsvga_init() above does the real work; these are the thin adapters
// the display layer calls. Note which capabilities are advertised and
// which are not: NEEDS_FLUSH always (this adapter shows nothing until
// told), CURSOR only when the hardware has one AND it's been enabled
// (it's off by default -- see g_cursor_enabled's comment), and neither
// acceleration bit despite the hardware advertising RECT_FILL/RECT_COPY
// in its caps, because nothing here implements them yet. Advertising a
// capability without the function is refused by display_probe(), which
// is the point of stating both.

static int vmsvga_drv_probe(void) {
    // THIS DRIVER SETS THE MODE; it does not inherit one.
    //
    // It used to mirror GRUB's geometry "so the takeover is invisible",
    // which is fine when GRUB got what the multiboot header asked for
    // and useless when it did not: a VESA BIOS with a short mode list
    // (VirtualBox's is the reported case) leaves GRUB on 640x480, and
    // faithfully re-programming 640x480 on an adapter that can do far
    // better is the one thing a modesetting driver should not do.
    //
    // So: walk the ladder (display.h), largest first, and take the
    // first mode the adapter accepts. vmsvga_init() already refuses a
    // request past SVGA_REG_MAX_WIDTH/HEIGHT without disturbing the
    // current mode, which is what makes trying several safe.
    int w, h;
    for (int i = 0; display_mode_candidate(i, &w, &h); i++) {
        if (vmsvga_init((uint32_t)w, (uint32_t)h)) {
            klog_printf("vmsvga: set %dx%d\n", w, h);
            return 1;
        }
    }

    // Nothing on the ladder worked. Fall back to whatever GRUB left --
    // always available, and better than no display at all. vesafb has
    // not been activated yet, so multiboot's own record is reached
    // through the surface the previous driver would report.
    struct display_surface cur;
    extern void vesafb_get_probe_surface(struct display_surface *out);
    vesafb_get_probe_surface(&cur);
    if (!cur.width || !cur.height) return 0;
    klog_printf("vmsvga: no ladder mode accepted -- keeping GRUB's %ux%u\n",
                 cur.width, cur.height);
    return vmsvga_init(cur.width, cur.height);
}

static void vmsvga_drv_get_surface(struct display_surface *out) { *out = g_surface; }
static void vmsvga_drv_flush(int x, int y, int w, int h) { vmsvga_update(x, y, w, h); }

static int vmsvga_drv_cursor_define(const uint32_t *argb, int w, int h, int hx, int hy) {
    return vmsvga_cursor_define(argb, w, h, hx, hy);
}
static void vmsvga_drv_cursor_move(int x, int y) { vmsvga_cursor_move(x, y); }
static void vmsvga_drv_cursor_show(int on) { vmsvga_cursor_show(on); }

static struct display_driver vmsvga_driver = {
    .name = "vmsvga",
    .probe = vmsvga_drv_probe,
    .get_surface = vmsvga_drv_get_surface,
    .caps = DISPLAY_CAP_NEEDS_FLUSH,
    .flush = vmsvga_drv_flush,
};

void vmsvga_register(void) {
    // Caps are decided at registration, not baked in: the cursor is
    // only advertised if this build/config actually enables it, so
    // display_probe()'s honesty check stays satisfied either way.
    if (g_cursor_enabled) {
        vmsvga_driver.caps |= DISPLAY_CAP_CURSOR;
        vmsvga_driver.cursor_define = vmsvga_drv_cursor_define;
        vmsvga_driver.cursor_move = vmsvga_drv_cursor_move;
        vmsvga_driver.cursor_show = vmsvga_drv_cursor_show;
    }
    DRIVER_REGISTER("vmsvga", "display");
    display_register(&vmsvga_driver);
}
