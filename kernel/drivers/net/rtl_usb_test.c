// KTESTs for the RTL8153 receive walk and transmit descriptor.
//
// This is the half of the driver that can be tested without the chip:
// QEMU emulates no RTL8153, so everything else needs the real adapter
// passed through. The walk is also where the interesting bugs are -- a
// bulk transfer is not a frame here, and a stride computed without the
// 8-byte padding delivers the first frame correctly and garbage after
// it, which is exactly the failure a single-frame fixture cannot see.
#include "usb.h"
#include "rtl_usb.h"
#include "ktest.h"
#include "netdev.h"
#include "string.h"

#define RXD 24

// One receive descriptor plus `len` payload bytes at `at`, padded to 8.
// `len` is what the device reports, which INCLUDES the Ethernet CRC.
static uint32_t put_frame(uint8_t *buf, uint32_t at, uint32_t len, uint8_t fill) {
    k_memset(buf + at, 0, RXD);
    buf[at + 0] = (uint8_t)len;
    buf[at + 1] = (uint8_t)(len >> 8);
    for (uint32_t i = 0; i < len; i++) buf[at + RXD + i] = fill;
    return RXD + ((len + 7) & ~7u);
}

// TWO FRAMES IN ONE TRANSFER, neither of them 8-aligned. Aggregation is
// switched off in the driver, so this is the shape it must survive
// rather than the shape it expects -- and it is the only one that
// notices a stride that forgot the padding.
KTEST("usb-r8153", "rx walk splits an aggregated transfer") {
    static uint8_t buf[512];
    k_memset(buf, 0, sizeof buf);
    uint32_t used = put_frame(buf, 0, 62, 0xA1);       // 58-byte frame
    uint32_t total = used + put_frame(buf, used, 101, 0xB2);   // 97-byte frame

    uint32_t off = 0, fo = 0, fl = 0;
    uint32_t step = usb_r8153_rx_step(buf + off, total - off, &fo, &fl);
    KTEST_ASSERT_EQ(step, 88u);          // 24 + roundup(62, 8)
    KTEST_ASSERT_EQ(fo, (uint32_t)RXD);
    KTEST_ASSERT_EQ(fl, 58u);            // the CRC is not part of the frame
    KTEST_ASSERT_EQ(buf[off + fo], 0xA1);

    off += step;
    step = usb_r8153_rx_step(buf + off, total - off, &fo, &fl);
    KTEST_ASSERT_EQ(step, 128u);         // 24 + roundup(101, 8)
    KTEST_ASSERT_EQ(fl, 97u);
    KTEST_ASSERT_EQ(buf[off + fo], 0xB2);

    off += step;
    KTEST_ASSERT_EQ(off, total);
}

// A descriptor that does not fit, a payload that does not fit, and the
// mask's ceiling -- all three stop the walk rather than reading past
// what the transfer actually carried.
KTEST("usb-r8153", "rx walk refuses a transfer it cannot trust") {
    static uint8_t buf[256];
    uint32_t fo = 1, fl = 1;

    k_memset(buf, 0, sizeof buf);
    put_frame(buf, 0, 62, 0xC3);
    KTEST_ASSERT_EQ(usb_r8153_rx_step(buf, RXD - 1, &fo, &fl), 0u);   // no descriptor
    KTEST_ASSERT_EQ(usb_r8153_rx_step(buf, RXD + 61, &fo, &fl), 0u);  // payload short

    // The length field at its ceiling names no following descriptor, so
    // the rest of the transfer is unwalkable.
    buf[0] = 0xFF; buf[1] = 0x7F;
    KTEST_ASSERT_EQ(usb_r8153_rx_step(buf, sizeof buf, &fo, &fl), 0u);
}

// A frame the walk must STEP OVER rather than stop at: a runt is one
// frame's problem, and stopping would lose everything behind it.
KTEST("usb-r8153", "rx walk drops a runt and keeps going") {
    static uint8_t buf[256];
    k_memset(buf, 0, sizeof buf);
    uint32_t used = put_frame(buf, 0, 9, 0xD4);        // shorter than a header
    uint32_t total = used + put_frame(buf, used, 62, 0xE5);

    uint32_t fo = 0, fl = 0;
    uint32_t step = usb_r8153_rx_step(buf, total, &fo, &fl);
    KTEST_ASSERT_EQ(step, 40u);          // 24 + roundup(9, 8)
    KTEST_ASSERT_EQ(fl, 0u);             // named as a drop, not delivered

    step = usb_r8153_rx_step(buf + step, total - step, &fo, &fl);
    KTEST_ASSERT_EQ(fl, 58u);
}

// The last frame's padding may simply not be there, and consuming more
// than the transfer carried would walk into the previous contents of a
// reused buffer.
KTEST("usb-r8153", "rx walk stops at the end of a short transfer") {
    static uint8_t buf[128];
    k_memset(buf, 0, sizeof buf);
    put_frame(buf, 0, 62, 0xF6);

    uint32_t fo = 0, fl = 0;
    uint32_t step = usb_r8153_rx_step(buf, RXD + 62, &fo, &fl);
    KTEST_ASSERT_EQ(step, (uint32_t)(RXD + 62));   // not the padded 88
    KTEST_ASSERT_EQ(fl, 58u);
}

KTEST("usb-r8153", "tx descriptor carries first, last and the length") {
    uint8_t d[8];
    k_memset(d, 0xAA, sizeof d);
    usb_r8153_tx_desc(d, 1514);

    // Little-endian: length in the low half, FS (bit 31) and LS (bit 30)
    // in the top byte. Both are required on a single-buffer frame.
    KTEST_ASSERT_EQ((uint32_t)d[0], 1514u & 0xFF);
    KTEST_ASSERT_EQ((uint32_t)d[1], 1514u >> 8);
    KTEST_ASSERT_EQ((uint32_t)d[2], 0u);
    KTEST_ASSERT_EQ((uint32_t)d[3], 0xC0u);
    for (int i = 4; i < 8; i++) KTEST_ASSERT_EQ((uint32_t)d[i], 0u);
}

// The id table decides which configuration enumeration picks, so a
// device it does not name must be left in configuration 0.
KTEST("usb-r8153", "only listed devices claim a vendor configuration") {
    KTEST_ASSERT(usb_r8153_claims(0x2357, 0x0601));   // TP-Link UE300
    KTEST_ASSERT(usb_r8153_claims(0x0BDA, 0x8153));
    KTEST_ASSERT(!usb_r8153_claims(0x2357, 0x0000));
    KTEST_ASSERT(!usb_r8153_claims(0x041E, 0x3256));  // a Sound Blaster G6
}

// The gate that keeps an untested part from being driven with a
// neighbour's sequence: the 2.5G part is named, the 8153B is refused.
KTEST("usb-r8153", "the version gate names the 8156B and refuses the 8153B") {
    KTEST_ASSERT(rtl_usb_chip_for(0x5C20) == &rtl8153_ops);
    KTEST_ASSERT(rtl_usb_chip_for(0x7410) == &rtl8156_ops);
    KTEST_ASSERT(rtl_usb_chip_for(0x7020) == &rtl8156_ops);
    KTEST_ASSERT(rtl_usb_chip_for(0x6000) == 0);   // RTL8153B: nothing here has driven one
    KTEST_ASSERT(rtl_usb_chip_for(0x0000) == 0);   // registers that did not read
    KTEST_ASSERT(usb_r8153_claims(0x0BDA, 0x8156));
}
