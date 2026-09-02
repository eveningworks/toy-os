#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include <stddef.h>

// The virtio entropy device. See kernel/drivers/virtio/virtio_rng.c.
//
// Brings the driver up if a virtio-rng device is on the PCI bus and
// registers it with krandom as an entropy SOURCE. Silent and harmless
// when there is none, which is the ordinary case on a default boot.
//
// Called from kernel_main() after pmm_init() -- a virtqueue needs
// contiguous frames -- and therefore AFTER stack_guard_randomize(),
// which is deliberate: the canary is drawn before this device can
// exist, and reordering boot so it could would move the frame allocator
// ahead of the guard page work that depends on it. The canary keeps
// whatever krandom_init() had; everything drawn afterwards gets this.

// Is there a working virtio-rng device?
int virtio_rng_present(void);

// Fill `buf` with `n` bytes of device entropy. 1 on success, 0 when
// there is no device, a request is already in flight, or the device
// stopped answering. Blocking (spin-polls the ring), so this is a
// SEEDING call -- see krandom.h on why it is not the per-draw source.
int virtio_rng_read(void *buf, size_t n);

// Successful fills, cumulative -- exported so a KTEST can assert the
// driver actually went to the device rather than being skipped.
unsigned virtio_rng_fills(void);

#endif
