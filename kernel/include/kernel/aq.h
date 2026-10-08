#ifndef AQ_H
#define AQ_H

#include <stdint.h>

// The part of the Aquantia AQtion driver (kernel/drivers/net/aq.c) that
// can be tested without the card: QEMU models no Aquantia part, so the
// descriptor words and the firmware's answers are decoded here, where a
// KTEST reaches them. INLINE, so the KTESTs in the image link whether
// the driver is built in or as a module (build.conf).
//
// Two generations share these: A1 (AQC100-112, "Atlantic") and A2
// (AQC113-116, "Antigua"). The descriptor rings are the same on both;
// the firmware interface is not, which is why there are three link
// decoders below.

#define AQ_RX_BUF 2048      // bytes per receive descriptor, a multiple of 1 KiB
#define AQ_ZLEN   60        // shortest frame on the wire, FCS excluded

// --- transmit descriptor (16 bytes) ---------------------------------
struct aq_tx_desc {
    uint64_t addr;
    uint32_t ctl1;
    uint32_t ctl2;
} __attribute__((packed));

#define AQ_TXD_TYPE_TXD   0x00000001u
#define AQ_TXD_DD         (1u << 20)
#define AQ_TXD_EOP        (1u << 21)
#define AQ_TXD_FCS        (1u << 23)   // the MAC appends the FCS
#define AQ_TXD_WB         (1u << 27)   // write DD back when sent
#define AQ_TXD_BLEN_SHIFT 4
#define AQ_TXD_PAYLEN_SHIFT 14

// One frame in one buffer. THE LENGTH GOES IN TWICE -- the buffer's
// (ctl1) and the packet's (ctl2) -- and a descriptor with only the first
// is sent as a zero-length packet.
static inline uint32_t aq_tx_ctl1(uint32_t len) {
    return AQ_TXD_TYPE_TXD | AQ_TXD_FCS | AQ_TXD_EOP | AQ_TXD_WB |
           ((len & 0xFFFFu) << AQ_TXD_BLEN_SHIFT);
}
static inline uint32_t aq_tx_ctl2(uint32_t len) {
    return (len & 0x3FFFFu) << AQ_TXD_PAYLEN_SHIFT;
}
static inline uint32_t aq_tx_pad(uint32_t len) {
    return len < AQ_ZLEN ? AQ_ZLEN : len;
}

// --- receive descriptor (16 bytes, two layouts over the same slot) ---
//
// THE DEVICE OVERWRITES THE BUFFER ADDRESS with its write-back, so a
// slot is re-armed by writing the address again, never by clearing DD.
struct aq_rx_desc_read {
    uint64_t buf_addr;
    uint64_t hdr_addr;
} __attribute__((packed));

struct aq_rx_desc_wb {
    uint32_t type;
    uint32_t rss_hash;
    uint16_t status;
    uint16_t pkt_len;
    uint16_t next_desc;
    uint16_t vlan;
} __attribute__((packed));

#define AQ_RXD_TYPE_DMA_ERR (1u << 12)
#define AQ_RXD_DD           (1u << 0)
#define AQ_RXD_EOP          (1u << 1)
#define AQ_RXD_MACERR       (1u << 2)

// What a written-back descriptor says the frame is, or 0 when it is not
// deliverable: not done, an error, part of a chain (no EOP -- a frame
// bigger than one buffer, which this driver does not reassemble), or a
// length no buffer could hold. pkt_len EXCLUDES the FCS, which the MAC
// strips -- unlike r8169's, whose reported length carries it.
static inline uint32_t aq_rx_frame_len(uint16_t status, uint32_t type, uint16_t pkt_len) {
    if (!(status & AQ_RXD_DD)) return 0;
    if (!(status & AQ_RXD_EOP)) return 0;
    if (status & AQ_RXD_MACERR) return 0;
    if (type & AQ_RXD_TYPE_DMA_ERR) return 0;
    if (pkt_len < 14 || pkt_len > AQ_RX_BUF) return 0;
    return pkt_len;
}

// --- what the firmware reports --------------------------------------

// A2: the rate nibble (bits 7:4) of the interface buffer's link status.
static inline uint64_t aq2_link_bps(uint32_t status) {
    switch ((status >> 4) & 0xF) {
    case 6: return 10000000000ull;
    case 5: return 5000000000ull;
    case 4: return 2500000000ull;
    case 3: return 1000000000ull;
    case 2: return 100000000ull;
    case 1: return 10000000ull;
    default: return 0;   // 0 is "no link"; 7-15 are no rate it has
    }
}

// A1, firmware 2.x/3.x: the 64-bit MPI state, one bit per full-duplex
// rate the link came up at. Highest first: the firmware sets one.
static inline uint64_t aq_fw2x_link_bps(uint64_t state) {
    if (state & (1ull << 11)) return 10000000000ull;
    if (state & (1ull << 10)) return 5000000000ull;
    if (state & (1ull << 9))  return 2500000000ull;
    if (state & (1ull << 8))  return 1000000000ull;
    if (state & (1ull << 5))  return 100000000ull;
    return 0;
}

// A1, firmware 1.x: the speed bits sit at 16 and up in MPI state, and
// 5G has two of them (5GSR is the short-reach variant).
static inline uint64_t aq_fw1x_link_bps(uint32_t state) {
    uint32_t s = state >> 16;
    if (s & (1u << 0)) return 10000000000ull;
    if (s & (3u << 1)) return 5000000000ull;
    if (s & (1u << 3)) return 2500000000ull;
    if (s & (1u << 4)) return 1000000000ull;
    if (s & (1u << 5)) return 100000000ull;
    return 0;
}

// THE MAC'S BYTE ORDER DIFFERS BY GENERATION. A2's firmware writes the
// address into its interface buffer in memory order (byte 0 is the low
// byte of the first word); A1's efuse holds it big-endian per word.
static inline void aq2_mac_from_words(const uint32_t w[2], uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
}
static inline void aq1_mac_from_words(const uint32_t w[2], uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(w[i / 4] >> (24 - 8 * (i % 4)));
}

// A2's version word is major in bits 7:0, minor 15:8, build 31:16 --
// the opposite way round from A1's register, which is major in 31:24.
// Both are normalised to A1's layout so one formatter prints either.
static inline uint32_t aq2_fw_version(uint32_t bundle) {
    return ((bundle & 0xFFu) << 24) | (((bundle >> 8) & 0xFFu) << 16) | (bundle >> 16);
}

#endif
