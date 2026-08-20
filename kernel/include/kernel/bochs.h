#ifndef BOCHS_H
#define BOCHS_H

#include "display.h"

// The Bochs DISPI extension -- QEMU's `-vga std` and `bochs-display`,
// and VirtualBox's VBoxVGA, as a MODESETTING display driver.
//
// **Why this exists.** `video=<W>x<H>` (docs/boot-flags.md) was inert on
// the one adapter every default boot and every headless test actually
// uses: vesafb cannot set a mode, so the screen size was whatever GRUB
// negotiated from boot.asm's multiboot2 request and nothing after boot
// could change it. vmsvga could set modes but only exists under
// `VGA=vmware`. This driver is the missing half -- the same DISPI
// register window Linux's `bochs-drm` drives.
//
// Register it AFTER virtio-gpu/vmsvga (they own specific hardware) and
// BEFORE vesafb_register(), which always claims. It DECLINES when no
// ladder mode is better than what GRUB already left, so vesafb picks up
// the mode already on screen rather than this driver re-programming it.
void bochs_register(void);

#endif
