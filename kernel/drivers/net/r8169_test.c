// KTESTs for the RTL8169/8168 descriptor rules.
//
// This is the half of the driver that can be tested without the chip:
// QEMU emulates no Realtek PCIe NIC, so everything else needs the card
// on a real bus. It is also where the silent bugs are -- a receive
// length used as reported delivers four bytes of Ethernet FCS as
// payload, and a transmit ring with no EOR anywhere sends the engine
// off the end of it. Both look like a working driver until something
// upstream checksums a frame, or the machine stops.
#include "r8169.h"
#include "r8169_regs.h"
#include "netdev.h"
#include "ktest.h"

#define OWN      0x80000000u
#define EOR      0x40000000u
#define FS       0x20000000u
#define LS       0x10000000u
#define RXERRSUM 0x00200000u

KTEST("r8169", "a transmit descriptor is first, last, owned and sized") {
    uint32_t o = r8169_tx_opts1(1514, 0);
    KTEST_ASSERT_EQ(o & 0xFFFFu, 1514u);
    KTEST_ASSERT(o & OWN);
    KTEST_ASSERT(o & FS);
    KTEST_ASSERT(o & LS);
    KTEST_ASSERT(!(o & EOR));
}

// The ring's last slot is the only one that wraps, and nothing else in
// the driver says so.
KTEST("r8169", "only the last transmit descriptor ends the ring") {
    KTEST_ASSERT(r8169_tx_opts1(60, 1) & EOR);
    KTEST_ASSERT(!(r8169_tx_opts1(60, 0) & EOR));
    // EOR must not eat into the length: 0x4000 and up would collide
    // with it if the length were masked to 14 bits like a receive one.
    KTEST_ASSERT_EQ(r8169_tx_opts1(1514, 1) & 0xFFFFu, 1514u);
}

// An ARP request is 42 bytes and the wire minimum is 60.
KTEST("r8169", "a short frame is padded to the wire minimum") {
    KTEST_ASSERT_EQ(r8169_tx_pad(42), 60u);
    KTEST_ASSERT_EQ(r8169_tx_pad(59), 60u);
    KTEST_ASSERT_EQ(r8169_tx_pad(60), 60u);
    KTEST_ASSERT_EQ(r8169_tx_pad(1514), 1514u);
}

// The reported length INCLUDES the Ethernet FCS. Delivering it whole is
// the bug this exists to catch: a 64-byte report is a 60-byte frame.
KTEST("r8169", "a received length has the FCS taken off it") {
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | 64u), 60u);
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | 1518u), 1514u);
}

KTEST("r8169", "a descriptor the device still owns yields nothing") {
    KTEST_ASSERT_EQ(r8169_rx_frame_len(OWN | FS | LS | 64u), 0u);
}

// A frame split across descriptors cannot happen while the buffer is
// larger than the largest frame accepted -- so a fragment is a fault,
// not a case to reassemble, and must not be delivered as a whole frame.
KTEST("r8169", "a fragment or an error is not a frame") {
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | 64u), 0u);              // no last
    KTEST_ASSERT_EQ(r8169_rx_frame_len(LS | 64u), 0u);              // no first
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | RXERRSUM | 64u), 0u);
}

// The length field is the device's, so a value no descriptor could have
// produced has to stop here rather than be handed on as a frame length.
KTEST("r8169", "an impossible length is refused") {
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | 0u), 0u);
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | 17u), 0u);   // under a header
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | 0x3FFFu), 0u);
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | (R8169_RX_BUF + 1)), 0u);
    KTEST_ASSERT_EQ(r8169_rx_frame_len(FS | LS | R8169_RX_BUF), R8169_RX_BUF - 4);
}

// EOR is set by the driver on the ring's last slot and comes back on
// the completion, where it must not be mistaken for anything.
KTEST("r8169", "the ring-end mark does not disturb a received length") {
    KTEST_ASSERT_EQ(r8169_rx_frame_len(EOR | FS | LS | 1518u), 1514u);
}

// Both drivers decide the receiver's bring-up order by this; the
// desktop's RTL8168G (XID 0x4c000000) is the case that broke.
KTEST("r8169", "the 8168G family is told apart from older chips by its revision") {
    KTEST_ASSERT(r8169_g_family(0x4C000000u));
    KTEST_ASSERT(r8169_g_family(0x4C100000u));        // a minor revision of the same
    KTEST_ASSERT(r8169_g_family(0x54000000u));        // 8168H
    KTEST_ASSERT(!r8169_g_family(0x2C000000u));       // 8168E
    KTEST_ASSERT(!r8169_g_family(0x00000000u));
}
