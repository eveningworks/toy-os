#ifndef VMSVGA_H
#define VMSVGA_H

#include <stdint.h>

// A minimal driver for the VMware SVGA II adapter (PCI 15ad:0405), the
// device QEMU presents as `-vga vmware`. It exists for one feature this
// machine otherwise cannot have: a HARDWARE MOUSE CURSOR.
//
// **Why a whole display driver for a cursor.** Plain VGA -- what
// `-vga std` gives, and what this kernel has always used -- has no
// cursor sprite for a linear framebuffer at all; VGA's only hardware
// cursor is the text-mode underline. So a hardware cursor isn't code
// this kernel was missing, it's a capability the device didn't have.
// Getting one means talking to a device that does.
//
// And the cursor can't simply be bolted onto GRUB's framebuffer: the
// adapter composites its cursor only while it is driving the display
// itself. So this takes the display over -- enables SVGA mode, sets the
// mode through the device's own registers, and re-points gfx.c at the
// adapter's framebuffer BAR. That's a small modesetting driver, which
// is the honest cost of the feature.
//
// **This is emulator-only, and the software cursor stays.** Real
// hardware needs a real GPU driver; `-vga std` has nothing to program.
// So the WM keeps its software sprite and uses the hardware path only
// when this reports it available -- see gfx.h's gfx_hw_cursor_*().
//
// Everything here is port I/O and MMIO against one specific device, so
// it lives in drivers/ by kernel/README.md's test. The MMIO needs no
// mapping work: the BARs QEMU assigns sit under 4GiB, which this kernel
// already identity-maps (see vmm.h).

// Probes PCI for the adapter and, if found, takes over the display at
// `want_w` x `want_h` x 32bpp and re-points gfx.c at it. Call AFTER
// pci_init() and after vga_init()/gfx_init() -- it replaces the
// framebuffer those established rather than racing them.
//
// Returns 1 if the adapter was found and the takeover succeeded, 0
// otherwise (no such device, unusable version, or the mode was
// refused). A 0 is not an error: it's the ordinary `-vga std` case, and
// everything carries on with GRUB's framebuffer.
int vmsvga_init(uint32_t want_w, uint32_t want_h);

// 1 once the driver is actually driving the display.
int vmsvga_active(void);

// Tells the adapter a rectangle of the framebuffer changed. REQUIRED:
// in SVGA mode nothing drawn appears until it is announced this way.
// gfx.c calls this from gfx_flush(); drivers/apps shouldn't need to.
void vmsvga_update(int x, int y, int w, int h);

// 1 once the display is ours AND the adapter has a usable cursor.
int vmsvga_cursor_available(void);

// Uploads a 32-bit ARGB cursor image (premultiplied is not required;
// QEMU blends straight ARGB) and makes it the current cursor.
// `hot_x`/`hot_y` are the click point within the image. Returns 1 on
// success.
int vmsvga_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y);

// Moves the cursor. Cheap by design -- with cursor-bypass this is a
// handful of writes into the FIFO's register area and involves no
// drawing, no damage tracking and no repaint at all, which is the whole
// point of the exercise.
void vmsvga_cursor_move(int x, int y);

// Shows/hides it without redefining the image.
void vmsvga_cursor_show(int on);

#endif
