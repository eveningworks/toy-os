// KTESTs for the Aquantia AQtion descriptor words and firmware decoding.
//
// The half of aq.c that runs without the card: QEMU models no Aquantia
// part. Each of these is a way the driver looks alive and is wrong --
// a transmit length in one of its two fields sends empty frames, a MAC
// read in the other generation's byte order is a valid-looking address
// nobody owns, and a 10 Gb/s link truncated to 32 bits reports 1.4.
// driver-none: the aq driver's tests
#include "aq.h"
#include "netdev.h"
#include "ktest.h"

KTEST("aq", "a transmit descriptor carries the length in BOTH fields") {
    uint32_t c1 = aq_tx_ctl1(1514), c2 = aq_tx_ctl2(1514);
    KTEST_ASSERT_EQ((c1 >> AQ_TXD_BLEN_SHIFT) & 0xFFFFu, 1514u);
    KTEST_ASSERT_EQ(c2 >> AQ_TXD_PAYLEN_SHIFT, 1514u);
    KTEST_ASSERT_EQ(c1 & 0xFu, AQ_TXD_TYPE_TXD);
    KTEST_ASSERT(c1 & AQ_TXD_EOP);
    KTEST_ASSERT(c1 & AQ_TXD_FCS);
    KTEST_ASSERT(!(c1 & AQ_TXD_DD));   // the device sets it, never the driver
}

KTEST("aq", "a short frame is padded to the wire minimum") {
    KTEST_ASSERT_EQ(aq_tx_pad(42), 60u);
    KTEST_ASSERT_EQ(aq_tx_pad(60), 60u);
    KTEST_ASSERT_EQ(aq_tx_pad(1514), 1514u);
}

KTEST("aq", "only a done, whole, error-free frame is delivered") {
    uint16_t ok = AQ_RXD_DD | AQ_RXD_EOP;
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok, 0, 60), 60u);
    KTEST_ASSERT_EQ(aq_rx_frame_len(AQ_RXD_EOP, 0, 60), 0u);              // not written back
    KTEST_ASSERT_EQ(aq_rx_frame_len(AQ_RXD_DD, 0, 60), 0u);               // part of a chain
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok | AQ_RXD_MACERR, 0, 60), 0u);
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok, AQ_RXD_TYPE_DMA_ERR, 60), 0u);
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok, 0, 13), 0u);                      // shorter than a header
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok, 0, AQ_RX_BUF + 1), 0u);           // bigger than its buffer
    KTEST_ASSERT_EQ(aq_rx_frame_len(ok, 0, AQ_RX_BUF), (uint32_t)AQ_RX_BUF);
}

KTEST("aq", "the descriptors are the 16 bytes the ring is sized for") {
    KTEST_ASSERT_EQ(sizeof(struct aq_tx_desc), 16u);
    KTEST_ASSERT_EQ(sizeof(struct aq_rx_desc_read), 16u);
    KTEST_ASSERT_EQ(sizeof(struct aq_rx_desc_wb), 16u);
}

KTEST("aq", "A2's link status decodes every rate, and nothing else") {
    KTEST_ASSERT(aq2_link_bps(6u << 4) == 10000000000ull);
    KTEST_ASSERT(aq2_link_bps(5u << 4) == 5000000000ull);
    KTEST_ASSERT(aq2_link_bps(4u << 4) == 2500000000ull);
    KTEST_ASSERT(aq2_link_bps(3u << 4) == 1000000000ull);
    KTEST_ASSERT(aq2_link_bps(2u << 4) == 100000000ull);
    KTEST_ASSERT(aq2_link_bps(1u << 4) == 10000000ull);
    KTEST_ASSERT(aq2_link_bps(0) == 0);
    KTEST_ASSERT(aq2_link_bps(7u << 4) == 0);
    // The low nibble is a state, not a rate.
    KTEST_ASSERT(aq2_link_bps((3u << 4) | 0xF | (1u << 11)) == 1000000000ull);
}

KTEST("aq", "A1's firmware link words decode, 1.x and 2.x") {
    KTEST_ASSERT(aq_fw2x_link_bps(1ull << 11) == 10000000000ull);
    KTEST_ASSERT(aq_fw2x_link_bps(1ull << 8) == 1000000000ull);
    KTEST_ASSERT(aq_fw2x_link_bps(1ull << 5) == 100000000ull);
    KTEST_ASSERT(aq_fw2x_link_bps((1ull << 35) | (1ull << 63)) == 0);   // pause, transaction id
    KTEST_ASSERT(aq_fw1x_link_bps(1u << 16) == 10000000000ull);
    KTEST_ASSERT(aq_fw1x_link_bps(1u << 18) == 5000000000ull);         // 5GSR alone is still 5G
    KTEST_ASSERT(aq_fw1x_link_bps(1u << 20) == 1000000000ull);
    KTEST_ASSERT(aq_fw1x_link_bps(0x2) == 0);                           // the mode byte, no speed
}

KTEST("aq", "a 10 Gb/s link survives the trip into net_device") {
    struct net_device d = { 0 };
    d.link_bps = aq2_link_bps(6u << 4);
    KTEST_ASSERT(d.link_bps == 10000000000ull);
}

KTEST("aq", "each generation's MAC words come out in its own byte order") {
    uint8_t mac[6];
    const uint32_t a2[2] = { 0x33221100u, 0x00005544u };
    aq2_mac_from_words(a2, mac);
    KTEST_ASSERT(mac[0] == 0x00 && mac[1] == 0x11 && mac[2] == 0x22 &&
                 mac[3] == 0x33 && mac[4] == 0x44 && mac[5] == 0x55);
    const uint32_t a1[2] = { 0x00112233u, 0x44550000u };
    aq1_mac_from_words(a1, mac);
    KTEST_ASSERT(mac[0] == 0x00 && mac[1] == 0x11 && mac[2] == 0x22 &&
                 mac[3] == 0x33 && mac[4] == 0x44 && mac[5] == 0x55);
}

KTEST("aq", "A2's version word is normalised to A1's layout") {
    // major 1, minor 2, build 0x0304 in A2's order: build | minor | major.
    KTEST_ASSERT_EQ(aq2_fw_version(0x03040201u), 0x01020304u);
}
