// The plain linear framebuffer GRUB hands us via multiboot2 -- as an
// ordinary display driver rather than a special case.
//
// **This is the fallback, and making it a driver is the point.** Before
// the display layer existed, gfx.c had "the multiboot framebuffer" as
// its built-in default and a real card was an override bolted on top;
// the two paths behaved differently for no good reason. Here it's just
// the driver that registers last and always claims, so there is exactly
// one path through the code.
//
// It's also the second implementation the interface needed in order to
// be a design rather than a guess -- and deliberately the OPPOSITE kind
// of device from vmsvga: passive, scanned continuously by the adapter,
// no cursor, no acceleration, no mode setting. Everything vmsvga has,
// this lacks; everything optional in display.h is exercised by exactly
// one of the two. An interface proven only against the fancy device
// would have baked in assumptions the simple one breaks.
#include "display.h"
#include "multiboot.h"
#include "klog.h"
#include "driver.h" // DRIVER_DECLARE -- `lsdrv -v` names THIS file

DRIVER_DECLARE("vesafb", "display", "VESA linear framebuffer, mode set by GRUB");

static struct display_surface g_surface;
static int g_have;

static int vesafb_probe(void) {
    struct framebuffer_info info;
    if (!multiboot_get_framebuffer(&info)) return 0;
    if (info.type != 1) return 0;                     // direct RGB only
    if (info.bpp != 32 && info.bpp != 24) return 0;

    g_surface.addr = info.addr;
    g_surface.pitch = info.pitch;
    g_surface.width = info.width;
    g_surface.height = info.height;
    g_surface.bpp = info.bpp;
    g_have = 1;
    return 1;
}

static void vesafb_get_surface(struct display_surface *out) { *out = g_surface; }

// No flush: the adapter scans this memory continuously, so drawing IS
// showing. No cursor, no accel, no modeset -- GRUB picked the mode and
// there is no way to ask for another without dropping to real mode.
static const struct display_driver vesafb_driver = {
    .name = "vesafb",
    .probe = vesafb_probe,
    .get_surface = vesafb_get_surface,
    .caps = 0,
};

void vesafb_register(void) {
    display_register(&vesafb_driver);
}

int vesafb_available(void) { return g_have; }

// GRUB's geometry, readable before any driver is active. vmsvga uses it
// to take the display over at exactly the mode already on screen, so
// the switch is invisible; without it the driver would have to invent a
// resolution and everything on screen would jump.
void vesafb_get_probe_surface(struct display_surface *out) {
    if (!g_have) vesafb_probe(); // harmless to repeat; it only reads multiboot
    *out = g_surface;
}
