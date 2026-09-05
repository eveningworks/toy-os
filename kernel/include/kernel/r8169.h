#ifndef R8169_H
#define R8169_H

#include <stdint.h>

// The part of the RTL8169/8168 driver that can be tested without the
// chip. QEMU emulates no Realtek PCIe NIC, so everything else in
// kernel/drivers/net/r8169.c needs the real card -- and both of the
// traps in its descriptor handling live in these two functions.

#define R8169_ZLEN   60     // shortest frame on the wire, FCS excluded
#define R8169_RX_BUF 2048   // bytes per receive descriptor

// opts1 for a single-buffer transmit: OWN, first, last and the length.
// EOR MARKS THE RING'S LAST DESCRIPTOR and is the trap here -- without
// it the transmit engine walks off the end of the ring.
uint32_t r8169_tx_opts1(uint32_t len, int last);

// What a completed receive descriptor's opts1 says the frame is, or 0
// when it is not deliverable: an error, a fragment, or a length no
// descriptor could have produced. THE REPORTED LENGTH INCLUDES THE
// ETHERNET FCS and the answer does not -- delivered as-is, those four
// bytes are payload every checksum above then fails on.
uint32_t r8169_rx_frame_len(uint32_t opts1);

// A frame shorter than the wire minimum, padded -- an ARP request is 42
// bytes. Padded here rather than left to the chip, so what goes out
// does not depend on which padding a revision does for itself.
static inline uint32_t r8169_tx_pad(uint32_t len) {
    return len < R8169_ZLEN ? R8169_ZLEN : len;
}

#endif
