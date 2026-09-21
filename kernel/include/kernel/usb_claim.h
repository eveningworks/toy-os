#ifndef KERNEL_USB_CLAIM_H
#define KERNEL_USB_CLAIM_H

#include <stdint.h>

// A USB DEVICE HELD BY A RING-3 PROCESS -- dev_claim.h's shape, for a
// bus the kernel keeps owning.
//
// THE DIFFERENCE FROM PCI, AND IT IS THE WHOLE DESIGN. A claimed PCI
// device is handed over WHOLE: its BARs are mapped into the holder and
// the holder programs the chip. A USB device cannot be: the host
// controller is shared by every device on the bus, so xhci.c stays in
// ring 0 and the holder asks IT to perform transfers. That is Linux's
// usbfs, which is what libusb sits on -- and libusb does not drive the
// host controller either.
//
// What it buys is the same as the PCI path's: the descriptor walk and
// the class logic -- untrusted input and the bulk of the code -- run
// where a mistake kills a process.

// 0 when nobody holds it, else the holding pid.
int usb_claim_holder_pid(uint8_t slot);

// Can this device be claimed at all? A device whose class driver has
// no unbind could never be let go, the same gate PCI's remove() is.
int usb_claim_claimable(uint8_t slot);

// Take it: unbind whatever class driver holds it, record the holder.
// 0, or -EBUSY/-EINVAL/-EPERM.
int usb_claim_take(uint8_t slot, uint64_t pml4, int pid);

// Give it back. `rebind` re-runs the class-driver match, the way
// DEV_RELEASE_REBIND does; without it the device is left UNBOUND.
int usb_claim_drop(uint8_t slot, uint64_t pml4, int rebind);

// The holder's claims die with its address space.
void usb_claim_space_gone(uint64_t pml4);

// 0 when `pml4` holds `slot`, else a negative errno. Every transfer
// syscall's first act.
int usb_claim_check(uint8_t slot, uint64_t pml4);

#endif // KERNEL_USB_CLAIM_H
